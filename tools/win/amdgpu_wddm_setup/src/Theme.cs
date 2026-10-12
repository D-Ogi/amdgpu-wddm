// The design tokens of the amdgpu-wddm windows: colours, radii, spacing, fonts, icon glyphs, and the GDI+ helpers that
// draw them. One file, so that the control application can adopt the same language later (its Ui.cs holds the older
// copy). The process is per-monitor DPI aware (app.manifest) and the forms do no automatic scaling, so every pixel
// size goes through S() and the fonts are sized in pixels with the same factor; the text scale (Windows "Make text
// bigger") multiplies the fonts only. Fonts follow the language: Yu Gothic UI for Japanese, Malgun Gothic for Korean.
// High contrast replaces the palette with the system colours.
using System;
using System.Collections.Generic;
using System.Drawing;
using System.Drawing.Drawing2D;
using System.Drawing.Text;
using System.Linq;
using System.Runtime.InteropServices;
using System.Windows.Forms;

namespace AmdgpuWddmSetup
{
    // The icon glyphs of the window, in the Segoe Fluent Icons code points (Segoe MDL2 Assets carries the same ones).
    // Every glyph a screen uses is listed here, so one place says what the window can draw; Gfx.Glyph draws a plain
    // shape instead when neither icon font is on the PC.
    static class Icons
    {
        public const string Check = "";        // CheckMark
        public const string Warning = "";      // Warning
        public const string Error = "";        // ErrorBadge
        public const string Info = "";         // Info
        public const string Cancel = "";       // Cancel
        public const string Restart = "";      // UpdateRestore (restart of Windows)
        public const string Download = "";     // Download
        public const string Shield = "";       // Shield
        public const string Gear = "";         // Settings
        public const string Folder = "";       // FolderHorizontal
        public const string Play = "";         // Play (the step that runs)
        public const string Dot = "";          // CircleRing
        // Two marks the window draws itself, because no icon font says them as quietly as a page needs: the ring of a
        // step that waits and the dot of a list item (Gfx.Glyph draws every mark that starts with \u0001).
        public const string Ring = "\u0001ring";
        public const string Bullet = "\u0001bullet";
        public const string Chevron = "";      // ChevronRight
        public const string Minimise = "";     // ChromeMinimize
        public const string Close = "";        // ChromeClose
        public const string Chip = "";         // Processor (the driver itself)
        public const string Lock = "";         // Lock (nothing changes without consent)
        public const string Sparkle = "";      // FavoriteStar (what is new)
        public const string Clock = "";        // DateTime (elapsed time)
        public const string Welcome = "";      // Home
        // The last step of the rail. It is not a check mark: a check is the mark of a step that is done, and
        // the same mark on a step that still waits reads as "already finished".
        public const string Flag = "";         // Flag (the end of the run)
    }

