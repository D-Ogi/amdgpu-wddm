// The review oracles (G-CU, G-VER, G-ART): oracle-cu.json, oracle-ver.json and oracle-art.json, written from the plan
// alone by the reviewer and read where they lie (AMDGPU_WDDM_ORACLE, set by build.ps1 -Oracle); never copied into the
// repository. The adapters below translate the oracles' field names and fixture vocabulary into this app's inputs and
// outputs and nothing else: every expected value comes from the oracle. A mismatch fails the run unless Disputed
// lists it (case id and field) with this app's reading of the plan.
using System;
using System.Collections.Generic;
using System.Globalization;
using System.IO;
using System.Linq;
using System.Web.Script.Serialization;
using AmdgpuWddmControl;

static partial class UnitTests
{
    // "<case id> <field>" -> why this app differs, with the plan text it follows.
    static readonly Dictionary<string, string> Disputed = new Dictionary<string, string>
    {
        { "VER-13 dates.release_date", "the Released label is the INSTALLED release's GitHub date (plan F-VER, WU-043); interfaces.md section 5 and " +
            "review 927 R8 show it only while Release\\Version is readable and equals the release the check compared against, so a missing " +
            "Release\\Version shows no Released date" },
    };

    sealed class OracleRun
    {
        public string Name;
        public int Cases, Matching;
        public readonly List<string> Mismatches = new List<string>(), DisputedHits = new List<string>();
        List<string> _case;
        string _id;

        public void Begin(string id) { _id = id; _case = new List<string>(); Cases++; }

        public void Same(string field, object expected, object actual)
        {
            string e = Norm(expected), a = Norm(actual);
            if (e != a) _case.Add(field + ": expected " + e + ", got " + a);
        }

        public void True(string field, bool ok, string detail) { if (!ok) _case.Add(field + ": " + detail); }

        public void End()
        {
            if (_case.Count == 0) { Matching++; return; }
            foreach (var m in _case)
            {
                string field = m.Substring(0, m.IndexOf(':'));
                string why;
                if (Disputed.TryGetValue(_id + " " + field, out why)) DisputedHits.Add(_id + " " + m + " [disputed: " + why + "]");
                else Mismatches.Add(_id + " " + m);
            }
        }

        static string Norm(object o)
        {
            if (o == null) return "null";
            if (o is bool) return (bool)o ? "true" : "false";
            if (o is int || o is long || o is uint || o is ulong || o is decimal) return Convert.ToDecimal(o).ToString(CultureInfo.InvariantCulture);
            return "\"" + o + "\"";
        }
    }

    static object J(object o, params string[] path)
    {
        foreach (var p in path)
        {
            var d = o as IDictionary<string, object>;
            if (d == null || !d.TryGetValue(p, out o)) return null;
        }
        return o;
    }

    static string JS(object o, params string[] path) { return J(o, path) as string; }
    static bool? JB(object o, params string[] path) { var v = J(o, path); return v is bool ? (bool?)(bool)v : null; }
    static long? JL(object o, params string[] path) { var v = J(o, path); return v is int || v is long || v is decimal ? (long?)Convert.ToInt64(v) : null; }
    static object[] JA(object o, params string[] path) { return J(o, path) as object[] ?? new object[0]; }

    static void OracleTests()
    {
        var dir = Environment.GetEnvironmentVariable("AMDGPU_WDDM_ORACLE");
        if (string.IsNullOrEmpty(dir)) { Console.WriteLine("oracles: not given (build.ps1 -Oracle <dir>), skipped"); return; }
        var json = new JavaScriptSerializer { MaxJsonLength = int.MaxValue };
        Func<string, object> load = name => json.DeserializeObject(File.ReadAllText(Path.Combine(dir, name)));
        Strings.Language = "en";
        foreach (var run in new[] { OracleCu(load("oracle-cu.json")), OracleVer(load("oracle-ver.json")), OracleArt(load("oracle-art.json")) })
        {
            Console.WriteLine("oracle " + run.Name + ": " + run.Matching + "/" + run.Cases + " cases match" +
                (run.DisputedHits.Count > 0 ? ", " + run.DisputedHits.Count + " disputed field(s)" : "") + (run.Mismatches.Count > 0 ? ", " + run.Mismatches.Count + " MISMATCH(ES)" : ""));
            foreach (var d in run.DisputedHits) Console.WriteLine("  disputed " + d);
            foreach (var m in run.Mismatches) Console.WriteLine("  MISMATCH " + m);
            Check(run.Mismatches.Count == 0, "oracle " + run.Name + ": no undisputed mismatch");
            Check(run.Cases > 0, "oracle " + run.Name + ": cases read");
        }
    }

