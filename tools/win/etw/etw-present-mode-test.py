#!/usr/bin/env python3
"""Host test for etw-present-mode.py: the classification, the two verdicts, and the flag decoders.

    python etw-present-mode-test.py

No ETW session, no lab, no capture of its own. It runs over two fixtures:

  the kept trace    testdata/base-composed-excerpt.txt, one second of the composed baseline
                    (scratch/train/etw-iflip-111536/gpu.etl, b18r1, 2026-10-05), trimmed by
                    testdata/make-fixture.py. Every shape in it is a real shape of this driver, including the
                    one the composed route takes: the client's Present Flags 0x9000, Model 2, DWM answering
                    bDirectFlip 1 with bIndependentFlip 0, and Windowed_Dx_Flip_Consumed for nearly every
                    present. Increment 1's control must read INERT on it and increment 2's conjunction must
                    read REFUTED, because the compositor consumed the frames.
  The Witcher 3     testdata/w3-exclusive-flip-excerpt.txt and w3-exclusive-1080-composed-excerpt.txt, one
                    second each of trial 478 (2026-10-08). The first is the game in exclusive fullscreen at the
                    native mode: every present independent-flipped and programmed, the VSync DPC scanning the
                    same three addresses, nothing consumed, and Win32k marking every InFrame token
                    SkipIndependentFlip true. The kernel witness must HOLD there and relabel those presents. The
                    second is the same game at 1920x1080, composed: the witness must not hold and nothing moves.
  synthetic         dumper texts written here, in the real header shapes, for the branches no capture of this
                    driver has ever taken: a hardware independent flip, a withdrawn independent candidate, an
                    MMIOFlip whose Flags carries the unconfirmed 0x40 bit, a full increment-2 conjunction
                    with the kernel driver's numbers supplied, and the negative controls of the kernel witness
                    (one present consumed, the DPC scanning another address, no independent flip at all).

Why the synthetic half exists: the independent-flip branch could not fire at all. The two flags are BOOL fields
that the dumper writes as "true"/"false", the parser read them with int(), int("true") raises, and the result was
None, whose bool() is False. Four runs against real traces reported no hardware flip and the reason looked like
the hardware. A branch that has never run is not implemented, so it gets a case here.

Set BC250_ETW_TEST_ETL to an .etl to also exercise the xperf path and the dump deletion. That case is skipped
when the variable is absent, because no .etl belongs in this repository.
"""
import importlib.util
import os
import re
import subprocess
import sys
import tempfile
from pathlib import Path

HERE = Path(__file__).resolve().parent
PARSER = HERE / 'etw-present-mode.py'
KEPT = HERE / 'testdata' / 'base-composed-excerpt.txt'
W3_FLIP = HERE / 'testdata' / 'w3-exclusive-flip-excerpt.txt'
W3_COMPOSED = HERE / 'testdata' / 'w3-exclusive-1080-composed-excerpt.txt'

# The header lines of the synthetic fixtures are the real ones, copied from the kept trace's header block, so a
# synthetic row cannot pass through a column layout that no capture produces.
HEADER = [
    'Microsoft-Windows-DxgKrnl/VSyncDPC/win:Info,  TimeStamp,     Process Name ( PID),   ThreadID, CPU,'
    ' etw:ActivityId, etw:Related ActivityId, etw:UserSid, etw:SessionId, pDxgAdapter, VidPnTargetId,'
    ' ScannedPhysicalAddress, VidPnSourceId, FrameNumber, FrameQPCTime, hFlipDevice, FlipType, FlipFenceId',
    'Microsoft-Windows-DxgKrnl/MMIOFlip/win:Info,  TimeStamp,     Process Name ( PID),   ThreadID, CPU,'
    ' etw:ActivityId, etw:Related ActivityId, etw:UserSid, etw:SessionId, pDxgAdapter, VidPnSourceId,'
    ' FlipSubmitSequence, FlipToDriverAllocation, FlipToPhysicalAddress, FlipToSegmentId, FlipPresentId,'
    ' FlipPhysicalAdapterMask, Flags',
    'Microsoft-Windows-DxgKrnl/PresentHistoryDetailed/win:Start,  TimeStamp,     Process Name ( PID),'
    '   ThreadID, CPU, etw:ActivityId, etw:Related ActivityId, etw:UserSid, etw:SessionId, hAdapter, Token,'
    ' Model, TokenSize, TokenData, Flags, CustomDuration, ScrollRect.left, ScrollRect.right, ScrollRect.top,'
    ' ScrollRect.bottom, ScrollOffset.X, ScrollOffset.Y, DirtyRectCount, Left[DirtyRectCount],'
    ' Right[DirtyRectCount], Top[DirtyRectCount], Bottom[DirtyRectCount], SourceRect.left, SourceRect.right,'
    ' SourceRect.top, SourceRect.bottom, DestWidth, DestHeight, TargetRect.left, TargetRect.right,'
    ' TargetRect.top, TargetRect.bottom, VmBusChannel',
    'Microsoft-Windows-DxgKrnl/Present/win:Info,  TimeStamp,     Process Name ( PID),   ThreadID, CPU,'
    ' etw:ActivityId, etw:Related ActivityId, etw:UserSid, etw:SessionId, hContext, hWindow, VidPnSourceId,'
    ' FlipInterval, Flags, ReturnStatus, hSrcAllocHandle, hDstAllocHandle',
    'Microsoft-Windows-DxgKrnl/QueuePacket/win:Start,  TimeStamp,     Process Name ( PID),   ThreadID, CPU,'
    ' etw:ActivityId, etw:Related ActivityId, etw:UserSid, etw:SessionId, hContext, PacketType, SubmitSequence,'
    ' DmaBufferSize, AllocationListSize, PatchLocationListSize, bPresent, hDmaBuffer, pQueuePacket,'
    ' ProgressFenceValue',
    'Microsoft-Windows-DxgKrnl/IndependentFlip/win:Info,  TimeStamp,     Process Name ( PID),   ThreadID, CPU,'
    ' etw:ActivityId, etw:Related ActivityId, etw:UserSid, etw:SessionId, SubmitSequence, FlipInterval',
    'Microsoft-Windows-DxgKrnl/VSyncInterrupt/win:Info,  TimeStamp,     Process Name ( PID),   ThreadID, CPU,'
    ' etw:ActivityId, etw:Related ActivityId, etw:UserSid, etw:SessionId, pDxgAdapter, VidPnTargetId,'
    ' ScannedPhysicalAddress',
    'Microsoft-Windows-Win32k/TokenCompositionSurfaceObject/win:Info,  TimeStamp,     Process Name ( PID),'
    '   ThreadID, CPU, etw:ActivityId, etw:Related ActivityId, etw:UserSid, etw:SessionId, pToken,'
    ' pCompositionSurfaceObject, SwapChainIndex, PresentCount, CompositionSurfaceLuid, BindId, FlipInterval,'
    ' DestWidth, DestHeight',
    'Microsoft-Windows-Win32k/TokenStateChanged/win:Info,  TimeStamp,     Process Name ( PID),   ThreadID, CPU,'
    ' etw:ActivityId, etw:Related ActivityId, etw:UserSid, etw:SessionId, pCompositionSurfaceObject,'
    ' SwapChainIndex, PresentCount, FenceValue, NewState, IndependentFlip, SkipIndependentFlip,'
    ' CompositionSurfaceLuid, BindId, EarlyComposition',
    'Microsoft-Windows-Dwm-Core/SCHEDULE_SURFACEUPDATE/win:Info,  TimeStamp,     Process Name ( PID),'
    '   ThreadID, CPU, etw:ActivityId, etw:Related ActivityId, etw:UserSid, etw:SessionId,'
    ' luidSurface {lowpart; highpart}, bindId, PresentCount, fenceValue, bDirectFlip, DXGI_ALPHA_MODE,'
    ' hmonAssociation, bStereoPreferRight, bTemporaryMono, bSwapPool, BufferContentType, bIndependentFlip,'
    ' uPesentDuration, BufferRealizationType, uRealizationIndex, bEnableScanout, hDxSurface',
    'Microsoft-Windows-Dwm-Core/Dx_Flip_Consumed/win:Info,  TimeStamp,     Process Name ( PID),   ThreadID,'
    ' CPU, etw:ActivityId, etw:Related ActivityId, etw:UserSid, etw:SessionId, displayAdapterLuid,'
    ' renderAdapterLuid, vidPnSourceId, vidPnTargetId, uniqueId, surfaceLuid, bindId, presentCount,'
    ' flipRect {left; top; right; bottom}',
    'EndHeader',
]

