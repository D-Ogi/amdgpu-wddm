// Typed KMD clock client. No bc250rd handle, raw mailbox messages or fallback.
using System;
using System.Runtime.InteropServices;

namespace Bc250Mon
{
    [StructLayout(LayoutKind.Sequential)]
    public struct ClockSnapshot
    {
        public uint Magic, Command, Status, Version;
        public uint NtStatus, AbiVersion, Op, RequestedMHz, RequestedMv;
        public uint ObservedMHz, ObservedVid;
        public int TemperatureMc;
        public uint InitialMHz, InitialVid, ExpectedVid, VoltageStaged, Ready;
        public uint Reserved0, Reserved1, Reserved2;
    }
    [StructLayout(LayoutKind.Sequential)]
    public struct StartHealthSnapshot
    {
        public uint Magic, Command, Status, Version;
        public uint NtStatus, AbiVersion, Op, Flags;
        public ulong Generation, Epoch, Completed, LastCompletionAgeMs, ReadyAgeMs;
        public ulong ExpectedGeneration, ExpectedEpoch;
        public uint Reserved0, Reserved1;
    }
    // BC250_ESCAPE_DPM, ABI 1, 160 bytes (driver/kmd/bc250kmd_escape.h of KMD 0.7.175 and later; test_telemetry.py).
    [StructLayout(LayoutKind.Sequential)]
    public struct DpmSnapshot
    {
        public uint Magic, Command, Status, Version;
        public uint NtStatus, AbiVersion, Op, Flags;
        public uint Mode, Requested, Reason, Throttle;
        public uint MaxMHz, CapMHz, TargetMHz, WantMHz;
        public uint CurrentMHz, CurrentMv, ObservedMHz, ObservedVid;
        public int TemperatureMc;
        public uint BusyPermille, BusyAvgPermille;
        public uint Raises, Lowers, ThermalEvents, Errors, Resyncs;
        public ulong Ticks;
        public ulong BusyTime100ns;
        public ulong UptimeMs;
        public ulong Generation;
        public ulong ExpectedGeneration;
        public uint SubmitBusyPermille;
        public uint SdmaBusyPermille;

        public const uint FlagTemperature = 128, FlagClock = 256, FlagHwBusy = 512;
    }
    // BC250_ESCAPE_CU_MODE, ABI 1, 184 bytes (driver/kmd/bc250kmd_escape.h; test_lab_state.py keeps the two
    // layouts equal). The snapshot the start's GFX bring-up left behind: how many compute units this start
    // applied, how many the registers name, and whether the 40 CU request is confirmed. No BAR access.
    [StructLayout(LayoutKind.Sequential)]
    public struct CuModeSnapshot
    {
        public uint Magic, Command, Status, Version;
        public uint NtStatus, AbiVersion, Op, Flags;
        public uint Requested;              // CuMode as read at start, 0 when absent (absent means 24)
        public uint Applied;                // 24 or 40, 0 when unknown (not run, restore failed)
        public uint Reason, ActiveCus, DisableMask, PciId;
        public uint RlcPgCntl, RlcAonWgpMask;
        [MarshalAs(UnmanagedType.ByValArray, SizeConst = 4)] public uint[] StockCc;
        [MarshalAs(UnmanagedType.ByValArray, SizeConst = 4)] public uint[] StockSpi;
        [MarshalAs(UnmanagedType.ByValArray, SizeConst = 4)] public uint[] Cc;
        [MarshalAs(UnmanagedType.ByValArray, SizeConst = 4)] public uint[] User;
        [MarshalAs(UnmanagedType.ByValArray, SizeConst = 4)] public uint[] Spi;
        [MarshalAs(UnmanagedType.ByValArray, SizeConst = 4)] public uint[] ActiveWgps;
        public ulong Generation;
        public ulong ExpectedGeneration;
        [MarshalAs(UnmanagedType.ByValArray, SizeConst = 2)] public uint[] Reserved;

