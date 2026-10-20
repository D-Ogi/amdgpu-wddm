// The help card of a check that the user fixes in Windows: today Smart App Control. The engine refuses the install
// while it is on (smartappcontrol.on, install.ps1) and notes evaluation mode and an unread state; the card says what to
// do in steps, shows up to three pictures of Windows Security, and has a button that opens that page of Windows
// Security. The window changes no Windows setting itself: the user does it, and Try again or Install reads it again.
// The pictures are English screenshots that build.ps1 embeds from -HelpArt (default art\help) as help.<file>. A
// picture the exe does not carry leaves its step as text, which says the same.
// Measured on 26100 (2026-10-20): while Smart App Control is on, Windows blocks this test-signed exe before its window
// opens, so today the "on" card shows only for a signed setup; install.cmd -Console reports the same check as text.
// Windows also blocks our user-mode driver DLLs in every process, DWM included, which is why evaluation is a warning.
using System.Collections.Generic;
using System.Linq;

namespace AmdgpuWddmSetup
{
    public sealed class HelpStep
    {
        public string TextId;
        // A file of art\help, or null for a step without a picture. An optional picture may be absent from the build.
        public string Image;
        public bool ImageOptional;
    }

    public sealed class HelpCard
    {
        public string CheckId, TitleId, BodyId, NoteId, ActionId, ActionUri;
        public HelpStep[] Steps;

        public IEnumerable<string> Images { get { return Steps.Where(s => s.Image != null).Select(s => s.Image); } }
    }

    public static class HelpCards
    {
        // Windows Security on its Smart App Control page (verified on 26100; windowsdefender://appbrowser is the page
        // App & browser control above it). windowsdefender: is the protocol of the Windows Security app; Settings
        // (ms-settings:) has no page for it.
        public const string SmartAppControlUri = "windowsdefender://smartapp";

        // At most three pictures in one card, so the card stays a page the user can read and not a gallery.
        public const int MaxImages = 3;
        // The embedded picture: at most ImagePixels wide and ImageBytes long (build.ps1 refuses a larger one). The
        // card draws it at most ShotWidth logical pixels wide, so the file is about 1.6 times that width and stays
        // sharp up to 150 % scaling.
        public const int ImagePixels = 720, ImageBytes = 160 * 1024, ShotWidth = 440;

        // The checks with a card, in the order a screen looks for one: the refusal first.
        public static readonly string[] CheckIds = { "smartappcontrol.on", "smartappcontrol.evaluation", "smartappcontrol.unknown" };

        static HelpCard SmartAppControl(string checkId, string bodyId, string lastStepId)
        {
            return new HelpCard
            {
                CheckId = checkId, TitleId = "help.sac.title", BodyId = bodyId, NoteId = "help.sac.note",
                ActionId = "help.sac.open", ActionUri = SmartAppControlUri,
                // The button opens the Smart App Control page itself, so the page App & browser control has no step
                // here (INSTALL.md shows it for the way through the Start menu).
                Steps = new[]
                {
                    new HelpStep { TextId = "help.sac.step.open", Image = "sac-3-settings.png" },
                    new HelpStep { TextId = "help.sac.step.off" },
                    new HelpStep { TextId = "help.sac.step.confirm", Image = "sac-4-confirm.png" },
                    new HelpStep { TextId = "help.sac.step.done", Image = "sac-5-off.png" },
                    new HelpStep { TextId = lastStepId },
                },
            };
        }

        // The card of one check id, or null. "on" refuses the install, so the last step is Try again; the two notes
        // come on the plan screen, where the last step is Install.
        public static HelpCard For(string checkId)
        {
            switch (checkId)
            {
                case "smartappcontrol.on": return SmartAppControl(checkId, "help.sac.on.body", "help.sac.step.retry");
                case "smartappcontrol.evaluation": return SmartAppControl(checkId, "help.sac.evaluation.body", "help.sac.step.install");
                case "smartappcontrol.unknown": return SmartAppControl(checkId, "help.sac.unknown.body", "help.sac.step.install");
                default: return null;
            }
        }

        // One card for a screen: the first of CheckIds that the screen's checks carry. Two cards of the same steps on
        // one page would only repeat them.
        public static HelpCard ForChecks(IEnumerable<string> ids)
        {
            var set = new HashSet<string>(ids ?? Enumerable.Empty<string>());
            return CheckIds.Where(set.Contains).Select(For).FirstOrDefault();
        }

        public static IEnumerable<HelpCard> All { get { return CheckIds.Select(For); } }

        // Every picture a card can show, and the ones a build must carry.
        public static IEnumerable<string> Images { get { return All.SelectMany(c => c.Images).Distinct().OrderBy(s => s, System.StringComparer.Ordinal); } }
        public static IEnumerable<string> RequiredImages
        {
            get { return All.SelectMany(c => c.Steps).Where(s => s.Image != null && !s.ImageOptional).Select(s => s.Image).Distinct().OrderBy(s => s, System.StringComparer.Ordinal); }
        }

        public static string Resource(string image) { return "help." + image; }

        // Every string id a card names, for the unit tests.
        public static IEnumerable<string> StringIds(HelpCard c)
        {
            foreach (var id in new[] { c.TitleId, c.BodyId, c.NoteId, c.ActionId }) if (id != null) yield return id;
            foreach (var s in c.Steps) yield return s.TextId;
        }
    }
}
