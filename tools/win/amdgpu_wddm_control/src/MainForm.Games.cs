// Games (WU-012..WU-015, WU-028, WU-054) and Graphics (B5 clock now vs after restart, the CU section of section 7,
// WU-055 restore defaults). Edits stay in the window until Apply; leaving with edits asks Save / Discard / Keep.
// Every write is one planned action through the elevated helper (game-profile, set-clocks, cu-mode, reset-defaults,
// graphics-defaults). The graphics settings rows are in MainForm.GfxSettings.cs.
using System;
using System.Collections.Generic;
using System.Drawing;
using System.IO;
using System.Linq;
using System.Windows.Forms;

namespace AmdgpuWddmControl
{
    public sealed partial class MainForm
    {
        // ---- Games model ----
        string _game;                                   // the image shown in the editor
        readonly Dictionary<string, Dictionary<string, bool>> _gameEdits = new Dictionary<string, Dictionary<string, bool>>(StringComparer.OrdinalIgnoreCase);
        readonly HashSet<string> _addedGames = new HashSet<string>(StringComparer.OrdinalIgnoreCase);
        bool _gameSortName, _showHidden;

        // ---- Graphics model ----
        bool? _autoEdit;                                // null: as stored
        uint? _ceilEdit;                                // the edited ceiling, null: unchecked (only while _ceilEdited)
        bool _ceilEdited;
        uint? _cuEdit;                                  // 24 or 40, null: as stored
        bool _keepGames = true;

        sealed class GameEntry
        {
            public string Image, Path, Stored, Recommended;
            public DateTime? LastLaunch;
            public bool D3D12;
            public bool Hidden;
            public bool OwnGfx;                         // has graphics or Vulkan settings of its own
        }

        // The changes of one game that Apply sends as one game-profile action: its switches, its graphics settings or both.
        sealed class GamePending
        {
            public string Image;
            public ProfileWrite Profile;                // null: the switches do not change
            public Dictionary<string, string> Gfx;      // null: the graphics settings do not change
        }

        bool GameEdited(string image) { return _gameEdits.ContainsKey(image) || _smEdits.ContainsKey(image) || _gfxGameEdits.ContainsKey(image); }

        bool AnyGameEdits { get { return _gameEdits.Count > 0 || _smEdits.Count > 0 || _gfxGameEdits.Count > 0; } }

        // One entry per file name: profiles are per file name (B3), so two folders with the same exe share one entry.
        List<GameEntry> GameList()
        {
            var map = new Dictionary<string, GameEntry>(StringComparer.OrdinalIgnoreCase);
            Func<string, GameEntry> get = image =>
            {
                GameEntry e;
                if (!map.TryGetValue(image, out e)) map[image] = e = new GameEntry { Image = image };
                return e;
            };
            foreach (var r in _recent)
            {
                if (!Profiles.IsValidImage(r.Image)) continue;
                var e = get(r.Image);
                if (e.LastLaunch == null || r.LastLaunchUtc > e.LastLaunch) { e.LastLaunch = r.LastLaunchUtc; e.Path = r.Path; e.D3D12 = r.D3D12; }
            }
            if (_snap.GameProfiles != null) foreach (var kv in _snap.GameProfiles) if (Profiles.IsValidImage(kv.Key)) get(kv.Key).Stored = kv.Value.Length == 0 ? null : kv.Value;
            if (_snap.DefaultApplications != null) foreach (var kv in _snap.DefaultApplications) if (Profiles.IsValidImage(kv.Key)) get(kv.Key).Recommended = kv.Value;
            if (_snap.GfxKeys != null)
                foreach (var image in GraphicsSettings.Games(_snap.GfxKeys)) if (GraphicsSettings.HasOwn(_snap.GfxKeys, image)) get(image).OwnGfx = true;
            foreach (var a in _addedGames) get(a);
            foreach (var g in _gameEdits.Keys.Concat(_smEdits.Keys).Concat(_gfxGameEdits.Keys)) get(g);
            var hidden = _prefs.HiddenGames;
            foreach (var e in map.Values) e.Hidden = hidden.Contains(e.Image);
            var list = map.Values.ToList();
            if (_gameSortName) list.Sort((a, b) => string.Compare(a.Image, b.Image, StringComparison.OrdinalIgnoreCase));
            else list = list.OrderByDescending(e => e.LastLaunch ?? DateTime.MinValue).ThenBy(e => e.Image, StringComparer.OrdinalIgnoreCase).ToList();
            return list;
        }

