"""Layouts, constants and names the operating-point and measurement-guard panels depend on.

The overlay repeats parts of the driver: the CU mode escape structure, the DPM and start-health flag bits, the
interop switch and end codes, the reason and throttle names, and the registry value names it reads. None of
those would fail loudly on a drift - P/Invoke would read the wrong words and the panel would show a wrong
number with full confidence, which is exactly the failure that cost the lab three days of 24 CU measurements.
So the normal monitor build parses both sides and refuses to build on a difference.

    python -m unittest discover -s tools/win/bc250mon      # build.ps1 runs this
"""
from pathlib import Path
import json
import re
import unittest

from test_telemetry import c_fields, cs_fields, size_of

HERE = Path(__file__).resolve().parent
REPO = HERE.parents[2]
ESCAPE_HEADER = REPO / 'driver/kmd/bc250kmd_escape.h'
CU_HEADER = REPO / 'driver/shim/include/bc250_cu_mode.h'
DPM_HEADER = REPO / 'driver/shim/include/bc250_dpm.h'
CUMODE_C = REPO / 'driver/kmd/cumode.c'
INTEROP_C = REPO / 'driver/kmd/interop.c'
INTEROP_POLICY_C = REPO / 'driver/kmd/interop_policy.c'
DPM_C = REPO / 'driver/kmd/dpm.c'
INTEROP_POLICY_H = REPO / 'driver/kmd/interop_policy.h'
CLI = REPO / 'tools/win/bc250kmd_cli/bc250kmd_cli.c'
WITNESS_PS1 = REPO / 'tools/release/installer/release-witness.ps1'
REGISTRY_DEFAULTS = REPO / 'tools/release/installer/registry-defaults.json'
DRIVER_CS = HERE / 'src/Driver.cs'
NAMES_CS = HERE / 'src/LabState.cs'
POINT_CS = HERE / 'src/OperatingPointProvider.cs'
GUARD_CS = HERE / 'src/MeasurementGuardProvider.cs'


def text(path):
    return path.read_text(encoding='utf-8')


def define(path, name):
    """A #define of a plain decimal number, with or without a u/ull suffix."""
    found = re.search(r'#define\s+%s\s+(\d+)u?(?:ll)?\b' % re.escape(name), text(path))
    assert found, '#define %s not found in %s' % (name, path)
    return int(found.group(1))


def enum_members(path, enum):
    """The enumerator names of `enum <enum> { ... }`, in order, without the trailing _COUNT."""
    body = re.search(r'enum %s \{(.*?)\};' % enum, text(path), re.S)
    assert body, 'enum %s not found in %s' % (enum, path)
    stripped = re.sub(r'/\*.*?\*/', '', body.group(1), flags=re.S)
    members = re.findall(r'(\w+)\s*=\s*\d+', stripped)
    assert members, enum + ' has no numbered members'
    return members


def suffixes(path, enum, prefix):
    return [name[len(prefix):] for name in enum_members(path, enum)]


def numbers(path, enum, prefix):
    return dict((name[len(prefix):], i) for i, name in enumerate(enum_members(path, enum)))


def cs_name_table(table):
    """[(text, enumerator suffix)] of a LabNames string array, from each entry's trailing comment."""
    body = re.search(r'public static readonly string\[\] %s =\s*\{(.*?)\n        \};' % table,
                     text(NAMES_CS), re.S)
    assert body, table + ' not found in LabState.cs'
    rows = []
    for line in body.group(1).splitlines():
        found = re.match(r'\s*"((?:[^"\\]|\\.)*)",\s*//\s*([A-Z0-9_]+)', line)
        if found:
            rows.append((found.group(1), found.group(2)))
    assert rows, table + ' has no commented entries'
    return rows


def cs_class(path, name):
    """The body of one C# class or struct, so a constant of that name elsewhere cannot answer for it."""
    body = re.search(r'(?:class|struct) %s\b(.*?)\n    \}' % re.escape(name), text(path), re.S)
    assert body, '%s not found in %s' % (name, path)
    return body.group(1)


