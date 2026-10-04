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
        public bool NoResult, ConsentTestSigning, RestartLater;
        public string BitLocker, PreparedDir, PreparedProblem, PreparedVersion, PrepareDestination, PrepareFirmware, SupportSaved;
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

        public static string ResultJson(string mode, string outcome, int exit, string message, bool mutated, string extra = "")
        {
            return "{\"schema\":\"" + EngineRun.ResultSchema + "\",\"invocation\":\"" + Id + "\",\"engine\":{\"contract\":\"" + EngineRun.Contract + "\",\"package_version\":\"" + Version +
                "\"},\"mode\":\"" + mode + "\",\"dry_run\":" + (mode == "plan" ? "true" : "false") + ",\"action\":\"install\",\"outcome\":\"" + outcome + "\",\"exit_code\":" + exit +
                ",\"mutated\":" + (mutated ? "true" : "false") + ",\"nothing_changed\":" + (mutated ? "false" : "true") + ",\"message_id\":\"" + message + "\"" + extra + "}";
        }

        static string[] PlanEvents(string action, string installed, string consents, string firmware, bool upgradeRows)
        {
            _seq = 0;
            var list = new List<string>
            {
                Ev("start", "\"mode\":\"plan\",\"gui\":true,\"dry_run\":true,\"package\":\"P\",\"contract\":\"" + EngineRun.Contract + "\",\"phase\":null"),
                Ev("stage", "\"id\":\"preflight\",\"text\":\"Preflight\""),
                Check("package.ok", "ok"), Check("admin.ok", "ok"), Check("windows.ok", "ok"), Check("gpu.ok", "ok"), Check("secureboot.off", "ok"),
                Check("testsigning.inactive", "ok"), Check(upgradeRows ? "bitlocker.on" : "bitlocker.unknown", "warn"), Check("hvci.off", "ok"),
                Check("vcruntime.ok", "ok"), Check("space.ok", "ok"), Check(firmware == "package-folder" ? "firmware.folder-ok" : "firmware.download-ok", "ok"),
                Ev("decision", "\"action\":\"" + action + "\",\"installed_version\":" + (installed == null ? "null" : "\"" + installed + "\"") + ",\"package_version\":\"" + Version +
                    "\",\"phase\":\"new\",\"consents\":[" + consents + "],\"consents_given\":{},\"restarts\":2,\"firmware_source\":\"" + firmware +
                    "\",\"firmware_dir\":\"\",\"notes\":[\"RELEASE-NOTES.md\"],\"secure_boot\":\"off\",\"bitlocker\":\"unknown\",\"compatibility\":{\"ok\":true,\"reasons\":[]}"),
            };
            var rows = upgradeRows
                ? "{\"group\":\"parameters\",\"name\":\"DpmMaxMHz\",\"decision\":\"kept\",\"current\":1700,\"value\":1500,\"default\":1500,\"present\":true}," +
                  "{\"group\":\"parameters\",\"name\":\"DpmMode\",\"decision\":\"same\",\"current\":1,\"value\":1,\"default\":1,\"present\":true}," +
                  "{\"group\":\"parameters\",\"name\":\"CuMode\",\"decision\":\"kept\",\"current\":40,\"value\":null,\"default\":null,\"present\":true}," +
                  "{\"group\":\"parameters\",\"name\":\"EnableFullWddm\",\"decision\":\"update\",\"current\":1,\"value\":2,\"default\":2,\"present\":true}," +
                  "{\"group\":\"parameters\",\"name\":\"EnableMmio\",\"decision\":\"same\",\"current\":1,\"value\":1,\"default\":1,\"present\":true}," +
                  "{\"group\":\"desktop_router\",\"name\":\"DwmForceCpu\",\"decision\":\"same\",\"current\":0,\"value\":0,\"default\":0,\"present\":true}," +
                  "{\"group\":\"d3d12:witcher3.exe\",\"name\":\"Experiment\",\"decision\":\"update\",\"current\":\"present-noprimary\",\"value\":\"present-noprimary,raytracing-tier\",\"default\":\"present-noprimary,raytracing-tier\",\"present\":true}"
                : "{\"group\":\"parameters\",\"name\":\"EnableMmio\",\"decision\":\"set\",\"current\":null,\"value\":1,\"default\":1,\"present\":false}," +
                  "{\"group\":\"parameters\",\"name\":\"EnableGfx\",\"decision\":\"set\",\"current\":null,\"value\":1,\"default\":1,\"present\":false}," +
                  "{\"group\":\"parameters\",\"name\":\"DpmMode\",\"decision\":\"set\",\"current\":null,\"value\":1,\"default\":1,\"present\":false}," +
                  "{\"group\":\"parameters\",\"name\":\"DpmMaxMHz\",\"decision\":\"set\",\"current\":null,\"value\":1500,\"default\":1500,\"present\":false}," +
                  "{\"group\":\"desktop_router\",\"name\":\"DwmForceCpu\",\"decision\":\"set\",\"current\":null,\"value\":0,\"default\":0,\"present\":false}," +
                  "{\"group\":\"app_router\",\"name\":\"Mode\",\"decision\":\"set\",\"current\":null,\"value\":\"allowlist\",\"default\":\"allowlist\",\"present\":false}," +
                  "{\"group\":\"d3d12:witcher3.exe\",\"name\":\"Experiment\",\"decision\":\"set\",\"current\":null,\"value\":\"present-noprimary\",\"default\":\"present-noprimary\",\"present\":false}";
            var summary = upgradeRows ? "\"kept\":2,\"updated\":2,\"added\":0,\"unchanged\":3,\"command\":0" : "\"kept\":0,\"updated\":0,\"added\":7,\"unchanged\":0,\"command\":0";
            list.Add(Ev("settings-plan", "\"summary\":{" + summary + "},\"rows\":[" + rows + "]"));
            list.Add(Ev("result", "\"outcome\":\"planned\",\"exit_code\":0,\"message_id\":\"result.planned\",\"mutated\":false"));
            return list.ToArray();
        }

        static string[] RunEvents(string lastStage, bool cancel, bool mutated, string mode = "run")
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
            var planInstall = PlanEvents("install", null, "\"test-signing\"", "download", false);
            var planUpgrade = PlanEvents("upgrade", "0.7.198.100-tester.10", "\"test-signing\",\"bitlocker\"", "package-folder", true);
            var planned = ResultJson("plan", "planned", 0, "result.planned", false, ",\"consents_needed\":[\"test-signing\"]");
            return new List<Fixture>
            {
                new Fixture { Name = "welcome", Screen = Screen.Welcome },
                new Fixture { Name = "welcome-prepared", Screen = Screen.Welcome, PreparedDir = @"D:\amdgpu-wddm-offline", PreparedVersion = Version },
                new Fixture { Name = "welcome-prepared-bad", Screen = Screen.Welcome, PreparedDir = @"D:\Downloads", PreparedProblem = "folder.not-prepared" },
                new Fixture { Name = "welcome-repair", Flow = "repair", Screen = Screen.Welcome },
                new Fixture { Name = "checking", RunKind = "plan", Screen = Screen.Checking, Events = PlanEvents("install", null, "", "download", false).Take(8).ToArray() },
                new Fixture { Name = "plan-install", RunKind = "plan", Screen = Screen.Plan, PlanEvents = planInstall, PlanResult = planned },
                new Fixture { Name = "plan-upgrade", RunKind = "plan", Screen = Screen.Plan, PlanEvents = planUpgrade, PlanResult = planned, ConsentTestSigning = true, BitLocker = "Suspend" },
                new Fixture { Name = "plan-already", RunKind = "plan", Screen = Screen.Plan, PlanEvents = PlanEvents("already", Version, "", "download", false), PlanResult = planned },
                new Fixture { Name = "working", Screen = Screen.Working, Events = RunEvents("files", false, true) },
                new Fixture { Name = "working-cancel", Screen = Screen.Working, Events = RunEvents("preflight", true, false) },
                new Fixture { Name = "continue", Flow = "continue", Screen = Screen.Working, Events = RunEvents("driver", false, true) },
                new Fixture { Name = "restart", Screen = Screen.Restart, Events = RunEvents("test-signing", false, true), Result = ResultJson("run", "restart-required", 0, "result.restart-test-signing", true, ",\"restart\":{\"required\":true,\"reason_id\":\"restart.test-signing\"}") },
                new Fixture { Name = "restart-later", Screen = Screen.Restart, RestartLater = true, Events = RunEvents("finish", false, true), Result = ResultJson("run", "restart-required", 0, "result.installed-restart", true) },
                new Fixture { Name = "result-verified", Flow = "continue", Screen = Screen.Result, Events = RunEvents("preflight", false, false, "verify"), Result = ResultJson("verify", "verified", 0, "result.verified", false) },
                new Fixture { Name = "result-refused", RunKind = "plan", Screen = Screen.Result, Events = PlanEvents("install", null, "", "download", false).Take(6).ToArray(), Result = ResultJson("plan", "refused", 2, "result.preflight-refused", false, ",\"failed_checks\":[\"gpu.missing\",\"secureboot.on\",\"vcruntime.missing\"]") },
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
