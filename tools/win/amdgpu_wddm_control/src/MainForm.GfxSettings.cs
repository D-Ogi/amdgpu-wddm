// The graphics settings for all games (the Graphics page) and for one game (the Games page), over GraphicsSettings.cs,
// and the shader model choice of a game (ShaderModelCeiling). Edits stay in the window until Apply, like every other
// edit of these pages; the writes are planned actions (graphics-defaults, game-profile with --gfx).
using System;
using System.Collections.Generic;
using System.Drawing;
using System.Linq;
using System.Windows.Forms;

namespace AmdgpuWddmControl
{
    public sealed partial class MainForm
    {
        // name -> the chosen value, null for "Application decides" / "Same as all games"; only real changes are kept.
        readonly Dictionary<string, string> _gfxGlobalEdits = new Dictionary<string, string>(StringComparer.Ordinal);
        readonly Dictionary<string, Dictionary<string, string>> _gfxGameEdits = new Dictionary<string, Dictionary<string, string>>(StringComparer.OrdinalIgnoreCase);
        // image -> the chosen shader model ceiling ("6.8", "6.7", "6.6"), only when it differs from the stored one.
        readonly Dictionary<string, string> _smEdits = new Dictionary<string, string>(StringComparer.OrdinalIgnoreCase);

        static readonly string[] GfxGraphicsNames = { "FrameRateLimit", "VSync", "Anisotropy", "MaxFrameLatency", "PerformanceOverlay", "RenderOnCpu", "ReportAmdDriverVersion" };
        // Empty while WsiRoute and MemoryOverflow wait for their ICD (GraphicsSettings.AwaitingIcd); with no name the
        // Vulkan section is not shown.
        static readonly string[] GfxVulkanNames = { };

        List<RegWrite> GfxGlobalWrites()
        {
            if (_snap.GfxKeys == null || _gfxGlobalEdits.Count == 0) return new List<RegWrite>();
            try { return GraphicsSettings.PlanWrites(_snap.GfxKeys, null, _gfxGlobalEdits); }
            catch (ArgumentException) { return new List<RegWrite>(); }
        }

        List<RegWrite> GfxGameWrites(string image)
        {
            Dictionary<string, string> edits;
            if (_snap.GfxKeys == null || !_gfxGameEdits.TryGetValue(image, out edits) || edits.Count == 0) return new List<RegWrite>();
            try { return GraphicsSettings.PlanWrites(_snap.GfxKeys, image, edits); }
            catch (ArgumentException) { return new List<RegWrite>(); }
        }

        bool ApplyGfxGlobal()
        {
            if (GfxGlobalWrites().Count == 0) { _gfxGlobalEdits.Clear(); return true; }
            bool ok = RunActionAndWait("graphics-defaults", new Recovery.PlanArgs { Gfx = GraphicsSettings.FormatEdits(_gfxGlobalEdits) });
            if (ok) _gfxGlobalEdits.Clear();
            return ok;
        }

        // A choice list in the theme (Ui.cs DropChoice): as wide as its longest entry, at most max.
        static DropChoice Choices(IList<string> texts, string name, int max) { return new DropChoice(texts, name, max); }

        // The widest a choice may be in a row with its label and its "?".
        static int ChoiceMax(int inner) { return inner - KeyWidth(inner) - Theme.S(96); }

        // The label column of a setting row.
        static int KeyWidth(int inner) { return Math.Min(Theme.S(230), inner / 3); }

        Label RowLabel(string text, int inner)
        {
            int w = KeyWidth(inner);
            var l = Ui.Label(text, null, null, w);
            l.MinimumSize = new Size(w, 0);
            l.Margin = Theme.Pad(0, 8, 8, 0);
            return l;
        }

        Label RowDetail(string text, int inner, Color? color = null)
        {
            int w = KeyWidth(inner);
            var l = Ui.Label(text, null, color ?? Theme.Dim, inner - w);
            l.Margin = new Padding(w, 0, 0, Theme.S(6));
            return l;
        }

