// The desktop route's kill switch DwmForceCpu, and its one writer (BD-296).
//
// The switch has two copies, and both are REG_DWORD values with the same meaning: 0 or absent is the GPU route,
// any other number is the CPU route.
//   HKLM\SOFTWARE\amdgpu-wddm\DesktopRouter DwmForceCpu
//       The router's copy (driver/umd/router/router.cpp). The router reads it in every hosted process, dwm.exe
//       included. A value of another type also sends the desktop to the CPU route (fail safe).
//   HKLM\SYSTEM\CurrentControlSet\Services\bc250kmd\Parameters DwmForceCpu
//       The driver's copy (driver/kmd/guard.c, GuardReadDesktopKillSwitch). The KMD reads it once, in DriverEntry,
//       through IoOpenDriverRegistryKey: Driver Verifier's isolation rules (33 and 36) do not let a driver open the
//       SOFTWARE hive by an absolute path. With the switch on, the KMD reports no GDI hardware acceleration, because
//       the CPU UMD that composes the desktop then cannot open a GDI surface that has no CPU view.
//
// Write is the only code that writes either copy. It writes both in one call, the router's copy first, and reads
// both back. If the second write fails, it puts the first copy back as it was, so a failure leaves the two copies as
// they were before the call. The control application's route actions (desktop-cpu, desktop-gpu, reset-defaults,
// undo), the rollback of a failed action and the installer all call it. The uninstaller removes the driver's copy
// in the same step that removes the router's key.
//
// A person can also set the switch by hand, for example from a recovery console or the lab's emergency channel,
// where this code is not available. The manual recovery writes both values with reg.exe (docs/testing/INSTALL.md).
// If only the router's copy is set, the desktop goes to the CPU route all the same, and the driver reports GDI
// hardware acceleration as before BD-296. The control application shows both copies and flags a difference.
//
// The installer compiles this file with Add-Type (Windows PowerShell 5.1, C# 5), so the code uses no language
// feature that is newer than C# 5 and no type of the control application.
using System;
using Microsoft.Win32;

namespace AmdgpuWddm
{
    public static class RouteSwitch
    {
        public const string RouterPath = @"SOFTWARE\amdgpu-wddm\DesktopRouter";
        public const string DriverPath = @"SYSTEM\CurrentControlSet\Services\bc250kmd\Parameters";
        public const string ValueName = "DwmForceCpu";

        // The two paths, the router's copy first: the order in which Write writes them.
        public static readonly string[] Paths = { RouterPath, DriverPath };

        // True for the two values of the switch, and for nothing else.
        public static bool Owns(string path, string name)
        {
            return name == ValueName && (string.Equals(path, RouterPath, StringComparison.OrdinalIgnoreCase) ||
                string.Equals(path, DriverPath, StringComparison.OrdinalIgnoreCase));
        }

        public static bool IsDriverCopy(string path)
        {
            return string.Equals(path, DriverPath, StringComparison.OrdinalIgnoreCase);
        }

        // One copy as found: absent, a DWORD, or a value of another type (Kind names it).
        public sealed class Copy
        {
            public bool Present;
            public bool Dword;
            public uint Number;
            public string Kind;

            // The route this copy selects, as the router reads it: absent and 0 are the GPU route, every other
            // DWORD and every other type are the CPU route.
            public bool Cpu { get { return Present && (!Dword || Number != 0); } }

            // The value Write would write to keep this route: null for absent, the DWORD itself, 1 for another type.
            public uint? Value { get { return !Present ? (uint?)null : Dword ? Number : 1u; } }

            public override string ToString()
            {
                if (!Present) return "absent";
                return Dword ? Number.ToString(System.Globalization.CultureInfo.InvariantCulture) : "a value of type " + Kind;
            }
        }

        // Both copies as found. DriverKey is false when the driver's Parameters key does not exist.
        public sealed class Copies
        {
            public Copy Router, Driver;
            public bool RouterKey, DriverKey;

            // The two copies differ: not the same value, or one present and the other not.
            public bool Mismatch
            {
                get { return Router.Present != Driver.Present || Router.Dword != Driver.Dword || (Router.Dword && Router.Number != Driver.Number); }
            }
        }

        public static Copy ReadCopy(RegistryKey key)
        {
            var c = new Copy();
            if (key == null) return c;
            object v = key.GetValue(ValueName, null, RegistryValueOptions.DoNotExpandEnvironmentNames);
            if (v == null) return c;
            c.Present = true;
            RegistryValueKind kind = key.GetValueKind(ValueName);
            c.Kind = kind.ToString();
            c.Dword = kind == RegistryValueKind.DWord;
            if (c.Dword) c.Number = unchecked((uint)(int)v);
            return c;
        }

        // hklm: the 64-bit view of HKEY_LOCAL_MACHINE (or a test root with the same two paths below it).
        public static Copies Read(RegistryKey hklm)
        {
            var r = new Copies();
            using (var router = hklm.OpenSubKey(RouterPath))
            using (var driver = hklm.OpenSubKey(DriverPath))
            {
                r.RouterKey = router != null;
                r.DriverKey = driver != null;
                r.Router = ReadCopy(router);
                r.Driver = ReadCopy(driver);
            }
            return r;
        }

        static void Put(RegistryKey key, uint? value)
        {
            if (value == null) key.DeleteValue(ValueName, false);
            else key.SetValue(ValueName, unchecked((int)value.Value), RegistryValueKind.DWord);
        }

        // Puts one copy back as it was found. A value of another type that Write replaced is put back as its route
        // (DWORD 1): this code writes DWORDs only.
        static void Restore(RegistryKey key, Copy before)
        {
            Put(key, before.Value);
        }

        static bool Holds(RegistryKey key, uint? value)
        {
            var c = ReadCopy(key);
            return value == null ? !c.Present : c.Present && c.Dword && c.Number == value.Value;
        }

        // Writes value (null: removes it) into both copies, reads both back, and returns a line for the log. Both
        // keys must exist: the router's key is the release's, and the driver's Parameters key is the driver
        // service's. A missing key is refused before anything is written. A failure throws, and leaves both copies
        // as they were.
        public static string Write(RegistryKey hklm, uint? value)
        {
            using (var router = hklm.OpenSubKey(RouterPath, true))
            using (var driver = hklm.OpenSubKey(DriverPath, true))
            {
                if (router == null) throw new InvalidOperationException(@"HKLM\" + RouterPath + " does not exist, so neither copy of " + ValueName + " was written");
                if (driver == null) throw new InvalidOperationException(@"HKLM\" + DriverPath + " does not exist (the driver service is not installed), so neither copy of " + ValueName + " was written");
                Copy routerBefore = ReadCopy(router), driverBefore = ReadCopy(driver);
                Put(router, value);
                try
                {
                    Put(driver, value);
                    if (!Holds(router, value)) throw new InvalidOperationException("the router's copy did not read back");
                    if (!Holds(driver, value)) throw new InvalidOperationException("the driver's copy did not read back");
                }
                catch (Exception e)
                {
                    string undo = "both copies were put back";
                    try { Restore(driver, driverBefore); Restore(router, routerBefore); }
                    catch (Exception again) { undo = "putting the copies back failed too: " + again.Message; }
                    throw new InvalidOperationException(ValueName + " was not written: " + e.Message + "; " + undo, e);
                }
                string now = value == null ? "removed" : value.Value.ToString(System.Globalization.CultureInfo.InvariantCulture);
                return ValueName + " " + now + " in both copies (router " + routerBefore + ", driver " + driverBefore + " before), read back";
            }
        }
    }
}
