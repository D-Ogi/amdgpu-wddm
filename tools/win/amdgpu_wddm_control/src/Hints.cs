// Hints before a disruptive action (WU-060, R5, B11): a recorded game running now (matched by its full path) and
// Windows' own "busy" state (SHQueryUserNotificationState: a full-screen or Direct3D application, presentation mode).
// They are hints, not proof: missing history or a process the app cannot inspect never authorizes anything. Every
// disruptive action needs the user's explicit confirmation, whatever the hints say; "Later" leaves it pending on
// Home. The app never ends a game or any other process.
using System;
using System.Collections.Generic;
using System.Diagnostics;
using System.Linq;
using System.Runtime.InteropServices;

namespace AmdgpuWddmControl
{
    public enum BusyState { Unknown, Free, Fullscreen, Direct3D, Presentation, Other }

    public sealed class ActivityHints
    {
        public readonly List<string> RunningGames = new List<string>();    // image names of recorded games running now
        public BusyState Busy = BusyState.Unknown;
    }

    // What the confirmation dialog shows; never an automatic action.
    public sealed class HintDecision
    {
        public readonly List<string> Lines = new List<string>();
        public bool NeedsConfirmation = true;        // always true: no hint, a missing hint or a clear state skips it
        public bool OfferLater = true;
    }

    public static class Hints
    {
        public static HintDecision Decide(ActivityHints h)
        {
            var d = new HintDecision();
            if (h == null) return d;
            foreach (var g in h.RunningGames.Distinct(StringComparer.OrdinalIgnoreCase)) d.Lines.Add(Strings.T("hint.game-running", g));
            if (h.Busy == BusyState.Fullscreen || h.Busy == BusyState.Direct3D) d.Lines.Add(Strings.T("hint.fullscreen"));
            if (h.Busy == BusyState.Presentation) d.Lines.Add(Strings.T("hint.presentation"));
            if (d.Lines.Count > 0) d.Lines.Add(Strings.T("hint.save-first"));
            return d;
        }

        // The recorded games running now, by full path. A process whose path cannot be read is not a hint either way.
        public static ActivityHints Read(IEnumerable<RecentLaunch> recorded)
        {
            var h = new ActivityHints { Busy = QueryBusy() };
            foreach (var game in recorded ?? Enumerable.Empty<RecentLaunch>())
            {
                Process[] procs;
                try { procs = Process.GetProcessesByName(System.IO.Path.GetFileNameWithoutExtension(game.Image)); }
                catch (Exception) { continue; }
                foreach (var p in procs)
                    using (p)
                    {
                        string path = null;
                        try { path = p.MainModule.FileName; } catch (Exception) { }
                        if (path != null && RecentLaunches.SamePath(path, game.Path) && !h.RunningGames.Contains(game.Image)) h.RunningGames.Add(game.Image);
                    }
            }
            return h;
        }

        public static BusyState QueryBusy()
        {
            try
            {
                int state;
                if (SHQueryUserNotificationState(out state) != 0) return BusyState.Unknown;
                switch (state)
                {
                    case 5: return BusyState.Free;                 // QUNS_ACCEPTS_NOTIFICATIONS
                    case 2: return BusyState.Fullscreen;           // QUNS_BUSY
                    case 3: return BusyState.Direct3D;             // QUNS_RUNNING_D3D_FULL_SCREEN
                    case 4: return BusyState.Presentation;         // QUNS_PRESENTATION_MODE
                    default: return BusyState.Other;
                }
            }
            catch (Exception) { return BusyState.Unknown; }
        }

        [DllImport("shell32.dll")]
        static extern int SHQueryUserNotificationState(out int state);
    }
}
