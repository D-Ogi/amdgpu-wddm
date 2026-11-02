using System;
using System.Collections.Generic;
using System.Drawing;
using System.Linq;
using System.Threading.Tasks;
using System.Windows.Forms;

namespace AmdgpuWddmControl
{
    public sealed partial class MainForm
    {
        RocmPayload _rocmPayload;
        List<RocmVenvEntry> _rocmVenvs;
        bool _rocmChecking, _rocmActing;

        void InitRocm()
        {
            _rocmPayload = RocmAttach.FindPayload();
            _rocmVenvs = RocmAttach.DiscoverVenvs();
        }

        void RefreshRocmStates()
        {
            if (_rocmPayload == null || _rocmChecking || _rocmActing) return;
            if (_rocmVenvs == null || _rocmVenvs.Count == 0) return;
            _rocmChecking = true;
            if (_page == "graphics") ShowPage(_page, null, false);
            var payload = _rocmPayload;
            var paths = _rocmVenvs.Select(v => v.Path).ToList();
            Task.Run(() => Crash.Watched("ROCm check", () =>
            {
                var results = new Dictionary<string, string>();
                foreach (var path in paths)
                {
                    var r = RocmAttach.Check(payload.Directory, path);
                    results[path] = r.ExitCode >= 0 ? RocmAttach.ParseState(r.Output) : "error";
                }
                return results;
            })).ContinueWith(task =>
            {
                _rocmChecking = false;
                if (task.Status == TaskStatus.RanToCompletion)
                    foreach (var v in _rocmVenvs)
                    {
                        string s;
                        if (task.Result.TryGetValue(v.Path, out s)) v.State = s;
                    }
                if (_page == "graphics") ShowPage(_page, null, false);
            }, TaskScheduler.FromCurrentSynchronizationContext());
        }

        Control BuildRocmCard(int width)
        {
            if (_rocmPayload == null) return null;

            var card = new CardPanel(Strings.T("rocm.title"), width);
            Mark("rocm.payload", card);
            card.Add(Ui.Dim(Strings.T("rocm.intro"), card.Inner));
            card.Pair(Strings.T("rocm.therock"), _rocmPayload.TheRock);

            if (_rocmChecking)
            {
                card.Add(Ui.Dim(Strings.T("rocm.checking"), card.Inner));
            }
            else if (_rocmVenvs != null && _rocmVenvs.Count > 0)
            {
                foreach (var v in _rocmVenvs)
                {
                    var venv = v;
                    var name = Ui.Label(ShortenPath(v.Path), Theme.Bold, null, card.Inner);
                    var stateText = Strings.T("rocm.state." + (v.State ?? "unknown"));
                    var stateColor = v.State == "attached" ? Theme.Good : v.State == "detached" ? Theme.Dim : Theme.Warn;
                    var state = Ui.Label(stateText, null, stateColor, card.Inner);

                    var buttons = Ui.WrapRow(card.Inner);
                    if (v.State != "attached" && v.State != "recovery" && v.State != "missing")
                    {
                        var attach = Ui.Button(Strings.T("rocm.attach"), (s, e) => RunRocmAttach(venv.Path), true);
                        attach.Enabled = !_rocmActing;
                        buttons.Controls.Add(attach);
                    }
                    if (v.State == "attached" || v.State == "refused" || v.State == "recovery")
                    {
                        var detach = Ui.Button(Strings.T("rocm.detach"), (s, e) => RunRocmDetach(venv.Path));
                        detach.Enabled = !_rocmActing;
                        buttons.Controls.Add(detach);
                    }

                    card.Add(name);
                    card.Add(state);
                    card.Add(buttons);
                }
            }
            else
            {
                card.Add(Ui.Dim(Strings.T("rocm.no-venvs"), card.Inner));
            }

            var browseRow = Ui.WrapRow(card.Inner);
            var browse = Ui.Button(Strings.T("rocm.browse"), (s, e) => BrowseRocmVenv());
            browse.Enabled = !_rocmActing && !_rocmChecking;
            browseRow.Controls.Add(browse);
            var refresh = Ui.Button(Strings.T("rocm.refresh"), (s, e) =>
            {
                _rocmVenvs = RocmAttach.DiscoverVenvs();
                RefreshRocmStates();
            });
            refresh.Enabled = !_rocmActing && !_rocmChecking;
            browseRow.Controls.Add(refresh);
            card.Add(browseRow);

            var result = ResultLine(card.Inner);
            if (result != null) card.Add(result);
            return card;
        }

        void RunRocmAttach(string venvPath)
        {
            if (_rocmPayload == null || _rocmActing) return;
            _rocmActing = true;
            ShowPage(_page, null, false);
            var payload = _rocmPayload;
            Task.Run(() => Crash.Watched("ROCm attach", () => RocmAttach.Attach(payload.Directory, venvPath))).ContinueWith(task =>
            {
                _rocmActing = false;
                if (task.Status == TaskStatus.RanToCompletion)
                {
                    var r = task.Result;
                    string msg = r.ExitCode == 0
                        ? RocmAttach.ParseActionResult(r.Output)
                        : Strings.T("rocm.failed", r.ExitCode);
                    Result(msg, r.ExitCode == 0);
                }
                else Result(Strings.T("rocm.failed", -1), false);
                _rocmVenvs = RocmAttach.DiscoverVenvs();
                RefreshRocmStates();
            }, TaskScheduler.FromCurrentSynchronizationContext());
        }

        void RunRocmDetach(string venvPath)
        {
            if (_rocmPayload == null || _rocmActing) return;
            _rocmActing = true;
            ShowPage(_page, null, false);
            var payload = _rocmPayload;
            Task.Run(() => Crash.Watched("ROCm detach", () => RocmAttach.Detach(payload.Directory, venvPath))).ContinueWith(task =>
            {
                _rocmActing = false;
                if (task.Status == TaskStatus.RanToCompletion)
                {
                    var r = task.Result;
                    string msg = r.ExitCode == 0
                        ? RocmAttach.ParseActionResult(r.Output)
                        : Strings.T("rocm.failed", r.ExitCode);
                    Result(msg, r.ExitCode == 0);
                }
                else Result(Strings.T("rocm.failed", -1), false);
                _rocmVenvs = RocmAttach.DiscoverVenvs();
                RefreshRocmStates();
            }, TaskScheduler.FromCurrentSynchronizationContext());
        }

        void BrowseRocmVenv()
        {
            if (_smoke) return;
            using (var dialog = new FolderBrowserDialog())
            {
                dialog.Description = Strings.T("rocm.browse.description");
                dialog.ShowNewFolderButton = false;
                if (dialog.ShowDialog(this) != DialogResult.OK) return;
                var path = dialog.SelectedPath;
                if (string.IsNullOrEmpty(path)) return;
                if (_rocmVenvs != null && _rocmVenvs.Any(v => string.Equals(v.Path, path, StringComparison.OrdinalIgnoreCase))) return;
                RunRocmAttach(path);
            }
        }

        static string ShortenPath(string path)
        {
            if (string.IsNullOrEmpty(path)) return "";
            if (path.Length <= 60) return path;
            var parts = path.Split('\\');
            if (parts.Length <= 3) return path;
            return parts[0] + "\\" + parts[1] + "\\...\\" + parts[parts.Length - 1];
        }
    }
}
