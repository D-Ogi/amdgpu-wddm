using System;
using System.Globalization;
using System.Threading.Tasks;
using System.Windows.Forms;

namespace AmdgpuWddmControl
{
    public sealed partial class MainForm
    {
        UmaState _uma;
        Control BuildUmaCard(int width)
        {
            var c = new CardPanel(Strings.T("uma.title"), width);
            c.Pair(Strings.T("uma.active"), UmaSetting.Active(_uma));
            c.Pair(Strings.T("uma.requested"), _uma != null && _uma.ReadValid ? UmaSetting.Size(_uma.RequestedMiB) : Strings.T("uma.unavailable"));
            c.Add(Ui.Dim(Strings.T("uma.tradeoff"), c.Inner));
            c.Add(Ui.Dim(Strings.T("uma.restart"), c.Inner));
            c.Add(Ui.Dim(Strings.T("uma.recovery"), c.Inner));
            if (_uma == null || !_uma.WriteAllowed) c.Add(Ui.Dim(Strings.T("uma.windows-unavailable"), c.Inner));
            if (UmaSetting.Pending(_uma)) c.Add(Ui.Dim(Strings.T("uma.pending"), c.Inner));
            var row = Ui.WrapRow(c.Inner);
            foreach (uint choice in UmaSetting.Choices)
            {
                uint selected = choice;
                var b = Ui.Button(UmaSetting.Size(choice), (s, e) => RunUma(selected, false));
                b.Enabled = !ReadOnlyProbe && UmaSetting.CanSet(_uma, choice);
                row.Controls.Add(b);
            }
            var restore = Ui.Button(Strings.T("uma.restore"), (s, e) => RunUma(0, true));
            restore.Enabled = !ReadOnlyProbe && UmaSetting.CanRestore(_uma);
            row.Controls.Add(restore); c.Add(row);
            return c;
        }
        void RunUma(uint target, bool restore)
        {
            if (restore ? !UmaSetting.CanRestore(_uma) : !UmaSetting.CanSet(_uma, target)) return;
            string confirmed = UmaSetting.ConfirmationToken(_uma);
            string prompt = Strings.T(restore ? "uma.confirm-restore" : "uma.confirm", UmaSetting.Size(target)) +
                "\n\n" + Strings.T("uma.tradeoff") + "\n\n" + Strings.T("uma.restart");
            if (MessageBox.Show(this, prompt, Strings.T("uma.title"), MessageBoxButtons.OKCancel,
                MessageBoxIcon.Warning, MessageBoxDefaultButton.Button2) != DialogResult.OK) return;
            Enabled = false;
            Task.Run(() => Program.RunElevatedCode("--uma-action", restore ? "restore" : target.ToString(CultureInfo.InvariantCulture), confirmed)).ContinueWith(t =>
            {
                Enabled = true;
                int code = t.Status == TaskStatus.RanToCompletion ? t.Result : 1;
                RefreshAll();
                Result(Strings.T(code == Program.NotElevated ? "action.not-elevated" : code == 0 ? "uma.restart" : "action.failed"), code == 0);
            }, TaskScheduler.FromCurrentSynchronizationContext());
        }
    }
}
