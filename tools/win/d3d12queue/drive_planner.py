# SPDX-License-Identifier: MIT
"""Receipt-gated interactive probe planner; no transport, file or device access.

Session completion is not process closure, restoration or GPU success. The
caller owns immutable attempt admission and the independent process supervisor.
"""
import math
import re
import subprocess

DRIVE_PLAN = ('create-device', 'create-queue', 'copy', 'status', 'exit')
# Host budget of one Drive. The long limit is for clients whose copy verb runs past 60 s (UploadRace pressure v4,
# about 93 s); the task's own 180 s bound and the supervisor still close every trial.
DRIVE_LIMIT = 65
DRIVE_LIMIT_LONG = 120

class DriveRefusal(Exception):
    """A bounded safe reason, never remote exception text or payload."""


class DriveState:
    def __init__(self):
        self.expected = {1: 'create-device'}
        self.receipts = {}
        self.issued = set()
        self.terminal = False
        self._planned = None
        self._blocked = False
        self._stop_seen = False

    def observe(self, snapshot):
        """Validate a complete snapshot and return (sequence, verb), or None.

        Refusals poison this planner. A terminal planner cannot be restarted.
        Only allowlisted receipt fields are retained, detached from input data.
        """
        if self._blocked:
            raise DriveRefusal('planner_refused')
        if self.terminal:
            return None
        self._planned = None
        try:
            self._planned = self._observe(snapshot)
            return self._planned
        except DriveRefusal:
            self._blocked = True
            raise

    def _validate_history(self):
        # interactive.h publishes receipts before COM cleanup. Only create_device
        # can change device, only create_queue can change queue, and only copy
        # changes copy_success/pending. Status/exit/abort leave all four intact.
        prior_state = dict(device=False, queue=False)
        prior_copy = prior_pending = False
        prior_elapsed = 0
        for sequence, receipt in enumerate((self.receipts[n] for n in sorted(self.receipts)), 1):
            if receipt['sequence'] != sequence:
                raise DriveRefusal('receipt_history_gap')
            state = receipt['state']
            copied, pending = receipt['copy_success'], receipt['gpu_pending']
            command = receipt['command']
            if state['queue'] and not state['device']:
                raise DriveRefusal('queue_without_device')
            if receipt['elapsed_ms'] < prior_elapsed:
                raise DriveRefusal('receipt_time_regressed')
            if command == 'create-device':
                if state['queue'] or copied or pending:
                    raise DriveRefusal('impossible_device_receipt')
            elif command == 'create-queue':
                if state['device'] != prior_state['device'] or copied or pending:
                    raise DriveRefusal('impossible_queue_receipt')
            elif command == 'copy':
                if state != prior_state or not state['device'] or not state['queue']:
                    raise DriveRefusal('copy_objects_missing_or_changed')
                # copy returns S_OK only after comparison and fence retirement;
                # failed allocation/map/submit/wait/readback paths may retain work.
                if copied != receipt['success'] or (copied and pending):
                    raise DriveRefusal('impossible_copy_result')
            elif state != prior_state or copied != prior_copy or pending != prior_pending:
                raise DriveRefusal('immutable_session_state_changed')
            prior_state = state
            prior_copy, prior_pending = copied, pending
            prior_elapsed = receipt['elapsed_ms']

    def _observe(self, snapshot):
        if not isinstance(snapshot, dict) or any(type(snapshot.get(k)) is not bool for k in ('started', 'runtime_ready', 'stop', 'terminal', 'cancel')):
            raise DriveRefusal('invalid_snapshot')
        receipts, commands = snapshot.get('receipts'), snapshot.get('commands')
        if not isinstance(receipts, list) or not isinstance(commands, list) or len(receipts) > 5 or len(commands) > 6:
            raise DriveRefusal('invalid_snapshot')
        self._stop_seen = self._stop_seen or snapshot['stop'] or snapshot['cancel']
        command_map = {}
        for command in commands:
            if not isinstance(command, dict) or type(command.get('sequence')) is not int:
                raise DriveRefusal('invalid_command')
            seq, verb = command['sequence'], command.get('command')
            if seq in command_map or seq not in self.expected or verb != self.expected[seq]:
                raise DriveRefusal('unexpected_or_duplicate_command')
            command_map[seq] = verb
        seen = set()
        for receipt in receipts:
            if not isinstance(receipt, dict):
                raise DriveRefusal('invalid_receipt')
            seq = receipt.get('sequence')
            if type(seq) is not int or seq in seen or seq not in self.expected or receipt.get('command') != self.expected[seq]:
                raise DriveRefusal('invalid_receipt_sequence')
            seen.add(seq)
            if command_map.get(seq)!=self.expected[seq]:
                raise DriveRefusal('receipt_without_immutable_command')
            hr = receipt.get('hr')
            state = receipt.get('state')
            if (type(receipt.get('schema')) is not int or receipt['schema'] != 1 or type(receipt.get('success')) is not bool or
                    not isinstance(hr, str) or not re.fullmatch('[0-9a-fA-F]{8}', hr) or
                    not isinstance(state, dict) or any(type(state.get(k)) is not bool for k in ('device', 'queue')) or
                    any(type(receipt.get(k)) is not bool for k in ('copy_success', 'gpu_pending')) or
                    type(receipt.get('elapsed_ms')) is not int or not 0 <= receipt['elapsed_ms'] <= 180000 or
                    receipt['success'] != (int(hr, 16) < 0x80000000)):
                raise DriveRefusal('invalid_receipt_fields')
            # Retain only public scalar fields; remote strings/payload never enter the log.
            safe = {k: receipt[k] for k in ('schema', 'sequence', 'command', 'success', 'hr', 'elapsed_ms', 'copy_success', 'gpu_pending')}
            safe['state'] = {k: state[k] for k in ('device', 'queue')}
            if seq in self.receipts and safe != self.receipts[seq]:
                raise DriveRefusal('changed_immutable_receipt')
            self.receipts[seq] = safe
        if not set(self.receipts).issubset(seen):
            raise DriveRefusal('missing_immutable_receipt')
        self._validate_history()
        if snapshot['terminal']:
            self.terminal = True
            return None
        if not snapshot['started']:
            raise DriveRefusal('attempt_not_running')
        if not snapshot['runtime_ready']:
            if receipts:
                raise DriveRefusal('receipt_before_runtime')
            return None
        if not command_map.get(1):
            raise DriveRefusal('missing_seeded_command')
        # No replacement or retry of a command already issued by this Drive.
        pending = max(self.expected)
        receipt = self.receipts.get(pending)
        if receipt is None:
            return None
        if receipt['command'] in ('exit', 'abort'):
            self.terminal = True
            return None
        next_seq = pending + 1
        if next_seq in command_map or next_seq in self.issued or next_seq > 5:
            raise DriveRefusal('next_sequence_not_free')
        if self._stop_seen:
            return next_seq, 'abort'
        success = receipt['success'] and not receipt['gpu_pending']
        if receipt['command'] == 'create-device':
            success = success and receipt['state']['device']
        elif receipt['command'] == 'create-queue':
            success = success and receipt['state']['device'] and receipt['state']['queue']
        elif receipt['command'] == 'copy':
            success = success and receipt['state']['device'] and receipt['state']['queue'] and receipt['copy_success'] and not receipt['gpu_pending']
        return next_seq, DRIVE_PLAN[next_seq-1] if success else 'exit'

    def issuing(self, sequence, command):
        """Consume the planned action BEFORE sending it; never retry its sequence.

        The bounded loop may replace a planned action with abort at its deadline.
        Controller admission and a fresh STOP check still belong to transport.
        """
        if self._blocked or self.terminal:
            raise DriveRefusal('planner_stopped')
        if (type(sequence) is not int or not self._planned or
                sequence != self._planned[0] or
                command not in (self._planned[1], 'abort')):
            raise DriveRefusal('unplanned_issue')
        if sequence in self.expected or sequence in self.issued:
            raise DriveRefusal('duplicate_issue')
        self.expected[sequence] = command
        self.issued.add(sequence)
        self._planned = None