    // ---- G-CU ---------------------------------------------------------------------------------------------------

    // A registry that records every set and delete with its result, with the oracle's injected failure.
    sealed class OracleReg : ICuRegistry
    {
        public readonly Dictionary<string, uint> Values = new Dictionary<string, uint>();
        public readonly List<string> Writes = new List<string>();
        public string FailDelete;
        public bool FailSet;
        public int FailFlush = -1, ReadsFailAfterFlush = -1;
        int _flushes;
        bool _readsFail;

        public uint? Read(string name)
        {
            if (_readsFail) throw new InvalidOperationException("injected read failure");
            uint v;
            return Values.TryGetValue(name, out v) ? (uint?)v : null;
        }

        public void Delete(string name)
        {
            bool fail = name == FailDelete;
            if (Values.ContainsKey(name) || fail) Writes.Add("delete_value " + name + " - " + (fail ? "injected_failure" : "ok"));
            if (fail) throw new InvalidOperationException("injected delete failure");
            Values.Remove(name);
        }

        public void SetDword(string name, uint value)
        {
            Writes.Add("set_value " + name + " " + value + " " + (FailSet ? "injected_failure" : "ok"));
            if (FailSet) throw new InvalidOperationException("injected set failure");
            Values[name] = value;
        }

        public void Flush()
        {
            int n = _flushes++;
            if (n == FailFlush) throw new InvalidOperationException("injected flush failure");
            if (n == ReadsFailAfterFlush) _readsFail = true;
        }
    }

    static readonly string[] CuNames = { "CuMode", "CuModePending", "CuModeConfirmed", "CuDisableWgp" };

    static CuStored OracleStored(object reg, string status)
    {
        if (status != "ok" || reg == null) return new CuStored { Unreadable = true };
        Func<string, uint?> g = k => { var v = JL(reg, k); return v == null ? (uint?)null : (uint)v.Value; };
        return new CuStored { Mode = g("CuMode"), Disable = g("CuDisableWgp"), Confirmed = g("CuModeConfirmed"), Pending = g("CuModePending") };
    }

    static CuModeState OracleSnapshot(object s)
    {
        if (s == null || JS(s, "read_status") != "ok") return null;
        Func<string, uint> u = k => (uint)(JL(s, k) ?? 0);
        return new CuModeState { Version = u("AbiVersion"), Generation = (ulong)(JL(s, "Generation") ?? 0), Flags = u("Flags"), Applied = u("Applied"), ActiveCus = u("ActiveCus"),
            Reason = u("Reason"), DisableMask = u("DisableMask") };
    }

    static string Snake(string camel)
    {
        var w = new System.Text.StringBuilder();
        for (int i = 0; i < camel.Length; i++) { if (char.IsUpper(camel[i]) && i > 0) w.Append('_'); w.Append(char.ToLowerInvariant(camel[i])); }
        return w.ToString();
    }

