// Recorded engine runs for --smoke-render: every screen is drawn from events and results in the engine's own format
// (docs/gui/interfaces-setup.md), so the render gate also exercises EngineRun and ResultView. The shapes follow real
// runs of install.ps1 -Plan -Gui and of the test-engine-events.ps1 fixtures.
using System;
using System.Collections.Generic;
using System.Linq;

namespace AmdgpuWddmSetup
{
    public sealed class Fixture
    {
        public string Name, Flow = "install", RunKind = "install", Package;
        public Screen Screen;
        public string[] PlanEvents, Events;
        public string PlanResult, Result;
        public bool NoResult, RestartLater;
        // The second the restart screen's countdown is drawn at; 0 = the full countdown.
        public int RestartSecondsLeft;
        // The welcome screen of a setup.exe whose files\ folder is not there (somebody copied the exe out of the zip).
        public bool FilesMissing;
        public string PreparedDir, PreparedProblem, PreparedVersion, PrepareDestination, PrepareFirmware, SupportSaved;
        // The turn of the Working screen's tips (SetupGuide.TipId): 1 draws the second tip, the name of Nagi.
        public int TipTurn;
    }

    public static class Fixtures
    {
        public const string Id = "fixture";
        const string Version = "0.7.199.100-tester.11";

        static int _seq;

        static string Ev(string type, string fields)
        {
            _seq++;
            return "{\"schema\":\"" + EngineRun.EventSchema + "\",\"invocation\":\"" + Id + "\",\"seq\":" + _seq + ",\"utc\":\"2026-10-04T08:00:00Z\",\"type\":\"" + type + "\"" + (fields.Length > 0 ? "," + fields : "") + "}";
        }

        static string Check(string id, string result) { return Ev("check", "\"id\":\"" + id + "\",\"result\":\"" + result + "\",\"name\":\"x\",\"detail\":\"fixture\""); }

        public static string ResultJson(string mode, string outcome, int exit, string message, bool mutated, string extra = "", string action = "install")
        {
            return "{\"schema\":\"" + EngineRun.ResultSchema + "\",\"invocation\":\"" + Id + "\",\"engine\":{\"contract\":\"" + EngineRun.Contract + "\",\"package_version\":\"" + Version +
                "\"},\"mode\":\"" + mode + "\",\"dry_run\":" + (mode == "plan" ? "true" : "false") + ",\"action\":\"" + action + "\",\"outcome\":\"" + outcome + "\",\"exit_code\":" + exit +
                ",\"mutated\":" + (mutated ? "true" : "false") + ",\"nothing_changed\":" + (mutated ? "false" : "true") + ",\"message_id\":\"" + message + "\"" + extra + "}";
        }

