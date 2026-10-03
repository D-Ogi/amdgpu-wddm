// What is installed: the BC-250's display driver key (INF version, the user-mode drivers it names), the KMD service
// image, the Vulkan driver manifests and the D3D12 application profiles. Registry reads only, no administrator.
using System;
using System.Collections.Generic;
using System.Diagnostics;
using System.IO;
using System.Linq;
using System.Security.Cryptography;
using System.Text.RegularExpressions;
using Microsoft.Win32;

namespace AmdgpuWddmControl
{
    public sealed class Component
    {
        public string Role, Path, Version = "", Sha256 = "";
        public bool Exists;
    }

    public sealed class InventoryState
    {
        public bool AdapterFound, DevicePresent;
        public string ReleaseVersion = "", ReleaseDir = "", KmdImage = "";
        public string DeviceService = "";
        public uint DeviceProblem;
        public string AdapterName = "", DriverVersion = "", DriverDate = "", Provider = "", InfPath = "";
        public readonly List<Component> Components = new List<Component>();
        public readonly List<string> Notes = new List<string>();
    }

    public static class Inventory
    {
        public const string HardwareIdPrefix = @"pci\ven_1002&dev_13fe";
        const string DisplayClass = @"SYSTEM\CurrentControlSet\Control\Class\{4d36e968-e325-11ce-bfc1-08002be10318}";

        public static InventoryState Read(bool hashes)
        {
            var s = new InventoryState();
            try { ReadDisplayKey(s); } catch (Exception e) { s.Notes.Add("display class key: " + e.Message); }
            try { ReadService(s); } catch (Exception e) { s.Notes.Add("service key: " + e.Message); }
            try { ReadVulkan(s); } catch (Exception e) { s.Notes.Add("Vulkan drivers: " + e.Message); }
            try { ReadDevice(s); } catch (Exception e) { s.Notes.Add("device status: " + e.Message); }
            try
            {
                using (var k = Registry.LocalMachine.OpenSubKey(@"SOFTWARE\amdgpu-wddm\Release"))
                    if (k != null) { s.ReleaseVersion = Convert.ToString(k.GetValue("Version") ?? ""); s.ReleaseDir = Convert.ToString(k.GetValue("InstallDir") ?? ""); }
            }
            catch (Exception e) { s.Notes.Add("release key: " + e.Message); }
            foreach (var c in s.Components) Describe(c, hashes);
            return s;
        }

        static void ReadDisplayKey(InventoryState s)
        {
            using (var cls = Registry.LocalMachine.OpenSubKey(DisplayClass))
            {
                if (cls == null) return;
                foreach (var name in cls.GetSubKeyNames().Where(n => Regex.IsMatch(n, "^[0-9]{4}$")))
                {
                    RegistryKey key;
                    try { key = cls.OpenSubKey(name); } catch (System.Security.SecurityException) { continue; }
                    if (key == null) continue;
                    using (key)
                    {
                        var id = (key.GetValue("MatchingDeviceId") as string ?? "").ToLowerInvariant();
                        if (!id.StartsWith(HardwareIdPrefix)) continue;
                        s.AdapterFound = true;
                        s.AdapterName = key.GetValue("DriverDesc") as string ?? "";
                        s.DriverVersion = key.GetValue("DriverVersion") as string ?? "";
                        s.DriverDate = key.GetValue("DriverDate") as string ?? "";
                        s.Provider = key.GetValue("ProviderName") as string ?? "";
                        s.InfPath = key.GetValue("InfPath") as string ?? "";
                        foreach (var valueName in key.GetValueNames().OrderBy(n => n, StringComparer.OrdinalIgnoreCase))
                        {
                            if (!valueName.EndsWith("DriverName", StringComparison.OrdinalIgnoreCase) &&
                                !valueName.EndsWith("DriverNameWow", StringComparison.OrdinalIgnoreCase)) continue;
                            foreach (var file in Strings(key.GetValue(valueName)))
                                s.Components.Add(new Component { Role = valueName, Path = ResolveDriverFile(file, valueName.EndsWith("Wow", StringComparison.OrdinalIgnoreCase)) });
                        }
                        return;
                    }
                }
            }
        }

