"""Layouts the telemetry line depends on, checked by the normal monitor build (python -m unittest discover).

BC250_ESCAPE_DPM exists three times: in the KMD's escape header, in the control DLL (a guarded copy while this
tree's driver/kmd/bc250kmd_escape.h predates the DPM escape) and in the monitor (Driver.cs DpmSnapshot).
BC250_VIDEO_MEMORY is the control DLL's own structure, mirrored by Driver.cs VideoMemorySnapshot. A layout that
drifts would not fail loudly: P/Invoke would read the wrong words, so the overlay would show wrong numbers.

The DPM reference is this tree's header once it defines the escape, otherwise the header of KMD 0.7.177.1
(commit REFERENCE_COMMIT, branch kmd177-dpm-busy); when neither is available the DPM checks are skipped, not
passed.
"""
from pathlib import Path
import re
import subprocess
import unittest

HERE = Path(__file__).resolve().parent
REPO = HERE.parents[2]
HEADER = REPO / 'driver/kmd/bc250kmd_escape.h'
CLI = REPO / 'tools/win/bc250kmd_cli/bc250kmd_cli.c'
DRIVER_CS = HERE / 'src/Driver.cs'
REFERENCE_COMMIT = '8fd4bcadf21ea463618fcfef688a31220a09058a'   # KMD 0.7.177.1: DPM busy from GRBM_STATUS

C_TYPES = [('unsigned long long', 'u64'), ('unsigned long', 'u32'), ('long', 'i32'),
           ('ULONGLONG', 'u64'), ('ULONG', 'u32'), ('LONG', 'i32')]
CS_TYPES = {'ulong': 'u64', 'uint': 'u32', 'int': 'i32'}


def c_fields(source, struct):
    """[(kind, name, count)] of `typedef struct _<struct> { ... } <struct>;`, comments removed."""
    body = re.search(r'typedef struct _%s \{(.*?)\} %s;' % (struct, struct), source, re.S)
    assert body, struct + ' not found'
    text = re.sub(r'//[^\n]*', '', body.group(1))
    defines = {n: int(v) for n, v in re.findall(r'#define\s+(\w+)\s+(\d+)u?\b', source)}
    fields = []
    for statement in filter(None, (s.strip() for s in text.split(';'))):
        kind = next(k for prefix, k in C_TYPES if re.match(re.escape(prefix) + r'\s', statement))
        names = statement[len(next(p for p, k in C_TYPES if statement.startswith(p) and k == kind)):]
        for name in names.split(','):
            name = name.strip()
            array = re.fullmatch(r'(\w+)\[(\w+)\]', name)
            if array:
                count = array.group(2)
                fields.append((kind, array.group(1), int(count) if count.isdigit() else defines[count]))
            else:
                fields.append((kind, name, 1))
    return fields


def cs_fields(struct):
    body = re.search(r'public struct %s\s*\{(.*?)\n    \}' % struct, DRIVER_CS.read_text(), re.S)
    assert body, struct + ' not found in Driver.cs'
    fields = []
    for size, kind, array, names in re.findall(
            r'(?:\[MarshalAs\(UnmanagedType\.ByValArray, SizeConst = (\d+)\)\]\s*)?public (uint|int|ulong)(\[\])?\s+([^;=]+);',
            body.group(1)):
        for name in names.split(','):
            fields.append((CS_TYPES[kind], name.strip(), int(size) if array else 1))
    return fields


def size_of(fields):
    offset = 0
    for kind, _, count in fields:
        width = 8 if kind == 'u64' else 4
        offset = (offset + width - 1) // width * width + width * count
    return (offset + 7) // 8 * 8


def reference_header():
    text = HEADER.read_text()
    if 'typedef struct _BC250_ESCAPE_DPM' in text:
        return text
    try:
        return subprocess.run(['git', '-C', str(REPO), 'show', REFERENCE_COMMIT + ':driver/kmd/bc250kmd_escape.h'],
                              capture_output=True, text=True, check=True).stdout
    except (OSError, subprocess.CalledProcessError):
        return None


