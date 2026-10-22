// The Nagi logo, the "horizon" mark (the project's Nagi logo, brand sheet, 2026-10-13), drawn from its
// geometry: the same drawing as tools/win/amdgpu_wddm_control/src/NagiMark.cs, which lists the geometry. The title
// bar of the setup window (Chrome.cs) draws it beside the product name. The icon of the exe stays nagi.ico.
using System;
using System.Drawing;
using System.Drawing.Drawing2D;

namespace AmdgpuWddmSetup
{
    public static class NagiMark
    {
        static readonly Color Ring = Color.FromArgb(0xF2, 0xD2, 0x7A), Sun = Color.FromArgb(0xE3, 0x9A, 0x3B),
            Sea = Color.FromArgb(0x52, 0xD6, 0xE6), Disc = Color.FromArgb(0x07, 0x09, 0x0D);

        // Whether the mark needs its dark disc on this background: the ring is light, and a high-contrast theme can
        // give the title bar a light colour.
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
    }
}