        // windowsChecks: the lines the Windows support policy can add. "ok" is the ordinary PC. "not-tested" is a Windows 11 older than 22631, an Insider
        // build and an LTSC edition at once, so one screen covers all three information lines.
        // sac: the Smart App Control line (install.ps1): "off" is the ordinary PC, "evaluation" and "unknown" add the
        // note and its help card to the plan screen.
        static string[] PlanEvents(string action, string installed, string consents, string firmware, bool upgradeRows,
                                   string windowsChecks = "ok", string sac = "off")
        {
            _seq = 0;
            var list = new List<string>
            {
                Ev("start", "\"mode\":\"plan\",\"gui\":true,\"dry_run\":true,\"package\":\"P\",\"contract\":\"" + EngineRun.Contract + "\",\"phase\":null"),
                Ev("stage", "\"id\":\"preflight\",\"text\":\"Preflight\""),
                Check("package.ok", "ok"), Check("admin.ok", "ok"),
            };
            if (windowsChecks == "ok") list.Add(Check("windows.ok", "ok"));
            else
            {
                list.Add(Check("windows.below-supported", "warn"));
                list.Add(Check("windows.insider", "warn"));
                list.Add(Check("windows.ltsc", "warn"));
            }
            list.AddRange(new[]
            {
                Check("gpu.ok", "ok"), Check("secureboot.off", "ok"),
                Check("testsigning.inactive", "ok"), Check(upgradeRows ? "bitlocker.on" : "bitlocker.unknown", "warn"), Check("hvci.off", "ok"),
                Check("smartappcontrol." + sac, sac == "off" ? "ok" : "warn"),
                // The Visual C++ runtime: "ok" is a PC that has it. The screen that draws the Windows lines also
                // carries the missing runtime, so one render shows the step the install adds for it.
                Check("vcruntime" + (windowsChecks == "ok" ? ".ok" : ".missing"), windowsChecks == "ok" ? "ok" : "warn"),
                Check("space.ok", "ok"), Check(firmware == "package-folder" ? "firmware.folder-ok" : "firmware.download-ok", "ok"),
                Ev("decision", "\"action\":\"" + action + "\",\"installed_version\":" + (installed == null ? "null" : "\"" + installed + "\"") + ",\"package_version\":\"" + Version +
                    "\",\"phase\":\"new\",\"consents\":[" + consents + "],\"consents_given\":{},\"restarts\":2,\"firmware_source\":\"" + firmware +
                    "\",\"firmware_dir\":\"\",\"notes\":[\"RELEASE-NOTES.md\"],\"secure_boot\":\"off\",\"bitlocker\":\"" + (upgradeRows ? "on" : "unknown") + "\",\"compatibility\":{\"ok\":true,\"reasons\":[]}"),
            });
            var rows = upgradeRows
                ? "{\"group\":\"parameters\",\"name\":\"DpmMaxMHz\",\"decision\":\"kept\",\"current\":1700,\"value\":1500,\"default\":1500,\"present\":true}," +
                  "{\"group\":\"parameters\",\"name\":\"DpmMode\",\"decision\":\"same\",\"current\":1,\"value\":1,\"default\":1,\"present\":true}," +
                  "{\"group\":\"parameters\",\"name\":\"CuMode\",\"decision\":\"kept\",\"current\":40,\"value\":null,\"default\":null,\"present\":true}," +
                  "{\"group\":\"parameters\",\"name\":\"EnableFullWddm\",\"decision\":\"update\",\"current\":1,\"value\":2,\"default\":2,\"present\":true}," +
                  "{\"group\":\"parameters\",\"name\":\"EnableMmio\",\"decision\":\"same\",\"current\":1,\"value\":1,\"default\":1,\"present\":true}," +
                  // BD-069: the upgrade screen of a computer whose driver closed the GPU desktop path itself. The two
                  // switches make one line, and the desktop line says the processor draws it until a repair.
                  "{\"group\":\"parameters\",\"name\":\"EnableGpuPresentBlit\",\"decision\":\"driver-closed\",\"current\":0,\"value\":0,\"default\":1,\"present\":true," +
                  "\"closure\":\"the driver closed the GPU desktop path after a boot that ended with the path in use\"}," +
                  "{\"group\":\"parameters\",\"name\":\"EnableCddDwmInterop\",\"decision\":\"driver-closed\",\"current\":0,\"value\":0,\"default\":1,\"present\":true," +
                  "\"closure\":\"the driver closed the GPU desktop path after a boot that ended with the path in use\"}," +
                  "{\"group\":\"desktop_router\",\"name\":\"DwmForceCpu\",\"decision\":\"same\",\"current\":0,\"value\":0,\"default\":0,\"present\":true}," +
                  "{\"group\":\"app_router\",\"name\":\"Mode\",\"decision\":\"update\",\"current\":\"allowlist\",\"value\":\"gpu-default\",\"default\":\"gpu-default\",\"present\":true}," +
                  "{\"group\":\"d3d12:witcher3.exe\",\"name\":\"Experiment\",\"decision\":\"update\",\"current\":\"present-noprimary\",\"value\":\"present-noprimary,raytracing-tier\",\"default\":\"present-noprimary,raytracing-tier\",\"present\":true}"
                : "{\"group\":\"parameters\",\"name\":\"EnableMmio\",\"decision\":\"set\",\"current\":null,\"value\":1,\"default\":1,\"present\":false}," +
                  "{\"group\":\"parameters\",\"name\":\"EnableGfx\",\"decision\":\"set\",\"current\":null,\"value\":1,\"default\":1,\"present\":false}," +
                  "{\"group\":\"parameters\",\"name\":\"DpmMode\",\"decision\":\"set\",\"current\":null,\"value\":1,\"default\":1,\"present\":false}," +
                  "{\"group\":\"parameters\",\"name\":\"DpmMaxMHz\",\"decision\":\"set\",\"current\":null,\"value\":1500,\"default\":1500,\"present\":false}," +
                  "{\"group\":\"desktop_router\",\"name\":\"DwmForceCpu\",\"decision\":\"set\",\"current\":null,\"value\":0,\"default\":0,\"present\":false}," +
                  "{\"group\":\"app_router\",\"name\":\"Mode\",\"decision\":\"set\",\"current\":null,\"value\":\"gpu-default\",\"default\":\"gpu-default\",\"present\":false}," +
                  "{\"group\":\"d3d12:witcher3.exe\",\"name\":\"Experiment\",\"decision\":\"set\",\"current\":null,\"value\":\"present-noprimary\",\"default\":\"present-noprimary\",\"present\":false}";
            var summary = upgradeRows ? "\"kept\":2,\"updated\":2,\"added\":0,\"unchanged\":3,\"command\":0,\"reopened\":0,\"driver_closed\":2"
                                      : "\"kept\":0,\"updated\":0,\"added\":7,\"unchanged\":0,\"command\":0,\"reopened\":0,\"driver_closed\":0";
            list.Add(Ev("settings-plan", "\"summary\":{" + summary + "},\"rows\":[" + rows + "]"));
            list.Add(Ev("result", "\"outcome\":\"planned\",\"exit_code\":0,\"message_id\":\"result.planned\",\"mutated\":false"));
            return list.ToArray();
        }