    static OracleRun OracleCu(object root)
    {
        var run = new OracleRun { Name = "CU" };
        foreach (var c in JA(root, "cases"))
        {
            string id = JS(c, "id");
            var input = J(c, "input");
            var expected = J(c, "expected");
            run.Begin(id);
            string action = JS(input, "action"), selection = JS(input, "selection"), failure = JS(input, "failure", "step");

            // The snapshot and stored values the window reads. A confirm or a restart is the KMD's work: its post-state
            // (the expected snapshot and the read-back registry) is the window's input.
            object snapshot = action == "confirm" || action == "restart" ? J(expected, "snapshot_after") : J(input, "snapshot_before");
            long? startGeneration = action == "confirm" ? JL(input, "confirm_expected_generation") : JL(snapshot, "expected_generation");
            var stored = OracleStored(J(input, "readback_registry"), JS(input, "readback_status"));
            CuSetterPlan plan = null;
            CuSetterResult res = null;
            OracleReg reg = null;
            if (action == "select")
            {
                reg = new OracleReg();
                foreach (var n in CuNames) { var v = JL(input, "registry_before", n); if (v != null) reg.Values[n] = (uint)v.Value; }
                // The failing step and, for a flush or a verifying read, how many flushes the oracle's steps make before it.
                int flushesBefore = 0;
                foreach (var s in JA(c, "ordered_steps"))
                {
                    if (JS(s, "step") == failure) break;
                    if (JS(s, "operation") == "flush_key") flushesBefore++;
                }
                switch (failure)
                {
                    case null: break;
                    case "S1.remove_mode": reg.FailDelete = "CuMode"; break;
                    case "S2.remove_confirmed": reg.FailDelete = "CuModeConfirmed"; break;
                    case "S3.remove_mask": reg.FailDelete = "CuDisableWgp"; break;
                    case "S1.write_mode": reg.FailSet = true; break;
                    case "S1.flush":
                    case "S4.flush": reg.FailFlush = flushesBefore; break;
                    case "S1.verify_stock": reg.ReadsFailAfterFlush = flushesBefore - 1; break;
                    default: run.True("failure", false, "no adapter for the injected step " + failure); break;
                }
                plan = CuMode.Plan(CuMode.ReadAll(reg), selection == "all40" ? CuMode.Full : CuMode.Stock);
                res = CuMode.Execute(plan, reg);
                stored = CuMode.StoredAfter(res);
            }
            var view = CuMode.View(OracleSnapshot(snapshot), startGeneration == null ? null : (ulong?)startGeneration.Value, stored,
                action == "confirm" && failure != null);

            run.Same("running_state", JS(expected, "running_state"), view.Class.ToString());
            run.Same("running_cores", JL(expected, "running_cores"), view.Class == CuClass.Unknown ? null : (object)OracleSnapshot(snapshot).ActiveCus);
            var choice = action == "select" ? CuMode.ChoiceAfter(res) : view.Stored;
            run.Same("choice", JS(expected, "choice"), choice.ToString());
            var choiceText = JS(expected, "choice_text");
            if (choiceText != null) run.True("choice_text", view.Choice.Contains(choiceText), "\"" + choiceText + "\" not in \"" + view.Choice + "\"");

            var p = CuMode.Prediction(stored);
            var ns = J(expected, "next_start");
            run.Same("next_start.knowledge", JS(ns, "knowledge"), p == null ? "unknown" : "policy_prediction");
            run.Same("next_start.mode", JL(ns, "mode"), p == null ? null : (object)p.Mode);
            run.Same("next_start.active_cus", JL(ns, "active_cus"), p == null ? null : (object)p.ActiveCus);
            run.Same("next_start.state", JS(ns, "state"), p == null ? null : p.State.ToString());
            run.Same("next_start.reason", JL(ns, "reason"), p == null ? null : (object)p.Reason);
            run.Same("next_start.pending", JB(ns, "pending"), p == null ? null : (object)p.Pending);
            run.Same("next_start.confirmed", JB(ns, "confirmed"), p == null ? null : (object)p.Confirmed);

            var tx = action == "select" ? CuMode.Transaction(plan, res) : action == "confirm" ? CuMode.ConfirmOutcome(view.Class) : CuTransaction.NotApplicable;
            run.Same("transaction_result", JS(expected, "transaction_result"), Snake(tx.ToString()));

            if (action == "select")
            {
                var after = J(expected, "registry_after") ?? J(input, "model_registry_after_attempts");
                foreach (var n in CuNames)
                {
                    uint v;
                    run.Same("registry_after." + n, JL(after, n), reg.Values.TryGetValue(n, out v) ? (object)v : null);
                }
            }
            var writes = JA(expected, "ordered_helper_writes").Select(w => JS(w, "operation") + " " + JS(w, "name") + " " + (JL(w, "value") != null ? JL(w, "value").ToString() : "-") + " " + JS(w, "result"));
            run.Same("ordered_helper_writes", string.Join("; ", writes), reg == null ? "" : string.Join("; ", reg.Writes));

            string text = view.Choice + " " + view.Running + " " + view.NextStart + (action == "select" ? " " + CuMode.ResultText(plan, res) : "");
            foreach (string t in JA(expected, "required_text")) run.True("required_text", text.Contains(t), "\"" + t + "\" not in \"" + text + "\"");
            foreach (string t in JA(expected, "forbidden_text")) run.True("forbidden_text", !text.Contains(t), "\"" + t + "\" in \"" + text + "\"");
            foreach (string t in JA(expected, "preview_required_text"))
            {
                var preview = plan == null ? "" : string.Join(" ", plan.Preview);
                run.True("preview_required_text", preview.Contains(t), "\"" + t + "\" not in \"" + preview + "\"");
            }
            Func<string, bool?> button = b => b == "confirm_now" ? view.OfferConfirm : b == "create_support_report" ? view.OfferReport : b == "choose_standard24" ? view.OfferChoose24
                : b == "try40_again" ? (bool?)view.OfferTry40 : null;
            foreach (string b in JA(expected, "buttons", "required")) run.True("buttons", button(b) == true, b + " required" + (button(b) == null ? " (no adapter)" : ""));
            foreach (string b in JA(expected, "buttons", "forbidden")) run.True("buttons", button(b) == false, b + " forbidden" + (button(b) == null ? " (no adapter)" : ""));
            run.End();
        }
        return run;
    }

