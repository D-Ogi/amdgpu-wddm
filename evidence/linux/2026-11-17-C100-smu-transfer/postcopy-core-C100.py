"""Two fixed data controls only. C097 VCN-handler withdrawal remains in force."""
import time

CONTROLS = ((0, 0x00580600), (0xEBC, 0xAD004136))


def command(backend, words, expected, emit, guard, *, now=time.monotonic, sleep=time.sleep):
    if len(words) != 6 or any(type(x) is not int or not 0 <= x <= 0xFFFFFFFF for x in words):
        raise RuntimeError('six u32 arguments required')
    valid = (words == [4, 0, 0, 0, 0, 0] and expected == (1, 3)) or (
        words[0] == 0x1F and words[1] == 0 and words[2] in (0, 0xEBC) and
        words[3:] == [1, 0, 0] and expected == (1, 2)) or (
        words[0] == 0x14 and words[1] == 0 and 0 < words[2] <= 0xFFFFFFFF - 4095 and
        words[2] % 4096 == 0 and words[3:] == [1, 0, 0] and expected == (1, 3))
    if not valid:
        raise RuntimeError('command outside fixed diagnostic protocol')
    start = now()
    def check():
        guard()
        if now() - start >= 2:
            raise RuntimeError('q2 command deadline')
    def read(name):
        check(); value = backend.read(name); check()
        if type(value) is not int or not 0 <= value <= 0xFFFFFFFF:
            raise RuntimeError('invalid q2 value')
        return value
    def write(name, value):
        check(); backend.write(name, value); check()
    if read('response') != 1:
        raise RuntimeError('q2 prior response not1')
    emit(dict(kind='q2-request', words=words, expected_pair=list(expected)))
    write('response', 0)
    if read('response') != 0:
        raise RuntimeError('q2 response clear failed')
    for i, value in enumerate(words):
        name = 'arg' + str(i)
        write(name, value)
        if read(name) != value:
            raise RuntimeError('q2 argument readback failed')
    if read('response') != 0:
        raise RuntimeError('q2 response arrived before command')
    write('command', 0x0A)
    for _ in range(400):
        status = read('response')
        if status == 1:
            break
        if status != 0:
            raise RuntimeError('q2 outer refusal ' + hex(status))
        sleep(0.001)
    else:
        raise RuntimeError('q2 poll limit')
    value = read('arg0')
    if read('response') != 1:
        raise RuntimeError('q2 response changed during result')
    emit(dict(kind='q2-response', words=words, pair=[1, value],
              load_refusal=(words[0] == 0x1F and value == 2)))
    if (1, value) != expected:
        raise RuntimeError('unexpected q2 result ' + repr((1, value)))
    return (1, value)


def observe(backend, buffers, emit, guard, *, transact=command):
    # Caller already required pinned q0 controls and exact setup00 descriptor.
    buffers.prepare()  # BOTH filled and snapshotted before first exposure.
    transact(backend, [4, 0, 0, 0, 0, 0], (1, 3), emit, guard)
    samples = []
    for slot, (source, expected) in enumerate(CONTROLS):
        guard()
        emit(dict(kind='stage', stage='load-control', slot=slot, source=source))
        transact(backend, [0x1F, 0, source, 1, 0, 0], (1, 2), emit, guard)
        guard()
        dma = buffers.expose(slot)
        guard()
        buffers.before_submit(slot)
        emit(dict(kind='stage', stage='dma-pending', slot=slot))
        pair = transact(backend, [0x14, dma >> 32, dma & 0xFFFFFFFF, 1, 0, 0],
                        (1, 3), emit, guard)
        sample = buffers.verify_after_reply(slot, pair, expected)
        emit(dict(kind='known-word', source=source, expected=expected, **sample))
        samples.append(sample)
    return dict(data_observation_pass=True, samples=samples, load_refusals=2,
                stable_load_ready_state_proven=False, vcn_activation=False,
                kernel_retains_until_reset=True)
