// Replies of the KMD's software-state escapes, parsed from the raw buffers bc250control.dll fills. Pure functions:
// no P/Invoke here, so the host tests run on any PC. Layouts: driver/kmd/bc250kmd_escape.h (BC250_ESCAPE_DPM 160
// bytes, BC250_ESCAPE_START_HEALTH 96, BC250_ESCAPE_INTEROP 104, BC250_ESCAPE_LOG 10812) and BC250_VIDEO_MEMORY of
// tools/win/bc250kmd_cli/bc250kmd_cli.c (264). test/UnitTests.cs checks the offsets against the header text.
using System;
using System.Collections.Generic;
using System.Globalization;
using System.Text;

namespace AmdgpuWddmControl
{
    public sealed class DpmState
    {
        public uint Version, Flags, Mode, Requested, Reason, Throttle;
        public uint MaxMHz, CapMHz, TargetMHz, WantMHz, CurrentMHz, CurrentMv, ObservedMHz, ObservedVid;
        public int TemperatureMc;
        public uint BusyPermille, BusyAvgPermille, ThermalEvents, Errors;
        public ulong UptimeMs, Generation;

        public const uint FlagRunning = 1, FlagGoverning = 2, FlagPending = 4, FlagConfirmed = 8, FlagPaused = 16,
            FlagStable = 32, FlagSession = 64, FlagTemperature = 128, FlagClock = 256, FlagHwBusy = 512;
        public bool Has(uint flag) { return (Flags & flag) != 0; }
    }

    public sealed class InteropState
    {
        public uint Version, Flags, Requested, Effective, Reason, ClosedReason, BlitSetting, CddSetting, BootId, SessionBootId;
        public uint Users, Marks, Unmarks, MarkFailures, PreviousEnd, LastEnd;
        public ulong Generation;
        public const uint SwitchBlit = 1, SwitchCdd = 2, FlagValid = 1, FlagSession = 2, FlagUnclean = 4, FlagStale = 8, FlagClosedByDriver = 16;
    }

    public sealed class StartHealthState
    {
        public uint Version, Flags;
        public ulong Generation, Epoch, Completed, LastCompletionAgeMs, ReadyAgeMs;
        public const uint Full = 1, Ready = 2, Visible = 4, Confirmed = 8;
    }

    // BC250_ESCAPE_CU_MODE (driver/kmd/cumode.c, docs/design/cu-mode.md): the CU mode of the start this describes.
    // Applied=40 is a mode, not proof of 40 active cores: the count is ActiveCus.
    public sealed class CuModeState
    {
        public uint Version, Flags, Requested, Applied, Reason, ActiveCus, DisableMask, PciId;
        public ulong Generation;
        public const uint FlagValid = 1, FlagPending = 2, FlagConfirmed = 4;
        public bool Has(uint flag) { return (Flags & flag) != 0; }
    }

    // BC250_ESCAPE_HWMON (driver/kmd/hwmon.c, docs/design/fan.md): what the board's own hardware monitor says
    // about the case fan. Read only, and only the chip's own numbers: the fan still follows the BIOS curve.
    // Reading means the reader is online AND the sample is fresh; everything else is "No reading", never a guess.
    public sealed class HwmonState
    {
        public uint Version, Flags, Reason, BasePort, CustomerId, EcVersion, EcBuild;
        public uint FanPresentMask, DutyPresentMask, ModeMask, AgeMs;
        public uint Engine, RpmValidMask, DutyValidMask;
        public uint[] Rpm = new uint[8];
        public uint[] DutyPermille = new uint[8];
        public int[] TemperatureMc = new int[4];
        public uint[] TemperatureSource = new uint[4];
        public ulong Samples, Errors, Retries, Refusals, Generation;

