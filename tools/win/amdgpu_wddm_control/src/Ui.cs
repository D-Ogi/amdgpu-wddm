// Colours, fonts, one scale factor and the few widgets every page uses. The process is per-monitor DPI aware
// (app.manifest) and the form does no automatic scaling, so every pixel size goes through S() and the fonts are sized
// in pixels with the same factor; the text scale (Windows "Make text bigger", WU-069) multiplies the fonts only, so
// labels wrap and cards grow. Fonts follow the language: Yu Gothic UI for Japanese, Malgun Gothic for Korean.
using System;
using System.Collections.Generic;
using System.Drawing;
using System.Linq;
using System.Runtime.InteropServices;
using System.Windows.Forms;

namespace AmdgpuWddmControl
{
    static class Theme
    {
        public static readonly Color Back = Color.FromArgb(18, 20, 26), Nav = Color.FromArgb(13, 15, 20), Card = Color.FromArgb(28, 31, 39),
            CardHi = Color.FromArgb(38, 42, 52), Text = Color.FromArgb(234, 236, 240), Dim = Color.FromArgb(160, 166, 178),
            Accent = Color.FromArgb(226, 38, 62), Teal = Color.FromArgb(64, 204, 214), Good = Color.FromArgb(90, 200, 120),
            Warn = Color.FromArgb(240, 180, 60), Line = Color.FromArgb(58, 62, 74), Focus = Color.FromArgb(120, 200, 255),
            GuideBack = Color.FromArgb(20, 44, 52);
        public static float Scale { get; private set; }
        public static float TextScale { get; private set; }
        public static Font Body, Bold, Small, Title, CardTitle, Brand, Big, Mono, MonoSmall;

        static Theme()
        {
            float dpi = 96;
            try { using (var g = Graphics.FromHwnd(IntPtr.Zero)) dpi = g.DpiX; } catch (Exception) { }
            Init(dpi / 96f, SystemTextScale());
        }

        // Windows "Make text bigger" (Accessibility > Text size): HKCU\Software\Microsoft\Accessibility TextScaleFactor, percent.
        public static float SystemTextScale()
        {
            try
            {
                using (var k = Microsoft.Win32.Registry.CurrentUser.OpenSubKey(@"Software\Microsoft\Accessibility"))
                {
                    var v = k == null ? null : k.GetValue("TextScaleFactor") as int?;
                    return v != null && v >= 100 && v <= 225 ? v.Value / 100f : 1f;
                }
            }
            catch (Exception) { return 1f; }
        }

        // scale: 1.0 at 96 DPI; --smoke-render passes 1.25, 1.5 or 2 to draw the pages as a 120, 144 or 192 DPI screen shows them.
        public static void Init(float scale, float textScale)
        {
            Scale = scale <= 0 ? 1 : scale;
            TextScale = textScale < 1 ? 1 : textScale;
            Fonts();
        }

        public static void Fonts()
        {
            string lang = Strings.Language;
            string face = lang == "ja" ? "Yu Gothic UI" : lang == "ko" ? "Malgun Gothic" : "Segoe UI";
            string semi = lang == "ja" ? "Yu Gothic UI Semibold" : lang == "ko" ? "Malgun Gothic" : "Segoe UI Semibold";
            Body = Px(face, 9.5f); Bold = Px(semi, 9.5f, lang == "ko"); Small = Px(face, 8.5f); Title = Px(semi, 17f, lang == "ko");
            CardTitle = Px(semi, 11.5f, lang == "ko"); Brand = Px("Segoe UI Semibold", 13f); Big = Px(semi, 14f, lang == "ko");
            Mono = Px("Consolas", 9f); MonoSmall = Px("Consolas", 8.5f);
        }

        static Font Px(string name, float points, bool bold = false)
        {
            return new Font(name, points * 96f / 72f * Scale * TextScale, bold ? FontStyle.Bold : FontStyle.Regular, GraphicsUnit.Pixel);
        }

        public static int S(int px) { return (int)Math.Round(px * Scale); }
        public static Size Sz(int w, int h) { return new Size(S(w), S(h)); }
        public static Padding Pad(int all) { return new Padding(S(all)); }
        public static Padding Pad(int left, int top, int right, int bottom) { return new Padding(S(left), S(top), S(right), S(bottom)); }

        [DllImport("uxtheme.dll", CharSet = CharSet.Unicode)]
        static extern int SetWindowTheme(IntPtr hwnd, string appName, string idList);

        [DllImport("user32.dll")]
        static extern IntPtr SendMessage(IntPtr hwnd, int msg, IntPtr wParam, IntPtr lParam);

        const int LVM_GETHEADER = 0x101F;

        // Dark scroll bars on a scrolling control (the Explorer dark theme of Windows 10 1809 and later).
        public static T DarkScroll<T>(T c) where T : Control
        {
            c.HandleCreated += (s, e) => { try { SetWindowTheme(c.Handle, "DarkMode_Explorer", null); } catch (Exception) { } };
            return c;
        }

