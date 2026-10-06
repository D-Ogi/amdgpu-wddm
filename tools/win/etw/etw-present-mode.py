#!/usr/bin/env python3
"""etw-present-mode.py - present model and flip evidence per process, from a DxgKrnl present-mode capture.

    python etw-present-mode.py TRACE.etl [PROCESS ...] [options]
    python etw-present-mode.py DUMP.txt [PROCESS ...] [options]

TRACE is the `.etl` of a capture made with the present-mode provider set (Microsoft-Windows-DxgKrnl at the full
keyword mask, Microsoft-Windows-Win32k Updates|Composition, Microsoft-Windows-Dwm-Core
Composition|DwmFrameRate|Scheduling|Overlays), as `scratch/train/b18r1-iflip-etw.ps1` and
`tools/win/lab-runner/etw/` start it. The `.etl` is turned into text with the xperf dumper, the same path
`scratch/m15/gpu-timeline/align/etwclock.py` and `etwdump.py` use, and **the text dump is deleted again** unless
`--keep-dump` is given: a dump of a 44 MB trace is 51 MB, and P: filled up once on exactly these files. A `.txt`
argument is read as an existing dumper text and is never deleted.

PROCESS arguments limit the per-present tables and every verdict line. They are a substring match on the
dumper's process column, so both `flipclient.exe` and the bare pid `4476` select a process. A session without the
kernel image rundown writes that column as `"Unknown" (4476)`, which is the normal shape of these captures: match
on the pid there.

This is not PresentMon and does not reproduce its state machine. It implements the rules below and nothing else;
a present it cannot place is counted as unclassified with the shape that made it so, never guessed into a class.
Field positions are read from the dumper's own header block, so no column index is hard-coded.

The rules, each named with the event that carries it:

  flip model     DxgKrnl/PresentHistory Start|Info and PresentHistoryDetailed Start in the presenting process
                 give the present's Token and Model (D3DKMT_PRESENT_MODEL, d3dkmthk.h:443; 2 is
                 REDIRECTED_FLIP). Win32k/TokenCompositionSurfaceObject ties that pToken to a composition
                 surface (CompositionSurfaceLuid, BindId, PresentCount) and a destination size.
  present call   DxgKrnl/Present Info is the D3DKMTPresent call itself and fires on both paths, so it is counted
                 per process on its own and never read as a mode. Its Flags is D3DKMT_PRESENTFLAGS
                 (d3dkmthk.h:393) and is decoded: 0x9000 is RedirectedFlip + PresentCountValid, the composed
                 shape of a flip-model client. Present calls beyond the process's token presents are reported as
                 "no present-history token", which is the blt or legacy path.
  independent    Win32k/TokenStateChanged for that surface and present count carries NewState (Win32k TokenState:
                 3 is InFrame) and the two flags IndependentFlip and SkipIndependentFlip, as the text "true" or
                 "false". The LAST such event for a present count decides, not the first and not the union of all
                 of them: the flags describe the state the token is in, and a token that was a candidate can be
                 taken off the independent path again. IndependentFlip true with SkipIndependentFlip false is the
                 compositor saying it handed the buffer to the display pipeline instead of composing it:
                 "hardware flip".
  composed       Dwm-Core/Dx_Flip_Consumed names (surfaceLuid, bindId, presentCount) that DWM itself consumed,
                 which is the compositor reading the buffer: "composed flip". Windowed_Dx_Flip_Consumed carries
                 the same three fields for a windowed flip chain and counts the same way (b18r1 lab, 2026-10-05:
                 a fullscreen-state D3D12 chain, 599 of its 600 presents arrived only as this event). The two
                 events are counted apart as well as together, because increment 2's conjunction asks for the
                 absence of both.
  dwm's own view Dwm-Core/SCHEDULE_SURFACEUPDATE carries bDirectFlip, bIndependentFlip and bEnableScanout for
                 (luidSurface, bindId, PresentCount) - the compositor's own answer about the same surface, from
                 the side that decides it. It is reported next to the classification above and never merged into
                 it: when the two disagree, that disagreement is the finding. On the composed baseline DWM
                 answers bDirectFlip 1 for 600 presents it then consumed itself, so this field alone is not a
                 witness of anything reaching the plane.
  per frame      DxgKrnl/QueuePacket Start carries (hContext, SubmitSequence, PacketType, bPresent) and its
                 process column is the submitting process, so a present packet of the client has a sequence
                 number of its own. DxgKrnl/IndependentFlip Info (keyword 0x4000000000000001, the DxgKrnl
                 Performance channel 0x11) names that SubmitSequence when dxgkrnl made the packet an independent
                 flip, and MMIOFlip Info names it in FlipSubmitSequence when the driver was asked to program the
                 plane for it. This is the kernel's own per-frame answer about the client's own buffer, and it is
                 the only one of the rules above that does not go through the compositor: an independent flip of
                 a client packet is exactly what M15.14 asks for. PresentMon treats the same event as its
                 Hardware_Independent_Flip (PresentData, PresentMonTraceConsumer.cpp, IndependentFlip_Info),
                 which is where the two field names come from.
  programmed     MMIOFlip Info's FlipToPhysicalAddress and FlipToDriverAllocation are what the driver was asked
                 to put on the plane. They are reported per value, and a value may be compared with
                 `--admitted-address`, which is an address the kernel driver logged as an admitted scan-out flip
                 (`bc250kmd_cli log summary`). Nothing in the trace says which allocation an address belongs to,
                 so the comparison is the only sound use of the number.
  flip flags     MMIOFlip Info's Flags. Three bits are confirmed by PresentMon's generated model of this
                 provider (ETW/Microsoft_Windows_DxgKrnl.h, SetVidPnSourceAddressFlags): ModeChange 0x1,
                 FlipImmediate 0x2, FlipOnNextVSync 0x4. The DDI word DXGK_SETVIDPNSOURCEADDRESS_FLAGS
                 (d3dkmddi.h:6212) continues SharedPrimaryTransition 0x20, IndependentFlipExclusive 0x40 and
                 MoveFlip 0x80, and M15.14 cares about the first two. **Whether the event's Flags field is that
                 full DDI word is unconfirmed**: PresentMon's model stops at 0x4, and all 718 flips of the
                 composed baseline read 4 under either mapping, so nothing has yet distinguished them. Those
                 three bits are therefore printed with a trailing `?` and named candidates, and a run that sees
                 one prints the warning line. They corroborate; the kernel driver's own counters remain the
                 witness.
  scanned        DxgKrnl/VSyncDPC and VSyncInterrupt carry ScannedPhysicalAddress, and VSyncDPC's FlipFenceId
                 carries a submit sequence in its high 32 bits. This is the display side and the only evidence
                 that does not depend on what DWM believed. A changing scanned address is NOT by itself the
                 shape of an application buffer reaching the plane: on a composing desktop the address alternates
                 between the compositor's own buffers at the refresh rate, and on this driver the DPC's field can
                 carry a value that is not an address at all. The only sound use of these numbers is to match a
                 distinct value against an address the kernel driver logged as admitted.

The verdict lines, which a trial greps instead of reading the tables:

  M15.14 VERDICT <process>      the per-frame counts and the compositor's DirectFlip answer.
  M15.14 INCREMENT1 <process>   the inert control of increment 1 (DECISION-route.md section 6): the front asks
                                the DirectFlip question and answers FALSE, so the capture must still be the
                                composed baseline. Five clauses: Present Flags 0x9000, PresentHistoryDetailed
                                Model 2, DWM bDirectFlip 1 with bIndependentFlip 0, no DxgKrnl/IndependentFlip,
                                and no MMIOFlip or VSync DPC naming a packet of the client. Any deviation is a
                                bug, not progress. result=INERT, MOVED or NO-DATA. Read it for the client under
                                test only: on a composing desktop the compositor's own packets are the ones that
                                reach the plane, so the compositor reads MOVED by construction.
  M15.14 INCREMENT2 <process>   the conjunction of increment 2, because DirectFlip without independent flip has
                                no unique ETW event. Four clauses: (1) neither Dx_Flip_Consumed nor
                                Windowed_Dx_Flip_Consumed for the client's present counts, (2) an MMIOFlip of
                                the client's own packet programmed an `--admitted-address`, (3) a VSync DPC
                                scanned that same address, (4) the kernel driver's counters from
                                `--kmd-counters`: scanout_flips > 0, scanout_requests > 0 and every admission
                                refusal column at zero. result=CONFIRMED, REFUTED or UNKNOWN.

A process named on the command line always gets all three lines, with zeros, because "the events were not in the
capture" and "the flip did not happen" must not look alike.

What this cannot do: say which plane a multi-plane overlay flip belongs to beyond LayerIndex, or tell exclusive
fullscreen from borderless. Both are outside the increment this parser was written for. It also cannot join a
present-history token to a submit sequence: nothing in this recipe carries both, so the compositor-side
classification and the kernel-side per-frame witness are reported side by side and compared by count, never
merged into one number.
"""
import argparse
import collections
import json
import os
import shutil
import subprocess
import sys
from pathlib import Path