        public const uint FlagValid = 1, FlagMonitoring = 2, FlagFresh = 4, FlagGated = 8,
            FlagIdPinned = 16, FlagDutyProven = 32, FlagStopped = 64;
        public const uint SourceApu = 0x46;         // AMD TSI at SMBus 0x98: the APU die
        public bool Has(uint flag) { return (Flags & flag) != 0; }
        public bool Reading { get { return Has(FlagValid) && Has(FlagFresh); } }

        // The board has five tachometer channels and one fan, so the reading is the fastest channel that turns.
        public uint FastestRpm
        {
            get { uint best = 0; foreach (var r in Rpm) if (r > best) best = r; return best; }
        }
        // The duty READ-BACK, highest channel, as a percentage. Shown only with FlagDutyProven.
        public uint DutyPercent
        {
            get { uint best = 0; foreach (var d in DutyPermille) if (d > best) best = d; return (best + 5) / 10; }
        }
        // The chip's own reading of the APU, which is an independent second measurement of Tctl. Support report only.
        public int? ApuMc
        {
            get
            {
                for (int i = 0; i < TemperatureSource.Length; i++)
                    if (TemperatureSource[i] == SourceApu) return TemperatureMc[i];
                return null;
            }
        }
    }

    // BC250_ESCAPE_DPM_CURVE (360 bytes, driver/kmd/dpm.c, docs/design/tuner.md): the operator's V/F curve, its
    // trial and the four vectors the chart draws. Candidate is the curve on trial, empty outside one.
    public sealed class CurveState
    {
        public uint Version, Flags, TrialMs, TrialRemainingMs, Serial, Applied, Error, ErrorLevel;
        public uint FirstMHz, StepMHz, Points;
        public uint[] Candidate, Active, Stored, Default, Floor;
        public uint Level, LevelMHz, LevelMv, ObservedMHz, ObservedVid, CeilingMHz, Mode;
        public int TemperatureMc;
        public uint Sets, Keeps, Cancels, Reverts;
        public ulong Generation;
        public const uint FlagValid = 1, FlagOnTrial = 2, FlagStored = 4, FlagPending = 8, FlagConfirmed = 16,
            FlagDefault = 32, FlagGoverning = 64, FlagApplied = 128;
        public bool Has(uint flag) { return (Flags & flag) != 0; }
    }

    // BC250_ESCAPE_CPU (296 bytes, driver/kmd/cpu.c): the processor's clock limit, undervolt, temperature cap,
    // readbacks and core mask. Applied is what the driver last sent, Stored what a Keep wrote, Baseline what the
    // first read of this start recorded.
    public sealed class CpuState
    {
        public uint Version, Flags, TrialMs, TrialRemainingMs, Serial, Error, Given;
        public uint AppliedMaxMHz, AppliedUvSteps, AppliedTempC;
        public uint StoredMaxMHz, StoredUvSteps, StoredTempC;
        public uint BaselineMaxMHz, BaselineUvSteps, BaselineTempC;
        public uint VoltageMv, GpuVoltageMv, CapC, Features;
        public uint[] CoreMHz, PstateMHz;
        public uint Cores, Threads, CoreMask, CoreMaskStored;
        public uint LastQueue, LastMessage, LastStatus, LastParameter;
        public int TemperatureMc;
        public uint SearchStep, SearchBest, SearchFail, SearchTested, Reads, Writes, Refusals, Reverts;
        // An owed way back: a revert was refused and the driver repeats it. RevertFailures counts the refusals.
        public uint RevertRetries, RevertFailures;
        public ulong Generation;
        public const uint FlagValid = 1, FlagOnTrial = 2, FlagStored = 4, FlagPending = 8, FlagConfirmed = 16,
            FlagQueue3Proven = 32, FlagTuneOn = 64, FlagSearching = 128, FlagCorePending = 256,
            FlagCoreConfirmed = 512, FlagBusy = 1024, FlagRevertOwed = 2048, FlagTempValid = 4096;
        public const uint GivenMax = 1, GivenUv = 2, GivenTemp = 4;
        public bool Has(uint flag) { return (Flags & flag) != 0; }
    }

