// The two controls of the tuning card: the curve chart with its knots, and a stepper for one number out of a short
// list. Both are plain WinForms and hold no driver state: the page gives them values and takes them back, so the
// model and its rules stay in Tuner.cs and the driver stays the authority (docs/design/tuner.md).
//
// Accessibility (G-A11Y): the chart takes the keyboard. Left and right move between the speeds, up and down change
// the selected voltage, Home and End jump to the standard line and to the lowest the driver allows. Every change
// also shows in the table under the chart, so nothing here is the only way to read or set a value.
using System;
using System.Drawing;
using System.Drawing.Drawing2D;
using System.Globalization;
using System.Windows.Forms;

namespace AmdgpuWddmControl
{
    sealed class CurveChart : Control
    {
        uint[] _mv = Tuner.Table(), _line = Tuner.Table(), _floor = Tuner.Floors();
        int _selected;
        bool _drag;
        Tuner.ChangeGate _gate;                     // holds the two events while a drag or a key press runs
        public event EventHandler Changed;          // a knot moved
        public event EventHandler Picked;           // the selection moved

        // The drawn voltage band, a little wider than the admitted one so that a knot at either end is still visible.
        const int LowMv = 800, HighMv = 1010;

        public CurveChart(int width, int height)
        {
            SetStyle(ControlStyles.AllPaintingInWmPaint | ControlStyles.OptimizedDoubleBuffer | ControlStyles.UserPaint | ControlStyles.ResizeRedraw, true);
            Size = new Size(width, height);
            MinimumSize = new Size(width, height);
            MaximumSize = new Size(width, height);
            Margin = Theme.Pad(0, 6, 0, 6);
            BackColor = Theme.Nav;
            TabStop = true;
            AccessibleRole = AccessibleRole.Chart;
            AccessibleName = Strings.T("graphics.tuning.chart");
            Describe();
        }

        public uint[] Values
        {
            get { return (uint[])_mv.Clone(); }
            set { if (value != null && value.Length == Tuner.Points) { _mv = (uint[])value.Clone(); Describe(); Invalidate(); } }
        }

        public void SetReference(uint[] line, uint[] floor)
        {
            if (line != null && line.Length == Tuner.Points) _line = (uint[])line.Clone();
            if (floor != null && floor.Length == Tuner.Points) _floor = (uint[])floor.Clone();
            Describe();
            Invalidate();
        }

        public int Selected
        {
            get { return _selected; }
            set
            {
                int v = Math.Max(0, Math.Min(Tuner.Points - 1, value));
                if (v == _selected) return;
                _selected = v;
                Describe();
                Invalidate();
                Fire(_gate.Mark(false));
            }
        }

        // The page may dispose this control inside its handler, so Fire is always the last thing a handler does.
        void Fire(Tuner.Raise r)
        {
            if (r == Tuner.Raise.Changed) { if (Changed != null) Changed(this, EventArgs.Empty); }
            else if (r == Tuner.Raise.Picked) { if (Picked != null) Picked(this, EventArgs.Empty); }
        }

        void Describe()
        {
            AccessibleDescription = Strings.T("graphics.tuning.point", Tuner.MHzAt(_selected)) + ", " + _mv[_selected] + " mV, "
                + Tuner.DeltaText(_mv[_selected], _line[_selected]);
        }

        Rectangle Plot()
        {
            int left = Theme.S(46), right = Theme.S(10), top = Theme.S(10), bottom = Theme.S(22);
            return new Rectangle(left, top, Math.Max(Theme.S(40), Width - left - right), Math.Max(Theme.S(40), Height - top - bottom));
        }

        Point At(Rectangle r, int i, uint mv)
        {
            int x = r.Left + (Tuner.Points == 1 ? 0 : r.Width * i / (Tuner.Points - 1));
            int y = r.Bottom - (int)((long)(Math.Max(LowMv, Math.Min(HighMv, (int)mv)) - LowMv) * r.Height / (HighMv - LowMv));
            return new Point(x, y);
        }

        uint MvAt(Rectangle r, int y)
        {
            int mv = LowMv + (int)((long)(r.Bottom - Math.Max(r.Top, Math.Min(r.Bottom, y))) * (HighMv - LowMv) / Math.Max(1, r.Height));
            return (uint)Math.Max(LowMv, Math.Min(HighMv, mv));
        }

        int NearestIndex(Rectangle r, int x)
        {
            int best = 0;
            for (int i = 1; i < Tuner.Points; i++)
                if (Math.Abs(At(r, i, _mv[i]).X - x) < Math.Abs(At(r, best, _mv[best]).X - x)) best = i;
            return best;
        }

        void Set(int index, uint mv)
        {
            uint v = Tuner.Nudge(mv, 0, index, _line, _floor);
            if (_mv[index] == v) return;
            _mv[index] = v;
            Describe();
            Invalidate();
            Fire(_gate.Mark(true));
        }

        protected override void OnPaint(PaintEventArgs e)
        {
            var g = e.Graphics;
            g.SmoothingMode = SmoothingMode.AntiAlias;
            g.Clear(BackColor);
            var r = Plot();
            using (var grid = new Pen(Theme.Line))
            using (var text = new SolidBrush(Theme.Dim))
            {
                for (int mv = 820; mv <= 1000; mv += 60)
                {
                    int y = At(r, 0, (uint)mv).Y;
                    g.DrawLine(grid, r.Left, y, r.Right, y);
                    g.DrawString(mv.ToString(CultureInfo.CurrentCulture), Theme.MonoSmall, text, Theme.S(2), y - Theme.S(7));
                }
                g.DrawString("1000", Theme.MonoSmall, text, r.Left, r.Bottom + Theme.S(3));
                var last = "2000 MHz";
                var w = g.MeasureString(last, Theme.MonoSmall);
                g.DrawString(last, Theme.MonoSmall, text, r.Right - w.Width, r.Bottom + Theme.S(3));
            }
            Line(g, r, _line, Theme.Dim, 1, false);
            Line(g, r, _floor, Color.FromArgb(120, Theme.Teal), 1, false);
            Line(g, r, _mv, Theme.Teal, 2, true);
            if (Focused)
                using (var focus = new Pen(Theme.Focus) { DashStyle = DashStyle.Dot })
                    g.DrawRectangle(focus, 0, 0, Width - 1, Height - 1);
        }

