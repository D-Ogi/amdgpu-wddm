// What the setup window knows about one engine run (docs/gui/interfaces-setup.md sections 2 and 3), built from the
// engine's events and its terminal result only. Pure: no file, process or window access, so the unit tests feed it
// recorded lines. The window shows plain words chosen by ids; the technical fields (detail, step, log) go only into
// the support file.
using System;
using System.Collections.Generic;
using System.Linq;

namespace AmdgpuWddmSetup
{
    public sealed class CheckRow
    {
        public string Id, Result, Name, Detail;
    }

    public sealed class SettingRow
    {
        public string Group, Name, Decision;
        public object Current, Value, Default;
        public bool Present;
    }

    public sealed class EngineDecision
    {
        public string Action, InstalledVersion, PackageVersion, Phase, FirmwareSource, FirmwareDir, SecureBoot, BitLocker;
        public string[] Consents = new string[0], Notes = new string[0], CompatibilityReasons = new string[0];
        public int Restarts;
        public bool CompatibilityOk;
    }

    public sealed class SettingsPlan
    {
        public int Kept, Updated, Added, Unchanged, Command;
        public readonly List<SettingRow> Rows = new List<SettingRow>();
    }

    public sealed class EngineResult
    {
        public string Invocation, Mode, Action, Outcome, MessageId, Step, Detail, Log, PhaseBefore, PhaseAfter, PackageVersion;
        public string RestartReason, ContinuationCommand;
        public int ExitCode;
        public bool DryRun, Mutated, NothingChanged, RestartRequired;
        public string[] ConsentsNeeded = new string[0], FailedChecks = new string[0];

        // The result file, believed only when it carries this window's invocation id and the known schema.
        public static EngineResult Read(string text, string invocation, out string problem)
        {
            problem = null;
            var o = Json.Parse(text);
            if (o == null) { problem = "the result file is missing or not JSON"; return null; }
            if (Json.Str(o, "schema") != EngineRun.ResultSchema) { problem = "unknown result schema " + Json.Str(o, "schema"); return null; }
            if (string.IsNullOrEmpty(invocation) || Json.Str(o, "invocation") != invocation) { problem = "the result belongs to another run (" + Json.Str(o, "invocation") + ")"; return null; }
            var outcome = Json.Str(o, "outcome");
            if (string.IsNullOrEmpty(outcome)) { problem = "the result has no outcome"; return null; }
            var restart = Json.Obj(o, "restart");
            var cont = Json.Obj(restart, "continuation");
            return new EngineResult
            {
                Invocation = invocation, Mode = Json.Str(o, "mode"), Action = Json.Str(o, "action"), Outcome = outcome,
                MessageId = Json.Str(o, "message_id"), Step = Json.Str(o, "step"), Detail = Json.Str(o, "detail"), Log = Json.Str(o, "log"),
                PhaseBefore = Json.Str(o, "phase_before"), PhaseAfter = Json.Str(o, "phase_after"),
                PackageVersion = Json.Str(Json.Obj(o, "engine"), "package_version"),
                ExitCode = Json.Int(o, "exit_code", -1), DryRun = Json.Bool(o, "dry_run"), Mutated = Json.Bool(o, "mutated"),
                NothingChanged = Json.Bool(o, "nothing_changed"),
                RestartRequired = Json.Bool(restart, "required"), RestartReason = Json.Str(restart, "reason_id"),
                ContinuationCommand = Json.Str(cont, "command"),
                ConsentsNeeded = Json.Strs(o, "consents_needed"), FailedChecks = Json.Strs(o, "failed_checks"),
            };
        }
    }

    public sealed class EngineRun
    {
        public const string EventSchema = "amdgpu-wddm.engine-event/1";
        public const string ResultSchema = "amdgpu-wddm.engine-result/1";
        public const string Contract = "amdgpu-wddm.engine/1";

        public readonly string Invocation;
        public string Mode, Package, StartPhase, EngineContract, CurrentStage, CancelWhere, RestartReason, Continuation;
        public bool Started, Gui, DryRun, CancelAvailable, InstallActionSeen, WitnessWritten;
        public int Steps, RealSteps, LastSeq, Ignored;
        public readonly List<string> Stages = new List<string>();
        public readonly List<CheckRow> Checks = new List<CheckRow>();
        public EngineDecision Decision;
        public SettingsPlan Settings;
        public string ResultOutcome, ResultMessageId;
        public readonly List<string> Problems = new List<string>();

        public EngineRun(string invocation) { Invocation = invocation; }