        string StoredOf(string image)
        {
            string v;
            return _snap.GameProfiles != null && _snap.GameProfiles.TryGetValue(image, out v) && v.Length > 0 ? v : null;
        }

        string RecommendedOf(string image)
        {
            string v;
            return _snap.DefaultApplications != null && _snap.DefaultApplications.TryGetValue(image, out v) ? v : null;
        }

        // The pending changes of the Games page, one per edited game that changes something.
        List<GamePending> GamePendings()
        {
            var list = new List<GamePending>();
            var images = new HashSet<string>(_gameEdits.Keys.Concat(_smEdits.Keys).Concat(_gfxGameEdits.Keys), StringComparer.OrdinalIgnoreCase);
            foreach (var image in images.OrderBy(i => i, StringComparer.OrdinalIgnoreCase))
            {
                var g = new GamePending { Image = image };
                Dictionary<string, bool> changes;
                _gameEdits.TryGetValue(image, out changes);
                string sm;
                _smEdits.TryGetValue(image, out sm);
                try
                {
                    var w = GameGroups.Plan(image, StoredOf(image), changes ?? new Dictionary<string, bool>(), sm);
                    if (w.Kind != ProfileWriteKind.None) g.Profile = w;
                }
                catch (ArgumentException) { }
                if (GfxGameWrites(image).Count > 0) g.Gfx = _gfxGameEdits[image];
                if (g.Profile != null || g.Gfx != null) list.Add(g);
            }
            return list;
        }

        bool ApplyGames()
        {
            bool all = true;
            foreach (var g in GamePendings())
            {
                var more = new Recovery.PlanArgs { Image = g.Image };
                if (g.Profile != null) more.Value = g.Profile.Kind == ProfileWriteKind.Set ? g.Profile.Value : "";
                if (g.Gfx != null) more.Gfx = GraphicsSettings.FormatEdits(g.Gfx);
                bool ok = RunActionAndWait("game-profile", more);
                if (ok) { _gameEdits.Remove(g.Image); _smEdits.Remove(g.Image); _gfxGameEdits.Remove(g.Image); }
                else { all = false; break; }
            }
            ShowPage(_page, null, false);
            return all;
        }

        void EditGroup(string image, GameGroup g, bool on)
        {
            Dictionary<string, bool> changes;
            if (!_gameEdits.TryGetValue(image, out changes)) _gameEdits[image] = changes = new Dictionary<string, bool>();
            var stored = GameGroups.State(g, StoredOf(image));
            if (stored == (on ? GroupState.On : GroupState.Off)) changes.Remove(g.Id); else changes[g.Id] = on;
            if (changes.Count == 0) _gameEdits.Remove(image);
        }

        // "Recommended" or "None" for one game (WU-054): every visible group follows the choice.
        void ResetGame(string image, bool recommended)
        {
            string rec = recommended ? RecommendedOf(image) : null;
            foreach (var g in GameGroups.All.Where(x => !x.SupportOnly || _prefs.ShowSupportOptions))
                EditGroup(image, g, rec != null && GameGroups.State(g, rec) != GroupState.Off);
            var sm = rec != null ? ShaderModelCeiling.State(rec) : ShaderModelCeiling.Default;
            if (sm == ShaderModelCeiling.State(StoredOf(image))) _smEdits.Remove(image); else _smEdits[image] = sm;
            if (!recommended) ResetGameGfx(image);
            ShowPage(_page, null, false);
        }

