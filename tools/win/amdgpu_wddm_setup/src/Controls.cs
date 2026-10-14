// The widgets of the setup window, drawn here with GDI+ so that they follow the design tokens of Theme.cs on every
// PC: a flat button with its states, the option controls, the card, the rows with an icon, the status line, the
// progress strip, the step rail row and the release notes panel.
//
// Every control a user can reach is a WinForms button, option control or text box, or carries IFocusTarget: the
// render gate (SetupForm.Accessibility) walks exactly those, so an owner-drawn control cannot escape the check for a
// name, a tab position, a default action and a place in the tab order that is not the title bar. Each of them draws
// its own focus ring, which no gate can see: a headless render draws no focused control. A control that only shows
// text is a panel with its text as its accessible name, never a bare panel after a label (Windows' window proxy
// would read that label twice).
using System;
using System.Drawing;
using System.Windows.Forms;

namespace AmdgpuWddmSetup
{
    // An owner-drawn control that the keyboard reaches and that is not one of the system's controls. No control of
    // this window carries it: every control a user can reach is a button, an option control or a text box, and the
    // contract says so. It stays as the one way in: a control that takes the keyboard itself must carry this, and
    // the render gate (SetupForm.Accessibility) then walks it with the buttons for its name, its tab position and
    // its default action.
    public interface IFocusTarget
    {
    }

    // A container that paints a colour of its own over its BackColor (the card, the field, the notes panel, the
    // guide's balloon). Its children paint their background in that colour, not in the BackColor of the window
    // behind it; Theme.Backdrop asks for it on the way up the parent chain (BD-137). A container that paints a
    // colour of its own and does not carry this hands its children the colour of the container behind it, which is
    // the BD-137 picture again with the colours swapped, so the colour half of G-BACKDROP reads this interface and
    // fails a child whose backdrop is not what the window really paints under it.
    public interface IBackdrop
    {
        Color Backdrop { get; }
    }

    public enum ButtonKind { Primary, Secondary, Quiet, Pill, Chrome }

    // One button in every state the mockup shows: a red gradient with a chevron for the primary action, a dark
    // outline for the others, a quiet one for a link, a pill for the language choice and a glyph for the title bar.
    public sealed class FlatButton : Button
    {
        public ButtonKind Kind = ButtonKind.Secondary;
        public string Glyph;            // a leading icon glyph, or the whole face of a chrome button
        public bool Chevron;            // the chevron of the primary action
        public bool Selected;           // a pill that shows the current choice
        bool _hot, _down;

        public FlatButton()
        {
            SetStyle(ControlStyles.UserPaint | ControlStyles.AllPaintingInWmPaint | ControlStyles.OptimizedDoubleBuffer | ControlStyles.ResizeRedraw | ControlStyles.SupportsTransparentBackColor, true);
            FlatStyle = FlatStyle.Flat;
            FlatAppearance.BorderSize = 0;
            BackColor = Color.Transparent;
            UseMnemonic = false;
            AutoSize = true;
            AutoSizeMode = AutoSizeMode.GrowAndShrink;
            Cursor = Cursors.Hand;
            AccessibleRole = AccessibleRole.PushButton;
        }

        protected override void OnMouseEnter(EventArgs e) { _hot = true; Invalidate(); base.OnMouseEnter(e); }
        protected override void OnMouseLeave(EventArgs e) { _hot = _down = false; Invalidate(); base.OnMouseLeave(e); }
        protected override void OnMouseDown(MouseEventArgs e) { _down = true; Invalidate(); base.OnMouseDown(e); }
        protected override void OnMouseUp(MouseEventArgs e) { _down = false; Invalidate(); base.OnMouseUp(e); }
        protected override void OnGotFocus(EventArgs e) { Invalidate(); base.OnGotFocus(e); }
        protected override void OnLostFocus(EventArgs e) { Invalidate(); base.OnLostFocus(e); }
        protected override void OnEnabledChanged(EventArgs e) { Invalidate(); base.OnEnabledChanged(e); }

        int IconSize { get { return Theme.S(Kind == ButtonKind.Chrome ? 13 : 15); } }
        int Radius { get { return Kind == ButtonKind.Pill ? Theme.S(13) : Kind == ButtonKind.Chrome ? Theme.S(Theme.RadiusSmall) : Theme.S(Theme.RadiusControl); } }

        public override Size GetPreferredSize(Size proposed)
        {
            if (Kind == ButtonKind.Chrome) return new Size(Theme.S(44), Theme.S(32));
            var text = Gfx.Measure(Text, Font, int.MaxValue / 2);
            int w = text.Width, h = Math.Max(text.Height, IconSize);
            if (Glyph != null) w += IconSize + Theme.S(8);
            if (Chevron) w += IconSize + Theme.S(6);
            var pad = Kind == ButtonKind.Primary ? Theme.S(20) : Kind == ButtonKind.Pill ? Theme.S(12) : Kind == ButtonKind.Quiet ? Theme.S(4) : Theme.S(16);
            var tall = Kind == ButtonKind.Pill ? Theme.S(8) : Theme.S(11);
            return new Size(w + pad * 2, h + tall * 2);
        }