        void Line(Graphics g, Rectangle r, uint[] v, Color color, int width, bool knots)
        {
            using (var pen = new Pen(color, Theme.S(width)))
            {
                for (int i = 1; i < Tuner.Points; i++) g.DrawLine(pen, At(r, i - 1, v[i - 1]), At(r, i, v[i]));
            }
            if (!knots) return;
            using (var fill = new SolidBrush(color))
            using (var sel = new SolidBrush(Theme.Accent))
                for (int i = 0; i < Tuner.Points; i++)
                {
                    var p = At(r, i, v[i]);
                    int s = Theme.S(i == _selected ? 5 : 3);
                    g.FillRectangle(i == _selected ? sel : fill, p.X - s, p.Y - s, s * 2, s * 2);
                }
        }

        protected override void OnMouseDown(MouseEventArgs e)
        {
            _gate.Hold();
            _drag = true;
            Focus();
            var r = Plot();
            Selected = NearestIndex(r, e.X);
            Set(_selected, MvAt(r, e.Y));
            base.OnMouseDown(e);
        }

        protected override void OnMouseMove(MouseEventArgs e)
        {
            if (_drag) Set(_selected, MvAt(Plot(), e.Y));
            base.OnMouseMove(e);
        }

        // The drag ends here and the page hears about it once. A lost capture (another window took the mouse) ends
        // the drag the same way, so the gate never stays shut.
        protected override void OnMouseUp(MouseEventArgs e)
        {
            _drag = false;
            base.OnMouseUp(e);
            Fire(_gate.Release());
        }

        protected override void OnMouseCaptureChanged(EventArgs e)
        {
            bool ended = _drag && !Capture;
            _drag = _drag && Capture;
            base.OnMouseCaptureChanged(e);
            if (ended) Fire(_gate.Release());
        }
        protected override void OnGotFocus(EventArgs e) { Invalidate(); base.OnGotFocus(e); }
        protected override void OnLostFocus(EventArgs e) { Invalidate(); base.OnLostFocus(e); }
        protected override bool IsInputKey(Keys key) { return true; }

        protected override void OnKeyDown(KeyEventArgs e)
        {
            int step = e.Control ? 5 : 1;
            _gate.Hold();
            switch (e.KeyCode)
            {
                case Keys.Left: Selected = _selected - 1; break;
                case Keys.Right: Selected = _selected + 1; break;
                case Keys.Down: Set(_selected, (uint)Math.Max(0, (int)_mv[_selected] - step)); break;
                case Keys.Up: Set(_selected, _mv[_selected] + (uint)step); break;
                case Keys.Home: Set(_selected, _line[_selected]); break;
                case Keys.End: Set(_selected, _floor[_selected]); break;
                default: _gate.Release(); base.OnKeyDown(e); return;
            }
            e.Handled = true;
            Fire(_gate.Release());
        }
    }

    // One number out of a short list, as "-  3600 MHz  +". The list is the driver's, so a person cannot step to a
    // value the driver would refuse; the suffix is the unit and never a sentence.
    sealed class ValuePicker : FlowLayoutPanel
    {
        readonly Label _value;
        readonly uint[] _choices;
        readonly string _suffix;
        int _index;
        public event EventHandler Stepped;

        public ValuePicker(uint[] choices, string suffix, string name)
        {
            _choices = choices != null && choices.Length > 0 ? choices : new uint[] { 0 };
            _suffix = suffix ?? "";
            AutoSize = true; WrapContents = false; Margin = Theme.Pad(0); BackColor = Color.Transparent;
            Controls.Add(Step("-", -1, Strings.T("ui.ceiling.lower")));
            _value = new Label
            {
                AutoSize = true, MinimumSize = Theme.Sz(96, 30), Padding = Theme.Pad(6, 0, 6, 0), TextAlign = ContentAlignment.MiddleCenter,
                Font = Theme.Body, ForeColor = Theme.Text, BackColor = Theme.Nav, Margin = Theme.Pad(0, 6, 0, 6), AccessibleName = name,
            };
            Controls.Add(_value);
            Controls.Add(Step("+", 1, Strings.T("ui.ceiling.higher")));
            Index = 0;
        }

        Button Step(string text, int delta, string name)
        {
            var b = Ui.Button(text, (s, e) => { Index += delta; if (Stepped != null) Stepped(this, EventArgs.Empty); });
            b.AutoSize = false; b.Size = Theme.Sz(32, 30); b.Padding = Theme.Pad(0); b.Margin = Theme.Pad(0, 6, 0, 6); b.AccessibleName = name;
            return b;
        }

        public int Index
        {
            get { return _index; }
            set
            {
                _index = Math.Max(0, Math.Min(_choices.Length - 1, value));
                _value.Text = _choices[_index].ToString(CultureInfo.CurrentCulture) + _suffix;
                _value.AccessibleDescription = _value.Text;
            }
        }

        public uint Value
        {
            get { return _choices[_index]; }
            set { var i = Array.IndexOf(_choices, value); if (i >= 0) Index = i; }
        }
    }
}
