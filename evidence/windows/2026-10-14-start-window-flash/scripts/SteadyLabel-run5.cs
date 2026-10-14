// A label for text that a timer changes while the person looks at it: the countdown of the display dialog, the result
// line of the sign-in notice, the live rows of the Performance, fan and tuning pages, the benchmark state and the
// guide panel (Ui.LiveLabel, Ui.CardPanel.LivePair, ThemedDialog.LiveLine).
//
// Why not a Label. On the lab the start window's Label (a window of its own on top of the form, with a fixed size)
// left a picture of navy with only part of the text in the window's own surface at 125 of 150 text changes, for
// about 2 ms each, and DWM showed it at 10 of 89 changes (fact M898, evidence/windows/2026-10-14-start-window-flash).
// The managed paint of a Label is not that picture: Control paints only in WM_PAINT (AllPaintingInWmPaint, so
// WM_ERASEBKGND draws nothing), and Label draws background and text into one buffer (OptimizedDoubleBuffer). What a
// Label has besides is its window: WinForms makes it a STATIC window with SS_OWNERDRAW (WindowsForms10.STATIC.*), and
// the STATIC window procedure handles WM_SETTEXT itself, outside the WM_PAINT that the Label buffers. The start
// window's fix was one window that paints itself and nothing else (start-window.ps1, 84f35234). This is the same fix
// for the control application.
//
// What it is. The Label itself, so its paint and its size rules are Label's own and the pixels are the same, in a
// window of the WinForms class instead of STATIC (CreateParams). Its only paint is the WM_PAINT of WinForms: background
// and text go into one buffer, which is copied to the window in one operation. A text set to the value it already has
// changes nothing and invalidates nothing. --smoke-steady checks all of this against a Label without a window on the
// screen (SteadyLabelCheck below).
using System;
using System.Drawing;
using System.Drawing.Imaging;
using System.Runtime.InteropServices;
using System.Text;
using System.Windows.Forms;

namespace AmdgpuWddmControl
{
    sealed class SteadyLabel : Label
    {
        const int SsTypeMask = 0x1F;   // the SS_ type field of a STATIC style (SS_OWNERDRAW 0x0D): no meaning outside STATIC

        public SteadyLabel()
        {
            SetStyle(ControlStyles.UserPaint | ControlStyles.AllPaintingInWmPaint | ControlStyles.OptimizedDoubleBuffer, true);
        }

        protected override CreateParams CreateParams
        {
            get
            {
                var cp = base.CreateParams;
                cp.ClassName = null;
                cp.Style &= ~SsTypeMask;
                return cp;
            }
        }

        public override string Text
        {
            get { return base.Text; }
            set { if (!string.Equals(value ?? "", base.Text, StringComparison.Ordinal)) base.Text = value; }
        }
    }

    // --smoke-steady: a SteadyLabel against a Label built with the same arguments, in every language, at 96, 144 and
    // 192 DPI, on a form that is never shown. An empty result is a pass.
    static class SteadyLabelCheck
    {
        [DllImport("user32.dll", CharSet = CharSet.Unicode)]
        static extern int GetClassName(IntPtr hwnd, StringBuilder name, int size);

        [DllImport("user32.dll")]
        static extern IntPtr SendMessage(IntPtr hwnd, int msg, IntPtr wParam, IntPtr lParam);

        const int WM_ERASEBKGND = 0x0014;
        static readonly Color Sentinel = Color.FromArgb(255, 0, 255);

        public static string Findings(out int cases)
        {
            var w = new StringBuilder();
            cases = 0;
            string language = Strings.Language;
            try
            {
                foreach (var lang in Strings.Languages)
                    foreach (var scale in new[] { 1f, 1.5f, 2f })
                    {
                        Strings.Language = lang;
                        Theme.Init(scale, 1f);
                        var texts = new[] { "", "61 °C", "1450 MHz, 812 mV", "A & B", Strings.T("display.keep.text", 12),
                            "A line long enough to wrap inside the narrow width of the test, and then once more, with numbers 0123456789." };
                        foreach (var text in texts)
                            foreach (var width in new[] { 0, Theme.S(140), Theme.S(420) })
                                foreach (var font in new[] { Theme.Body, Theme.Bold, Theme.CardTitle })
                                {
                                    cases++;
                                    Compare(w, lang + " " + scale + "x '" + text + "' width " + width + " " + font.Name, text, font, width);
                                }
                    }
            }
            finally
            {
                Strings.Language = language;
                Theme.Init(1f, 1f);
            }
            return w.ToString();
        }