        protected override void OnPaint(PaintEventArgs e)
        {
            var g = e.Graphics;
            var r = new Rectangle(0, 0, Width, Height);
            using (var back = new SolidBrush(Theme.Backdrop(this))) g.FillRectangle(back, r);
            Color face = Theme.Text, line = Color.Empty;
            bool on = Enabled;
            switch (Kind)
            {
                case ButtonKind.Primary:
                    if (!on) Gfx.Fill(g, r, Radius, Theme.CardHi);
                    else Gfx.FillGradient(g, r, Radius, _down ? Theme.AccentLow : _hot ? Theme.AccentHi : Theme.Accent, _down ? Theme.AccentLow : Theme.Accent);
                    face = on ? Color.White : Theme.Dim;
                    if (Theme.HighContrast) { Gfx.Fill(g, r, Radius, on ? Theme.Accent : Theme.CardHi); face = SystemColors.HighlightText; line = Theme.Line; }
                    break;
                case ButtonKind.Secondary:
                    Gfx.Fill(g, r, Radius, _down ? Theme.Card : _hot ? Theme.CardHi : Theme.Card);
                    line = _hot ? Theme.Dim : Theme.Line;
                    face = on ? Theme.Text : Theme.Dim;
                    break;
                case ButtonKind.Quiet:
                    if (_hot || _down) Gfx.Fill(g, r, Radius, Theme.CardHi);
                    face = on ? Theme.Teal : Theme.Dim;
                    break;
                case ButtonKind.Pill:
                    // The mockup marks the current language with the accent underline alone; the filled pill belongs
                    // to the mouse, so that the current choice does not read as a hovered button.
                    if (_down || _hot) Gfx.Fill(g, r, Radius, _down ? Theme.Card : Theme.CardHi);
                    face = Selected ? Theme.Text : Theme.Dim;
                    break;
                case ButtonKind.Chrome:
                    if (_down || _hot) Gfx.Fill(g, r, Radius, _down ? Theme.Card : Theme.CardHi);
                    face = _hot && Glyph == Icons.Close ? Color.White : Theme.Text;
                    if (_hot && Glyph == Icons.Close) Gfx.Fill(g, r, Radius, Theme.Accent);
                    break;
            }
            if (line != Color.Empty) Gfx.Border(g, r, Radius, line);
            if (Kind == ButtonKind.Chrome)
            {
                Gfx.Glyph(g, Glyph, r, face, IconSize);
                if (Focused) Gfx.FocusRing(g, r, Radius);
                return;
            }

            var text = Gfx.Measure(Text, Font, int.MaxValue / 2);
            int iconRoom = Glyph != null ? IconSize + Theme.S(8) : 0;
            int chevronRoom = Chevron ? IconSize + Theme.S(6) : 0;
            int blockWidth = iconRoom + text.Width + chevronRoom;
            int x = (Width - blockWidth) / 2;
            if (Kind == ButtonKind.Quiet) x = Theme.S(4);
            if (Glyph != null)
            {
                Gfx.Glyph(g, Glyph, new Rectangle(x, (Height - IconSize) / 2, IconSize, IconSize), face, IconSize);
                x += iconRoom;
            }
            TextRenderer.DrawText(g, Text, Font, new Rectangle(x, (Height - text.Height) / 2, text.Width, text.Height), face,
                TextFormatFlags.NoPrefix | TextFormatFlags.SingleLine | TextFormatFlags.Left | TextFormatFlags.NoPadding);
            x += text.Width;
            if (Chevron) Gfx.Glyph(g, Icons.Chevron, new Rectangle(x + Theme.S(6), (Height - IconSize) / 2, IconSize, IconSize), face, (int)(IconSize * 0.8f));
            // A pill shows the chosen language with the accent underline of the mockup. The underline clears the
            // descenders of the text itself, so a Korean or Japanese glyph does not sit on it.
            if (Kind == ButtonKind.Pill && Selected)
            {
                int under = Math.Min(Height - Math.Max(1, Theme.S(2)), (Height + text.Height) / 2 + Theme.S(2));
                using (var b = new SolidBrush(Theme.Accent))
                    g.FillRectangle(b, new Rectangle(Theme.S(10), under, Math.Max(1, Width - Theme.S(20)), Math.Max(1, Theme.S(2))));
            }
            if (Focused) Gfx.FocusRing(g, r, Radius);
        }
    }

    // The option controls, drawn here: a check box and a radio button that follow the palette and show hover, focus
    // and the accent of the mockup. They stay CheckBox and RadioButton, so their behaviour and their accessible role
    // are the system's.
    public sealed class ThemeCheck : CheckBox
    {
        bool _hot;

        public ThemeCheck()
        {
            SetStyle(ControlStyles.UserPaint | ControlStyles.AllPaintingInWmPaint | ControlStyles.OptimizedDoubleBuffer | ControlStyles.ResizeRedraw | ControlStyles.SupportsTransparentBackColor, true);
            BackColor = Color.Transparent;
            UseMnemonic = false;
            AutoSize = true;
            Cursor = Cursors.Hand;
        }

        protected override void OnMouseEnter(EventArgs e) { _hot = true; Invalidate(); base.OnMouseEnter(e); }
        protected override void OnMouseLeave(EventArgs e) { _hot = false; Invalidate(); base.OnMouseLeave(e); }
        protected override void OnGotFocus(EventArgs e) { Invalidate(); base.OnGotFocus(e); }
        protected override void OnLostFocus(EventArgs e) { Invalidate(); base.OnLostFocus(e); }
        protected override void OnCheckedChanged(EventArgs e) { Invalidate(); base.OnCheckedChanged(e); }

        int Box { get { return Theme.S(18); } }
        int Room { get { return Box + Theme.S(10); } }

        public override Size GetPreferredSize(Size proposed)
        {
            int width = (MaximumSize.Width > 0 ? MaximumSize.Width : Theme.S(420)) - Room;
            var t = Gfx.Measure(Text, Font, width);
            return new Size(Room + t.Width + Theme.S(2), Math.Max(Box, t.Height) + Theme.S(4));
        }