    public sealed class VideoMemoryState
    {
        public uint Segments;
        public ulong LocalResident, LocalCommitted, LocalLimit, ApertureResident, ApertureLimit, Dedicated;
    }

    public sealed class LogPage
    {
        public uint Version, Total, Lost, Above, Returned, Next, RingLines;
        public readonly List<LogLine> Lines = new List<LogLine>();
    }

    public struct LogLine
    {
        public uint Sequence, Milliseconds;
        public string Text;
    }

    public static class KmdReply
    {
        public const uint Magic = 0x30353242;   // "B250"
        public const int DpmBytes = 160, StartHealthBytes = 96, InteropBytes = 104, VideoMemoryBytes = 264, CuModeBytes = 184;
        public const int HwmonBytes = 216;
        public const int CurveBytes = 360, CpuBytes = 296, CurvePoints = 11, CpuCoreSlots = 8;
        public const uint CmdCuMode = 22, CmdHwmon = 27, CmdCurve = 28, CmdCpu = 29;
        public const int LogHeadBytes = 60, LogLineBytes = 168, LogTextBytes = 160, LogMaxLines = 64;
        public const int LogBytes = LogHeadBytes + LogMaxLines * LogLineBytes;
        public const uint CmdStartHealth = 21, CmdDpm = 23, CmdInterop = 25, CmdGetLog = 12;

        static uint U(byte[] b, int index) { return BitConverter.ToUInt32(b, index * 4); }
        static ulong Q(byte[] b, int offset) { return BitConverter.ToUInt64(b, offset); }

        static void Head(byte[] b, int size, uint command)
        {
            if (b == null || b.Length != size) throw new FormatException("reply has " + (b == null ? 0 : b.Length) + " bytes, " + size + " expected");
            if (U(b, 0) != Magic || U(b, 1) != command) throw new FormatException("reply is not a command " + command + " answer");
        }

        public static DpmState ParseDpm(byte[] b)
        {
            Head(b, DpmBytes, CmdDpm);
            if (U(b, 5) != 1) throw new FormatException("DPM ABI " + U(b, 5) + ", 1 expected");
            return new DpmState
            {
                Version = U(b, 3), Flags = U(b, 7), Mode = U(b, 8), Requested = U(b, 9), Reason = U(b, 10), Throttle = U(b, 11),
                MaxMHz = U(b, 12), CapMHz = U(b, 13), TargetMHz = U(b, 14), WantMHz = U(b, 15),
                CurrentMHz = U(b, 16), CurrentMv = U(b, 17), ObservedMHz = U(b, 18), ObservedVid = U(b, 19),
                TemperatureMc = (int)U(b, 20), BusyPermille = U(b, 21), BusyAvgPermille = U(b, 22),
                ThermalEvents = U(b, 25), Errors = U(b, 26), UptimeMs = Q(b, 128), Generation = Q(b, 136),
            };
        }

        public static InteropState ParseInterop(byte[] b)
        {
            Head(b, InteropBytes, CmdInterop);
            if (U(b, 5) != 1) throw new FormatException("interop ABI " + U(b, 5) + ", 1 expected");
            return new InteropState
            {
                Version = U(b, 3), Flags = U(b, 7), Requested = U(b, 8), Effective = U(b, 9), Reason = U(b, 10),
                ClosedReason = U(b, 11), BlitSetting = U(b, 12), CddSetting = U(b, 13), BootId = U(b, 14), SessionBootId = U(b, 15),
                Users = U(b, 16), Marks = U(b, 17), Unmarks = U(b, 18), MarkFailures = U(b, 19), PreviousEnd = U(b, 20), LastEnd = U(b, 21),
                Generation = Q(b, 88),
            };
        }