        static string GfxNoteId(GfxSetting s) { return "gfx.note." + s.Id; }

        // One setting: its name, its choices and its "?", then where the value comes from (a game) and its note.
        // image null: the settings for all games.
        void AddGfxRow(CardPanel c, GfxSetting s, string image)
        {
            var view = GraphicsSettings.View(s, _snap.GfxKeys, image);
            Dictionary<string, string> edits;
            if (image == null) edits = _gfxGlobalEdits;
            else if (!_gfxGameEdits.TryGetValue(image, out edits)) edits = new Dictionary<string, string>(StringComparer.Ordinal);
            string edit;
            bool edited = edits.TryGetValue(s.Name, out edit);
            Control input;
            if (s.Name == "ReportAmdDriverVersion")
            {
                var box = Ui.Check(s.Title, c.Inner - Theme.S(40));
                box.Checked = GraphicsSettings.Checked(view.Here, edited, edit);
                box.CheckedChanged += (o, e) =>
                {
                    GraphicsSettings.Check(_gfxGlobalEdits, s, view.Here, box.Checked);
                    BeginInvoke((Action)(() => ShowPage(_page, null, false)));
                };
                input = box;
                c.Add(Ui.Row(box, Explain(s.Id, s.Title)));
            }
            else
            {
                var items = GraphicsSettings.Items(view);
                var combo = Choices(items.Select(x => x.Text).ToList(), s.Title, ChoiceMax(c.Inner));
                combo.SelectedIndex = GraphicsSettings.Selected(items, view.Here, edited, edit);
                string gameImage = image;
                combo.SelectionChangeCommitted += (o, e) =>
                {
                    if (combo.SelectedIndex < 0) return;
                    Dictionary<string, string> target;
                    if (gameImage == null) target = _gfxGlobalEdits;
                    else if (!_gfxGameEdits.TryGetValue(gameImage, out target)) _gfxGameEdits[gameImage] = target = new Dictionary<string, string>(StringComparer.Ordinal);
                    GraphicsSettings.Choose(target, s, view.Here, items[combo.SelectedIndex]);
                    if (gameImage != null && target.Count == 0) _gfxGameEdits.Remove(gameImage);
                    BeginInvoke((Action)(() => ShowPage(_page, null, false)));
                };
                input = combo;
                c.Add(Ui.WrapRow(c.Inner, RowLabel(s.Title, c.Inner), combo, Explain(s.Id, s.Title)));
            }
            if (image == null && !s.GameFirst) Mark(s.SearchId, input);
            if (image != null && s.GameFirst) Mark(s.SearchId, input);
            // A game always says where its value comes from; the card for all games only when it is not plain.
            if (image != null || edited || view.Global.State == GfxState.Invalid || view.Global.State == GfxState.Other)
                c.Add(RowDetail(GraphicsSettings.OriginText(view, edited), c.Inner, edited ? Theme.Warn : (Color?)null));
            if (Strings.Has(GfxNoteId(s)))
                c.Add(RowDetail(Strings.T(GfxNoteId(s)), c.Inner, s.Name == "RenderOnCpu" ? Theme.Warn : (Color?)null));
        }

        // The Graphics page's card: the settings for all games.
        CardPanel BuildGfxGlobalCard(int width)
        {
            var c = new CardPanel(Strings.T("gfx.global.title"), width);
            c.Add(Ui.Dim(Strings.T("gfx.global.intro"), c.Inner));
            if (_snap.GfxKeys == null) { c.Add(Ui.Dim(Strings.T("gfx.unreadable"), c.Inner)); return c; }
            foreach (var name in GfxGraphicsNames)
            {
                var s = GraphicsSettings.Find(name);
                // Running DirectX 11 on the processor is a last resort for one game: the card for all games shows it
                // only when it is stored there (so that it can be taken back).
                if (s.GameFirst && GraphicsSettings.View(s, _snap.GfxKeys, null).Global.State == GfxState.Absent && !_gfxGlobalEdits.ContainsKey(s.Name)) continue;
                AddGfxRow(c, s, null);
            }
            if (GfxVulkanNames.Length > 0) c.Add(Ui.Label(Strings.T("gfx.section.vulkan"), Theme.Bold, null, c.Inner));
            foreach (var name in GfxVulkanNames) AddGfxRow(c, GraphicsSettings.Find(name), null);
            return c;
        }

