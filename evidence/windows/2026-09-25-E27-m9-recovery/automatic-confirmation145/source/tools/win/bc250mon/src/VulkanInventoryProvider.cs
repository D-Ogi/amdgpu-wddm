// Cached Vulkan queries are separate from DWM's live UMD. Never initialize an ICD here.
using System;
using System.Globalization;
using System.IO;
using System.Linq;
using System.Web.Script.Serialization;

namespace Bc250Mon
{
    public sealed class VulkanInventory
    {
        public int SchemaVersion { get; set; }
        public string Status { get; set; }
        public string CapturedUtc { get; set; }
        public string Collector { get; set; }
        public string ToolSha256 { get; set; }
        public string IcdPath { get; set; }
        public string IcdSha256 { get; set; } // Manifest hash, not library hash.
        public string IcdLibraryPath { get; set; }
        public string IcdLibrarySha256 { get; set; }
        public bool? IcdVerified { get; set; }
        public string LoaderEvidence { get; set; }
        public string KmdVersion { get; set; }
        public int? DeviceCount { get; set; }
        public DeviceInfo Device { get; set; }
        public int? InstanceExtensionCount { get; set; }
        public int? DeviceExtensionCount { get; set; }
        public int? LayerCount { get; set; }
        public string[] Layers { get; set; }
        public FormatInfo Formats { get; set; }
        public FeatureInfo[] Features { get; set; }
        public string DetailsPath { get; set; }
        public string Error { get; set; }
        public sealed class DeviceInfo
        {
            public string Name { get; set; }
            public string ApiVersion { get; set; }
            public string DriverName { get; set; }
            public string DriverInfo { get; set; }
            public string DriverVersion { get; set; }
            public string DeviceType { get; set; }
        }
        public sealed class FormatInfo
        {
            public int? EnumeratedCount { get; set; }
            public int? SupportedCount { get; set; }
            public int? StorageImageCount { get; set; }
            public int? ColorAttachmentCount { get; set; }
        }
        public sealed class FeatureInfo
        {
            public string Name { get; set; }
            public bool? Supported { get; set; }
        }
    }
    public sealed class VulkanInventoryProvider : IProvider
    {
        readonly string _path;
        DateTime _write;
        long _length = -1;
        VulkanInventory _inventory;
        string _error;
        public VulkanInventoryProvider(string dataDir)
        {
            string configured = Environment.GetEnvironmentVariable("BC250MON_VULKAN_INVENTORY");
            _path = string.IsNullOrWhiteSpace(configured) ? Path.Combine(dataDir, "vulkan-inventory.json") :
                Path.IsPathRooted(configured) ? configured : Path.Combine(dataDir, configured);
        }
        public string Name { get { return "vulkan"; } }
        public TimeSpan Period { get { return TimeSpan.FromSeconds(5); } }
        public void Poll(State state)
        {
            try
            {
                var file = new FileInfo(_path);
                if (!file.Exists) { _inventory = null; _length = -1; _error = "not captured"; }
                else if (file.LastWriteTimeUtc != _write || file.Length != _length)
                {
                    // Discard previous success before reading a replacement.
                    _inventory = null; _error = null;
                    if (file.Length > 262144) throw new InvalidDataException("inventory exceeds 256 KiB");
                    _inventory = Parse(File.ReadAllText(_path));
                    _write = file.LastWriteTimeUtc; _length = file.Length;
                }
            }
            catch (Exception e) { _inventory = null; _length = -1; _error = "cannot read inventory: " + e.Message; }
            var kmd = state.Take(0).Panels.FirstOrDefault(p => p.Name == "kmdinfo");
            var version = kmd == null ? null : kmd.Rows.FirstOrDefault(r => r.Label == "Version");
            state.SetPanel(BuildPanel(_inventory, _error, DateTime.UtcNow,
                version == null ? null : version.Value.Split(' ')[0]));
        }
        public static VulkanInventory Parse(string json)
        {
            var inventory = new JavaScriptSerializer { MaxJsonLength = 262144 }.Deserialize<VulkanInventory>(json);
            if (inventory == null || inventory.SchemaVersion != 1)
                throw new InvalidDataException("unsupported inventory schema");
            if (inventory.Status != "ok" && inventory.Status != "partial" && inventory.Status != "error" && inventory.Status != "unsupported")
                throw new InvalidDataException("unknown capture status");
            DateTimeOffset captured;
            if (string.IsNullOrWhiteSpace(inventory.CapturedUtc) ||
                !DateTimeOffset.TryParse(inventory.CapturedUtc, CultureInfo.InvariantCulture,
                    DateTimeStyles.None, out captured) || captured.Offset != TimeSpan.Zero)
                throw new InvalidDataException("capture timestamp must be UTC");
            if ((inventory.Status == "ok" || inventory.Status == "partial") && (inventory.Device == null ||
                string.IsNullOrWhiteSpace(inventory.Device.Name) || string.IsNullOrWhiteSpace(inventory.IcdPath)))
                throw new InvalidDataException("successful inventory needs a device and explicit ICD manifest");
            return inventory;
        }
        static string Known(string text) { return string.IsNullOrWhiteSpace(text) ? "not captured" : text; }
        static string Count(int? count) { return count.HasValue && count.Value >= 0 ? count.Value.ToString(CultureInfo.InvariantCulture) : "?"; }
        static string Feature(VulkanInventory inventory, string name, string label)
        {
            var feature = (inventory.Features ?? new VulkanInventory.FeatureInfo[0]).FirstOrDefault(f => f != null && f.Name == name);
            return label + ":" + (feature == null || !feature.Supported.HasValue ? "?" : feature.Supported.Value ? "yes" : "no");
        }
        static bool KmdVersionsDiffer(string captured, string current)
        {
            Version a, b;
            if (Version.TryParse(captured, out a) && Version.TryParse(current, out b)) return a != b;
            // GET_INFO reports milestone in high 16 bits, revision in low 16
            // (bc250kmd_cli.c's version line). DriverVer is 0.milestone.revision.1;
            // the final package field is not exposed by this DDI.
            uint packed;
            if (Version.TryParse(captured, out a) && a.Major == 0 && a.Build >= 0 &&
                a.Minor <= 65535 && a.Build <= 65535 && current != null && current.StartsWith("0x") &&
                uint.TryParse(current.Substring(2), NumberStyles.HexNumber, CultureInfo.InvariantCulture, out packed))
                return packed != (((uint)a.Minor << 16) | (uint)a.Build);
            uint other;
            if (captured != null && current != null && captured.StartsWith("0x") && current.StartsWith("0x") &&
                uint.TryParse(captured.Substring(2), NumberStyles.HexNumber, CultureInfo.InvariantCulture, out packed) &&
                uint.TryParse(current.Substring(2), NumberStyles.HexNumber, CultureInfo.InvariantCulture, out other))
                return packed != other;
            return false; // Incomparable metadata does not establish a change.
        }
        public static Panel BuildPanel(VulkanInventory inventory, string error, DateTime nowUtc, string currentKmd)
        {
            var panel = new Panel { Name = "vulkan", Title = "Vulkan inventory (cached)", Order = 13 };
            if (inventory == null) { panel.Rows.Add(new Row("Capture", Known(error), Level.Warn)); return panel; }
            DateTimeOffset captured = DateTimeOffset.Parse(inventory.CapturedUtc, CultureInfo.InvariantCulture);
            TimeSpan age = nowUtc - captured.UtcDateTime;
            string elapsed = age < TimeSpan.Zero ? "clock ahead" : age.TotalHours >= 1 ?
                ((int)age.TotalHours) + "h " + age.Minutes + "m ago" : ((int)age.TotalMinutes) + "m " + age.Seconds + "s ago";
            bool changedKmd = KmdVersionsDiffer(inventory.KmdVersion, currentKmd);
            panel.Rows.Add(new Row("Capture", captured.ToString("MM-dd HH:mm:ss 'UTC'") + "; " + elapsed +
                (changedKmd ? "; KMD changed" : ""), changedKmd || age.TotalHours >= 24 || age.TotalMinutes < -2 ? Level.Warn : Level.Info));
            if (inventory.Status != "ok" && inventory.Status != "partial")
            {
                panel.Rows.Add(new Row("Result", inventory.Status + ": " + Known(inventory.Error), inventory.Status == "error" ? Level.Error : Level.Warn));
                panel.Rows.Add(new Row("ICD requested", Known(inventory.IcdPath)));
                panel.Rows.Add(new Row("Full report", Known(inventory.DetailsPath)));
                return panel;
            }
            if (inventory.Status == "partial")
                panel.Rows.Add(new Row("Partial capture", Known(inventory.Error), Level.Warn));
            var device = inventory.Device;
            panel.Rows.Add(new Row("Device / API", device.Name + " / " + Known(device.ApiVersion)));
            panel.Rows.Add(new Row("Driver", Known(device.DriverName) + " " + Known(device.DriverVersion) +
                (string.IsNullOrWhiteSpace(device.DriverInfo) ? "" : "; " + device.DriverInfo)));
            bool observed = inventory.IcdVerified == true && !string.IsNullOrWhiteSpace(inventory.IcdLibraryPath);
            panel.Rows.Add(new Row(observed ? "ICD observed" : "ICD requested",
                observed ? inventory.IcdLibraryPath : inventory.IcdPath, observed ? Level.Info : Level.Warn));
            panel.Rows.Add(new Row("Ext / layers", Count(inventory.InstanceExtensionCount) + " instance, " +
                Count(inventory.DeviceExtensionCount) + " device; " + Count(inventory.LayerCount) + " layers"));
            var formats = inventory.Formats ?? new VulkanInventory.FormatInfo();
            panel.Rows.Add(new Row("Formats", Count(formats.SupportedCount) + "/" + Count(formats.EnumeratedCount) +
                " supported; storage " + Count(formats.StorageImageCount) + ", color " + Count(formats.ColorAttachmentCount)));
            panel.Rows.Add(new Row("Advertised", Feature(inventory, "timelineSemaphore", "timeline") + " " +
                Feature(inventory, "bufferDeviceAddress", "BDA") + " " + Feature(inventory, "shaderFloat16", "FP16") + " " +
                Feature(inventory, "sparseBinding", "sparse") + " (? = not captured)"));
            panel.Rows.Add(new Row("Full report", Known(inventory.DetailsPath)));
            panel.Rows.Add(new Row("Scope", "ICD declarations; not a rendering test"));
            return panel;
        }
    }
}
