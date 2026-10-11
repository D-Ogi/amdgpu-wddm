// The fan curve chart of the Case fan card (docs/design/fan.md Part B). X is the guard temperature from 20 to 95 C,
// Y the fan speed from 0 to 100 %. It draws the two limits the driver keeps whatever the curve says - the 20 % floor
// and full speed from 87 C - the curve the card shows, the curve in force when it differs, and a ring where the fan
// runs now. It holds no driver state: every move goes through FanCurves.Move, so a curve edited here is always one
// the driver accepts, and the page rebuilds from the values it takes back.
//
// Accessibility (G-A11Y): the chart takes the keyboard. Left and right (Home, End) select a point, up and down change
// its speed by 1 % (Page Up and Page Down by 5 %), Ctrl with left and right changes its temperature by 1 C. Each point
// is also an accessible child with its own name and value, and the rows under the chart show every value again.
using System;
using System.Drawing;
using System.Drawing.Drawing2D;
using System.Globalization;
using System.Windows.Forms;

namespace AmdgpuWddmControl
{
    sealed class FanChart : Control
    {
        uint[] _c = { 40, 85 }, _pct = { 50, 100 };
        uint[] _forceC, _forcePct;              // the curve in force, drawn dashed when it differs; null: not drawn
        int _selected;
        double _nowC = -1;                      // the guard temperature now, -1: no reading
        uint _nowPct;
        bool _drag;
        Tuner.ChangeGate _gate;
        public event EventHandler Changed;      // a point moved
        public event EventHandler Picked;       // the selection moved

        static int LowC { get { return (int)FanCurves.MinC; } }
        static int HighC { get { return (int)FanCurves.MaxC; } }

        public FanChart(int width, int height)
        {
            SetStyle(ControlStyles.AllPaintingInWmPaint | ControlStyles.OptimizedDoubleBuffer | ControlStyles.UserPaint | ControlStyles.ResizeRedraw, true);
            Size = new Size(width, height);
            MinimumSize = new Size(width, height);
            MaximumSize = new Size(width, height);
            Margin = Theme.Pad(0, 6, 0, 6);
            BackColor = Theme.Nav;
            TabStop = true;
            AccessibleRole = AccessibleRole.Chart;
            AccessibleName = Strings.T("perf.fan.chart");
            Describe();
        }

        public uint[] C { get { return (uint[])_c.Clone(); } }
        public uint[] Pct { get { return (uint[])_pct.Clone(); } }

        public void SetCurve(uint[] c, uint[] pct)
        {
            if (c == null || pct == null || c.Length != pct.Length || c.Length == 0) return;
            _c = (uint[])c.Clone(); _pct = (uint[])pct.Clone();
            _selected = Math.Min(_selected, _c.Length - 1);
            Describe();
            Invalidate();
        }

        // The curve the driver runs now, drawn dashed under the shown one; nothing when the two are the same.
        public void SetInForce(uint[] c, uint[] pct)
        {
            bool same = c != null && FanCurves.CurveText(c, pct) == FanCurves.CurveText(_c, _pct);
            _forceC = same || c == null ? null : (uint[])c.Clone();
            _forcePct = same || pct == null ? null : (uint[])pct.Clone();
            Invalidate();
        }

        // Where the fan runs now: the guard temperature and the duty the driver applies. A negative temperature hides it.
        public void SetNow(double temperatureC, uint pct)
        {
            _nowC = temperatureC; _nowPct = pct;
            Invalidate();
        }

        public int Selected
        {
            get { return _selected; }
            set
            {
                int v = Math.Max(0, Math.Min(_c.Length - 1, value));
                if (v == _selected) return;
                _selected = v;
                Describe();
                Invalidate();
                NotifyFocus();
                Fire(_gate.Mark(false));
            }
        }

        void Fire(Tuner.Raise r)
        {
            if (r == Tuner.Raise.Changed) { if (Changed != null) Changed(this, EventArgs.Empty); }
            else if (r == Tuner.Raise.Picked) { if (Picked != null) Picked(this, EventArgs.Empty); }
        }