class TelemetryAbiTest(unittest.TestCase):
    def setUp(self):
        self.reference = reference_header()

    def need_reference(self):
        if self.reference is None:
            self.skipTest('no header defines BC250_ESCAPE_DPM (this tree nor ' + REFERENCE_COMMIT[:8] + ')')

    def test_monitor_dpm_layout(self):
        self.need_reference()
        expected = c_fields(self.reference, 'BC250_ESCAPE_DPM')
        self.assertEqual(expected, cs_fields('DpmSnapshot'))
        self.assertEqual(size_of(expected), 192)      # ABI 2 (0.7.207); the ABI 1 prefix is 160 bytes

    def test_control_dll_dpm_copy(self):
        self.need_reference()
        source = CLI.read_text()
        guarded = re.search(r'#ifndef BC250_ESCAPE_RUN_DPM\n(.*?)#endif', source, re.S)
        if guarded is None:
            self.assertIn('typedef struct _BC250_ESCAPE_DPM', HEADER.read_text(), 'copy removed before the header has it')
            return
        copy = guarded.group(1)
        self.assertEqual(c_fields(self.reference, 'BC250_ESCAPE_DPM'), c_fields(copy, 'BC250_ESCAPE_DPM'))
        for name, value in re.findall(r'#define (BC250_\w+) (\d+)u', copy):
            found = re.search(r'#define %s\s+(\d+)u' % name, self.reference)
            self.assertIsNotNone(found, name)
            self.assertEqual(int(found.group(1)), int(value), name)

    def test_protocol_constants(self):
        self.need_reference()
        for name, value in {'BC250_ESCAPE_RUN_DPM': 23, 'BC250_DPM_ABI': 2, 'BC250_DPM_ABI_1': 1,
                            'BC250_DPM_ABI1_SIZE': 160, 'BC250_DPM_OP_READ': 0,
                            'BC250_DPM_FLAG_RUNNING': 1, 'BC250_DPM_FLAG_TEMPERATURE': 128,
                            'BC250_DPM_FLAG_CLOCK': 256, 'BC250_DPM_FLAG_HW_BUSY': 512,
                            'BC250_DPM_FLAG_IDLE': 1024}.items():
            found = re.search(r'#define %s\s+(\d+)u' % name, self.reference)
            self.assertIsNotNone(found, name)
            self.assertEqual(int(found.group(1)), value, name)
        managed = DRIVER_CS.read_text() + (HERE / 'src/TelemetryProvider.cs').read_text()
        for name, value in {'FlagTemperature': 128, 'FlagClock': 256, 'FlagHwBusy': 512, 'FlagIdle': 1024,
                            'FlagRunning': 1}.items():
            self.assertRegex(managed, r'\b%s = %d\b' % (name, value))

    def test_video_memory_layout(self):
        fields = c_fields(CLI.read_text(), 'BC250_VIDEO_MEMORY')
        self.assertEqual(fields, cs_fields('VideoMemorySnapshot'))
        self.assertEqual(size_of(fields), 264)

    def test_sizes_passed_by_the_monitor(self):
        source = DRIVER_CS.read_text()
        self.assertIn('Bc250Dpm(out data, 192)', source)         # ABI 2 first
        self.assertIn('Bc250Dpm(out data, 160)', source)         # the ABI 1 prefix when a caller refuses 192
        self.assertIn('Bc250VideoMemory(null, out data, 264)', source)


class MonTelemetryTest(unittest.TestCase):
    """mon.py telemetry's line: what scripts parse."""
    def test_line(self):
        import mon
        line = mon.format_telemetry({'available': True, 'temperatureC': 67.5, 'loadPercent': None, 'gfxMHz': 1000,
                                     'vramUsedMB': 1234, 'vramTotalMB': 2048, 'ageSeconds': 0.4,
                                     'note': 'load n/a: KMD 0x000700B0 has no GRBM busy share (0.7.177)'})
        self.assertEqual(line, 'tctl_c=67.5 load_pct=n/a gfx_mhz=1000 vram_used_mb=1234 vram_total_mb=2048 age_s=0.4\n'
                               '# load n/a: KMD 0x000700B0 has no GRBM busy share (0.7.177)')
        self.assertEqual(mon.format_telemetry({'available': False}), 'telemetry=n/a')
        pairs = dict(p.split('=') for p in mon.format_telemetry({'available': True, 'loadPercent': 12.3}).split())
        self.assertEqual(pairs['load_pct'], '12.3')
        self.assertEqual(pairs['tctl_c'], 'n/a')


if __name__ == '__main__':
    unittest.main()