GAME = '"flipclient.exe" (4242)'
DWM = '"dwm.exe" (900)'
IDLE = '"Idle" (0)'
LUID = 0x623B68E9
# The client's own buffer, as the kernel driver would log it after admitting a scan-out flip.
ADMITTED = 0x271001000
PRESENT_FLAGS_REDIRECTED_FLIP = 0x9000


def detailed(time, process, token, model=2):
    return ('Microsoft-Windows-DxgKrnl/PresentHistoryDetailed/win:Start, %9d, %24s, 100, 0, , , , , 0x1,'
            ' 0x%X, %d, 2200, 0x00000180, 0, 0, 0, 0, 0, 0, 0, 0, 0, , , , , 0, 1920, 0, 1200, 1920, 1200,'
            ' 0, 1920, 0, 1200, 0' % (time, process, token, model))


def present_call(time, process, flags=PRESENT_FLAGS_REDIRECTED_FLIP):
    return ('Microsoft-Windows-DxgKrnl/Present/win:Info, %9d, %24s, 100, 0, , , , , 1073745792,'
            ' 0x00000000000702b8, 0, 0, %d, 0, 0x0000000040001800, 0x0000000000000000'
            % (time, process, flags))


def surface_object(time, process, token, count):
    return ('Microsoft-Windows-Win32k/TokenCompositionSurfaceObject/win:Info, %9d, %24s, 100, 0, , , , ,'
            ' 0x%X, 0xffff1000, 0x00000000, 0x%08X, 0x%016X, 0x0000000000000001, 1, 1920, 1200'
            % (time, process, token, count, LUID))


def state(time, count, independent, skip, new_state):
    return ('Microsoft-Windows-Win32k/TokenStateChanged/win:Info, %9d, %24s, 100, 0, , , , ,'
            ' 0xffff1000, 0x00000000, 0x%08X, 0x0000000000000000, 0x%08X, %s, %s, 0x%016X,'
            ' 0x0000000000000001, false'
            % (time, DWM, count, new_state, 'true' if independent else 'false',
               'true' if skip else 'false', LUID))


def consumed(time, count):
    return ('Microsoft-Windows-Dwm-Core/Dx_Flip_Consumed/win:Info, %9d, %24s, 100, 0, , , , ,'
            ' 0x243c1, 0x243c1, 1, 4352, 3, 0x%016X, 1, %d, [{left : 0; top : 0; right : 1920; bottom : 1200}]'
            % (time, DWM, LUID, count))


def consumed_struct(time, count):
    """The same row with its surfaceLuid expanded, which is how the dumper writes a LUID when the manifest
    gives it as a struct. Reading that cell as one number returns nothing, and a consumption nobody decoded
    looks exactly like a present nobody consumed."""
    return ('Microsoft-Windows-Dwm-Core/Dx_Flip_Consumed/win:Info, %9d, %24s, 100, 0, , , , ,'
            ' 0x243c1, 0x243c1, 1, 4352, 3, [{lowpart : 0x%08X; highpart : 0x00000000}], 1, %d,'
            ' [{left : 0; top : 0; right : 1920; bottom : 1200}]' % (time, DWM, LUID, count))