        // outcome: the engine's result event, written before its process ends. The stage screen is drawn once more
        // with it, so the fixtures cover both sides of that branch: an ending that finished and one that stopped.
        static string[] RunEvents(string lastStage, bool cancel, bool mutated, string mode = "run", string outcome = null, bool repaired = false)
        {
            _seq = 0;
            var list = new List<string> { Ev("start", "\"mode\":\"" + mode + "\",\"gui\":true,\"dry_run\":false,\"package\":\"P\",\"contract\":\"" + EngineRun.Contract + "\",\"phase\":\"new\"") };
            var stages = mode == "prepare-offline" ? new[] { "prepare-check", "firmware", "copy", "finish" } : new[] { "preflight", "test-signing", "install", "firmware", "files", "driver", "settings", "finish" };
            foreach (var s in stages)
            {
                list.Add(Ev("stage", "\"id\":\"" + s + "\",\"text\":\"x\""));
                if (s == "preflight" && mode != "prepare-offline") list.Add(Ev("cancel", "\"available\":" + (cancel ? "true" : "false") + ",\"where\":\"before-changes\""));
                if (s == lastStage) break;
            }
            if (mutated) { list.Add(Ev("install-action", "\"action\":\"install\",\"boot_id\":41")); list.Add(Ev("step", "\"description\":\"x\",\"dry_run\":false")); }
            if (repaired) list.Add(Ev("pending-removals-cancelled", "\"count\":4"));
            if (outcome != null)
                list.Add(Ev("result", "\"outcome\":\"" + outcome + "\",\"exit_code\":0,\"message_id\":\"result.other\",\"mutated\":" + (mutated ? "true" : "false")));
            return list.ToArray();
        }

        public static EngineRun Run(string[] lines)
        {
            var r = new EngineRun(Id);
            foreach (var l in lines) r.Apply(l);
            return r;
        }

        public static EngineResult Result(string json)
        {
            if (json == null) return null;
            string problem;
            return EngineResult.Read(json, Id, out problem);
        }

