// Numeric Win32 readings do not depend on Windows' performance-counter language.
// Contracts: GetSystemTimes (kernel includes idle), GlobalMemoryStatusEx (available physical bytes).
// https://learn.microsoft.com/windows/win32/api/processthreadsapi/nf-processthreadsapi-getsystemtimes
// https://learn.microsoft.com/windows/win32/api/sysinfoapi/nf-sysinfoapi-globalmemorystatusex
using System;
using System.ComponentModel;
using System.Runtime.InteropServices;

namespace Bc250Mon
{
    public struct SystemCpuTimes
    {
        public ulong Idle, Kernel, User;
        public SystemCpuTimes(ulong idle, ulong kernel, ulong user) { Idle = idle; Kernel = kernel; User = user; }
    }

    public static class SystemMetrics
    {
        [StructLayout(LayoutKind.Sequential)]
        struct MemoryStatus
        {
            public uint Length, Load;
            public ulong TotalPhysical, AvailablePhysical, TotalPageFile, AvailablePageFile;
            public ulong TotalVirtual, AvailableVirtual, AvailableExtendedVirtual;
        }

        [DllImport("kernel32.dll", SetLastError = true)]
        [return: MarshalAs(UnmanagedType.Bool)]
        static extern bool GetSystemTimes(out ulong idle, out ulong kernel, out ulong user);
        [DllImport("kernel32.dll", SetLastError = true)]
        [return: MarshalAs(UnmanagedType.Bool)]
        static extern bool GlobalMemoryStatusEx(ref MemoryStatus status);
        [DllImport("kernel32.dll")]
        public static extern ushort GetActiveProcessorGroupCount();

        public static SystemCpuTimes ReadCpu()
        {
            ulong idle, kernel, user;
            if (!GetSystemTimes(out idle, out kernel, out user)) throw new Win32Exception(Marshal.GetLastWin32Error());
            return new SystemCpuTimes(idle, kernel, user);
        }

        public static ulong AvailableMemoryMiB()
        {
            var status = new MemoryStatus { Length = (uint)Marshal.SizeOf(typeof(MemoryStatus)) };
            if (!GlobalMemoryStatusEx(ref status)) throw new Win32Exception(Marshal.GetLastWin32Error());
            return status.AvailablePhysical / (1024 * 1024);
        }

        public static double? CpuPercent(SystemCpuTimes before, SystemCpuTimes after)
        {
            if (after.Idle < before.Idle || after.Kernel < before.Kernel || after.User < before.User) return null;
            double total = (double)(after.Kernel - before.Kernel) + (after.User - before.User);
            double idle = after.Idle - before.Idle;
            if (total <= 0 || idle > total) return null;
            return 100 * (total - idle) / total;
        }
    }
}