def consumed_without_luid(time, count):
    """A consumption row whose layout has no surface LUID at all: the key cannot be built from it."""
    return ('Microsoft-Windows-Dwm-Core/Dx_Flip_Consumed/win:Info, %9d, %24s, 100, 0, , , , ,'
            ' 0x243c1, 0x243c1, 1, 4352, 3, 1, %d, [{left : 0; top : 0; right : 1920; bottom : 1200}]'
            % (time, DWM, count))


def header_with(replacement):
    """The header block with the Dx_Flip_Consumed line's LUID column rewritten."""
    return [line.replace(' uniqueId, surfaceLuid, bindId,', replacement) for line in HEADER]


def surface_update(time, count, direct, independent, scanout):
    return ('Microsoft-Windows-Dwm-Core/SCHEDULE_SURFACEUPDATE/win:Info, %9d, %24s, 100, 0, , , , ,'
            ' [{lowpart : 0x%08X; highpart : 0x00000000}], 0x0000000000000001, 0x%016X,'
            ' 0x0000000000000000, %d, 3, 0x0000000000000000, 0, 0, 0, 0, %d, 0, 0, 0, %s,'
            ' 0x0000000000000000'
            % (time, DWM, LUID, count, direct, independent, 'true' if scanout else 'false'))


def vsync_interrupt(time, address):
    return ('Microsoft-Windows-DxgKrnl/VSyncInterrupt/win:Info, %9d, %24s, 100, 0, , , , , 0x1, 0,'
            ' 0x%016X' % (time, IDLE, address))


def vsync_dpc(time, address, sequence, flip_id=0x12):
    """A VSync DPC: the scanned address, and the submit sequence in the high half of FlipFenceId."""
    return ('Microsoft-Windows-DxgKrnl/VSyncDPC/win:Info, %9d, %24s, 100, 0, , , , , 0x1, 0, 0x%016X, 0,'
            ' 0, 0, 0x0, 0, 0x%08X%08X' % (time, IDLE, address, sequence, flip_id))


def packet(time, process, sequence, present=True):
    """A present packet of a process: the only row that gives the client's frame a submit sequence."""
    return ('Microsoft-Windows-DxgKrnl/QueuePacket/win:Start, %9d, %24s, 100, 0, , , , , 0xffffe000,'
            ' 4, %d, 0, 0, 0, %s, 0x0, 0x0, 0' % (time, process, sequence, 'true' if present else 'false'))


def independent_flip(time, sequence, interval=1):
    return ('Microsoft-Windows-DxgKrnl/IndependentFlip/win:Info, %9d, %24s, 100, 0, , , , , %d, %d'
            % (time, IDLE, sequence, interval))


def mmio_flip(time, sequence, flags, address=ADMITTED, allocation=0xFFFFC08F3C7DB6E0):
    return ('Microsoft-Windows-DxgKrnl/MMIOFlip/win:Info, %9d, %24s, 100, 0, , , , , 0x1, 0, %d,'
            ' 0x%016X, 0x%016X, 1, 0, 1, %d' % (time, IDLE, sequence, allocation, address, flags))


def mixed_dump():
    """Four presents of one client, one per classification branch, with the kernel side of the same frames."""
    rows = []
    # Present 1: the compositor hands the buffer to the display pipeline and never consumes it.
    rows += [detailed(1000, GAME, 0xA1), present_call(1010, GAME), surface_object(1100, GAME, 0xA1, 1),
             state(1200, 1, True, False, 3), surface_update(1250, 1, 1, 1, True),
             vsync_interrupt(1300, ADMITTED)]
    # Present 2: DWM consumes it, so it was composed.
    rows += [detailed(17000, GAME, 0xA2), present_call(17010, GAME), surface_object(17100, GAME, 0xA2, 2),
             state(17200, 2, False, False, 3), consumed(17400, 2),
             surface_update(17250, 2, 1, 0, True), vsync_interrupt(17300, 0x271002000)]
    # Present 3: independent first, then taken off the independent path. The last state decides, so this
    # is not a hardware flip - a sticky OR of the flags would have reported it as one.
    rows += [detailed(33000, GAME, 0xA3), present_call(33010, GAME), surface_object(33100, GAME, 0xA3, 3),
             state(33200, 3, True, False, 3), state(33300, 3, True, True, 6),
             surface_update(33250, 3, 1, 0, True), vsync_interrupt(33400, 0x271002000)]
    # Present 4: no token state at all.
    rows += [detailed(49000, GAME, 0xA4), present_call(49010, GAME), surface_object(49100, GAME, 0xA4, 4),
             vsync_interrupt(49200, 0x271002000)]
    # The kernel side of the same four frames, by submit sequence. 7001 is the one frame dxgkrnl turned into
    # an independent flip and programmed with FlipOnNextVSync; 7002 was programmed without being one; 7003
    # is a present packet nothing else names; 7004 belongs to the compositor and must not be counted for the
    # client. 7005 is a non-present packet of the client, which is not a frame.
    rows += [packet(950, GAME, 7001), independent_flip(1280, 7001, 1), mmio_flip(1290, 7001, 4),
             vsync_dpc(1300, ADMITTED, 7001),
             packet(16950, GAME, 7002), mmio_flip(17290, 7002, 2),
             packet(32950, GAME, 7003),
             packet(48950, DWM, 7004), independent_flip(49100, 7004, 1),
             packet(48960, GAME, 7005, present=False)]
    return '\n'.join(HEADER + rows) + '\n'


def composed_dump(consumed_row=consumed, header=None):
    """One composed present of the client, and ANOTHER process flipping its own buffers in the same capture.

    The inert control is about the route of one process. A capture holds the whole desktop, so the second
    half of this dump is what a trace-wide count would read as "our client moved".
    """
    rows = [detailed(1000, GAME, 0xC1), present_call(1010, GAME), surface_object(1100, GAME, 0xC1, 1),
            state(1200, 1, False, False, 3), surface_update(1250, 1, 1, 0, True),
            packet(950, GAME, 9001),
            packet(960, DWM, 9002), independent_flip(1280, 9002, 1), mmio_flip(1290, 9002, 4)]
    if consumed_row is not None:
        rows.append(consumed_row(1400, 1))
    return '\n'.join((header or HEADER) + rows) + '\n'