    // ---- G-VER --------------------------------------------------------------------------------------------------

    static readonly DateTime OracleBase = new DateTime(2026, 10, 4, 0, 0, 0, DateTimeKind.Utc);

    static long OracleBoot(string label)
    {
        long n;
        return label != null && long.TryParse(label.Substring(label.LastIndexOf('-') + 1), NumberStyles.None, CultureInfo.InvariantCulture, out n) ? n : -1;
    }

    static OracleRun OracleVer(object root)
    {
        var run = new OracleRun { Name = "VER" };
        // The package identities of the fixture (version -> manifest SHA256), over all cases: the app reads the installed
        // package's manifest from its install folder, whatever the running-release mapping of a case holds.
        var manifests = new Dictionary<string, string>();
        foreach (var c in JA(root, "cases"))
            foreach (var p in JA(c, "input", "package_candidates"))
                if (JS(p, "version") != null) manifests[JS(p, "version")] = JS(p, "manifest_sha256");
        foreach (var c in JA(root, "cases"))
        {
            var i = J(c, "input");
            var e = J(c, "expected");
            run.Begin(JS(c, "id"));
            var f = new DriverFacts
            {
                BootId = OracleBoot(JS(i, "current_boot_id")),
                ReplyAbi = JB(i, "reply", "available") == true ? DriverCard.ParseAbi(JS(i, "reply", "kmd_abi")) : null,
                InstalledVersion = JS(i, "installed", "release_version"),
                StatePackageVersion = JS(i, "installed", "state_package_version"),
                Phase = JS(i, "installed", "phase"),
            };
            switch (JS(i, "manifest_mapping_status"))
            {
                case "complete": f.Mapping = MappingStatus.Complete; break;
                case "incomplete": f.Mapping = MappingStatus.Incomplete; break;
                case "conflicting": f.Mapping = MappingStatus.Conflicting; break;
                default: f.Mapping = MappingStatus.Absent; break;
            }
            foreach (var p in JA(i, "package_candidates"))
                f.Packages.Add(new KnownPackage { Name = JS(p, "release"), Version = JS(p, "version"), ManifestSha256 = JS(p, "manifest_sha256"), ImageSha256 = JS(p, "driver_image_sha256"),
                    KmdBuild = JS(p, "kmd_build"), KmdAbi = JS(p, "kmd_abi"), KmdVersion = JS(p, "kmd_version"), Source = "oracle" });
            string installedManifest;
            f.InstalledManifestSha256 = f.InstalledVersion != null && manifests.TryGetValue(f.InstalledVersion, out installedManifest) ? installedManifest : null;
            var w = J(i, "witness");
            if (w != null)
            {
                string writer = JS(w, "writer");
                f.Witness = new ReleaseWitness
                {
                    RecordedBy = writer == "engine_verify" ? "verify" : writer == "start_confirm" ? "start-confirm" : writer,
                    BootId = OracleBoot(JS(w, "boot_id")), RecordedUtc = Recovery.Stamp(OracleBase.AddSeconds(JL(w, "sequence") ?? 0)),
                    Release = JS(w, "release"), Version = JS(w, "version"), ManifestSha256 = JS(w, "manifest_sha256"), KmdImageSha256 = JS(w, "driver_image_sha256"),
                    KmdBuild = JS(w, "kmd_build"), KmdAbi = JS(w, "kmd_abi"),
                };
            }
            foreach (var a in JA(i, "install_actions"))
                f.InstallActions.Add(new InstallAction { BootId = OracleBoot(JS(a, "boot_id")), Utc = Recovery.Stamp(OracleBase.AddSeconds(JL(a, "sequence") ?? 0)) });
            var ev = J(i, "started_device_evidence");
            if (ev != null)
                f.Image = new LoadedImage { Available = JB(ev, "available") == true, OfStartedDevice = JS(ev, "evidence_kind") == "image_of_started_device_not_service_path",
                    BootId = JS(ev, "boot_id") != null ? (long?)OracleBoot(JS(ev, "boot_id")) : null, Sha256 = JS(ev, "driver_image_sha256"), KmdBuild = JS(ev, "kmd_build") };
            var vr = J(i, "verification");
            if (vr != null)
                f.Reports.Add(new VerifyReport { Valid = JB(vr, "readable") == true, Passed = JS(vr, "outcome") == "pass", Utc = Recovery.Stamp(OracleBase), PackageVersion = JS(vr, "package_version"),
                    ManifestSha256 = JS(vr, "manifest_sha256"), PassedCount = 1, FailedCount = JS(vr, "outcome") == "pass" ? 0 : 1 });
            var infDate = JS(i, "dates", "inf_driver_date");
            DateTime inf;
            if (infDate != null && DateTime.TryParseExact(infDate, "yyyy-MM-dd", CultureInfo.InvariantCulture, DateTimeStyles.None, out inf))
                f.DriverDate = inf.ToString("M-d-yyyy", CultureInfo.InvariantCulture);

            var v = DriverCard.Decide(f);

            string running = v.Running == RunningKind.Exact ? "exact" : v.Running == RunningKind.Ambiguous ? "ambiguous" : "unknown";
            run.Same("running.status", JS(e, "running", "status"), running);
            run.Same("running.release", JS(e, "running", "release"), v.RunningRelease);
            run.Same("running.display.kind", JS(e, "running", "display", "kind"), v.Running == RunningKind.Exact ? "release" : "cannot_determine_exactly");
            run.Same("running.display.value", JS(e, "running", "display", "value"), v.Running == RunningKind.Exact ? v.RunningVersion : null);
            if (JB(e, "running", "candidate_names_on_ui") == false && v.Running != RunningKind.Exact)
                foreach (var p in f.Packages)
                    run.True("running.candidate_names_on_ui", !v.RunningText.Contains(p.Version) && !v.RunningText.Contains(p.Name), p.Version + " in \"" + v.RunningText + "\"");

            run.Same("installed.status", JS(e, "installed", "status"), v.InstalledVersion != null ? "known" : "unknown");
            run.Same("installed.release", JS(e, "installed", "release"), v.InstalledVersion);
            run.Same("installed.display.kind", JS(e, "installed", "display", "kind"), v.InstalledVersion != null ? "release" : "unknown");
            run.Same("installed.display.value", JS(e, "installed", "display", "value"), v.InstalledVersion);
            if (v.InstalledVersion != null) run.True("installed.text", v.InstalledText.Contains(v.InstalledVersion), "\"" + v.InstalledText + "\"");

            run.Same("pending.restart_required", JB(e, "pending", "restart_required"), v.InstalledPending);
            run.Same("pending.release", JS(e, "pending", "release"), v.InstalledPending ? v.InstalledVersion : null);
            run.Same("pending.display.kind", JS(e, "pending", "display", "kind"), v.InstalledPending ? "restart_pending" : "none");

            run.Same("verification.status", JS(e, "verification", "status"), v.Verification == VerifyKind.Verified ? "verified" : v.Verification == VerifyKind.Failed ? "failed" : "unknown");
            run.Same("verification.display", JS(e, "verification", "display"),
                v.Verification == VerifyKind.Verified ? "verified" : v.Verification == VerifyKind.Failed ? "not_verified" : "could_not_be_checked");

            run.Same("update_comparison.basis", JS(e, "update_comparison", "basis"), "installed");
            run.Same("update_comparison.release", JS(e, "update_comparison", "release"), v.InstalledVersion);

            // Dates: the INF date on its own label; the publication date of the installed release from a successful check.
            var driverDate = JS(e, "dates", "driver_date");
            run.True("dates.driver_date", driverDate == null ? v.DriverDateText == Strings.T("drv.date.none") : v.DriverDateText.Contains(driverDate), "\"" + v.DriverDateText + "\"");
            UpdateCache cache = null;
            if (JB(i, "dates", "release_check_succeeded") == true)
            {
                var installed = JS(i, "installed", "release_version");
                cache = new UpdateCache { LastAttemptUtc = Recovery.Stamp(OracleBase), LastSuccessUtc = Recovery.Stamp(OracleBase), LastAttemptOutcome = "UpToDate",
                    CheckedInstalled = installed, InstalledTag = installed == null ? null : "v" + installed, InstalledPublishedUtc = JS(i, "dates", "github_published_date") };
            }
            var releaseDate = JS(e, "dates", "release_date");
            run.Same("dates.release_date", releaseDate == null ? null : releaseDate.Substring(0, 10), UpdateCheck.InstalledReleased(cache, v.InstalledVersion));
            run.Same("dates.labels_distinct", JB(e, "dates", "labels_distinct"), Strings.T("drv.date", "x") != Strings.T("drv.released", "x"));
            run.End();
        }
        return run;
    }