        protected override void OnPaint(PaintEventArgs e)
        {
            var g = e.Graphics;
            using (var back = new SolidBrush(Theme.Backdrop(this))) g.FillRectangle(back, ClientRectangle);
            var box = new Rectangle(0, Theme.S(2), Box, Box);
            Gfx.Fill(g, box, Theme.S(Theme.RadiusSmall), Checked ? Theme.Accent : _hot ? Theme.CardHi : Theme.Card);
            Gfx.Border(g, box, Theme.S(Theme.RadiusSmall), Checked ? Theme.Accent : _hot ? Theme.Dim : Theme.Line);
            if (Checked) Gfx.Glyph(g, Icons.Check, box, Theme.HighContrast ? SystemColors.HighlightText : Color.White, Theme.S(12));
            var text = new Rectangle(Room, 0, Math.Max(1, Width - Room), Height);
            TextRenderer.DrawText(g, Text, Font, text, Enabled ? Theme.Text : Theme.Dim, Gfx.Wrap);
            if (Focused) Gfx.FocusRing(g, new Rectangle(0, 0, Width, Height), Theme.S(Theme.RadiusSmall));
        }
    }

    public sealed class ThemeRadio : RadioButton
    {
        bool _hot;

        public ThemeRadio()
        {
            SetStyle(ControlStyles.UserPaint | ControlStyles.AllPaintingInWmPaint | ControlStyles.OptimizedDoubleBuffer | ControlStyles.ResizeRedraw | ControlStyles.SupportsTransparentBackColor, true);
            BackColor = Color.Transparent;
            UseMnemonic = false;
            AutoSize = true;
            Cursor = Cursors.Hand;
        }

        protected override void OnMouseEnter(EventArgs e) { _hot = true; Invalidate(); base.OnMouseEnter(e); }
        protected override void OnMouseLeave(EventArgs e) { _hot = false; Invalidate(); base.OnMouseLeave(e); }
        protected override void OnGotFocus(EventArgs e) { Invalidate(); base.OnGotFocus(e); }
        protected override void OnLostFocus(EventArgs e) { Invalidate(); base.OnLostFocus(e); }
        protected override void OnCheckedChanged(EventArgs e) { Invalidate(); base.OnCheckedChanged(e); }

        int Box { get { return Theme.S(18); } }
        int Room { get { return Box + Theme.S(10); } }

        public override Size GetPreferredSize(Size proposed)
        {
            int width = (MaximumSize.Width > 0 ? MaximumSize.Width : Theme.S(420)) - Room;
            var t = Gfx.Measure(Text, Font, width);
            return new Size(Room + t.Width + Theme.S(2), Math.Max(Box, t.Height) + Theme.S(4));
        }

        protected override void OnPaint(PaintEventArgs e)
        {
            var g = e.Graphics;
            using (var back = new SolidBrush(Theme.Backdrop(this))) g.FillRectangle(back, ClientRectangle);
            var box = new Rectangle(0, Theme.S(2), Box, Box);
            g.SmoothingMode = System.Drawing.Drawing2D.SmoothingMode.AntiAlias;
            using (var b = new SolidBrush(_hot ? Theme.CardHi : Theme.Card)) g.FillEllipse(b, box);
            using (var pen = new Pen(Checked ? Theme.Accent : _hot ? Theme.Dim : Theme.Line, Math.Max(1, Theme.S(Checked ? 2 : 1)))) g.DrawEllipse(pen, box);
            if (Checked)
            {
                var dot = Rectangle.Inflate(box, -Theme.S(5), -Theme.S(5));
                using (var b = new SolidBrush(Theme.Accent)) g.FillEllipse(b, dot);
            }
            g.SmoothingMode = System.Drawing.Drawing2D.SmoothingMode.Default;
            TextRenderer.DrawText(g, Text, Font, new Rectangle(Room, 0, Math.Max(1, Width - Room), Height), Enabled ? Theme.Text : Theme.Dim, Gfx.Wrap);
            if (Focused) Gfx.FocusRing(g, new Rectangle(0, 0, Width, Height), Theme.S(Theme.RadiusSmall));
        }
    }

    // A text box in the palette, with a border that follows focus.
    public sealed class ThemeBox : Panel, IBackdrop
    {
        public readonly TextBox Box = new TextBox();

        public Color Backdrop { get { return Theme.Nav; } }

        public ThemeBox(int width, string name, bool readOnly)
        {
            // Transparent, so that the card behind it shows through the rounded corners: a solid background of its
            // own left a dark square outside each corner arc and a fringe along the right edge.
            SetStyle(ControlStyles.AllPaintingInWmPaint | ControlStyles.OptimizedDoubleBuffer | ControlStyles.ResizeRedraw | ControlStyles.SupportsTransparentBackColor, true);
            BackColor = Color.Transparent;
            AccessibleName = "";
            Box.BorderStyle = BorderStyle.None;
            Box.BackColor = Theme.Nav;
            Box.ForeColor = Theme.Text;
            Box.Font = Theme.Body;
            Box.ReadOnly = readOnly;
            Box.AccessibleName = name;
            Box.TabStop = true;
            Box.Width = width - Theme.S(20);
            Box.GotFocus += (s, e) => Invalidate();
            Box.LostFocus += (s, e) => Invalidate();
            Controls.Add(Box);
            // The same height as the button beside it: a field and its "Choose folder..." button line up at the top
            // and at the bottom, as a field and its browse button do in a Windows 11 form.
            Size = new Size(width, Math.Max(Ui.RowHeight(), Box.PreferredHeight + Theme.S(10)));
            Box.Location = new Point(Theme.S(10), (Height - Box.PreferredHeight) / 2);
            Margin = Theme.Pad(0, 8, 8, 0);
        }

