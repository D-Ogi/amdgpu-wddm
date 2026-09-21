// Thin wrapper over bc250rd.sys (tools/win/bc250rd). Mirrors bc250rd_ioctl.h; keep the two in step.
using System;
using System.ComponentModel;
using System.Runtime.InteropServices;
using Microsoft.Win32.SafeHandles;

namespace Bc250Mon
{
    public sealed class Driver : IDisposable
    {
        const uint GENERIC_READ = 0x80000000, GENERIC_WRITE = 0x40000000, OPEN_EXISTING = 3;
        const uint IOCTL_ATTACH = 0x00226000, IOCTL_SMN_READ = 0x00226008, IOCTL_SMU_MSG = 0x0022E00C;
        const uint SMN_THM_TCON_CUR_TMP = 0x00059800;

        public const uint MsgRequestGfxclk = 0x0E, MsgGetGfxFrequency = 0x37, MsgGetGfxVid = 0x38,
                          MsgForceGfxVid = 0x3B, MsgUnforceGfxVid = 0x3C;

        [DllImport("kernel32.dll", SetLastError = true, CharSet = CharSet.Unicode)]
        static extern SafeFileHandle CreateFile(string name, uint access, uint share, IntPtr sa, uint disposition, uint flags, IntPtr template);

        [DllImport("kernel32.dll", SetLastError = true)]
        static extern bool DeviceIoControl(SafeFileHandle h, uint code, byte[] inBuf, int inLen, byte[] outBuf, int outLen, out int returned, IntPtr overlapped);

        readonly object _lock = new object();
        SafeFileHandle _h;

        void EnsureOpen()
        {
            if (_h != null && !_h.IsInvalid) return;
            _h = CreateFile(@"\\.\Bc250Rd", GENERIC_READ | GENERIC_WRITE, 0, IntPtr.Zero, OPEN_EXISTING, 0, IntPtr.Zero);
            if (_h.IsInvalid) throw new Win32Exception(Marshal.GetLastWin32Error(), "cannot open bc250rd (driver loaded? elevated?)");
            var info = new byte[64];
            int got;
            if (!DeviceIoControl(_h, IOCTL_ATTACH, null, 0, info, info.Length, out got, IntPtr.Zero))
            {
                int err = Marshal.GetLastWin32Error();
                _h.Dispose(); _h = null;
                throw new Win32Exception(err, "bc250rd attach failed");
            }
        }

        public double ReadTemperature()
        {
            lock (_lock)
            {
                EnsureOpen();
                var buf = BitConverter.GetBytes(SMN_THM_TCON_CUR_TMP);
                int got;
                if (!DeviceIoControl(_h, IOCTL_SMN_READ, buf, 4, buf, 4, out got, IntPtr.Zero))
                    throw new Win32Exception(Marshal.GetLastWin32Error(), "temperature read failed");
                uint v = BitConverter.ToUInt32(buf, 0);
                return (v >> 21) * 0.125 - ((v & (1u << 19)) != 0 ? 49.0 : 0.0);
            }
        }

        // Returns the value register; throws unless the SMU answered OK. The driver enforces the allow-list.
        public uint Smu(uint message, uint parameter)
        {
            lock (_lock)
            {
                EnsureOpen();
                var buf = new byte[16];
                BitConverter.GetBytes(message).CopyTo(buf, 0);
                BitConverter.GetBytes(parameter).CopyTo(buf, 4);
                int got;
                if (!DeviceIoControl(_h, IOCTL_SMU_MSG, buf, 16, buf, 16, out got, IntPtr.Zero))
                    throw new Win32Exception(Marshal.GetLastWin32Error(), "SMU message refused by the driver");
                uint response = BitConverter.ToUInt32(buf, 8);
                if (response != 1) throw new InvalidOperationException(string.Format("SMU message 0x{0:X} answered 0x{1:X}", message, response));
                return BitConverter.ToUInt32(buf, 12);
            }
        }

        public static uint VidFromMillivolts(uint mv) { return (1550u - mv) * 160u / 1000u; }
        public static double MillivoltsFromVid(uint vid) { return 1550.0 - vid * 1000.0 / 160.0; }

        public void Dispose() { if (_h != null) _h.Dispose(); }
    }
}