def true_dump(dpc_address=ADMITTED):
    """The shape increment 2 asks for: three presents, none consumed, the client's own address on the plane.

    dpc_address moves only what the VSync DPC scanned, so a caller can build the case where the display
    interrupt saw the client's buffer and the DPC did not.
    """
    rows = []
    for index in range(3):
        base = 1000 + index * 16000
        count = index + 1
        rows += [detailed(base, GAME, 0xB0 + index), present_call(base + 10, GAME),
                 surface_object(base + 100, GAME, 0xB0 + index, count),
                 state(base + 200, count, True, False, 3),
                 surface_update(base + 250, count, 1, 1, True),
                 packet(base - 50, GAME, 8001 + index),
                 independent_flip(base + 280, 8001 + index, 1),
                 # The second flip carries 0x40, the unconfirmed candidate bit, so the warning line and the
                 # trailing question mark have a case of their own.
                 mmio_flip(base + 290, 8001 + index, 4 | (0x40 if index == 1 else 0)),
                 vsync_dpc(base + 300, dpc_address, 8001 + index),
                 vsync_interrupt(base + 310, ADMITTED)]
    return '\n'.join(HEADER + rows) + '\n'


def skip_dump(consume=None, dpc_address=ADMITTED, independent=True):
    """Trial 478's shape: Win32k marks every InFrame token IndependentFlip true with SkipIndependentFlip true,
    while the kernel independent-flips and programs the client's own packets.

    consume names a present count DWM consumed; dpc_address moves what the VSync DPC scanned; independent
    False leaves out the IndependentFlip events. Each is one negative control of the kernel witness.
    """
    rows = []
    for index in range(3):
        base = 1000 + index * 10000
        count = index + 1
        rows += [detailed(base, GAME, 0xD0 + index), present_call(base + 10, GAME),
                 surface_object(base + 100, GAME, 0xD0 + index, count),
                 state(base + 200, count, True, True, 2), state(base + 210, count, True, True, 3),
                 packet(base - 50, GAME, 6001 + index)]
        if independent:
            rows.append(independent_flip(base + 280, 6001 + index, 0))
        rows += [mmio_flip(base + 290, 6001 + index, 0x2, address=ADMITTED + index * 0x1000),
                 vsync_dpc(base + 300, dpc_address + index * 0x1000, 0, 0)]
        if consume == count:
            rows.append(consumed(base + 400, count))
    return '\n'.join(HEADER + rows) + '\n'


def load_module():
    spec = importlib.util.spec_from_file_location('etw_present_mode', PARSER)
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


class Checker:
    def __init__(self):
        self.ok = 0
        self.failed = 0

    def check(self, what, condition, detail=''):
        if condition:
            self.ok += 1
            print('ok   %s' % what)
        else:
            self.failed += 1
            print('FAIL %s%s' % (what, (': %s' % detail) if detail else ''))

    def present(self, what, pattern, text):
        self.check(what, re.search(pattern, text) is not None, '/%s/ not in the output' % pattern)

    def absent(self, what, pattern, text):
        self.check(what, re.search(pattern, text) is None, '/%s/ is in the output and must not be' % pattern)


def run(*args):
    done = subprocess.run([sys.executable, str(PARSER)] + [str(a) for a in args],
                          capture_output=True, text=True, check=False)
    return done


def unit_cases(check, module):
    """The pure decoders, called directly: a wrong bit name is a wrong conclusion in a report."""
    check.check('the three confirmed MMIOFlip bits are named',
                module.mmio_flag_text(0x7) == 'ModeChange+FlipImmediate+FlipOnNextVSync',
                module.mmio_flag_text(0x7))
    check.check('the candidate bits carry a question mark',
                module.mmio_flag_text(0x44) == 'FlipOnNextVSync+IndependentFlipExclusive?',
                module.mmio_flag_text(0x44))
    check.check('0x20 is the candidate SharedPrimaryTransition, not FlipRestart',
                module.mmio_flag_text(0x20) == 'SharedPrimaryTransition?', module.mmio_flag_text(0x20))
    check.check('an unmapped bit stays raw', module.mmio_flag_text(0x100) == 'unknown 0x100',
                module.mmio_flag_text(0x100))
    check.check('a missing Flags field is not read as zero',
                module.mmio_flag_text(None) == 'no Flags field', module.mmio_flag_text(None))
    check.check('0x9000 is RedirectedFlip with PresentCountValid',
                module.present_flag_text(0x9000) == 'PresentCountValid+RedirectedFlip',
                module.present_flag_text(0x9000))
    check.check("the compositor's own 0x401004 is a plain Flip",
                module.present_flag_text(0x401004) == 'Flip+PresentCountValid+PresentRegionsValid',
                module.present_flag_text(0x401004))
    check.check('a present flag the WDK header does not declare stays raw',
                module.present_flag_text(0x40000000) == 'unknown 0x40000000',
                module.present_flag_text(0x40000000))
    check.check('a multi-plane submit sequence comes from the high half',
                module.submit_sequence('MMIOFlipMultiPlaneOverlay', '0x0000271100000000') == 0x2711)
    check.check('a plain submit sequence does not',
                module.submit_sequence('MMIOFlip', '7001') == 7001)
    check.check('"true" is True and not an int() crash', module.flag('true') is True)
    check.check('an absent flag is None, not False', module.flag(None) is None)
    check.check('counters parse from one comma list',
                module.parse_counters(['scanout_flips=3,admit_ok=3']) == {'scanout_flips': 3, 'admit_ok': 3})
    check.check('the Win32k InFrame state is 3', module.TOKEN_STATE[3] == 'InFrame')
    check.check('REDIRECTED_FLIP is model 2', module.PRESENT_MODEL[2] == 'REDIRECTED_FLIP')


