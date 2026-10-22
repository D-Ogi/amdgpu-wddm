// The Nagi logo, the "horizon" mark: a ring, a calm sea line and an amber sun (the project's Nagi logo,
// brand sheet of 2026-10-13). It is drawn here from its geometry on a 100 x 100 box, so it is sharp at every scale and
// the exe carries no picture of it. The application icon stays the guide character's face (art\icon\nagi.ico).
//   ring  circle (50, 50) r 40, stroke #f2d27a, width 6
//   sun   circle (61, 36) r 7.5, fill #e39a3b
//   sea   path M20 58 C 30 51.5, 40 51.5, 50 58 S 70 64.5, 80 58, stroke #52d6e6, width 6
//   line  M32 71 H 68, stroke #52d6e6, width 4, opacity 0.55
//   round caps and joins
// The ring is light: on a light background (high contrast) the mark sits on a dark disc (#07090d), so it stays
// visible. setup's Chrome.cs draws the same mark in the title bar of the setup window.
using System;
using System.Drawing;
using System.Drawing.Drawing2D;
using System.Windows.Forms;

namespace AmdgpuWddmControl
{
    public static class NagiMark
    {
        static readonly Color Ring = Color.FromArgb(0xF2, 0xD2, 0x7A), Sun = Color.FromArgb(0xE3, 0x9A, 0x3B),
            Sea = Color.FromArgb(0x52, 0xD6, 0xE6), Disc = Color.FromArgb(0x07, 0x09, 0x0D);

        // Whether the mark needs its dark disc on this background.
        public static bool NeedsDisc(Color background) { return background.GetBrightness() > 0.5f; }

        public static void Draw(Graphics g, Rectangle box, Color background)
        {
            var state = g.Save();
            try
            {
                g.SmoothingMode = SmoothingMode.AntiAlias;
                float u = Math.Min(box.Width, box.Height) / 100f;
                g.TranslateTransform(box.X + (box.Width - 100 * u) / 2, box.Y + (box.Height - 100 * u) / 2);
                g.ScaleTransform(u, u);
                if (NeedsDisc(background)) using (var b = new SolidBrush(Disc)) g.FillEllipse(b, 3, 3, 94, 94);
                using (var ring = Stroke(Ring, 6)) g.DrawEllipse(ring, 10, 10, 80, 80);
                using (var sun = new SolidBrush(Sun)) g.FillEllipse(sun, 61 - 7.5f, 36 - 7.5f, 15, 15);
                using (var sea = Stroke(Sea, 6))
                using (var path = new GraphicsPath())
                {
                    path.AddBezier(20, 58, 30, 51.5f, 40, 51.5f, 50, 58);
                    // "S 70 64.5, 80 58": the first control point is (40, 51.5) mirrored about (50, 58).
                    path.AddBezier(50, 58, 60, 64.5f, 70, 64.5f, 80, 58);
                    g.DrawPath(sea, path);
                }
                using (var line = Stroke(Color.FromArgb(140, Sea), 4)) g.DrawLine(line, 32, 71, 68, 71);
            }
            finally { g.Restore(state); }
        }

        static Pen Stroke(Color c, float width)
        {
            return new Pen(c, width) { StartCap = LineCap.Round, EndCap = LineCap.Round, LineJoin = LineJoin.Round };
        }

        // The first colour that is not transparent, from the control up through its parents.
        public static Color BackgroundOf(Control c)
        {
            for (var p = c; p != null; p = p.Parent) if (p.BackColor.A == 255) return p.BackColor;
            return Theme.Back;
        }
    }

    // The mark as a control: decorative (no accessible role, no tab stop, no text), and it fills its own client
    // rectangle with the background it sits on (G-BACKDROP).
    public sealed class NagiMarkBox : Control
    {
        public NagiMarkBox(int size)
        {
            SetStyle(ControlStyles.AllPaintingInWmPaint | ControlStyles.OptimizedDoubleBuffer | ControlStyles.UserPaint | ControlStyles.ResizeRedraw, true);
            SetStyle(ControlStyles.Selectable, false);
            Size = new Size(size, size);
            TabStop = false;
            AccessibleRole = AccessibleRole.None;
            AccessibleName = "";
        }

        protected override void OnPaint(PaintEventArgs e)
        {
            var back = NagiMark.BackgroundOf(Parent);
            using (var b = new SolidBrush(back)) e.Graphics.FillRectangle(b, ClientRectangle);
            NagiMark.Draw(e.Graphics, ClientRectangle, back);
        }
    }
}
