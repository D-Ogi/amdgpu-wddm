// Which graphics API a running 3D application started and which driver path serves it. The evidence is the set
// of image files mapped into the application's own process (the runtime and the UMD it loaded), not the registry.
// Cheap by design (a 300 ms game stall came from a KMD escape poll, see README): no KMD escape, no remote thread,
// no loader lock. Module handles come from EnumProcessModulesEx (ReadProcessMemory of the loader list, the target
// keeps running) and names from GetMappedFileName (one query per module); names are re-read only when the set of
// module handles changes or the cached list is 30 s old. The scan has an off switch (graphics-api.pause) and a
// per-process one (graphics-api.skip), so a measured session can remove its cost without a rebuild.
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

        // Runtime DLLs that Direct3D applications must get from the Windows directory. A copy of one of these
        // next to the application replaces the system runtime (the translation layers shipped beside a game),
        // so the application no longer goes through the installed Windows driver and a UMD module in the
        // process proves nothing about its path. ReplacedRuntimes names the ones found outside \Windows\.
        static readonly HashSet<string> Runtimes = new HashSet<string>(StringComparer.OrdinalIgnoreCase)
        { "d3d12.dll", "d3d11.dll", "d3d10.dll", "d3d10_1.dll", "d3d10core.dll", "d3d9.dll", "dxgi.dll" };
        public static bool IsRuntime(string moduleFileName) { return Runtimes.Contains(moduleFileName ?? ""); }
        public const string SideLoaded = "replacement DLL next to the app, not the system runtime";

        // The path of one mapped image, as GetMappedFileName gives it (\Device\HarddiskVolumeN\Windows\...).
        public static bool IsSystemImage(string mappedPath)
        {
            string path = (mappedPath ?? "").Replace('/', '\\').ToLowerInvariant();
            return path.Contains("\\windows\\system32\\") || path.Contains("\\windows\\syswow64\\")
                || path.Contains("\\windows\\winsxs\\");
        }

        // modules: lower-case file names of every image mapped in the process. wow64: the process is 32-bit.
        // replacedRuntimes: the subset of `modules` that are Direct3D runtimes mapped from outside \Windows\.
        //
        // A GPU claim needs the whole user-mode stack of one render path: the shell the runtime opened, the
        // engine that shell loads and the ICD the engine draws with. Each shell loads its engine and ICD by
        // name in one step (driver/umd/dxvk/umd-entry.cpp, driver/umd/d3d12/adapter-caps.cpp), so a shell on
        // its own is a device still being created - or a load that failed. D3D11 then usually falls back to
        // WARP, which the WARP row reports, but D3D12 has no implicit fallback: the shell stays mapped and
        // nothing else appears. Escalate() therefore turns an incomplete stack amber once it has persisted.
        public static List<ApiPath> Classify(ICollection<string> modules, bool wow64 = false,
                                             ICollection<string> replacedRuntimes = null)
        {
            Func<string, bool> has = modules.Contains;
            Func<string, bool> replaced = n => replacedRuntimes != null && replacedRuntimes.Contains(n);
            var result = new List<ApiPath>();
            bool warp = has("d3d10warp.dll"), icd = has("amdgpu_wddm_radv.dll");
            if (replaced("dxgi.dll")) result.Add(Foreign("DXGI", SideLoaded));
            if (has("d3d12.dll") || has("d3d12core.dll"))
            {
                bool shell = has("amdgpu_wddm_d3d12.dll"), engine = has("amdgpu_wddm_vkd3d.dll");
                if (replaced("d3d12.dll")) result.Add(Foreign("D3D12", SideLoaded));
                else if (shell && engine && icd) result.Add(Gpu("D3D12", "GPU (amdgpu-wddm vkd3d + RADV)"));
                else if (warp) result.Add(Cpu("D3D12", "CPU (WARP)"));
                else if (shell) result.Add(Idle("D3D12", engine ? "our D3D12 shell and engine loaded, no ICD yet"
                                                               : "our D3D12 shell loaded, no engine yet"));
                // The x86 payload of the release has no D3D12 (BD-064): a 32-bit D3D12 application cannot
                // reach our driver at all, which is a different answer from "the device is still starting".
                else if (wow64) result.Add(Foreign("D3D12", "no x86 D3D12 in this package"));
                else result.Add(Idle("D3D12", "none of our UMDs loaded"));
            }
            bool d3d11 = has("d3d11.dll"), d3d10 = has("d3d10.dll") || has("d3d10_1.dll");
            if (d3d11 || d3d10)
            {
                string api = d3d11 ? (d3d10 ? "D3D11+10" : "D3D11") : "D3D10";
                bool zink = has("bc250d3d_zink.dll");
                bool shell = has("amdgpu_wddm_d3d11.dll"), engine = has("amdgpu_wddm_dxvk.dll");
                // Every runtime that contributed to this row has to be tested: one side-loaded runtime next to
                // the application is enough, whichever of the two the row merged.
                if (replaced("d3d11.dll") || replaced("d3d10.dll") || replaced("d3d10_1.dll") || replaced("d3d10core.dll"))
                    result.Add(Foreign(api, SideLoaded));
                else
                {
                    if (shell && engine && icd) result.Add(Gpu(api, "GPU (amdgpu-wddm DXVK + RADV)"));
                    else if (zink && icd) result.Add(Gpu(api, "GPU (zink, desktop route)"));
                    // bc250d3d.dll is the CPU UMD of both router decisions (driver/umd/router/router-policy.h).
                    // dwm is excluded from this panel, so a process that reaches here took the application
                    // decision (AppRouter Mode/Deny), not the desktop one. A HostedClients entry on the
                    // desktop CPU route maps the same two files and cannot be told apart from the modules.
                    else if (has("bc250d3d.dll"))
                        result.Add(Cpu(api, has("bc250d3d_router.dll") ? "CPU (llvmpipe, app route)"
                                                                       : "CPU (llvmpipe)"));
                    else if (has("d3d11on12.dll")) result.Add(Idle(api, "via D3D11On12 (D3D12 path)"));
                    else if (warp) result.Add(Cpu(api, "CPU (WARP)"));
                    else if (shell) result.Add(Idle(api, engine ? "our D3D11 shell and engine loaded, no ICD yet"
                                                                : "our D3D11 shell loaded, no engine yet"));
                    else if (zink) result.Add(Idle(api, "our zink UMD loaded, no ICD yet"));
                    else if (has("bc250d3d_router.dll")) result.Add(Idle(api, "router loaded, no backend yet"));
                    else result.Add(Idle(api, "none of our UMDs loaded"));
                }
            }
            if (has("d3d9.dll"))
                result.Add(replaced("d3d9.dll") ? Foreign("D3D9", SideLoaded)
                    : has("d3d9on12.dll") ? Idle("D3D9", "via D3D9On12 (D3D12 path)")
                    : has("bc250umd.dll") ? Cpu("D3D9", "stub UMD, no D3D9 renderer") : Idle("D3D9", "none of our UMDs loaded"));
            if (has("vulkan-1.dll"))
                // Only the registered system ICD counts here: amdgpu_wddm_radv.dll is the copy our D3D shells
                // load directly, it is not reached through the Vulkan loader and says nothing about vulkan-1.
                result.Add(has("vulkan_radeon.dll") ? Gpu("Vulkan", "GPU (RADV ICD)")
                    : has("vulkan_lvp.dll") ? Cpu("Vulkan", "CPU (lavapipe)") : Idle("Vulkan", "none of our ICDs loaded"));
            // An OpenGL application maps d3d11.dll or dxgi.dll too (SDL, GLFW and most GL engines do, for the
            // display and the adapter list), so the absence of a Direct3D runtime is no condition for this row.
            // It is suppressed only when a row already claims one of our GPU paths: there the D3D path is what
            // draws, and opengl32.dll in it is Microsoft's software GL that nothing in this package serves.
            if (has("opengl32.dll") && !result.Any(p => p.Level == Level.Good))
                result.Add(Cpu("OpenGL", "no OpenGL ICD in this package"));
            return result;
        }

        // How long an incomplete user-mode stack (shell without engine, or without ICD) may stay grey as a
        // device still being created. After this it is a failed LoadLibrary that nothing else reports: D3D12
        // has no implicit WARP fallback, so the shell stays mapped for the life of the process.
        public static readonly TimeSpan StalledAfter = TimeSpan.FromSeconds(10);
        public static bool IsIncomplete(ApiPath p) { return p.Level == Level.Info && p.Path.EndsWith(" yet"); }
        // The incomplete rows of one report, as a key: a change of state restarts the grace period.
        public static string IncompleteKey(AppReport app)
        {
            return string.Join("|", app.Apis.Where(IsIncomplete).Select(p => p.Api + " " + p.Path));
        }
        // Rewrites the rows of `app` in place once its incomplete state is older than StalledAfter.
        public static void Escalate(AppReport app, TimeSpan age)
        {
            if (age < StalledAfter) return;
            foreach (var p in app.Apis.Where(IsIncomplete).ToList())
            {
                p.Path = p.Path.Replace("no engine yet", "engine never loaded")
                               .Replace("no ICD yet", "ICD never loaded")
                               .Replace("no backend yet", "backend never loaded") + " (load failed)";
                p.Level = Level.Warn;
            }
        }
        static ApiPath Gpu(string api, string path) { return new ApiPath { Api = api, Path = path, Level = Level.Good }; }
        static ApiPath Cpu(string api, string path) { return new ApiPath { Api = api, Path = path, Level = Level.Warn }; }
        static ApiPath Foreign(string api, string path) { return new ApiPath { Api = api, Path = path, Level = Level.Warn }; }
        static ApiPath Idle(string api, string path) { return new ApiPath { Api = api, Path = path, Level = Level.Info }; }

        // The foreground process first, then the processes whose API is known (a process we may not open tells
        // us nothing about 3D and must never take the slot of an application we did classify), then the ones
        // on our GPU path, then the rest, by PID.
        public static List<AppReport> Select(IEnumerable<AppReport> apps)
        {
            return apps.Where(a => a.AccessDenied || a.Apis.Count > 0)
                       .OrderByDescending(a => a.Foreground).ThenBy(a => a.AccessDenied)
                       .ThenByDescending(a => a.Ours).ThenBy(a => a.Pid)
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
            public long ModuleKey;
            public DateTime ScannedUtc;
            public AppReport Report;
        }
        // One module load plus one unload between polls leaves the module count unchanged while the set the
        // classification reads has changed (a failed device creation maps d3d10warp.dll exactly like that),
        // so the cache is keyed on the handles themselves. The sweep they save costs about 2 ms.
        static long ModuleKey(IntPtr[] modules)
        {
            long key = modules.Length;
            foreach (var m in modules) key = key * 1000003 + (long)m;
            return key;
        }

        sealed class Incomplete { public string Key; public DateTime SinceUtc; }
        readonly Dictionary<int, Cached> _cache = new Dictionary<int, Cached>();
        readonly Dictionary<int, Incomplete> _incomplete = new Dictionary<int, Incomplete>();
        string _lastLogged;
        int _self;
        uint _session;

        // Opt-out, as GraphicsPipelineProvider has one for its KMD summary poll: the marker file stops the
        // process scan of the next poll (so a measured session can remove this provider's cost without a
        // rebuild), the name file excludes single process names (a title whose anti-tamper check dislikes a
        // repeated handle on it). Both are read on every poll; neither has to exist.
        public const string PauseFileName = "graphics-api.pause", SkipFileName = "graphics-api.skip";
        readonly string _pausePath, _skipPath;
        List<AppReport> _lastShown = new List<AppReport>();
        DateTime _lastShownUtc;
        HashSet<string> _skip = new HashSet<string>(StringComparer.OrdinalIgnoreCase);
        DateTime _skipWrite;

        public GraphicsApiProvider(string dataDir)
        {
            _pausePath = Path.Combine(dataDir, PauseFileName);
            _skipPath = Path.Combine(dataDir, SkipFileName);
        }

        public string Name { get { return "apps"; } }
        public TimeSpan Period { get { return TimeSpan.FromSeconds(3); } }

        // Process base names (with or without .exe, one per line, # comments) that are never opened.
        void LoadSkipList()
        {
            try
            {
                DateTime write = File.Exists(_skipPath) ? File.GetLastWriteTimeUtc(_skipPath) : default(DateTime);
                if (write == _skipWrite) return;
                _skipWrite = write;
                var names = new HashSet<string>(StringComparer.OrdinalIgnoreCase);
                if (write != default(DateTime))
                    foreach (string raw in File.ReadAllLines(_skipPath))
                    {
                        string line = raw.Trim();
                        if (line.Length == 0 || line.StartsWith("#")) continue;
                        names.Add(Path.GetFileNameWithoutExtension(line));
                    }
                _skip = names;
            }
            catch { }
        }

        public void Poll(State state)
        {
            var panel = new Panel { Name = Name, Title = "Active 3D app", Order = 11 };
            bool paused;
            try { paused = File.Exists(_pausePath); } catch { paused = false; }
            if (paused)
            {
                if (_lastShown.Count > 0) AddRows(panel, _lastShown);
                panel.Rows.Add(new Row("Sampled", _lastShownUtc == default(DateTime) ? "paused; no sample yet"
                    : "paused; sample " + _lastShownUtc.ToLocalTime().ToString("HH:mm:ss"), Level.Warn));
                state.SetPanel(panel);
                return;
            }
            LoadSkipList();
            if (_self == 0) using (var me = Process.GetCurrentProcess()) { _self = me.Id; _session = (uint)me.SessionId; }
            int self = _self;
            uint session = _session;
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
            foreach (int gone in _incomplete.Keys.Where(k => !pids.Contains(k)).ToList()) _incomplete.Remove(gone);
            // A shell without its engine or ICD is grey only while it can still be a device being created.
            foreach (var a in reports)
            {
                string key = IncompleteKey(a);
                if (key.Length == 0) { _incomplete.Remove(a.Pid); continue; }
                Incomplete inc;
                if (!_incomplete.TryGetValue(a.Pid, out inc) || inc.Key != key)
                    _incomplete[a.Pid] = inc = new Incomplete { Key = key, SinceUtc = DateTime.UtcNow };
                Escalate(a, DateTime.UtcNow - inc.SinceUtc);
            }
            var shown = Select(reports);
            AddRows(panel, shown);
            _lastShown = shown;
            _lastShownUtc = DateTime.UtcNow;
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
            // The name comes from a query-limited-information handle first. PROCESS_VM_READ plus a loader-list
            // read is what a memory scanner does, and an anti-tamper check in a game may react to it, so the
            // excluded and skipped processes (dwm, explorer, the shells, the skip file) are never opened that way.
            string name;
            IntPtr probe = OpenProcess(ProcessQueryLimitedInformation, false, pid);
            if (probe == IntPtr.Zero) return null;
            try { name = ImageName(probe); } finally { CloseHandle(probe); }
            string bare = Path.GetFileNameWithoutExtension(name);
            if (IsExcluded(bare) || _skip.Contains(bare)) return null;

            IntPtr h = OpenProcess(ProcessQueryInformation | ProcessVmRead, false, pid);
            if (h == IntPtr.Zero)
                return Marshal.GetLastWin32Error() == 5
                    ? new AppReport { Pid = pid, Name = name, AccessDenied = true } : null;
            try
            {
                long create, exit, kernel, user;
                if (!GetProcessTimes(h, out create, out exit, out kernel, out user)) return null;
                IntPtr[] modules = Modules(h);
                Cached c;
                bool fresh = _cache.TryGetValue(pid, out c) && c.CreateTime == create && modules != null
                    && c.ModuleKey == ModuleKey(modules) && DateTime.UtcNow - c.ScannedUtc < TimeSpan.FromSeconds(30);
                if (fresh) return c.Report;
                // A process still starting can refuse the list (ERROR_PARTIAL_COPY): keep the last answer.
                if (modules == null) return c != null && c.CreateTime == create ? c.Report : null;
                bool wow;
                var report = new AppReport { Pid = pid, Name = name, Wow64 = IsWow64Process(h, out wow) && wow };
                var names = new HashSet<string>(StringComparer.OrdinalIgnoreCase);
                var replaced = new HashSet<string>(StringComparer.OrdinalIgnoreCase);
                var buffer = new StringBuilder(1024);
                foreach (var m in modules)
                {
                    buffer.Length = 0;
                    if (GetMappedFileNameW(h, m, buffer, buffer.Capacity) <= 0) continue;
                    string mapped = buffer.ToString(), file = Path.GetFileName(mapped).ToLowerInvariant();
                    names.Add(file);
                    if (IsRuntime(file) && !IsSystemImage(mapped)) replaced.Add(file);
                }
                report.Apis = Classify(names, report.Wow64, replaced);
                _cache[pid] = new Cached { CreateTime = create, ModuleKey = ModuleKey(modules), ScannedUtc = DateTime.UtcNow, Report = report };
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
