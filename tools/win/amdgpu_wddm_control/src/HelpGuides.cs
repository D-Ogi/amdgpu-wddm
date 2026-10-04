// The help content's plain-words parts (WU-063, WU-058): the symptom guides from the string tables and the repair
// entry of the kept repair set (docs/gui/interfaces.md section 4). Reads only; the setup it starts elevates itself.
using System;
using System.Collections.Generic;
using System.Diagnostics;
using System.IO;
using Microsoft.Win32;

namespace AmdgpuWddmControl
{
    public static class HelpGuides
    {
        public static readonly string[] Ids = { "dark", "slow", "crash", "display" };
        // The recovery view (WU-068, C14) shows the Safe Mode guide in place of the slow-games one.
        public static readonly string[] RecoveryIds = { "dark", "safe", "crash", "display" };

        // The steps of a guide: help.guide.<id>.1 .. n, as many as the English table has.
        public static List<string> Steps(string id)
        {
            var steps = new List<string>();
            for (int i = 1; Strings.Has("help.guide." + id + "." + i); i++) steps.Add(Strings.T("help.guide." + id + "." + i));
            return steps;
        }

        public const string RepairPath = @"SOFTWARE\amdgpu-wddm\Release", RepairValue = "RepairSetup";

        // The setup exe of the kept repair set (interfaces.md section 4), or null when it is not there.
        public static string RepairSetup()
        {
            try
            {
                using (var k = Registry.LocalMachine.OpenSubKey(RepairPath))
                {
                    var v = k == null ? null : k.GetValue(RepairValue) as string;
                    if (string.IsNullOrEmpty(v) || !Path.IsPathRooted(v) || !v.EndsWith(".exe", StringComparison.OrdinalIgnoreCase) || !File.Exists(v)) return null;
                    return v;
                }
            }
            catch (Exception) { return null; }
        }

        public static bool StartRepair()
        {
            var setup = RepairSetup();
            if (setup == null) return false;
            try { using (Process.Start(new ProcessStartInfo(setup, "--repair") { UseShellExecute = true })) { } return true; }
            catch (Exception) { return false; }
        }
    }
}
