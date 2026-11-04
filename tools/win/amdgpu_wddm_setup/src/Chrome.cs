// The window frame: a title bar of our own instead of the white caption of Windows (the accepted mockup), the rounded
// corners and the dark frame that Windows 11 draws for a borderless window, and the short crossfade between screens.
//
// The window keeps everything a window owes its user: it moves with the mouse (the title bar asks Windows for the
// caption drag, so the taskbar, the snap gesture and the double-click of a title bar behave as Windows means them to),
// Alt+F4 closes it, Escape and Enter keep their buttons and the minimise and close glyphs are ordinary buttons with a
// name, a focus ring and a default action.
using System;
using System.Drawing;
using System.Linq;
using System.Runtime.InteropServices;
using System.Windows.Forms;

namespace AmdgpuWddmSetup
{
    static class Dwm
    {
        const int DWMWA_USE_IMMERSIVE_DARK_MODE = 20, DWMWA_WINDOW_CORNER_PREFERENCE = 33, DWMWA_BORDER_COLOR = 34;
        const int DWMWCP_ROUND = 2;

        [DllImport("dwmapi.dll")]
        static extern int DwmSetWindowAttribute(IntPtr hwnd, int attribute, ref int value, int size);

        // Windows 11 rounds the corners and draws the thin frame in our colour; Windows 10 ignores all three and the
        // window stays square, which is how its own windows look.
        public static void Apply(Form form)
        {
            if (form == null || !form.IsHandleCreated) return;
            try
            {
                int on = 1;
                DwmSetWindowAttribute(form.Handle, DWMWA_USE_IMMERSIVE_DARK_MODE, ref on, sizeof(int));
                int round = DWMWCP_ROUND;
                DwmSetWindowAttribute(form.Handle, DWMWA_WINDOW_CORNER_PREFERENCE, ref round, sizeof(int));
                var c = Theme.Line;
                int border = c.R | (c.G << 8) | (c.B << 16);
                DwmSetWindowAttribute(form.Handle, DWMWA_BORDER_COLOR, ref border, sizeof(int));
            }
            catch (Exception) { }
        }
    }

    // The title bar: the mark, the product name, the language choice and the two window glyphs.
    public sealed class ChromeBar : Panel
    {
        const int WM_NCLBUTTONDOWN = 0xA1, HTCAPTION = 2;

        [DllImport("user32.dll")]
        static extern bool ReleaseCapture();

        [DllImport("user32.dll")]
        static extern bool PostMessage(IntPtr hwnd, int msg, IntPtr wParam, IntPtr lParam);

        [DllImport("user32.dll")]
        static extern bool GetCursorPos(out System.Drawing.Point point);

        readonly Form _form;
        readonly string _brand;

        // The language choice of the bar (G-CHROME reads it).
        public FlatButton Language { get; private set; }

        // The names the language list shows, in its order: every language by its own name.
        public static string[] LanguageNames() { return Strings.Languages.Select(Strings.Endonym).ToArray(); }

        // The list of languages as a menu under the button, in the window's palette; the current one is checked.
        static void OpenLanguages(Control under, Action<string> chooseLanguage)
        {
            var menu = new ContextMenuStrip { ShowImageMargin = false, ShowCheckMargin = true, BackColor = Theme.Nav, ForeColor = Theme.Text, Font = Theme.Body, Renderer = new DarkMenu() };
            menu.AccessibleName = Strings.LanguageLabel();
            menu.Items.Add(new ToolStripLabel(Strings.LanguageLabel()) { ForeColor = Theme.Dim, Font = Theme.Small });
            menu.Items.Add(new ToolStripSeparator());
            ToolStripMenuItem chosen = null;
            foreach (var code in Strings.Languages)
            {
                string c = code;
                var item = new ToolStripMenuItem(Strings.Endonym(code)) { ForeColor = Theme.Text, Checked = code == Strings.Language };
                if (item.Checked) chosen = item;
                item.Click += (s, e) => { if (c != Strings.Language) chooseLanguage(c); };
                menu.Items.Add(item);
            }
            menu.Closed += (s, e) => { if (under.IsHandleCreated && !under.IsDisposed) under.BeginInvoke((Action)menu.Dispose); else menu.Dispose(); };
            menu.Show(under, new Point(0, under.Height));
            if (chosen != null) chosen.Select();
        }

        sealed class DarkMenu : ToolStripProfessionalRenderer
        {
            public DarkMenu() : base(new Colors()) { RoundedEdges = false; }

            protected override void OnRenderItemText(ToolStripItemTextRenderEventArgs e) { if (!(e.Item is ToolStripLabel)) e.TextColor = Theme.Text; base.OnRenderItemText(e); }