        // One line of the events file. Lines of another run or another schema are counted and skipped; a sequence that
        // does not grow is a problem (recorded for the support file) but the event is still used.
        public void Apply(string line)
        {
            var e = Json.Parse(line);
            if (e == null || Json.Str(e, "schema") != EventSchema || Json.Str(e, "invocation") != Invocation) { Ignored++; return; }
            int seq = Json.Int(e, "seq");
            if (seq <= LastSeq) Problems.Add("event " + seq + " after " + LastSeq);
            LastSeq = Math.Max(LastSeq, seq);
            switch (Json.Str(e, "type"))
            {
                case "start":
                    Started = true; Mode = Json.Str(e, "mode"); Gui = Json.Bool(e, "gui"); DryRun = Json.Bool(e, "dry_run");
                    Package = Json.Str(e, "package"); StartPhase = Json.Str(e, "phase"); EngineContract = Json.Str(e, "contract");
                    if (EngineContract != Contract) Problems.Add("engine contract " + EngineContract);
                    break;
                case "stage":
                    CurrentStage = Json.Str(e, "id");
                    if (CurrentStage != null && !Stages.Contains(CurrentStage)) Stages.Add(CurrentStage);
                    break;
                case "check":
                    Checks.Add(new CheckRow { Id = Json.Str(e, "id"), Result = Json.Str(e, "result"), Name = Json.Str(e, "name"), Detail = Json.Str(e, "detail") });
                    break;
                case "decision":
                    var c = Json.Obj(e, "compatibility");
                    Decision = new EngineDecision
                    {
                        Action = Json.Str(e, "action"), InstalledVersion = Json.Str(e, "installed_version"), PackageVersion = Json.Str(e, "package_version"),
                        Phase = Json.Str(e, "phase"), Consents = Json.Strs(e, "consents"), Restarts = Json.Int(e, "restarts"),
                        FirmwareSource = Json.Str(e, "firmware_source"), FirmwareDir = Json.Str(e, "firmware_dir"), Notes = Json.Strs(e, "notes"),
                        SecureBoot = Json.Str(e, "secure_boot"), BitLocker = Json.Str(e, "bitlocker"),
                        CompatibilityOk = c != null && Json.Bool(c, "ok"), CompatibilityReasons = Json.Strs(c, "reasons"),
                    };
                    break;
                case "settings-plan":
                    var s = Json.Obj(e, "summary");
                    Settings = new SettingsPlan { Kept = Json.Int(s, "kept"), Updated = Json.Int(s, "updated"), Added = Json.Int(s, "added"), Unchanged = Json.Int(s, "unchanged"), Command = Json.Int(s, "command") };
                    foreach (var r in Json.Arr(e, "rows").OfType<IDictionary<string, object>>())
                    {
                        object cur, val, def;
                        r.TryGetValue("current", out cur); r.TryGetValue("value", out val); r.TryGetValue("default", out def);
                        Settings.Rows.Add(new SettingRow { Group = Json.Str(r, "group"), Name = Json.Str(r, "name"), Decision = Json.Str(r, "decision"), Current = cur, Value = val, Default = def, Present = Json.Bool(r, "present") });
                    }
                    break;
                case "cancel":
                    CancelAvailable = Json.Bool(e, "available"); CancelWhere = Json.Str(e, "where");
                    break;
                case "install-action":
                    InstallActionSeen = true;
                    break;
                case "step":
                    Steps++;
                    if (!Json.Bool(e, "dry_run")) RealSteps++;
                    break;
                case "restart-required":
                    RestartReason = Json.Str(e, "reason_id"); Continuation = Json.Str(e, "continuation");
                    break;
                case "witness":
                    WitnessWritten = Json.Bool(e, "written");
                    break;
                case "result":
                    ResultOutcome = Json.Str(e, "outcome"); ResultMessageId = Json.Str(e, "message_id"); CancelAvailable = false;
                    break;
            }
        }

        // Whether the run may have changed this PC, as far as the events tell (a missing result cannot say more).
        public bool MayHaveChanged { get { return InstallActionSeen || RealSteps > 0; } }

        // The stages a run of this mode goes through, in order, for the progress list.
        public static string[] ExpectedStages(string mode, string action)
        {
            if (mode == "prepare-offline") return new[] { "prepare-check", "firmware", "copy", "finish" };
            if (action == "verify") return new[] { "preflight", "verify" };
            return new[] { "preflight", "test-signing", "install", "firmware", "files", "driver", "settings", "finish" };
        }
    }

    public enum ViewKind { Plan, Restart, Success, Information, Problem, Cancelled }

    // What the window shows for a finished run. Text ids only; SetupForm turns them into words.
    public sealed class ResultView
    {
        public ViewKind Kind;
        public string TitleId, BodyId;
        public bool NothingChanged;         // A3: "Nothing was changed." only when the bound result says so
        public bool ChangesMade;            // some changes were made before the stop (no promise to undo them)
        public bool ChangesUnknown;         // no bound result: setup cannot tell
        public bool OfferRestart, OfferRetry, OfferRepair;
        public string[] FailedChecks = new string[0];
        public string[] ConsentsNeeded = new string[0];