    static class Theme
    {
        // ---- palette ---------------------------------------------------------------------------------------------
        // Dark by design (the accepted mockup). HighContrast swaps every colour for a system one.
        public static Color Back { get { return _hc ? SystemColors.Window : Rgb(0x10, 0x12, 0x17); } }
        public static Color Nav { get { return _hc ? SystemColors.Control : Rgb(0x0A, 0x0C, 0x10); } }
        public static Color Card { get { return _hc ? SystemColors.Control : Rgb(0x1A, 0x1D, 0x25); } }
        public static Color CardHi { get { return _hc ? SystemColors.ControlLight : Rgb(0x24, 0x28, 0x33); } }
        public static Color Text { get { return _hc ? SystemColors.WindowText : Rgb(0xEC, 0xEE, 0xF2); } }
        public static Color Dim { get { return _hc ? SystemColors.GrayText : Rgb(0xA2, 0xA8, 0xB4); } }
        public static Color Accent { get { return _hc ? SystemColors.Highlight : Rgb(0xE2, 0x26, 0x3E); } }
        public static Color AccentHi { get { return _hc ? SystemColors.HotTrack : Rgb(0xF2, 0x44, 0x5E); } }
        public static Color AccentLow { get { return _hc ? SystemColors.Highlight : Rgb(0xB4, 0x16, 0x2C); } }
        public static Color Teal { get { return _hc ? SystemColors.HotTrack : Rgb(0x40, 0xCC, 0xD6); } }
        public static Color Good { get { return _hc ? SystemColors.WindowText : Rgb(0x4F, 0xC7, 0x7B); } }
        public static Color Warn { get { return _hc ? SystemColors.WindowText : Rgb(0xF0, 0xB4, 0x3C); } }
        public static Color Line { get { return _hc ? SystemColors.WindowFrame : Rgb(0x30, 0x35, 0x41); } }
        public static Color Focus { get { return _hc ? SystemColors.WindowText : Rgb(0x78, 0xC8, 0xFF); } }
        public static Color GuideBack { get { return _hc ? SystemColors.Control : Rgb(0x10, 0x26, 0x2D); } }
        // The tint of the step the window is on, and the hover tint of a row or a quiet button.
        public static Color Tint { get { return _hc ? SystemColors.Highlight : Rgb(0x2A, 0x14, 0x1C); } }
        // The tint of the step a run stopped in: deeper, so the stopped step is not only a colour of a glyph.
        public static Color TintBad { get { return _hc ? SystemColors.Highlight : Rgb(0x45, 0x12, 0x1E); } }
        // What is drawn on a tinted field. In high contrast a tint is Highlight, and Highlight carries HighlightText
        // alone: WindowText on Highlight is the classic high-contrast failure.
        public static Color OnTint { get { return _hc ? SystemColors.HighlightText : Rgb(0xEC, 0xEE, 0xF2); } }

        static Color Rgb(int r, int g, int b) { return Color.FromArgb(r, g, b); }

        static bool _hc;
        public static bool HighContrast { get { return _hc; } }

        // ---- shape and spacing -----------------------------------------------------------------------------------
        public const int RadiusCard = 8, RadiusControl = 6, RadiusSmall = 4;
        public const int GapTight = 6, Gap = 12, GapWide = 20, GapSection = 16;

        public static float Scale { get; private set; }
        public static float TextScale { get; private set; }

        // Body and Bold are the running text, Small the notes under a row, CardTitle the title of a card, Title a page
        // headline (28-32 px at 96 DPI), Brand the name in the title bar, Big a number or a result word.
        public static Font Body, Bold, Small, SmallBold, Title, CardTitle, Brand, Big;

        // Windows animation effects: the short screen transitions and the entrance of the result icon run only with
        // them on. A headless render never animates (the window is never shown).
        public static bool Animations
        {
            get { try { return SystemInformation.UIEffectsEnabled && !_hc; } catch (Exception) { return false; } }
        }

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

        // scale: 1.0 at 96 DPI; --smoke-render passes 1.25, 1.5 or 2 to draw the pages as a 120, 144 or 192 DPI screen
        // shows them. highContrast: this PC's setting when it is null, and the value itself when the caller forces it
        // (--high-contrast, so the render gate draws the high-contrast palette on a PC that does not have it on).
        public static void Init(float scale, float textScale, bool? highContrast = null)
        {
            Scale = scale <= 0 ? 1 : scale;
            TextScale = textScale < 1 ? 1 : textScale;
            if (highContrast.HasValue) _hc = highContrast.Value;
            else try { _hc = SystemInformation.HighContrast; } catch (Exception) { _hc = false; }
            Fonts();
        }

        // ---- fonts -----------------------------------------------------------------------------------------------

        static HashSet<string> _installed;

        // The first of these families that this PC has, else the last one (which Windows then replaces itself).
        public static string Face(params string[] names)
        {
            if (_installed == null)
            {
                try
                {
                    using (var c = new InstalledFontCollection()) _installed = new HashSet<string>(c.Families.Select(f => f.Name), StringComparer.OrdinalIgnoreCase);
                }
                catch (Exception) { _installed = new HashSet<string>(StringComparer.OrdinalIgnoreCase); }
            }
            foreach (var n in names) if (_installed.Contains(n)) return n;
            return names[names.Length - 1];
        }

