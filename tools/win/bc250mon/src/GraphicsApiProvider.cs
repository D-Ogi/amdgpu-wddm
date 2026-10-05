// Which graphics API a running 3D application started and which driver path serves it. The evidence is the set
// of image files mapped into the application's own process (the runtime and the UMD it loaded), not the registry.
// Cheap by design (a 300 ms game stall came from a KMD escape poll, see README): no KMD escape, no remote thread,
// no loader lock. Module handles come from EnumProcessModulesEx (ReadProcessMemory of the loader list, the target
// keeps running) and names from GetMappedFileName (one query per module); names are re-read only when the module
// count changes or the cached list is 30 s old.
using System;
using System.Collections.Generic;
using System.Diagnostics;
using System.IO;
using System.Linq;
using System.Runtime.InteropServices;
using System.Text;

namespace Bc250Mon
{
    public sealed class GraphicsApiProvider : IProvider
    {
        // One graphics API found in a process and the path that serves it.
        public sealed class ApiPath
        {
            public string Api, Path;
            public Level Level;
        }

        // One candidate process as the panel shows it.
        public sealed class AppReport
        {
            public int Pid;
            public string Name;
            public bool Foreground, Wow64, AccessDenied;
            public List<ApiPath> Apis = new List<ApiPath>();
            public bool Ours { get { return Apis.Any(a => a.Level == Level.Good); } }
        }

        // Shells, launchers and our own tools draw with D3D11 too; they are not the "3D app" the owner asks about.
        static readonly HashSet<string> Excluded = new HashSet<string>(StringComparer.OrdinalIgnoreCase)
        {
            "dwm", "explorer", "shellexperiencehost", "startmenuexperiencehost", "searchhost", "searchapp",
            "textinputhost", "lockapp", "applicationframehost", "systemsettings", "sihost", "ctfmon", "taskmgr",
            "bc250mon", "steam", "steamwebhelper", "msedge", "msedgewebview2", "windowsterminal", "openconsole",
            "conhost", "widgets", "gamebar", "gamebarftserver", "phoneexperiencehost", "crossdeviceresume",
        };
        public static bool IsExcluded(string processName) { return Excluded.Contains(processName ?? ""); }

        public const int MaxApps = 3, MaxCandidates = 8;

        // modules: lower-case file names of every image mapped in the process.
        public static List<ApiPath> Classify(ICollection<string> modules)
        {
            Func<string, bool> has = modules.Contains;
            var result = new List<ApiPath>();
            bool warp = has("d3d10warp.dll");
            if (has("d3d12.dll") || has("d3d12core.dll"))
                result.Add(has("amdgpu_wddm_d3d12.dll") ? Gpu("D3D12", "GPU (amdgpu-wddm vkd3d + RADV)")
                    : warp ? Cpu("D3D12", "CPU (WARP)") : Idle("D3D12", "no UMD loaded"));
            bool d3d11 = has("d3d11.dll"), d3d10 = has("d3d10.dll") || has("d3d10_1.dll");
            if (d3d11 || d3d10)
            {
                string api = d3d11 ? (d3d10 ? "D3D11+10" : "D3D11") : "D3D10";
                if (has("amdgpu_wddm_d3d11.dll")) result.Add(Gpu(api, "GPU (amdgpu-wddm DXVK + RADV)"));
                else if (has("bc250d3d_zink.dll")) result.Add(Gpu(api, "GPU (zink, desktop route)"));
                else if (has("bc250d3d.dll")) result.Add(Cpu(api, "CPU (llvmpipe, desktop route)"));
                else if (has("d3d11on12.dll")) result.Add(Idle(api, "via D3D11On12 (D3D12 path)"));
                else if (warp) result.Add(Cpu(api, "CPU (WARP)"));
                else if (has("bc250d3d_router.dll")) result.Add(Idle(api, "router loaded, no backend yet"));
                else result.Add(Idle(api, "no UMD loaded"));
            }
            if (has("d3d9.dll"))
                result.Add(has("d3d9on12.dll") ? Idle("D3D9", "via D3D9On12 (D3D12 path)")
                    : has("bc250umd.dll") ? Cpu("D3D9", "stub UMD, no D3D9 renderer") : Idle("D3D9", "no UMD loaded"));
            if (has("vulkan-1.dll"))
                result.Add(has("vulkan_radeon.dll") ? Gpu("Vulkan", "GPU (RADV ICD)")
                    : has("vulkan_lvp.dll") ? Cpu("Vulkan", "CPU (lavapipe)") : Idle("Vulkan", "no ICD loaded"));
            if (has("opengl32.dll") && result.Count == 0)
                result.Add(Cpu("OpenGL", "no OpenGL ICD in this package"));
            return result;
        }
        static ApiPath Gpu(string api, string path) { return new ApiPath { Api = api, Path = path, Level = Level.Good }; }
        static ApiPath Cpu(string api, string path) { return new ApiPath { Api = api, Path = path, Level = Level.Warn }; }
        static ApiPath Idle(string api, string path) { return new ApiPath { Api = api, Path = path, Level = Level.Info }; }