        static readonly HashSet<string> Known = new HashSet<string>(StringComparer.Ordinal)
        {
            "result.dry-run-complete", "result.dry-run-verify", "result.already", "result.restart-test-signing", "result.restart-driver",
            "result.installed-restart", "result.testsigning-not-active", "result.testsigning-secureboot", "result.restart-still-pending",
            "result.verify-before-restart", "result.verified", "result.verified-with-warnings", "result.preflight-refused",
            "result.preflight-error", "result.needs-admin", "result.package-damaged", "result.firmware-folder-bad",
            "result.firmware-unreachable", "result.prepare-destination", "result.busy", "result.verify-failed", "result.step-failed",
            "result.cancelled", "result.cancelled-after-changes", "result.deadline", "result.deadline-after-changes", "result.offline-prepared",
            "result.planned", "result.needs-consent",
        };

        public static IEnumerable<string> KnownMessageIds { get { return Known; } }

        public static ResultView For(EngineResult r, EngineRun run)
        {
            if (r == null)
                return new ResultView
                {
                    Kind = ViewKind.Problem, TitleId = "result.none.title", BodyId = "result.none.body", ChangesUnknown = true,
                    ChangesMade = run != null && run.MayHaveChanged, OfferRetry = true,
                };
            var v = new ResultView
            {
                FailedChecks = r.FailedChecks, ConsentsNeeded = r.ConsentsNeeded,
                NothingChanged = r.NothingChanged && !r.Mutated,
                ChangesMade = r.Mutated,
            };
            var id = r.MessageId != null && Known.Contains(r.MessageId) ? r.MessageId : "result.other";
            v.TitleId = id + ".title";
            v.BodyId = id + ".body";
            switch (r.Outcome)
            {
                case "planned":
                case "needs-consent":
                    v.Kind = ViewKind.Plan;
                    break;
                case "restart-required":
                    // exit 0: a boundary reached; 7: the restart has not happened yet; 5: test signing not active
                    // after the restart (Secure Boot on: a restart does not help).
                    v.Kind = r.MessageId == "result.testsigning-secureboot" ? ViewKind.Problem : ViewKind.Restart;
                    v.OfferRestart = r.MessageId != "result.testsigning-secureboot" && !r.DryRun;
                    break;
                case "verified":
                case "prepared":
                    v.Kind = ViewKind.Success;
                    break;
                case "completed":
                case "already":
                    v.Kind = ViewKind.Information;
                    v.OfferRepair = r.Outcome == "already";
                    break;
                case "cancelled":
                    v.Kind = ViewKind.Cancelled;
                    v.OfferRetry = true;
                    break;
                case "verify-failed":
                    v.Kind = ViewKind.Problem;
                    v.OfferRepair = true;
                    break;
                default:    // refused, failed and anything newer than this window
                    v.Kind = ViewKind.Problem;
                    v.OfferRetry = true;
                    break;
            }
            // A3: a pre-mutation stop may say that nothing was changed; after a change no text promises a way back.
            if (v.Kind == ViewKind.Success || v.Kind == ViewKind.Restart || v.Kind == ViewKind.Information) v.NothingChanged = false;
            return v;
        }
    }

    // The setup's guide panel: the same ranks as the control app's (plan v7 section 6, tools/win/amdgpu_wddm_control/
    // src/Guide.cs), restricted to the causes setup has. The lowest rank wins; the panel only repeats the page.
    public enum SetupCause { InstallStopped = 1, VerificationFailed = 2, PendingRestart = 3, WorkInProgress = 4, UpgradeDone = 5, Welcome = 6, PageTip = 7 }

    public static class SetupGuide
    {
        public static int Rank(SetupCause c)
        {
            switch (c)
            {
                case SetupCause.InstallStopped: case SetupCause.VerificationFailed: return 1;
                case SetupCause.PendingRestart: return 3;
                case SetupCause.WorkInProgress: return 4;
                case SetupCause.UpgradeDone: return 5;
                default: return 6;
            }
        }

        public static string Expression(SetupCause c)
        {
            switch (Rank(c)) { case 1: return "03-surprised"; case 3: return "07-hopeful"; case 4: return "05-thinking"; case 5: return "04-wink"; }
            return c == SetupCause.Welcome ? "01-wave" : "02-curious";
        }

        public static SetupCause Pick(IEnumerable<SetupCause> active)
        {
            var list = active.ToList();
            if (list.Count == 0) return SetupCause.PageTip;
            return list.OrderBy(Rank).ThenBy(c => (int)c).First();
        }

        public static string TextId(SetupCause c)
        {
            switch (c)
            {
                case SetupCause.InstallStopped: return "guide.install-stopped";
                case SetupCause.VerificationFailed: return "guide.verification-failed";
                case SetupCause.PendingRestart: return "guide.pending-restart";
                case SetupCause.WorkInProgress: return "guide.work-in-progress";
                case SetupCause.UpgradeDone: return "guide.upgrade-done";
                case SetupCause.Welcome: return "guide.welcome";
                default: return "guide.page-tip";
            }
        }
    }
}
