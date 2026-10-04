// The status card on Home (WU-007, WU-052, WU-061, A3) and the active causes of the guide's verdict (section 6).
// Every result, warning, refusal and next step is here in plain words; the guide panel only repeats or explains it.
// The technical states stay on the support view (Recovery.Describe) and in the support report.
using System;
using System.Collections.Generic;
using System.Linq;

namespace AmdgpuWddmControl
{
    public sealed class StatusInputs
    {
        public RecoverySnapshot Snapshot;           // null: nothing could be read
        public DriverCardView Driver;               // null: not read
        public GuideCause? Work;                    // Installing, Repairing or CreatingReport while user-started work runs
        public string UpgradeDone;                  // the release that qualified (DriverCard.UpgradeLatch); null: none
        public bool UpdateAvailable, CuConfirmFailed;
        public bool LaterRestart;                   // the user chose "Later" for a restart this session
    }

    public sealed class StatusItem
    {
        public GuideCause? Cause;                   // null: shown, but not a verdict cause
        public string Text, Next;
        public string Action;                       // null, restart, confirm-start, reopen-gpu-path, cu-confirm, repair, page:<name>
        public string ActionLabel;
        public string Severity;                     // problem, attention, pending, info, ok
    }

    public sealed class StatusCard
    {
        public string Title, Severity;
        public readonly List<StatusItem> Items = new List<StatusItem>();
        public readonly List<string> Pending = new List<string>();     // what waits for the restart, in plain words
        public bool StatusRead;                     // false: nothing could be read, so no verdict claims anything
        public bool Healthy;                        // read, and nothing of rank 1-3: the explicit "all good" cause (497.4)
        public IEnumerable<GuideCause> Causes { get { return Items.Where(i => i.Cause != null).Select(i => i.Cause.Value); } }
    }

    public static class HomeStatus
    {
        static StatusItem Item(StatusCard c, GuideCause? cause, string id, string severity, string action = null, params object[] args)
        {
            var i = new StatusItem
            {
                Cause = cause, Text = Strings.T("status." + id, args), Next = Strings.Has("status." + id + ".next") ? Strings.T("status." + id + ".next") : null,
                Action = action, ActionLabel = action == null ? null : Strings.T("status.action." + (action.StartsWith("page:", StringComparison.Ordinal) ? action.Substring(5) : action)),
                Severity = severity,
            };
            c.Items.Add(i);
            return i;
        }

        // The things that apply at the next restart of Windows, compared with what runs now (B5: a pending value is
        // never shown as the current one).
        public static List<string> PendingRestart(RecoverySnapshot s, DriverCardView d)
        {
            var list = new List<string>();
            if (d != null && d.InstalledPending) list.Add(Strings.T("status.pending.driver"));
            if (s == null || !s.DriverRunning) return list;
            if (s.Dpm != null)
            {
                bool storedAuto = s.P("DpmMode") == 1, runAuto = s.Dpm.Mode == 1;
                uint storedMax = s.P("DpmMaxMHz") ?? DpmSettings.DefaultMaxMHz;
                if (storedAuto != runAuto || storedAuto && storedMax != s.Dpm.MaxMHz) list.Add(Strings.T("status.pending.clock"));
            }
            var cu = CuMode.View(s.Cu, s.Health != null ? (ulong?)s.Health.Generation : null, s.StoredCu());
            if (s.Cu != null && s.Cu.Has(CuModeState.FlagValid) && cu.ChoiceMode != 0 && cu.ChoiceMode != s.Cu.Applied && cu.Class != CuClass.Fallback)
                list.Add(Strings.T("status.pending.cores"));
            if (Recovery.SwitchesRequested(s) && s.P("InteropClosedReason") == null && !Recovery.SwitchesOpenNow(s)) list.Add(Strings.T("status.pending.desktop"));
            return list;
        }

        public static StatusCard Compute(StatusInputs x)
        {
            var c = new StatusCard();
            var s = x.Snapshot;
            var d = x.Driver;
            c.StatusRead = s != null && !s.ReadFailed;
            if (!c.StatusRead) Item(c, null, "unreadable", "unknown");
            else if (!s.DriverInstalled) Item(c, GuideCause.DriverNotRunning, "not-installed", "problem");
            else if (!s.DriverRunning) Item(c, GuideCause.DriverNotRunning, "driver-not-running", "problem", "restart");
            if (d != null && d.InstallStopped) Item(c, GuideCause.InstallStopped, "install-stopped", "problem", "repair");
            if (d != null && d.Verification == VerifyKind.Failed) Item(c, GuideCause.VerificationFailed, "verification-failed", "problem", "repair");

            if (!c.StatusRead) s = null;      // the rest reads the snapshot only when it was read
            CuView cu = null;
            if (s != null && s.DriverInstalled)
            {
                cu = CuMode.View(s.Cu, s.Health != null ? (ulong?)s.Health.Generation : null, s.StoredCu(), x.CuConfirmFailed);
                if (cu.Class == CuClass.Unknown && cu.ChoiceMode == CuMode.Full && s.DriverRunning) Item(c, GuideCause.CuUnknownAfter40, "cu-unknown-after-40", "problem", "page:graphics");
            }
            if (s != null && s.DriverRunning && !Recovery.Confirmed(s.Health) && Recovery.ConfirmBlocker(s.Health, false) == null)
                Item(c, GuideCause.StartAtRisk, "start-at-risk", "attention", "confirm-start");
            if (cu != null)
            {
                if (cu.Class == CuClass.Fallback) Item(c, GuideCause.CuFallback, "cu-fallback", "attention", "page:graphics");
                if (x.CuConfirmFailed && cu.Class == CuClass.Waiting) Item(c, GuideCause.CuConfirmFailed, "cu-confirm-failed", "attention", "cu-confirm");
                else if (cu.Class == CuClass.Waiting && cu.OfferConfirm) Item(c, null, "cu-waiting", "attention", "cu-confirm");
                if (cu.Class == CuClass.NotSaved) Item(c, GuideCause.CuNotSaved, "cu-not-saved", "attention", "page:graphics");
            }
            if (s != null && s.DriverInstalled && s.P("InteropClosedReason") != null) Item(c, GuideCause.GpuDesktopClosed, "gpu-desktop-closed", "attention", "reopen-gpu-path");
            if (s != null && Recovery.DwmVerdict(s) == "observed") Item(c, GuideCause.DesktopReplaced, "desktop-replaced", "attention", "restart");

            c.Pending.AddRange(PendingRestart(s, d));
            if (c.Pending.Count > 0) Item(c, GuideCause.PendingRestart, x.LaterRestart ? "pending-later" : "pending-restart", "pending", "restart", string.Join(", ", c.Pending));
            if (x.Work != null) Item(c, x.Work, "work-in-progress", "info");
            if (x.UpgradeDone != null && d != null && DriverCard.UpgradeVerified(d) && string.Equals(d.InstalledVersion, x.UpgradeDone, StringComparison.Ordinal)) Item(c, GuideCause.UpgradeDone, "upgrade-done", "ok", null, d.InstalledVersion);
            if (x.UpdateAvailable) Item(c, GuideCause.UpdateAvailable, "update-available", "info", "page:driver");

            int rank = c.Causes.Any() ? Guide.Rank(c.Causes.Min()) : 8;
            c.Healthy = c.StatusRead && rank > 3;
            c.Severity = rank == 1 ? "problem" : rank == 2 ? "attention" : rank == 3 ? "pending" : c.StatusRead ? "ok" : "unknown";
            c.Title = Strings.T("status.title." + c.Severity);
            return c;
        }
    }
}