XPERF_NAME = 'xperf.exe' if os.name == 'nt' else 'xperf'
XPERF_FALLBACK = r'C:\Program Files (x86)\Windows Kits\10\Windows Performance Toolkit\xperf.exe'

PRESENT_EVENTS = ('Present/', 'PresentHistory', 'PresentMultiPlaneOverlay', 'Flip', 'Token', 'Dx_Flip_Consumed',
                  'VSync', 'SCHEDULE_PRESENT', 'SCHEDULE_SURFACEUPDATE', 'CompositionSurfaceObjectUpdate',
                  'QueuePacket', 'IndependentFlip')

# D3DKMT_PRESENTFLAGS of DxgKrnl/Present Info, every bit the WDK 10.0.26100 header declares (d3dkmthk.h:393).
PRESENT_FLAGS = ((0x1, 'Blt'), (0x2, 'ColorFill'), (0x4, 'Flip'), (0x8, 'FlipDoNotFlip'),
                 (0x10, 'FlipDoNotWait'), (0x20, 'FlipRestart'), (0x40, 'DstRectValid'), (0x80, 'SrcRectValid'),
                 (0x100, 'RestrictVidPnSource'), (0x200, 'SrcColorKey'), (0x400, 'DstColorKey'),
                 (0x800, 'LinearToSrgb'), (0x1000, 'PresentCountValid'), (0x2000, 'Rotate'),
                 (0x4000, 'PresentToBitmap'), (0x8000, 'RedirectedFlip'), (0x10000, 'RedirectedBlt'),
                 (0x20000, 'FlipStereo'), (0x40000, 'FlipStereoTemporaryMono'),
                 (0x80000, 'FlipStereoPreferRight'), (0x100000, 'BltStereoUseRight'),
                 (0x200000, 'PresentHistoryTokenOnly'), (0x400000, 'PresentRegionsValid'),
                 (0x800000, 'PresentDDA'), (0x1000000, 'ProtectedContentBlankedOut'),
                 (0x2000000, 'RemoteSession'), (0x4000000, 'CrossAdapter'), (0x8000000, 'DurationValid'),
                 (0x10000000, 'PresentIndirect'), (0x20000000, 'PresentHMD'))
# RedirectedFlip + PresentCountValid: the composed shape of a flip-model client, and increment 1's control.
PRESENT_FLAGS_REDIRECTED_FLIP = 0x9000

# D3DKMT_PRESENT_MODEL (d3dkmthk.h:443).
PRESENT_MODEL = {0: 'UNINITIALIZED', 1: 'REDIRECTED_GDI', 2: 'REDIRECTED_FLIP', 3: 'REDIRECTED_BLT',
                 4: 'REDIRECTED_VISTABLT', 5: 'SCREENCAPTUREFENCE', 6: 'REDIRECTED_GDI_SYSMEM',
                 7: 'REDIRECTED_COMPOSITION', 8: 'SURFACECOMPLETE', 9: 'FLIPMANAGER'}
PRESENT_MODEL_REDIRECTED_FLIP = 2

# Win32k TokenState (PresentMon's ETW/Microsoft_Windows_Win32k.h).
TOKEN_STATE = {0: 'Created', 1: 'Pending', 2: 'Completed', 3: 'InFrame', 4: 'Confirmed', 5: 'Retired',
               6: 'Discarded'}
TOKEN_STATE_IN_FRAME = 3