def kept_trace_cases(check):
    """The composed baseline: increment 1 must read INERT and increment 2 must read REFUTED."""
    if not KEPT.is_file():
        check.check('the kept trace excerpt is present', False, '%s is missing' % KEPT)
        return
    done = run(KEPT, '4476')
    check.check('the parser runs over the kept excerpt', done.returncode == 0, done.stderr[-400:])
    text = done.stdout
    check.present('the client is found by its pid', r'"Unknown" \(4476\): 60 presents', text)
    check.present('its rate is its own window', r'60 presents over its own 0\.984 s, 61\.0/s', text)
    check.present('every present call is the composed 0x9000 shape',
                  r'Present Flags 0x9000 \(PresentCountValid\+RedirectedFlip\)\s+60', text)
    check.present('PresentHistoryDetailed says REDIRECTED_FLIP',
                  r'Model 2 \(REDIRECTED_FLIP\)\s+60\s+\(60 from PresentHistoryDetailed\)', text)
    check.present('the InFrame token states are reported with both flags',
                  r'TokenStateChanged InFrame independent False skip False\s+58', text)
    check.present('and the InFrame promotion counts, which increment 3 reads',
                  r'InFrame rows 58, with IndependentFlip 0, with SkipIndependentFlip 0', text)
    check.present('DWM answers DirectFlip and composes anyway',
                  r'DWM says: direct flip True, independent False, scanout True\s+58', text)
    check.present('the compositor consumed nearly every frame',
                  r'composed flip \(DWM consumed\)\s+58', text)
    check.present('the two presents at the excerpt edges are unclassified, not guessed',
                  r'unclassified\s+2\s', text)
    check.present('no frame of the client reached the plane',
                  r'\(4476\): 60 present packets \(60 rows\), 0 independent flips, 0 reached MMIOFlip, '
                  r'0 named by a VSync DPC', text)
    check.present('the composed verdict',
                  r'M15\.14 VERDICT "Unknown" \(4476\): packets=60 independent=0 mmio=0 vsync=0 '
                  r'directflip=58/58 result=COMPOSED', text)
    check.present('increment 1 reads INERT on the composed baseline',
                  r'M15\.14 INCREMENT1 "Unknown" \(4476\): present_flags_9000=PASS detailed_model_2=PASS '
                  r'dwm_directflip_1_independent_0=PASS no_independent_flip=PASS '
                  r'no_plane_for_this_process=PASS result=INERT', text)
    check.present('increment 2 is refuted by the consumption',
                  r'M15\.14 INCREMENT2 "Unknown" \(4476\): not_consumed_by_dwm=FAIL .*result=REFUTED', text)
    check.present('the kernel witness does not hold on the composed baseline',
                  r'M15\.14 KERNEL-WITNESS "Unknown" \(4476\): independent_flips=FAIL .*not_consumed_by_dwm=FAIL '
                  r'result=NOT-HELD relabelled=0', text)
    check.present('and names the count that refutes it',
                  r'not_consumed_by_dwm\s+FAIL\s+58 Windowed_Dx_Flip_Consumed and 0 Dx_Flip_Consumed', text)
    check.present('a clause with no input says so instead of passing',
                  r'kernel_counters\s+NO-DATA\s+need scanout_flips', text)
    check.present('the display side names three compositor buffers',
                  r'VSyncDPC ScannedPhysicalAddress: 60 samples, 3 distinct, 59 changes', text)
    check.present('the reading of a changing address is qualified',
                  r'a changing address is what a composing desktop does too', text)
    check.absent('the unconfirmed bits never appear on the baseline', r'WARNING: an MMIOFlip Flags word', text)
    check.absent('no header layout is missing from the excerpt',
                 r'rows with no usable header layout', text)

    # The compositor's own frames are the ones on the plane: its FlipToPhysicalAddress and the scanned address
    # are the same number, which is what clauses 2 and 3 of increment 2 test for. Here that pair is the
    # compositor's primary and the verdict must still not be CONFIRMED, because clause 1 has no data for it.
    done = run(KEPT, '1956', '--admitted-address', '0x000000F403AD0000',
               '--kmd-counters', 'scanout_flips=20,scanout_requests=20,admit_alignment=0,admit_segment=0')
    check.check('the parser runs with an admitted address', done.returncode == 0, done.stderr[-400:])
    text = done.stdout
    check.present('a programmed address that was admitted is marked',
                  r'FlipToPhysicalAddress 0x000000F403AD0000\s+20  ADMITTED', text)
    check.present('clause 2 finds it', r'mmio_programmed_an_admitted_address\s+PASS\s+programmed '
                                       r'0xF403AD0000', text)
    check.present('clause 3 finds the same address scanned',
                  r'vsync_scanned_that_address\s+PASS\s+scanned 0xF403AD0000', text)
    check.present('clause 4 passes on the supplied counters',
                  r'kernel_counters\s+PASS\s+scanout_flips 20, scanout_requests 20, 2 refusal columns all '
                  r'zero', text)
    check.present('three clauses passing is still not CONFIRMED while clause 1 has no data',
                  r'M15\.14 INCREMENT2 "Unknown" \(1956\): not_consumed_by_dwm=NO-DATA .*result=UNKNOWN', text)
    check.present("the compositor's own control reads MOVED, as the docstring says it must",
                  r'M15\.14 INCREMENT1 "Unknown" \(1956\): .*result=MOVED', text)

    done = run(KEPT, '4476', '--kmd-counters', 'scanout_flips=20,scanout_requests=20,admit_alignment=4')
    check.present('a non-zero refusal column fails clause 4',
                  r"kernel_counters\s+FAIL\s+scanout_flips 20, scanout_requests 20, refusals "
                  r"\{'admit_alignment': 4\}", done.stdout)

    done = run(KEPT, 'notaprocess')
    check.check('an absent process does not fail the run', done.returncode == 0, done.stderr[-400:])
    check.present('an absent process gets a verdict of zero, not silence',
                  r'M15\.14 VERDICT notaprocess: packets=0 independent=0 mmio=0 vsync=0 directflip=0/0 '
                  r'result=NO-DATA', done.stdout)
    check.present('and a NO-DATA control rather than a pass',
                  r'M15\.14 INCREMENT1 notaprocess: .*result=NO-DATA', done.stdout)


