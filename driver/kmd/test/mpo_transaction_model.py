#!/usr/bin/env python3
"""Adversarial MPO schedules under an explicit double-buffered register model.

This is not a DCN emulator or a hardware/HLK pass. Source checks establish the
selected normal AMD lock path. The model then assumes that the master lock
blocks latching of both planes. Hardware observation must validate that
assumption, the latch boundary and each register's buffering before integration.
"""
import argparse
from dataclasses import dataclass
import hashlib
import json
from pathlib import Path
import re

ROOT = Path(__file__).resolve().parents[3]


def body(source, name):
    match = re.search(r'\b' + re.escape(name) + r'\([^;{}]*\)\s*\{', source)
    if not match:
        raise ValueError('Missing function: ' + name)
    start = match.end()
    depth = 1
    for end in range(start, len(source)):
        depth += (source[end] == '{') - (source[end] == '}')
        if not depth:
            return source[start:end]
    raise ValueError('Unclosed function: ' + name)


def source_contract():
    names = ['driver/amdgpu-import/reference/hdmi-link/hwseq201.c',
             'driver/amdgpu-import/reference/connector/optc201.c',
             'driver/amdgpu-import/reference/connector/optc10.c',
             'driver/kmd/dcn_coeff.c', 'driver/kmd/dcn_mpc.c',
             'driver/kmd/dcn_surface_write.c', 'driver/kmd/dcn_geometry.c']
    raw = {name: (ROOT / name).read_bytes() for name in names}
    src = {name: data.decode().replace('\r\n', '\n') for name, data in raw.items()}
    hwseq, optc201, optc10 = [src[name] for name in names[:3]]
    lock = body(hwseq, 'dcn201_pipe_control_lock')
    normal = body(optc10, 'optc1_lock')
    unlock = body(optc10, 'optc1_unlock')
    triple = body(optc201, 'optc201_triplebuffer_lock')
    checks = {
        'top_pipe_only': bool(re.search(r'if\s*\(pipe->top_pipe\)\s*return;', lock)),
        'normal_lock_dispatch': 'pipe->stream_res.tg->funcs->lock(' in lock,
        'normal_unlock_dispatch': 'pipe->stream_res.tg->funcs->unlock(' in lock,
        'normal_function_binding': bool(re.search(r'\.lock\s*=\s*optc1_lock\b', optc201)),
        'normal_unlock_binding': bool(re.search(r'\.unlock\s*=\s*optc1_unlock\b', optc201)),
        'master_selector_is_tg': bool(re.search(r'OTG_MASTER_UPDATE_LOCK_SEL,\s*optc->inst', normal)),
        'master_request_set': bool(re.search(r'OTG_MASTER_UPDATE_LOCK,\s*1\)', normal)),
        'master_ack_wait': bool(re.search(r'UPDATE_LOCK_STATUS,\s*1,\s*1,\s*10', normal)),
        'master_request_clear': bool(re.search(r'OTG_MASTER_UPDATE_LOCK,\s*0\)', unlock)),
        'triple_path_is_distinct_control': 'OTG_MASTER_UPDATE_LOCK_VUPDATE_KEEPOUT_EN' in triple
            and 'OTG_MASTER_UPDATE_LOCK_VUPDATE_KEEPOUT_EN' not in normal,
    }
    failed = [name for name, ok in checks.items() if not ok]
    if failed:
        raise ValueError('Review changed lock contract: ' + ', '.join(failed))
    return {'checks': checks, 'sha256': {n: hashlib.sha256(b).hexdigest() for n, b in raw.items()}}


@dataclass(frozen=True)
class Plane:
    enabled: bool
    address: int
    geometry: int
    color: int


class Display:
    """All tuple fields latch together when unlocked; no immediate fields here."""
    def __init__(self, initial):
        self.live = list(initial)
        self.shadow = list(initial)
        self.locked = False
        self.frames = []

    def edge(self):
        if not self.locked:
            self.live = self.shadow.copy()
        self.frames.append(tuple(self.live))


def latch_discriminator():
    """Two explicit hardware hypotheses with identical idle/final observations.

    H1 is the model above: a master lock inhibits shadow-to-live transfer.
    H2 adds an armed bank: the lock inhibits staging-to-armed transfer only.
    H2 is a countermodel, not an assertion that DCN201 implements this protocol.
    """
    old = (Plane(True,10,100,1000),Plane(True,20,200,2000))
    first = (Plane(True,11,100,1000),old[1])
    final = (first[0],Plane(True,21,200,2000))
    traces = {}
    for arm, lock_before_write in (('unlocked',False),('lock_before_write',True),('arm_before_lock',False)):
        states = []
        for staging_only in (False,True):
            live = old
            shadow = list(old)
            armed = old
            locked = lock_before_write
            shadow[0] = first[0]
            if not locked:
                armed = tuple(shadow)
            if arm == 'arm_before_lock':
                locked = True
                shadow[1] = final[1]
            # Observation at the boundary while the later lock is still held.
            if staging_only:
                live = armed
            elif not locked:
                live = tuple(shadow)
            during = live
            # Both hypotheses converge after unlock plus another boundary.
            armed = tuple(shadow)
            live = armed
            states.append({'during': [vars(p) for p in during], 'after': [vars(p) for p in live]})
        traces[arm] = states
    assert traces['unlocked'][0] == traces['unlocked'][1]
    assert traces['lock_before_write'][0] == traces['lock_before_write'][1]
    assert traces['arm_before_lock'][0]['during'] != traces['arm_before_lock'][1]['during']
    assert traces['arm_before_lock'][0]['after'] == traces['arm_before_lock'][1]['after']
    assert traces['arm_before_lock'][0]['during'] == [vars(p) for p in old]
    assert traces['arm_before_lock'][1]['during'] == [vars(p) for p in first]
    return {'checks':6,'hypotheses':['lock_inhibits_latch','lock_inhibits_staging_only'],
            'traces':traces,'idle_or_after_unlock_observation_discriminates':False,
            'during_later_lock_boundary_discriminates':True,'hardware_answer':None}