        // Device Manager's view of the BC-250: which driver service runs it and its problem code (Code 52: the
        // signature was refused, usually test signing is off; Code 43: the driver reported a failure).
        static void ReadDevice(InventoryState s)
        {
            using (var search = new System.Management.ManagementObjectSearcher(
                "SELECT Name, Service, ConfigManagerErrorCode FROM Win32_PnPEntity WHERE PNPDeviceID LIKE 'PCI\\\\VEN_1002&DEV_13FE%'"))
                foreach (var o in search.Get())
                    using (o)
                    {
                        s.DevicePresent = true;
                        s.DeviceService = Convert.ToString(o["Service"]);
                        s.DeviceProblem = Convert.ToUInt32(o["ConfigManagerErrorCode"] ?? 0u);
                        if (s.AdapterName.Length == 0) s.AdapterName = Convert.ToString(o["Name"]);
                    }
        }

        public static string DeviceProblemText(uint code)
        {
            switch (code)
            {
                case 0: return "working";
                case 22: return "disabled in Device Manager (Code 22)";
                case 28: return "no driver installed (Code 28)";
                case 31: return "Windows cannot load the driver (Code 31)";
                case 43: return "the driver reported a failure (Code 43)";
                case 52: return "the driver signature was refused (Code 52): turn on test signing";
                default: return "problem Code " + code;
            }
        }

        static void ReadService(InventoryState s)
        {
            using (var key = Registry.LocalMachine.OpenSubKey(@"SYSTEM\CurrentControlSet\Services\bc250kmd"))
            {
                if (key == null) { s.Notes.Add("The bc250kmd service is not installed."); return; }
                var image = key.GetValue("ImagePath", null, RegistryValueOptions.DoNotExpandEnvironmentNames) as string;
                if (string.IsNullOrEmpty(image)) return;
                s.KmdImage = ResolveImagePath(image);
                s.Components.Insert(0, new Component { Role = "Kernel-mode driver", Path = s.KmdImage });
            }
        }

        static void ReadVulkan(InventoryState s)
        {
            foreach (var path in new[] { @"SOFTWARE\Khronos\Vulkan\Drivers", @"SOFTWARE\WOW6432Node\Khronos\Vulkan\Drivers" })
                using (var key = Registry.LocalMachine.OpenSubKey(path))
                {
                    if (key == null) continue;
                    foreach (var json in key.GetValueNames())
                    {
                        if (json.IndexOf("amdgpu", StringComparison.OrdinalIgnoreCase) < 0 &&
                            json.IndexOf("bc250", StringComparison.OrdinalIgnoreCase) < 0 &&
                            json.IndexOf("radv", StringComparison.OrdinalIgnoreCase) < 0) continue;
                        s.Components.Add(new Component { Role = path.Contains("WOW6432") ? "Vulkan driver manifest (32-bit)" : "Vulkan driver manifest", Path = json });
                        var library = ManifestLibrary(json);
                        if (library != null) s.Components.Add(new Component { Role = "Vulkan driver", Path = library });
                    }
                }
        }

        // "library_path" of a Vulkan ICD manifest, relative to the manifest when not absolute.
        public static string ManifestLibrary(string jsonPath)
        {
            try
            {
                var text = File.ReadAllText(jsonPath);
                var m = Regex.Match(text, "\"library_path\"\\s*:\\s*\"((?:[^\"\\\\]|\\\\.)*)\"");
                if (!m.Success) return null;
                var lib = Regex.Unescape(m.Groups[1].Value);
                return Path.IsPathRooted(lib) ? lib : Path.GetFullPath(Path.Combine(Path.GetDirectoryName(jsonPath), lib));
            }
            catch (Exception) { return null; }
        }

        static IEnumerable<string> Strings(object value)
        {
            var one = value as string;
            if (one != null) { if (one.Length > 0) yield return one; yield break; }
            var many = value as string[];
            if (many != null) foreach (var m in many) if (!string.IsNullOrEmpty(m)) yield return m;
        }

        // UMD names are plain file names loaded from System32 (SysWOW64 for the Wow values) or full paths into the
        // driver store.
        public static string ResolveDriverFile(string file, bool wow)
        {
            if (Path.IsPathRooted(file)) return file;
            var windows = Environment.GetFolderPath(Environment.SpecialFolder.Windows);
            return Path.Combine(windows, wow ? "SysWOW64" : "System32", file);
        }