def witcher_cases(check):
    """Trial 478, one second of each shape: the kernel witness HOLDS for the flipping game and relabels the
    presents Win32k marked skipped; it does not hold for the composed one, and nothing moves there."""
    for fixture in (W3_FLIP, W3_COMPOSED):
        if not fixture.is_file():
            check.check('the fixture %s is present' % fixture.name, False, '%s is missing' % fixture)
            return
    done = run(W3_FLIP, '6812')
    check.check('the parser runs over the W3 flip excerpt', done.returncode == 0, done.stderr[-400:])
    text = done.stdout
    check.present('Win32k marked every InFrame token skipped',
                  r'InFrame rows 102, with IndependentFlip 102, with SkipIndependentFlip 102', text)
    check.present('the kernel witness holds, with all four clauses',
                  r'M15\.14 KERNEL-WITNESS "Unknown" \(6812\): independent_flips=PASS mmio_programmed=PASS '
                  r'vsync_scanned_same_addresses=PASS not_consumed_by_dwm=PASS result=HOLDS relabelled=102', text)
    check.present('the DPC scanned the three addresses the game programmed',
                  r'vsync_scanned_same_addresses\s+PASS\s+all 3 scanned: 0xF434664000, 0xF43B930000, '
                  r'0xF43C1FA000', text)
    check.present('those presents are hardware flips by the witness',
                  r'hardware flip \(kernel witness; Win32k skip\)\s+102\s+98\.1%', text)
    check.absent('and no longer composed by the Win32k label', r'composed flip \(independent skipped\)', text)
    check.present('the timeline follows the relabel', r'0  hardware flip \(kernel witness; Win32k skip\) 102', text)
    check.present('the two presents at the edges stay unclassified', r'unclassified\s+2\s', text)
    check.present('the kernel count is printed beside it, not joined',
                  r'\(6812\): 104 present packets \(104 rows\), 102 independent flips, 102 reached MMIOFlip', text)
    check.present('the verdict line is unchanged by the witness',
                  r'M15\.14 VERDICT "Unknown" \(6812\): packets=104 independent=102 mmio=102 vsync=0 '
                  r'directflip=0/0 result=INDEPENDENT-FLIP', text)

    done = run(W3_COMPOSED, '6812')
    check.check('the parser runs over the W3 composed excerpt', done.returncode == 0, done.stderr[-400:])
    text = done.stdout
    check.present('the composed game: DWM consumed its frames', r'composed flip \(DWM consumed\)\s+58\s', text)
    check.present('the witness does not hold there',
                  r'M15\.14 KERNEL-WITNESS "Unknown" \(6812\): independent_flips=FAIL mmio_programmed=NO-DATA '
                  r'vsync_scanned_same_addresses=NO-DATA not_consumed_by_dwm=FAIL result=NOT-HELD relabelled=0', text)
    check.absent('and nothing is relabelled', r'kernel witness; Win32k skip', text)
    check.present('the composed verdict stands',
                  r'M15\.14 VERDICT "Unknown" \(6812\): packets=95 independent=0 mmio=0 vsync=0 '
                  r'directflip=0/58 result=COMPOSED', text)