        public static StartHealthState ParseStartHealth(byte[] b)
        {
            Head(b, StartHealthBytes, CmdStartHealth);
            if (U(b, 5) != 1) throw new FormatException("start health ABI " + U(b, 5) + ", 1 expected");
            return new StartHealthState
            {
                Version = U(b, 3), Flags = U(b, 7), Generation = Q(b, 32), Epoch = Q(b, 40), Completed = Q(b, 48),
                LastCompletionAgeMs = Q(b, 56), ReadyAgeMs = Q(b, 64),
            };
        }

        public static CuModeState ParseCuMode(byte[] b)
        {
            Head(b, CuModeBytes, CmdCuMode);
            if (U(b, 5) != 1) throw new FormatException("CU mode ABI " + U(b, 5) + ", 1 expected");
            return new CuModeState
            {
                Version = U(b, 3), Flags = U(b, 7), Requested = U(b, 8), Applied = U(b, 9), Reason = U(b, 10),
                ActiveCus = U(b, 11), DisableMask = U(b, 12), PciId = U(b, 13), Generation = Q(b, 160),
            };
        }

        public static HwmonState ParseHwmon(byte[] b)
        {
            Head(b, HwmonBytes, CmdHwmon);
            if (U(b, 5) != 1) throw new FormatException("hardware monitor ABI " + U(b, 5) + ", 1 expected");
            var h = new HwmonState
            {
                Version = U(b, 3), Flags = U(b, 7), BasePort = U(b, 8), CustomerId = U(b, 9),
                EcVersion = U(b, 10), EcBuild = U(b, 11), FanPresentMask = U(b, 12), DutyPresentMask = U(b, 13),
                ModeMask = U(b, 14), AgeMs = U(b, 39),
                Samples = Q(b, 160), Errors = Q(b, 168), Retries = Q(b, 176), Generation = Q(b, 184),
                Reason = U(b, 48), Engine = U(b, 49), RpmValidMask = U(b, 50), DutyValidMask = U(b, 51),
                Refusals = Q(b, 208),
            };
            for (int i = 0; i < 8; i++) { h.Rpm[i] = U(b, 15 + i); h.DutyPermille[i] = U(b, 23 + i); }
            for (int i = 0; i < 4; i++) { h.TemperatureMc[i] = (int)U(b, 31 + i); h.TemperatureSource[i] = U(b, 35 + i); }
            return h;
        }

        static uint[] Vector(byte[] b, int first, int count)
        {
            var v = new uint[count];
            for (int i = 0; i < count; i++) v[i] = U(b, first + i);
            return v;
        }

        public static CurveState ParseCurve(byte[] b)
        {
            Head(b, CurveBytes, CmdCurve);
            if (U(b, 5) != 1) throw new FormatException("curve ABI " + U(b, 5) + ", 1 expected");
            if (U(b, 16) != CurvePoints) throw new FormatException("curve has " + U(b, 16) + " points, " + CurvePoints + " expected");
            return new CurveState
            {
                Version = U(b, 3), Flags = U(b, 7), TrialMs = U(b, 8), TrialRemainingMs = U(b, 9),
                Serial = U(b, 10), Applied = U(b, 11), Error = U(b, 12), ErrorLevel = U(b, 13),
                FirstMHz = U(b, 14), StepMHz = U(b, 15), Points = U(b, 16),
                Candidate = Vector(b, 17, CurvePoints), Active = Vector(b, 28, CurvePoints),
                Stored = Vector(b, 39, CurvePoints), Default = Vector(b, 50, CurvePoints),
                Floor = Vector(b, 61, CurvePoints),
                Level = U(b, 72), LevelMHz = U(b, 73), LevelMv = U(b, 74),
                ObservedMHz = U(b, 75), ObservedVid = U(b, 76), TemperatureMc = (int)U(b, 77),
                CeilingMHz = U(b, 78), Mode = U(b, 79),
                Sets = U(b, 80), Keeps = U(b, 81), Cancels = U(b, 82), Reverts = U(b, 83),
                Generation = Q(b, 336),
            };
        }

