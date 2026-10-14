// What the progress screen of a run shows (Checking for a plan run, Working for a real one), as text ids and marks:
// SetupForm turns them into words, glyphs and colours. Pure, so the unit tests feed it runs built from event lines.
//
// The window polls the engine's events every 250 ms. It used to build the whole screen again for every batch of new
// events: the progress strip, the stage list, the rail and the guide panel were disposed and made again, also for the
// events that change nothing on the screen (a 'step' for every change an install makes). The strip lost its window
// for a frame and its moving band jumped back each time (owner, 2026-10-14: "co 6-8 sekund niebieski pasek od
// instalatora robi krótkie mignięcie"). Now the window keeps the screen while its Frame stays the same and changes only
// what differs (SetupForm.UpdateWorking): a stage's mark, the note, the Cancel button, the value of the strip, and new
// check rows at the end of the list.
using System;
using System.Collections.Generic;
using System.Linq;
using System.Text;

namespace AmdgpuWddmSetup
{
    public enum StageMark { Waiting, Current, Done, Failed }

    public sealed class WorkingView
    {
        public bool Checking;
        public string TitleId, BodyId, NoteId;
        public bool PendingRemovals, CancelShown, CancelEnabled;
        public readonly List<string> StageIds = new List<string>();
        public readonly List<StageMark> Marks = new List<StageMark>();
        public readonly List<CheckRow> Checks = new List<CheckRow>();
        // The part of the run that is done, from the stages; null while no stage is known (the moving band).
        public float? Progress;

        // cancelRequested: the window already asked the engine to stop. pendingRemovals: an earlier run of this window
        // cancelled Windows' pending removal of driver files (the notice above the strip).
        public static WorkingView Of(EngineRun run, string flow, bool checking, bool cancelRequested, bool pendingRemovals)
        {
            var v = new WorkingView { Checking = checking, PendingRemovals = pendingRemovals };
            v.TitleId = checking ? "checking.title" : flow == "prepare" ? "working.prepare.title" : flow == "repair" ? "working.repair.title" : flow == "continue" ? "working.continue.title" : "working.title";
            v.BodyId = checking ? "checking.body" : flow == "prepare" ? "working.prepare.body" : "working.body";
            if (checking)
            {
                if (run != null) v.Checks.AddRange(run.Checks);
                return v;
            }
            var mode = run != null && run.Mode != null ? run.Mode : (flow == "prepare" ? "prepare-offline" : "run");
            var action = run != null && run.Decision != null ? run.Decision.Action : null;
            var expected = EngineRun.ExpectedStages(mode, action).ToList();
            // A stage the fixed list does not know (an optional one, such as the extras of the package) goes where the
            // run actually reached it: right after the stage before it. Appending it at the end would put the current
            // stage last and mark every real stage as finished (the index arithmetic below reads the order).
            int at = -1;
            foreach (var s in run == null ? new List<string>() : run.Stages)
            {
                int known = expected.IndexOf(s);
                if (known >= 0) { at = known; continue; }
                at = at + 1;
                if (at < 0) at = 0;
                if (at > expected.Count) at = expected.Count;
                expected.Insert(at, s);
            }
            int current = run == null || run.CurrentStage == null ? -1 : expected.IndexOf(run.CurrentStage);
            // Whether the run stopped in this stage. The engine writes its result event before its process ends, so
            // this screen is drawn once more with an outcome already known: restart-required and already are normal
            // endings of an install, and the stage the run reached in them is done, not crossed out in red.
            bool stopped = run != null && ResultView.StoppedIn(run.ResultOutcome);
            int finished = 0;
            for (int i = 0; i < expected.Count; i++)
            {
                bool done = i < current || (run != null && run.ResultOutcome != null && i <= current && !stopped);
                bool failed = stopped && i == current;
                bool on = i == current && !done && !failed;
                if (done) finished++;
                v.StageIds.Add(expected[i]);
                v.Marks.Add(done ? StageMark.Done : failed ? StageMark.Failed : on ? StageMark.Current : StageMark.Waiting);
            }
            if (expected.Count > 0 && current >= 0) v.Progress = Math.Min(1f, (finished + 0.5f) / expected.Count);
            v.NoteId = cancelRequested ? "working.cancel.requested" : run != null && run.CancelAvailable ? "working.cancel.available" : "working.cancel.unavailable";
            v.CancelShown = flow != "prepare";
            v.CancelEnabled = run != null && run.CancelAvailable && !cancelRequested;
            return v;
        }

        // The controls the screen is made of. Two views with the same frame use the same controls; only the marks,
        // the note, the Cancel button's state, the strip's value and the checks after the ones already listed differ.
        public string Frame
        {
            get
            {
                return (Checking ? "checking" : "working") + "|" + TitleId + "|" + BodyId + "|" + (PendingRemovals ? 1 : 0) + "|" +
                    (CancelShown ? 1 : 0) + "|" + string.Join(",", StageIds);
            }
        }

        // Everything the screen shows. Equal keys draw the same screen.
        public string Key
        {
            get
            {
                var k = new StringBuilder(Frame);
                k.Append('|').Append(string.Join(",", Marks.Select(m => m.ToString())));
                k.Append('|').Append(NoteId).Append('|').Append(CancelEnabled ? 1 : 0);
                k.Append('|').Append(Progress.HasValue ? Progress.Value.ToString("R", System.Globalization.CultureInfo.InvariantCulture) : "busy");
                foreach (var c in Checks) k.Append('|').Append(c.Id).Append(':').Append(c.Result);
                return k.ToString();
            }
        }

        // Whether this view's checks are the earlier view's checks with more after them (or the same): the window then
        // adds the new rows to the list it shows and builds nothing.
        public bool ExtendsChecks(WorkingView earlier)
        {
            if (earlier == null || Checks.Count < earlier.Checks.Count) return false;
            for (int i = 0; i < earlier.Checks.Count; i++)
                if (!ReferenceEquals(Checks[i], earlier.Checks[i]) && (Checks[i].Id != earlier.Checks[i].Id || Checks[i].Result != earlier.Checks[i].Result)) return false;
            return true;
        }
    }
}