        Control BuildGames(int width)
        {
            var p = Frame("games", width);
            var list = new CardPanel(Strings.T("games.list.title"), width);
            var games = GameList();
            var filter = new TextBox { Width = Math.Min(Theme.S(280), list.Inner), Font = Theme.Body, BackColor = Theme.Nav, ForeColor = Theme.Text, BorderStyle = BorderStyle.FixedSingle, AccessibleName = Strings.T("games.filter"), Margin = Theme.Pad(0, 6, 10, 6) };
            filter.HandleCreated += (s, e) => SendMessage(filter.Handle, EM_SETCUEBANNER, (IntPtr)1, Strings.T("games.filter"));
            var sort = Ui.Button(Strings.T(_gameSortName ? "games.sort.recent" : "games.sort.name"), (s, e) => { _gameSortName = !_gameSortName; ShowPage(_page, null, false); });
            var hidden = Ui.Check(Strings.T("games.show-hidden"));
            hidden.Checked = _showHidden;
            hidden.CheckedChanged += (s, e) => { _showHidden = hidden.Checked; ShowPage(_page, null, false); };
            var add = Ui.Button(Strings.T("games.add"), (s, e) => AddGame());
            Mark("games.add", add);
            list.Add(Ui.WrapRow(list.Inner, filter, sort, hidden, add));
            var rows = new List<KeyValuePair<string, Control>>();
            foreach (var g in games)
            {
                if (g.Hidden && !_showHidden) continue;
                var entry = g;
                var name = Ui.Button(entry.Image + (GameEdited(entry.Image) ? "  *" : ""), (s, e) => { _game = entry.Image; ShowPage(_page, null, false); }, entry.Image.Equals(_game, StringComparison.OrdinalIgnoreCase));
                name.MinimumSize = new Size(Math.Min(Theme.S(200), list.Inner / 3), 0);
                name.TextAlign = ContentAlignment.MiddleLeft;
                name.AccessibleName = entry.Image + (GameEdited(entry.Image) ? ", " + Strings.T("games.unsaved") : "");
                string about = entry.LastLaunch != null ? Strings.T("games.launched", When(entry.LastLaunch.Value), Strings.T(entry.D3D12 ? "games.api.d3d12" : "games.api.d3d11")) : entry.Stored != null || entry.OwnGfx ? Strings.T("games.has-settings") : Strings.T("games.no-settings");
                int infoWidth = Math.Max(Theme.S(120), list.Inner - Math.Min(Theme.S(200), list.Inner / 3) - Theme.S(130));
                var info = Ui.Dim(about, infoWidth);
                info.MinimumSize = new Size(Math.Min(infoWidth, Theme.S(220)), 0);
                info.Margin = Theme.Pad(0, 12, 10, 0);
                var hide = Ui.Button(Strings.T(entry.Hidden ? "games.unhide" : "games.hide"), (s, e) =>
                {
                    var set = _prefs.HiddenGames;
                    if (entry.Hidden) set.Remove(entry.Image); else set.Add(entry.Image);
                    _prefs.HiddenGames = set;
                    ShowPage(_page, null, false);
                });
                hide.AccessibleName = Strings.T(entry.Hidden ? "games.unhide" : "games.hide") + ": " + entry.Image;
                var row = Ui.WrapRow(list.Inner, name, info, hide);
                rows.Add(new KeyValuePair<string, Control>(entry.Image, row));
                list.Add(row);
            }
            if (rows.Count == 0) list.Add(Ui.Dim(Strings.T(_prefs.RecordRecentLaunches ? "games.empty" : "games.empty.off"), list.Inner));
            filter.TextChanged += (s, e) =>
            {
                var q = SettingsSearch.Normalize(filter.Text);
                foreach (var r in rows) r.Value.Visible = q.Length == 0 || SettingsSearch.Normalize(r.Key).Contains(q);
            };
            list.Add(Ui.Dim(Strings.T("games.hide.note"), list.Inner));
            p.Controls.Add(list);

            var shown = games.FirstOrDefault(g => g.Image.Equals(_game, StringComparison.OrdinalIgnoreCase));
            if (shown == null) { p.Controls.Add(Ui.Dim(Strings.T("games.select"), width)); return p; }
            p.Controls.Add(BuildGameEditor(shown, width));
            return p;
        }