        public static CpuState ParseCpu(byte[] b)
        {
            Head(b, CpuBytes, CmdCpu);
            if (U(b, 5) != 1) throw new FormatException("CPU ABI " + U(b, 5) + ", 1 expected");
            return new CpuState
            {
                Version = U(b, 3), Flags = U(b, 7), TrialMs = U(b, 8), TrialRemainingMs = U(b, 9),
                Serial = U(b, 10), Error = U(b, 11), Given = U(b, 12),
                AppliedMaxMHz = U(b, 16), AppliedUvSteps = U(b, 17), AppliedTempC = U(b, 18),
                StoredMaxMHz = U(b, 19), StoredUvSteps = U(b, 20), StoredTempC = U(b, 21),
                BaselineMaxMHz = U(b, 22), BaselineUvSteps = U(b, 23), BaselineTempC = U(b, 24),
                VoltageMv = U(b, 25), GpuVoltageMv = U(b, 26), CapC = U(b, 27), Features = U(b, 28),
                CoreMHz = Vector(b, 29, CpuCoreSlots), PstateMHz = Vector(b, 37, CpuCoreSlots),
                Cores = U(b, 45), Threads = U(b, 46), CoreMask = U(b, 47), CoreMaskStored = U(b, 48),
                LastQueue = U(b, 49), LastMessage = U(b, 50), LastStatus = U(b, 51), LastParameter = U(b, 52),
                TemperatureMc = (int)U(b, 53),
                SearchStep = U(b, 54), SearchBest = U(b, 55), SearchFail = U(b, 56), SearchTested = U(b, 57),
                Reads = U(b, 58), Writes = U(b, 59), Refusals = U(b, 60), Reverts = U(b, 61),
                RevertRetries = U(b, 62), RevertFailures = U(b, 63),
                Generation = Q(b, 272),
            };
        }

        public static VideoMemoryState ParseVideoMemory(byte[] b)
        {
            if (b == null || b.Length != VideoMemoryBytes || U(b, 0) != VideoMemoryBytes) throw new FormatException("video memory reply size");
            return new VideoMemoryState
            {
                Segments = U(b, 1), LocalResident = Q(b, 24), LocalCommitted = Q(b, 32), LocalLimit = Q(b, 40),
                ApertureResident = Q(b, 48), ApertureLimit = Q(b, 56), Dedicated = Q(b, 64),
            };
        }

        public static LogPage ParseLog(byte[] b)
        {
            Head(b, LogBytes, CmdGetLog);
            var page = new LogPage
            {
                Version = U(b, 3), Total = U(b, 7), Lost = U(b, 8), Above = U(b, 9), Returned = U(b, 10), Next = U(b, 11),
                RingLines = U(b, 13),
            };
            if (page.Returned > LogMaxLines) throw new FormatException("log page claims " + page.Returned + " lines");
            for (int i = 0; i < page.Returned; i++)
            {
                int at = LogHeadBytes + i * LogLineBytes;
                int end = at + 8, limit = at + 8 + LogTextBytes;
                while (end < limit && b[end] != 0) end++;
                page.Lines.Add(new LogLine
                {
                    Sequence = BitConverter.ToUInt32(b, at),
                    Milliseconds = BitConverter.ToUInt32(b, at + 4),
                    Text = Encoding.ASCII.GetString(b, at + 8, end - at - 8),
                });
            }
            return page;
        }

        // BC250_KMD_VERSION: milestone in the high half, revision in the low half (0x000700C5 is 0.7.197).
        public static string VersionText(uint v)
        {
            if (v == 0) return "unknown";
            return (v >> 24) + "." + ((v >> 16) & 0xFF) + "." + (v & 0xFFFF);
        }

        public static string ModeText(uint mode) { return mode == 1 ? "Automatic (DPM)" : mode == 0 ? "Fixed (1000 MHz)" : "Unknown (" + mode + ")"; }

