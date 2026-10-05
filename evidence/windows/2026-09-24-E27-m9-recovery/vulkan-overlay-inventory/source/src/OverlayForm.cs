// The window on the BC-250's own screen. Passive mode: always on top, translucent, click-through, never takes
// focus. Interactive mode (Ctrl+Alt+F9): the same window accepts the mouse and shows one button per action.
// Global hotkeys work in both modes and while hidden:
//   Ctrl+Alt+F9 controls, Ctrl+Alt+F10 hide/show, Ctrl+Alt+F12 STOP.
// Function keys, not letters: on the Polish layout (and every other AltGr layout) AltGr is Ctrl+Alt, so a
// global Ctrl+Alt+<letter> swallows the letter the owner is trying to type - Ctrl+Alt+S ate "s with acute".
using System;
using System.Drawing;
using System.Linq;
using System.Runtime.InteropServices;
using System.Windows.Forms;

namespace Bc250Mon
{
    public sealed class OverlayForm : Form
    {
        const int WS_EX_TRANSPARENT = 0x20, WS_EX_TOOLWINDOW = 0x80, WS_EX_LAYERED = 0x80000, WS_EX_NOACTIVATE = 0x08000000;
        const int GWL_EXSTYLE = -20, WM_HOTKEY = 0x0312, MOD_ALT = 1, MOD_CONTROL = 2;
        const int HotControls = 1, HotHide = 2, HotStop = 3;
        const int PanelWidth = 460, Margin_ = 14, LogLines = 9;

        [DllImport("user32.dll")] static extern int GetWindowLong(IntPtr h, int index);
        [DllImport("user32.dll")] static extern int SetWindowLong(IntPtr h, int index, int value);
        [DllImport("user32.dll")] static extern bool RegisterHotKey(IntPtr h, int id, int modifiers, int vk);
        [DllImport("user32.dll")] static extern uint GetDpiForWindow(IntPtr h);

        readonly State _state;
        readonly Actions _actions;
        readonly FlowLayoutPanel _buttons = new FlowLayoutPanel();
        readonly Timer _timer = new Timer { Interval = 500 };
        // The layout below is written in 96-dpi pixels and painted through one scale transform, fonts included
        // (hence pixel units). Point-sized fonts on a fixed pixel grid fell apart when Windows chose 200 %.
        readonly Font _title = new Font("Segoe UI Semibold", 14.7f, GraphicsUnit.Pixel), _text = new Font("Consolas", 13.3f, GraphicsUnit.Pixel),
                      _small = new Font("Consolas", 11.3f, GraphicsUnit.Pixel), _button = new Font("Consolas", 10f);
        float _scale = 1f;
        volatile bool _dirty = true;
        bool _interactive;
        DateTime _lastPaint;

        public OverlayForm(State state, Actions actions)
        {
            _state = state; _actions = actions;
            FormBorderStyle = FormBorderStyle.None;
            ShowInTaskbar = false; TopMost = true; DoubleBuffered = true;
            StartPosition = FormStartPosition.Manual;
            BackColor = Color.FromArgb(16, 18, 22);
            Opacity = 0.88;
            Width = PanelWidth; Height = 300;

            _buttons.Dock = DockStyle.Bottom; _buttons.AutoSize = true; _buttons.Visible = false;
            _buttons.Padding = new Padding(8); _buttons.BackColor = Color.FromArgb(30, 34, 40);
            foreach (var a in actions.All.Where(x => x.ShowButton))
            {
                var info = a;
                var b = new Button { Text = a.Label, AutoSize = true, FlatStyle = FlatStyle.Flat, ForeColor = Color.White, Font = _button,
                                     BackColor = a.Name == "stop.set" ? Color.FromArgb(150, 30, 30) : Color.FromArgb(50, 56, 66), Margin = new Padding(4) };
                b.Click += (s, e) => { string err; _actions.Invoke(info.Name, null, "overlay button", out err); };
                _buttons.Controls.Add(b);
            }
            Controls.Add(_buttons);

            state.Changed += () => _dirty = true;
            actions.UiRequest += what => BeginInvoke((Action)(() => OnUiRequest(what)));
            _timer.Tick += (s, e) => { FollowDpi(); if (_dirty || (DateTime.Now - _lastPaint).TotalSeconds >= 1) { _dirty = false; Invalidate(); } };
            _timer.Start();
        }

