// The window's build gates, all without showing it (the owner's rule: no window on the development PC).
//   Describe()        --smoke: every page's text, support view included.
//   RenderPages(...)  --smoke-render (G-RENDER, G-NOINT, G-A11Y): every page drawn to PNG at the default and the
//                     minimum window size; overlap, horizontal overflow, scroll range and page width checked; the text
//                     of every page outside the support view checked for internals; every interactive control checked
//                     for an accessible name; the guide panel's text present with and without the character.
//   UseFixture(...)   a recorded snapshot and demo games instead of this PC's state, so that the cards with
//                     warnings, games and the CU section are drawn too. Nothing is written in any of them.
using System;
using System.Collections.Generic;
using System.Drawing;
using System.IO;
using System.Linq;
using System.Text;
using System.Text.RegularExpressions;
using System.Windows.Forms;

namespace AmdgpuWddmControl
{
    public sealed partial class MainForm
    {
        bool _fixture;

        public void UseFixture(RecoverySnapshot s)
        {
            _fixture = true;
            _snap = s ?? new RecoverySnapshot();
            if (_snap.GameProfiles == null) _snap.GameProfiles = new Dictionary<string, string>(StringComparer.OrdinalIgnoreCase);
            _snap.GameProfiles["witcher3.exe"] = "raytracing-tier,present-noprimary,x-future-switch";
            _snap.GameProfiles["ascent.exe"] = "recording-bind,retire-handoff,deferred-replay";
            if (_snap.DefaultApplications == null) _snap.DefaultApplications = new Dictionary<string, string>(StringComparer.OrdinalIgnoreCase);
            _snap.DefaultApplications["witcher3.exe"] = "raytracing-tier";
            var now = DateTime.UtcNow;
            _recent = new List<RecentLaunch>
            {
                new RecentLaunch { Key = "00", Image = "witcher3.exe", Path = @"D:\Games\The Witcher 3\bin\x64_dx12\witcher3.exe", LastLaunchUtc = now.AddHours(-2), Api = "d3d12", Launches = 12 },
                new RecentLaunch { Key = "01", Image = "ROTTR.exe", Path = @"D:\Games\Rise of the Tomb Raider\ROTTR.exe", LastLaunchUtc = now.AddDays(-1), Api = "d3d12", Launches = 3 },
                new RecentLaunch { Key = "02", Image = "factorio.exe", Path = @"D:\Games\Factorio\bin\x64\factorio.exe", LastLaunchUtc = now.AddDays(-3), Api = "d3d11", Launches = 40 },
            };
            _upd = new UpdateCache { LastSuccessUtc = Recovery.Stamp(now.AddMinutes(-30)), LastAttemptUtc = Recovery.Stamp(now.AddMinutes(-30)), LastAttemptOutcome = "Available", CandidateTag = "v1.0.1.0-tester.12", CandidateVersion = "1.0.1.0-tester.12" };
            _drv = DriverCard.Decide(new DriverFacts { InstalledVersion = "1.0.0.0-tester.11", DriverDate = "10-3-2026" });
            _vram = new VideoMemoryState { Segments = 1, LocalResident = 3L << 30, Dedicated = 8L << 30, LocalLimit = 8L << 30 };
            _game = "witcher3.exe";
            _gameEdits["witcher3.exe"] = new Dictionary<string, bool> { { "cpu", true } };
            _ceilEdited = true; _ceilEdit = 1800;
            ComputeStatus();
            ShowPage(_page, null, false);
        }

        // ---- --smoke ------------------------------------------------------------------------------------------------

        public string Describe()
        {
            var w = new StringBuilder();
            w.AppendLine(Program.ProductName + " " + Program.VersionText);
            w.AppendLine("Status: " + _status.Title);
            foreach (var i in _status.Items) w.AppendLine("  [" + i.Severity + "] " + i.Text + (i.Action != null ? " -> " + i.Action : ""));
            w.AppendLine("Guide: " + _verdict.Cause + " (" + _verdict.Expression + ")");
            string back = _page;
            foreach (var page in PageNames)
            {
                ShowPage(page, null, false);
                CreateHandles(this);
                w.AppendLine("[" + page + "]");
                foreach (var t in Texts(_content, false)) w.AppendLine("  " + t.Replace("\r", " ").Replace("\n", " "));
            }
            ShowPage(back, null, false);
            return w.ToString();
        }

