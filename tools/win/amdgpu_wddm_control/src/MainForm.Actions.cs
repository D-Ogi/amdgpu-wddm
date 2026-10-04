// Changes from the window: one planned action each (Recovery.Plan from a fresh reading), shown in plain words
// (PlainPlan), then the elevated helper, which plans again from its own reading and writes. The window shows the
// result in plain words; the helper's log goes to the support view and the report. A restart is offered after a
// change that needs one, with the hints of WU-060, and is never forced; "Later" leaves it pending on Home.
using System;
using System.Collections.Generic;
using System.Globalization;
using System.Linq;
using System.Threading.Tasks;
using System.Windows.Forms;

namespace AmdgpuWddmControl
{
    public sealed partial class MainForm
    {
        // Runs one action. done(ok) runs on the UI thread after the helper finished (not after a refusal or a cancel).
        void RunAction(string action, Recovery.PlanArgs more = null, uint? mode = null, uint? ceiling = null, bool setClocks = false, Action<bool> done = null)
        {
            RecoverySnapshot snapshot;
            try { snapshot = RecoveryProbe.Read("window"); }
            catch (Exception) { Result(Strings.T("action.cannot-read"), false); return; }
            var plan = Recovery.Plan(action, snapshot, mode, ceiling, RecoveryProbe.Backups(), false, more);
            if (plan.Refused) { Result(PlainPlan.Refusal(plan), false); return; }
            var dialog = PlainPlan.Describe(plan);
            var hints = dialog.Restart ? Hints.Decide(Hints.Read(_recent)) : null;
            if (!ConfirmDialog.Ask(this, dialog, hints)) return;

            string runId = Guid.NewGuid().ToString("N").Substring(0, 12);
            var verb = new List<string> { "--action", action, "--run-id", runId };
            if (mode != null || setClocks) { verb.Add("--mode"); verb.Add(mode == null ? "unset" : mode.Value.ToString(CultureInfo.InvariantCulture)); }
            if (ceiling != null || setClocks) { verb.Add("--ceiling"); verb.Add(ceiling == null ? "unset" : ceiling.Value.ToString(CultureInfo.InvariantCulture)); }
            if (more != null)
            {
                if (more.Cu != null) { verb.Add("--cu"); verb.Add(more.Cu.Value.ToString(CultureInfo.InvariantCulture)); }
                if (more.Games != null) { verb.Add("--games"); verb.Add(more.Games); }
                if (more.Image != null) { verb.Add("--image"); verb.Add(more.Image); }
                if (more.Value != null) { verb.Add("--value"); verb.Add(more.Value); }
            }
            _work = true;
            ComputeStatus();
            Result(Strings.T("action.working"), true);
            Enabled = false;
            Task.Run(() => Program.RunElevatedCode(verb.ToArray())).ContinueWith(t =>
            {
                Enabled = true;
                _work = false;
                int code = t.Status == TaskStatus.RanToCompletion ? t.Result : RecoveryRunner.Failed;
                bool ok = code == RecoveryRunner.Done;
                if (action == "cu-confirm") _cuConfirmFailed = !ok;
                string text = code == Program.NotElevated ? Strings.T("action.not-elevated") : ok ? Strings.T("action.done") : Strings.T("action.failed");
                RefreshAll();
                if (action == "cu-confirm" && ok)
                {
                    // CONFIRM may answer while the mark is not saved: the class after the re-read says what happened.
                    var view = CuMode.View(_snap.Cu, _snap.Health != null ? (ulong?)_snap.Health.Generation : null, _snap.StoredCu());
                    if (view.Class == CuClass.Waiting) { _cuConfirmFailed = true; text = Strings.T("cu.run.confirm-failed"); ok = false; ComputeStatus(); }
                }
                if (ok && plan.OfferRestart) text += " " + Strings.T("plan.when.restart");
                Result(text, ok);
                if (done != null) done(ok);
                if (ok && plan.OfferRestart) OfferRestart();
            }, TaskScheduler.FromCurrentSynchronizationContext());
        }

        void Result(string text, bool ok)
        {
            _lastResult = text; _lastResultOk = ok; _lastResultPage = _page;
            ShowPage(_page, null, false);
        }

        // The result line of the page that ran the last action (WU-061: result and next step).
        Label ResultLine(int width)
        {
            if (_lastResult == null || _lastResultPage != _page) return null;
            var l = Ui.Label(_lastResult, Theme.Bold, _lastResultOk ? Theme.Good : Theme.Warn, width);
            l.AccessibleRole = AccessibleRole.StaticText;
            return l;
        }

        void OfferRestart()
        {
            var hints = Hints.Decide(Hints.Read(_recent));
            if (RestartDialog.Ask(this, hints)) RestartWindows();
            else { _laterRestart = true; ComputeStatus(); ShowPage(_page, null, false); }
        }

        void RestartWindows()
        {
            var error = WindowsRestart.Request();
            if (error != null) Result(Strings.T("action.restart-failed"), false);
        }

        // ---- unsaved changes ---------------------------------------------------------------------------------------

        bool Dirty(string page)
        {
            if (page == "games") return GameWrites().Count > 0;
            if (page == "graphics") return ClockWrites().Count > 0 || CuChoiceChanged;
            return false;
        }

        bool SavePage(string page)
        {
            bool ok = false;
            if (page == "games") ok = ApplyGames();
            else if (page == "graphics") ok = ApplyGraphics();
            return ok && !Dirty(page);
        }

        void DiscardPage(string page)
        {
            if (page == "games") _gameEdits.Clear();
            if (page == "graphics") { _autoEdit = null; _ceilEdit = null; _ceilEdited = false; _cuEdit = null; }
        }

        // The modal helper run is synchronous for Save in the three-way dialog: it waits for the action to finish.
        bool RunActionAndWait(string action, Recovery.PlanArgs more = null, uint? mode = null, uint? ceiling = null, bool setClocks = false)
        {
            bool? result = null;
            RunAction(action, more, mode, ceiling, setClocks, ok => result = ok);
            while (result == null && !Enabled) { Application.DoEvents(); System.Threading.Thread.Sleep(20); }
            return result == true;
        }

        // ---- the update check (A5, D2) -----------------------------------------------------------------------------

        void StartUpdateCheck(bool manual)
        {
            if (_smoke) return;
            var now = DateTime.UtcNow;
            if (manual ? UpdateCheck.ManualCheckAllowed(_upd, now) : UpdateCheck.AutoCheckDue(_upd, _prefs.UpdateCheckAtStart, now))
            {
                UpdateCheck.Start(InstalledVersion);
                if (_page == "driver") ShowPage(_page, null, false);
            }
            else if (manual) ShowPage(_page, null, false);
        }

        void OnUpdateFinished(UpdateResult r)
        {
            try
            {
                BeginInvoke((Action)(() =>
                {
                    if (IsDisposed) return;
                    _upd = UpdateCheck.LoadCache();
                    ComputeStatus();
                    if (_page == "driver" || _page == "home") ShowPage(_page, null, false); else UpdateGuide();
                }));
            }
            catch (InvalidOperationException) { }
        }
    }
}