def cs_const(source, name):
    """A `const ... <name> = <number>` inside the given C# text."""
    found = re.search(r'\b%s\s*=\s*(\d+)\b' % re.escape(name), source)
    assert found, name + ' not found'
    return int(found.group(1))


class CuModeAbiTest(unittest.TestCase):
    def test_layout(self):
        fields = c_fields(text(ESCAPE_HEADER), 'BC250_ESCAPE_CU_MODE')
        self.assertEqual(fields, cs_fields('CuModeSnapshot'))
        self.assertEqual(size_of(fields), 184)

    def test_size_passed_by_the_monitor(self):
        source = text(DRIVER_CS)
        self.assertIn('if (bytes != 184) throw', source)
        self.assertIn('Bc250CuMode(0, 0, out data, bytes)', source)

    def test_protocol_constants(self):
        for name, value in {'BC250_ESCAPE_RUN_CU_MODE': 22, 'BC250_CU_MODE_ABI': 1, 'BC250_CU_MODE_OP_READ': 0,
                            'BC250_CU_MODE_FLAG_VALID': 1, 'BC250_CU_MODE_FLAG_PENDING': 2,
                            'BC250_CU_MODE_FLAG_CONFIRMED': 4, 'BC250_CU_MODE_FLAG_STOCK_RECORD': 8,
                            'BC250_CU_MODE_FLAG_CONSISTENT': 16, 'BC250_CU_MODE_FLAG_WROTE': 32,
                            'BC250_CU_MODE_SA_COUNT': 4}.items():
            self.assertEqual(define(ESCAPE_HEADER, name), value, name)
        snapshot = cs_class(DRIVER_CS, 'CuModeSnapshot')
        for cs, suffix in {'FlagValid': 'VALID', 'FlagPending': 'PENDING', 'FlagConfirmed': 'CONFIRMED',
                           'FlagStockRecord': 'STOCK_RECORD', 'FlagConsistent': 'CONSISTENT',
                           'FlagWrote': 'WROTE'}.items():
            self.assertEqual(cs_const(snapshot, cs),
                             define(ESCAPE_HEADER, 'BC250_CU_MODE_FLAG_' + suffix), cs)
        self.assertEqual(cs_const(snapshot, 'Stock'), define(CU_HEADER, 'BC250_CU_MODE_STOCK'))
        self.assertEqual(cs_const(snapshot, 'Full'), define(CU_HEADER, 'BC250_CU_MODE_FULL'))
        self.assertEqual(define(CU_HEADER, 'BC250_CU_MODE_FULL'), 40)

    def test_the_monitor_reads_only(self):
        """The overlay never confirms a CU mode: that is `cumode confirm`, with an administrator behind it."""
        source = text(DRIVER_CS)
        self.assertNotIn('BC250_CU_MODE_OP_CONFIRM', source)
        self.assertNotIn('Bc250CuMode(1', source)

    def test_reason_names(self):
        table = cs_name_table('CuReason')
        self.assertEqual(suffixes(CU_HEADER, 'bc250_cu_reason', 'BC250_CU_REASON_'),
                         [suffix for _, suffix in table],
                         'LabNames.CuReason no longer follows enum bc250_cu_reason')

    def test_fallback_reasons_are_the_durable_ones(self):
        """A reason that makes the driver write `CuMode = 24` durably is red on the panel, not amber."""
        reason = numbers(CU_HEADER, 'bc250_cu_reason', 'BC250_CU_REASON_')
        found = re.search(r'IsFallbackReason\(uint reason\)\s*\{\s*return ([^;]+);', text(POINT_CS), re.S)
        self.assertIsNotNone(found, 'IsFallbackReason not found')
        used = set(int(n) for n in re.findall(r'reason == (\d+)', found.group(1)))
        self.assertEqual(used, set(reason[name] for name in
                                   ('PENDING_UNCONFIRMED', 'POWER_GATING', 'STOCK_UNEXPECTED',
                                    'READBACK', 'RESTORE_FAILED')))