        // The foreground process first, then processes that run on our GPU path, then the rest, by PID.
        public static List<AppReport> Select(IEnumerable<AppReport> apps)
        {
            return apps.Where(a => a.AccessDenied || a.Apis.Count > 0)
                       .OrderByDescending(a => a.Foreground).ThenByDescending(a => a.Ours).ThenBy(a => a.Pid)
                       .Take(MaxApps).ToList();
        }

        public static void AddRows(Panel panel, IList<AppReport> apps)
        {
            if (apps.Count == 0) panel.Rows.Add(new Row("3D app", "none with a window"));
            foreach (var a in apps)
            {
                string name = a.Name.Length > 17 ? a.Name.Substring(0, 16) + "~" : a.Name;
                string tags = "pid " + a.Pid + (a.Wow64 ? ", x86" : "") + (a.Foreground ? ", foreground" : "");
                if (a.AccessDenied) { panel.Rows.Add(new Row(name, "API unknown (access); " + tags, Level.Warn)); continue; }
                panel.Rows.Add(new Row(name, string.Join(" + ", a.Apis.Select(p => p.Api)) + "; " + tags));
                foreach (var p in a.Apis) panel.Rows.Add(new Row("  " + p.Api, p.Path, p.Level));
            }
        }

        sealed class Cached
        {
            public long CreateTime;
            public int ModuleCount;
            public DateTime ScannedUtc;
            public AppReport Report;
        }
        readonly Dictionary<int, Cached> _cache = new Dictionary<int, Cached>();
        string _lastLogged;

        public string Name { get { return "apps"; } }
        public TimeSpan Period { get { return TimeSpan.FromSeconds(3); } }

        public void Poll(State state)
        {
            var panel = new Panel { Name = Name, Title = "Active 3D app", Order = 11 };
            int self = Process.GetCurrentProcess().Id;
            uint session = (uint)Process.GetCurrentProcess().SessionId;
            int foreground = WindowPid(GetForegroundWindow());
            var pids = new List<int>();
            if (foreground > 0) pids.Add(foreground);
            pids.AddRange(WindowPids().Where(p => p != foreground));
            var reports = new List<AppReport>();
            foreach (int pid in pids)
            {
                if (pid == self || reports.Count >= MaxCandidates) continue;
                uint s;
                if (!ProcessIdToSessionId((uint)pid, out s) || s != session) continue;
                var report = Inspect(pid);
                if (report == null) continue;
                report.Foreground = pid == foreground;
                reports.Add(report);
            }
            foreach (int gone in _cache.Keys.Where(k => !pids.Contains(k)).ToList()) _cache.Remove(gone);
            var shown = Select(reports);
            AddRows(panel, shown);
            panel.Rows.Add(new Row("Sampled", DateTime.Now.ToString("HH:mm:ss") + " (loaded modules, 3 s poll)"));
            state.SetPanel(panel);

            var top = shown.FirstOrDefault(a => a.Foreground && a.Apis.Count > 0);
            string line = top == null ? null : top.Name + ": " + string.Join(", ", top.Apis.Select(p => p.Api + " " + p.Path));
            if (line != null && line != _lastLogged)
            {
                state.Log(Name, top.Ours ? Level.Good : Level.Info, line);
                _lastLogged = line;
            }
        }

        // Returns null when the process is gone or excluded. A process we may not read gives AccessDenied.
        AppReport Inspect(int pid)
        {
            IntPtr h = OpenProcess(ProcessQueryInformation | ProcessVmRead, false, pid);
            if (h == IntPtr.Zero)
            {
                int error = Marshal.GetLastWin32Error();
                if (error != 5) return null;
                h = OpenProcess(ProcessQueryLimitedInformation, false, pid);
                if (h == IntPtr.Zero) return null;
                try { string denied = ImageName(h); return IsExcluded(Path.GetFileNameWithoutExtension(denied)) ? null : new AppReport { Pid = pid, Name = denied, AccessDenied = true }; }
                finally { CloseHandle(h); }
            }
            try
            {
                string name = ImageName(h);
                if (IsExcluded(Path.GetFileNameWithoutExtension(name))) return null;
                long create, exit, kernel, user;
                if (!GetProcessTimes(h, out create, out exit, out kernel, out user)) return null;
                IntPtr[] modules = Modules(h);
                Cached c;
                bool fresh = _cache.TryGetValue(pid, out c) && c.CreateTime == create && modules != null
                    && c.ModuleCount == modules.Length && DateTime.UtcNow - c.ScannedUtc < TimeSpan.FromSeconds(30);
                if (fresh) return c.Report;
                // A process still starting can refuse the list (ERROR_PARTIAL_COPY): keep the last answer.
                if (modules == null) return c != null && c.CreateTime == create ? c.Report : null;
                bool wow;
                var report = new AppReport { Pid = pid, Name = name, Wow64 = IsWow64Process(h, out wow) && wow };
                var names = new HashSet<string>(StringComparer.OrdinalIgnoreCase);
                var buffer = new StringBuilder(1024);
                foreach (var m in modules)
                {
                    buffer.Length = 0;
                    if (GetMappedFileNameW(h, m, buffer, buffer.Capacity) > 0)
                        names.Add(Path.GetFileName(buffer.ToString()).ToLowerInvariant());
                }
                report.Apis = Classify(names);
                _cache[pid] = new Cached { CreateTime = create, ModuleCount = modules.Length, ScannedUtc = DateTime.UtcNow, Report = report };
                return report;
            }
            finally { CloseHandle(h); }
        }

