"""Host companion for the six GUI lab kits. Default: local validation only.

Execution is for the lead lab operator: --config candidate.json --execute.
No credentials, target discovery, UI or network calls happen on import/check.
The lab supervisor owns its deadline independently of this process and SSH.

Results never enter this repository: they go to <BC250_ROOT>/scratch/gui/codex/runs/<trial_id>
(BC250_GUI_TRIAL_RUNS names another directory outside the repository). BC250_ROOT is the workspace root,
by default the parent directory of this repository, as tools/win/target.py reads it.
"""
from __future__ import annotations
import argparse
import os
import base64
import hashlib
import importlib.util
import json
import math
import re
import subprocess
import sys
import time
from datetime import datetime, timezone
from pathlib import Path

HERE = Path(__file__).resolve().parent
# bc250-win/tools/win/gui-trials/run-trial.py: tools/win is one level up, the repository three.
WIN = HERE.parent
REPO = HERE.parents[2]
ROOT = Path(os.environ.get('BC250_ROOT') or REPO.parent)
RUNS = Path(os.environ.get('BC250_GUI_TRIAL_RUNS') or ROOT / 'scratch/gui/codex/runs')
PROTOCOL = 'bc250.gui-trial.v1'
STAGES = {'L1': {'view'}, 'L3': {'startup-and-click'},
          'L4': {'without-prepared', 'with-prepared'},
          'L5': {'upgrade', 'resume', 'repair', 'verify'},
          'L-CU': {'baseline-24', 'request-40', 'observe-40', 'confirm-40',
                   'request-24', 'observe-24'}, 'L-OFF': {'repair'}}
TERMINAL = {'evidence-ready', 'failed', 'stopped', 'recovery-required'}
SUPPORT = ['gui-trial-common.ps1', 'gui-trial-firewall.ps1']


def utc():
    return datetime.now(timezone.utc).isoformat()


def digest(path):
    return hashlib.sha256(Path(path).read_bytes()).hexdigest()


def read_config(path):
    data = json.loads(Path(path).read_text(encoding='utf-8-sig'))
    validate_config(data)
    return data


def validate_config(c):
    if c.get('schema') != 1:
        raise ValueError('config requires schema=1')
    if not isinstance(c.get('trial_id'), str) or not re.fullmatch(r'[A-Za-z0-9_-]{1,64}', c['trial_id']):
        raise ValueError('trial_id must be 1-64 ASCII letters, digits, hyphens or underscores')
    if c.get('trial') not in STAGES or c.get('stage') not in STAGES[c['trial']]:
        raise ValueError('unknown trial/stage')
    expected = 'C:\\BC250\\tmp\\gui\\' + c['trial_id']
    if c.get('directory', '').lower() != expected.lower():
        raise ValueError('directory must be C:\\BC250\\tmp\\gui\\<trial_id>')
    seconds = c.get('duration_seconds')
    if type(seconds) is not int or not 45 <= seconds <= 180:
        raise ValueError('duration_seconds must be an integer in [45,180]')
    if not re.fullmatch(r'[A-Za-z0-9_-]{1,63}', c.get('target_computer_name', '')):
        raise ValueError('explicit target_computer_name required')
    if not isinstance(c.get('artifacts'), list):
        raise ValueError('artifacts must be an array')
    if type(c.get('expected_session_id')) is not int or c['expected_session_id'] < 1:
        raise ValueError('an explicit interactive expected_session_id is required')
    if not re.fullmatch(r'S-1-[0-9-]+', c.get('expected_user_sid', '')):
        raise ValueError('expected_user_sid is required')
    for name in ('scenario_adapter', 'bounded_helper'):
        item = c.get(name, {})
        if not re.fullmatch(r'[A-Za-z]:\\[^\r\n"\x00]+', item.get('path', '')):
            raise ValueError(name + ' needs an absolute lab path')
        if not re.fullmatch(r'[0-9a-fA-F]{64}', item.get('sha256', '')):
            raise ValueError(name + ' needs SHA-256')
    if c['trial'] == 'L-CU' and c.get('thermal', {}).get('mode') != 'local-cli':
        raise ValueError('L-CU requires the independent lab thermal guard')
    # Detailed admission, hash, session, boot and firewall checks belong to Prepare.


def events(text, c):
    if len(text) > 4 * 1024 * 1024:
        raise ValueError('remote event response exceeds 4 MiB')
    found = []
    for line in text.splitlines():
        if not line.strip().startswith('{'):
            continue
        event = json.loads(line)
        if event.get('protocol') != PROTOCOL:
            continue
        if event.get('schema') != 1 or any(event.get(k) != c[k] for k in ('trial_id', 'trial', 'stage')):
            raise ValueError('event does not belong to this trial/stage')
        found.append(event)
    if not found:
        raise ValueError('no valid trial event in response')
    return found