        // The game editor's sections after the DirectX 12 switches: the shader model, then the game's own graphics
        // and Vulkan settings.
        void AddShaderModel(CardPanel c, GameEntry game)
        {
            string stored = ShaderModelCeiling.State(game.Stored);
            string edit;
            bool edited = _smEdits.TryGetValue(game.Image, out edit);
            string chosen = edited ? edit : stored;
            var title = Strings.T("search.games.sm");
            var combo = Choices(ShaderModelCeiling.Choices.Select(ShaderModelCeiling.Label).ToList(), title, ChoiceMax(c.Inner));
            combo.SelectedIndex = Array.IndexOf(ShaderModelCeiling.Choices, chosen);
            combo.SelectionChangeCommitted += (o, e) =>
            {
                if (combo.SelectedIndex < 0) return;
                var pick = ShaderModelCeiling.Choices[combo.SelectedIndex];
                if (pick == stored) _smEdits.Remove(game.Image); else _smEdits[game.Image] = pick;
                BeginInvoke((Action)(() => ShowPage(_page, null, false)));
            };
            Mark("games.sm", combo);
            c.Add(Ui.WrapRow(c.Inner, RowLabel(title, c.Inner), combo, Explain("sm", title)));
            c.Add(RowDetail(edited ? Strings.T("games.changed") : GameGroups.OriginText(ShaderModelCeiling.Origin(game.Stored, game.Recommended)), c.Inner, edited ? Theme.Warn : (Color?)null));
            c.Add(RowDetail(Strings.T("gfx.note.sm"), c.Inner));
        }

        void AddGameGfx(CardPanel c, GameEntry game)
        {
            c.Add(SectionLabel(Strings.T("games.section.graphics"), c.Inner));
            if (_snap.GfxKeys == null) { c.Add(Ui.Dim(Strings.T("gfx.unreadable"), c.Inner)); return; }
            foreach (var name in GfxGraphicsNames.Where(n => !GraphicsSettings.Find(n).GlobalOnly)) AddGfxRow(c, GraphicsSettings.Find(name), game.Image);
            if (GfxVulkanNames.Length > 0) c.Add(SectionLabel(Strings.T("gfx.section.vulkan"), c.Inner));
            foreach (var name in GfxVulkanNames) AddGfxRow(c, GraphicsSettings.Find(name), game.Image);
        }

        static Label SectionLabel(string text, int inner)
        {
            var l = Ui.Label(text, Theme.Bold, Theme.Teal, inner);
            l.Margin = Theme.Pad(0, 10, 0, 2);
            return l;
        }

        // "Use the driver defaults" for one game: every graphics and Vulkan setting of the game back to "Same as all
        // games", and the shader model back to 6.8. "Use the recommended settings" leaves them: the installer
        // recommends none.
        void ResetGameGfx(string image)
        {
            if (_snap.GfxKeys == null) return;
            var edits = new Dictionary<string, string>(StringComparer.Ordinal);
            foreach (var s in GraphicsSettings.All.Where(x => !x.GlobalOnly))
                if (GraphicsSettings.View(s, _snap.GfxKeys, image).Game.State != GfxState.Absent) edits[s.Name] = null;
            if (edits.Count > 0) _gfxGameEdits[image] = edits; else _gfxGameEdits.Remove(image);
        }
    }
}
