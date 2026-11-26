"""C103: two signatures test one q3 mem64 mapping; no VCN or unlock operation."""
import time

CHALLENGES = (0x12345670, 0xFFFFFFFC)
VERSION = 0x00580600


def run(backend, target, emit, guard, *, now=time.monotonic, sleep=time.sleep):
    # target is supplied only from a pinned, generated q0 descriptor.
    if type(target) is not int or not 0 < target <= 0xFFFFFFFC or target % 4:
        raise RuntimeError('invalid generated target')

    def read(q, name):
        guard()
        value = backend.read(q, name)
        guard()
        if type(value) is not int or not 0 <= value <= 0xFFFFFFFF:
            raise RuntimeError('invalid register value')
        return value

    def write(q, name, value):
        guard()
        backend.write(q, name, value)
        guard()

    def snapshot(q):
        first = read(q, 'response')
        value = dict(response=first, command=read(q, 'command'),
                     parameter=read(q, 'parameter'))
        if read(q, 'response') != first:
            raise RuntimeError('response changed during snapshot')
        emit(dict(kind='mailbox-snapshot', queue=q, **value))
        return value

    def transact(q, message, argument, expected):
        if not ((q == 0 and ((message == 2 and argument == 0 and expected == VERSION) or
                             (message == 1 and argument in CHALLENGES and expected == argument + 1))) or
                (q == 3 and message == 0x2A and argument == target and
                 expected in tuple(x + 1 for x in CHALLENGES))):
            raise RuntimeError('operation outside fixed protocol')
        started = now()

        def check():
            guard()
            if now() - started >= 2:
                raise RuntimeError('mailbox deadline')

        def r(name):
            check(); value = read(q, name); check()
            return value

        def w(name, value):
            check(); write(q, name, value); check()

        if r('response') != 1:
            raise RuntimeError('prior response is not1')
        emit(dict(kind='request', queue=q, message=message, argument=argument,
                  expected_value=expected))
        w('response', 0)
        if r('response') != 0:
            raise RuntimeError('response clear failed')
        w('parameter', argument)
        if r('parameter') != argument:
            raise RuntimeError('argument readback failed')
        if r('response') != 0:
            raise RuntimeError('response arrived before command')
        w('command', message)
        for _ in range(400):
            response = r('response')
            if response == 1:
                break
            if response != 0:
                raise RuntimeError('mailbox refusal ' + hex(response))
            sleep(0.001)
        else:
            raise RuntimeError('mailbox poll limit')
        value = r('parameter')
        if r('response') != 1:
            raise RuntimeError('response changed during result')
        emit(dict(kind='response', queue=q, message=message, response=1, value=value))
        if value != expected:
            raise RuntimeError('signature mismatch')
        return value

    transact(0, 2, 0, VERSION)
    prior = snapshot(3)
    guard(); sleep(0.004); guard()
    if snapshot(3) != prior or prior['response'] != 1:
        raise RuntimeError('q3 initial state not stable completed1; no q3 write')
    samples = []
    for challenge in CHALLENGES:
        expected = challenge + 1
        transact(0, 1, challenge, expected)
        signature = dict(parameter=expected, response=1, command=1)
        if snapshot(0) != signature:
            raise RuntimeError('q0 before signature changed')
        observed = transact(3, 0x2A, target, expected)
        if snapshot(0) != signature:
            raise RuntimeError('q0 after signature changed')
        samples.append(dict(challenge=challenge, observed=observed))
        emit(dict(kind='mapping-sample', target=target, **samples[-1]))
    guard()
    return dict(passed=True, target=target, samples=samples, mapping_scope='one-q0-parameter',
                no_activation=True, no_unlock=True, no_dma=True,
                other_address_mapping_proven=False, reset_required=True)