class DpmAndHealthFlagTest(unittest.TestCase):
    def test_dpm_flags(self):
        view = cs_class(NAMES_CS, 'DpmView')
        for suffix, cs in {'RUNNING': 'FlagRunning', 'GOVERNING': 'FlagGoverning', 'PENDING': 'FlagPending',
                           'CONFIRMED': 'FlagConfirmed', 'PAUSED': 'FlagPaused', 'STABLE': 'FlagStable',
                           'SESSION': 'FlagSession'}.items():
            self.assertEqual(cs_const(view, cs), define(ESCAPE_HEADER, 'BC250_DPM_FLAG_' + suffix), suffix)
        self.assertEqual(cs_const(view, 'ModeFixed'), define(DPM_HEADER, 'BC250_DPM_MODE_FIXED'))
        self.assertEqual(cs_const(view, 'ModeDpm'), define(DPM_HEADER, 'BC250_DPM_MODE_DPM'))

    def test_throttle_names(self):
        table = cs_name_table('DpmThrottle')
        self.assertEqual(suffixes(DPM_HEADER, 'bc250_dpm_throttle', 'BC250_DPM_THROTTLE_'),
                         [suffix for _, suffix in table],
                         'LabNames.DpmThrottle no longer follows enum bc250_dpm_throttle')

    def test_throttle_rules_name_the_right_numbers(self):
        throttle = numbers(DPM_HEADER, 'bc250_dpm_throttle', 'BC250_DPM_THROTTLE_')
        amber = re.search(r'else if \((s\.Throttle == \d+(?: \|\| s\.Throttle == \d+)*)\) capLevel = Level\.Warn;',
                          text(POINT_CS))
        self.assertIsNotNone(amber, 'the amber throttle rule was not found')
        self.assertEqual(set(int(n) for n in re.findall(r'Throttle == (\d+)', amber.group(1))),
                         set(throttle[n] for n in ('THERMAL_SOFT', 'THERMAL_HARD', 'THERMAL_WARM', 'THERMAL_RAMP')))
        self.assertIn('s.Throttle == %d' % throttle['SMU'], text(POINT_CS))

    def test_start_health_flags(self):
        view = cs_class(POINT_CS, 'StartHealthView')
        for suffix, cs in {'FULL': 'FlagFull', 'READY': 'FlagReady', 'VISIBLE': 'FlagVisible',
                           'CONFIRMED': 'FlagConfirmed', 'REQUIRED': 'FlagRequired'}.items():
            self.assertEqual(cs_const(view, cs), define(ESCAPE_HEADER, 'BC250_START_HEALTH_' + suffix), suffix)
        self.assertEqual(cs_const(view, 'FreshMs'), define(ESCAPE_HEADER, 'BC250_START_HEALTH_FRESH_MS'))