        protected override void OnPaint(PaintEventArgs e)
        {
            var r = new Rectangle(0, 0, Width, Height);
            Gfx.Fill(e.Graphics, r, Theme.S(Theme.RadiusControl), Theme.Nav);
            Gfx.Border(e.Graphics, r, Theme.S(Theme.RadiusControl), Box.Focused ? Theme.Focus : Theme.Line);
            base.OnPaint(e);
        }
    }

    // The widgets that stayed: a label, a row, a stack. Every interactive control gets an accessible name (G-A11Y).
    static class Ui
    {
        public static Label Label(string text, Font font = null, Color? color = null, int width = 0)
        {
            var l = new Label { Text = text, AutoSize = true, Font = font ?? Theme.Body, ForeColor = color ?? Theme.Text, Margin = Theme.Pad(0, 3, 0, 3), UseMnemonic = false, BackColor = Color.Transparent };
            if (width > 0) l.MaximumSize = new Size(width, 0);
            return l;
        }

        public static Label Dim(string text, int width) { return Label(text, null, Theme.Dim, width); }

        // The height of a button and of a text box beside it, from the running text of a button.
        public static int RowHeight()
        {
            return Math.Max(Gfx.Measure("Ag", Theme.Bold, int.MaxValue / 2).Height, Theme.S(15)) + Theme.S(11) * 2;
        }

        // chevron: the forward chevron of a step that leads somewhere. It is not a synonym of "primary": the only
        // action of the last screen is Close, and a chevron there points at nothing.
        public static FlatButton Button(string text, EventHandler click, bool primary = false, bool chevron = false)
        {
            var b = new FlatButton
            {
                Text = text, Font = Theme.Bold, Kind = primary ? ButtonKind.Primary : ButtonKind.Secondary, Chevron = chevron,
                Margin = Theme.Pad(0, 6, 10, 6), AccessibleName = text.TrimEnd('.', '…'),
            };
            if (click != null) b.Click += click;
            return b;
        }

        public static FlatButton Quiet(string text, string glyph, EventHandler click)
        {
            var b = new FlatButton { Text = text, Font = Theme.Bold, Kind = ButtonKind.Quiet, Glyph = glyph, Margin = Theme.Pad(0, 4, 0, 0), AccessibleName = text.TrimEnd('.', '…') };
            if (click != null) b.Click += click;
            return b;
        }

        public static ThemeCheck Check(string text, int width = 0)
        {
            var c = new ThemeCheck { Text = text, ForeColor = Theme.Text, Font = Theme.Bold, Margin = Theme.Pad(0, 6, 0, 0), AccessibleName = text };
            if (width > 0) c.MaximumSize = new Size(width, 0);
            return c;
        }

        public static ThemeRadio Radio(string text, int width = 0)
        {
            var r = new ThemeRadio { Text = text, ForeColor = Theme.Text, Font = Theme.Bold, Margin = Theme.Pad(0, 4, 18, 0), AccessibleName = text };
            if (width > 0) r.MaximumSize = new Size(width, 0);
            return r;
        }

        public static FlowLayoutPanel Row(params Control[] controls)
        {
            var r = new FlowLayoutPanel { AutoSize = true, AutoSizeMode = AutoSizeMode.GrowAndShrink, WrapContents = false, Margin = Theme.Pad(0), BackColor = Color.Transparent };
            foreach (var c in controls) if (c != null) r.Controls.Add(c);
            return r;
        }

        public static FlowLayoutPanel Stack(int width = 0)
        {
            var p = new FlowLayoutPanel { FlowDirection = FlowDirection.TopDown, WrapContents = false, AutoSize = true, AutoSizeMode = AutoSizeMode.GrowAndShrink, Margin = Theme.Pad(0), BackColor = Color.Transparent };
            if (width > 0) p.MaximumSize = new Size(width, 0);
            return p;
        }
    }

    // A row of a card: an icon, a line of text and an optional note under it. The whole row is one control with its
    // text as its accessible name, so a screen reader reads the icon's meaning once and the text once.
    public sealed class IconRow : Panel
    {
        public string Glyph = Icons.Bullet;
        public Color GlyphColor = Theme.Dim;
        public string Head = "", Note;
        public Color HeadColor = Theme.Text, NoteColor = Theme.Dim;
        public Font HeadFont, NoteFont;
        public bool Strong;

        public IconRow(int width)
        {
            SetStyle(ControlStyles.AllPaintingInWmPaint | ControlStyles.OptimizedDoubleBuffer | ControlStyles.ResizeRedraw | ControlStyles.SupportsTransparentBackColor, true);
            BackColor = Color.Transparent;
            Width = width;
            Margin = Theme.Pad(0, 3, 0, 3);
            AccessibleRole = AccessibleRole.StaticText;
            AutoSize = false;
        }

        int Icon { get { return Theme.S(18); } }
        int Room { get { return Icon + Theme.S(12); } }
        Font Face { get { return HeadFont ?? (Strong ? Theme.Bold : Theme.Body); } }

        public IconRow Done()
        {
            Height = Measure();
            AccessibleName = Head + (Note != null ? ". " + Note : "");
            return this;
        }

        int Measure()
        {
            int room = Math.Max(1, Width - Room);
            int h = Gfx.Measure(Head, Face, room).Height;
            if (!string.IsNullOrEmpty(Note)) h += Theme.S(2) + Gfx.Measure(Note, NoteFont ?? Theme.Small, room).Height;
            return Math.Max(h, Icon) + Theme.S(4);
        }

