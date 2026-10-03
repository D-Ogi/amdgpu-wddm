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
        public const int DpmBytes = 160, StartHealthBytes = 96, InteropBytes = 104, VideoMemoryBytes = 264;
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
