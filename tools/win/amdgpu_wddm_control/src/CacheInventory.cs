// The shader cache inventory (F-CACHE, WU-057 phase 1, C11): each cache on its own row, in plain words. Phase 1 only
// shows; clearing comes in phase 2. The app never says "no cache" for a cache it has not checked. Every cache below is
// per user, under the LocalAppData folder of the account that runs this app (games of another account are not counted).
//   D3D12 engine: %LOCALAPPDATA%\amdgpu-wddm\vkd3d (vkd3d-proton ddi_engine.c bc250_prepare_cache_directory):
//                 vkd3d-proton.<program>.cache pairs and a .cache.driver pipeline cache.
//   D3D11 engine: %LOCALAPPDATA%\amdgpu-wddm\dxvk (DXVK src/ddi/shader_cache_policy.cpp): on by default in both
//                 flavours. One <16 hex digits>-<16 hex digits>.dxvk.lut/.dxvk.bin pair per program and driver build;
//                 the first 16 digits name the program. 2 GiB for all programs together (DXVK SHADER_CACHE.md).
//   Mesa (Vulkan and OpenGL, and the RADV under both DirectX engines): %LOCALAPPDATA%\mesa_shader_cache, the
//                 multi-file disk cache (Mesa src/util/disk_cache_os.c), entries in two-character subdirectories,
//                 1 GiB by default. Shared by all programs, so no program count.
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

    // Where the caches are. Null for a directory: the LocalAppData folder cannot be found.
    public sealed class CacheDirectories
    {
        public string D3D12, D3D11, Mesa;
    }

    public static class CacheInventory
    {
        static string LocalAppData()
        {
            var local = Environment.GetFolderPath(Environment.SpecialFolder.LocalApplicationData);
            return string.IsNullOrEmpty(local) ? null : local;
        }

        public static string D3D12Directory()
        {
            var local = LocalAppData();
            return local == null ? null : Path.Combine(local, "amdgpu-wddm", "vkd3d");
        }

        public static CacheDirectories Directories()
        {
            var local = LocalAppData();
            if (local == null) return new CacheDirectories();
            return new CacheDirectories
            {
                D3D12 = Path.Combine(local, "amdgpu-wddm", "vkd3d"),
                D3D11 = Path.Combine(local, "amdgpu-wddm", "dxvk"),
                Mesa = Path.Combine(local, "mesa_shader_cache"),
            };
        }

        // A name that only the D3D11 engine's cache writes: 16 lower-case hex digits, '-', 16 lower-case hex digits,
        // ".dxvk.lut" or ".dxvk.bin" (DXVK dxvk_shader_cache_quota.h isCacheFileName).
        public static bool IsD3D11CacheFile(string name)
        {
            if (name == null || name.Length != 42) return false;
            for (int i = 0; i < 33; i++)
            {
                char c = name[i];
                if (i == 16) { if (c != '-') return false; }
                else if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'))) return false;
            }
            var tail = name.Substring(33);
            return tail == ".dxvk.lut" || tail == ".dxvk.bin";
        }

        public static List<CacheRow> Read(string d3d12Directory)
        {
            var d = Directories();
            d.D3D12 = d3d12Directory;
            return Read(d);
        }

        public static List<CacheRow> Read(CacheDirectories d)
        {
            var rows = new List<CacheRow>();

            var d3d12 = new CacheRow { Id = "d3d12", Name = Strings.T("cache.d3d12") };
            Fill(d3d12, d.D3D12, false, names =>
                // One program per vkd3d-proton.<program>.cache; the .cache.driver and .write files belong to it.
                names.Count(n => n.StartsWith("vkd3d-proton.", StringComparison.OrdinalIgnoreCase) && n.EndsWith(".cache", StringComparison.OrdinalIgnoreCase)));
            d3d12.Text = d3d12.State == CacheState.Present ? Strings.T("cache.d3d12.present", Size(d3d12.Bytes), d3d12.Programs)
                : d3d12.State == CacheState.Empty ? Strings.T("cache.d3d12.empty") : Strings.T("cache.unknown");
            rows.Add(d3d12);

            var d3d11 = new CacheRow { Id = "d3d11", Name = Strings.T("cache.d3d11") };
            Fill(d3d11, d.D3D11, false, names =>
                // One program per distinct first 16 digits; a program has one pair per driver build.
                names.Where(IsD3D11CacheFile).Select(n => n.Substring(0, 16)).Distinct().Count());
            d3d11.Text = d3d11.State == CacheState.Present ? Strings.T("cache.d3d11.present", Size(d3d11.Bytes), d3d11.Programs)
                : d3d11.State == CacheState.Empty ? Strings.T("cache.d3d11.empty") : Strings.T("cache.unknown");
            rows.Add(d3d11);

            var mesa = new CacheRow { Id = "vulkan", Name = Strings.T("cache.vulkan") };
            Fill(mesa, d.Mesa, true, null);
            mesa.Text = mesa.State == CacheState.Present ? Strings.T("cache.vulkan.present", Size(mesa.Bytes))
                : mesa.State == CacheState.Empty ? Strings.T("cache.vulkan.empty") : Strings.T("cache.unknown");
            rows.Add(mesa);

            rows.Add(new CacheRow { Id = "windows", Name = Strings.T("cache.windows"), State = CacheState.Windows, Text = Strings.T("cache.windows.text") });
            return rows;
        }

        // Size and state of one cache directory. An absent directory is an empty cache; a directory that cannot be
        // read, or no LocalAppData folder, is unknown.
        static void Fill(CacheRow row, string directory, bool recursive, Func<IEnumerable<string>, int> programs)
        {
            try
            {
                if (directory == null) { row.State = CacheState.Unknown; return; }
                if (!Directory.Exists(directory)) { row.State = CacheState.Empty; return; }
                var files = new DirectoryInfo(directory).GetFiles("*", recursive ? SearchOption.AllDirectories : SearchOption.TopDirectoryOnly);
                row.Bytes = files.Sum(f => f.Length);
                if (programs != null) row.Programs = programs(files.Select(f => f.Name));
                row.State = files.Length == 0 ? CacheState.Empty : CacheState.Present;
            }
            catch (Exception) { row.State = CacheState.Unknown; }
        }

        public static string Size(long bytes)
        {
            if (bytes >= 1L << 30) return (bytes / (double)(1L << 30)).ToString("0.0", CultureInfo.InvariantCulture) + " GB";
            if (bytes >= 1L << 20) return (bytes / (double)(1L << 20)).ToString("0", CultureInfo.InvariantCulture) + " MB";
            return Math.Max(1, (bytes + 1023) / 1024).ToString(CultureInfo.InvariantCulture) + " KB";
        }
    }
}