# MMIOFlip Info's Flags. The first three are confirmed for this event by PresentMon's generated model; the
# second three are the DDI word's next bits and are candidates only. See "flip flags" in the module docstring.
MMIO_FLAGS_CONFIRMED = ((0x1, 'ModeChange'), (0x2, 'FlipImmediate'), (0x4, 'FlipOnNextVSync'))
MMIO_FLAGS_CANDIDATE = ((0x20, 'SharedPrimaryTransition?'), (0x40, 'IndependentFlipExclusive?'),
                        (0x80, 'MoveFlip?'))
MMIO_FLAGS_CANDIDATE_NOTE = ('the 0x20/0x40/0x80 names come from DXGK_SETVIDPNSOURCEADDRESS_FLAGS and are '
                             'UNCONFIRMED for this event: they corroborate, they never witness')


def mmio_flag_text(value):
    """The Flags word of an MMIOFlip family event as text, with the unconfirmed bits marked."""
    if value is None:
        return 'no Flags field'
    named = [name for bit, name in MMIO_FLAGS_CONFIRMED if value & bit]
    named += [name for bit, name in MMIO_FLAGS_CANDIDATE if value & bit]
    known = sum(bit for bit, _ in MMIO_FLAGS_CONFIRMED) | sum(bit for bit, _ in MMIO_FLAGS_CANDIDATE)
    rest = value & ~known
    if rest:
        named.append('unknown 0x%X' % rest)
    return '+'.join(named) if named else 'none'


def present_flag_text(value):
    """The Flags word of DxgKrnl/Present Info as text."""
    if value is None:
        return 'no Flags field'
    named = [name for bit, name in PRESENT_FLAGS if value & bit]
    rest = value & ~sum(bit for bit, _ in PRESENT_FLAGS)
    if rest:
        named.append('unknown 0x%X' % rest)
    return '+'.join(named) if named else 'none'


def submit_sequence(name, text):
    """The submit sequence of an MMIOFlip family event.

    MMIOFlip Info's FlipSubmitSequence is the sequence itself, while the multi-plane overlay events pack it
    in the high 32 bits of a 64-bit field (PresentMon reads FlipSubmitSequence >> 32 for those). A plain
    MMIOFlip value that does not fit in 32 bits is read the same way rather than guessed at.
    """
    value = number(text or '')
    if value is None:
        return None
    if 'MultiPlaneOverlay' in name or value > 0xffffffff:
        return value >> 32
    return value


def number(text):
    try:
        return int(text, 16) if text.lower().startswith('0x') else int(text)
    except (ValueError, AttributeError):
        return None


def flag(text):
    """A BOOL field of these providers, which the dumper writes as "true"/"false" or as 0/1.

    number() returns None for "true", and bool(None) is False, so reading these fields as numbers made
    every independent-flip test fail silently and the positive branch unreachable. None means the field
    was absent or unreadable, which is not the same as false.
    """
    if text is None:
        return None
    lowered = text.strip().lower()
    if lowered in ('true', '1'):
        return True
    if lowered in ('false', '0'):
        return False
    value = number(lowered)
    return None if value is None else value != 0


def layouts(path):
    """name -> {field count: {field name: index}}, from the dumper's header block."""
    maps = collections.defaultdict(dict)
    with open(path, encoding='utf-8', errors='replace') as f:
        for line in f:
            if line.startswith('EndHeader'):
                break
            parts = [p.strip() for p in line.rstrip('\n').split(',')]
            if len(parts) < 3 or not parts[0] or parts[1] != 'TimeStamp':
                continue
            maps[parts[0]][len(parts)] = {name: i for i, name in enumerate(parts)}
    return maps


def rows(path):
    with open(path, encoding='utf-8', errors='replace') as f:
        header = True
        for line in f:
            if header:
                if line.startswith('EndHeader'):
                    header = False
                continue
            parts = [p.strip() for p in line.rstrip('\n').split(',')]
            if len(parts) > 3 and parts[1].isdigit():
                yield parts


class Reader:
    """One row with its header map; field() returns None for a field this layout does not have."""

    def __init__(self, maps):
        self.maps = maps
        self.missing = collections.Counter()

    def fields(self, parts):
        by_count = self.maps.get(parts[0])
        if not by_count:
            # No header line for this event kind at all. Every field comes back as None and the row is dropped;
            # parse() counts those rows in 'no_layout', once per row rather than once per field read.
            return None
        found = by_count.get(len(parts))
        if found is None:
            # Variable-length arrays make a row longer than its header; the fixed head still lines up.
            best = max((n for n in by_count if n <= len(parts)), default=None)
            if best is None:
                self.missing[parts[0]] += 1
                return None
            found = by_count[best]
        return found

    def field(self, parts, name):
        found = self.fields(parts)
        if not found:
            return None
        index = found.get(name)
        if index is None:
            # A struct field's header cell carries its member list with it, as in
            # "luidSurface {lowpart; highpart}", so the plain name never matches. The members stay in that
            # one cell in the data rows too, so the column still lines up.
            prefix = name + ' {'
            for header, position in found.items():
                if header.startswith(prefix):
                    index = position
                    break
        if index is None:
            return None
        return parts[index] if index < len(parts) else None

    def integer(self, parts, name):
        return number(self.field(parts, name) or '')

    def luid(self, parts, name):
        """A LUID field, either as one 64-bit number or as the dumper's expanded {lowpart; highpart} struct."""
        text = self.field(parts, name)
        if text is None:
            return None
        if 'lowpart' in text:
            low = high = None
            for piece in text.strip('[]{} ').split(';'):
                key, _, value = piece.partition(':')
                if key.strip() == 'lowpart':
                    low = number(value.strip())
                elif key.strip() == 'highpart':
                    high = number(value.strip())
            if low is None:
                return None
            return ((high or 0) << 32) | low
        return number(text)


