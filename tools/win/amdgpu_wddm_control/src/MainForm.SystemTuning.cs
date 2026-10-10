using System;
using System.Drawing;
using System.Linq;
using System.Threading.Tasks;
using System.Windows.Forms;

namespace AmdgpuWddmControl
{
    public sealed partial class MainForm
    {
        SystemTuningSnapshot _systemTuning;
        bool _systemReading, _systemChanging, _systemReadFailed;
        int _pauseDays = 7;

        bool SystemFixture { get { return _smoke || ReadOnlyProbe || _fixture; } }

        // Navigation starts one bounded background read. Rebuilding the page never reads the machine.
        void ReadSystemTuning()
        {
            if (SystemFixture) { _systemTuning = SystemTuning.Fixture(); return; }
            if (_systemReading || _systemChanging || !IsHandleCreated) return;
            _systemReading = true;
            if (_page == "system") ShowPage(_page, null, false);
            Task.Run(() => SystemTuningProcess.Read()).ContinueWith(task =>
            {
                if (IsDisposed || Disposing) return;
                _systemReading = false;
                _systemReadFailed = task.Status != TaskStatus.RanToCompletion || !task.Result.Ok;
                _systemTuning = _systemReadFailed ? null : task.Result;
                if (_page == "system") ShowPage(_page, null, false);
            }, TaskScheduler.FromCurrentSynchronizationContext());
        }

        Control BuildSystemTuning(int width)
        {
            if (SystemFixture && _systemTuning == null) _systemTuning = SystemTuning.Fixture();
            var page = Frame("system", width);
            var status = new CardPanel(Strings.T("system.overview"), width);
            status.Add(Ui.Dim(Strings.T("system.backup"), status.Inner));
            if (_systemReadFailed) status.Add(Ui.Label(Strings.T("system.read-failed"), Theme.Bold, Theme.Warn, status.Inner));
            else if (_systemReading || _systemTuning == null) status.Add(Ui.Dim(Strings.T("system.reading"), status.Inner));
            var refresh = Ui.Button(Strings.T("system.refresh"), (s, e) => ReadSystemTuning());
            refresh.Enabled = !_systemReading && !_systemChanging;
            var recommended = SystemButton("system.recommended", "ApplyRecommended", null,
                _systemTuning != null && _systemTuning.Items.Any(x => x.Recommended && x.CanApply));
            var restore = SystemButton("system.restore-all", "RestoreAll", null,
                _systemTuning != null && _systemTuning.Items.Any(x => x.CanRestore));
            status.Add(Ui.WrapRow(status.Inner, refresh, recommended, restore));
            var result = ResultLine(status.Inner); if (result != null) status.Add(result);
            page.Controls.Add(status); Mark("system.background", status);

            foreach (var entry in SystemTuning.Catalog.Where(x => x.Kind != "driverPolicy" && x.Kind != "pausePolicy"))
            {
                var card = SystemItemCard(entry, width);
                page.Controls.Add(card);
                if (entry.Key == "drive") Mark("system.drive", card);
            }

            var drivers = SystemItemCard(SystemTuning.Entry("policy.DriverUpdates"), width);
            page.Controls.Add(drivers); Mark("system.drivers", drivers);
            var updates = new CardPanel(Strings.T("system.item.updates"), width);
            updates.Add(Ui.Dim(Strings.T("system.item.updates.description"), updates.Inner));
            var pause = _systemTuning?.Find("policy.UpdatePause");
            updates.Pair(Strings.T("system.current"), SystemTuning.StateText(pause));
            if (pause != null && pause.RequestedUntil != null) updates.Pair(Strings.T("system.requested-until"), pause.RequestedUntil);
            updates.Pair(Strings.T("system.observed"), Strings.T("system.observed." + (pause?.ObservedState ?? "unknown")));
            updates.Add(Ui.Dim(Strings.T("system.pause-policy"), updates.Inner));
            var daysLabel = Ui.Label(Strings.T("system.days"), Theme.Bold, null, updates.Inner);
            var days = new NumericUpDown { Minimum = 1, Maximum = 35, Value = _pauseDays, Width = Theme.S(90),
                Font = Theme.Body, ForeColor = Theme.Text, BackColor = Theme.CardHi, AccessibleName = Strings.T("system.days"),
                Margin = Theme.Pad(0, 4, 0, 6) };
            days.ValueChanged += (s, e) => _pauseDays = (int)days.Value;
            // NumericUpDown owns a child TextBox. Give that edit the same accessible label as its parent.
            foreach (Control child in days.Controls) child.AccessibleName = Strings.T("system.days");
            days.Enabled = !_systemChanging;
            updates.Add(daysLabel); updates.Add(days);
            updates.Add(Ui.WrapRow(updates.Inner,
                SystemButton("system.pause", "PauseUpdates", null, pause?.CanApply ?? false),
                SystemButton("system.resume", "ResumeUpdates", null, pause?.CanRestore ?? false)));
            page.Controls.Add(updates); Mark("system.updates", updates);
            return page;
        }