def load_module(name, path):
    spec = importlib.util.spec_from_file_location(name, path)
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


def ps_quote(s):
    return "'" + str(s).replace("'", "''") + "'"


class LabIO:
    """Only constructed after the operator supplies --execute."""
    def __init__(self, config, output):
        self.config, self.output = config, output
        target_module = load_module('gui_trial_target', WIN / 'target.py')
        self.target = target_module.Target()
        self.mon = load_module('gui_trial_mon', WIN / 'bc250mon/mon.py')
        # Reuse mon.py API/image decoding with the same target.py instance and a
        # short SSH deadline, instead of leaving a child Python/SSH on timeout.
        self.mon.TARGET = self
        self.remote_config = config['directory'] + '\\config.json'
        self.remote_script = config['directory'] + '\\' + config['trial'] + '.ps1'

    def run(self, powershell, timeout=10):
        encoded = base64.b64encode(powershell.encode('utf-16-le')).decode('ascii')
        result = self.target.ssh('powershell -NoProfile -EncodedCommand ' + encoded,
                                 timeout=min(timeout, 10))
        if result.returncode:
            raise RuntimeError('target.py transport or remote command failed')
        return result.stdout

    def stage(self):
        files = [HERE / name for name in SUPPORT] + [HERE / (self.config['trial'] + '.ps1')]
        files.append(self.output / 'config.json')
        for file in files:
            if not file.is_file():
                raise ValueError('missing kit file: ' + file.name)
        # Reserve atomically before tar can overwrite an existing trial. Never
        # reuse an old remote config even when this host has no local record.
        directory = ps_quote(self.config['directory'])
        self.run("$ErrorActionPreference='Stop'; New-Item -ItemType Directory -Path " +
                 directory + " -ErrorAction Stop | Out-Null; 'reserved'")
        self.target.push(files, self.config['directory'], timeout=30)

    def mode(self, name):
        if name not in {'Prepare', 'Start', 'Observe', 'Stop', 'Cleanup'}:
            raise ValueError('invalid mode')
        command = ('& ' + ps_quote(self.remote_script) + ' -Mode ' + name +
                   ' -ConfigPath ' + ps_quote(self.remote_config))
        return events(self.run(command), self.config)

    def overlay(self, text, level='info'):
        response = self.mon.call('POST', '/status', {'text': text, 'level': level})
        if not isinstance(response, dict) or 'error' in response or 'raw' in response:
            raise RuntimeError('overlay did not acknowledge status')

    def screenshot(self, path):
        try:
            size, image = self.mon.image('/screenshot?scale=0.5&format=png&overlay=1')
        except SystemExit as e:
            raise RuntimeError('half-scale screenshot unavailable') from e
        if not image.startswith(b'\x89PNG\r\n\x1a\n'):
            raise ValueError('screenshot was not PNG')
        path.write_bytes(image)
        return {'file': path.name, 'sha256': digest(path), 'size': size, 'scale': 0.5}

    def watts(self):
        result = subprocess.run([sys.executable, str(WIN / 'smartplug/plug.py'), 'telemetry'],
                                capture_output=True, text=True, timeout=12, check=True)
        reading = json.loads(result.stdout)
        if type(reading.get('power_w')) not in (float, int):
            raise ValueError('plug power unavailable')
        reading['freshness'] = 'unknown'
        return reading