def parse(dump):
    """Every table the report and the verdicts need, from one pass over a dumper text."""
    reader = Reader(layouts(dump))
    t = {
        'counts': collections.Counter(),
        'tokens': {},                                        # Token -> (time, process, model)
        'models': collections.defaultdict(collections.Counter),          # process -> Counter(model)
        'detailed_models': collections.defaultdict(collections.Counter),  # process -> Counter(model)
        'surfaces': {},                                      # key -> (process, destination size)
        'owner_of': {},                                      # key -> the process that presented it
        'keys': collections.defaultdict(set),                # process -> {(luid, bind, count)}
        'states': {},                                        # key -> (independent, skip, state): LAST wins
        'state_log': [],                                     # every row, as (key, independent, skip, state)
        'state_rows': collections.defaultdict(collections.Counter),       # process -> Counter(text)
        'inframe': collections.defaultdict(collections.Counter),  # process -> rows/independent/skip at InFrame
        'dwm_surface': {},                                   # key -> (direct, independent, scanout)
        'consumed': collections.defaultdict(collections.Counter),         # key -> Counter(event)
        'presents': [],                                      # (time, process, key) of flip-model presents
        'present_calls': collections.defaultdict(collections.Counter),    # process -> Counter(flags)
        'packets': collections.defaultdict(dict),            # process -> {submit sequence: first time}
        'packet_rows': collections.Counter(),
        'independent_flips': {},                             # sequence -> (time, interval)
        'mmio': collections.defaultdict(list),               # sequence -> [(time, event, flags, phys, alloc)]
        'mmio_physical': collections.Counter(),
        'mmio_allocation': collections.Counter(),
        'programmed': [],                                    # (time, event, address text, layer)
        'scanned': collections.defaultdict(list),            # event -> [(time, value)]
        'scanned_values': collections.Counter(),
        'scanned_sequence': collections.Counter(),
        'flip_info': collections.Counter(),
        'dwm_schedule': 0,
        'span_us': (None, None),
        'missing': reader.missing,
        'no_layout': collections.Counter(),
    }
    t_min = t_max = None

    for parts in rows(dump):
        name, time, process = parts[0], int(parts[1]), parts[2]
        t['counts'][name] += 1
        if name not in reader.maps:
            # The dumper writes a header line for every event kind it emits, so this is a damaged or
            # hand-edited text. Those rows read as all-None and would vanish without a word.
            t['no_layout'][name] += 1
        t_min = time if t_min is None else min(t_min, time)
        t_max = time if t_max is None else max(t_max, time)
        if name.startswith('Microsoft-Windows-DxgKrnl/PresentHistory'):
            token = reader.integer(parts, 'Token')
            model = reader.integer(parts, 'Model')
            if token:
                t['tokens'].setdefault(token, (time, process, model))
            if model is not None:
                t['models'][process][model] += 1
                if name.startswith('Microsoft-Windows-DxgKrnl/PresentHistoryDetailed'):
                    t['detailed_models'][process][model] += 1
        elif name.startswith('Microsoft-Windows-Win32k/TokenCompositionSurfaceObject'):
            luid = reader.luid(parts, 'CompositionSurfaceLuid')
            bind = reader.integer(parts, 'BindId')
            count = reader.integer(parts, 'PresentCount')
            if None not in (luid, bind, count):
                # The present belongs to the process this row was logged in. The pToken join to
                # PresentHistory is NOT used for that (see "flip model" in the module docstring): pToken is a
                # pointer to a reused kernel object, the composed baseline has one distinct value for 604
                # rows, and a wrong join would move a present to another process in silence. The token's own
                # timestamp is taken only when the token matches and names this very process.
                token = reader.integer(parts, 'pToken')
                origin = t['tokens'].get(token)
                when = origin[0] if origin and origin[1] == process else time
                key = (luid, bind, count)
                t['surfaces'][key] = (process, '%sx%s' % (reader.integer(parts, 'DestWidth'),
                                                          reader.integer(parts, 'DestHeight')))
                t['owner_of'][key] = process
                t['keys'][process].add(key)
                t['presents'].append((when, process, key))
        elif name.startswith('Microsoft-Windows-Win32k/TokenStateChanged'):
            luid = reader.luid(parts, 'CompositionSurfaceLuid')
            bind = reader.integer(parts, 'BindId')
            count = reader.integer(parts, 'PresentCount')
            independent = flag(reader.field(parts, 'IndependentFlip'))
            skip = flag(reader.field(parts, 'SkipIndependentFlip'))
            state = reader.integer(parts, 'NewState')
            if None not in (luid, bind, count):
                # Last state wins. The rows arrive in time order, and these flags are the token's state,
                # not an accumulating set of things that ever happened to it: a sticky OR could only ever
                # turn a present that ended up composed into a reported hardware flip.
                t['states'][(luid, bind, count)] = (independent, skip, state)
                t['state_log'].append(((luid, bind, count), independent, skip, state))
        elif name.startswith(('Microsoft-Windows-Dwm-Core/Dx_Flip_Consumed',
                              'Microsoft-Windows-Dwm-Core/Windowed_Dx_Flip_Consumed')):
            luid = reader.integer(parts, 'surfaceLuid')
            bind = reader.integer(parts, 'bindId')
            count = reader.integer(parts, 'presentCount')
            if None not in (luid, bind, count):
                t['consumed'][(luid, bind, count)][name.split('/')[1]] += 1
        elif name.startswith('Microsoft-Windows-Dwm-Core/SCHEDULE_SURFACEUPDATE'):
            luid = reader.luid(parts, 'luidSurface')
            bind = reader.integer(parts, 'bindId')
            count = reader.integer(parts, 'PresentCount')
            if None not in (luid, bind, count):
                t['dwm_surface'][(luid, bind, count)] = (flag(reader.field(parts, 'bDirectFlip')),
                                                         flag(reader.field(parts, 'bIndependentFlip')),
                                                         flag(reader.field(parts, 'bEnableScanout')))
        elif name.startswith('Microsoft-Windows-Dwm-Core/SCHEDULE_PRESENT/win:Start'):
            t['dwm_schedule'] += 1
        elif name.startswith('Microsoft-Windows-DxgKrnl/Present/win:Info'):
            t['present_calls'][process][reader.integer(parts, 'Flags')] += 1
        elif name.startswith('Microsoft-Windows-DxgKrnl/QueuePacket/win:Start'):
            sequence = reader.integer(parts, 'SubmitSequence')
            # bPresent is the packet of a present. A capture whose layout has no such field leaves every
            # packet out rather than counting command buffers as frames.
            if sequence is not None and flag(reader.field(parts, 'bPresent')):
                t['packet_rows'][process] += 1
                t['packets'][process].setdefault(sequence, time)
        elif name.startswith('Microsoft-Windows-DxgKrnl/IndependentFlip'):
            sequence = reader.integer(parts, 'SubmitSequence')
            if sequence is not None:
                t['independent_flips'].setdefault(sequence, (time, reader.integer(parts, 'FlipInterval')))
        elif name.startswith('Microsoft-Windows-DxgKrnl/Flip/win:Info'):
            t['flip_info']['MMIOFlip %s' % flag(reader.field(parts, 'MMIOFlip'))] += 1
        elif name.startswith('Microsoft-Windows-DxgKrnl/VSync'):
            value = reader.integer(parts, 'ScannedPhysicalAddress')
            if value is not None:
                t['scanned'][name.split('/')[1]].append((time, value))
                t['scanned_values'][value] += 1
            # VSyncDPC's FlipFenceId carries the submit sequence in its high 32 bits: the frame the plane
            # is reading, from the display side.
            fence = reader.integer(parts, 'FlipFenceId')
            if fence is not None:
                t['scanned_sequence'][fence >> 32] += 1
        elif 'MMIOFlip' in name or name.startswith('Microsoft-Windows-DxgKrnl/FlipMultiPlaneOverlay'):
            event = name.split('/')[1]
            physical = reader.integer(parts, 'FlipToPhysicalAddress')
            allocation = reader.integer(parts, 'FlipToDriverAllocation')
            address = reader.field(parts, 'FlipToPhysicalAddress') or reader.field(parts, 'FlipToDriverAllocation') \
                or reader.field(parts, 'hAllocation') or reader.field(parts, 'FlipPresentId')
            t['programmed'].append((time, event, address, reader.field(parts, 'LayerIndex')))
            if physical is not None:
                t['mmio_physical'][physical] += 1
            if allocation is not None:
                t['mmio_allocation'][allocation] += 1
            sequence = submit_sequence(name, reader.field(parts, 'FlipSubmitSequence'))
            if sequence is not None:
                t['mmio'][sequence].append((time, event, reader.integer(parts, 'Flags'), physical, allocation))
    # Every token state is attributed to the presenting process through the surface key, because the events
    # themselves are logged in the compositor. This is the whole distribution, not the last state per key:
    # increment 3 asks for the InFrame rows and their two flags.
    for key, independent, skip, state in t['state_log']:
        owner = t['owner_of'].get(key)
        if owner is None:
            t['state_rows']['(no surface object)']['%s independent %s skip %s'
                                                  % (TOKEN_STATE.get(state, 'state %s' % state),
                                                     independent, skip)] += 1
            continue
        t['state_rows'][owner]['%s independent %s skip %s'
                               % (TOKEN_STATE.get(state, 'state %s' % state), independent, skip)] += 1
        if state == TOKEN_STATE_IN_FRAME:
            # Increment 3's own evidence: an InFrame token with IndependentFlip true and SkipIndependentFlip
            # false is the compositor promoting the client to the independent path.
            t['inframe'][owner]['rows'] += 1
            t['inframe'][owner]['independent'] += 1 if independent else 0
            t['inframe'][owner]['skip'] += 1 if skip else 0
    t['span_us'] = (t_min, t_max)
    return t