def synthetic_cases(check):
    """The branches no capture of this driver has taken yet."""
    with tempfile.TemporaryDirectory() as directory:
        mixed = Path(directory) / 'mixed.txt'
        mixed.write_text(mixed_dump(), encoding='utf-8')
        done = run(mixed, 'flipclient')
        check.check('the parser runs over the mixed synthetic dump', done.returncode == 0, done.stderr[-400:])
        text = done.stdout
        check.present('one hardware flip', r'hardware flip \(independent\)\s+1\b', text)
        check.present('one composed flip', r'composed flip \(DWM consumed\)\s+1\b', text)
        check.present('the withdrawn candidate is composed, not hardware',
                      r'composed flip \(independent skipped\)\s+1\b', text)
        check.present('the present with no state is unclassified', r'unclassified\s+1\b', text)
        check.present('its reason is named', r"no TokenStateChanged': 1", text)
        check.present('a present DWM said nothing about is named so',
                      r'DWM says: no SCHEDULE_SURFACEUPDATE\s+1', text)
        check.present("the client's own present packets are counted, and nobody else's",
                      r'flipclient\.exe" \(4242\): 3 present packets \(3 rows\), 1 independent flips, '
                      r'2 reached MMIOFlip, 1 named by a VSync DPC', text)
        check.present('the flip interval is reported', r'flip intervals: \{1: 1\}', text)
        check.present('the documented MMIOFlip flags are named', r'MMIOFlip FlipOnNextVSync\s+1', text)
        check.present('and the other one too', r'MMIOFlip FlipImmediate\s+1', text)
        check.present('the verdict line names the independent flip',
                      r'M15\.14 VERDICT "flipclient\.exe" \(4242\): packets=3 independent=1 mmio=2 vsync=1 '
                      r'directflip=3/3 result=INDEPENDENT-FLIP', text)
        check.present('increment 1 reports MOVED once a frame flipped',
                      r'M15\.14 INCREMENT1 "flipclient\.exe" \(4242\): .*no_independent_flip=FAIL '
                      r'no_plane_for_this_process=FAIL result=MOVED', text)
        check.present('the independent-flip events are summarised',
                      r'independent-flip events: 2, present packets with one: 2', text)
        check.present('one consumed present keeps the kernel witness from holding',
                      r'M15\.14 KERNEL-WITNESS "flipclient\.exe" \(4242\): .*not_consumed_by_dwm=FAIL '
                      r'result=NOT-HELD relabelled=0', text)

        # The kernel witness (increment 3) and its negative controls, on trial 478's shape.
        skip = Path(directory) / 'skip.txt'
        skip.write_text(skip_dump(), encoding='utf-8')
        done = run(skip, 'flipclient')
        check.check('the parser runs over the skip dump', done.returncode == 0, done.stderr[-400:])
        check.present('the witness holds on the skip shape',
                      r'M15\.14 KERNEL-WITNESS "flipclient\.exe" \(4242\): independent_flips=PASS '
                      r'mmio_programmed=PASS vsync_scanned_same_addresses=PASS not_consumed_by_dwm=PASS '
                      r'result=HOLDS relabelled=3', done.stdout)
        check.present('and all three presents are hardware flips by it',
                      r'hardware flip \(kernel witness; Win32k skip\)\s+3\s+100\.0%', done.stdout)
        check.present('the Win32k label it overrides is printed beside it',
                      r'InFrame rows 3, with IndependentFlip 3, with SkipIndependentFlip 3', done.stdout)
        consumed_one = Path(directory) / 'skip-consumed.txt'
        consumed_one.write_text(skip_dump(consume=2), encoding='utf-8')
        done = run(consumed_one, 'flipclient')
        check.present('one consumed present: NOT-HELD',
                      r'not_consumed_by_dwm=FAIL result=NOT-HELD relabelled=0', done.stdout)
        check.present('the consumed present is composed',
                      r'composed flip \(DWM consumed\)\s+1\b', done.stdout)
        check.present('and the other two keep the Win32k label, because the witness is per process',
                      r'composed flip \(independent skipped\)\s+2\b', done.stdout)
        elsewhere = Path(directory) / 'skip-dpc-elsewhere.txt'
        elsewhere.write_text(skip_dump(dpc_address=0x3FF000000), encoding='utf-8')
        done = run(elsewhere, 'flipclient')
        check.present('a DPC that scanned other addresses: NOT-HELD, and it names them',
                      r'vsync_scanned_same_addresses\s+FAIL\s+3 of 3 addresses never scanned: 0x271001000, '
                      r'0x271002000, 0x271003000', done.stdout)
        check.present('so nothing is relabelled', r'composed flip \(independent skipped\)\s+3\b', done.stdout)
        no_flip = Path(directory) / 'skip-no-independent.txt'
        no_flip.write_text(skip_dump(independent=False), encoding='utf-8')
        done = run(no_flip, 'flipclient')
        check.present('no IndependentFlip event: NOT-HELD',
                      r'independent_flips=FAIL mmio_programmed=NO-DATA vsync_scanned_same_addresses=NO-DATA '
                      r'not_consumed_by_dwm=PASS result=NOT-HELD relabelled=0', done.stdout)
        check.absent('and no relabel', r'kernel witness; Win32k skip\)\s+\d', done.stdout)

        true = Path(directory) / 'true.txt'
        true.write_text(true_dump(), encoding='utf-8')
        done = run(true, 'flipclient', '--admitted-address', '0x271001000',
                   '--kmd-counters', 'scanout_flips=3,scanout_requests=3',
                   '--kmd-counters', 'admit_alignment=0,admit_segment=0,admit_pitch=0')
        check.check('the parser runs over the TRUE dump', done.returncode == 0, done.stderr[-400:])
        text = done.stdout
        check.present('every present is a hardware flip', r'hardware flip \(independent\)\s+3\s+100\.0%', text)
        check.present('all four clauses of increment 2 pass',
                      r'M15\.14 INCREMENT2 "flipclient\.exe" \(4242\): not_consumed_by_dwm=PASS '
                      r'mmio_programmed_an_admitted_address=PASS vsync_scanned_that_address=PASS '
                      r'kernel_counters=PASS result=CONFIRMED', text)
        check.present('the admitted address is named in clause 2',
                      r'mmio_programmed_an_admitted_address\s+PASS\s+programmed 0x271001000', text)
        check.present('the unconfirmed candidate bit is decoded with a question mark',
                      r'MMIOFlip FlipOnNextVSync\+IndependentFlipExclusive\?\s+1', text)
        check.present('and a run that sees one is warned about the mapping',
                      r'WARNING: an MMIOFlip Flags word carried 0x20, 0x40 or 0x80\. '
                      r'the 0x20/0x40/0x80 names come from DXGK_SETVIDPNSOURCEADDRESS_FLAGS and are '
                      r'UNCONFIRMED', text)
        check.present('the verdict is an independent flip',
                      r'M15\.14 VERDICT "flipclient\.exe" \(4242\): packets=3 independent=3 mmio=3 vsync=3 '
                      r'directflip=3/3 result=INDEPENDENT-FLIP', text)
        check.present('every InFrame token carries the promotion',
                      r'InFrame rows 3, with IndependentFlip 3, with SkipIndependentFlip 0', text)

        # Without the kernel driver's numbers the same trace cannot be CONFIRMED. A conjunction that passes on
        # three clauses out of four is the error this verdict exists to prevent.
        done = run(true, 'flipclient')
        check.present('the same trace is UNKNOWN without an admitted address and counters',
                      r'M15\.14 INCREMENT2 "flipclient\.exe" \(4242\): not_consumed_by_dwm=PASS '
                      r'mmio_programmed_an_admitted_address=NO-DATA vsync_scanned_that_address=NO-DATA '
                      r'kernel_counters=NO-DATA result=UNKNOWN', done.stdout)

        # The VSync DPC is the witness of clause 3, and the display interrupt is not. A capture where the
        # interrupt saw the client's address and the DPC saw the compositor's must not pass clause 3.
        other = Path(directory) / 'dpc-elsewhere.txt'
        other.write_text(true_dump(dpc_address=0x3FF000000), encoding='utf-8')
        done = run(other, 'flipclient', '--admitted-address', '0x271001000',
                   '--kmd-counters', 'scanout_flips=3,scanout_requests=3,admit_alignment=0')
        check.present('the display interrupt alone cannot satisfy clause 3',
                      r'vsync_scanned_that_address\s+FAIL\s+no VSync DPC scanned 0x271001000', done.stdout)
        check.present('and that refutes the conjunction',
                      r'M15\.14 INCREMENT2 "flipclient\.exe" \(4242\): .*result=REFUTED', done.stdout)

        # A trace whose header block lost a line must say so rather than drop the rows in silence.
        broken = Path(directory) / 'broken.txt'
        broken.write_text('\n'.join(line for line in true_dump().splitlines()
                                    if not line.startswith('Microsoft-Windows-DxgKrnl/MMIOFlip/win:Info,'
                                                           '  TimeStamp')) + '\n', encoding='utf-8')
        done = run(broken, 'flipclient')
        check.present('a missing header line is reported, not ignored',
                      r"rows with no usable header layout: \{'Microsoft-Windows-DxgKrnl/MMIOFlip/win:Info': 3\}",
                      done.stdout)

        # The inert control is per process. Another process's independent flip is a warning on the line,
        # not a verdict about our client: a trace-wide count would send the lead rolling back a route that
        # never moved.
        composed = Path(directory) / 'composed.txt'
        composed.write_text(composed_dump(), encoding='utf-8')
        done = run(composed, 'flipclient')
        check.check('the parser runs over the composed dump', done.returncode == 0, done.stderr[-400:])
        check.present("another process's flip does not fail this client's control",
                      r'no_independent_flip\s+PASS\s+0 naming this process, 1 elsewhere in the trace '
                      r'\(another process flipping its own buffers is not this verdict', done.stdout)
        check.present('so the client reads INERT',
                      r'M15\.14 INCREMENT1 "flipclient\.exe" \(4242\): .*no_independent_flip=PASS '
                      r'no_plane_for_this_process=PASS result=INERT', done.stdout)
        done = run(composed, 'dwm')
        check.present('the compositor carries the caveat instead of a finding',
                      r'M15\.14 INCREMENT1 "dwm\.exe" \(900\): .*result=MOVED {2}CAVEAT: the compositor '
                      r'itself',
                      done.stdout)

        # A struct-shaped surfaceLuid is the shape that broke: read as one number it comes back empty, and
        # a consumption nobody decoded is indistinguishable from a present nobody consumed.
        struct = Path(directory) / 'consumed-struct.txt'
        struct.write_text(composed_dump(consumed_row=consumed_struct,
                                        header=header_with(' uniqueId, surfaceLuid {lowpart; highpart},'
                                                           ' bindId,')), encoding='utf-8')
        done = run(struct, 'flipclient')
        check.check('the parser runs over the struct-LUID dump', done.returncode == 0, done.stderr[-400:])
        check.present('an expanded surfaceLuid still names the present it consumed',
                      r'composed flip \(DWM consumed\)\s+1\b', done.stdout)
        check.present('and the consumption refuses increment 2',
                      r'not_consumed_by_dwm\s+FAIL\s+0 Windowed_Dx_Flip_Consumed and 1 Dx_Flip_Consumed',
                      done.stdout)

        # And a layout with no surface LUID at all cannot be read as "nothing was consumed".
        undecoded = Path(directory) / 'consumed-undecoded.txt'
        undecoded.write_text(composed_dump(consumed_row=consumed_without_luid,
                                           header=header_with(' uniqueId, bindId,')), encoding='utf-8')
        done = run(undecoded, 'flipclient')
        check.check('the parser runs over the undecodable dump', done.returncode == 0, done.stderr[-400:])
        check.present('a consumption row that did not decode is not an absence of consumption',
                      r"not_consumed_by_dwm\s+FAIL\s+.*1 consumption rows did not decode their surface key "
                      r"\(\{'Dx_Flip_Consumed': 1\}\), so this absence is not evidence", done.stdout)

        # The command line must refuse what it cannot do instead of doing something else.
        done = run(true, '--dump-out', str(Path(directory) / 'x.txt'))
        check.check('--dump-out is refused for a text argument', done.returncode != 0, done.stdout)
        done = run(Path(directory) / 'nothing.txt')
        check.check('a missing file is an error', done.returncode != 0, done.stdout)
        done = run(true, '--admitted-address', 'notanumber')
        check.check('a bad admitted address is an error', done.returncode != 0, done.stdout)
        done = run(true, '--kmd-counters', 'scanout_flips')
        check.check('a counter without a value is an error', done.returncode != 0, done.stdout)