# The longest single poll or command round trip, within the loop's budget.
CALL_SECONDS = 20


def drive_loop(poll, issue, emit, seconds, clock, sleep, diagnostic=None, *, retry_poll_timeout=False,
               limit=DRIVE_LIMIT):
    """Run callbacks within a 1..limit s host budget (65 s unless a long client opts into up to
    DRIVE_LIMIT_LONG); command delivery is never retried.

    poll(timeout) returns a complete snapshot; issue(seq, verb, timeout) must
    enforce its timeout, recheck STOP and validate controller acknowledgement.
    emit/diagnostic must be prompt; the independent process supervisor remains
    mandatory. Opting in asserts that poll is read-only: at most one direct
    subprocess.TimeoutExpired from poll may be retried under the same deadline.
    Other callback failures stop this loop and never establish closure.
    """
    if type(limit) is not int or not DRIVE_LIMIT <= limit <= DRIVE_LIMIT_LONG:
        raise ValueError('Drive limit must be %d..%d seconds' % (DRIVE_LIMIT, DRIVE_LIMIT_LONG))
    if type(seconds) not in (int, float) or not math.isfinite(seconds) or not 1 <= seconds <= limit:
        raise ValueError('Drive deadline must be 1..%d seconds' % limit)
    if type(retry_poll_timeout) is not bool:
        raise ValueError('retry_poll_timeout must be bool')
    state=DriveState();started=clock();end=started+seconds;last_progress=None
    poll_retry_used=False
    while clock()<end:
        remaining=end-clock()
        if remaining<=0:
            break
        stage='poll'
        try:
            # One round trip to a loaded target has taken more than 8 s; the budget stays the bound.
            selected_timeout=min(CALL_SECONDS,remaining)
            poll_started=clock()
            try:
                snapshot=poll(selected_timeout)
            except subprocess.TimeoutExpired:
                # Only this read-only call is repeatable. Never inspect exception
                # text, command, output or stderr: these may contain private data.
                now=clock();left=max(0,end-now)
                retry=retry_poll_timeout and not poll_retry_used and left>0
                decision=('disabled' if not retry_poll_timeout else
                          'deadline' if left<=0 else 'exhausted' if poll_retry_used else 'retry')
                details=dict(stage='poll',exception_type='TimeoutExpired',category='callback_failure',
                             timeout_seconds=selected_timeout,duration_seconds=max(0,now-poll_started),
                             elapsed_seconds=max(0,now-started),remaining_seconds=left,
                             retry=retry,retry_decision=decision)
                # Logging/observer failures are outside the retry catch, even
                # when they also raise TimeoutExpired.
                stage='poll_timeout_diagnostic'
                if diagnostic:diagnostic(details)
                emit(dict(event='poll_timeout',**details))
                if retry:
                    poll_retry_used=True
                    continue
                if retry_poll_timeout and left<=0:
                    return 'drive_deadline'
                emit(dict(event='transport_or_capture_failed',stage='poll',category=details['category']))
                return 'transport_or_capture_failed'
            stage='observe'
            action=state.observe(snapshot)
            stage='emit'
            progress=dict(event='progress',runtime_ready=snapshot['runtime_ready'],stop=snapshot['stop'],cancel=snapshot['cancel'],
                          terminal=snapshot['terminal'],receipts=list(state.receipts.values()))
            if progress!=last_progress:
                emit(progress);last_progress=progress
            if state.terminal:
                return 'interactive_terminal_observed'
            remaining=end-clock()
            if remaining<=0:
                break
            if snapshot['stop'] or snapshot['cancel'] or remaining<=2:
                if action:
                    sequence,_=action
                    state.issuing(sequence,'abort')
                    emit(dict(event='issuing',sequence=sequence,command='abort'))
                    stage='issue'
                    issue(sequence,'abort',min(2,remaining))
                return 'stop_observed' if snapshot['stop'] or snapshot['cancel'] else 'drive_deadline'
            if action:
                sequence,command=action
                state.issuing(sequence,command)
                emit(dict(event='issuing',sequence=sequence,command=command))
                stage='issue'
                issue(sequence,command,min(CALL_SECONDS,remaining))
            sleep(min(0.4,max(0,end-clock())))
        except DriveRefusal as error:
            emit(dict(event='refused',reason=str(error)))
            return 'admission_refused'
        except Exception as error:
            # Delivery may have succeeded despite a network timeout. Never retry.
            details=dict(stage=stage,exception_type=type(error).__name__)
            details['category']='callback_failure'
            if diagnostic:diagnostic(details)
            emit(dict(event='transport_or_capture_failed',stage=stage,category=details['category']))
            return 'transport_or_capture_failed'
    return 'drive_deadline'