def span_seconds(t):
    lo, hi = t['span_us']
    return (hi - lo) / 1e6 if lo is not None and hi is not None and hi > lo else 0.0


def selected(t, wanted):
    """Every process the tables know, filtered by the substring arguments (pid or image name)."""
    names = set(t['present_calls']) | set(t['packets']) | set(p for _time, p, _key in t['presents'])
    if not wanted:
        return sorted(names)
    return sorted(n for n in names if any(w.lower() in n.lower() for w in wanted))


def present_mode(t, key):
    """The compositor-side mode of one present, as (mode, reason). The reason is None unless unclassified."""
    independent, skip, _state = t['states'].get(key, (None, None, None))
    if independent and not skip:
        return 'hardware flip (independent)', None
    if key in t['consumed']:
        return 'composed flip (DWM consumed)', None
    if independent and skip:
        return 'composed flip (independent skipped)', None
    return 'unclassified', 'no TokenStateChanged' if key not in t['states'] else 'state without consumption'


def classify(t, process):
    """The compositor-side mode of each present of one process, plus DWM's own answer for the same keys."""
    modes = collections.Counter()
    dwm_view = collections.Counter()
    reasons = collections.Counter()
    first = last = None
    for time, owner, key in t['presents']:
        if owner != process:
            continue
        mode, reason = present_mode(t, key)
        if reason:
            reasons[reason] += 1
        modes[mode] += 1
        if key in t['dwm_surface']:
            direct, dwm_independent, scanout = t['dwm_surface'][key]
            dwm_view['direct flip %s, independent %s, scanout %s' % (direct, dwm_independent, scanout)] += 1
        else:
            dwm_view['no SCHEDULE_SURFACEUPDATE'] += 1
        first = time if first is None else min(first, time)
        last = time if last is None else max(last, time)
    return modes, dwm_view, reasons, (first, last)


def kernel_side(t, process):
    """The kernel's own per-frame answer for one process: its present packets and what happened to them."""
    own = t['packets'].get(process, {})
    flips = sorted(s for s in own if s in t['independent_flips'])
    programmed = sorted(s for s in own if s in t['mmio'])
    scanned = sorted(s for s in own if s in t['scanned_sequence'])
    physical = collections.Counter()
    flags = collections.Counter()
    for sequence in programmed:
        for _time, event, value, phys, _alloc in t['mmio'][sequence]:
            flags['%s %s' % (event, mmio_flag_text(value))] += 1
            if phys is not None:
                physical[phys] += 1
    return {'packets': own, 'flips': flips, 'programmed': programmed, 'scanned': scanned,
            'physical': physical, 'flags': flags}


def parse_counters(values):
    """`--kmd-counters name=number,...` into a dict; the names are bc250kmd_cli's own spellings."""
    out = {}
    for chunk in values or []:
        for piece in chunk.replace(';', ',').split(','):
            piece = piece.strip()
            if not piece:
                continue
            name, _, text = piece.partition('=')
            value = number(text.strip())
            if value is None:
                raise SystemExit('--kmd-counters: %r is not <name>=<number>' % piece)
            out[name.strip()] = value
    return out


