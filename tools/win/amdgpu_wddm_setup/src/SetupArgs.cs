// The setup window's command line (docs/gui/interfaces-setup.md section 8), parsed without side effects.
//
//   amdgpu_wddm_setup.exe                     welcome: install or update from this package or a prepared folder
//   amdgpu_wddm_setup.exe --continue          after a restart (RunOnce): the engine continues at once
//   amdgpu_wddm_setup.exe --repair            from the control app's Help (Release\RepairSetup): repair
//   amdgpu_wddm_setup.exe --prepare-offline   prepare a folder for a PC without internet (no administrator needed)
//   [--no-start-menu-shortcut]                the Start menu option of the plan screen starts off (default: on)
//   [--no-desktop-shortcut]                   the desktop option of the plan screen starts off (default: on)
//   [--package <dir>]                         another package folder than the one the exe is in
//   [--run-root <dir>]                        where the engine runs keep their files (default %TEMP%\amdgpu-wddm-setup)
//   [--dry-run]                               the install run is an engine dry run: every screen, no change (L4)
//   amdgpu_wddm_setup.exe --version
//   amdgpu_wddm_setup.exe --smoke-render <dir> <scale> <lang> [--text-scale <f>] [--no-nagi] [--high-contrast]
//                         [--work-area <w>x<h>]
//                         [--package <dir>]    no window: every screen from fixtures drawn to <dir>, checked
//                                             (--no-nagi: the guide panel without the guide character;
//                                             --high-contrast: the high-contrast palette on a PC without it;
//                                             --work-area: the working area of the screen the window is drawn for,
//                                             in logical units, so that a render never reads this PC's screen)
//   amdgpu_wddm_setup.exe --smoke-engine <package> <out-dir> [--script <name>] [--cancel] [--plan] -- <engine args>
//                                             no window: one engine run, its model and screens summarised
//   amdgpu_wddm_setup.exe --smoke-start-failure <out-dir>
//                                             no window: plan, install and prepare with an engine that cannot start
//   amdgpu_wddm_setup.exe --smoke-payload <out-dir>
//                                             no window: the package this exe carries (Payload.cs) is read, checked
//                                             and unpacked into <out-dir>, with a summary in <out-dir>\payload.txt.
//                                             The folder is a plain one, so the reader can be tested without
//                                             administrator rights; an install always unpacks into the protected
//                                             %ProgramData%\amdgpu-wddm\setup instead
//   amdgpu_wddm_setup.exe --smoke-loading <out-dir>
//                                             no window: the loading window (LoadingWindow.cs) in each of its stages,
//                                             four languages, four scales, three text sizes and two screens, checked;
//                                             findings in <out-dir>\loading.txt
//   (internal) --loading-handover <name>     given by the copy that asks for administrator rights to the copy it
//                                             starts: the name of the event the new copy sets when its own loading
//                                             window is on the screen, so the first one can close its window then
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
        // Set by Program when this exe carried the package inside itself and unpacked it (Payload.cs): the folder
        // the window removes when the run ends with no restart pending. Never a command-line option.
        public string PayloadPackage;
        // The event of the copy that started this one as administrator (--loading-handover); null when none did.
        public string Handover;
        public const string HandoverPrefix = "amdgpu-wddm-setup-loading-";
        public bool DryRun, ApplySystemTuning;
        // The two shortcut options of the plan screen, both on unless a switch states otherwise.
        // null is "nothing was stated": the screen opens the box on the answer the engine reports, and the run that
        // continues an install after a restart passes no switch at all, so the saved answer stands (BD: a positive
        // switch clears the saved negative one, which turned an unticked box back on at the restart).
        public bool? StartMenuShortcut, DesktopShortcut;
        // The Nagi wallpaper box of the same card, on unless a switch or the person says no;
        // null in the same way as the two shortcut answers.
        public bool? Wallpaper;
        // The picture on the start screen, the same box and the same null rule as the wallpaper.
        public bool? BootLogo;
        public readonly List<string> EngineArgs = new List<string>();
        // smoke entries
        public string SmokeDir, Language = "en", Script = "install.ps1";
        public float Scale = 1f, TextScale = 1f;
        public bool Cancel, PlanRun;
        public bool NoNagi;             // render the guide panel without the guide character (she is on by default)
        public bool HighContrast;       // draw with the system colours of high contrast, which this PC may not have on
        // The working area of the screen the window is drawn for, in logical units; 0x0 = ask this PC's screen. The
        // small-screen rule and the window's own size read it, so one render is the same picture on any PC.
        public int WorkWidth, WorkHeight;

        public bool Headless { get { return Mode == "smoke-render" || Mode == "smoke-engine" || Mode == "smoke-start-failure" || Mode == "smoke-payload" || Mode == "smoke-loading" || Mode == "smoke-progress" || Mode == "version"; } }

        // A handover name is this program's own: the prefix and 32 hex digits, so a name from the command line can
        // never reach another program's object.
        public static bool HandoverName(string name)
        {
            if (name == null || !name.StartsWith(HandoverPrefix, StringComparison.Ordinal) || name.Length != HandoverPrefix.Length + 32) return false;
            for (int k = HandoverPrefix.Length; k < name.Length; k++)
                if ("0123456789abcdef".IndexOf(name[k]) < 0) return false;
            return true;
        }

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
                    case "--apply-system-tuning": a.ApplySystemTuning = true; break;
                    case "--no-start-menu-shortcut": a.StartMenuShortcut = false; break;
                    case "--start-menu-shortcut": a.StartMenuShortcut = true; break;
                    case "--no-desktop-shortcut": a.DesktopShortcut = false; break;
                    case "--desktop-shortcut": a.DesktopShortcut = true; break;
                    case "--no-wallpaper": a.Wallpaper = false; break;
                    case "--wallpaper": a.Wallpaper = true; break;
                    case "--no-boot-logo": a.BootLogo = false; break;
                    case "--boot-logo": a.BootLogo = true; break;
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
                    case "--smoke-start-failure":
                        a.SetMode("smoke-start-failure");
                        a.SmokeDir = next();
                        if (a.SmokeDir == null) a.Error = "--smoke-start-failure <out-dir>";
                        break;
                    case "--smoke-payload":
                        a.SetMode("smoke-payload");
                        a.SmokeDir = next();
                        if (a.SmokeDir == null) a.Error = "--smoke-payload <out-dir>";
                        break;
                    case "--smoke-loading":
                        a.SetMode("smoke-loading");
                        a.SmokeDir = next();
                        if (a.SmokeDir == null) a.Error = "--smoke-loading <out-dir>";
                        break;
                    case "--smoke-progress":
                        a.SetMode("smoke-progress");
                        a.SmokeDir = next();
                        if (a.SmokeDir == null) a.Error = "--smoke-progress <out-dir>";
                        break;
                    case "--loading-handover":
                        a.Handover = next();
                        if (!HandoverName(a.Handover)) a.Error = "--loading-handover needs a name of this program";
                        break;
                    case "--script": a.Script = next(); if (a.Script != "install.ps1" && a.Script != "prepare-offline.ps1") a.Error = "--script install.ps1 or prepare-offline.ps1"; break;
                    case "--no-nagi": a.NoNagi = true; break;
                    case "--work-area":
                        var area = (next() ?? "").Split('x');
                        int aw, ah;
                        if (area.Length != 2 || !int.TryParse(area[0], NumberStyles.Integer, CultureInfo.InvariantCulture, out aw) ||
                            !int.TryParse(area[1], NumberStyles.Integer, CultureInfo.InvariantCulture, out ah) || aw < 320 || ah < 240 || aw > 16384 || ah > 16384)
                            a.Error = "--work-area <width>x<height>, at least 320x240";
                        else { a.WorkWidth = aw; a.WorkHeight = ah; }
                        break;
                    case "--high-contrast": a.HighContrast = true; break;
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
            if (a.Error == null && a.NoNagi && a.Mode != "smoke-render") a.Error = "--no-nagi only with --smoke-render";
            if (a.Error == null && a.HighContrast && a.Mode != "smoke-render") a.Error = "--high-contrast only with --smoke-render";
            if (a.Error == null && a.WorkWidth > 0 && a.Mode != "smoke-render") a.Error = "--work-area only with --smoke-render";
            if (a.Error == null && a.Handover != null && a.Headless) a.Error = "--loading-handover only with a window";
            return a;
        }

        public static string[] TuningArguments(bool? choice)
        {
            return choice.HasValue ? new[] { choice.Value ? "-ApplySystemTuning" : "-SkipSystemTuning" } : new string[0];
        }

        void SetMode(string mode)
        {
            if (Mode != "welcome" && Mode != mode) Error = "--" + Mode + " and --" + mode + " together";
            Mode = mode;
        }
    }
}
