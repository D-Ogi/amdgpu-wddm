// The overclock unlock card of the tuning section (OcUnlock.cs): what this start runs, a button that asks for the
// cooler and three warnings, and a button that takes the unlock away. The questions are the gate; the plan of the
// oc-tier action and its confirmation dialog come after them, as for every other change.
using System;
using System.Windows.Forms;

namespace AmdgpuWddmControl
{
    // The first question: the cooler on the board. Escape and "Not sure" stop the run.
    sealed class OcCoolingDialog : ThemedDialog
    {
        public uint Tier = OcUnlock.None;

        OcCoolingDialog() : base(Strings.T("oc.q.title"), 520)
        {
            Line(Strings.T("oc.q.cooling"), Theme.Dim);
            Line(Strings.T("oc.q.air.text", OcUnlock.GpuMaxMHz(OcUnlock.Air), OcUnlock.CpuMaxMHz(OcUnlock.Air)), Theme.Dim);
            Line(Strings.T("oc.q.liquid.text", OcUnlock.GpuMaxMHz(OcUnlock.Liquid), OcUnlock.CpuMaxMHz(OcUnlock.Liquid)), Theme.Dim);
            Line(Strings.T("oc.q.stock.text"), Theme.Dim);
            var stop = Add(Strings.T("oc.q.stock"), DialogResult.Cancel, true);
            var liquid = Add(Strings.T("oc.q.liquid"), DialogResult.OK);
            var air = Add(Strings.T("oc.q.air"), DialogResult.OK);
            air.Click += (s, e) => Tier = OcUnlock.Air;
            liquid.Click += (s, e) => Tier = OcUnlock.Liquid;
            CancelButton = stop;
            AcceptButton = stop;           // Enter never picks a cooler by accident
        }

        public static uint Ask(IWin32Window owner)
        {
            using (var d = new OcCoolingDialog()) return d.ShowDialog(owner) == DialogResult.OK ? d.Tier : OcUnlock.None;
        }

        internal static Form Sample() { return new OcCoolingDialog(); }
    }

    // One warning: "I understand" or "Stop". Stop is the default for Enter and Escape alike.
    sealed class OcWarningDialog : ThemedDialog
    {
        OcWarningDialog(int number, string text) : base(Strings.T("oc.warn.title", number, 3), 520)
        {
            Line(text, Theme.Dim);
            var stop = Add(Strings.T("oc.warn.stop"), DialogResult.Cancel, true);
            Add(Strings.T("oc.warn.accept"), DialogResult.OK);
            CancelButton = stop;
            AcceptButton = stop;
        }

        public static bool Ask(IWin32Window owner, int number, string text)
        {
            using (var d = new OcWarningDialog(number, text)) return d.ShowDialog(owner) == DialogResult.OK;
        }

        internal static Form Sample() { return new OcWarningDialog(1, "t"); }
    }

    public sealed partial class MainForm
    {
        CardPanel BuildOcCard(int width, bool installed)
        {
            var card = new CardPanel(Strings.T("oc.title"), width);
            Mark("graphics.tuning.oc", card);
            if (!OcUnlock.Available)
            {
                // A board the driver does not identify as a BC-250, or a driver older than the tiers: no control.
                card.Add(Ui.Dim(Strings.T(installed ? "board.unsupported" : "oc.not-installed"), card.Inner));
                return card;
            }
            uint? stored = _snap != null ? _snap.P(OcUnlock.TierName) : null;
            card.Pair(Strings.T("oc.now"), OcUnlock.StateText());
            if ((stored ?? OcUnlock.None) != OcUnlock.Effective)
                card.Pair(Strings.T("oc.next"), OcUnlock.TierText(stored ?? OcUnlock.None));
            card.Add(Ui.Dim(Strings.T("oc.intro", OcUnlock.GpuMaxMHz(OcUnlock.None), OcUnlock.CpuMaxMHz(OcUnlock.None)), card.Inner));
            card.Add(Ui.Dim(Strings.T("oc.lock.short"), card.Inner));
            var unlock = Ui.Button(Strings.T("oc.unlock"), (s, e) => RunOcUnlock());
            unlock.Enabled = !ReadOnlyProbe && installed;
            var off = Ui.Button(Strings.T("oc.off"), (s, e) => RunAction(OcUnlock.Action, new Recovery.PlanArgs { OcTier = OcUnlock.None }));
            off.Enabled = !ReadOnlyProbe && installed && (stored != null || OcUnlock.Effective != OcUnlock.None);
            card.Add(Ui.WrapRow(card.Inner, unlock, off));
            return card;
        }

        // The cooler, then the three warnings, then the ordinary plan and its confirmation. Any "Stop" ends the run
        // with nothing written.
        void RunOcUnlock()
        {
            var q = new OcUnlock.Questions();
            q.Cooling(OcCoolingDialog.Ask(this));
            int number = 1;
            while (q.WarningKey != null)
            {
                string text = q.WarningKey == "oc.warn.1" ? Strings.T("oc.warn.1", OcUnlock.GpuMaxMHz(q.Tier), OcUnlock.CpuMaxMHz(q.Tier)) : Strings.T(q.WarningKey);
                q.Answer(OcWarningDialog.Ask(this, number++, text));
            }
            if (q.Step != OcUnlock.Step.Done)
            {
                Result(Strings.T("oc.stopped"), true);
                return;
            }
            RunAction(OcUnlock.Action, new Recovery.PlanArgs { OcTier = q.Tier });
        }
    }
}