def increment1(t, process):
    """The inert control of increment 1: the capture must still be the composed baseline.

    Five clauses, each PASS, FAIL or NO-DATA. The whole check is NO-DATA when the process presented nothing,
    MOVED when any clause failed, INERT when every clause passed.
    """
    clauses = []
    calls = t['present_calls'].get(process, collections.Counter())
    other = {f: n for f, n in calls.items() if f != PRESENT_FLAGS_REDIRECTED_FLIP}
    clauses.append(('present_flags_9000',
                    'NO-DATA' if not calls else ('PASS' if not other else 'FAIL'),
                    '%d of %d calls' % (calls.get(PRESENT_FLAGS_REDIRECTED_FLIP, 0), sum(calls.values()))
                    + ('' if not other else '; also %s' % {present_flag_text(f): n for f, n in other.items()})))

    detailed = t['detailed_models'].get(process, collections.Counter())
    wrong = {m: n for m, n in detailed.items() if m != PRESENT_MODEL_REDIRECTED_FLIP}
    clauses.append(('detailed_model_2',
                    'NO-DATA' if not detailed else ('PASS' if not wrong else 'FAIL'),
                    '%d of %d tokens' % (detailed.get(PRESENT_MODEL_REDIRECTED_FLIP, 0), sum(detailed.values()))
                    + ('' if not wrong else '; also %s'
                       % {PRESENT_MODEL.get(m, 'model %s' % m): n for m, n in wrong.items()})))

    keys = t['keys'].get(process, set())
    updates = [t['dwm_surface'][k] for k in keys if k in t['dwm_surface']]
    composed = sum(1 for direct, independent, _scanout in updates if direct is True and independent is False)
    clauses.append(('dwm_directflip_1_independent_0',
                    'NO-DATA' if not updates else ('PASS' if composed == len(updates) else 'FAIL'),
                    '%d of %d surface updates' % (composed, len(updates))))

    kernel = kernel_side(t, process)
    clauses.append(('no_independent_flip',
                    'PASS' if not kernel['flips'] and not t['independent_flips'] else 'FAIL',
                    '%d naming this process, %d in the trace'
                    % (len(kernel['flips']), len(t['independent_flips']))))

    plane = len(kernel['programmed']) + len(kernel['scanned'])
    clauses.append(('no_plane_for_this_process',
                    'PASS' if plane == 0 else 'FAIL',
                    '%d packets reached MMIOFlip, %d named by a VSync DPC'
                    % (len(kernel['programmed']), len(kernel['scanned']))))

    if any(status == 'FAIL' for _name, status, _detail in clauses):
        result = 'MOVED'
    elif not calls and not kernel['packets']:
        result = 'NO-DATA'
    else:
        result = 'INERT'
    return {'result': result, 'clauses': clauses}


def increment2(t, process, admitted, counters):
    """The conjunction of increment 2, because DirectFlip alone has no unique ETW event.

    CONFIRMED needs all four clauses PASS. A clause with no input is NO-DATA and the whole verdict is UNKNOWN;
    a clause that contradicts the conjunction is FAIL and the verdict is REFUTED.
    """
    clauses = []
    keys = t['keys'].get(process, set())
    windowed = plain = 0
    for key in keys:
        windowed += t['consumed'].get(key, {}).get('Windowed_Dx_Flip_Consumed', 0)
        plain += t['consumed'].get(key, {}).get('Dx_Flip_Consumed', 0)
    clauses.append(('not_consumed_by_dwm',
                    'NO-DATA' if not keys else ('PASS' if windowed + plain == 0 else 'FAIL'),
                    '%d Windowed_Dx_Flip_Consumed and %d Dx_Flip_Consumed over %d present counts'
                    % (windowed, plain, len(keys))))

    kernel = kernel_side(t, process)
    matched = sorted(a for a in kernel['physical'] if a in admitted)
    if not admitted:
        status, detail = 'NO-DATA', 'no --admitted-address given; %d addresses programmed for this process' \
                                   % len(kernel['physical'])
    elif matched:
        status, detail = 'PASS', 'programmed %s' % ', '.join('0x%X' % a for a in matched)
    else:
        status, detail = 'FAIL', 'none of %d admitted addresses among the %d programmed for this process' \
                                 % (len(admitted), len(kernel['physical']))
    clauses.append(('mmio_programmed_an_admitted_address', status, detail))

    # The DPC alone, not every VSync event: the decision names VSyncDPC.ScannedPhysicalAddress as the
    # first-rank witness, because it does not depend on what DWM believed.
    dpc = set(value for _time, value in t['scanned'].get('VSyncDPC', []))
    scanned_match = sorted(a for a in matched if a in dpc)
    if not matched:
        status, detail = 'NO-DATA', 'clause 2 named no address to look for'
    elif not dpc:
        status, detail = 'NO-DATA', 'no VSyncDPC event with a ScannedPhysicalAddress in this capture'
    elif scanned_match:
        status, detail = 'PASS', 'scanned %s' % ', '.join('0x%X' % a for a in scanned_match)
    else:
        status, detail = 'FAIL', 'no VSync DPC scanned %s' % ', '.join('0x%X' % a for a in matched)
    clauses.append(('vsync_scanned_that_address', status, detail))

    flips = counters.get('scanout_flips')
    requests = counters.get('scanout_requests')
    refusals = {name: value for name, value in counters.items()
                if name.startswith('admit_') and name != 'admit_ok'}
    bad = {name: value for name, value in refusals.items() if value}
    if flips is None or requests is None or not refusals:
        status = 'NO-DATA'
        detail = 'need scanout_flips, scanout_requests and at least one admit_* refusal column in ' \
                 '--kmd-counters; have %s' % (sorted(counters) or 'none')
    elif flips > 0 and requests > 0 and not bad:
        status = 'PASS'
        detail = 'scanout_flips %d, scanout_requests %d, %d refusal columns all zero' \
                 % (flips, requests, len(refusals))
    else:
        status = 'FAIL'
        detail = 'scanout_flips %d, scanout_requests %d, refusals %s' % (flips, requests, bad or 'none')
    clauses.append(('kernel_counters', status, detail))

    statuses = [status for _name, status, _detail in clauses]
    if 'FAIL' in statuses:
        result = 'REFUTED'
    elif 'NO-DATA' in statuses:
        result = 'UNKNOWN'
    else:
        result = 'CONFIRMED'
    return {'result': result, 'clauses': clauses}


def clause_text(verdict):
    return ' '.join('%s=%s' % (name, status) for name, status, _detail in verdict['clauses'])


