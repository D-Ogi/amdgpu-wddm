#!/usr/bin/env python3
"""Decode raw g_HipDispatchJournal bytes, never virtual memory or a live device.

Version 1 layout: driver/kmd/hip_journal.h and shim/include/bc250_hip_journal.h.
Even generation is a writer protocol witness, not proof a live extraction was atomic.
Output contains raw process/kernel/GPU addresses and packed argument bytes.
"""
import argparse
import json
import struct
from pathlib import Path

HEADER = 80
PAYLOAD = 131072
SLOT_PREFIX = 152
SLOT_BYTES = SLOT_PREFIX + PAYLOAD
SLOT_COUNT = 32
TOTAL = HEADER + SLOT_COUNT * SLOT_BYTES
STATES = ['empty', 'writing', 'uploaded', 'bound', 'submitted', 'completed',
          'failed', 'cancelled', 'abandoned']


def require(condition, detail):
    if not condition:
        raise ValueError(detail)


def record(data):
    require(len(data) >= 96, 'short record')
    size, symbol_size, argument_size, binding_count = struct.unpack_from('<4I', data)
    entry, descriptor, argument_va = struct.unpack_from('<3Q', data, 16)
    geometry = struct.unpack_from('<8I', data, 40)
    symbol_at, argument_at, bindings_at, reserved = struct.unpack_from('<4I', data, 72)
    dispatch_id, = struct.unpack_from('<Q', data, 88)
    require(96 <= size <= 32768 and size <= len(data), 'record size')
    require(entry and descriptor and dispatch_id, 'record identity')
    require(not argument_size or argument_va, 'missing kernarg VA')
    require(all(geometry[:6]) and geometry[7] == 0 and reserved == 0, 'geometry or reserved')
    require(2 <= symbol_size <= 4096 and argument_size <= 16384 and binding_count <= 128, 'record limits')
    require(symbol_at == 96 and argument_at == (96 + symbol_size + 7) & ~7, 'symbol layout')
    require(bindings_at == (argument_at + argument_size + 7) & ~7 and
            size == bindings_at + binding_count * 40, 'argument layout')
    symbol = data[symbol_at:symbol_at + symbol_size]
    require(symbol[-1:] == b'\0' and b'\0' not in symbol[:-1], 'symbol terminator')
    require(not any(data[symbol_at + symbol_size:argument_at]) and
            not any(data[argument_at + argument_size:bindings_at]), 'nonzero padding')
    arguments = data[argument_at:argument_at + argument_size]
    bindings = []
    previous = -1
    for at in range(bindings_at, size, 40):
        index, offset, kind, flags, value, base, length = struct.unpack_from('<4I3Q', data, at)
        require(index > previous and kind == 1 and flags in (0, 1), 'binding order/type')
        require(offset <= argument_size and argument_size - offset >= 8, 'binding span')
        require(struct.unpack_from('<Q', arguments, offset)[0] == value, 'binding value')
        if flags:
            require(length and base <= value and value - base < length and
                    base + length <= 0xffffffffffffffff, 'binding interval')
        else:
            require(base == 0 and length == 0, 'unresolved interval')
        bindings.append(dict(argument_index=index, argument_offset=offset, value=hex(value),
                             base=hex(base), bytes=length,
                             classification='known' if flags else ('null' if value == 0 else 'unresolved')))
        previous = index
    return size, dict(dispatch_id=dispatch_id, symbol=symbol[:-1].decode('utf-8', errors='backslashreplace'),
                      symbol_hex=symbol.hex(), entry_va=hex(entry), descriptor_va=hex(descriptor),
                      kernarg_va=hex(argument_va), grid=list(geometry[:3]), block=list(geometry[3:6]),
                      dynamic_lds_bytes=geometry[6], kernarg_hex=arguments.hex(), bindings=bindings)