        public const uint FlagValid = 1, FlagPending = 2, FlagConfirmed = 4,
                          FlagStockRecord = 8, FlagConsistent = 16, FlagWrote = 32;
        public const uint Stock = 24, Full = 40;
    }
    // BC250_ESCAPE_INTEROP, ABI 1, 104 bytes (driver/kmd/bc250kmd_escape.h; test_lab_state.py keeps the two
    // layouts equal). The GPU DWM interop decision of this start. The registry mirror the driver leaves behind
    // (`InteropLastState`) carries the same effective and requested bits but none of these flags, and without
    // them a live session marker cannot be told from the trace of a machine that died with the path in use.
    [StructLayout(LayoutKind.Sequential)]
    public struct InteropSnapshot
    {
        public uint Magic, Command, Status, Version;
        public uint NtStatus, AbiVersion, Op, Flags;
        public uint Requested, Effective, Reason, ClosedReason;
        public uint BlitSetting, CddSetting;
        public uint BootId, SessionBootId;
        public uint Users, Marks, Unmarks, MarkFailures;
        public uint PreviousEnd, LastEnd;
        public ulong Generation;
        [MarshalAs(UnmanagedType.ByValArray, SizeConst = 2)] public uint[] Reserved;

        public const uint FlagValid = 1, FlagSession = 2, FlagUnclean = 4, FlagStale = 8,
                          FlagClosedByDriver = 16, FlagPersisted = 32, FlagPersistFailed = 64,
                          FlagBlitAbsent = 128, FlagCddAbsent = 256, FlagBlitUnreadable = 512,
                          FlagCddUnreadable = 1024, FlagPowerCallback = 2048, FlagDown = 4096;
    }
    // BC250_VIDEO_MEMORY of tools/win/bc250kmd_cli/bc250kmd_cli.c: the control DLL's digest of dxgkrnl's segment
    // statistics, 264 bytes (test_telemetry.py).
    [StructLayout(LayoutKind.Sequential)]
    public struct VideoMemorySnapshot
    {
        public uint Size;
        public uint Segments;
        public uint ApertureMask;
        public uint LuidLow;
        public int LuidHigh;
        public uint Reserved;
        public ulong LocalResident, LocalCommitted, LocalLimit;
        public ulong ApertureResident, ApertureLimit;
        public ulong DedicatedVideoMemory;
        [MarshalAs(UnmanagedType.ByValArray, SizeConst = 8)] public ulong[] Resident;
        [MarshalAs(UnmanagedType.ByValArray, SizeConst = 8)] public ulong[] Committed;
        [MarshalAs(UnmanagedType.ByValArray, SizeConst = 8)] public ulong[] Limit;
    }
    // BC250_ESCAPE_HWMON, ABI 1, 216 bytes (driver/kmd/bc250kmd_escape.h; test_telemetry.py keeps the two
    // layouts equal). What the board's own Nuvoton hardware monitor says about the case fan: the tachometers,
    // the duty read-back and the chip's own temperatures. A published snapshot, so no port access and no BAR
    // access happen in this escape; the fan itself still follows the BIOS curve (docs/design/fan.md).
    [StructLayout(LayoutKind.Sequential)]
    public struct HwmonSnapshot
    {
        public uint Magic, Command, Status, Version;
        public uint NtStatus, AbiVersion, Op, Flags;
        public uint BasePort;
        public uint CustomerId;
        public uint EcVersion;
        public uint EcBuild;
        public uint FanPresentMask;
        public uint DutyPresentMask;
        public uint ModeMask;
        [MarshalAs(UnmanagedType.ByValArray, SizeConst = 8)] public uint[] Rpm;
        [MarshalAs(UnmanagedType.ByValArray, SizeConst = 8)] public uint[] DutyPermille;
        [MarshalAs(UnmanagedType.ByValArray, SizeConst = 4)] public int[] TemperatureMc;
        [MarshalAs(UnmanagedType.ByValArray, SizeConst = 4)] public uint[] TemperatureSource;
        public uint AgeMs;
        public ulong Samples, Errors, Retries;
        public ulong Generation;
        public uint Reason;
        public uint Engine;
        public uint RpmValidMask, DutyValidMask;
        public ulong Refusals;

