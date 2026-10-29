// Which bc250control.dll the monitor uses. Every DllImport of bc250control.dll binds by module name, and Windows
// gives a P/Invoke the module of that name that the process loaded first, from any directory. So Program.Main loads
// the installed DLL by its full path before the first P/Invoke, and all imports bind to it.
//
// The installed DLL is the one that matches the installed driver: a state catalog change makes an older private copy
// beside bc250mon.exe fail with "open driver state: Windows error 1306" (lab 2026-10-29). The install folder is
// HKLM\SOFTWARE\amdgpu-wddm\Release InstallDir (REG_SZ, written by the release installer, the same value that the
// control application reads), else %ProgramFiles%\amdgpu-wddm. In that folder the DLL is control\bc250control.dll
// (beside the control application), else tools\bc250control.dll (beside bc250kmd_cli.exe). The private copy beside
// the executable is used only when no install folder has the DLL, or when the installed one does not load.
using System;
using System.Collections.Generic;
using System.ComponentModel;
using System.IO;
using System.Reflection;
using System.Runtime.InteropServices;
using System.Security.Cryptography;

// The CLR's default search for a DllImport tries the executable's folder by full path first, and that loads the
// private copy as a second module beside the preloaded installed one (the load test showed two modules). Without
// AssemblyDirectory, the CLR passes the bare name to LoadLibraryEx, and Windows returns the module of that name that is
// already loaded. The other imports of this assembly are system DLLs, which SafeDirectories finds in System32.
[assembly: DefaultDllImportSearchPaths(DllImportSearchPath.SafeDirectories)]

namespace Bc250Mon
{
    public static class ControlDll
    {
        public const string FileName = "bc250control.dll";
        public static readonly string[] SubDirectories = { "control", "tools" };

        // Tests replace this source; production reads the registry value and the Program Files folder.
        internal static Func<IEnumerable<string>> InstallRoots = DefaultInstallRoots;

        public static string Kind { get; private set; }     // "installed", "private" or "none"
        public static string Path { get; private set; }
        public static string Sha256 { get; private set; }
        public static string Note { get; private set; }     // why the private copy is in use, or missing exports
        public static Level Level { get; private set; }

        // The text of the overlay's "Control DLL" row.
        public static string Describe()
        {
            if (Kind == null) return "not loaded yet";
            string text = Kind + (Path != null ? " " + Path : "") + (Sha256 != null ? " (" + Sha256.Substring(0, 8) + ")" : "");
            return Note != null ? text + "; " + Note : text;
        }

        public static IEnumerable<string> DefaultInstallRoots()
        {
            string dir = null;
            try
            {
                using (var k = Microsoft.Win32.Registry.LocalMachine.OpenSubKey(@"SOFTWARE\amdgpu-wddm\Release"))
                    dir = k == null ? null : k.GetValue("InstallDir") as string;
            }
            catch (Exception) { }
            if (!string.IsNullOrEmpty(dir)) yield return dir;
            string pf = Environment.GetFolderPath(Environment.SpecialFolder.ProgramFiles);
            if (!string.IsNullOrEmpty(pf)) yield return System.IO.Path.Combine(pf, "amdgpu-wddm");
        }

        // The installed DLLs in order of preference: control\ then tools\ of each install root.
        public static List<string> InstalledCandidates()
        {
            var found = new List<string>();
            foreach (string root in InstallRoots())
            {
                if (string.IsNullOrEmpty(root)) continue;
                foreach (string sub in SubDirectories)
                {
                    string p = System.IO.Path.Combine(root, sub, FileName);
                    if (File.Exists(p) && !found.Exists(f => string.Equals(f, p, StringComparison.OrdinalIgnoreCase))) found.Add(p);
                }
            }
            return found;
        }

        // Call once, before any P/Invoke into bc250control.dll.
        public static void Load(string appDirectory)
        {
            var failures = new List<string>();
            foreach (string candidate in InstalledCandidates())
            {
                if (TryLoad(candidate, failures)) { Set("installed", candidate, null); return; }
            }
            string own = System.IO.Path.Combine(appDirectory, FileName);
            string why = failures.Count > 0 ? "installed DLL did not load: " + string.Join(", ", failures) : "no installed bc250control.dll";
            if (File.Exists(own) && TryLoad(own, failures)) { Set("private", own, why); return; }
            Kind = "none"; Path = null; Sha256 = null; Level = Level.Error;
            Note = why + (File.Exists(own) ? ", private copy did not load: " + failures[failures.Count - 1] : ", no private copy beside the executable");
        }