        public static string PointText(int i, uint c, uint pct)
        {
            return Strings.T("perf.fan.point", i + 1) + ": " + Strings.T("perf.fan.point.value", c, pct);
        }

        void Describe()
        {
            AccessibleDescription = PointText(_selected, _c[_selected], _pct[_selected]);
        }

        void NotifyFocus()
        {
            if (IsHandleCreated) AccessibilityNotifyClients(AccessibleEvents.Focus, _selected);
        }

        Rectangle Plot()
        {
            int left = Theme.S(42), right = Theme.S(12), top = Theme.S(10), bottom = Theme.S(22);
            return new Rectangle(left, top, Math.Max(Theme.S(40), Width - left - right), Math.Max(Theme.S(40), Height - top - bottom));
        }

        static int X(Rectangle r, double c)
        {
            return r.Left + (int)Math.Round((Math.Max(LowC, Math.Min(HighC, c)) - LowC) * r.Width / (HighC - LowC));
        }

        static int Y(Rectangle r, double pct)
        {
            return r.Bottom - (int)Math.Round(Math.Max(0, Math.Min(100, pct)) * r.Height / 100.0);
        }

        static int CAt(Rectangle r, int x) { return LowC + (int)Math.Round((x - r.Left) * (double)(HighC - LowC) / Math.Max(1, r.Width)); }
        static int PctAt(Rectangle r, int y) { return (int)Math.Round((r.Bottom - y) * 100.0 / Math.Max(1, r.Height)); }

        Point Knot(Rectangle r, int i) { return new Point(X(r, _c[i]), Y(r, _pct[i])); }

        int Nearest(Rectangle r, Point p)
        {
            int best = 0; double bestD = double.MaxValue;
            for (int i = 0; i < _c.Length; i++)
            {
                var k = Knot(r, i);
                double d = Math.Pow(k.X - p.X, 2) + Math.Pow(k.Y - p.Y, 2);
                if (d < bestD) { bestD = d; best = i; }
            }
            return best;
        }

        void MovePoint(int i, int c, int pct)
        {
            uint oldC = _c[i], oldP = _pct[i];
            FanCurves.Move(_c, _pct, i, c, pct);
            if (_c[i] == oldC && _pct[i] == oldP) return;
            Describe();
            Invalidate();
            if (IsHandleCreated) AccessibilityNotifyClients(AccessibleEvents.ValueChange, i);
            Fire(_gate.Mark(true));
        }