        Control BuildGameEditor(GameEntry game, int width)
        {
            var c = new CardPanel(game.Image, width);
            c.Add(Ui.Dim(Strings.T("games.scope", game.Image), c.Inner));
            if (Hints.Read(_recent).RunningGames.Any(r => r.Equals(game.Image, StringComparison.OrdinalIgnoreCase)))
                c.Add(Ui.Label(Strings.T("games.running", game.Image), null, Theme.Warn, c.Inner));
            Dictionary<string, bool> changes;
            _gameEdits.TryGetValue(game.Image, out changes);
            var now = GameGroups.Apply(game.Stored, changes);
            string nowValue = string.Join(",", now);
            c.Add(SectionLabel(Strings.T("games.section.d3d12"), c.Inner));
            foreach (var g in GameGroups.All)
            {
                if (g.SupportOnly && !_prefs.ShowSupportOptions) continue;
                var group = g;
                var state = GameGroups.State(group, nowValue);
                var box = Ui.Check(group.Title, c.Inner - Theme.S(40));
                box.CheckState = state == GroupState.On ? CheckState.Checked : state == GroupState.Partial ? CheckState.Indeterminate : CheckState.Unchecked;
                string origin = changes != null && changes.ContainsKey(group.Id) ? Strings.T("games.changed") : GameGroups.OriginText(GameGroups.Origin(group, game.Stored, game.Recommended));
                box.AccessibleDescription = origin + (state == GroupState.Partial ? ", " + Strings.T("game.state.partial") : "");
                box.CheckedChanged += (s, e) => { EditGroup(game.Image, group, box.Checked); ShowPage(_page, null, false); };
                if (group.Id == "rt" || group.Id == "present" || group.Id == "cpu") Mark("games." + group.Id, box);
                c.Add(Ui.Row(box, Explain(group.Id, group.Title)));
                var detail = Ui.Dim(origin + (state == GroupState.Partial ? " - " + Strings.T("game.state.partial") : "") + ". " + group.Cost, c.Inner - Theme.S(26));
                detail.Margin = Theme.Pad(26, 0, 0, 8);
                c.Add(detail);
            }
            if (_prefs.ShowSupportOptions)
            {
                var other = GameGroups.Hidden(nowValue).Where(n => Profiles.Find(n) == null).ToList();
                if (other.Count > 0) c.Add(Ui.Dim(Strings.T("games.other-kept", string.Join(", ", other)), c.Inner));
            }
            else if (GameGroups.Hidden(nowValue).Count > 0) c.Add(Ui.Dim(Strings.T("games.hidden-kept"), c.Inner));
            AddShaderModel(c, game);
            AddGameGfx(c, game);
            c.Add(Ui.Dim(Strings.T("games.when", game.Image), c.Inner));

            var writes = GamePendings();
            bool dirty = writes.Any(w => w.Image.Equals(game.Image, StringComparison.OrdinalIgnoreCase));
            if (AnyGameEdits) c.Add(Ui.Label(Strings.T(writes.Count > 0 ? "games.unsaved.count" : "games.unsaved.none", writes.Count), null, writes.Count > 0 ? Theme.Warn : Theme.Dim, c.Inner));
            var apply = Ui.Button(Strings.T("ui.apply"), (s, e) => ApplyGames(), true);
            apply.Enabled = writes.Count > 0;
            var discard = Ui.Button(Strings.T("ui.discard"), (s, e) => { DiscardPage("games"); ShowPage(_page, null, false); });
            discard.Enabled = AnyGameEdits;
            c.Add(Ui.WrapRow(c.Inner, apply, discard));

            var backups = RecoveryProbe.Backups();
            var undo = Ui.Button(Strings.T("games.undo"), (s, e) => RunAction("game-undo", new Recovery.PlanArgs { Image = game.Image }));
            undo.Enabled = !dirty && Recovery.GameUndoTarget(backups, game.Image) != null;
            Mark("games.undo", undo);
            var redo = Ui.Button(Strings.T("games.redo"), (s, e) => RunAction("game-redo", new Recovery.PlanArgs { Image = game.Image }));
            redo.Enabled = !dirty && Recovery.GameRedoTarget(backups, game.Image) != null;
            var resetRow = Ui.WrapRow(c.Inner, undo, redo);
            if (game.Recommended != null) resetRow.Controls.Add(Ui.Button(Strings.T("games.reset.recommended"), (s, e) => ResetGame(game.Image, true)));
            resetRow.Controls.Add(Ui.Button(Strings.T("games.reset.none"), (s, e) => ResetGame(game.Image, false)));
            c.Add(resetRow);
            if (game.Recommended != null) c.Add(Ui.Dim(Strings.T("games.reset.note"), c.Inner));
            var result = ResultLine(c.Inner);
            if (result != null) c.Add(result);
            return c;
        }

        void AddGame()
        {
            if (_smoke) return;
            using (var dialog = new OpenFileDialog { Title = Strings.T("games.add.title"), Filter = Strings.T("games.add.filter") + " (*.exe)|*.exe", CheckFileExists = true })
            {
                if (dialog.ShowDialog(this) != DialogResult.OK) return;
                var image = Path.GetFileName(dialog.FileName);
                if (!Profiles.IsValidImage(image)) { Result(Strings.T("games.add.invalid"), false); return; }
                _addedGames.Add(image);
                _game = image;
                ShowPage(_page, null, false);
            }
        }

        // ---- Graphics ------------------------------------------------------------------------------------------------

        uint? StoredMode { get { return _snap.P("DpmMode"); } }
        uint? StoredMax { get { return _snap.P("DpmMaxMHz"); } }
        bool AutoChosen { get { return _autoEdit ?? StoredMode == 1; } }
        uint? CeilingChosen { get { return _ceilEdited ? _ceilEdit : StoredMax; } }