            sealed class Colors : ProfessionalColorTable
            {
                public override Color ToolStripDropDownBackground { get { return Theme.Nav; } }
                public override Color MenuBorder { get { return Theme.Line; } }
                public override Color MenuItemBorder { get { return Theme.Line; } }
                public override Color MenuItemSelected { get { return Theme.CardHi; } }
                public override Color ImageMarginGradientBegin { get { return Theme.Nav; } }
                public override Color ImageMarginGradientMiddle { get { return Theme.Nav; } }
                public override Color ImageMarginGradientEnd { get { return Theme.Nav; } }
                public override Color CheckBackground { get { return Theme.Nav; } }
                public override Color CheckSelectedBackground { get { return Theme.CardHi; } }
                public override Color SeparatorDark { get { return Theme.Line; } }
            }
        }

        public ChromeBar(Form form, string brand, int height, Action<string> chooseLanguage, string minimiseName, string closeName)
        {
            SetStyle(ControlStyles.AllPaintingInWmPaint | ControlStyles.OptimizedDoubleBuffer | ControlStyles.ResizeRedraw, true);
            _form = form;
            _brand = brand;
            Dock = DockStyle.Top;
            Height = height;
            BackColor = Theme.Nav;
            AccessibleName = "";
            Text = brand;

            var close = new FlatButton { Kind = ButtonKind.Chrome, Glyph = Icons.Close, AccessibleName = closeName, Text = "", AutoSize = false, Margin = Theme.Pad(0) };
            close.Size = close.GetPreferredSize(Size.Empty);
            close.Click += (s, e) => _form.Close();
            var minimise = new FlatButton { Kind = ButtonKind.Chrome, Glyph = Icons.Minimise, AccessibleName = minimiseName, Text = "", AutoSize = false, Margin = Theme.Pad(0) };
            minimise.Size = minimise.GetPreferredSize(Size.Empty);
            minimise.Click += (s, e) => _form.WindowState = FormWindowState.Minimized;
            close.Location = new Point(Width - close.Width - Theme.S(6), (Height - close.Height) / 2);
            minimise.Location = new Point(close.Left - minimise.Width - Theme.S(2), close.Top);
            close.Anchor = minimise.Anchor = AnchorStyles.Top | AnchorStyles.Right;

            // The language choice: one button with a globe and the current language's own name, which opens the list of
            // every language, each by its own name (Strings.Endonym) and never translated, under a label that keeps the
            // English word. A person who cannot read the current language finds it by the globe.
            // The tab order of the bar follows what the eye sees, left to right: the language choice, then minimise,
            // then close. WinForms orders children of the same tab index by their z-order, which put "Close window"
            // first; every index here is explicit, and the window gives the bar the last index of all (AssignTabOrder),
            // so the first Tab of the window never lands on the close glyph.
            var current = Strings.Endonym(Strings.Language);
            var language = new FlatButton
            {
                Text = current + " \u25BE", Font = Theme.Small, Kind = ButtonKind.Pill, Glyph = Icons.Globe, Selected = true, Margin = Theme.Pad(0),
                AccessibleName = Strings.LanguageLabel() + ": " + current, AccessibleRole = AccessibleRole.ComboBox, Tag = "lang", TabIndex = 0,
            };
            language.Click += (s, e) => OpenLanguages(language, chooseLanguage);
            language.KeyDown += (s, e) => { if (e.KeyCode == Keys.F4 || (e.Alt && e.KeyCode == Keys.Down)) { e.Handled = true; OpenLanguages(language, chooseLanguage); } };
            Language = language;
            minimise.TabIndex = 1;
            close.TabIndex = 2;
            Controls.Add(language);
            Controls.Add(minimise);
            Controls.Add(close);
            language.Size = language.GetPreferredSize(Size.Empty);
            language.Location = new Point(minimise.Left - language.Width - Theme.S(16), (Height - language.Height) / 2);
            language.Anchor = AnchorStyles.Top | AnchorStyles.Right;
        }