        // ---- --smoke-render ------------------------------------------------------------------------------------------

        public sealed class RenderOptions
        {
            public string Directory, SwitchTo;
            public bool Nagi;
        }

        public string RenderPages(RenderOptions o)
        {
            var w = new StringBuilder();
            foreach (var size in new[] { new { Name = "", Size = Theme.Sz(1240, 760) }, new { Name = "-min", Size = MinimumSize } })
            {
                ClientSize = size.Size;
                BuildShell();
                foreach (var page in PageNames)
                {
                    ShowPage(page, null, false);
                    PerformLayout();
                    CreateHandles(this);
                    Snap(Path.Combine(o.Directory, page + size.Name + ".png"));
                    string where = page + size.Name;
                    var built = _content.Controls.Count > 0 ? _content.Controls[0] : null;
                    if (built == null) { w.AppendLine(where + ": empty page"); continue; }
                    if (size.Name == "")
                        using (var bmp = new Bitmap(Math.Max(1, built.Width), Math.Max(1, built.Height)))
                        {
                            built.DrawToBitmap(bmp, new Rectangle(0, 0, built.Width, built.Height));
                            bmp.Save(Path.Combine(o.Directory, page + "-full.png"), System.Drawing.Imaging.ImageFormat.Png);
                        }
                    var last = LastShown(built);
                    int bottom = _content.PointToClient(last.Parent.PointToScreen(last.Location)).Y + last.Height;
                    int extent = _content.DisplayRectangle.Bottom + _content.Padding.Bottom;
                    if (bottom > extent) w.AppendLine(where + ": the last control " + Label(last) + " ends at " + bottom + ", outside the scroll range " + extent);
                    if (built.Width > ColumnWidth + Theme.S(2)) w.AppendLine(where + ": the page is " + built.Width + " px wide, the column has " + ColumnWidth);
                    foreach (var p in Overlaps(this)) w.AppendLine(where + ": " + p);
                    foreach (var p in Overflows(this)) w.AppendLine(where + ": " + p);
                    foreach (var p in NoInternals(Texts(this, true))) w.AppendLine(where + ": G-NOINT " + p);
                    foreach (var p in Accessibility(this)) w.AppendLine(where + ": G-A11Y " + p);
                    if (_guide == null || string.IsNullOrEmpty(_guide.Text2)) w.AppendLine(where + ": the guide panel has no text");
                    if (_guide != null && _guide.ArtShown && !_prefs.ShowNagi) w.AppendLine(where + ": G-ART art shown with Show Nagi unchecked");
                    if (_guide != null && _guide.Playing) w.AppendLine(where + ": G-PERF a frame sequence plays in a window that is not shown");
                }
            }
            if (!ThreeWayDialog.EscapeKeepsEditing()) w.AppendLine("G-A11Y: Escape in Save / Discard / Keep editing does not keep editing");
            if (o.SwitchTo != null)
            {
                SwitchLanguageForTest(o.SwitchTo);
                ShowPage("home", null, false);
                CreateHandles(this);
                Snap(Path.Combine(o.Directory, "home-switched-" + o.SwitchTo + ".png"));
                foreach (var kv in _navButtons)
                    if (kv.Value.Text != Strings.In(o.SwitchTo, "nav." + kv.Key)) w.AppendLine("language switch: the " + kv.Key + " button still reads " + kv.Value.Text);
                foreach (var p in Overlaps(this)) w.AppendLine("home after the switch: " + p);
            }
            return w.ToString();
        }

        void Snap(string path)
        {
            using (var bmp = new Bitmap(Width, Height))
            {
                DrawToBitmap(bmp, new Rectangle(0, 0, Width, Height));
                bmp.Save(path, System.Drawing.Imaging.ImageFormat.Png);
            }
        }

        // ---- the checks --------------------------------------------------------------------------------------------

