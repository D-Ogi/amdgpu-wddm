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
    public sealed class Driver : IDisposable
    {
        [DllImport("bc250control.dll", ExactSpelling = true, CallingConvention = CallingConvention.Winapi)]
        static extern int Bc250ClockControl(uint op, uint mhz, uint mv, out ClockSnapshot data, uint bytes);
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