        protected override void OnPaint(PaintEventArgs e)
        {
            var g = e.Graphics;
            g.SmoothingMode = SmoothingMode.AntiAlias;
            g.Clear(BackColor);
            var r = Plot();
            // The two limits first, as shaded zones under everything else.
            using (var floor = new HatchBrush(HatchStyle.BackwardDiagonal, Color.FromArgb(110, Theme.Dim), Color.FromArgb(60, Theme.Line)))
                g.FillRectangle(floor, r.Left, Y(r, FanCurves.FloorPct), r.Width, r.Bottom - Y(r, FanCurves.FloorPct));
            using (var hot = new SolidBrush(Color.FromArgb(45, Theme.Warn)))
                g.FillRectangle(hot, X(r, FanCurves.EmergencyC), r.Top, r.Right - X(r, FanCurves.EmergencyC), r.Height);
            using (var grid = new Pen(Theme.Line))
            using (var text = new SolidBrush(Theme.Dim))
            {
                for (int pct = 0; pct <= 100; pct += 20)
                {
                    int y = Y(r, pct);
                    g.DrawLine(grid, r.Left, y, r.Right, y);
                    g.DrawString(pct.ToString(CultureInfo.CurrentCulture) + "%", Theme.MonoSmall, text, Theme.S(2), y - Theme.S(7));
                }
                for (int c = 20; c <= 90; c += 10)
                {
                    int x = X(r, c);
                    g.DrawLine(grid, x, r.Bottom, x, r.Bottom + Theme.S(3));
                    var label = c.ToString(CultureInfo.CurrentCulture) + (c == 20 ? " °C" : "");
                    float w = g.MeasureString(label, Theme.MonoSmall).Width;
                    float at = c == 20 ? x : x - w / 2;
                    if (at + w <= Width && Math.Abs(c - (int)FanCurves.EmergencyC) > 4) g.DrawString(label, Theme.MonoSmall, text, at, r.Bottom + Theme.S(4));
                }
            }
            using (var edge = new Pen(Theme.Warn, Theme.S(1)) { DashStyle = DashStyle.Dash })
            using (var warn = new SolidBrush(Theme.Warn))
            {
                g.DrawLine(edge, X(r, FanCurves.EmergencyC), r.Top, X(r, FanCurves.EmergencyC), r.Bottom);
                g.DrawLine(edge, r.Left, Y(r, FanCurves.FloorPct), r.Right, Y(r, FanCurves.FloorPct));
                var hot = FanCurves.EmergencyC.ToString(CultureInfo.CurrentCulture);
                float w = g.MeasureString(hot, Theme.MonoSmall).Width;
                g.DrawString(hot, Theme.MonoSmall, warn, X(r, FanCurves.EmergencyC) - w / 2, r.Bottom + Theme.S(4));
            }
            if (_forceC != null) Curve(g, r, _forceC, _forcePct, Theme.Dim, true);
            Curve(g, r, _c, _pct, Theme.Teal, false);
            using (var fill = new SolidBrush(Theme.Teal))
            using (var sel = new SolidBrush(Theme.Text))
                for (int i = 0; i < _c.Length; i++)
                {
                    var p = Knot(r, i);
                    int s = Theme.S(i == _selected ? 6 : 4);
                    g.FillEllipse(i == _selected ? sel : fill, p.X - s, p.Y - s, s * 2, s * 2);
                }
            if (_nowC >= 0)
                using (var ring = new Pen(Theme.Good, Theme.S(2)))
                {
                    int x = X(r, _nowC), y = Y(r, _nowPct), s = Theme.S(7);
                    g.DrawEllipse(ring, x - s, y - s, s * 2, s * 2);
                }
            if (Focused)
                using (var focus = new Pen(Theme.Focus) { DashStyle = DashStyle.Dot })
                    g.DrawRectangle(focus, 0, 0, Width - 1, Height - 1);
        }

        // A curve as the driver reads it: flat before the first point and after the last, straight lines between.
        static void Curve(Graphics g, Rectangle r, uint[] c, uint[] pct, Color color, bool dashed)
        {
            if (c == null || pct == null || c.Length == 0) return;
            using (var pen = new Pen(color, Theme.S(dashed ? 1 : 2)) { DashStyle = dashed ? DashStyle.Dash : DashStyle.Solid })
            {
                g.DrawLine(pen, r.Left, Y(r, pct[0]), X(r, c[0]), Y(r, pct[0]));
                for (int i = 1; i < c.Length; i++) g.DrawLine(pen, X(r, c[i - 1]), Y(r, pct[i - 1]), X(r, c[i]), Y(r, pct[i]));
                g.DrawLine(pen, X(r, c[c.Length - 1]), Y(r, pct[c.Length - 1]), r.Right, Y(r, pct[c.Length - 1]));
            }
        }

        protected override void OnMouseDown(MouseEventArgs e)
        {
            _gate.Hold();
            _drag = true;
            Focus();
            var r = Plot();
            Selected = Nearest(r, e.Location);
            base.OnMouseDown(e);
            // A press only picks the point; the drag moves it. The page hears about it on release.
        }

        protected override void OnMouseMove(MouseEventArgs e)
        {
            if (_drag) { var r = Plot(); MovePoint(_selected, CAt(r, e.X), PctAt(r, e.Y)); }
            Cursor = _drag || Hit(e.Location) ? Cursors.Hand : Cursors.Default;
            base.OnMouseMove(e);
        }

        bool Hit(Point p)
        {
            var r = Plot();
            var k = Knot(r, Nearest(r, p));
            return Math.Abs(k.X - p.X) <= Theme.S(10) && Math.Abs(k.Y - p.Y) <= Theme.S(10);
        }

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