        public const uint FlagValid = 1, FlagMonitoring = 2, FlagFresh = 4, FlagGated = 8,
                          FlagIdPinned = 16, FlagDutyProven = 32, FlagStopped = 64;
        public const uint SourceApu = 0x46;         // AMD TSI at SMBus 0x98: the APU die
    }
    // Reading the snapshot above. Kept out of the structure so that its layout stays a plain mirror.
    public static class Hwmon
    {
        public static bool Has(HwmonSnapshot h, uint flag) { return (h.Flags & flag) != 0; }
        // A reading means online AND inside the freshness window. Anything else is "no reading", never a guess.
        public static bool Reading(HwmonSnapshot h)
        { return Has(h, HwmonSnapshot.FlagValid) && Has(h, HwmonSnapshot.FlagFresh); }
        // The board has five tachometer channels and one fan on them, so the speed is the fastest that turns.
        public static uint Rpm(HwmonSnapshot h)
        {
            uint best = 0;
            if (h.Rpm != null) foreach (uint r in h.Rpm) if (r > best) best = r;
            return best;
        }
        // The duty READ-BACK, highest channel, as a percentage. Only meaningful with FlagDutyProven.
        public static uint DutyPercent(HwmonSnapshot h)
        {
            uint best = 0;
            if (h.DutyPermille != null) foreach (uint d in h.DutyPermille) if (d > best) best = d;
            return (best + 5) / 10;
        }
        // The chip's own reading of the APU die: an independent second measurement of what the SMU calls Tctl.
        public static double? ApuC(HwmonSnapshot h)
        {
            if (h.TemperatureSource == null || h.TemperatureMc == null) return null;
            for (int i = 0; i < h.TemperatureSource.Length && i < h.TemperatureMc.Length; i++)
                if (h.TemperatureSource[i] == HwmonSnapshot.SourceApu) return h.TemperatureMc[i] / 1000.0;
            return null;
        }
    }
    // What TelemetryProvider reads; Driver is the real one, the host test brings its own.
    public interface ITelemetrySource
    {
        DpmSnapshot ReadDpm();
        VideoMemorySnapshot ReadVideoMemory();
        ClockSnapshot ReadClock();
        HwmonSnapshot ReadHwmon();
    }
    // What OperatingPointProvider reads. All three are adapter-owned software snapshots answered with
    // NoAdapterSynchronization alone, so none idles GPU scheduling or touches a BAR.
    public interface ILabStateSource
    {
        CuModeSnapshot ReadCuMode();
        StartHealthSnapshot ReadStartHealthSnapshot();
        InteropSnapshot ReadInterop();
    }
    public sealed class Driver : IDisposable, ITelemetrySource, ILabStateSource
    {
        [DllImport("bc250control.dll", ExactSpelling = true, CallingConvention = CallingConvention.Winapi)]
        static extern int Bc250ClockControl(uint op, uint mhz, uint mv, out ClockSnapshot data, uint bytes);
        [DllImport("bc250control.dll", ExactSpelling = true, CallingConvention = CallingConvention.Winapi)]
        static extern int Bc250Dpm(out DpmSnapshot data, uint bytes);
        [DllImport("bc250control.dll", ExactSpelling = true, CallingConvention = CallingConvention.Winapi, CharSet = CharSet.Unicode)]
        static extern int Bc250VideoMemory(string hardwareId, out VideoMemorySnapshot data, uint bytes);
        [DllImport("bc250control.dll", ExactSpelling = true, CallingConvention = CallingConvention.Winapi)]
        static extern int Bc250StartHealth(uint op, ulong expectedGeneration, ulong expectedEpoch,
            out StartHealthSnapshot data, uint bytes);
        // Added to bc250control.dll after 2026-09-30. A deployed DLL older than that has no such export and the
        // first call throws EntryPointNotFoundException; OperatingPointProvider says so instead of going blind.
        [DllImport("bc250control.dll", ExactSpelling = true, CallingConvention = CallingConvention.Winapi)]
        static extern int Bc250CuMode(uint op, ulong expectedGeneration, out CuModeSnapshot data, uint bytes);
        // The control application's own interop read. READ only; there is no write operation at all.
        [DllImport("bc250control.dll", ExactSpelling = true, CallingConvention = CallingConvention.Winapi)]
        static extern int Bc250Interop(out InteropSnapshot data, uint bytes);
        // Added to bc250control.dll with KMD 0.7.212.1. A DLL older than that has no such export and the first
        // call throws EntryPointNotFoundException, which the fan row reports instead of going blind.
        [DllImport("bc250control.dll", ExactSpelling = true, CallingConvention = CallingConvention.Winapi)]
        static extern int Bc250Hwmon(out HwmonSnapshot data, uint bytes);

