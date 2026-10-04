// The guide panel (plan v7 section 6, WU-076, WU-077): one verdict picks the panel text and, when "Show Nagi" is
// checked, Nagi's expression. Deterministic: of all active causes the one with the lowest rank wins, and inside a rank
// the first cause in the plan's order; the GuideCause enum lists them in exactly that order, so the smallest value
// wins. No random choice. The panel only repeats or explains what the page or the status card already says.
using System;
using System.Collections.Generic;
using System.IO;
using System.Linq;

namespace AmdgpuWddmControl
{
    public enum GuideCause
    {
        // 1 problem
        DriverNotRunning, InstallStopped, VerificationFailed, CuUnknownAfter40,
        // 2 attention
        StartAtRisk, CuFallback, CuConfirmFailed, CuNotSaved, GpuDesktopClosed, DesktopReplaced,
        // 3 pending restart
        PendingRestart,
        // 4 user-started work in progress
        WorkInProgress,
        // 5 upgrade done (verified only)
        UpgradeDone,
        // 6 page context (only on the page that has it)
        Welcome, About, PageTip,
        // 7 update available (information)
        UpdateAvailable,
        // 8 all good
        AllGood,
    }

    public sealed class GuideVerdict
    {
        public GuideCause Cause;
        public int Rank;
        public string Expression;       // 01-wave .. 09-thumbs-up
        public string TextId;           // guide.<cause>
        public bool Animate;            // whether a frame sequence may play now
    }

    public static class Guide
    {
        public static int Rank(GuideCause c)
        {
            if (c <= GuideCause.CuUnknownAfter40) return 1;
            if (c <= GuideCause.DesktopReplaced) return 2;
            if (c == GuideCause.PendingRestart) return 3;
            if (c == GuideCause.WorkInProgress) return 4;
            if (c == GuideCause.UpgradeDone) return 5;
            if (c <= GuideCause.PageTip) return 6;
            if (c == GuideCause.UpdateAvailable) return 7;
            return 8;
        }

        public static string Expression(GuideCause c)
        {
            switch (Rank(c))
            {
                case 1: return "03-surprised";
                case 2: return "06-warning";
                case 3: return "07-hopeful";
                case 4: return "05-thinking";
                case 5: return "04-wink";
                case 6: return c == GuideCause.Welcome ? "01-wave" : c == GuideCause.About ? "08-giggle" : "02-curious";
                case 7: return "02-curious";
                default: return "09-thumbs-up";
            }
        }

        public static readonly string[] Expressions = { "01-wave", "02-curious", "03-surprised", "04-wink", "05-thinking", "06-warning", "07-hopeful", "08-giggle", "09-thumbs-up" };

        // The v3 art (scratch\gui\mascot\v3\final, owner-supplied, never committed) for each expression id. Two ids share
        // a file until the set has its own: 05-thinking uses the curious face, 02-curious the same.
        public static readonly Dictionary<string, string> ArtFile = new Dictionary<string, string>
        {
            { "01-wave", "sheet-welcome" }, { "02-curious", "sheet-curious" }, { "03-surprised", "qwen-surprised" }, { "04-wink", "sheet-wink" },
            { "05-thinking", "sheet-curious" }, { "06-warning", "sheet-surprised" }, { "07-hopeful", "qwen-hopeful" }, { "08-giggle", "sheet-relaxed" },
            { "09-thumbs-up", "sheet-smile" },
        };

        // The verdict over every active cause. An empty set is "all good".
        public static GuideVerdict Choose(IEnumerable<GuideCause> active)
        {
            var c = (active ?? Enumerable.Empty<GuideCause>()).DefaultIfEmpty(GuideCause.AllGood).Min();
            return new GuideVerdict { Cause = c, Rank = Rank(c), Expression = Expression(c), TextId = "guide." + Id(c) };
        }

        public static string Id(GuideCause c)
        {
            var name = c.ToString();
            var w = new System.Text.StringBuilder();
            for (int i = 0; i < name.Length; i++)
            {
                if (char.IsUpper(name[i]) && i > 0) w.Append('-');
                w.Append(char.ToLowerInvariant(name[i]));
            }
            return w.ToString().Replace("after40", "after-40");
        }

        // Whether a frame sequence may play: only with Nagi shown, reduced animation not in force (the app's setting or
        // Windows' "Animation effects" off), no rank 1-3 verdict, the window visible, and a complete frame set.
        public static bool MayAnimate(bool showNagi, bool reduceAnimations, bool? windowsAnimations, int rank, bool windowVisible, int frames)
        {
            return showNagi && !reduceAnimations && windowsAnimations != false && rank > 3 && windowVisible && frames > 1;
        }

        // Whether the art is drawn at all: only with "Show Nagi" checked and the expression's image available.
        public static bool ShowArt(bool showNagi, bool artAvailable) { return showNagi && artAvailable; }

        // An unasked tip ("Did you know", first-use hints) shows only with "Show tips automatically" checked; the verdict
        // text, asked-for help, results and warnings always show.
        public static bool ShowUnaskedTip(bool showTipsAutomatically) { return showTipsAutomatically; }

        // The art's resource name in the exe for an expression and a slot size: build.ps1 -NagiArt embeds
        // nagi.<expression>@<size>.png (and nagi.<expression>-fNN@<size>.png frames when the art has them).
        public static string Resource(string expression, int size) { return "nagi." + expression + "@" + size + ".png"; }
        public static string FrameResource(string expression, int frame, int size) { return "nagi." + expression + "-f" + frame.ToString("00") + "@" + size + ".png"; }

        // The slot's art size for a DPI scale: 128 px at 100 %, the @256 image from 150 % up (drawn scaled to the slot).
        public static int ArtSize(float scale) { return scale >= 1.5f ? 256 : 128; }

        // Frames per expression at most ~2 s at 12 frames a second; a longer or incomplete set falls back to the static image.
        public const int MaxFrames = 24, FrameMs = 83;
        public static int UsableFrames(IList<bool> present)
        {
            if (present == null || present.Count < 2 || present.Count > MaxFrames || present.Any(p => !p)) return 0;
            return present.Count;
        }
    }
}