class InteropEscapeTest(unittest.TestCase):
    """The escape the panel now reads, because the registry mirror cannot tell a live session from a dead one."""

    def test_layout(self):
        fields = c_fields(text(ESCAPE_HEADER), 'BC250_ESCAPE_INTEROP')
        self.assertEqual(fields, cs_fields('InteropSnapshot'))
        self.assertEqual(size_of(fields), 104)

    def test_size_passed_by_the_monitor(self):
        source = text(DRIVER_CS)
        self.assertIn('if (bytes != 104) throw', source)
        self.assertIn('Bc250Interop(out data, bytes)', source)

    def test_protocol_constants(self):
        snapshot = cs_class(DRIVER_CS, 'InteropSnapshot')
        self.assertEqual(define(ESCAPE_HEADER, 'BC250_ESCAPE_RUN_INTEROP'), 25)
        self.assertEqual(define(ESCAPE_HEADER, 'BC250_INTEROP_ABI'), 1)
        self.assertEqual(define(ESCAPE_HEADER, 'BC250_INTEROP_OP_READ'), 0)
        for cs, suffix in {'FlagValid': 'VALID', 'FlagSession': 'SESSION', 'FlagUnclean': 'UNCLEAN',
                           'FlagStale': 'STALE', 'FlagClosedByDriver': 'CLOSED_BY_DRIVER',
                           'FlagPersisted': 'PERSISTED', 'FlagPersistFailed': 'PERSIST_FAILED',
                           'FlagBlitAbsent': 'BLIT_ABSENT', 'FlagCddAbsent': 'CDD_ABSENT',
                           'FlagBlitUnreadable': 'BLIT_UNREADABLE', 'FlagCddUnreadable': 'CDD_UNREADABLE',
                           'FlagPowerCallback': 'POWER_CALLBACK', 'FlagDown': 'DOWN'}.items():
            self.assertEqual(cs_const(snapshot, cs),
                             define(ESCAPE_HEADER, 'BC250_INTEROP_FLAG_' + suffix), cs)

    def test_there_is_no_write_operation(self):
        """interop.c has no write escape at all: the operator changes the registry and restarts."""
        self.assertNotIn('BC250_INTEROP_OP_WRITE', text(ESCAPE_HEADER))
        self.assertIn('Bc250Interop(out InteropSnapshot data, uint bytes);', text(DRIVER_CS))

    def test_reason_names(self):
        """LabNames.InteropReason mirrors g_InteropReason of interop_policy.c, entry for entry."""
        body = re.search(r'g_InteropReason\[BC250_INTEROP_REASON_COUNT\] = \{(.*?)\};',
                         text(INTEROP_POLICY_C), re.S)
        self.assertIsNotNone(body, 'g_InteropReason not found')
        driver = re.findall(r'"([^"]*)"', body.group(1))
        body = re.search(r'string\[\] InteropReason =\s*\{(.*?)\};', text(NAMES_CS), re.S)
        self.assertIsNotNone(body, 'LabNames.InteropReason not found')
        self.assertEqual(driver, re.findall(r'"([^"]*)"', body.group(1)),
                         'LabNames.InteropReason no longer follows g_InteropReason')

    def test_the_session_marker_is_the_live_state(self):
        """
        Why the panel must not raise its level on `InteropSession` or `InteropLastEnd`: the first is on disk
        while a device that uses the path is alive, and the second is never cleared at a later start. Only the
        three flags above distinguish a marker a dead machine left. If the driver ever changes that, the panel's
        rule has to change with it.
        """
        source = text(INTEROP_C)
        self.assertRegex(source, r'InteropSession\s+this boot\'s BootId, on disk while a DDI device that used '
                                 r'the path is alive')
        self.assertRegex(source, r'InteropLastEnd\s+how the last session ended')
        # interop.c writes InteropLastEnd at the unmark and nothing deletes it afterwards.
        self.assertNotRegex(source, r'InteropDelete\(INTEROP_SETTING_LAST_END\)')

    def test_the_panel_judges_the_escape_flags_only(self):
        source = text(POINT_CS)
        marker = re.search(r'if \(v\.Session\.HasValue\) text\.Append\("; session marked"\);', source)
        self.assertIsNotNone(marker, 'the mirror path no longer prints the session marker')
        # No level change on either of the two mirror values: they are both normal states.
        tail = source[marker.start():marker.start() + 600]
        self.assertNotIn('Level.Warn', tail.split('panel.Rows.Add')[0])


class InteropMirrorTest(unittest.TestCase):
    def test_switch_bits(self):
        view = cs_class(POINT_CS, 'InteropView')
        blit = define(ESCAPE_HEADER, 'BC250_INTEROP_SWITCH_BLIT')
        cdd = define(ESCAPE_HEADER, 'BC250_INTEROP_SWITCH_CDD')
        self.assertEqual(cs_const(view, 'SwitchBlit'), blit)
        self.assertEqual(cs_const(view, 'SwitchCdd'), cdd)
        self.assertEqual(cs_const(view, 'Both'), blit | cdd)

    def test_end_codes(self):
        source = text(NAMES_CS)
        for suffix, words in {'NONE': 'none', 'STOP': 'device stop', 'USERS': 'last user gone',
                              'SYSTEM_POWER': 'system power', 'ADAPTER_D3': 'adapter D3'}.items():
            value = define(ESCAPE_HEADER, 'BC250_INTEROP_END_' + suffix)
            self.assertIn('case %d: return "%s";' % (value, words), source, suffix)

    def test_last_state_encoding(self):
        """`InteropLastState` is effective | requested << 8 (driver/kmd/interop.c)."""
        self.assertRegex(text(INTEROP_C), r'InteropLastState\s+effective bits \| requested bits << 8')
        source = text(POINT_CS)
        self.assertIn('(LastState.Value >> 8) & 0xFF', source)
        self.assertIn('LastState.Value & 0xFF', source)