        static void Compare(StringBuilder w, string name, string text, Font font, int width)
        {
            using (var form = new Form { ShowInTaskbar = false, FormBorderStyle = FormBorderStyle.None, BackColor = Theme.Back })
            {
                var panel = new Panel { BackColor = Theme.Card, Size = Theme.Sz(600, 400), Location = Point.Empty };
                form.Controls.Add(panel);
                Label plain = Ui.Label(text, font, Theme.Teal, width);
                SteadyLabel steady = Ui.LiveLabel(text, font, Theme.Teal, width);
                plain.Location = new Point(Theme.S(10), Theme.S(10));
                steady.Location = new Point(Theme.S(10), Theme.S(200));
                panel.Controls.Add(plain);
                panel.Controls.Add(steady);
                var unused = form.Handle;
                unused = plain.Handle;
                unused = steady.Handle;
                form.PerformLayout();

                if (plain.Size != steady.Size) w.AppendLine(name + ": size " + steady.Size + ", a Label has " + plain.Size);
                string plainClass = ClassOf(plain.Handle), steadyClass = ClassOf(steady.Handle);
                if (plainClass.IndexOf("STATIC", StringComparison.OrdinalIgnoreCase) < 0)
                    w.AppendLine(name + ": the Label's window class is '" + plainClass + "', not a STATIC: this check no longer sees the difference");
                if (steadyClass.IndexOf("STATIC", StringComparison.OrdinalIgnoreCase) >= 0)
                    w.AppendLine(name + ": the window class is '" + steadyClass + "'");
                if (steady.Width > 0 && steady.Height > 0)
                {
                    int steadyErase = Erased(steady);
                    if (steadyErase != 0) w.AppendLine(name + ": WM_ERASEBKGND drew " + steadyErase + " pixels");
                    int differ = Differ(plain, steady);
                    if (differ != 0) w.AppendLine(name + ": " + differ + " pixels differ from a Label");
                }
                int invalidations = 0;
                steady.Invalidated += (s, e) => invalidations++;
                steady.Text = text;
                if (invalidations != 0) w.AppendLine(name + ": the same text invalidated the window " + invalidations + " time(s)");
                steady.Text = text + "1";
                if (invalidations == 0) w.AppendLine(name + ": a new text did not invalidate the window");
                if (steady.Text != text + "1") w.AppendLine(name + ": the new text was not kept");
            }
        }

        static string ClassOf(IntPtr hwnd)
        {
            var b = new StringBuilder(256);
            GetClassName(hwnd, b, b.Capacity);
            return b.ToString();
        }

        // The pixels that WM_ERASEBKGND draws into a bitmap of the sentinel colour.
        static int Erased(Control c)
        {
            using (var bmp = new Bitmap(c.Width, c.Height, PixelFormat.Format32bppRgb))
            {
                using (var g = Graphics.FromImage(bmp))
                {
                    g.Clear(Sentinel);
                    IntPtr hdc = g.GetHdc();
                    try { SendMessage(c.Handle, WM_ERASEBKGND, hdc, IntPtr.Zero); }
                    finally { g.ReleaseHdc(hdc); }
                }
                int n = 0, sentinel = Sentinel.ToArgb() & 0xFFFFFF;
                foreach (int px in Pixels(bmp)) if ((px & 0xFFFFFF) != sentinel) n++;
                return n;
            }
        }

        static int Differ(Control a, Control b)
        {
            using (var ba = new Bitmap(a.Width, a.Height, PixelFormat.Format32bppRgb))
            using (var bb = new Bitmap(b.Width, b.Height, PixelFormat.Format32bppRgb))
            {
                a.DrawToBitmap(ba, new Rectangle(Point.Empty, a.Size));
                b.DrawToBitmap(bb, new Rectangle(Point.Empty, b.Size));
                int[] pa = Pixels(ba), pb = Pixels(bb);
                int n = 0;
                for (int i = 0; i < pa.Length; i++) if ((pa[i] & 0xFFFFFF) != (pb[i] & 0xFFFFFF)) n++;
                return n;
            }
        }

        static int[] Pixels(Bitmap bmp)
        {
            var data = bmp.LockBits(new Rectangle(Point.Empty, bmp.Size), ImageLockMode.ReadOnly, PixelFormat.Format32bppRgb);
            try
            {
                var px = new int[bmp.Width * bmp.Height];
                for (int y = 0; y < bmp.Height; y++)
                    Marshal.Copy(data.Scan0 + y * data.Stride, px, y * bmp.Width, bmp.Width);
                return px;
            }
            finally { bmp.UnlockBits(data); }
        }
    }
}