        // enum bc250_dpm_reason, driver/shim/include/bc250_dpm.h
        public static string ReasonText(uint reason)
        {
            switch (reason)
            {
                case 0: return "As requested";
                case 1: return "Automatic clocks are off in the settings";
                case 2: return "The setting is out of range, fixed clock used";
                case 3: return "The last start with automatic clocks did not finish, fixed clock used";
                case 4: return "Windows stopped while the clock was high, fixed clock used";
                case 5: return "The driver could not save its start mark, fixed clock used";
                case 6: return "No power management controller, fixed clock used";
                case 7: return "The driver did not start the clock governor";
                case 8: return "The clock governor stopped after errors";
                default: return "Unknown reason (" + reason + ")";
            }
        }

        // enum bc250_dpm_throttle, driver/shim/include/bc250_dpm.h
        public static string ThrottleText(uint throttle)
        {
            switch (throttle)
            {
                case 0: return "None";
                case 1: return "Temperature at 87 C: clock reduced";
                case 2: return "Temperature at 90 C: lowest clock";
                case 3: return "No temperature reading: lowest clock";
                case 4: return "Clock ceiling setting";
                case 5: return "Stable power state requested by an application";
                case 6: return "Governor stopped after errors";
                case 7: return "Fixed clock";
                default: return "Unknown (" + throttle + ")";
            }
        }

        public static string TemperatureText(DpmState d)
        {
            return d.Has(DpmState.FlagTemperature) ? (d.TemperatureMc / 1000.0).ToString("0.0", CultureInfo.InvariantCulture) + " C" : "not available";
        }

        public static string ClockText(DpmState d)
        {
            uint mhz = d.Has(DpmState.FlagClock) && d.ObservedMHz != 0 ? d.ObservedMHz : d.CurrentMHz;
            return mhz == 0 ? "not available" : mhz + " MHz";
        }

        public static string CompositionText(InteropState s)
        {
            if ((s.Flags & InteropState.FlagValid) == 0) return "Not decided (driver start incomplete)";
            if (s.Effective == 0)
                return (s.Flags & InteropState.FlagClosedByDriver) != 0 ? "CPU (the driver closed the GPU path after a failure)" : "CPU (GPU path off)";
            return s.Users > 0 ? "GPU (" + s.Users + " device" + (s.Users == 1 ? "" : "s") + " on the GPU path)" : "GPU path ready, not in use now";
        }

        // The release's desktop router (HKLM\SOFTWARE\amdgpu-wddm\DesktopRouter DwmForceCpu) wins over the KMD's
        // interop decision: with DwmForceCpu 1 DWM loads the CPU UMD whatever the switches say. Changed only by the
        // Recovery page's actions (Recovery.cs).
        public const string DesktopRouterPath = @"SOFTWARE\amdgpu-wddm\DesktopRouter";

        public static string CompositionLine(uint? dwmForceCpu, InteropState interop, string interopError)
        {
            if (dwmForceCpu == 1) return "CPU route (GPU route disabled: DwmForceCpu 1)";
            return interop != null ? CompositionText(interop) : interopError ?? "-";
        }

        public static string StatusText(int ntstatus)
        {
            switch ((uint)ntstatus)
            {
                case 0xC000000E: return "The BC-250 driver is not loaded";
                case 0xC00000BB: return "The driver does not support this request";
                case 0xC00000A3: return "The driver is not ready";
                case 0xC0000022: return "Access denied";
                default: return "Driver request failed (0x" + ((uint)ntstatus).ToString("X8") + ")";
            }
        }

        public static string Bytes(ulong v)
        {
            if (v == ulong.MaxValue) return "no limit";
            if (v >= 1UL << 30) return (v / (double)(1UL << 30)).ToString("0.00", CultureInfo.InvariantCulture) + " GiB";
            if (v >= 1UL << 20) return (v / (double)(1UL << 20)).ToString("0", CultureInfo.InvariantCulture) + " MiB";
            return v + " bytes";
        }
    }
}
