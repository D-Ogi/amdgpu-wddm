// The setup window's command line (docs/gui/interfaces-setup.md section 8), parsed without side effects.
//
//   amdgpu_wddm_setup.exe                     welcome: install or update from this package or a prepared folder
//   amdgpu_wddm_setup.exe --continue          after a restart (RunOnce): the engine continues at once
//   amdgpu_wddm_setup.exe --repair            from the control app's Help (Release\RepairSetup): repair
//   amdgpu_wddm_setup.exe --prepare-offline   prepare a folder for a PC without internet (no administrator needed)
//   [--package <dir>]                         another package folder than the one the exe is in
//   [--run-root <dir>]                        where the engine runs keep their files (default %TEMP%\amdgpu-wddm-setup)
//   [--dry-run]                               the install run is an engine dry run: every screen, no change (L4)
//   amdgpu_wddm_setup.exe --version
//   amdgpu_wddm_setup.exe --smoke-render <dir> <scale> <lang> [--text-scale <f>] [--package <dir>]
//                                             no window: every screen from fixtures drawn to <dir>, checked
//   amdgpu_wddm_setup.exe --smoke-engine <package> <out-dir> [--script <name>] [--cancel] [--plan] -- <engine args>
//                                             no window: one engine run, its model and screens summarised
using System;
using System.Collections.Generic;
using System.Globalization;
using System.Linq;

namespace AmdgpuWddmSetup
{
    public sealed class SetupArgs
    {
        public string[] Raw = new string[0];
        public string Mode = "welcome";
        public string Package, RunRoot, Error;
        public bool DryRun;
        public readonly List<string> EngineArgs = new List<string>();
        // smoke entries
        public string SmokeDir, Language = "en", Script = "install.ps1";
        public float Scale = 1f, TextScale = 1f;
        public bool Cancel, PlanRun;

        public bool Headless { get { return Mode == "smoke-render" || Mode == "smoke-engine" || Mode == "version"; } }

        public static SetupArgs Parse(string[] args)
        {
            var a = new SetupArgs { Raw = args ?? new string[0] };
            var list = a.Raw.ToList();
            int i = 0;
            Func<string> next = () => i + 1 < list.Count ? list[++i] : null;
            for (; i < list.Count; i++)
            {
                var x = list[i];
                switch (x)
                {
                    case "--version": a.SetMode("version"); break;
                    case "--continue": a.SetMode("continue"); break;
                    case "--repair": a.SetMode("repair"); break;
                    case "--prepare-offline": a.SetMode("prepare-offline"); break;
                    case "--dry-run": a.DryRun = true; break;
                    case "--package": a.Package = next(); if (a.Package == null) a.Error = "--package needs a folder"; break;
                    case "--run-root": a.RunRoot = next(); if (a.RunRoot == null) a.Error = "--run-root needs a folder"; break;
                    case "--smoke-render":
                        a.SetMode("smoke-render");
                        a.SmokeDir = next();
                        var scale = next();
                        a.Language = next();
                        float s;
                        if (a.SmokeDir == null || a.Language == null || !float.TryParse(scale, NumberStyles.Float, CultureInfo.InvariantCulture, out s) || s < 1 || s > 4)
                            a.Error = "--smoke-render <dir> <scale 1..4> <language>";
                        else a.Scale = s;
                        if (a.Language != null && Array.IndexOf(Strings.Languages, a.Language) < 0) a.Error = "unknown language " + a.Language;
                        break;
                    case "--text-scale":
                        float t;
                        if (!float.TryParse(next(), NumberStyles.Float, CultureInfo.InvariantCulture, out t) || t < 1 || t > 2.25) a.Error = "--text-scale <1..2.25>";
                        else a.TextScale = t;
                        break;
                    case "--smoke-engine":
                        a.SetMode("smoke-engine");
                        a.Package = next();
                        a.SmokeDir = next();
                        if (a.Package == null || a.SmokeDir == null) a.Error = "--smoke-engine <package> <out-dir>";
                        break;
                    case "--script": a.Script = next(); if (a.Script != "install.ps1" && a.Script != "prepare-offline.ps1") a.Error = "--script install.ps1 or prepare-offline.ps1"; break;
                    case "--cancel": a.Cancel = true; break;
                    case "--plan": a.PlanRun = true; break;
                    case "--":
                        if (a.Mode != "smoke-engine") { a.Error = "engine arguments only with --smoke-engine"; break; }
                        a.EngineArgs.AddRange(list.Skip(i + 1));
                        i = list.Count;
                        break;
                    default: a.Error = "unknown argument " + x; break;
                }
                if (a.Error != null) break;
            }
            if (a.Error == null && a.DryRun && a.Mode != "welcome" && a.Mode != "repair") a.Error = "--dry-run only with the welcome or --repair";
            if (a.Error == null && (a.Cancel || a.PlanRun) && a.Mode != "smoke-engine") a.Error = "--cancel and --plan only with --smoke-engine";
            return a;
        }

        void SetMode(string mode)
        {
            if (Mode != "welcome" && Mode != mode) Error = "--" + Mode + " and --" + mode + " together";
            Mode = mode;
        }
    }
}