        static bool TryLoad(string path, List<string> failures)
        {
            IntPtr module = LoadLibraryExW(path, IntPtr.Zero, LoadWithAlteredSearchPath);
            if (module != IntPtr.Zero) return true;
            failures.Add(path + " (Windows error " + Marshal.GetLastWin32Error() + ")");
            return false;
        }

        static void Set(string kind, string path, string why)
        {
            Kind = kind; Path = path; Level = kind == "installed" ? Level.Good : Level.Warn;
            try { using (var s = File.OpenRead(path)) using (var h = SHA256.Create()) Sha256 = BitConverter.ToString(h.ComputeHash(s)).Replace("-", ""); }
            catch (Exception) { Sha256 = null; }
            var missing = MissingExports(path);
            if (missing.Count > 0) { why = (why != null ? why + "; " : "") + "missing exports: " + string.Join(", ", missing); Level = Level.Warn; }
            Note = why;
        }

        // Every entry point that a DllImport of bc250control.dll in this assembly names.
        public static SortedSet<string> Imports()
        {
            var names = new SortedSet<string>(StringComparer.Ordinal);
            const BindingFlags all = BindingFlags.Static | BindingFlags.Instance | BindingFlags.Public | BindingFlags.NonPublic | BindingFlags.DeclaredOnly;
            foreach (Type t in typeof(ControlDll).Assembly.GetTypes())
                foreach (MethodInfo m in t.GetMethods(all))
                    foreach (DllImportAttribute a in m.GetCustomAttributes(typeof(DllImportAttribute), false))
                        if (string.Equals(System.IO.Path.GetFileNameWithoutExtension(a.Value), "bc250control", StringComparison.OrdinalIgnoreCase))
                            names.Add(string.IsNullOrEmpty(a.EntryPoint) ? m.Name : a.EntryPoint);
            return names;
        }

        // The imports that the loaded module at path does not export. An older install lacks the newest entry points;
        // the rows that need them fail on their own, and this note names them once.
        public static List<string> MissingExports(string path)
        {
            var missing = new List<string>();
            IntPtr module = GetModuleHandleW(path);
            if (module == IntPtr.Zero) return missing;
            foreach (string name in Imports())
                if (GetProcAddress(module, name) == IntPtr.Zero) missing.Add(name);
            return missing;
        }

        // The path of the module that the name bc250control.dll binds to now, or null.
        public static string BoundModule()
        {
            IntPtr module = GetModuleHandleW(FileName);
            if (module == IntPtr.Zero) return null;
            var buffer = new System.Text.StringBuilder(32768);
            uint n = GetModuleFileNameW(module, buffer, (uint)buffer.Capacity);
            if (n == 0) throw new Win32Exception(Marshal.GetLastWin32Error());
            return buffer.ToString(0, (int)n);
        }

        const uint LoadWithAlteredSearchPath = 0x00000008;   // LOAD_WITH_ALTERED_SEARCH_PATH: dependencies from the DLL's folder
        [DllImport("kernel32.dll", SetLastError = true, CharSet = CharSet.Unicode, ExactSpelling = true)]
        static extern IntPtr LoadLibraryExW(string path, IntPtr file, uint flags);
        [DllImport("kernel32.dll", SetLastError = true, CharSet = CharSet.Unicode, ExactSpelling = true)]
        static extern IntPtr GetModuleHandleW(string name);
        [DllImport("kernel32.dll", SetLastError = true, CharSet = CharSet.Ansi, ExactSpelling = true, BestFitMapping = false)]
        static extern IntPtr GetProcAddress(IntPtr module, string name);
        [DllImport("kernel32.dll", SetLastError = true, CharSet = CharSet.Unicode, ExactSpelling = true)]
        static extern uint GetModuleFileNameW(IntPtr module, System.Text.StringBuilder name, uint size);
    }
}
