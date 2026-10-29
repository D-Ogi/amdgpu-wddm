// ControlDll.Load in one process per case (a module stays loaded, so a case cannot be undone). test-control-load.ps1
// builds the layouts and runs each case. Arguments: <case> <install root or "-">; the private copy is beside the exe.
using System;
using System.Collections.Generic;
using System.Diagnostics;
using System.IO;
using System.Runtime.InteropServices;

namespace Bc250Mon
{
    static class ControlDllTest
    {
        // The same binding by module name as the imports in src\*.cs; pure catalog, no driver access.
        [DllImport("bc250control.dll", ExactSpelling = true)] static extern uint AmdgpuClientCount();

        static int checks;
        static void Check(bool value, string name)
        {
            checks++;
            Console.WriteLine((value ? "PASS  " : "FAIL  ") + name);
            if (!value) throw new Exception(name);
        }

        static int Loaded()
        {
            int n = 0;
            foreach (ProcessModule m in Process.GetCurrentProcess().Modules)
                if (string.Equals(m.ModuleName, ControlDll.FileName, StringComparison.OrdinalIgnoreCase)) n++;
            return n;
        }

        static int Main(string[] argv)
        {
            try
            {
                string mode = argv[0], root = argv.Length > 1 && argv[1] != "-" ? argv[1] : null;
                string own = Path.Combine(AppDomain.CurrentDomain.BaseDirectory, ControlDll.FileName);
                ControlDll.InstallRoots = () => root == null ? new string[0] : new[] { root };
                Check(ControlDll.Imports().Contains("AmdgpuClientOpen") && ControlDll.Imports().Contains("Bc250Dpm"),
                    mode + ": the import list is read from the attributes (" + ControlDll.Imports().Count + " entry points)");
                if (mode == "nopreload")
                {
                    // Positive control: without ControlDll.Load the name binds to the copy beside the exe.
                    Check(AmdgpuClientCount() > 0, mode + ": catalog call");
                    Check(string.Equals(ControlDll.BoundModule(), own, StringComparison.OrdinalIgnoreCase), mode + ": bound to the private copy " + ControlDll.BoundModule());
                    return Done();
                }
                ControlDll.Load(AppDomain.CurrentDomain.BaseDirectory);
                Console.WriteLine("      row: " + ControlDll.Describe());
                Check(AmdgpuClientCount() > 0, mode + ": catalog call through the import");
                string bound = ControlDll.BoundModule();
                Check(Loaded() == 1, mode + ": exactly one bc250control.dll in the process");
                string expect = mode == "installed" ? Path.Combine(root, "control", ControlDll.FileName)
                              : mode == "tools" ? Path.Combine(root, "tools", ControlDll.FileName)
                              : own;
                Check(string.Equals(bound, expect, StringComparison.OrdinalIgnoreCase), mode + ": bound to " + bound);
                Check(string.Equals(ControlDll.Path, expect, StringComparison.OrdinalIgnoreCase), mode + ": ControlDll.Path is the bound module");
                string kind = mode == "installed" || mode == "tools" ? "installed" : "private";
                Check(ControlDll.Kind == kind, mode + ": kind " + ControlDll.Kind);
                Check(ControlDll.Describe().StartsWith(kind + " " + expect, StringComparison.OrdinalIgnoreCase), mode + ": the row names the kind and the path");
                Check(ControlDll.MissingExports(bound).Count == 0, mode + ": the loaded DLL exports every import");
                if (mode == "installed" || mode == "tools") Check(ControlDll.Level == Level.Good && ControlDll.Note == null, mode + ": no note");
                if (mode == "private") Check(ControlDll.Level == Level.Warn && ControlDll.Note == "no installed bc250control.dll", mode + ": note says why");
                if (mode == "broken")
                    Check(ControlDll.Level == Level.Warn && ControlDll.Note.Contains("installed DLL did not load") && ControlDll.Note.Contains("Windows error 193"),
                        mode + ": note names the installed DLL that did not load");
                return Done();
            }
            catch (Exception e) { Console.WriteLine("FAIL  " + e.Message); return 1; }
        }

        static int Done() { Console.WriteLine("PASSED: " + checks + " checks"); return 0; }
    }
}