        protected override void OnPaint(PaintEventArgs e)
        {
            var g = e.Graphics;
            using (var b = new SolidBrush(Theme.Backdrop(this))) g.FillRectangle(b, ClientRectangle);
            Gfx.Glyph(g, Glyph, new Rectangle(0, Theme.S(1), Icon, Icon), GlyphColor, Theme.S(15));
            int room = Math.Max(1, Width - Room);
            var size = Gfx.Measure(Head, Face, room);
            TextRenderer.DrawText(g, Head, Face, new Rectangle(Room, 0, room, size.Height), HeadColor, Gfx.Wrap);
            if (!string.IsNullOrEmpty(Note))
                TextRenderer.DrawText(g, Note, NoteFont ?? Theme.Small, new Rectangle(Room, size.Height + Theme.S(2), room, Height - size.Height), NoteColor, Gfx.Wrap);
        }
    }

    // The line under a page headline: a round icon of the state and one sentence.
    public sealed class StatusLine : Panel
    {
        public enum State { Good, Warn, Bad, Busy, Info }

        readonly string _text;
        readonly State _state;

        public StatusLine(string text, State state, int width)
        {
            SetStyle(ControlStyles.AllPaintingInWmPaint | ControlStyles.OptimizedDoubleBuffer | ControlStyles.ResizeRedraw | ControlStyles.SupportsTransparentBackColor, true);
            BackColor = Color.Transparent;
            _text = text ?? "";
            _state = state;
            Width = width;
            Margin = Theme.Pad(0, 2, 0, 14);
            AccessibleRole = AccessibleRole.StaticText;
            AccessibleName = _text;
            Height = Math.Max(Theme.S(24), Gfx.Measure(_text, Theme.Bold, Math.Max(1, width - Theme.S(34))).Height + Theme.S(4));
        }

        public Color Colour
        {
            get
            {
                switch (_state)
                {
                    case State.Good: return Theme.Good;
                    case State.Warn: return Theme.Warn;
                    case State.Bad: return Theme.Accent;
                    case State.Busy: return Theme.Teal;
                    default: return Theme.Dim;
                }
            }
        }

        string Mark
        {
            get
            {
                switch (_state)
                {
                    case State.Good: return Icons.Check;
                    case State.Warn: return Icons.Warning;
                    case State.Bad: return Icons.Cancel;
                    case State.Busy: return Icons.Play;
                    default: return Icons.Info;
                }
            }
        }

        protected override void OnPaint(PaintEventArgs e)
        {
            var g = e.Graphics;
            using (var b = new SolidBrush(Theme.Backdrop(this))) g.FillRectangle(b, ClientRectangle);
            int d = Theme.S(22);
            Gfx.Badge(g, new Rectangle(0, 0, d, d), Colour, Mark);
            int room = Math.Max(1, Width - Theme.S(34));
            TextRenderer.DrawText(g, _text, Theme.Bold, new Rectangle(Theme.S(34), Theme.S(1), room, Height), Colour, Gfx.Wrap);
        }
    }

    // The progress of a run: a rounded track with a moving band (the engine does not report a percentage for every
    // stage) or a filled part when the stages say how far the run is. Double-buffered, so it never flickers.
    public sealed class ProgressStrip : Control
    {
        public float Value;             // 0..1, the known part
        public bool Indeterminate = true;
        // 0..1, moved by the window's timer. A screen that is drawn and not shown (a render, or a PC whose animation
        // effects are off) keeps this first value, so the strip always shows a band and never an empty track.
        public float Phase = 0.42f;

        // For the gate --smoke-progress: the strips this process made, and per strip the window handles it got and
        // the times it asked to be drawn again (Invalidate).
        public static int Made;
        public int Handles, Invalidations;

        public ProgressStrip(int width)
        {
            Made++;
            SetStyle(ControlStyles.AllPaintingInWmPaint | ControlStyles.OptimizedDoubleBuffer | ControlStyles.UserPaint | ControlStyles.ResizeRedraw, true);
            Width = width;
            Height = Theme.S(8);
            TabStop = false;
            AccessibleRole = AccessibleRole.ProgressBar;
            Margin = Theme.Pad(0, 6, 0, 16);
        }

        // The known part (0..1), or null for the moving band. The strip is drawn again only when that changes.
        public void SetDone(float? done)
        {
            bool busy = !done.HasValue;
            float value = done ?? 0f;
            if (busy == Indeterminate && (busy || value == Value)) return;
            Indeterminate = busy;
            Value = value;
            Invalidate();
        }

        // One step of the moving band, for the window's animation timer. A strip that shows a known part has no band,
        // so it is not drawn again.
        public void Step(float step)
        {
            if (!Indeterminate) return;
            Phase += step;
            if (Phase > 1f) Phase -= 1f;
            Invalidate();
        }

        protected override void OnHandleCreated(EventArgs e)
        {
            Handles++;
            base.OnHandleCreated(e);
        }

        protected override void OnInvalidated(InvalidateEventArgs e)
        {
            Invalidations++;
            base.OnInvalidated(e);
        }

        // A progress bar that says nothing is worse than no progress bar: the accessible object gives the part that
        // is done as a percentage, or the busy state while the engine reports no stage of its own.
        protected override AccessibleObject CreateAccessibilityInstance() { return new Spoken(this); }

        sealed class Spoken : ControlAccessibleObject
        {
            readonly ProgressStrip _strip;
            public Spoken(ProgressStrip strip) : base(strip) { _strip = strip; }
            public override AccessibleRole Role { get { return AccessibleRole.ProgressBar; } }
            public override AccessibleStates State { get { return _strip.Indeterminate ? AccessibleStates.Busy | AccessibleStates.ReadOnly : AccessibleStates.ReadOnly; } }
            public override string Value
            {
                get { return _strip.Indeterminate ? "" : ((int)Math.Round(Math.Max(0f, Math.Min(1f, _strip.Value)) * 100)) + "%"; }
                set { }
            }
        }