def report(t, wanted, admitted, counters, out=sys.stdout):
    """The whole report, and the verdict dictionary a caller may write as JSON."""
    def say(text=''):
        print(text, file=out)

    span = span_seconds(t)
    say('trace span %.3f s, %d events, %d event kinds' % (span, sum(t['counts'].values()), len(t['counts'])))
    if t['no_layout']:
        say('rows with no usable header layout: %s' % dict(t['no_layout']))
    if t['missing']:
        say('field reads past the end of a header layout: %s' % dict(t['missing']))
    say()
    say('-- present and flip events --')
    for name, n in sorted(t['counts'].items(), key=lambda kv: -kv[1]):
        if any(key in name for key in PRESENT_EVENTS):
            say('%8d  %s' % (n, name))

    names = selected(t, wanted)
    say()
    say('-- present mode per process --')
    if not names:
        say('no present of a named process in this dump')
    timeline = collections.defaultdict(collections.Counter)
    for process in sorted(names, key=lambda p: -(sum(t['present_calls'].get(p, {}).values())
                                                 + len(t['packets'].get(p, {})))):
        modes, dwm_view, reasons, (first, last) = classify(t, process)
        calls = t['present_calls'].get(process, collections.Counter())
        extra = sum(calls.values()) - sum(modes.values())
        if extra > 0:
            # A Present call beyond the token presents went through the blt or legacy path. The difference is
            # taken per process, not per present: no field in this recipe joins the two events.
            modes['no present-history token'] += extra
        total = sum(modes.values())
        own = (last - first) / 1e6 if first is not None and last > first else 0.0
        say('%s: %d presents over its own %.3f s, %.1f/s (%d D3DKMTPresent calls; trace span %.3f s)'
            % (process, total, own, total / own if own else 0.0, sum(calls.values()), span))
        for mode, n in sorted(modes.items(), key=lambda kv: -kv[1]):
            say('    %-38s %6d  %5.1f%%' % (mode, n, 100.0 * n / total if total else 0.0))
        for flags, n in sorted(calls.items(), key=lambda kv: -kv[1]):
            say('    Present Flags 0x%X %-24s %6d' % (flags or 0, '(%s)' % present_flag_text(flags), n))
        for model, n in sorted(t['models'].get(process, {}).items(), key=lambda kv: -kv[1]):
            detailed = t['detailed_models'].get(process, {}).get(model, 0)
            say('    Model %d %-32s %6d  (%d from PresentHistoryDetailed)'
                % (model, '(%s)' % PRESENT_MODEL.get(model, 'unknown'), n, detailed))
        for text, n in sorted(t['state_rows'].get(process, {}).items(), key=lambda kv: -kv[1]):
            say('    TokenStateChanged %-34s %6d' % (text, n))
        at_frame = t['inframe'].get(process)
        if at_frame:
            say('    InFrame rows %d, with IndependentFlip %d, with SkipIndependentFlip %d'
                % (at_frame['rows'], at_frame['independent'], at_frame['skip']))
        for view, n in sorted(dwm_view.items(), key=lambda kv: -kv[1]):
            say('    DWM says: %-28s %6d' % (view, n))
        if reasons:
            say('    unclassified reasons: %s' % dict(reasons))
        lo = t['span_us'][0]
        if lo is not None:
            for time, owner, key in t['presents']:
                if owner == process:
                    timeline[int((time - lo) / 1e6)][present_mode(t, key)[0]] += 1

    say()
    say('-- per second (mode: presents) --')
    for second in sorted(timeline):
        say('%4d  %s' % (second, ', '.join('%s %d' % (m, n) for m, n in sorted(timeline[second].items()))))

    say()
    say('-- per-frame flip witness (kernel side) --')
    if not t['packets']:
        say('no DxgKrnl/QueuePacket Start with bPresent in this dump: the per-frame witness needs the '
            'DxgKrnl session at a keyword mask that carries it (the capture recipe uses 0xffffffffffffffff)')
    verdicts = {}
    reported = list(names)
    for word in wanted:
        if not any(word.lower() in name.lower() for name in names):
            # A verdict of zero, not silence: "the process presented nothing" and "the events are missing"
            # are different findings and a trial must be able to tell them apart.
            reported.append(word)
    for process in reported:
        kernel = kernel_side(t, process)
        own, flips = kernel['packets'], kernel['flips']
        modes, dwm_view, _reasons, _window = classify(t, process)
        say('%s: %d present packets (%d rows), %d independent flips, %d reached MMIOFlip, %d named by a '
            'VSync DPC' % (process, len(own), t['packet_rows'].get(process, 0), len(flips),
                           len(kernel['programmed']), len(kernel['scanned'])))
        if flips:
            say('    flip intervals: %s'
                % dict(collections.Counter(t['independent_flips'][s][1] for s in flips)))
        for text, n in sorted(kernel['flags'].items(), key=lambda kv: -kv[1]):
            say('    %-52s %6d' % (text, n))
        for address, n in sorted(kernel['physical'].items(), key=lambda kv: -kv[1])[:6]:
            say('    FlipToPhysicalAddress 0x%016X %6d%s'
                % (address, n, '  ADMITTED' if address in admitted else ''))
        # The compositor's side for the same process, from the tables above: how many of its surface updates
        # DWM answered DirectFlip for. The two sides are counted, never joined (see the module docstring).
        direct_yes = direct_total = 0
        for view, n in dwm_view.items():
            if view.startswith('direct flip'):
                direct_total += n
                if view.startswith('direct flip True'):
                    direct_yes += n
        result = 'INDEPENDENT-FLIP' if flips else ('COMPOSED' if direct_total or own else 'NO-DATA')
        say('M15.14 VERDICT %s: packets=%d independent=%d mmio=%d vsync=%d directflip=%d/%d result=%s'
            % (process, len(own), len(flips), len(kernel['programmed']), len(kernel['scanned']),
               direct_yes, direct_total, result))
        one = increment1(t, process)
        two = increment2(t, process, admitted, counters)
        say('M15.14 INCREMENT1 %s: %s result=%s' % (process, clause_text(one), one['result']))
        for name, status, detail in one['clauses']:
            say('    %-32s %-8s %s' % (name, status, detail))
        say('M15.14 INCREMENT2 %s: %s result=%s' % (process, clause_text(two), two['result']))
        for name, status, detail in two['clauses']:
            say('    %-36s %-8s %s' % (name, status, detail))
        verdicts[process] = {'mode': result, 'packets': len(own), 'independent': len(flips),
                             'mmio': len(kernel['programmed']), 'vsync': len(kernel['scanned']),
                             'directflip': [direct_yes, direct_total], 'modes': dict(modes),
                             'increment1': one, 'increment2': two}

    say()
    say('-- display side --')
    say('DWM SCHEDULE_PRESENT starts %d' % t['dwm_schedule'])
    # ScannedPhysicalAddress is reported per event, because the two events do not mean the same thing: the
    # interrupt reports the address the plane is reading, while on a driver that flips through a multi-plane
    # overlay the DPC's field carries a present id instead. Read the values, not the field's name.
    for event in sorted(t['scanned']):
        samples = t['scanned'][event]
        values = collections.Counter(v for _, v in samples)
        changes = sum(1 for i in range(1, len(samples)) if samples[i][1] != samples[i - 1][1])
        say('%s ScannedPhysicalAddress: %d samples, %d distinct, %d changes (%.1f/s)'
            % (event, len(samples), len(values), changes, changes / span if span else 0.0))
        for value, n in values.most_common(6):
            say('    0x%016X  %6d samples%s' % (value, n, '  ADMITTED' if value in admitted else ''))
    if t['scanned']:
        say('    (a changing address is what a composing desktop does too; compare a distinct value with an')
        say('     address the kernel driver logged as an admitted scan-out flip, not with the change rate)')
    say('driver-programmed flips: %s'
        % (dict(collections.Counter(event for _t, event, _a, _l in t['programmed'])) or 'none'))
    for address, n in t['mmio_physical'].most_common(6):
        say('    FlipToPhysicalAddress   0x%016X %6d%s'
            % (address, n, '  ADMITTED' if address in admitted else ''))
    for allocation, n in t['mmio_allocation'].most_common(6):
        say('    FlipToDriverAllocation  0x%016X %6d' % (allocation, n))
    if t['flip_info']:
        say('DxgKrnl/Flip Info: %s' % dict(t['flip_info']))
    say('independent-flip events: %d, present packets with one: %d'
        % (len(t['independent_flips']),
           sum(1 for p in t['packets'] for s in t['packets'][p] if s in t['independent_flips'])))
    seen_candidate = any(value and value & sum(bit for bit, _ in MMIO_FLAGS_CANDIDATE)
                         for rows_ in t['mmio'].values() for _t, _e, value, _p, _a in rows_
                         if value is not None)
    if seen_candidate:
        say('WARNING: an MMIOFlip Flags word carried 0x20, 0x40 or 0x80. %s' % MMIO_FLAGS_CANDIDATE_NOTE)
    return verdicts