class RegistryNameTest(unittest.TestCase):
    def found(self, path, pattern):
        return set(re.findall(pattern, text(path)))

    def test_cu_value_names(self):
        driver = self.found(CUMODE_C, r'L"(Cu\w+)"')
        used = self.found(POINT_CS, r'parameters\.Get\("(Cu\w+)"\)')
        self.assertTrue(used, 'the panel reads no CU value')
        self.assertLessEqual(used, driver,
                             'the overlay reads CU values the driver never writes: %s' % (used - driver))

    def test_interop_value_names(self):
        driver = self.found(INTEROP_C, r'L"(Interop\w+)"')
        used = self.found(POINT_CS, r'parameters\.Get\("(Interop\w+)"\)')
        self.assertTrue(used, 'the panel reads no interop value')
        self.assertLessEqual(used, driver,
                             'the overlay reads interop values the driver never writes: %s' % (used - driver))

    def test_latched_gate_names_exist_in_the_installer_table(self):
        defaults = json.loads(text(REGISTRY_DEFAULTS))['defaults']['parameters']
        body = re.search(r'LatchedGates =\s*\{(.*?)\};', text(GUARD_CS), re.S)
        self.assertIsNotNone(body)
        gates = re.findall(r'"(\w+)"', body.group(1))
        self.assertTrue(gates)
        for gate in gates:
            self.assertIn(gate, defaults, gate + ' is not a name the installer applies')

    def test_cu_mode_is_still_outside_the_installer_table(self):
        """
        The reason the panel adds `CuMode` to the expectations itself: no installer default carries it, so a
        release install leaves 24 behind. If a release ever does apply it, this test fails and the expectations
        default can be reconsidered.
        """
        defaults = json.loads(text(REGISTRY_DEFAULTS))['defaults']['parameters']
        self.assertNotIn('CuMode', defaults)
        self.assertIn('expected["CuMode"] = x.CuMode;', text(GUARD_CS))

    def test_parameters_key_path(self):
        self.assertIn(r'SYSTEM\CurrentControlSet\Services\bc250kmd\Parameters', text(POINT_CS))


class AbsentDefaultTest(unittest.TestCase):
    """
    The panel's own table of what the driver does with a name that is not in the Parameters key. It decides
    whether "absent" is a deviation, so every entry has to agree with the driver's reader.
    """

    def table(self):
        body = re.search(r'Dictionary<string, int> Absent = new[^{]*\{(.*?)\n        \};', text(GUARD_CS), re.S)
        assert body, 'the Absent table was not found in MeasurementGuardProvider.cs'
        return dict((name, int(value)) for name, value in re.findall(r'\{ "(\w+)", (\d+) \}', body.group(1)))

    def test_guard_read_setting_defaults(self):
        """Every GuardReadSetting(L"Name", N) of the driver that the table knows must carry the same N."""
        table = self.table()
        seen = 0
        for path in sorted((REPO / 'driver/kmd').glob('*.c')):
            for name, value in re.findall(r'GuardReadSetting\(L"(\w+)",\s*(\d+)\)', text(path)):
                if name in table:
                    seen += 1
                    self.assertEqual(table[name], int(value), '%s in %s' % (name, path.name))
        self.assertGreater(seen, 10, 'no driver reader was matched: the regular expression went stale')

    def test_the_two_interop_switches_are_on_when_absent(self):
        """The entries that make this table worth having: absent means 1, not a difference from the default 1."""
        table = self.table()
        self.assertEqual(table['EnableGpuPresentBlit'], 1)
        self.assertEqual(table['EnableCddDwmInterop'], 1)
        self.assertRegex(text(INTEROP_POLICY_H), r'an absent value means 1 \(on\)')
        self.assertIn('if (v->state == BC250_INTEROP_ABSENT) { *requested |= bit;', text(INTEROP_POLICY_C))

    def test_dpm_and_cu_defaults(self):
        table = self.table()
        self.assertEqual(table['DpmMaxMHz'], define(DPM_HEADER, 'BC250_DPM_DEFAULT_MAX_MHZ'))
        self.assertEqual(table['CuMode'], define(CU_HEADER, 'BC250_CU_MODE_STOCK'))
        # BC250_DPM_DEFAULT_MODE is BC250_DPM_MODE_FIXED, which the escape header numbers.
        self.assertRegex(text(DPM_HEADER), r'#define\s+BC250_DPM_DEFAULT_MODE\s+BC250_DPM_MODE_FIXED')
        self.assertEqual(table['DpmMode'], define(DPM_HEADER, 'BC250_DPM_MODE_FIXED'))

    def test_every_installer_default_is_known(self):
        """
        A name the installer applies whose absent meaning the table does not know would be reported as plain
        "absent", which is the old behaviour and never wrong - but it is worth knowing when one appears.
        """
        table = self.table()
        defaults = json.loads(text(REGISTRY_DEFAULTS))['defaults']['parameters']
        self.assertEqual([name for name in defaults if name not in table], [])