        protected override bool ShowWithoutActivation { get { return true; } }

        protected override CreateParams CreateParams
        {
            get
            {
                var cp = base.CreateParams;
                cp.ExStyle |= WS_EX_TOOLWINDOW | WS_EX_LAYERED | WS_EX_NOACTIVATE | WS_EX_TRANSPARENT;
                return cp;
            }
        }

        protected override void OnHandleCreated(EventArgs e)
        {
            base.OnHandleCreated(e);
            Hotkey(HotControls, Keys.F9, "controls");
            Hotkey(HotHide, Keys.F10, "hide/show");
            Hotkey(HotStop, Keys.F12, "STOP");
            Screenshot.OverlayWindow = Handle;      // lets a capture with overlay=0 leave this window out
            FollowDpi();
            Dock_();
        }

        // A hotkey another process already owns is refused silently by Windows, and the owner would then press
        // it and see nothing happen. Say so in the log instead.
        void Hotkey(int id, Keys key, string what)
        {
            if (!RegisterHotKey(Handle, id, MOD_CONTROL | MOD_ALT, (int)key))
                _state.Log("ui", Level.Warn, "hotkey Ctrl+Alt+" + key + " (" + what + ") is taken, use the buttons or the API");
        }

        // The scaling can change under us (Windows picks another one when the display driver changes), and this
        // process is per-monitor aware, so nobody rescales the window for us.
        void FollowDpi()
        {
            if (!IsHandleCreated) return;
            float scale = Math.Max(96u, GetDpiForWindow(Handle)) / 96f;
            if (Math.Abs(scale - _scale) < 0.01f) return;
            _scale = scale;
            Width = (int)(PanelWidth * _scale);
            Dock_();
            _dirty = true;
        }

        void Dock_()
        {
            var area = Screen.PrimaryScreen.WorkingArea;
            Location = new Point(area.Right - Width - 12, area.Top + 12);
        }

        protected override void WndProc(ref Message m)
        {
            if (m.Msg == WM_HOTKEY)
            {
                string err;
                int id = m.WParam.ToInt32();
                if (id == HotControls) OnUiRequest(_interactive ? "passive" : "interactive");
                else if (id == HotHide) OnUiRequest(Visible ? "hide" : "show");
                else if (id == HotStop) _actions.Invoke("stop.set", null, "hotkey", out err);
            }
            base.WndProc(ref m);
        }

        void OnUiRequest(string what)
        {
            if (what == "hide") { Hide(); return; }
            if (what == "show") { Show(); _dirty = true; return; }
            _interactive = what == "interactive";
            if (_interactive && !Visible) Show();
            int ex = GetWindowLong(Handle, GWL_EXSTYLE);
            ex = _interactive ? ex & ~(WS_EX_TRANSPARENT | WS_EX_NOACTIVATE) : ex | WS_EX_TRANSPARENT | WS_EX_NOACTIVATE;
            SetWindowLong(Handle, GWL_EXSTYLE, ex);
            _buttons.Visible = _interactive;
            _dirty = true;
        }

        static Color ColorOf(Level l)
        {
            switch (l)
            {
                case Level.Good: return Color.FromArgb(120, 220, 140);
                case Level.Warn: return Color.FromArgb(250, 200, 90);
                case Level.Error: return Color.FromArgb(255, 110, 100);
                default: return Color.FromArgb(225, 228, 232);
            }
        }