def find_xperf(given=None):
    for candidate in (given, os.environ.get('BC250_XPERF'), shutil.which(XPERF_NAME), XPERF_FALLBACK):
        if candidate and Path(candidate).is_file():
            return str(candidate)
    raise SystemExit('xperf.exe not found: pass --xperf, set BC250_XPERF, or install the Windows Performance\n'
                     'Toolkit. tracerpt is not a substitute: its CSV is a different format, and feeding it to\n'
                     'an xperf parser is the mistake recorded in evidence/windows/2026-09-27-E34-dwm012-artifacts.')


def dump_etl(etl, out, xperf=None):
    """The dumper text of an .etl, the same xperf path etwclock.py and etwdump.py use."""
    tool = find_xperf(xperf)
    done = subprocess.run([tool, '-i', str(etl), '-o', str(out), '-a', 'dumper'],
                          capture_output=True, text=True, check=False)
    if done.returncode != 0 or not Path(out).is_file():
        raise SystemExit('xperf dumper failed (%d): %s' % (done.returncode, (done.stderr or '').strip()[-400:]))
    return Path(out)


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('trace', help='the capture: an .etl, or an existing xperf dumper text')
    ap.add_argument('process', nargs='*', help='substring of the process column (image name or bare pid)')
    ap.add_argument('--keep-dump', action='store_true',
                    help='keep the text dump this run made from an .etl (it is deleted by default: a dump of '
                         'a 44 MB trace is 51 MB)')
    ap.add_argument('--dump-out', help='where to write that text dump (default: <trace>-dump.txt beside the .etl)')
    ap.add_argument('--xperf', help='path to xperf.exe')
    ap.add_argument('--admitted-address', action='append', default=[], metavar='ADDR',
                    help='a physical address the kernel driver logged as an admitted scan-out flip; repeatable. '
                         'Increment 2 clauses 2 and 3 need at least one')
    ap.add_argument('--kmd-counters', action='append', default=[], metavar='NAME=N',
                    help="bc250kmd_cli counter deltas for increment 2 clause 4, comma separated: "
                         "'scanout_flips=12,scanout_requests=12,admit_alignment=0'")
    ap.add_argument('--json', dest='json_out', help='also write the verdicts to this file as JSON')
    args = ap.parse_args(argv)

    admitted = set()
    for text in args.admitted_address:
        value = number(text.strip())
        if value is None:
            raise SystemExit('--admitted-address: %r is not a number' % text)
        admitted.add(value)
    counters = parse_counters(args.kmd_counters)

    trace = Path(args.trace)
    if not trace.is_file():
        raise SystemExit('no such file: %s' % trace)
    made_dump = None
    if trace.suffix.lower() == '.etl':
        out = Path(args.dump_out) if args.dump_out else trace.with_name(trace.stem + '-dump.txt')
        print('xperf dumper: %s -> %s' % (trace, out))
        made_dump = dump_etl(trace, out, args.xperf)
        dump = made_dump
    else:
        if args.dump_out:
            raise SystemExit('--dump-out applies to an .etl argument only')
        dump = trace
    try:
        t = parse(dump)
        verdicts = report(t, args.process, admitted, counters)
        if args.json_out:
            Path(args.json_out).write_text(json.dumps(verdicts, indent=1, sort_keys=True) + '\n',
                                           encoding='ascii')
            print('\nverdicts written to %s' % args.json_out)
    finally:
        if made_dump is not None and made_dump.is_file():
            if args.keep_dump:
                print('text dump kept at %s (%.1f MB)' % (made_dump, made_dump.stat().st_size / 1e6))
            else:
                size = made_dump.stat().st_size
                made_dump.unlink()
                print('text dump deleted (%.1f MB); --keep-dump keeps it' % (size / 1e6))
    return 0


if __name__ == '__main__':
    sys.exit(main())