        public static string ResolveImagePath(string image)
        {
            var windows = Environment.GetFolderPath(Environment.SpecialFolder.Windows);
            if (image.StartsWith(@"\SystemRoot\", StringComparison.OrdinalIgnoreCase)) return Path.Combine(windows, image.Substring(12));
            if (image.StartsWith(@"System32\", StringComparison.OrdinalIgnoreCase)) return Path.Combine(windows, image);
            if (image.StartsWith(@"\??\")) return image.Substring(4);
            return Environment.ExpandEnvironmentVariables(image);
        }

        static void Describe(Component c, bool hash)
        {
            try
            {
                c.Exists = File.Exists(c.Path);
                if (!c.Exists) return;
                var info = FileVersionInfo.GetVersionInfo(c.Path);
                c.Version = info.FileVersion ?? "";
                if (hash) c.Sha256 = Sha256(c.Path);
            }
            catch (Exception e) { c.Version = "unreadable: " + e.Message; }
        }

        public static string Sha256(string path)
        {
            using (var sha = SHA256.Create())
            using (var f = File.OpenRead(path))
                return BitConverter.ToString(sha.ComputeHash(f)).Replace("-", "");
        }

        public static string Report(InventoryState s)
        {
            var w = new System.Text.StringBuilder();
            w.AppendLine("device present: " + s.DevicePresent + (s.DevicePresent ? ", service " + s.DeviceService + ", " + DeviceProblemText(s.DeviceProblem) : ""));
            w.AppendLine("release: " + s.ReleaseVersion + " in " + s.ReleaseDir);
            w.AppendLine("adapter found: " + s.AdapterFound);
            w.AppendLine("adapter name: " + s.AdapterName);
            w.AppendLine("driver version (INF): " + s.DriverVersion);
            w.AppendLine("driver date: " + s.DriverDate);
            w.AppendLine("provider: " + s.Provider);
            w.AppendLine("inf: " + s.InfPath);
            foreach (var c in s.Components)
                w.AppendLine(string.Format("{0} | {1} | exists {2} | version {3} | sha256 {4}", c.Role, c.Path, c.Exists, c.Version, c.Sha256));
            foreach (var n in s.Notes) w.AppendLine("note: " + n);
            return w.ToString();
        }
    }

    // Registry I/O of the two settings pages. Writes need an administrator: the UI runs them in an elevated copy of
    // this program (Program.RunElevated), which calls the same methods.
    public static class SettingsStore
    {
        public static uint? ReadDword(string path, string name)
        {
            using (var key = Registry.LocalMachine.OpenSubKey(path))
            {
                var v = key == null ? null : key.GetValue(name);
                return v is int ? (uint?)(uint)(int)v : null;
            }
        }

        public static Dictionary<string, string> ReadAll(string path)
        {
            var d = new Dictionary<string, string>();
            using (var key = Registry.LocalMachine.OpenSubKey(path))
                if (key != null)
                    foreach (var n in key.GetValueNames())
                    {
                        var v = key.GetValue(n);
                        d[n] = v is byte[] ? BitConverter.ToString((byte[])v) : v is string[] ? string.Join(";", (string[])v) : Convert.ToString(v);
                    }
            return d;
        }

        public static SortedDictionary<string, string> ReadProfiles()
        {
            var d = new SortedDictionary<string, string>(StringComparer.OrdinalIgnoreCase);
            using (var root = Registry.LocalMachine.OpenSubKey(Profiles.RegistryPath))
                if (root != null)
                    foreach (var image in root.GetSubKeyNames())
                        using (var k = root.OpenSubKey(image))
                            d[image] = k == null ? "" : k.GetValue(Profiles.ValueName) as string ?? "";
            return d;
        }

        public static void WriteProfile(string image, string value)
        {
            if (!Profiles.IsValidImage(image)) throw new ArgumentException("Not an application file name: " + image);
            if (!Profiles.IsValidValue(value) || value.Length == 0) throw new ArgumentException("The switch list is empty or outside the driver's syntax.");
            using (var key = Registry.LocalMachine.CreateSubKey(Profiles.RegistryPath + "\\" + image))
                key.SetValue(Profiles.ValueName, value, RegistryValueKind.String);
            using (var key = Registry.LocalMachine.OpenSubKey(Profiles.RegistryPath + "\\" + image))
                if (key == null || (key.GetValue(Profiles.ValueName) as string) != value) throw new InvalidOperationException("The profile did not read back as written.");
        }

        public static void RemoveProfile(string image)
        {
            if (!Profiles.IsValidImage(image)) throw new ArgumentException("Not an application file name: " + image);
            using (var root = Registry.LocalMachine.OpenSubKey(Profiles.RegistryPath, true))
                if (root != null) root.DeleteSubKeyTree(image, false);
        }
    }
}
