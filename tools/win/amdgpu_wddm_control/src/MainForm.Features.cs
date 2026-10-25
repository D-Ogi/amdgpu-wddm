// The driver's own switches on the Graphics page (DriverFeatures.cs): one check box each, shown only where the
// running driver says that the switch has an effect. A click runs the planned action at once, as the picture on the
// start screen does (MainForm.Display.cs). The driver reads the setting when it starts, so each row says that a new
// choice works after the next restart of Windows, and the action offers that restart.
using System.Linq;

namespace AmdgpuWddmControl
{
    public sealed partial class MainForm
    {
        // Null when the driver can use neither switch on this computer: then the page shows no card at all.
        CardPanel BuildDriverFeatures(int width)
        {
            var shown = DriverFeatures.All.Where(f => DriverFeatures.Shown(_snap, f)).ToList();
            if (shown.Count == 0) return null;
            var card = new CardPanel(Strings.T("graphics.features.title"), width);
            foreach (var f in shown)
            {
                var feature = f;
                bool on = DriverFeatures.On(_snap.P(f.ValueName));
                var box = Ui.Check(Strings.T("graphics." + f.Id), card.Inner - Theme.S(40));
                box.Checked = on;
                box.CheckedChanged += (s, e) =>
                {
                    if (box.Checked == on) return;
                    RunAction(box.Checked ? feature.ActionOn : feature.ActionOff);
                };
                Mark("graphics." + f.Id, box);
                card.Add(Ui.Row(box, Explain(f.Id, box.Text)));
                card.Add(Ui.Dim(Strings.T("graphics." + f.Id + ".help"), card.Inner));
                if (DriverFeatures.Pending(_snap, f)) card.Add(Ui.Label(Strings.T("graphics.feature.pending"), null, Theme.Warn, card.Inner));
            }
            return card;
        }
    }
}