        public static string IconFace { get; private set; }

        // The fonts of the generation before the last switch. They are not disposed while they are replaced: the
        // controls on the screen still hold them and would throw while painting. The window disposes them after it
        // has rebuilt its frame (DropRetired), one generation behind.
        static readonly List<Font> _retired = new List<Font>();

        public static void DropRetired()
        {
            foreach (var f in _retired) { try { f.Dispose(); } catch (Exception) { } }
            _retired.Clear();
        }

        static void Retire(params Font[] fonts)
        {
            foreach (var f in fonts) if (f != null) _retired.Add(f);
        }

        public static void Fonts()
        {
            string lang = Strings.Language;
            // Every font this call replaces goes to the retired list, including the cached icon fonts.
            Retire(Body, Bold, Small, SmallBold, Title, CardTitle, Brand, Big);
            Retire(_icons.Values.ToArray());
            // Japanese and Korean have no Segoe: their own interface faces carry every Latin character too.
            string face = lang == "ja" ? Face("Yu Gothic UI", "Meiryo UI", "Segoe UI") : lang == "ko" ? Face("Malgun Gothic", "Segoe UI") : Face("Segoe UI");
            string semi = lang == "ja" ? Face("Yu Gothic UI Semibold", "Yu Gothic UI") : lang == "ko" ? Face("Malgun Gothic") : Face("Segoe UI Semibold", "Segoe UI");
            // The headline face of Windows 11. Korean has no semibold family, so its headline is drawn bold.
            string display = lang == "ja" ? Face("Yu Gothic UI Semibold", "Yu Gothic UI") : lang == "ko" ? Face("Malgun Gothic")
                : Face("Segoe UI Variable Display Semib", "Segoe UI Semibold", "Segoe UI");
            bool heavy = lang == "ko";
            Body = Px(face, 9.5f); Bold = Px(semi, 9.5f, heavy); Small = Px(face, 8.5f); SmallBold = Px(semi, 8.5f, heavy);
            Title = Px(display, 22f, heavy); CardTitle = Px(semi, 11.5f, heavy); Big = Px(display, 15f, heavy);
            Brand = Px(lang == "ja" || lang == "ko" ? semi : Face("Segoe UI Variable Display Semib", "Segoe UI Semibold"), 13.5f, heavy);
            IconFace = Face("Segoe Fluent Icons", "Segoe MDL2 Assets", NoFont);
            HasIconFont = IconFace != NoFont;
            if (!HasIconFont) IconFace = "Segoe UI";
            _icons.Clear();
        }

        static Font Px(string name, float points, bool bold = false)
        {
            return new Font(name, points * 96f / 72f * Scale * TextScale, bold ? FontStyle.Bold : FontStyle.Regular, GraphicsUnit.Pixel);
        }

        static readonly Dictionary<int, Font> _icons = new Dictionary<int, Font>();

        // An icon font of a height in device pixels (the caller has scaled it already).
        public static Font Icon(int px)
        {
            Font f;
            if (_icons.TryGetValue(px, out f)) return f;
            f = new Font(IconFace, Math.Max(6, px), FontStyle.Regular, GraphicsUnit.Pixel);
            _icons[px] = f;
            return f;
        }

        // Windows 11 has Segoe Fluent Icons, Windows 10 has Segoe MDL2 Assets. Without either one the window draws
        // plain shapes (Gfx.Glyph), so a PC that lost its icon font still reads.
        const string NoFont = "\u0001no-icon-font";
        public static bool HasIconFont { get; private set; }

        public static int S(int px) { return (int)Math.Round(px * Scale); }
        public static Size Sz(int w, int h) { return new Size(S(w), S(h)); }
        public static Padding Pad(int all) { return new Padding(S(all)); }
        public static Padding Pad(int left, int top, int right, int bottom) { return new Padding(S(left), S(top), S(right), S(bottom)); }