        // A list view in the theme colours, headers and rows drawn here.
        public static ListView DarkList(ListView lv)
        {
            DarkScroll(lv);
            lv.OwnerDraw = true;
            lv.HandleCreated += (s, e) =>
            {
                try { SetWindowTheme(SendMessage(lv.Handle, LVM_GETHEADER, IntPtr.Zero, IntPtr.Zero), "DarkMode_ItemsView", null); } catch (Exception) { }
            };
            const TextFormatFlags flags = TextFormatFlags.VerticalCenter | TextFormatFlags.Left | TextFormatFlags.EndEllipsis | TextFormatFlags.NoPrefix | TextFormatFlags.SingleLine;
            lv.DrawColumnHeader += (s, e) =>
            {
                using (var b = new SolidBrush(Nav)) e.Graphics.FillRectangle(b, e.Bounds);
                using (var pen = new Pen(Line)) e.Graphics.DrawLine(pen, e.Bounds.Left, e.Bounds.Bottom - 1, e.Bounds.Right, e.Bounds.Bottom - 1);
                TextRenderer.DrawText(e.Graphics, e.Header.Text, lv.Font, Rectangle.Inflate(e.Bounds, -S(6), 0), Dim, flags);
            };
            lv.DrawItem += (s, e) => { };
            lv.DrawSubItem += (s, e) =>
            {
                using (var b = new SolidBrush(e.Item.Selected ? Line : lv.BackColor)) e.Graphics.FillRectangle(b, e.Bounds);
                var fore = e.Item.UseItemStyleForSubItems ? e.Item.ForeColor : e.SubItem.ForeColor;
                TextRenderer.DrawText(e.Graphics, e.SubItem.Text, lv.Font, Rectangle.Inflate(e.Bounds, -S(6), 0), fore, flags);
                if (e.ColumnIndex == 0 && e.Item.Focused && lv.Focused)
                    using (var pen = new Pen(Focus)) e.Graphics.DrawRectangle(pen, e.Item.Bounds.X, e.Item.Bounds.Y, e.Item.Bounds.Width - 1, e.Item.Bounds.Height - 1);
            };
            lv.MouseMove += (s, e) => { var item = lv.GetItemAt(e.X, e.Y); if (item != null) lv.Invalidate(item.Bounds); };
            return lv;
        }
    }

    // The widgets. Every interactive control gets an accessible name (G-A11Y) and a visible focus border.
    static class Ui
    {
        public static Label Label(string text, Font font = null, Color? color = null, int width = 0)
        {
            var l = new Label { Text = text, AutoSize = true, Font = font ?? Theme.Body, ForeColor = color ?? Theme.Text, Margin = Theme.Pad(0, 3, 0, 3), UseMnemonic = false, BackColor = Color.Transparent };
            if (width > 0) l.MaximumSize = new Size(width, 0);
            return l;
        }

        public static Label Dim(string text, int width) { return Label(text, null, Theme.Dim, width); }

        static void FocusCue(ButtonBase b, Color border)
        {
            b.GotFocus += (s, e) => { if (b is Button) ((Button)b).FlatAppearance.BorderColor = Theme.Focus; ((ButtonBase)s).FlatAppearance.BorderSize = 2; };
            b.LostFocus += (s, e) => { if (b is Button) ((Button)b).FlatAppearance.BorderColor = border; ((ButtonBase)s).FlatAppearance.BorderSize = 1; };
        }

        public static Button Button(string text, EventHandler click, bool primary = false)
        {
            var b = new Button
            {
                Text = text, AutoSize = true, AutoSizeMode = AutoSizeMode.GrowAndShrink, FlatStyle = FlatStyle.Flat, ForeColor = Theme.Text, Font = Theme.Bold,
                BackColor = primary ? Theme.Accent : Theme.CardHi, Padding = Theme.Pad(10, 4, 10, 4), Margin = Theme.Pad(0, 6, 10, 6), Cursor = Cursors.Hand,
                AccessibleName = text.TrimEnd('.', '…'), UseMnemonic = false,
            };
            var border = primary ? Theme.Accent : Theme.Line;
            b.FlatAppearance.BorderColor = border;
            if (primary) b.EnabledChanged += (s, e) => { b.BackColor = b.Enabled ? Theme.Accent : Theme.CardHi; };
            FocusCue(b, border);
            if (click != null) b.Click += click;
            return b;
        }

        // A check box that stays readable: never disabled (a disabled box draws etched text on the dark theme).
        public static CheckBox Check(string text, int width = 0)
        {
            var c = new CheckBox { Text = text, AutoSize = true, ForeColor = Theme.Text, BackColor = Color.Transparent, Font = Theme.Bold, Margin = Theme.Pad(0, 6, 0, 0), UseVisualStyleBackColor = false, AccessibleName = text, UseMnemonic = false };
            if (width > 0) c.MaximumSize = new Size(width, 0);
            return c;
        }