    // ---- G-ART --------------------------------------------------------------------------------------------------

    static readonly Dictionary<string, GuideCause> ArtIds = new Dictionary<string, GuideCause>
    {
        { "driver_not_running", GuideCause.DriverNotRunning }, { "install_failed_or_stopped", GuideCause.InstallStopped }, { "verification_failed", GuideCause.VerificationFailed },
        { "cu_unknown_after_40_request", GuideCause.CuUnknownAfter40 }, { "start_unconfirmed_at_risk", GuideCause.StartAtRisk }, { "cu_fallback", GuideCause.CuFallback },
        { "cu_confirmation_failed", GuideCause.CuConfirmFailed }, { "cu_confirmation_not_saved", GuideCause.CuNotSaved }, { "gpu_desktop_path_closed", GuideCause.GpuDesktopClosed },
        { "dwm_replacement_observed", GuideCause.DesktopReplaced }, { "pending_restart", GuideCause.PendingRestart }, { "installing", GuideCause.Installing },
        { "repairing", GuideCause.Repairing }, { "creating_support_report", GuideCause.CreatingReport }, { "upgrade_verified", GuideCause.UpgradeDone },
        { "setup_welcome_or_app_first_start", GuideCause.Welcome }, { "about_page", GuideCause.About }, { "page_tip", GuideCause.PageTip },
        { "update_available", GuideCause.UpdateAvailable }, { "all_good", GuideCause.AllGood },
    };