        [DllImport("uxtheme.dll", CharSet = CharSet.Unicode)]
        static extern int SetWindowTheme(IntPtr hwnd, string appName, string idList);

        // Dark scroll bars on a scrolling control (the Explorer dark theme of Windows 10 1809 and later).
        public static T DarkScroll<T>(T c) where T : Control
        {
            c.HandleCreated += (s, e) => { try { SetWindowTheme(c.Handle, "DarkMode_Explorer", null); } catch (Exception) { } };
            return c;
        }
    }

    // The drawing helpers: rounded shapes, a soft elevation and the icon glyphs.
    static class Gfx
    {
        public static GraphicsPath Round(Rectangle r, int radius)
        {
            var p = new GraphicsPath();
            int d = Math.Max(1, radius * 2);
            if (r.Width <= d || r.Height <= d) { p.AddRectangle(r); return p; }
            p.AddArc(r.X, r.Y, d, d, 180, 90);
            p.AddArc(r.Right - d - 1, r.Y, d, d, 270, 90);
            p.AddArc(r.Right - d - 1, r.Bottom - d - 1, d, d, 0, 90);
            p.AddArc(r.X, r.Bottom - d - 1, d, d, 90, 90);
            p.CloseFigure();
            return p;
        }

        public static void Fill(Graphics g, Rectangle r, int radius, Color fill)
        {
            var old = g.SmoothingMode;
            g.SmoothingMode = SmoothingMode.AntiAlias;
            using (var p = Round(r, radius))
            using (var b = new SolidBrush(fill)) g.FillPath(b, p);
            g.SmoothingMode = old;
        }

        public static void FillGradient(Graphics g, Rectangle r, int radius, Color top, Color bottom)
        {
            if (r.Width <= 0 || r.Height <= 0) return;
            var old = g.SmoothingMode;
            g.SmoothingMode = SmoothingMode.AntiAlias;
            using (var p = Round(r, radius))
            using (var b = new LinearGradientBrush(new Rectangle(r.X, r.Y, r.Width, r.Height + 1), top, bottom, LinearGradientMode.Vertical))
                g.FillPath(b, p);
            g.SmoothingMode = old;
        }

        public static void Border(Graphics g, Rectangle r, int radius, Color line, int width = 1)
        {
            var old = g.SmoothingMode;
            g.SmoothingMode = SmoothingMode.AntiAlias;
            using (var p = Round(new Rectangle(r.X, r.Y, r.Width - 1, r.Height - 1), radius))
            using (var pen = new Pen(line, width)) g.DrawPath(pen, p);
            g.SmoothingMode = old;
        }

        // A card: the fill, a 1 px border and a soft shadow in the space below it (reserve "depth" pixels there).
        public static void Card(Graphics g, Rectangle r, int radius, Color fill, Color line, int depth)
        {
            for (int i = depth; i > 0; i--)
            {
                var s = new Rectangle(r.X + i / 2, r.Y + i, r.Width - i, r.Height);
                using (var p = Round(s, radius))
                using (var b = new SolidBrush(Color.FromArgb(Math.Max(6, 26 - i * 6), 0, 0, 0)))
                {
                    var old = g.SmoothingMode;
                    g.SmoothingMode = SmoothingMode.AntiAlias;
                    g.FillPath(b, p);
                    g.SmoothingMode = old;
                }
            }
            Fill(g, r, radius, fill);
            Border(g, r, radius, line);
        }

        // One glyph centred in a square. Without an icon font on the PC, a plain shape says the same.
        public static void Glyph(Graphics g, string glyph, Rectangle box, Color color, int size = 0)
        {
            if (box.Width <= 0 || box.Height <= 0) return;
            int px = size > 0 ? size : (int)(Math.Min(box.Width, box.Height) * 0.82f);
            if (Theme.HasIconFont && !string.IsNullOrEmpty(glyph) && glyph[0] != '\u0001')
            {
                var old = g.TextRenderingHint;
                g.TextRenderingHint = TextRenderingHint.ClearTypeGridFit;
                TextRenderer.DrawText(g, glyph, Theme.Icon(px), box, color,
                    TextFormatFlags.HorizontalCenter | TextFormatFlags.VerticalCenter | TextFormatFlags.NoPrefix | TextFormatFlags.SingleLine | TextFormatFlags.NoPadding);
                g.TextRenderingHint = old;
                return;
            }
            Shape(g, glyph, box, color);
        }