class DpmErrorToleranceTest(unittest.TestCase):
    def test_the_panel_does_not_call_a_recovered_retry_a_failure(self):
        """
        `Errors` counts failed SMU transitions of the whole start; the governor gives up only after
        BC250_DPM_ERROR_LIMIT failures in a row, and resets that counter on every success. So the count alone
        is not red, and the panel must not make it so (it reads amber, with the number in the text).
        """
        self.assertEqual(define(REPO / 'driver/kmd/dpm.h', 'BC250_DPM_ERROR_LIMIT'), 3)
        self.assertIn('S->ErrorsInRow = 0;', text(DPM_C))
        self.assertRegex(text(DPM_C), r'S->ErrorsInRow >= BC250_DPM_ERROR_LIMIT')
        source = text(POINT_CS)
        self.assertNotIn('s.Errors > 0) level = Level.Error', source)
        self.assertNotIn('s.Throttle == 6 || s.Errors > 0', source)
        self.assertIn('else if (s.Errors > 0) capLevel = Level.Warn;', source)


class StartHealthCompletionTest(unittest.TestCase):
    def test_the_no_completion_sentinel(self):
        """start_health.c writes ~0ull when nothing has completed; printed as an age it was 1.8e16 seconds."""
        self.assertRegex(text(REPO / 'driver/kmd/start_health.c'),
                         r'Data->LastCompletionAgeMs=H->LastCompletion \?.*: ~0ull;')
        self.assertIn('public const ulong NoCompletion = ulong.MaxValue;', text(POINT_CS))
        self.assertIn('if (v.NothingCompleted) text.Append(", no completed presentation yet");', text(POINT_CS))


