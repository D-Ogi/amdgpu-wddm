// The shader cache inventory (F-CACHE, WU-057 phase 1, C11): each cache on its own row, in plain words. Phase 1 only
// shows; clearing comes in phase 2. The app never says "no cache" for a cache it has not checked.
//   D3D12 engine: EXISTS, per user, %LOCALAPPDATA%\amdgpu-wddm\vkd3d (vkd3d-proton.<program>.cache pairs and a
//                 .cache.driver pipeline cache).
//   D3D11 engine: off by design in the design document, not yet confirmed against the registered engine -> unknown.
//   Vulkan (Mesa ICD): not verified for our build -> unknown.
//   Windows' DirectX shader cache: owned by Windows (Disk Cleanup), a pointer only.
using System;
using System.Collections.Generic;
using System.Globalization;
using System.IO;
using System.Linq;

namespace AmdgpuWddmControl
{
    public enum CacheState { Present, Empty, Unknown, Windows }

    public sealed class CacheRow
    {
        public string Id, Name, Text;
        public CacheState State;
        public long Bytes;
        public int Programs;
    }

    public static class CacheInventory
    {
        public static string D3D12Directory()
        {
            var local = Environment.GetFolderPath(Environment.SpecialFolder.LocalApplicationData);
            return string.IsNullOrEmpty(local) ? null : Path.Combine(local, "amdgpu-wddm", "vkd3d");
        }

        public static List<CacheRow> Read(string d3d12Directory)
        {
            var rows = new List<CacheRow>();
            var d3d12 = new CacheRow { Id = "d3d12", Name = Strings.T("cache.d3d12") };
            try
            {
                if (d3d12Directory != null && Directory.Exists(d3d12Directory))
                {
                    var files = new DirectoryInfo(d3d12Directory).GetFiles("*", SearchOption.TopDirectoryOnly);
                    d3d12.Bytes = files.Sum(f => f.Length);
                    // One program per vkd3d-proton.<program>.cache; the .cache.driver and .write files belong to it.
                    d3d12.Programs = files.Select(f => f.Name).Where(n => n.StartsWith("vkd3d-proton.", StringComparison.OrdinalIgnoreCase) && n.EndsWith(".cache", StringComparison.OrdinalIgnoreCase)).Count();
                    d3d12.State = files.Length == 0 ? CacheState.Empty : CacheState.Present;
                }
                else d3d12.State = CacheState.Empty;
            }
            catch (Exception) { d3d12.State = CacheState.Unknown; }
            d3d12.Text = d3d12.State == CacheState.Present ? Strings.T("cache.d3d12.present", Size(d3d12.Bytes), d3d12.Programs)
                : d3d12.State == CacheState.Empty ? Strings.T("cache.d3d12.empty") : Strings.T("cache.unknown");
            rows.Add(d3d12);
            rows.Add(new CacheRow { Id = "d3d11", Name = Strings.T("cache.d3d11"), State = CacheState.Unknown, Text = Strings.T("cache.unknown") });
            rows.Add(new CacheRow { Id = "vulkan", Name = Strings.T("cache.vulkan"), State = CacheState.Unknown, Text = Strings.T("cache.unknown") });
            rows.Add(new CacheRow { Id = "windows", Name = Strings.T("cache.windows"), State = CacheState.Windows, Text = Strings.T("cache.windows.text") });
            return rows;
        }

        public static string Size(long bytes)
        {
            if (bytes >= 1L << 30) return (bytes / (double)(1L << 30)).ToString("0.0", CultureInfo.InvariantCulture) + " GB";
            if (bytes >= 1L << 20) return (bytes / (double)(1L << 20)).ToString("0", CultureInfo.InvariantCulture) + " MB";
            return Math.Max(1, (bytes + 1023) / 1024).ToString(CultureInfo.InvariantCulture) + " KB";
        }
    }
}