        // The bar itself drags the window; its buttons keep their own clicks.
        //
        // The message is posted, not sent. Windows answers it with a message loop of its own that lasts until the
        // button is released, and a sent message would run that whole loop inside this handler, with this bar's
        // frame underneath it: the window rebuilds itself when a drag carries it onto a screen of another size
        // (SetupForm, the rule), and a rebuild disposes this bar. A posted message starts the
        // drag after this handler has returned, so nothing of this bar is on the stack while it lasts.
        protected override void OnMouseDown(MouseEventArgs e)
        {
            base.OnMouseDown(e);
            if (e.Button != MouseButtons.Left || _form == null || !_form.IsHandleCreated) return;
            try
            {
                ReleaseCapture();
                System.Drawing.Point p;
                if (!GetCursorPos(out p)) p = Control.MousePosition;
                // The point of the hit, as Windows packs it: the low word is x, the high word is y, and a screen
                // left of or above the primary one gives negative numbers, so both are masked to their own word.
                PostMessage(_form.Handle, WM_NCLBUTTONDOWN, (IntPtr)HTCAPTION, (IntPtr)(((p.Y & 0xFFFF) << 16) | (p.X & 0xFFFF)));
            }
            catch (Exception) { }
        }

        protected override void OnPaint(PaintEventArgs e)
        {
            var g = e.Graphics;
            using (var b = new SolidBrush(Theme.Nav)) g.FillRectangle(b, ClientRectangle);
            using (var pen = new Pen(Theme.Line)) g.DrawLine(pen, 0, Height - 1, Width, Height - 1);
            // The Nagi mark beside the product name (NagiMark.cs, the project's logo).
            int d = Theme.S(26), x = Theme.S(20), y = (Height - d) / 2;
            NagiMark.Draw(g, new Rectangle(x, y, d, d), Theme.Nav);
            var size = Gfx.Measure(_brand, Theme.Brand, Width);
            TextRenderer.DrawText(g, _brand, Theme.Brand, new Rectangle(x + d + Theme.S(14), (Height - size.Height) / 2, Width, size.Height), Theme.Text,
                TextFormatFlags.NoPrefix | TextFormatFlags.SingleLine | TextFormatFlags.Left | TextFormatFlags.NoPadding);
        }
    }

    // The crossfade between two screens: a picture of the screen that leaves, fading out over the one that arrives.
    // It exists only while a shown window animates and only when the screen really changes; a headless render never
    // makes one. It never takes a click: the mouse goes through it to the screen underneath (WM_NCHITTEST), so no
    // button can be dead while a fade runs.
    public sealed class FadeOverlay : Panel
    {
        const int WM_NCHITTEST = 0x0084, HTTRANSPARENT = -1;

        Bitmap _shot;
        int _alpha = 255;
        readonly Timer _timer;

        public FadeOverlay(Control over, int milliseconds)
        {
            SetStyle(ControlStyles.AllPaintingInWmPaint | ControlStyles.OptimizedDoubleBuffer | ControlStyles.UserPaint, true);
            AccessibleName = "";
            TabStop = false;
            Enabled = false;
            Bounds = new Rectangle(0, 0, over.Width, over.Height);
            _shot = new Bitmap(Math.Max(1, over.Width), Math.Max(1, over.Height));
            try { over.DrawToBitmap(_shot, new Rectangle(0, 0, _shot.Width, _shot.Height)); }
            catch (Exception) { }
            int step = Math.Max(1, 255 / Math.Max(1, milliseconds / 16));
            _timer = new Timer { Interval = 16 };
            _timer.Tick += (s, e) =>
            {
                // The window may be closed in the middle of a fade: the overlay is then disposed through its parent
                // while this timer still ticks.
                if (IsDisposed || _shot == null) { _timer.Stop(); return; }
                _alpha -= step;
                if (_alpha <= 0) { Done(); return; }
                Invalidate();
            };
        }

        protected override void WndProc(ref Message m)
        {
            if (m.Msg == WM_NCHITTEST) { m.Result = (IntPtr)HTTRANSPARENT; return; }
            base.WndProc(ref m);
        }

        public void Run(Control parent)
        {
            parent.Controls.Add(this);
            BringToFront();
            _timer.Start();
        }

        void Done()
        {
            _timer.Stop();
            if (Parent != null) Parent.Controls.Remove(this);
            Dispose();
        }

        protected override void Dispose(bool disposing)
        {
            if (disposing)
            {
                _timer.Stop();
                _timer.Dispose();
                if (_shot != null) { _shot.Dispose(); _shot = null; }
            }
            base.Dispose(disposing);
        }

        protected override void OnPaint(PaintEventArgs e)
        {
            if (_shot == null) return;
            using (var attr = new System.Drawing.Imaging.ImageAttributes())
            {
                var m = new System.Drawing.Imaging.ColorMatrix { Matrix33 = Math.Max(0, _alpha) / 255f };
                attr.SetColorMatrix(m);
                e.Graphics.DrawImage(_shot, new Rectangle(0, 0, Width, Height), 0, 0, _shot.Width, _shot.Height, GraphicsUnit.Pixel, attr);
            }
        }
    }
}
