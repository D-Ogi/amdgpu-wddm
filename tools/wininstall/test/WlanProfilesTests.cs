// Synthetic WLAN API buffers only. Never enumerates this PC's interfaces or profiles.
using System;
using System.Collections.Generic;
using System.ComponentModel;
using System.Globalization;
using System.Runtime.InteropServices;
using System.Threading;
using AmdgpuWddm.WinInstall;

static class WlanProfilesTests
{
    static int checks;
    static void Check(bool ok, string message)
    {
        checks++;
        if (!ok) throw new Exception(message);
    }

    sealed class FakeApi : IWlanProfilesApi, IDisposable
    {
        internal string Name;
        internal uint OpenError, EnumError, ProfileError;
        internal bool NoInterfaces, NoProfiles, BadCount, NullInterfaces;
        internal int Closed, Freed, Reads;
        readonly HashSet<IntPtr> allocated = new HashSet<IntPtr>();

        IntPtr List<T>(params T[] values) where T : struct
        {
            int stride = Marshal.SizeOf(typeof(T));
            IntPtr p = Marshal.AllocHGlobal(8 + values.Length * stride);
            allocated.Add(p);
            Marshal.WriteInt32(p, values.Length);
            Marshal.WriteInt32(p, 4, 0); // dwIndex is not a pointer or an item count.
            for (int i = 0; i < values.Length; i++) Marshal.StructureToPtr(values[i], IntPtr.Add(p, 8 + i * stride), false);
            return p;
        }

        public uint Open(out IntPtr handle) { handle = OpenError == 0 ? new IntPtr(123) : IntPtr.Zero; return OpenError; }
        public uint Interfaces(IntPtr handle, out IntPtr list)
        {
            list = NullInterfaces ? IntPtr.Zero : NoInterfaces ? List<WlanProfiles.InterfaceInfo>() : List(
                new WlanProfiles.InterfaceInfo { Id = new Guid("00000000-0000-0000-0000-000000000001"), Description = "unused" },
                new WlanProfiles.InterfaceInfo { Id = new Guid("00000000-0000-0000-0000-000000000002"), Description = "translated label" });
            if (BadCount) Marshal.WriteInt32(list, -1);
            return EnumError;
        }
        public uint Profiles(IntPtr handle, ref Guid id, out IntPtr list)
        {
            Reads++;
            list = Reads == 1 || NoProfiles ? List<WlanProfiles.ProfileInfo>() : List(
                new WlanProfiles.ProfileInfo { Name = Name, Flags = 0 },
                new WlanProfiles.ProfileInfo { Name = "second-choice", Flags = 0 });
            return ProfileError;
        }
        public void Free(IntPtr list) { Check(allocated.Remove(list), "free exactly once"); Marshal.FreeHGlobal(list); Freed++; }
        public void Close(IntPtr handle) { Check(handle == new IntPtr(123), "close original handle"); Closed++; }
        public void Dispose() { foreach (var p in allocated) Marshal.FreeHGlobal(p); Check(allocated.Count == 0, "all native lists freed"); }
    }

    static void Error(FakeApi api, int code)
    {
        try { WlanProfiles.First(api); Check(false, "query failure must throw"); }
        catch (Win32Exception e) { Check(e.NativeErrorCode == code, "numeric failure is retained"); }
    }

    static int Main()
    {
        Check(Marshal.SizeOf(typeof(WlanProfiles.InterfaceInfo)) == 532, "WLAN_INTERFACE_INFO SDK layout");
        Check(Marshal.OffsetOf(typeof(WlanProfiles.InterfaceInfo), "State").ToInt32() == 528, "interface state SDK offset");
        Check(Marshal.SizeOf(typeof(WlanProfiles.ProfileInfo)) == 516, "WLAN_PROFILE_INFO SDK layout");
        Check(Marshal.OffsetOf(typeof(WlanProfiles.ProfileInfo), "Flags").ToInt32() == 512, "profile flags SDK offset");
        var previous = Thread.CurrentThread.CurrentCulture;
        var previousUi = Thread.CurrentThread.CurrentUICulture;
        try
        {
            foreach (var culture in new[] { "en-US", "pl-PL", "es-ES", "ja-JP", "ko-KR" })
            {
                Thread.CurrentThread.CurrentCulture = CultureInfo.GetCultureInfo(culture);
                Thread.CurrentThread.CurrentUICulture = CultureInfo.GetCultureInfo(culture);
                // Non-ASCII, punctuation and meaningful whitespace are data, not labels to trim.
                using (var api = new FakeApi { Name = "  Test-Zażółć-español-日本語-한국어:1  " })
                {
                    Check(WlanProfiles.First(api) == api.Name, culture + ": exact UTF-16 profile");
                    Check(api.Reads == 2 && api.Freed == 3 && api.Closed == 1, "empty first interface skipped; preferred profile returned; ownership closed");
                }
                using (var api = new FakeApi { NoInterfaces = true })
                    Check(WlanProfiles.First(api) == null && api.Reads == 0 && api.Closed == 1, "no interfaces is an empty reading");
                using (var api = new FakeApi { NoProfiles = true })
                    Check(WlanProfiles.First(api) == null && api.Freed == 3 && api.Closed == 1, "no profiles is an empty reading");
                using (var api = new FakeApi { OpenError = 5 }) { Error(api, 5); Check(api.Closed == 0, "failed open has no handle to close"); }
                using (var api = new FakeApi { EnumError = 5 }) { Error(api, 5); Check(api.Closed == 1 && api.Freed == 1, "enumeration failure releases returned allocation"); }
                using (var api = new FakeApi { ProfileError = 5 }) { Error(api, 5); Check(api.Closed == 1 && api.Freed == 2, "profile failure releases both lists"); }
                foreach (bool nullList in new[] { false, true })
                    using (var api = new FakeApi { BadCount = !nullList, NullInterfaces = nullList })
                    {
                        try { WlanProfiles.First(api); Check(false, "invalid native list must fail"); }
                        catch (InvalidOperationException) { Check(api.Closed == 1 && api.Reads == 0, "invalid list rejected before item read"); }
                    }
                Console.WriteLine(culture + ": synthetic WLAN query cases passed");
            }
        }
        finally { Thread.CurrentThread.CurrentCulture = previous; Thread.CurrentThread.CurrentUICulture = previousUi; }
        Console.WriteLine(checks + " WLAN checks passed; no native API calls");
        return 0;
    }
}