        protected override void OnPaint(PaintEventArgs e)
        {
            var g = e.Graphics;
            var r = new Rectangle(0, 0, Width, Height);
            using (var b = new SolidBrush(Theme.Backdrop(this))) g.FillRectangle(b, r);
            int radius = Height / 2;
            Gfx.Fill(g, r, radius, Theme.CardHi);
            if (Indeterminate)
            {
                int band = Math.Max(Theme.S(90), Width / 5);
                int travel = Width + band;
                int x = (int)(Phase * travel) - band;
                var seg = Rectangle.Intersect(r, new Rectangle(x, 0, band, Height));
                if (seg.Width > 2) Gfx.FillGradient(g, seg, radius, Theme.AccentHi, Theme.Accent);
            }
            else
            {
                int w = (int)(Width * Math.Max(0f, Math.Min(1f, Value)));
                if (w > 2) Gfx.FillGradient(g, new Rectangle(0, 0, w, Height), radius, Theme.AccentHi, Theme.Accent);
            }
        }
    }

    // One step of the rail: its number or state glyph, its name, the accent bar and the tint of the current step.
    public sealed class RailRow : Panel
    {
        readonly string _text, _glyph, _state;
        readonly RailState _kind;

        public RailRow(int width, int height, string text, string glyph, RailState kind, string stateWord)
        {
            SetStyle(ControlStyles.AllPaintingInWmPaint | ControlStyles.OptimizedDoubleBuffer | ControlStyles.ResizeRedraw, true);
            _text = text; _glyph = glyph; _kind = kind; _state = stateWord;
            Size = new Size(width, height);
            Margin = Theme.Pad(10, 0, 10, 4);
            AccessibleRole = AccessibleRole.StaticText;
            AccessibleName = text + (stateWord != null ? " (" + stateWord + ")" : "");
        }

        protected override void OnPaint(PaintEventArgs e)
        {
            var g = e.Graphics;
            var r = new Rectangle(0, 0, Width, Height);
            using (var b = new SolidBrush(Theme.Backdrop(this))) g.FillRectangle(b, r);
            bool on = _kind == RailState.Current || _kind == RailState.Failed;
            if (on)
            {
                Gfx.Fill(g, r, Theme.S(Theme.RadiusControl), _kind == RailState.Failed ? Theme.TintBad : Theme.Tint);
                // The accent bar at the left edge. In high contrast the tint is Highlight, so the bar is drawn in
                // HighlightText: an accent on an accent would disappear.
                using (var p = Gfx.Round(new Rectangle(0, 0, Theme.S(10), Height), Theme.S(Theme.RadiusControl)))
                using (var b = new SolidBrush(Theme.HighContrast ? Theme.OnTint : Theme.Accent))
                {
                    g.SetClip(new Rectangle(0, 0, Theme.S(4), Height));
                    g.SmoothingMode = System.Drawing.Drawing2D.SmoothingMode.AntiAlias;
                    g.FillPath(b, p);
                    g.SmoothingMode = System.Drawing.Drawing2D.SmoothingMode.Default;
                    g.ResetClip();
                }
            }
            var mark = _kind == RailState.Done ? Icons.Check : _kind == RailState.Failed ? Icons.Cancel : _glyph;
            var colour = _kind == RailState.Failed ? Theme.Accent : _kind == RailState.Done ? Theme.Good : on ? Theme.Text : Theme.Dim;
            // Everything drawn on the tinted field of the current or the stopped step takes the colour that field
            // carries: WindowText on Highlight cannot be read.
            if (on && Theme.HighContrast) colour = Theme.OnTint;
            int icon = Theme.S(18);
            Gfx.Glyph(g, mark, new Rectangle(Theme.S(16), (Height - icon) / 2, icon, icon), colour, Theme.S(16));
            var font = on ? Theme.Bold : Theme.Body;
            int x = Theme.S(16) + icon + Theme.S(12);
            var size = Gfx.Measure(_text, font, Math.Max(1, Width - x - Theme.S(8)));
            TextRenderer.DrawText(g, _text, font, new Rectangle(x, (Height - size.Height) / 2, Math.Max(1, Width - x - Theme.S(8)), size.Height),
                _kind == RailState.Failed ? (Theme.HighContrast ? Theme.OnTint : Theme.Text) : colour, Gfx.Wrap);
        }
    }

    // The result screen's big round icon, with the short entrance of a window that may animate.
    public sealed class ResultMark : Panel
    {
        readonly Color _colour;
        readonly string _glyph;
        public float Grow = 1f;         // 0..1 during the entrance

        public ResultMark(Color colour, string glyph, int size)
        {
            SetStyle(ControlStyles.AllPaintingInWmPaint | ControlStyles.OptimizedDoubleBuffer | ControlStyles.ResizeRedraw | ControlStyles.SupportsTransparentBackColor, true);
            _colour = colour; _glyph = glyph;
            Size = new Size(size, size);
            Margin = Theme.Pad(0, 0, 0, 14);
            BackColor = Color.Transparent;
            AccessibleName = "";
            AccessibleRole = AccessibleRole.Graphic;
        }