        protected override void OnGotFocus(EventArgs e) { Invalidate(); base.OnGotFocus(e); NotifyFocus(); }
        protected override void OnLostFocus(EventArgs e) { Invalidate(); base.OnLostFocus(e); }

        protected override bool IsInputKey(Keys key)
        {
            switch (key & Keys.KeyCode)
            {
                case Keys.Left: case Keys.Right: case Keys.Up: case Keys.Down: case Keys.Home: case Keys.End: case Keys.PageUp: case Keys.PageDown: return true;
                default: return base.IsInputKey(key);
            }
        }

        protected override void OnKeyDown(KeyEventArgs e)
        {
            _gate.Hold();
            int i = _selected;
            switch (e.KeyCode)
            {
                case Keys.Left: if (e.Control) MovePoint(i, (int)_c[i] - 1, (int)_pct[i]); else Selected = i - 1; break;
                case Keys.Right: if (e.Control) MovePoint(i, (int)_c[i] + 1, (int)_pct[i]); else Selected = i + 1; break;
                case Keys.Up: MovePoint(i, (int)_c[i], (int)_pct[i] + 1); break;
                case Keys.Down: MovePoint(i, (int)_c[i], (int)_pct[i] - 1); break;
                case Keys.PageUp: MovePoint(i, (int)_c[i], (int)_pct[i] + 5); break;
                case Keys.PageDown: MovePoint(i, (int)_c[i], (int)_pct[i] - 5); break;
                case Keys.Home: Selected = 0; break;
                case Keys.End: Selected = _c.Length - 1; break;
                default: _gate.Release(); base.OnKeyDown(e); return;
            }
            e.Handled = true;
            Fire(_gate.Release());
        }

        // Each point is an accessible child: "Point 2: 60 °C 70 %", with its place on the screen, so a screen reader
        // can list the points and follow the selected one.
        protected override AccessibleObject CreateAccessibilityInstance() { return new ChartAccessible(this); }

        sealed class ChartAccessible : ControlAccessibleObject
        {
            readonly FanChart _chart;
            public ChartAccessible(FanChart chart) : base(chart) { _chart = chart; }
            public override int GetChildCount() { return _chart._c.Length; }
            public override AccessibleObject GetChild(int index) { return index >= 0 && index < _chart._c.Length ? new PointAccessible(_chart, index) : null; }
            public override AccessibleObject GetFocused() { return _chart.Focused ? GetChild(_chart._selected) : base.GetFocused(); }
            public override AccessibleObject GetSelected() { return GetChild(_chart._selected); }
        }

        sealed class PointAccessible : AccessibleObject
        {
            readonly FanChart _chart;
            readonly int _i;
            public PointAccessible(FanChart chart, int i) { _chart = chart; _i = i; }
            public override string Name { get { return Strings.T("perf.fan.point", _i + 1); } }
            public override string Value { get { return Strings.T("perf.fan.point.value", _chart._c[_i], _chart._pct[_i]); } set { } }
            public override AccessibleRole Role { get { return AccessibleRole.Graphic; } }
            public override AccessibleObject Parent { get { return _chart.AccessibilityObject; } }
            public override AccessibleStates State
            {
                get
                {
                    var s = AccessibleStates.Selectable | AccessibleStates.Focusable;
                    if (_i == _chart._selected) s |= AccessibleStates.Selected | (_chart.Focused ? AccessibleStates.Focused : 0);
                    return s;
                }
            }
            public override Rectangle Bounds
            {
                get
                {
                    var k = _chart.Knot(_chart.Plot(), _i);
                    int s = Theme.S(6);
                    return _chart.RectangleToScreen(new Rectangle(k.X - s, k.Y - s, s * 2, s * 2));
                }
            }
            public override void Select(AccessibleSelection flags)
            {
                if ((flags & (AccessibleSelection.TakeSelection | AccessibleSelection.TakeFocus)) != 0) { _chart.Focus(); _chart.Selected = _i; }
            }
        }
    }
}