def model():
    checked = 0

    def require(condition):
        nonlocal checked
        checked += 1
        if not condition:
            raise AssertionError('Model invariant ' + str(checked))

    old = (Plane(True, 10, 100, 1000), Plane(True, 20, 200, 2000))
    # Address-only completion would miss both an attribute update and a disable.
    new = (Plane(True, 10, 101, 1001), Plane(False, 20, 201, 2001))
    updates = [(p, field) for p in range(2)
               for field in ('enabled', 'address', 'geometry', 'color')]

    def write(d, pair, values):
        p, field = pair
        fields = vars(d.shadow[p]).copy()
        fields[field] = getattr(values[p], field)
        d.shadow[p] = Plane(**fields)

    # An edge at EVERY write boundary. No timing assumption makes the critical
    # section safe: keeping the lock must do so, including a failed write whose
    # effect occurred before the accessor reported failure.
    for edge in range(len(updates) + 1):
        d = Display(old)
        d.locked = True
        for i in range(len(updates) + 1):
            if i == edge:
                d.edge()
                require(tuple(d.live) == old)
            if i < len(updates):
                write(d, updates[i], new)
        d.locked = False
        d.edge()
        require(tuple(d.live) == new)

    unsafe_rollback = []
    for fail in range(len(updates)):
        for after_effect in (False, True):
            d = Display(old)
            d.locked = True
            for i in range(fail + int(after_effect)):
                write(d, updates[i], new)
            # Naive error return/unlock: old allocations and attribute sets can
            # no longer be inferred from the function's failure status.
            unsafe = Display(old)
            unsafe.shadow = d.shadow.copy()
            unsafe.edge()
            if tuple(unsafe.live) not in (old, new):
                unsafe_rollback.append({'failed_write': fail, 'after_effect': after_effect})
            # Restore the WHOLE coherent snapshot, never just the failed field.
            for pair in updates:
                write(d, pair, old)
                d.edge()
                require(tuple(d.live) == old)
            d.locked = False
            d.edge()
            require(tuple(d.live) == old and tuple(d.shadow) == old)
            # Recovery can also fail. Keeping the lock prevents this model's
            # partial shadow from scanning out; it does NOT prove hardware safe
            # stop or authorize recycling either allocation in a real driver.
            for restore_fail in range(len(updates)):
                for restore_after in (False, True):
                    broken = Display(old)
                    broken.shadow = unsafe.shadow.copy()
                    broken.locked = True
                    for i in range(restore_fail + int(restore_after)):
                        write(broken, updates[i], old)
                    broken.edge()
                    require(tuple(broken.live) == old and broken.locked)
    require(bool(unsafe_rollback))  # Negative control actually finds mixed states.

    # Earlier accepted plane 0 is waiting for the next boundary. A new plane 1
    # update must not delay it by taking the TG-wide lock over that boundary.
    first = (new[0], old[1])
    naive = Display(old)
    naive.shadow[0] = new[0]
    naive.locked = True  # Later call starts preparing plane 1.
    naive.edge()
    require(tuple(naive.live) != first)
    guarded = Display(old)
    guarded.shadow[0] = new[0]
    # Proposal: return RETRY to the later, not-yet-accepted call before locking.
    guarded.edge()
    require(tuple(guarded.live) == first)
    guarded.locked = True
    guarded.shadow[1] = new[1]
    guarded.edge()
    require(tuple(guarded.live) == first)
    guarded.locked = False
    guarded.edge()
    require(tuple(guarded.live) == new)
    # Omitted plane 0 is retained, including its previously latched attributes.
    require(guarded.live[0] == new[0])

    # A same-address request with new geometry/color needs new completion
    # evidence. Address equality and a selected coefficient bank are insufficient.
    stale = Display(old)
    stale.locked = True
    stale.shadow = list(new)
    require(all(a.address == b.address for a, b in zip(stale.live, new)))
    require(tuple(stale.live) != new)
    # Separate request IDs cannot be made into a global ordering. Layer 0's ID
    # is not evidence about layer 1; the WDK defines them as per-plane identifiers.
    requested_ids, observed_ids = (200, 7), (199, 7)
    require(observed_ids[1] == requested_ids[1] and observed_ids[0] != requested_ids[0])
    return {'checks': checked, 'partial_write_unlock_counterexamples': unsafe_rollback,
            'global_lock_delays_earlier_plane': True,
            'retry_before_lock_preserves_earlier_plane_in_model': True,
            'same_addresses_do_not_prove_new_attributes': True}


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--out', type=Path)
    args = parser.parse_args()
    report = {'scope': 'Source binding plus conditional state model, not hardware or HLK',
              'source': source_contract(), 'model': model(),
              'next_probe': latch_discriminator(),
              'unproven': ['actual per-register buffering and latch boundary',
                           'rollback after address trigger or immediate side effect',
                           'plane-disable drain and safe allocation retirement',
                           'RETRY scheduling latency and starvation under Windows',
                           'frame-coherent hardware observation and PresentId mapping']}
    if args.out:
        args.out.write_text(json.dumps(report, indent=2) + '\n', encoding='utf-8')
    print('MPO transaction model:', len(report['source']['checks']), 'source checks,',
          report['model']['checks'], 'model checks,', report['next_probe']['checks'],
          'discriminator checks PASS; hardware contract remains open')