        protected override void OnPaint(PaintEventArgs e)
        {
            var g = e.Graphics;
            using (var b = new SolidBrush(Theme.Backdrop(this))) g.FillRectangle(b, ClientRectangle);
            float k = Math.Max(0.2f, Math.Min(1f, Grow));
            int d = (int)(Width * k);
            var box = new Rectangle((Width - d) / 2, (Height - d) / 2, d, d);
            g.SmoothingMode = System.Drawing.Drawing2D.SmoothingMode.AntiAlias;
            using (var b = new SolidBrush(Color.FromArgb(40, _colour))) g.FillEllipse(b, box);
            g.SmoothingMode = System.Drawing.Drawing2D.SmoothingMode.Default;
            var inner = Rectangle.Inflate(box, -(int)(d * 0.18f), -(int)(d * 0.18f));
            Gfx.Badge(g, inner, _colour, _glyph);
        }
    }

    // A titled block of a page; Inner is the width its content may use. The title is drawn by the card and is the
    // card's accessible name (a group), not a label: Windows names an unnamed window after the nearest label before it
    // in its parent, so a title label made every bare row of the card a second copy of the title for a screen reader.
    sealed class CardPanel : Panel, IBackdrop
    {
        public readonly FlowLayoutPanel Body;
        public readonly int Inner;
        readonly string _title;
        readonly string _glyph;
        readonly Color _back;
        Rectangle _titleBox;
        readonly int _depth;
        const TextFormatFlags TitleFlags = TextFormatFlags.WordBreak | TextFormatFlags.NoPrefix | TextFormatFlags.Left | TextFormatFlags.Top;

        public CardPanel(string title, int width, Color? back = null, string glyph = null, Color? line = null)
        {
            SetStyle(ControlStyles.ResizeRedraw | ControlStyles.OptimizedDoubleBuffer | ControlStyles.AllPaintingInWmPaint, true);
            _back = back ?? Theme.Card;
            _line = line ?? Theme.Line;
            _glyph = glyph;
            _depth = Theme.S(3);
            BackColor = Theme.Back;
            AutoSize = true; AutoSizeMode = AutoSizeMode.GrowAndShrink;
            Padding = Theme.Pad(18, 14, 18, 16);
            Margin = Theme.Pad(0, 0, 0, 14);
            MinimumSize = new Size(width, 0); MaximumSize = new Size(width, 0);
            Inner = width - Padding.Horizontal;
            _title = title;
            int top = Padding.Top;
            if (title != null)
            {
                int indent = glyph != null ? Theme.S(26) : 0;
                var h = TextRenderer.MeasureText(title, Theme.CardTitle, new Size(Inner - indent, int.MaxValue), TitleFlags).Height;
                _titleBox = new Rectangle(Padding.Left + indent, top + Theme.S(2), Inner - indent, h);
                top = _titleBox.Bottom + Theme.S(10);
                AccessibleName = title;
                AccessibleRole = AccessibleRole.Grouping;
            }
            Padding = new Padding(Padding.Left, top, Padding.Right, Padding.Bottom + _depth);
            Body = Ui.Stack(Inner);
            Body.Dock = DockStyle.Fill;
            Body.BackColor = Color.Transparent;
            Controls.Add(Body);
        }

        readonly Color _line;

        public string Title { get { return _title; } }

        // What the card paints over its own BackColor: the colour its rows draw their background in (BD-137).
        public Color Backdrop { get { return _back; } }

        // The pixels the shadow takes under the card: the card itself is drawn down to Height - Depth.
        public int Depth { get { return _depth; } }

        protected override void OnPaint(PaintEventArgs e)
        {
            var g = e.Graphics;
            using (var b = new SolidBrush(Theme.Backdrop(this))) g.FillRectangle(b, ClientRectangle);
            Gfx.Card(g, new Rectangle(0, 0, Width, Height - _depth), Theme.S(Theme.RadiusCard), _back, _line, _depth);
            if (_title == null) return;
            if (_glyph != null) Gfx.Glyph(g, _glyph, new Rectangle(Theme.S(18), _titleBox.Y, Theme.S(18), _titleBox.Height), Theme.Teal, Theme.S(16));
            TextRenderer.DrawText(g, _title, Theme.CardTitle, _titleBox, Theme.Text, TitleFlags);
        }

        public T Add<T>(T c) where T : Control { Body.Controls.Add(c); return c; }

        public IconRow Row(string glyph, Color colour, string head, string note = null, bool strong = false, Font font = null)
        {
            var r = new IconRow(Inner) { Glyph = glyph, GlyphColor = colour, Head = head, Note = note, Strong = strong, HeadFont = font };
            Body.Controls.Add(r.Done());
            return r;
        }

        // A label and a value in two columns, the value wrapping inside the card.
        public Label Pair(string key, string value, Color? color = null)
        {
            int keyWidth = Math.Min(Theme.S(210), Inner / 3);
            var k = Ui.Label(key, null, Theme.Dim, keyWidth);
            k.MinimumSize = new Size(keyWidth, 0);
            var v = Ui.Label(value, null, color, Inner - keyWidth - Theme.S(8));
            Body.Controls.Add(Ui.Row(k, v));
            return v;
        }
    }

    // The guide's speech bubble: the tip in a teal-outlined balloon with a tail that points at the character under it
    // (the accepted mockup). The bubble carries the words, so a screen reader reads the tip and never the picture.
    public sealed class Bubble : Panel, IBackdrop
    {
        // The colour the balloon paints over its own BackColor, and so the colour anything placed inside it clears
        // itself in: the balloon is darker than the guide card it sits on (BD-137).
        public Color Backdrop { get { return Theme.HighContrast ? SystemColors.Control : Color.FromArgb(0x0C, 0x1E, 0x24); } }

        // Where the tail leaves the balloon, as a part of its width: the window points it at the guide's head.
        // WithTail false is the panel without her, where a tail would point at nobody.
        public float TailAt = 0.62f;
        bool _withTail = true;