        // Control.Visible reads false for every control of a form that was never shown, so the checks use the control's
        // own visible flag (STATE_VISIBLE).
        static readonly System.Reflection.MethodInfo GetState = typeof(Control).GetMethod("GetState", System.Reflection.BindingFlags.NonPublic | System.Reflection.BindingFlags.Instance);
        static bool HasVisibleFlag(Control c) { return (bool)GetState.Invoke(c, new object[] { 2 }); }

        static void CreateHandles(Control c)
        {
            if (!c.IsHandleCreated) { var h = c.Handle; }
            foreach (Control k in c.Controls) CreateHandles(k);
        }

        static IEnumerable<Control> ShownIn(Control root, bool skipSupport)
        {
            foreach (Control c in root.Controls)
            {
                if (!HasVisibleFlag(c)) continue;
                if (skipSupport && "support".Equals(c.Tag)) continue;
                yield return c;
                foreach (var k in ShownIn(c, skipSupport)) yield return k;
            }
        }

        public static List<string> Texts(Control root, bool skipSupport)
        {
            var list = new List<string>();
            foreach (var c in ShownIn(root, skipSupport))
            {
                if (c is TextBox || c is ListBox) continue;
                if (!string.IsNullOrEmpty(c.Text)) list.Add(c.Text);
                if (!string.IsNullOrEmpty(c.AccessibleName) && c.AccessibleName != c.Text) list.Add(c.AccessibleName);
            }
            return list;
        }

        static Control LastShown(Control c)
        {
            var kids = c.Controls.Cast<Control>().Where(HasVisibleFlag).ToList();
            if (kids.Count == 0) return c;
            return LastShown(kids.OrderBy(k => k.Bottom).Last());
        }

        static string Label(Control c) { return c.GetType().Name + (string.IsNullOrEmpty(c.Text) ? "" : " \"" + (c.Text.Length > 40 ? c.Text.Substring(0, 40) : c.Text) + "\""); }

        static IEnumerable<string> Overlaps(Control parent)
        {
            var kids = parent.Controls.Cast<Control>().Where(HasVisibleFlag).Where(k => !(k is ListBox)).ToList();
            for (int i = 0; i < kids.Count; i++)
                for (int j = i + 1; j < kids.Count; j++)
                {
                    // Docked panels of the shell share edges by design; only an intersection with area counts.
                    var a = kids[i].Bounds; a.Intersect(kids[j].Bounds);
                    if (a.Width > 0 && a.Height > 0)
                        yield return Label(kids[i]) + " " + kids[i].Bounds + " overlaps " + Label(kids[j]) + " " + kids[j].Bounds;
                }
            foreach (var k in kids) foreach (var o in Overlaps(k)) yield return o;
        }

        // A control wider than the room its parent gives it is cut on the right (labels wrap; buttons do not).
        static IEnumerable<string> Overflows(Control root)
        {
            foreach (var c in ShownIn(root, false))
            {
                var parent = c.Parent;
                if (parent == null || parent is Form || parent is ScrollableControl && ((ScrollableControl)parent).AutoScroll) continue;
                int room = parent.ClientSize.Width - parent.Padding.Right;
                if (c.Right > room + 1) yield return Label(c) + " ends at " + c.Right + ", its parent " + Label(parent) + " has " + room;
            }
        }

        static IEnumerable<string> NoInternals(IEnumerable<string> texts) { return PlainWords.Findings(texts); }

        // G-A11Y: every interactive control has a name for screen readers and is reachable with Tab; the character
        // image is decorative.
        static IEnumerable<string> Accessibility(Control root)
        {
            foreach (var c in ShownIn(root, false))
            {
                bool interactive = c is ButtonBase || c is TextBox || c is ListBox || c is ComboBox;
                if (interactive && string.IsNullOrWhiteSpace(c.AccessibleName)) yield return Label(c) + " has no accessible name";
                if (interactive && !c.TabStop && !(c is RadioButton)) yield return Label(c) + " cannot be reached with Tab";
                if (c is PictureBox && c.AccessibleRole != AccessibleRole.None) yield return "the character image is not marked decorative";
            }
        }

        // ---- --smoke-perf (G-PERF) ---------------------------------------------------------------------------------

        public bool ReadOnlyProbe;
    }
}