def etl_case(check):
    """The xperf path and the dump deletion, when an .etl is offered through the environment."""
    etl = os.environ.get('BC250_ETW_TEST_ETL')
    if not etl:
        print('skip the .etl case: set BC250_ETW_TEST_ETL to a present-mode capture to run it')
        return
    source = Path(etl)
    if not source.is_file():
        check.check('BC250_ETW_TEST_ETL names a file', False, '%s is missing' % source)
        return
    with tempfile.TemporaryDirectory() as directory:
        dump = Path(directory) / 'from-etl.txt'
        done = run(source, '--dump-out', dump)
        check.check('the .etl runs through the xperf dumper', done.returncode == 0, done.stderr[-400:])
        check.present('the dumper step is announced', r'xperf dumper: ', done.stdout)
        check.present('the text dump is deleted again', r'text dump deleted \([0-9.]+ MB\)', done.stdout)
        check.check('and is really gone', not dump.exists(), '%s still exists' % dump)
        done = run(source, '--dump-out', dump, '--keep-dump')
        check.present('--keep-dump keeps it', r'text dump kept at ', done.stdout)
        check.check('and it is on disk', dump.is_file())


def main():
    check = Checker()
    unit_cases(check, load_module())
    kept_trace_cases(check)
    witcher_cases(check)
    synthetic_cases(check)
    etl_case(check)
    print('etw-present-mode self-test: %d ok, %d failure(s)' % (check.ok, check.failed))
    return 1 if check.failed else 0


if __name__ == '__main__':
    raise SystemExit(main())