        public static List<Fixture> All()
        {
            // No consent is asked for any more, so the engine's decision names none: these
            // fixtures carry the empty list the engine now writes, and the plan screens below hold no control that
            // asks the user for one. A BitLocker state of "on" or "unknown" is information on the screen, not a
            // question: the upgrade fixture carries "on", the install one "unknown" (PlanEvents).
            var planInstall = PlanEvents("install", null, "", "download", false);
            var planUpgrade = PlanEvents("upgrade", "0.7.198.100-tester.10", "", "package-folder", true);
            var planned = ResultJson("plan", "planned", 0, "result.planned", false);
            return new List<Fixture>
            {
                new Fixture { Name = "welcome", Screen = Screen.Welcome },
                new Fixture { Name = "welcome-prepared", Screen = Screen.Welcome, PreparedDir = @"D:\amdgpu-wddm-offline", PreparedVersion = Version },
                new Fixture { Name = "welcome-prepared-bad", Screen = Screen.Welcome, PreparedDir = @"D:\Downloads", PreparedProblem = "folder.not-prepared" },
                new Fixture { Name = "welcome-repair", Flow = "repair", Screen = Screen.Welcome },
                new Fixture { Name = "welcome-files-missing", Screen = Screen.Welcome, FilesMissing = true },
                // setup.exe without its files\ folder, but a prepared folder is chosen: that folder brings the files,
                // so the screen must not say they are missing while the check button is on.
                new Fixture { Name = "welcome-files-missing-prepared", Screen = Screen.Welcome, FilesMissing = true, PreparedDir = @"D:\amdgpu-wddm-offline", PreparedVersion = Version },
                new Fixture { Name = "checking", RunKind = "plan", Screen = Screen.Checking, Events = PlanEvents("install", null, "", "download", false).Take(8).ToArray() },
                new Fixture { Name = "plan-install", RunKind = "plan", Screen = Screen.Plan, PlanEvents = planInstall, PlanResult = planned },
                new Fixture { Name = "plan-upgrade", RunKind = "plan", Screen = Screen.Plan, PlanEvents = planUpgrade, PlanResult = planned },
                new Fixture { Name = "plan-already", RunKind = "plan", Screen = Screen.Plan, PlanEvents = PlanEvents("already", Version, "", "download", false), PlanResult = planned },
                // A Windows the driver is not tested on: older than 23H2, an Insider build and an LTSC edition. The
                // three lines are information and the Install button stays on.
                new Fixture { Name = "plan-windows-not-tested", RunKind = "plan", Screen = Screen.Plan,
                    PlanEvents = PlanEvents("install", null, "", "download", false, "not-tested"), PlanResult = planned },
                // Smart App Control in evaluation mode, and in a state setup could not read: a note each, with the
                // help card under the checks, and the Install button stays on.
                new Fixture { Name = "plan-sac-evaluation", RunKind = "plan", Screen = Screen.Plan,
                    PlanEvents = PlanEvents("install", null, "", "download", false, "ok", "evaluation"), PlanResult = planned },
                new Fixture { Name = "plan-sac-unknown", RunKind = "plan", Screen = Screen.Plan,
                    PlanEvents = PlanEvents("install", null, "", "download", false, "ok", "unknown"), PlanResult = planned },
                new Fixture { Name = "working", Screen = Screen.Working, Events = RunEvents("files", false, true) },
                new Fixture { Name = "working-reinstalled", Screen = Screen.Working, Events = RunEvents("files", false, true, repaired: true) },
                new Fixture { Name = "restart-reinstalled", Screen = Screen.Restart, Events = RunEvents("finish", false, true, repaired: true),
                    Result = ResultJson("run", "restart-required", 0, "result.installed-restart", true) },
                new Fixture { Name = "working-cancel", Screen = Screen.Working, Events = RunEvents("preflight", true, false) },
                new Fixture { Name = "continue", Flow = "continue", Screen = Screen.Working, Events = RunEvents("driver", false, true) },
                // The stage screen after the engine wrote its result and before its process ended: a restart is a
                // normal ending (the stage is done), a failure is not (the stage carries the stopped mark).
                new Fixture { Name = "working-restart", Screen = Screen.Working, Events = RunEvents("test-signing", false, true, "run", "restart-required"),
                    Result = ResultJson("run", "restart-required", 0, "result.restart-test-signing", true, ",\"restart\":{\"required\":true,\"reason_id\":\"restart.test-signing\"}") },
                new Fixture { Name = "working-stopped", Screen = Screen.Working, Events = RunEvents("driver", false, true, "run", "failed"),
                    Result = ResultJson("run", "failed", 6, "result.step-failed", true, ",\"step\":\"x\",\"detail\":\"x\"") },
                new Fixture { Name = "working-name-tip", Screen = Screen.Working, Events = RunEvents("files", false, true), TipTurn = 1 },
                new Fixture { Name = "restart", Screen = Screen.Restart, Events = RunEvents("test-signing", false, true), Result = ResultJson("run", "restart-required", 0, "result.restart-test-signing", true, ",\"restart\":{\"required\":true,\"reason_id\":\"restart.test-signing\"}") },
                // The countdown of the restart screen a few seconds in, so that a render shows the running number
                // and not only its first value.
                new Fixture { Name = "restart-countdown", Screen = Screen.Restart, RestartSecondsLeft = 7, Events = RunEvents("test-signing", false, true), Result = ResultJson("run", "restart-required", 0, "result.restart-test-signing", true, ",\"restart\":{\"required\":true,\"reason_id\":\"restart.test-signing\"}") },
                new Fixture { Name = "restart-later", Screen = Screen.Restart, RestartLater = true, Events = RunEvents("finish", false, true), Result = ResultJson("run", "restart-required", 0, "result.installed-restart", true) },
                new Fixture { Name = "result-verified", Flow = "continue", Screen = Screen.Result, Events = RunEvents("preflight", false, false, "verify"), Result = ResultJson("verify", "verified", 0, "result.verified", false) },
                new Fixture { Name = "result-refused", RunKind = "plan", Screen = Screen.Result, Events = PlanEvents("install", null, "", "download", false).Take(6).ToArray(), Result = ResultJson("plan", "refused", 2, "result.preflight-refused", false, ",\"failed_checks\":[\"gpu.missing\",\"secureboot.on\",\"vcruntime.no-installer\"]") },
                // Smart App Control on: the plan is refused before any change, the help card says how to turn it off,
                // and Try again stays the default (it is not in ResultView.RetryCannotFix).
                new Fixture { Name = "result-refused-sac", RunKind = "plan", Screen = Screen.Result, Events = PlanEvents("install", null, "", "download", false).Take(6).ToArray(), Result = ResultJson("plan", "refused", 2, "result.preflight-refused", false, ",\"failed_checks\":[\"smartappcontrol.on\"]") },
                // gui-L4 trials: this version installed and checked, refused because of a development installation.
                new Fixture { Name = "result-refused-lab", RunKind = "plan", Screen = Screen.Result, Events = PlanEvents("install", null, "", "download", false).Take(6).ToArray(), Result = ResultJson("plan", "refused", 2, "result.preflight-refused", false, ",\"failed_checks\":[\"lab-install.present\"]", "already") },
                new Fixture { Name = "result-failed", Screen = Screen.Result, Events = RunEvents("driver", false, true), Result = ResultJson("run", "failed", 6, "result.step-failed", true, ",\"step\":\"pnputil /add-driver\",\"detail\":\"exit 5\"") },
                new Fixture { Name = "result-none", Screen = Screen.Result, Events = RunEvents("files", false, true), NoResult = true },
                new Fixture { Name = "result-cancelled", Screen = Screen.Result, Events = RunEvents("preflight", false, false), Result = ResultJson("run", "cancelled", 8, "result.cancelled", false), SupportSaved = @"C:\Users\Public\Desktop\amdgpu-wddm-setup.zip" },
                new Fixture { Name = "prepare", Flow = "prepare", RunKind = "prepare", Screen = Screen.Prepare },
                new Fixture { Name = "prepare-firmware", Flow = "prepare", RunKind = "prepare", Screen = Screen.Prepare, PrepareDestination = @"E:\amdgpu-wddm-offline", PrepareFirmware = @"E:\firmware" },
                new Fixture { Name = "prepare-working", Flow = "prepare", RunKind = "prepare", Screen = Screen.Working, Events = RunEvents("copy", false, false, "prepare-offline") },
                new Fixture { Name = "prepare-done", Flow = "prepare", RunKind = "prepare", Screen = Screen.Result, Events = RunEvents("finish", false, false, "prepare-offline"), Result = ResultJson("prepare-offline", "prepared", 0, "result.offline-prepared", false) },
            };
        }
    }
}