        static IntPtr[] Modules(IntPtr h)
        {
            var modules = new IntPtr[512];
            for (int attempt = 0; attempt < 3; ++attempt)
            {
                int needed;
                if (!EnumProcessModulesEx(h, modules, modules.Length * IntPtr.Size, out needed, ListModulesAll)) return null;
                int count = needed / IntPtr.Size;
                if (count <= modules.Length) { Array.Resize(ref modules, count); return modules; }
                modules = new IntPtr[count + 64];
            }
            return null;
        }

        static string ImageName(IntPtr h)
        {
            var buffer = new StringBuilder(1024);
            int size = buffer.Capacity;
            return QueryFullProcessImageNameW(h, 0, buffer, ref size) ? Path.GetFileName(buffer.ToString()) : "?";
        }

        static int WindowPid(IntPtr hwnd)
        {
            if (hwnd == IntPtr.Zero) return 0;
            uint pid;
            GetWindowThreadProcessId(hwnd, out pid);
            return (int)pid;
        }

        // Owners of visible, uncloaked, not minimized top-level windows of a useful size, in z-order.
        static List<int> WindowPids()
        {
            var pids = new List<int>();
            EnumWindows((hwnd, _) =>
            {
                RECT r;
                int cloaked;
                if (IsWindowVisible(hwnd) && !IsIconic(hwnd) && GetWindowRect(hwnd, out r)
                    && r.Right - r.Left >= 200 && r.Bottom - r.Top >= 150
                    && (DwmGetWindowAttribute(hwnd, DwmwaCloaked, out cloaked, 4) != 0 || cloaked == 0))
                {
                    int pid = WindowPid(hwnd);
                    if (pid > 0 && !pids.Contains(pid)) pids.Add(pid);
                }
                return true;
            }, IntPtr.Zero);
            return pids;
        }

        const uint ProcessQueryInformation = 0x0400, ProcessVmRead = 0x0010, ProcessQueryLimitedInformation = 0x1000;
        const uint ListModulesAll = 3;
        const int DwmwaCloaked = 14;
        [StructLayout(LayoutKind.Sequential)] struct RECT { public int Left, Top, Right, Bottom; }
        delegate bool EnumWindowsProc(IntPtr hwnd, IntPtr param);
        [DllImport("user32.dll")] static extern bool EnumWindows(EnumWindowsProc callback, IntPtr param);
        [DllImport("user32.dll")] static extern IntPtr GetForegroundWindow();
        [DllImport("user32.dll")] static extern uint GetWindowThreadProcessId(IntPtr hwnd, out uint pid);
        [DllImport("user32.dll")] static extern bool IsWindowVisible(IntPtr hwnd);
        [DllImport("user32.dll")] static extern bool IsIconic(IntPtr hwnd);
        [DllImport("user32.dll")] static extern bool GetWindowRect(IntPtr hwnd, out RECT rect);
        [DllImport("dwmapi.dll")] static extern int DwmGetWindowAttribute(IntPtr hwnd, int attribute, out int value, int size);
        [DllImport("kernel32.dll", SetLastError = true)] static extern IntPtr OpenProcess(uint access, bool inherit, int pid);
        [DllImport("kernel32.dll")] static extern bool CloseHandle(IntPtr h);
        [DllImport("kernel32.dll")] static extern bool ProcessIdToSessionId(uint pid, out uint session);
        [DllImport("kernel32.dll")] static extern bool GetProcessTimes(IntPtr h, out long create, out long exit, out long kernel, out long user);
        [DllImport("kernel32.dll")] static extern bool IsWow64Process(IntPtr h, out bool wow64);
        [DllImport("kernel32.dll", CharSet = CharSet.Unicode)]
        static extern bool QueryFullProcessImageNameW(IntPtr h, uint flags, StringBuilder name, ref int size);
        [DllImport("kernel32.dll", EntryPoint = "K32EnumProcessModulesEx", SetLastError = true)]
        static extern bool EnumProcessModulesEx(IntPtr h, [Out] IntPtr[] modules, int bytes, out int needed, uint filter);
        [DllImport("kernel32.dll", EntryPoint = "K32GetMappedFileNameW", CharSet = CharSet.Unicode)]
        static extern int GetMappedFileNameW(IntPtr h, IntPtr address, StringBuilder name, int size);
    }
}
