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
    public sealed class Driver : IDisposable
    {
        [DllImport("bc250control.dll", ExactSpelling = true, CallingConvention = CallingConvention.Winapi)]
        static extern int Bc250ClockControl(uint op, uint mhz, uint mv, out ClockSnapshot data, uint bytes);
        [DllImport("bc250control.dll", ExactSpelling = true, CallingConvention = CallingConvention.Winapi)]
        static extern int Bc250StartHealth(uint op, ulong expectedGeneration, ulong expectedEpoch,
            out StartHealthSnapshot data, uint bytes);

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
        public static double MillivoltsFromVid(uint vid) { return 1550.0 - vid * 1000.0 / 160.0; }
        public void Dispose() { } // requests own and close their adapter handles
    }
}