        internal static void ValidateStartHealthReply(int transport, StartHealthSnapshot data, uint op,
            ulong expectedGeneration, ulong expectedEpoch)
        {
            if (transport < 0 || data.Status != 0 || data.NtStatus != 0)
                throw new InvalidOperationException("KMD start health failed (transport 0x" + transport.ToString("X8") +
                    ", status " + data.Status + ", reason 0x" + data.NtStatus.ToString("X8") + ")");
            if (data.Magic != 0x30353242 || data.Command != 21 || data.AbiVersion != 1 || data.Op != op)
                throw new InvalidOperationException("KMD start health reply mismatch");
            if ((data.Flags & 8) != 0 && ((data.Flags & 1) == 0 || data.Generation == 0 || data.Epoch == 0))
                throw new InvalidOperationException("KMD confirmed reply has no full-WDDM identity");
            if (op == 1 && (expectedGeneration == 0 || expectedEpoch == 0 ||
                data.Generation != expectedGeneration || data.Epoch != expectedEpoch ||
                (data.Flags & 15) != 15))
                throw new InvalidOperationException("KMD confirmed another or unhealthy start");
        }
        static StartHealthSnapshot StartHealthRequest(uint op, ulong generation, ulong epoch)
        {
            StartHealthSnapshot data;
            uint bytes = (uint)Marshal.SizeOf(typeof(StartHealthSnapshot));
            if (bytes != 96) throw new InvalidOperationException("KMD start health layout mismatch");
            int status = Bc250StartHealth(op, generation, epoch, out data, bytes);
            ValidateStartHealthReply(status, data, op, generation, epoch);
            return data;
        }
        public static StartHealthSnapshot ReadStartHealth() { return StartHealthRequest(0, 0, 0); }
        public static StartHealthSnapshot ConfirmStartHealth(ulong generation, ulong epoch)
        { return StartHealthRequest(1, generation, epoch); }
        public StartHealthSnapshot ReadStartHealthSnapshot() { return ReadStartHealth(); }

        // BC250_ESCAPE_RUN_CU_MODE op READ (bc250kmd_cli.c, Bc250CuMode). READ only: the overlay reports the CU
        // mode, it never confirms one - a confirmation belongs to the operator's `cumode confirm`.
        public CuModeSnapshot ReadCuMode()
        {
            CuModeSnapshot data;
            uint bytes = (uint)Marshal.SizeOf(typeof(CuModeSnapshot));
            if (bytes != 184) throw new InvalidOperationException("KMD CU mode layout mismatch");
            int status = Bc250CuMode(0, 0, out data, bytes);
            if (status < 0)
                throw new InvalidOperationException("KMD CU mode snapshot unavailable (0x" + status.ToString("X8") + ")");
            if (data.Magic != 0x30353242 || data.Command != 22 || data.AbiVersion != 1 || data.Op != 0)
                throw new InvalidOperationException("KMD CU mode reply mismatch");
            return data;
        }