class RunningReleaseWitnessTest(unittest.TestCase):
    """
    The `KMD image` row's green case. One kmd_abi covers every build of a driver revision, so the ABI word
    alone cannot say that the release's own .sys is the loaded one; the witness says it by SHA-256.
    """

    def test_path_and_field_names(self):
        witness = text(WITNESS_PS1)
        self.assertRegex(witness, r"WitnessPath = Join-Path \$env:ProgramData 'amdgpu-wddm\\installer\\running-release\.json'")
        guard = text(GUARD_CS)
        self.assertIn(r'@"amdgpu-wddm\installer\running-release.json"', guard)
        for field in ('schema', 'recorded_by', 'boot_id', 'version', 'kmd_build', 'kmd_abi'):
            self.assertRegex(witness, r'(?m)^\s+%s\s+=' % field, field + ' is not a witness field')
            self.assertIn('"%s"' % field, guard, field + ' is not read by the panel')

    def test_the_two_writers_and_the_schema(self):
        witness = text(WITNESS_PS1)
        self.assertRegex(witness, r'WitnessSchema = 1')
        self.assertRegex(witness, r"ValidateSet\('verify', 'start-confirm'\)\]\[string\]\$RecordedBy")
        guard = text(GUARD_CS)
        self.assertIn('schema != "1"', guard)
        self.assertIn('by != "verify" && by != "start-confirm"', guard)

    def test_the_boot_id_is_read_the_way_the_driver_tools_read_it(self):
        key = r'SYSTEM\CurrentControlSet\Control\Session Manager\Memory Management\PrefetchParameters'
        self.assertIn(key, text(WITNESS_PS1))
        self.assertIn(key, text(GUARD_CS))
        self.assertIn('"BootId"', text(GUARD_CS))

    def test_the_witness_binds_the_loaded_image(self):
        """What makes the record worth trusting: it is written only when the loaded image hashes to the release's."""
        witness = text(WITNESS_PS1)
        self.assertRegex(witness, r'\$Reading\.image\.sha256 -ne \(\[string\]\$comp\.sha256\)\.ToUpperInvariant\(\)')
        self.assertRegex(witness, r'was replaced after this boot started')

    def test_one_abi_covers_several_builds(self):
        """The reason the ABI word alone is not enough; if this ever stops being true, say so in the README."""
        self.assertRegex(text(REPO / 'tools/release/test-dryrun.ps1'),
                         r"kmd_abi -eq '0x[0-9A-F]+' -and \[version\]\(\$m\.kmd_build\) -ge")


class ControlDllExportTest(unittest.TestCase):
    def test_cu_mode_export_signature(self):
        """The managed declaration must match the control DLL's export, argument for argument."""
        native = re.search(r'Bc250CuMode\(ULONG op, ULONGLONG expectedGeneration, '
                           r'BC250_ESCAPE_CU_MODE \*data, ULONG bytes\)', text(CLI))
        self.assertIsNotNone(native, 'Bc250CuMode not found in the control DLL source')
        self.assertIn('static extern int Bc250CuMode(uint op, ulong expectedGeneration, '
                      'out CuModeSnapshot data, uint bytes);', text(DRIVER_CS))

    def test_interop_export_signature(self):
        native = re.search(r'Bc250Interop\(BC250_ESCAPE_INTEROP \*data, ULONG bytes\)', text(CLI))
        self.assertIsNotNone(native, 'Bc250Interop not found in the control DLL source')
        self.assertIn('static extern int Bc250Interop(out InteropSnapshot data, uint bytes);', text(DRIVER_CS))

    def test_the_panel_only_sends_software_snapshots(self):
        """
        Every escape of this panel is answered with NoAdapterSynchronization alone, which is what keeps it clear
        of the adapter lock a game's threads wait on (BD-054). The escape header states it per command, right
        above the structure, and the driver's request handlers refuse any other flag.
        """
        header = text(ESCAPE_HEADER)
        for command, struct in (('BC250_ESCAPE_RUN_CU_MODE', 'BC250_ESCAPE_CU_MODE'),
                                ('BC250_ESCAPE_RUN_INTEROP', 'BC250_ESCAPE_INTEROP')):
            block = header[:header.index('typedef struct _' + struct)].rsplit('// ---', 1)[-1]
            self.assertIn('NoAdapterSynchronization', block.rsplit('\n\n', 1)[-1],
                          command + ' no longer documents NoAdapterSynchronization')
        # Start health says it in its own words, and KmdProvider has sent that one since long before this panel.
        self.assertRegex(header, r'Adapter-owned software snapshot; READ must not idle GPU scheduling or read BARs')
        for handler, source in (('CuModeRequest', text(CUMODE_C)), ('InteropRequest', text(INTEROP_C))):
            found = re.search(r'%s\(.*?\n\}' % handler, source, re.S)
            self.assertIsNotNone(found, handler + ' not found')
            self.assertIn('Flags', found.group(0), handler + ' no longer checks the escape flags')


if __name__ == '__main__':
    unittest.main()
