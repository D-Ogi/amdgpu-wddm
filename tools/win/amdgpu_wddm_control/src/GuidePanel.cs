// The guide panel (section 6): a fixed reserved area in the side panel, never over settings, warnings or content.
// Its text always shows; Nagi's image only with "Show Nagi" checked and the art embedded in the exe (build.ps1
// -NagiArt; the owner's art is never committed). Without art it is the text-only panel. A frame sequence plays once
// when the verdict changes, only when Guide.Present allows it, on a timer that exists only while it plays. The
// image is decorative: screen readers get the text, not the picture.
using System;
using System.Collections.Generic;
using System.Drawing;
using System.IO;
using System.Linq;
using System.Reflection;
using System.Windows.Forms;

namespace AmdgpuWddmControl
{
    // The embedded art: nagi.<expression>@<size>.png, and frames nagi.<expression>-fNN@<size>.png when present.
    public static class NagiArt
    {
        static readonly Dictionary<string, Image> Cache = new Dictionary<string, Image>();
        public static int Loaded { get { lock (Cache) return Cache.Count; } }

        public static bool Has(string expression, int size)
        {
            return Assembly.GetExecutingAssembly().GetManifestResourceInfo(Guide.Resource(expression, size)) != null;
        }

        public static Image Load(string resource)
        {
            lock (Cache)
            {
                Image img;
                if (Cache.TryGetValue(resource, out img)) return img;
                try
                {
                    using (var s = Assembly.GetExecutingAssembly().GetManifestResourceStream(resource))
                    {
                        if (s == null || s.Length > 4 << 20) return null;
                        var ms = new MemoryStream();
                        s.CopyTo(ms);
                        img = Image.FromStream(ms);
                    }
                }
                catch (Exception) { img = null; }
                if (img != null) Cache[resource] = img;
                return img;
            }
        }

        public static List<Image> Frames(string expression, int size)
        {
            var present = new List<bool>();
            for (int i = 1; i <= Guide.MaxFrames + 1; i++)
            {
                bool has = Assembly.GetExecutingAssembly().GetManifestResourceInfo(Guide.FrameResource(expression, i, size)) != null;
                if (!has) break;
                present.Add(true);
            }
            int n = Guide.UsableFrames(present);
            var frames = new List<Image>();
            for (int i = 1; i <= n; i++)
            {
                var img = Load(Guide.FrameResource(expression, i, size));
                if (img == null) return new List<Image>();
                frames.Add(img);
            }
            return frames;
        }
    }

    sealed class GuidePanel : Panel
    {
        readonly AppPrefs _prefs;
        readonly Label _title, _text;
        readonly PictureBox _art;
        readonly int _width;
        Timer _frames;
        List<Image> _sequence;
        int _frame;
        string _shownExpression;
        public bool Paused;

        public GuidePanel(int width, AppPrefs prefs)
        {
            _prefs = prefs; _width = width;
            BackColor = Theme.GuideBack; AutoSize = true; AutoSizeMode = AutoSizeMode.GrowAndShrink; Padding = Theme.Pad(14, 12, 14, 14);
            MinimumSize = new Size(width, 0); MaximumSize = new Size(width, 0); Margin = Theme.Pad(0);
            var stack = Ui.Stack(width - Padding.Horizontal);
            stack.Dock = DockStyle.Fill;
            _title = Ui.Label("", Theme.CardTitle, Theme.Teal, width - Padding.Horizontal);
            stack.Controls.Add(_title);
            _art = new PictureBox
            {
                Size = Theme.Sz(128, 128), SizeMode = PictureBoxSizeMode.Zoom, Visible = false, BackColor = Theme.GuideBack, Margin = Theme.Pad(0, 6, 0, 6),
                AccessibleRole = AccessibleRole.None, AccessibleName = "", TabStop = false,
            };
            stack.Controls.Add(_art);
            _text = Ui.Label("", null, null, width - Padding.Horizontal);
            stack.Controls.Add(_text);
            Controls.Add(stack);
            AccessibleName = Strings.T("guide.title.plain");
        }

        public bool ArtShown { get { return _art.Visible; } }
        public string Text2 { get { return _text.Text; } }
        public bool Playing { get { return _frames != null; } }

        // v null: no verdict (nothing read): the text-only panel.
        public void Show(GuideVerdict v, string text, bool windowVisible)
        {
            int size = Guide.ArtSize(Theme.Scale);
            bool changed = v == null || _shownExpression != v.Expression;
            var frames = v != null && changed && _prefs.ShowNagi ? NagiArt.Frames(v.Expression, size) : new List<Image>();
            var d = Guide.Present(v, new GuideContext
            {
                ShowNagi = _prefs.ShowNagi, ReduceAnimations = _prefs.ReduceAnimations, ShowTipsAutomatically = _prefs.ShowTipsAutomatically,
                WindowsAnimations = AppPrefs.WindowsAnimations(), WindowVisible = windowVisible && !Paused,
                ArtAvailable = v != null && _prefs.ShowNagi && NagiArt.Has(v.Expression, size),
                FramesFound = frames.Count, FramesComplete = true, Trigger = changed ? GuideTrigger.VerdictChanged : GuideTrigger.None,
            });
            bool show = d.Figure != null;
            _title.Text = Strings.T(show ? "guide.title" : "guide.title.plain");
            _text.Text = text;
            _art.Visible = show;
            if (!show) { Stop(); _art.Image = null; _shownExpression = null; return; }
            if (!changed) return;
            _shownExpression = d.Figure;
            Stop();
            _art.Image = NagiArt.Load(Guide.Resource(d.Figure, size));
            if (_art.Image == null) { _art.Visible = false; return; }
            if (d.Animate)
            {
                _sequence = frames; _frame = 0;
                _frames = new Timer { Interval = Guide.FrameMs };
                _frames.Tick += (s, e) =>
                {
                    if (Paused || _frame >= _sequence.Count) { Stop(); _art.Image = NagiArt.Load(Guide.Resource(_shownExpression, size)); return; }
                    _art.Image = _sequence[_frame++];
                };
                _frames.Start();
            }
        }

        public void Stop()
        {
            if (_frames == null) return;
            _frames.Stop(); _frames.Dispose(); _frames = null; _sequence = null;
        }

        protected override void Dispose(bool disposing) { if (disposing) Stop(); base.Dispose(disposing); }
    }
}