    static GuideContext OracleContext(object x)
    {
        var g = new GuideContext
        {
            ShowNagi = JB(x, "show_nagi") == true, ReduceAnimations = JB(x, "reduce_animations") == true, ShowTipsAutomatically = JB(x, "show_tips_automatically") == true,
            WindowsAnimations = JB(x, "windows_animations_enabled"), WindowVisible = JS(x, "window_state") == "visible", ArtAvailable = JB(x, "art_available") == true,
        };
        switch (JS(x, "frames_status"))
        {
            case "ready": g.FramesFound = 12; g.FramesComplete = true; break;
            case "failed": g.FramesFound = 12; g.FramesComplete = false; break;          // a frame did not load
            case "oversized": g.FramesFound = Guide.MaxFrames + 1; g.FramesComplete = true; break;
            default: g.FramesFound = 0; g.FramesComplete = false; break;                 // missing
        }
        switch (JS(x, "trigger"))
        {
            case "panel_opened": g.Trigger = GuideTrigger.PanelOpened; break;
            case "none": g.Trigger = GuideTrigger.None; break;
            default: g.Trigger = GuideTrigger.VerdictChanged; break;
        }
        return g;
    }

    static OracleRun OracleArt(object root)
    {
        var run = new OracleRun { Name = "ART" };
        var contexts = J(root, "contexts");
        foreach (var c in JA(root, "cases"))
        {
            var i = J(c, "input");
            var e = J(c, "expected");
            run.Begin(JS(c, "id"));
            var x = OracleContext(J(i, "context") ?? J(contexts, JS(i, "context_id")));
            var causes = new List<GuideCause>();
            bool known = true;
            foreach (string id in JA(i, "active_verdicts").Concat(JA(i, "other_eligible_verdicts")))
            {
                GuideCause cause;
                if (ArtIds.TryGetValue(id, out cause)) causes.Add(cause); else known = false;
            }
            run.True("active_verdicts", known, "a verdict id without an adapter");
            if (JS(c, "kind") == "rank5_eligibility")
            {
                // The raw rank 5 guards as the Driver card sees them: a witness of this boot naming the new release, and the
                // installed package's verification record.
                var w = J(i, "current_boot_running_witness");
                var rec = J(i, "verification_record");
                string phase = JS(i, "raw_upgrade_phase");
                var view = new DriverCardView { InstalledVersion = "new", InstalledPending = phase == "installed" || phase == "driver-pending-restart" };
                view.Running = w != null && JB(w, "valid") == true ? RunningKind.Exact : RunningKind.Unknown;
                view.RunningVersion = view.Running == RunningKind.Exact ? (JB(w, "package_matches_new_version") == true ? "new" : "old") : null;
                view.Verification = rec == null || JB(rec, "installed_package_matches") != true ? VerifyKind.Unknown : JS(rec, "result") == "pass" ? VerifyKind.Verified : VerifyKind.Failed;
                bool eligible = DriverCard.UpgradeVerified(view);
                if (eligible) causes.Add(GuideCause.UpgradeDone);
                run.Same("upgrade_verified_eligible", JB(e, "upgrade_verified_eligible"), eligible);
                run.Same("installed_verification", JS(e, "installed_verification"),
                    view.Verification == VerifyKind.Verified ? "verified" : view.Verification == VerifyKind.Failed ? "not_verified" : "could_not_be_checked");
            }
            var d = Guide.Evaluate(causes, x);
            string winner = d.Verdict == null ? null : ArtIds.First(p => p.Value == d.Verdict.Cause).Key;
            run.Same("winner", JS(e, "winner"), winner);
            run.Same("rank", JL(e, "rank"), d.Verdict == null ? null : (object)d.Verdict.Rank);
            run.Same("intrarank_order", JL(e, "intrarank_order"), d.Verdict == null ? null : (object)d.Verdict.Order);
            run.Same("figure", JS(e, "figure"), d.Figure);
            run.Same("animation_allowed", JB(e, "animation_allowed"), d.Animate);
            run.Same("animation_required", JB(e, "animation_required"), false);
            run.Same("maximum_animation_seconds", JL(e, "maximum_animation_seconds"), d.MaxAnimationSeconds);
            run.True("maximum_animation_seconds.frames", !d.Animate || x.FramesFound * Guide.FrameMs <= d.MaxAnimationSeconds * 1000, "the frame set is longer than the bound");
            run.Same("loop_allowed", JB(e, "loop_allowed"), false);
            run.Same("automatic_tips_allowed", JB(e, "automatic_tips_allowed"), d.AutomaticTips);
            run.Same("settings_writes", JA(e, "settings_writes").Length, 0);
            if (J(e, "paint_now") != null) run.Same("paint_now", JB(e, "paint_now"), d.PaintNow);
            if (J(e, "frame_timer_ticks") != null) run.Same("frame_timer_ticks", JL(e, "frame_timer_ticks"), d.Animate && d.PaintNow ? x.FramesFound : 0);
            // Essential and verdict text: the panel text is the verdict's text, shown with or without the figure.
            if (d.Verdict != null) run.True("verdict_text_retained", Strings.Has(d.Verdict.TextId), d.Verdict.TextId + " has no text");
            run.End();
        }
        return run;
    }
}
