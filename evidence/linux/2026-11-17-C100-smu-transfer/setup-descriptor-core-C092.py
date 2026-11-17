"""One q2 setup00 and its published descriptor. Injected I/O, no DMA or retries."""
import time


def run_setup(backend, emit, guard, now=time.monotonic, sleep=time.sleep):
    start = now()
    def check():
        guard()
        if now() - start >= 2:
            raise RuntimeError('setup observation deadline')
    def read(name):
        check()
        value = backend.read(name)
        check()
        if type(value) is not int or not 0 <= value <= 0xFFFFFFFF:
            raise RuntimeError('invalid register value')
        return value
    def write(name, value):
        check()
        backend.write(name, value)
        check()
    def snapshot(phase):
        first = read('response')
        result = dict(response=first, command=read('command'),
                      args=[read('arg' + str(i)) for i in range(6)])
        if read('response') != first:
            raise RuntimeError('response changed during snapshot')
        emit(dict(kind='q2-snapshot', phase=phase, **result))
        return result

    before = snapshot('before-1')
    if before['response'] != 1:
        raise RuntimeError('prior q2 response is not completed success; no setup write')
    sleep(0.004)
    if snapshot('before-2') != before:
        raise RuntimeError('q2 changed before setup; no setup write')
    write('response', 0)
    if read('response') != 0:
        raise RuntimeError('q2 clear failed')
    # The mode and every argument are zero: synchronous setup only.
    for i in range(6):
        name = 'arg' + str(i)
        write(name, 0)
        if read(name) != 0:
            raise RuntimeError('q2 argument did not read back')
    if read('response') != 0:
        raise RuntimeError('q2 responded before command')
    write('command', 0x0A)
    for _ in range(400):
        status = read('response')
        if status == 1:
            break
        if status != 0:
            raise RuntimeError('q2 setup rejected: ' + hex(status))
        sleep(0.001)
    else:
        raise RuntimeError('q2 completion poll limit')
    after = snapshot('after')
    if after['response'] != 1 or after['args'][0] != 3:
        raise RuntimeError('setup did not return (1,3)')
    # B+2/B+3 are not explicitly cleared by setup. Preserve/report upper bits.
    descriptor = after['args'][1:5]
    match = descriptor[0] & 0xFFFF == 0x301 and descriptor[1:] == [0x3E000, 0x3F794, 0x3F860]
    if not match:
        raise RuntimeError('setup descriptor does not match the decoded firmware')
    emit(dict(kind='setup-observation', before=before, after=after,
              descriptor_match=True, load_readiness_proven=False,
              descriptor_changed=before['args'][1:5] != descriptor))
    return after