        public static RadioButton Radio(string text, int width = 0)
        {
            var r = new RadioButton { Text = text, AutoSize = true, ForeColor = Theme.Text, BackColor = Color.Transparent, Font = Theme.Bold, Margin = Theme.Pad(0, 4, 18, 0), UseVisualStyleBackColor = false, AccessibleName = text, UseMnemonic = false };
            if (width > 0) r.MaximumSize = new Size(width, 0);
            return r;
        }

        // The "?" next to an option (WU-064): its explanation opens in the side panel.
        public static Button Explain(string settingName, Action open)
        {
            var b = new Button
            {
                Text = "?", Size = Theme.Sz(26, 26), FlatStyle = FlatStyle.Flat, ForeColor = Theme.Teal, BackColor = Theme.Card, Font = Theme.Bold,
                Margin = Theme.Pad(8, 4, 0, 0), Cursor = Cursors.Hand, AccessibleName = Strings.T("ui.explain", settingName), TabStop = true,
            };
            b.FlatAppearance.BorderColor = Theme.Line;
            FocusCue(b, Theme.Line);
            b.Click += (s, e) => open();
            return b;
        }

        public static FlowLayoutPanel Row(params Control[] controls)
        {
            var r = new FlowLayoutPanel { AutoSize = true, AutoSizeMode = AutoSizeMode.GrowAndShrink, WrapContents = false, Margin = Theme.Pad(0), BackColor = Color.Transparent };
            foreach (var c in controls) if (c != null) r.Controls.Add(c);
            return r;
        }

        public static FlowLayoutPanel WrapRow(int width, params Control[] controls)
        {
            var r = new FlowLayoutPanel { AutoSize = true, AutoSizeMode = AutoSizeMode.GrowAndShrink, WrapContents = true, Margin = Theme.Pad(0), BackColor = Color.Transparent, MaximumSize = new Size(width, 0) };
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

    // A titled block of a page; Inner is the width its content may use.
    sealed class CardPanel : Panel
    {
        public readonly FlowLayoutPanel Body;
        public readonly int Inner;

        public CardPanel(string title, int width, Color? back = null)
        {
            BackColor = back ?? Theme.Card; AutoSize = true; AutoSizeMode = AutoSizeMode.GrowAndShrink; Padding = Theme.Pad(16, 12, 16, 14);
            Margin = Theme.Pad(0, 0, 0, 14);
            MinimumSize = new Size(width, 0); MaximumSize = new Size(width, 0);
            Inner = width - Padding.Horizontal;
            Body = Ui.Stack(Inner);
            Body.Dock = DockStyle.Fill;
            if (title != null) Body.Controls.Add(Ui.Label(title, Theme.CardTitle, null, Inner));
            Controls.Add(Body);
        }

        public T Add<T>(T c) where T : Control { Body.Controls.Add(c); return c; }

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

    // The clock ceiling as "-  1500 MHz  +": one 100 MHz step per click, held inside the 1000-2000 MHz grid.
    sealed class CeilingPicker : FlowLayoutPanel
    {
        readonly Label _value;
        int _index = -1;
        public event EventHandler Stepped;

        public CeilingPicker()
        {
            AutoSize = true; WrapContents = false; Margin = Theme.Pad(0); BackColor = Color.Transparent;
            Controls.Add(Step("-", -1, Strings.T("ui.ceiling.lower")));
            _value = new Label
            {
                // Grows with the text scale (WU-069): a fixed width cut "MHz" off at 150 % text.
                AutoSize = true, MinimumSize = Theme.Sz(96, 30), Padding = Theme.Pad(6, 0, 6, 0), TextAlign = ContentAlignment.MiddleCenter, Font = Theme.Body,
                ForeColor = Theme.Text, BackColor = Theme.Nav, Margin = Theme.Pad(0, 6, 0, 6), AccessibleName = Strings.T("search.graphics.clock-ceiling"),
            };
            Controls.Add(_value);
            Controls.Add(Step("+", 1, Strings.T("ui.ceiling.higher")));
            Index = Array.IndexOf(DpmSettings.CeilingChoices, DpmSettings.DefaultMaxMHz);
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
            set { _index = Math.Max(0, Math.Min(DpmSettings.CeilingChoices.Length - 1, value)); _value.Text = Value + " MHz"; _value.AccessibleDescription = _value.Text; }
        }

        public uint Value { get { return DpmSettings.CeilingChoices[_index]; } set { var i = Array.IndexOf(DpmSettings.CeilingChoices, value); Index = i >= 0 ? i : Array.IndexOf(DpmSettings.CeilingChoices, DpmSettings.DefaultMaxMHz); } }
    }
}