        // The shapes that stand in for the glyphs that carry a state.
        static void Shape(Graphics g, string glyph, Rectangle box, Color color)
        {
            var old = g.SmoothingMode;
            g.SmoothingMode = SmoothingMode.AntiAlias;
            float share = glyph == Icons.Ring ? 0.5f : glyph == Icons.Bullet ? 0.3f : 0.7f;
            int d = Math.Max(2, (int)(Math.Min(box.Width, box.Height) * share));
            var r = new Rectangle(box.X + (box.Width - d) / 2, box.Y + (box.Height - d) / 2, d, d);
            using (var pen = new Pen(color, Math.Max(1.5f, d / 8f)))
            {
                if (glyph == Icons.Ring) { g.DrawEllipse(pen, r); g.SmoothingMode = old; return; }
                if (glyph == Icons.Bullet) { using (var b = new SolidBrush(color)) g.FillEllipse(b, r); g.SmoothingMode = old; return; }
                if (glyph == Icons.Check)
                    g.DrawLines(pen, new[] { new Point(r.Left, r.Top + r.Height / 2), new Point(r.Left + r.Width / 3, r.Bottom - r.Height / 6), new Point(r.Right, r.Top + r.Height / 6) });
                else if (glyph == Icons.Cancel || glyph == Icons.Error)
                {
                    g.DrawLine(pen, r.Left, r.Top, r.Right, r.Bottom);
                    g.DrawLine(pen, r.Right, r.Top, r.Left, r.Bottom);
                }
                else if (glyph == Icons.Chevron)
                    g.DrawLines(pen, new[] { new Point(r.Left + r.Width / 4, r.Top), new Point(r.Right - r.Width / 4, r.Top + r.Height / 2), new Point(r.Left + r.Width / 4, r.Bottom) });
                else if (glyph == Icons.Dot)
                    g.DrawEllipse(pen, r);
                else
                    using (var b = new SolidBrush(color)) g.FillEllipse(b, r);
            }
            g.SmoothingMode = old;
        }

        // A round status badge: a filled circle of the state colour with its glyph in the window's background colour.
        public static void Badge(Graphics g, Rectangle box, Color color, string glyph)
        {
            var old = g.SmoothingMode;
            g.SmoothingMode = SmoothingMode.AntiAlias;
            using (var b = new SolidBrush(color)) g.FillEllipse(b, box);
            g.SmoothingMode = old;
            var inner = Rectangle.Inflate(box, -(int)(box.Width * 0.22f), -(int)(box.Height * 0.22f));
            Glyph(g, glyph, inner, Theme.HighContrast ? SystemColors.Window : Color.FromArgb(0x10, 0x12, 0x17));
        }

        public static void FocusRing(Graphics g, Rectangle r, int radius)
        {
            var old = g.SmoothingMode;
            g.SmoothingMode = SmoothingMode.AntiAlias;
            using (var p = Round(new Rectangle(r.X + 1, r.Y + 1, r.Width - 3, r.Height - 3), radius))
            using (var pen = new Pen(Theme.Focus, Math.Max(1, Theme.S(2))) { DashStyle = DashStyle.Solid }) g.DrawPath(pen, p);
            g.SmoothingMode = old;
        }

        public const TextFormatFlags Wrap = TextFormatFlags.WordBreak | TextFormatFlags.NoPrefix | TextFormatFlags.Left | TextFormatFlags.Top;

        public static Size Measure(string text, Font font, int width)
        {
            return TextRenderer.MeasureText(text ?? "", font, new Size(width, int.MaxValue), Wrap);
        }
    }
}