        public bool WithTail
        {
            get { return _withTail; }
            set { if (_withTail == value) return; _withTail = value; Say(Text); }
        }

        public Bubble(int width)
        {
            SetStyle(ControlStyles.AllPaintingInWmPaint | ControlStyles.OptimizedDoubleBuffer | ControlStyles.ResizeRedraw | ControlStyles.SupportsTransparentBackColor, true);
            BackColor = Color.Transparent;
            Width = width;
            Margin = Theme.Pad(0, 0, 0, 0);
            AccessibleRole = AccessibleRole.StaticText;
            Say("");
        }

        int Tail { get { return _withTail ? Theme.S(10) : 0; } }
        int Inset { get { return Theme.S(12); } }

        public void Say(string text)
        {
            Text = text ?? "";
            AccessibleName = Text;
            Height = Gfx.Measure(Text, Theme.Body, Math.Max(1, Width - Inset * 2)).Height + Inset * 2 + Tail;
            Invalidate();
        }

        protected override void OnPaint(PaintEventArgs e)
        {
            var g = e.Graphics;
            using (var b = new SolidBrush(Theme.Backdrop(this))) g.FillRectangle(b, ClientRectangle);
            var body = new Rectangle(0, 0, Width, Height - Tail);
            Gfx.Fill(g, body, Theme.S(Theme.RadiusCard), Backdrop);
            Gfx.Border(g, body, Theme.S(Theme.RadiusCard), Theme.Teal);
            if (_withTail)
            {
                // The tail, drawn as a triangle that keeps the balloon's outline, over the guide's head.
                int at = (int)(Width * Math.Max(0.1f, Math.Min(0.85f, TailAt)));
                at = Math.Max(Inset * 2, Math.Min(Width - Inset * 2 - Tail * 2, at));
                var p = new Point[] { new Point(at, body.Bottom - 2), new Point(at + Tail * 2, body.Bottom - 2), new Point(at + Tail + Tail / 2, Height - 1) };
                g.SmoothingMode = System.Drawing.Drawing2D.SmoothingMode.AntiAlias;
                using (var fill = new SolidBrush(Backdrop)) g.FillPolygon(fill, p);
                using (var pen = new Pen(Theme.Teal))
                {
                    g.DrawLine(pen, p[0], p[2]);
                    g.DrawLine(pen, p[1], p[2]);
                }
                g.SmoothingMode = System.Drawing.Drawing2D.SmoothingMode.Default;
            }
            TextRenderer.DrawText(g, Text, Theme.Body, new Rectangle(Inset, Inset, Math.Max(1, Width - Inset * 2), Math.Max(1, body.Height - Inset)), Theme.Text, Gfx.Wrap);
        }
    }

    // The release notes, as formatted text: headings, bullets and paragraphs with their spacing. The blocks are laid
    // out at their full height inside the page, so the page has one scroll axis: a box of its own put a second scroll
    // thumb 50 px from the page's and showed the longest text on the screen through the smallest window on it.
    // The notes are release content, so G-NOINT does not read them (Tag "noint-exempt").
    public sealed class NotesPanel : Panel, IBackdrop
    {
        readonly FlowLayoutPanel _flow;

        // The notes sit on the field colour, not on the card: a row inside them clears itself in that colour.
        public Color Backdrop { get { return Theme.Nav; } }

        public NotesPanel(string markdown, int width, string name)
        {
            SetStyle(ControlStyles.ResizeRedraw | ControlStyles.OptimizedDoubleBuffer | ControlStyles.AllPaintingInWmPaint | ControlStyles.SupportsTransparentBackColor, true);
            BackColor = Color.Transparent;
            AutoSize = true;
            AutoSizeMode = AutoSizeMode.GrowAndShrink;
            MinimumSize = new Size(width, 0);
            MaximumSize = new Size(width, 0);
            Padding = Theme.Pad(13, 10, 13, 12);
            Tag = "noint-exempt";
            AccessibleName = name ?? "";
            AccessibleRole = AccessibleRole.Grouping;
            Margin = Theme.Pad(0, 8, 0, 0);
            int room = width - Padding.Horizontal;
            _flow = Ui.Stack(room);
            _flow.Dock = DockStyle.Fill;
            _flow.BackColor = Color.Transparent;
            _flow.AccessibleName = "";
            foreach (var block in NotesDoc.Parse(markdown))
            {
                Control c;
                if (block.Kind == NoteKind.Heading)
                {
                    c = Ui.Label(block.Text, Theme.SmallBold, Theme.Teal, room);
                    c.Margin = Theme.Pad(0, 10, 0, 4);
                }
                else if (block.Kind == NoteKind.Bullet)
                {
                    var r = new IconRow(room) { Glyph = Icons.Bullet, GlyphColor = Theme.Dim, Head = block.Text, HeadFont = Theme.Small, HeadColor = Theme.Text };
                    r.Margin = Theme.Pad(0, 1, 0, 1);
                    c = r.Done();
                }
                else
                {
                    c = Ui.Label(block.Text, Theme.Small, block.Kind == NoteKind.Title ? Theme.Text : Theme.Dim, room);
                    c.Margin = Theme.Pad(0, 2, 0, 4);
                }
                _flow.Controls.Add(c);
            }
            Controls.Add(_flow);
        }

        protected override void OnPaint(PaintEventArgs e)
        {
            var r = new Rectangle(0, 0, Width, Height);
            Gfx.Fill(e.Graphics, r, Theme.S(Theme.RadiusSmall), Theme.Nav);
            Gfx.Border(e.Graphics, r, Theme.S(Theme.RadiusSmall), Theme.Line);
            base.OnPaint(e);
        }
    }
}