        protected override void OnPaint(PaintEventArgs e)
        {
            _lastPaint = DateTime.Now;
            var g = e.Graphics;
            g.TextRenderingHint = System.Drawing.Text.TextRenderingHint.ClearTypeGridFit;
            var snap = _state.Take(LogLines);
            g.ScaleTransform(_scale, _scale);
            int x = Margin_, y = 10, w = PanelWidth - 2 * Margin_;
            var dim = Color.FromArgb(140, 148, 160);

            using (var b = new SolidBrush(Color.FromArgb(150, 190, 255)))
                g.DrawString("BC-250 lab  " + Environment.MachineName, _title, b, x, y);
            using (var b = new SolidBrush(dim))
            {
                string clock = DateTime.Now.ToString("HH:mm:ss");
                g.DrawString(clock, _text, b, x + w - g.MeasureString(clock, _text).Width, y + 2);
            }
            y += 26;

            if (snap.Stop)
            {
                g.FillRectangle(Brushes.Firebrick, x, y, w, 24);
                g.DrawString("STOP REQUESTED - tests must end", _title, Brushes.White, x + 6, y + 1);
                y += 30;
            }

            using (var b = new SolidBrush(ColorOf(snap.StatusLevel)))
            {
                var size = g.MeasureString(snap.Status, _text, w);
                g.DrawString(snap.Status, _text, b, new RectangleF(x, y, w, size.Height));
                y += (int)size.Height + 8;
            }

            var panels = snap.Panels.Where(p => p.Name != "kmdinfo" ||
                !snap.Panels.Any(item => item.Name == "graphics")).ToList();
            var heights = panels.Select(p => OverlayLayout.PanelHeight(g, _text, p, w - 130)).ToList();
            // Logs and the existing controls hint form the final block.
            heights.Add(19 + snap.Log.Count * 15 + 6 + 22);
            var area = Screen.PrimaryScreen.WorkingArea;
            int availableHeight = (int)((area.Height - 24 - (_buttons.Visible ? _buttons.Height : 0)) / _scale);
            int maxColumns = Math.Max(1, (int)((area.Width - 24) / (PanelWidth * _scale)));
            int columns, bottom;
            var positions = OverlayLayout.Flow(heights, y, availableHeight, maxColumns, out columns, out bottom);
            for (int i = 0; i < panels.Count; ++i)
            {
                var p = panels[i];
                x = Margin_ + positions[i].X * PanelWidth;
                y = positions[i].Y;
                using (var b = new SolidBrush(dim)) g.DrawString(p.Title.ToUpperInvariant(), _small, b, x, y);
                using (var pen = new Pen(Color.FromArgb(60, 66, 76))) g.DrawLine(pen, x, y + 15, x + w, y + 15);
                y += 19;
                foreach (var r in p.Rows)
                {
                    using (var b = new SolidBrush(dim)) g.DrawString(r.Label, _text, b, x, y);
                    using (var b = new SolidBrush(ColorOf(r.Level)))
                    {
                        var rect = new RectangleF(x + 130, y, w - 130, 400);
                        var size = g.MeasureString(r.Value, _text, w - 130);
                        g.DrawString(r.Value, _text, b, rect);
                        y += Math.Max(18, (int)size.Height);
                    }
                }
                y += 8;
            }

            x = Margin_ + positions[panels.Count].X * PanelWidth;
            y = positions[panels.Count].Y;
            using (var b = new SolidBrush(dim)) g.DrawString("LOG", _small, b, x, y);
            using (var pen = new Pen(Color.FromArgb(60, 66, 76))) g.DrawLine(pen, x, y + 15, x + w, y + 15);
            y += 19;
            foreach (var l in snap.Log)
            {
                string line = l.Time.ToString("HH:mm:ss") + " " + l.Text;
                if (line.Length > 62) line = line.Substring(0, 61) + "~";
                using (var b = new SolidBrush(ColorOf(l.Level))) g.DrawString(line, _small, b, x, y);
                y += 15;
            }
            y += 6;
            using (var b = new SolidBrush(Color.FromArgb(110, 118, 130)))
                g.DrawString("Ctrl+Alt+F9 controls   Ctrl+Alt+F10 hide   Ctrl+Alt+F12 STOP", _small, b, x, y);
            y += 22;

            int wanted = (int)(bottom * _scale) + (_buttons.Visible ? _buttons.Height : 0);
            int wantedWidth = (int)(columns * PanelWidth * _scale);
            if (Math.Abs(wanted - Height) > 2 || Width != wantedWidth)
                BeginInvoke((Action)(() => { Width = wantedWidth; Height = wanted; Dock_(); }));
        }
    }
}