        CardPanel SystemItemCard(SystemTuningEntry entry, int width)
        {
            var card = new CardPanel(entry.Name, width);
            card.Add(Ui.Dim(Strings.T(entry.Scope == "User" ? "system.scope.user" : "system.scope.machine"), card.Inner));
            card.Add(Ui.Dim(Strings.T("system.item." + entry.Key + ".description"), card.Inner));
            var item = _systemTuning?.Find(entry.Id);
            card.Pair(Strings.T("system.current"), SystemTuning.StateText(item));
            card.Add(Ui.Dim(Strings.T(item == null ? "system.state.unknown" : item.Managed ? "system.managed" : "system.unmanaged"), card.Inner));
            card.Add(Ui.WrapRow(card.Inner,
                SystemButton("system.apply", "Apply", entry.Id, item?.CanApply ?? false),
                SystemButton("system.restore", "Restore", entry.Id, item?.CanRestore ?? false)));
            return card;
        }

        Button SystemButton(string key, string action, string item, bool allowed)
        {
            var button = Ui.Button(Strings.T(key), (s, e) => ChangeSystemTuning(action, item));
            button.Enabled = allowed && !_systemReading && !_systemChanging && !SystemFixture;
            return button;
        }

        void ChangeSystemTuning(string action, string item)
        {
            if (SystemFixture || _systemChanging || _systemReading || _systemTuning == null || !_systemTuning.Ok) return;
            int days = action == "PauseUpdates" ? _pauseDays : 0;
            string scope = item == null ? "Machine" : SystemTuning.Entry(item).Scope;
            string[] args = SystemTuning.Arguments(action, item, days, scope);
            var plan = new PlainDialog { Title = Strings.T("system.plan.title") };
            if (item != null) plan.Changes.Add(Strings.T(action == "Apply" ? "system.plan.apply" : "system.plan.restore", SystemTuning.Entry(item).Name));
            else if (action == "PauseUpdates") plan.Changes.Add(Strings.T("system.plan.pause", days));
            else if (action == "ResumeUpdates") plan.Changes.Add(Strings.T("system.plan.resume"));
            else
            {
                plan.Changes.Add(Strings.T(action == "RestoreAll" ? "system.plan.restore-all" : "system.plan.recommended"));
                foreach (var entry in SystemTuning.Catalog)
                {
                    var current = _systemTuning.Find(entry.Id);
                    if (current != null && (action == "RestoreAll" ? current.CanRestore : current.Recommended && current.CanApply))
                        plan.Changes.Add(entry.Name + " (" + Strings.T(entry.Scope == "User" ? "system.scope.user" : "system.scope.machine") + ")");
                }
            }
            plan.Notes.Add(Strings.T("system.plan.recheck"));
            plan.Notes.Add(Strings.T("system.backup"));
            if (item == "policy.DriverUpdates") plan.Notes.Add(Strings.T("system.item.drivers.description"));
            if (action == "PauseUpdates" || action == "ResumeUpdates") plan.Notes.Add(Strings.T("system.pause-policy"));
            if (!ConfirmDialog.Ask(this, plan, null)) return;
            bool restoreUser = action == "RestoreAll" && _systemTuning.Items.Any(x => x.CanRestore && SystemTuning.Entry(x.Id).Scope == "User");
            bool restoreMachine = action == "RestoreAll" && _systemTuning.Items.Any(x => x.CanRestore && SystemTuning.Entry(x.Id).Scope == "Machine");
            _systemChanging = true;
            Result(Strings.T("system.working"), true);
            Task.Run(() =>
            {
                if (scope == "User") return SystemTuningProcess.ChangeUser(action, item);
                if (action != "RestoreAll") return Program.RunElevatedCode(args);
                // The original account is handled before a UAC dialog can start a helper as another account.
                int userCode = restoreUser ? SystemTuningProcess.ChangeUser("RestoreAll", null) : 0;
                int machineCode = restoreMachine ? Program.RunElevatedCode(args) : 0;
                if (userCode == 0 && machineCode == 0) return 0;
                // The other scope might already have changed, including when UAC was declined.
                return userCode == SystemTuningProcess.Timeout || machineCode == SystemTuningProcess.Timeout ? SystemTuningProcess.Timeout : SystemTuningProcess.Failed;
            }).ContinueWith(task =>
            {
                if (IsDisposed || Disposing) return;
                _systemChanging = false;
                int code = task.Status == TaskStatus.RanToCompletion ? task.Result : SystemTuningProcess.Failed;
                _lastResult = Strings.T(code == 0 ? "system.done" : code == Program.NotElevated ? "system.cancelled" :
                    code == SystemTuningProcess.Timeout ? "system.timeout" : "system.failed");
                _lastResultPage = "system"; _lastResultOk = code == 0;
                _systemTuning = null;
                ReadSystemTuning();
            }, TaskScheduler.FromCurrentSynchronizationContext());
        }
    }
}
