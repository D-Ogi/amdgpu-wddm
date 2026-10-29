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
        public bool CompatibilityOk, SystemTuning;
        // The two shortcut answers the engine would act on: its own command line, or the answer an earlier run of
        // the same install saved. Both are on where the engine says nothing about them.
        public bool StartMenuShortcut = true, DesktopShortcut = true;
        // The wallpaper answer the engine would act on, on where the engine says nothing (an older engine).
        public bool Wallpaper = true;
        // The start-screen picture the engine would act on: the answer of its own command line, or, where nobody
        // stated one, the answer the installed driver already carries. On where the engine says nothing.
        public bool BootLogo = true;
    }

    public sealed class SettingsPlan
    {
        public int Kept, Updated, Added, Unchanged, Command;
        public readonly List<SettingRow> Rows = new List<SettingRow>();
    }

    public sealed class EngineResult
    {
        public string Invocation, Mode, Action, Outcome, MessageId, Step, Detail, Log, PhaseBefore, PhaseAfter, PackageVersion;
        // The engine's automatic diagnostics file of a driver start that did not pass (null for every other run).
        public string Diagnostics;
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
                Diagnostics = Json.Str(o, "diagnostics"),
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
        public int PendingRemovalsCancelled;
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
                        SystemTuning = Json.Bool(e, "system_tuning"),
                        StartMenuShortcut = Json.BoolOr(e, "start_menu_shortcut", true), DesktopShortcut = Json.BoolOr(e, "desktop_shortcut", true),
                        Wallpaper = Json.BoolOr(e, "wallpaper", true), BootLogo = Json.BoolOr(e, "boot_logo", true),
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
                case "pending-removals-cancelled":
                    PendingRemovalsCancelled = Math.Max(PendingRemovalsCancelled, Json.Int(e, "count"));
                    break;
                case "result":
                    ResultOutcome = Json.Str(e, "outcome"); ResultMessageId = Json.Str(e, "message_id"); CancelAvailable = false;
                    break;
            }
        }

        // Whether the run may have changed this PC, as far as the events tell (a missing result cannot say more).
        public bool MayHaveChanged { get { return InstallActionSeen || RealSteps > 0; } }

        // The stages a run of this mode goes through, in order, for the progress list. The optional stages are not
        // here: 'extras' and 'components' (the llama.cpp and Ollama extras of install.cmd) happen only when a run can
        // choose an extra, which this window's own runs cannot - it has no control for them and consents to nothing
        // its command line did not name. A run that does reach them (a command line with -Components) reports them as
        // stage events, and SetupForm puts such a stage into the list where the run reached it.
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
        public bool RetryHelps = true;      // false: a new try in this session meets the same refusal (Try again is not the default)
        public bool AlreadyInstalled;       // the engine found this version installed and checked, but the run did not end as "already"
        public string PackageVersion;
        public string[] FailedChecks = new string[0];
        public string[] ConsentsNeeded = new string[0];

        // Failed checks that a new try cannot change while setup is open: the hardware, the Windows edition, the setup
        // files, a development installation, and Secure Boot (it changes only in the firmware settings, after a restart).
        // The engine does not mark a cause as transient, so the window decides by check id. A refusal with one failed check
        // outside this list (free space, the firmware download, ...) keeps Try again as the default. A missing
        // Microsoft Visual C++ runtime is no longer a refusal at all: the install brings it.
        // A set of files that does not carry Microsoft's own installer for it is a damaged download, so a new try
        // in this session meets the same refusal.
        // The engine outcomes that end a run without stopping it. The stage screen is drawn once more after the
        // engine wrote its result and before its process ends: the stage the run reached is done for these, and only
        // the others (refused, failed, cancelled, verify-failed, and anything newer than this window) are crossed out.
        public static readonly HashSet<string> OutcomesThatFinish = new HashSet<string>(StringComparer.Ordinal)
        {
            "verified", "completed", "prepared", "restart-required", "already", "planned", "needs-consent",
        };

        public static bool StoppedIn(string outcome) { return outcome != null && !OutcomesThatFinish.Contains(outcome); }

        public static readonly HashSet<string> RetryCannotFix = new HashSet<string>(StringComparer.Ordinal)
        {
            "gpu.missing", "gpu.several", "windows.not-x64", "windows.too-old", "package.damaged", "firmware.no-list",
            "lab-install.present", "secureboot.on", "vcruntime.no-installer",
            "driver-rank.not-above", "driver-rank.unreadable",
        };

        static readonly HashSet<string> Known = new HashSet<string>(StringComparer.Ordinal)
        {
            "result.dry-run-complete", "result.dry-run-verify", "result.already", "result.restart-test-signing", "result.restart-driver",
            "result.installed-restart", "result.testsigning-not-active", "result.testsigning-secureboot", "result.restart-still-pending",
            "result.verify-before-restart", "result.verified", "result.verified-with-warnings", "result.preflight-refused",
            "result.preflight-error", "result.needs-admin", "result.package-damaged", "result.firmware-folder-bad",
            "result.firmware-unreachable", "result.prepare-destination", "result.busy", "result.verify-failed", "result.step-failed",
            "result.cancelled", "result.cancelled-after-changes", "result.deadline", "result.deadline-after-changes", "result.offline-prepared",
            "result.planned", "result.needs-consent", "result.install-unfinished",
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
            v.RetryHelps = r.MessageId != "result.package-damaged" && !(r.FailedChecks.Length > 0 && r.FailedChecks.All(RetryCannotFix.Contains));
            v.AlreadyInstalled = r.Action == "already" && r.Outcome != "already";
            v.PackageVersion = r.PackageVersion;
            return v;
        }
    }

    public enum RailState { Pending, Done, Current, Failed }

    // The step rail. A run that stops shows the step it stopped in as current (failed for a problem, plain for a
    // cancel), the steps before it done and the steps after it not done: a refusal at Check never shows Review and
    // Install as done.
    public static class Rail
    {
        // count: the steps of the flow (5, or 3 for preparing a folder); screenStep: the step of the screen shown;
        // view: the finished run's view when the result screen shows it, else null; runKind: plan, install or prepare.
        public static RailState[] States(int count, int screenStep, ResultView view, string runKind)
        {
            int current = screenStep;
            bool failed = false;
            if (view != null && (view.Kind == ViewKind.Problem || view.Kind == ViewKind.Cancelled))
            {
                current = count == 3 || runKind == "plan" ? 1 : 3;     // Prepare; Check; Install
                failed = view.Kind == ViewKind.Problem;
            }
            var states = new RailState[count];
            for (int i = 0; i < count; i++) states[i] = i < current ? RailState.Done : i > current ? RailState.Pending : failed ? RailState.Failed : RailState.Current;
            return states;
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

        // The art of an expression in the exe, embedded by build.ps1 from the control application's art directory
        // under the same names it uses (Guide.Resource): one map of expression to file, one set of resources. The
        // window embeds the @256 pictures only: its smallest slot is 176 device pixels at 96 DPI, larger than the
        // @128 picture, so a @128 resource could never be drawn and the exe would carry it for nothing.
        public static string Resource(string expression, int size) { return "nagi." + expression + "@" + size + ".png"; }

        // The art is drawn at most 1.25 times the @256 picture, so it never blurs: 320 device pixels. The welcome and
        // prepare screens give her the whole width of the panel, every other screen a smaller slot beside its tip.
        public const int MaxArtPixels = 320, HeroArtDesign = 244, ArtDesign = 176;

        public static int ArtBox(float scale, bool hero)
        {
            return Math.Min(MaxArtPixels, (int)Math.Round((hero ? HeroArtDesign : ArtDesign) * (scale <= 0 ? 1 : scale)));
        }

        // The picture to load for that slot. The smallest slot is larger than the @128 picture at every scale
        // (ArtBox(scale, false) >= 176), so the window asks for the @256 art and no other size is embedded.
        public const int ArtPixels = 256;
        public static int ArtSize(float scale) { return ArtPixels; }

        // Every expression a screen of this window can ask for: Expression() of every cause there is. The build
        // embeds these and no more, so the exe carries no picture it could never draw (the control application
        // embeds all nine, because its verdicts reach all nine). The unit tests print this list for the build.
        public static readonly string[] ArtExpressions = Enum.GetValues(typeof(SetupCause)).Cast<SetupCause>()
            .Select(Expression).Distinct().OrderBy(e => e, StringComparer.Ordinal).ToArray();

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

        // While an installation runs (the Working screen), its tip takes turns with one more: what the name Nagi
        // stands for. The turn changes every TipSeconds; an even turn is the working tip, an odd
        // one the name. Every other screen and cause keeps its one tip, whatever the turn.
        public const int TipSeconds = 15;
        public const string NameTipId = "guide.name";

        public static string TipId(SetupCause c, int turn)
        {
            return c == SetupCause.WorkInProgress && (turn & 1) == 1 ? NameTipId : TextId(c);
        }
    }
}