        List<RegWrite> ClockWrites()
        {
            if (!_snap.DriverInstalled) return new List<RegWrite>();
            return DpmSettings.PlanWrites(StoredMode, StoredMax, AutoChosen, CeilingChosen);
        }

        CuView CuNow() { return CuMode.View(_snap.Cu, _snap.Health != null ? (ulong?)_snap.Health.Generation : null, _snap.StoredCu(), _cuConfirmFailed); }

        bool CuChoiceChanged { get { return _cuEdit != null && _cuEdit.Value != CuNow().ChoiceMode; } }

        bool ApplyGraphics()
        {
            // The settings for all games first: they need no restart.
            bool ok = ApplyGfxGlobal();
            if (ok && ClockWrites().Count > 0)
            {
                ok = RunActionAndWait("set-clocks", null, AutoChosen ? 1u : (uint?)null, CeilingChosen, true);
                if (ok) { _autoEdit = null; _ceilEdited = false; _ceilEdit = null; }
            }
            if (ok && CuChoiceChanged)
            {
                ok = RunActionAndWait("cu-mode", new Recovery.PlanArgs { Cu = _cuEdit });
                if (ok) _cuEdit = null;
            }
            ShowPage(_page, null, false);
            return ok;
        }

        Control BuildGraphics(int width)
        {
            var p = Frame("graphics", width);
            bool installed = _snap.DriverInstalled;

            var clock = new CardPanel(Strings.T("graphics.clock.title"), width);
            clock.Pair(Strings.T("graphics.clock.now"), installed ? ClockNow() : Strings.T("perf.no-reading"));
            uint shownMax = CeilingChosen ?? DpmSettings.DefaultMaxMHz;
            clock.Pair(Strings.T("graphics.clock.after"), ClockText(AutoChosen ? 1u : 0u, shownMax), ClockWrites().Count > 0 ? Theme.Warn : (Color?)null);
            var auto = Ui.Check(Strings.T("search.graphics.clock-auto"), clock.Inner - Theme.S(40));
            auto.Checked = AutoChosen;
            auto.CheckedChanged += (s, e) => { _autoEdit = auto.Checked == (StoredMode == 1) ? (bool?)null : auto.Checked; ShowPage(_page, null, false); };
            Mark("graphics.clock-auto", auto);
            clock.Add(Ui.Row(auto, Explain("clock-auto", auto.Text)));
            clock.Add(Ui.Dim(Strings.T("graphics.clock-auto.default"), clock.Inner));
            var ceilOn = Ui.Check(Strings.T("graphics.clock-ceiling.check"));
            ceilOn.Checked = CeilingChosen != null;
            var picker = new CeilingPicker();
            if (CeilingChosen != null) picker.Value = CeilingChosen.Value;
            ceilOn.CheckedChanged += (s, e) => { SetCeiling(ceilOn.Checked ? (uint?)picker.Value : null); };
            picker.Stepped += (s, e) => SetCeiling(picker.Value);
            Mark("graphics.clock-ceiling", ceilOn);
            clock.Add(Ui.WrapRow(clock.Inner, ceilOn, picker, Explain("clock-ceiling", Strings.T("search.graphics.clock-ceiling"))));
            clock.Add(Ui.Dim(Strings.T("graphics.clock-ceiling.default", DpmSettings.DefaultMaxMHz), clock.Inner));
            clock.Add(Ui.Dim(Strings.T("graphics.thermal"), clock.Inner));
            p.Controls.Add(clock);

            var cores = new CardPanel(Strings.T("graphics.cores.title"), width);
            Mark("graphics.cores", cores);
            var view = CuNow();
            if (!installed) cores.Add(Ui.Dim(Strings.T("graphics.not-installed"), cores.Inner));
            else
            {
                cores.Add(Ui.Label(view.Running, Theme.Bold, null, cores.Inner));
                cores.Add(Ui.Label(view.Choice, null, null, cores.Inner));
                cores.Add(Ui.Dim(view.NextStart, cores.Inner));
                if (!string.IsNullOrEmpty(view.Note)) cores.Add(Ui.Dim(view.Note, cores.Inner));
                uint chosen = _cuEdit ?? view.ChoiceMode;
                var r24 = Ui.Radio(Strings.T("graphics.cores.24"));
                var r40 = Ui.Radio(Strings.T("graphics.cores.40"));
                r24.Checked = chosen == CuMode.Stock; r40.Checked = chosen == CuMode.Full;
                r24.CheckedChanged += (s, e) => { if (r24.Checked) { _cuEdit = CuMode.Stock; ShowPage(_page, null, false); } };
                r40.CheckedChanged += (s, e) => { if (r40.Checked) { _cuEdit = CuMode.Full; ShowPage(_page, null, false); } };
                cores.Add(Ui.WrapRow(cores.Inner, r24, r40, Explain("cores", Strings.T("graphics.cores.title"))));
                if (chosen == CuMode.Full) cores.Add(Ui.Label(CuMode.EffectText(), null, Theme.Warn, cores.Inner));
                cores.Add(Ui.Dim(Strings.T("cu.choice.after-restart"), cores.Inner));
                var offers = Ui.WrapRow(cores.Inner);
                if (view.OfferConfirm) offers.Controls.Add(Ui.Button(Strings.T("status.action.cu-confirm"), (s, e) => RunAction("cu-confirm"), true));
                if (view.OfferReport) offers.Controls.Add(Ui.Button(Strings.T("help.report.create"), (s, e) => Navigate("help", "help.report")));
                if (offers.Controls.Count > 0) cores.Add(offers);
            }
            p.Controls.Add(cores);

            p.Controls.Add(BuildGfxGlobalCard(width));

            // Apply / Discard for the clock, the cores and the settings for all games (one planned action each).
            var save = new CardPanel(null, width);
            int pending = ClockWrites().Count + (CuChoiceChanged ? 1 : 0) + GfxGlobalWrites().Count;
            save.Add(Ui.Label(Strings.T(pending > 0 ? "graphics.unsaved" : "graphics.saved"), null, pending > 0 ? Theme.Warn : Theme.Dim, save.Inner));
            var apply = Ui.Button(Strings.T("ui.apply"), (s, e) => ApplyGraphics(), true);
            apply.Enabled = installed && pending > 0;
            var discard = Ui.Button(Strings.T("ui.discard"), (s, e) => { DiscardPage("graphics"); ShowPage(_page, null, false); });
            discard.Enabled = pending > 0;
            save.Add(Ui.WrapRow(save.Inner, apply, discard));
            var result = ResultLine(save.Inner);
            if (result != null) save.Add(result);
            p.Controls.Add(save);

            // The tuning cards (docs/design/tuner.md): their own Apply, because they write no setting that a restart
            // would keep - the driver holds each trial and takes it back by itself.
            AddTuning(p, width, installed);

            var reset = new CardPanel(Strings.T("search.graphics.reset"), width);
            Mark("graphics.reset", reset);
            reset.Add(Ui.Dim(Strings.T("graphics.reset.text"), reset.Inner));
            var keep = Ui.Check(Strings.T("graphics.reset.keep-games"), reset.Inner);
            keep.Checked = _keepGames;
            keep.CheckedChanged += (s, e) => _keepGames = keep.Checked;
            reset.Add(keep);
            var resetButton = Ui.Button(Strings.T("graphics.reset.button"), (s, e) => RunAction("reset-defaults", new Recovery.PlanArgs { Games = _keepGames ? "keep" : "reset" }));
            resetButton.Enabled = installed;
            var undoTarget = Recovery.UndoTarget(RecoveryProbe.Backups());
            var undoButton = Ui.Button(Strings.T("graphics.undo"), (s, e) => RunAction("undo"));
            undoButton.Enabled = undoTarget != null;
            reset.Add(Ui.WrapRow(reset.Inner, resetButton, undoButton, Explain("reset", Strings.T("search.graphics.reset"))));
            reset.Add(Ui.Dim(undoTarget == null ? Strings.T("graphics.undo.none") : Strings.T("graphics.undo.when", UndoWhen(undoTarget.Utc)), reset.Inner));
            p.Controls.Add(reset);

            p.Controls.Add(LaterCard(width, "later.sharpen", "later.aa"));
            return p;
        }

        static string UndoWhen(string utc)
        {
            DateTime t;
            return DateTime.TryParse(utc, System.Globalization.CultureInfo.InvariantCulture, System.Globalization.DateTimeStyles.AdjustToUniversal | System.Globalization.DateTimeStyles.AssumeUniversal, out t)
                ? When(DateTime.SpecifyKind(t, DateTimeKind.Utc)) : utc;
        }

        void SetCeiling(uint? value)
        {
            _ceilEdited = value != StoredMax;
            _ceilEdit = _ceilEdited ? value : null;
            ShowPage(_page, null, false);
        }
    }
}