class Trial:
    def __init__(self, config, output, io, clock=time.monotonic, sleep=time.sleep):
        self.c, self.output, self.io = config, output, io
        self.clock, self.sleep = clock, sleep
        self.seen, self.pictures, self.errors = set(), [], []
        self.started = False

    def log(self, kind, value):
        with (self.output / 'host.jsonl').open('a', encoding='utf-8') as f:
            f.write(json.dumps({'utc': utc(), 'kind': kind, 'value': value}, ensure_ascii=False) + '\n')

    def keep(self, batch):
        new_events = []
        for event in batch:
            sequence = event.get('sequence')
            if sequence is not None:
                key = (event['trial_id'], sequence)
                if key in self.seen:
                    continue
                self.seen.add(key)
            self.log('lab', event)
            new_events.append(event)
        return new_events

    def picture(self, reason):
        if not self.started:
            raise RuntimeError('capture before running proof')
        item = self.io.screenshot(self.output / ('screen-%02d.png' % len(self.pictures)))
        item['reason'] = reason
        self.pictures.append(item)
        self.log('screenshot', item)

    def execute(self):
        status, prepared, terminal = 'failed', False, False
        try:
            self.io.stage()
            self.io.overlay('GUI ' + self.c['trial'] + ': przygotowanie ograniczonej proby')
            # A lost Prepare acknowledgement can still leave an owned task behind.
            prepared = True
            self.keep(self.io.mode('Prepare'))
            start_time = self.clock()
            batch = self.io.mode('Start')
            self.keep(batch)
            if not any(e.get('event') == 'running' for e in batch):
                raise RuntimeError('Start did not prove running')
            self.started = True
            deadline = start_time + self.c.get('duration_seconds', 180)
            self.picture('running')
            next_power = self.clock()
            if self.c['trial'] == 'L-CU':
                self.log('plug', self.io.watts())
                next_power = self.clock() + 15
            while self.clock() < deadline:
                batch = self.io.mode('Observe')
                new_events = self.keep(batch)
                for event in new_events:
                    if event.get('event') == 'stage' and len(self.pictures) < 8:
                        name = (event.get('data') or {}).get('name')
                        if name:
                            self.picture(str(name))
                states = [e for e in batch if e.get('event') == 'state']
                if not states:
                    raise RuntimeError('Observe returned no state')
                current = states[-1]
                if current.get('status') in TERMINAL:
                    status, terminal = current['status'], True
                    self.picture('terminal')
                    break
                if self.c['trial'] == 'L-CU' and self.clock() >= next_power:
                    self.log('plug', self.io.watts())
                    next_power = self.clock() + 15
                self.sleep(min(5, max(0, deadline - self.clock())))
            if not terminal:
                status = 'stopped'
                self.log('stop', 'host deadline reached; lab has its own deadline')
        except (Exception, KeyboardInterrupt) as exc:
            # Raw transport exceptions may carry private target configuration.
            self.errors.append(type(exc).__name__)
            self.log('host-error', type(exc).__name__)
            status = 'failed'
        finally:
            if prepared:
                if not terminal:
                    try:
                        self.keep(self.io.mode('Stop'))
                    except Exception as exc:
                        self.errors.append('Stop:' + type(exc).__name__)
                try:
                    # Stop is asynchronous and result.json precedes task exit.
                    # The lab owns the 180 s work bound; this short host wait
                    # observes closure and never extends a lab deadline.
                    closing_deadline = self.clock() + 20
                    while True:
                        batch = self.io.mode('Observe')
                        self.keep(batch)
                        states = [e for e in batch if e.get('event') == 'state']
                        if states and states[-1].get('can_cleanup') is True:
                            break
                        if self.clock() >= closing_deadline:
                            raise RuntimeError('lab closure not established')
                        self.sleep(min(5, closing_deadline - self.clock()))
                    closed = self.io.mode('Cleanup')
                    self.keep(closed)
                    if not any(e.get('event') == 'cleanup' and e.get('status') == 'cleaned' for e in closed):
                        raise RuntimeError('cleanup acknowledgement missing')
                except Exception as exc:
                    status = 'recovery-required'
                    self.errors.append('Cleanup:' + type(exc).__name__)
            try:
                self.io.overlay('GUI ' + self.c['trial'] + ': ' + status,
                                'info' if status == 'evidence-ready' else 'warn')
            except Exception as exc:
                self.errors.append('overlay:' + type(exc).__name__)
            report = {'schema': 1, 'trial_id': self.c['trial_id'], 'trial': self.c['trial'],
                      'stage': self.c['stage'], 'status': status, 'started': self.started,
                      'acceptance_pass': False, 'review': 'operator-checklist-required',
                      'screenshots': self.pictures, 'errors': self.errors, 'finished_utc': utc()}
            (self.output / 'host-result.json').write_text(json.dumps(report, indent=2) + '\n', encoding='utf-8')
        return report


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--config', required=True, type=Path)
    mode = parser.add_mutually_exclusive_group()
    mode.add_argument('--execute', action='store_true', help='lead operator only; starts the actual lab stage')
    mode.add_argument('--check', action='store_true', help='local config validation only (default)')
    args = parser.parse_args(argv)
    config = read_config(args.config)
    if not args.execute:
        print(json.dumps({'status': 'local-config-check-only', 'trial': config['trial'],
                          'stage': config['stage'], 'lab_contacted': False,
                          'note': 'remote paths, hashes, admission and scenario are not validated here'}))
        return 0
    output = RUNS / config['trial_id']
    output.mkdir(parents=True, exist_ok=False)
    (output / 'config.json').write_text(json.dumps(config, indent=2) + '\n', encoding='utf-8')
    result = Trial(config, output, LabIO(config, output)).execute()
    print(json.dumps(result, indent=2))
    return 0 if result['status'] == 'evidence-ready' else 1


if __name__ == '__main__':
    raise SystemExit(main())