def payload(data, slot):
    require(80 <= len(data) <= PAYLOAD, 'upload size')
    words = struct.unpack_from('<8I4Q4I', data)
    require(words[0] == 0x30353242 and words[1] == 35 and words[5] == 1, 'upload identity')
    require(words[2] == 0xffffffff and words[3] == 0 and words[4] == 0, 'upload input status')
    require(words[6] == len(data) and 1 <= words[7] <= 32, 'upload extent')
    require(all(words[8:11]) and words[8] % 4 == 0 and words[9] % 8 == 0, 'upload GPU identity/alignment')
    require(words[11] == 0 and not any(words[12:]), 'upload reserved/operation')
    require(tuple(words[8:11]) == tuple(slot[key] for key in ('ib_va', 'fence_va', 'fence_value')), 'slot/upload binding')
    at = 80
    records = []
    ids = set()
    for _ in range(words[7]):
        size, decoded = record(data[at:])
        require(decoded['dispatch_id'] not in ids, 'duplicate dispatch ID')
        ids.add(decoded['dispatch_id'])
        records.append(decoded)
        at += size
    require(at == len(data), 'trailing upload data')
    return records


def decode(raw):
    require(len(raw) == TOTAL, 'missing or extra raw journal bytes')
    values = struct.unpack_from('<6I7Q', raw)
    require(values[:5] == (0x4a504948, 1, TOTAL, SLOT_BYTES, SLOT_COUNT), 'journal layout/version')
    require(values[5] < SLOT_COUNT, 'next slot')
    result = dict(version=1, next_slot=values[5], next_id=values[6],
                  next_context=values[7], uploads=values[8], overwritten=values[9],
                  refused=values[10], completed=values[11], cancelled=values[12], slots=[], warnings=[])
    seen_ids = set()
    for index in range(SLOT_COUNT):
        start = HEADER + index * SLOT_BYTES
        q = struct.unpack_from('<14Q8I', raw, start)
        generation, identity = q[:2]
        state, length, pid, os_fence, sequence, vmid, ib_bytes, status = q[14:]
        if state == 0 and generation == 0 and identity == 0:
            continue
        item = dict(slot=index, generation=generation, id=identity, process_id=pid,
                    context_cookie=q[2], context=hex(q[3]), adapter=hex(q[4]), process=hex(q[5]),
                    uploaded_100ns=q[6], bound_100ns=q[7], submitted_100ns=q[8], finished_100ns=q[9],
                    ib_va=q[10], fence_va=q[11], fence_value=q[12], root=hex(q[13]),
                    os_fence=os_fence, sequence=sequence, vmid=vmid, ib_bytes=ib_bytes,
                    status=f'0x{status:08x}', state=STATES[state] if state < len(STATES) else 'invalid')
        try:
            require(0 < state < len(STATES), 'slot state')
            require(generation and not generation & 1 and state != 1, 'incomplete/torn slot')
            require(identity and identity <= values[6] and q[2] and length <= PAYLOAD, 'slot identity/length')
            require(identity not in seen_ids, 'duplicate slot identity')
            seen_ids.add(identity)
            require(raw[start + 144] in (0, 1) and raw[start + 145] in (0, 1) and
                    not any(raw[start + 148:start + 152]), 'slot reserved')
            item['cancel_requested'] = bool(raw[start + 144])
            item['replay_pending'] = bool(raw[start + 145])
            item['attempts'] = struct.unpack_from('<H', raw, start + 146)[0]
            item['records'] = payload(raw[start + SLOT_PREFIX:start + SLOT_PREFIX + length], item)
            item['valid'] = True
        except ValueError as error:
            item.update(valid=False, error=str(error))
            result['warnings'].append(f'slot {index}: {error}')
        result['slots'].append(item)
    result['slots'].sort(key=lambda item: item['id'])
    return result


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('raw', type=Path)
    parser.add_argument('--out', type=Path)
    args = parser.parse_args()
    require(args.raw.stat().st_size == TOTAL, 'wrong file size; extract the exact symbol object')
    result = decode(args.raw.read_bytes())
    text = json.dumps(result, indent=2) + '\n'
    if args.out:
        args.out.write_text(text, encoding='utf-8')
    else:
        print(text, end='')
    return 1 if result['warnings'] else 0


if __name__ == '__main__':
    raise SystemExit(main())