        // BC250_ESCAPE_RUN_INTEROP op READ (bc250kmd_cli.c, Bc250Interop): the start's decision, the session
        // marker it found and what it did about it. Software state, NoAdapterSynchronization alone, no BAR access.
        public InteropSnapshot ReadInterop()
        {
            InteropSnapshot data;
            uint bytes = (uint)Marshal.SizeOf(typeof(InteropSnapshot));
            if (bytes != 104) throw new InvalidOperationException("KMD interop layout mismatch");
            int status = Bc250Interop(out data, bytes);
            if (status < 0)
                throw new InvalidOperationException("KMD interop snapshot unavailable (0x" + status.ToString("X8") + ")");
            if (data.Magic != 0x30353242 || data.Command != 25 || data.AbiVersion != 1 || data.Op != 0)
                throw new InvalidOperationException("KMD interop reply mismatch");
            return data;
        }
        readonly object _lock = new object();
        static ClockSnapshot Request(uint op, uint mhz, uint mv)
        {
            ClockSnapshot data;
            int status = Bc250ClockControl(op, mhz, mv, out data, (uint)Marshal.SizeOf(typeof(ClockSnapshot)));
            if (status < 0) throw new InvalidOperationException("KMD clock unavailable (0x" + status.ToString("X8") + ")");
            if (data.AbiVersion != 1 || data.Ready != 1) throw new InvalidOperationException("KMD clock reply mismatch");
            return data;
        }
        public ClockSnapshot ReadClock() { lock (_lock) { return Request(0, 0, 0); } }
        public double ReadTemperature() { return ReadClock().TemperatureMc / 1000.0; }
        public ClockSnapshot SetClock(uint mhz, uint mv) { lock (_lock) { return Request(1, mhz, mv); } }

        // The governor's published snapshot: no SMU message, no BAR access (bc250kmd_cli.c, Bc250Dpm).
        public DpmSnapshot ReadDpm()
        {
            DpmSnapshot data;
            int status = Bc250Dpm(out data, 160);
            if (status < 0) throw new InvalidOperationException("KMD DPM snapshot unavailable (0x" + status.ToString("X8") + ")");
            return data;
        }
        // dxgkrnl's segment statistics of the BC-250 (bc250kmd_cli.c, Bc250VideoMemory).
        public VideoMemorySnapshot ReadVideoMemory()
        {
            VideoMemorySnapshot data;
            int status = Bc250VideoMemory(null, out data, 264);
            if (status < 0) throw new InvalidOperationException("segment statistics unavailable (0x" + status.ToString("X8") + ")");
            return data;
        }
        // BC250_ESCAPE_RUN_HWMON op READ (bc250kmd_cli.c, Bc250Hwmon): the board's hardware monitor as the
        // sampler last published it. READ is the only operation there is, and it touches no port.
        public HwmonSnapshot ReadHwmon()
        {
            HwmonSnapshot data;
            uint bytes = (uint)Marshal.SizeOf(typeof(HwmonSnapshot));
            if (bytes != 216) throw new InvalidOperationException("KMD hardware monitor layout mismatch");
            int status = Bc250Hwmon(out data, bytes);
            if (status < 0)
                throw new InvalidOperationException("KMD fan snapshot unavailable (0x" + status.ToString("X8") + ")");
            if (data.Magic != 0x30353242 || data.Command != 27 || data.AbiVersion != 1 || data.Op != 0)
                throw new InvalidOperationException("KMD fan reply mismatch");
            return data;
        }
        public static double MillivoltsFromVid(uint vid) { return 1550.0 - vid * 1000.0 / 160.0; }
        public void Dispose() { } // requests own and close their adapter handles
    }
}
