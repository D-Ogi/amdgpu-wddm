// WLAN API declarations and layouts: Windows SDK 10.0.26100.0, wlanapi.h.
// https://learn.microsoft.com/windows/win32/api/wlanapi/nf-wlanapi-wlangetprofilelist
// Query profile names as Unicode data, never by parsing netsh's translated display text.
using System;
using System.ComponentModel;
using System.Runtime.InteropServices;

namespace AmdgpuWddm.WinInstall
{
    internal interface IWlanProfilesApi
    {
        uint Open(out IntPtr handle);
        uint Interfaces(IntPtr handle, out IntPtr list);
        uint Profiles(IntPtr handle, ref Guid id, out IntPtr list);
        void Free(IntPtr list);
        void Close(IntPtr handle);
    }

    public static class WlanProfiles
    {
        [StructLayout(LayoutKind.Sequential, CharSet = CharSet.Unicode)]
        internal struct InterfaceInfo
        {
            public Guid Id;
            [MarshalAs(UnmanagedType.ByValTStr, SizeConst = 256)] public string Description;
            public uint State;
        }

        [StructLayout(LayoutKind.Sequential, CharSet = CharSet.Unicode)]
        internal struct ProfileInfo
        {
            [MarshalAs(UnmanagedType.ByValTStr, SizeConst = 256)] public string Name;
            public uint Flags;
        }

        static uint Count(IntPtr list)
        {
            if (list == IntPtr.Zero) throw new InvalidOperationException("WLAN returned no list.");
            uint count = unchecked((uint)Marshal.ReadInt32(list));
            // Bound arithmetic and enumeration even if a provider returns a damaged header.
            if (count > 65536) throw new InvalidOperationException("WLAN returned an invalid list length.");
            return count;
        }

        static void RequireSuccess(uint result)
        {
            if (result != 0) throw new Win32Exception(unchecked((int)result));
        }

        // The watchdog has one configured profile in the lab. If more exist, use the first
        // nonempty interface's highest-preference profile, preserving the old selection policy.
        // Null means no profile; query failure is an exception, not an empty successful reading.
        public static string First() { return First(new NativeApi()); }

        internal static string First(IWlanProfilesApi api)
        {
            IntPtr handle;
            RequireSuccess(api.Open(out handle));
            if (handle == IntPtr.Zero) throw new InvalidOperationException("WLAN returned no handle.");
            try
            {
                IntPtr interfaces = IntPtr.Zero;
                try
                {
                    RequireSuccess(api.Interfaces(handle, out interfaces));
                    uint count = Count(interfaces);
                    int stride = Marshal.SizeOf(typeof(InterfaceInfo));
                    for (uint i = 0; i < count; i++)
                    {
                        var item = (InterfaceInfo)Marshal.PtrToStructure(
                            IntPtr.Add(interfaces, checked(8 + (int)i * stride)), typeof(InterfaceInfo));
                        IntPtr profiles = IntPtr.Zero;
                        try
                        {
                            RequireSuccess(api.Profiles(handle, ref item.Id, out profiles));
                            if (Count(profiles) == 0) continue;
                            var profile = (ProfileInfo)Marshal.PtrToStructure(IntPtr.Add(profiles, 8), typeof(ProfileInfo));
                            if (string.IsNullOrEmpty(profile.Name)) throw new InvalidOperationException("WLAN returned an empty profile name.");
                            return profile.Name;
                        }
                        finally { if (profiles != IntPtr.Zero) api.Free(profiles); }
                    }
                    return null;
                }
                finally { if (interfaces != IntPtr.Zero) api.Free(interfaces); }
            }
            finally { api.Close(handle); }
        }

        sealed class NativeApi : IWlanProfilesApi
        {
            [DllImport("wlanapi.dll", ExactSpelling = true)]
            static extern uint WlanOpenHandle(uint version, IntPtr reserved, out uint negotiated, out IntPtr handle);
            [DllImport("wlanapi.dll", ExactSpelling = true)]
            static extern uint WlanEnumInterfaces(IntPtr handle, IntPtr reserved, out IntPtr list);
            [DllImport("wlanapi.dll", ExactSpelling = true)]
            static extern uint WlanGetProfileList(IntPtr handle, ref Guid id, IntPtr reserved, out IntPtr list);
            [DllImport("wlanapi.dll", ExactSpelling = true)]
            static extern void WlanFreeMemory(IntPtr list);
            [DllImport("wlanapi.dll", ExactSpelling = true)]
            static extern uint WlanCloseHandle(IntPtr handle, IntPtr reserved);

            public uint Open(out IntPtr handle) { uint version; return WlanOpenHandle(2, IntPtr.Zero, out version, out handle); }
            public uint Interfaces(IntPtr handle, out IntPtr list) { return WlanEnumInterfaces(handle, IntPtr.Zero, out list); }
            public uint Profiles(IntPtr handle, ref Guid id, out IntPtr list) { return WlanGetProfileList(handle, ref id, IntPtr.Zero, out list); }
            public void Free(IntPtr list) { WlanFreeMemory(list); }
            public void Close(IntPtr handle) { WlanCloseHandle(handle, IntPtr.Zero); }
        }
    }
}
