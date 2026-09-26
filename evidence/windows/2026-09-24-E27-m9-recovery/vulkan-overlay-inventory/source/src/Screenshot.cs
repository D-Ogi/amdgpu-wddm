// Screen and window capture: the only way the remote agent can see what the owner sees. Only this process
// can do it - the SSH session lives in session 0 and has no desktop. Every capture is logged by the caller
// with the source "screenshot", so the owner reads on the overlay that a picture was taken.
using System;
using System.Collections.Generic;
using System.Diagnostics;
using System.Drawing;
using System.Drawing.Drawing2D;
using System.Drawing.Imaging;
using System.IO;
using System.Linq;
using System.Runtime.InteropServices;
using System.Text;
using System.Threading;

namespace Bc250Mon
{
    public sealed class WindowInfo
    {
        public IntPtr Handle;
        public string Title, Process;
        public Rectangle Bounds;
        public bool Minimized;
    }

    public static class Screenshot
    {
        const int SM_CXSCREEN = 0, SM_CYSCREEN = 1;
        const int DWMWA_EXTENDED_FRAME_BOUNDS = 9, DWMWA_CLOAKED = 14;
        const int PW_RENDERFULLCONTENT = 2;
        const uint WDA_NONE = 0, WDA_EXCLUDEFROMCAPTURE = 0x11;

        delegate bool EnumWindowsProc(IntPtr hwnd, IntPtr param);

        struct RECT
        {
            public int Left, Top, Right, Bottom;
            public RECT(int l, int t, int r, int b) { Left = l; Top = t; Right = r; Bottom = b; }
            public Rectangle ToRectangle() { return Rectangle.FromLTRB(Left, Top, Right, Bottom); }
        }

        [DllImport("user32.dll")] static extern int GetSystemMetrics(int index);
        [DllImport("user32.dll")] static extern bool EnumWindows(EnumWindowsProc callback, IntPtr param);
        [DllImport("user32.dll")] static extern bool IsWindowVisible(IntPtr h);
        [DllImport("user32.dll")] static extern bool IsIconic(IntPtr h);
        [DllImport("user32.dll", CharSet = CharSet.Unicode)] static extern int GetWindowTextLength(IntPtr h);
        [DllImport("user32.dll", CharSet = CharSet.Unicode)] static extern int GetWindowText(IntPtr h, StringBuilder text, int max);
        [DllImport("user32.dll")] static extern bool GetWindowRect(IntPtr h, out RECT rect);
        [DllImport("user32.dll")] static extern int GetWindowThreadProcessId(IntPtr h, out int pid);
        [DllImport("user32.dll")] static extern bool PrintWindow(IntPtr h, IntPtr hdc, int flags);
        [DllImport("user32.dll")] static extern bool SetWindowDisplayAffinity(IntPtr h, uint affinity);
        [DllImport("dwmapi.dll")] static extern int DwmGetWindowAttribute(IntPtr h, int attribute, out RECT value, int size);
        [DllImport("dwmapi.dll")] static extern int DwmGetWindowAttribute(IntPtr h, int attribute, out int value, int size);
        [DllImport("dwmapi.dll")] static extern int DwmFlush();

        // Set once by OverlayForm. The API is built before the window exists, so this static is where the two
        // meet; it lets a capture leave our own overlay out.
        public static IntPtr OverlayWindow;

        // Takes the overlay out of the picture for the duration of a capture. WDA_EXCLUDEFROMCAPTURE (Windows
        // 10 2004 and later) keeps the window on the monitor and only removes it from the capture APIs, so the
        // owner sees no flicker. If it is refused, OverlayHidden stays false and the caller says so.
        public sealed class CaptureScope : IDisposable
        {
            readonly IntPtr _overlay;
            public bool OverlayHidden { get; private set; }

            public CaptureScope(bool includeOverlay)
            {
                _overlay = OverlayWindow;
                if (includeOverlay || _overlay == IntPtr.Zero) return;
                OverlayHidden = SetWindowDisplayAffinity(_overlay, WDA_EXCLUDEFROMCAPTURE);
                if (!OverlayHidden) return;
                DwmFlush();                     // wait for the first composed frame without the overlay in it
                Thread.Sleep(40);
            }

            public void Dispose() { if (OverlayHidden) SetWindowDisplayAffinity(_overlay, WDA_NONE); }
        }

        // The whole primary screen. The manifest declares dpiAware, so the metrics are physical pixels and
        // there is no scaling to undo.
        public static Bitmap CaptureScreen()
        {
            var size = new Size(GetSystemMetrics(SM_CXSCREEN), GetSystemMetrics(SM_CYSCREEN));
            var bmp = new Bitmap(size.Width, size.Height, PixelFormat.Format32bppRgb);
            using (var g = Graphics.FromImage(bmp))
                g.CopyFromScreen(0, 0, 0, 0, size, CopyPixelOperation.SourceCopy);
            return bmp;
        }

        // One window, whether or not it is on top. PrintWindow asks the window to draw itself, which also
        // catches DirectX and Chromium content with PW_RENDERFULLCONTENT; windows that refuse (or answer with
        // one flat colour) are copied from the screen instead, and then whatever covers them is in the image.
        public static Bitmap CaptureWindow(IntPtr hwnd, out string method)
        {
            RECT wr;
            if (!IsWindowVisible(hwnd) || !GetWindowRect(hwnd, out wr)) throw new ArgumentException("window is gone or not visible");
            Rectangle full = wr.ToRectangle(), frame = FrameBounds(hwnd, full);
            if (full.Width <= 0 || full.Height <= 0) throw new ArgumentException("window has no area (minimized?)");

            var bmp = new Bitmap(full.Width, full.Height, PixelFormat.Format32bppRgb);
            bool drawn;
            using (var g = Graphics.FromImage(bmp))
            {
                IntPtr hdc = g.GetHdc();
                try { drawn = PrintWindow(hwnd, hdc, PW_RENDERFULLCONTENT); }
                finally { g.ReleaseHdc(hdc); }
            }
            if (drawn && !LooksBlank(bmp))
            {
                method = "printwindow";
                // Cut off the invisible resize border GetWindowRect reports but the DWM does not draw.
                var inner = new Rectangle(frame.X - full.X, frame.Y - full.Y, frame.Width, frame.Height);
                inner.Intersect(new Rectangle(0, 0, bmp.Width, bmp.Height));
                if (inner.Width <= 0 || inner.Height <= 0) return bmp;
                using (bmp) return bmp.Clone(inner, PixelFormat.Format32bppRgb);
            }
            bmp.Dispose();

            method = "screen region";
            var region = new Bitmap(frame.Width, frame.Height, PixelFormat.Format32bppRgb);
            using (var g = Graphics.FromImage(region))
                g.CopyFromScreen(frame.X, frame.Y, 0, 0, frame.Size, CopyPixelOperation.SourceCopy);
            return region;
        }

        // Visible top-level windows that carry a title, in z-order: the first match of a title search is the
        // one nearest the front. Cloaked windows are the UWP ghosts Windows keeps alive off-screen.
        public static List<WindowInfo> Windows()
        {
            var found = new List<WindowInfo>();
            EnumWindowsProc callback = (h, param) =>
            {
                RECT wr;
                int cloaked;
                if (!IsWindowVisible(h) || GetWindowTextLength(h) == 0 || !GetWindowRect(h, out wr)) return true;
                if (DwmGetWindowAttribute(h, DWMWA_CLOAKED, out cloaked, sizeof(int)) == 0 && cloaked != 0) return true;
                var title = new StringBuilder(512);
                GetWindowText(h, title, title.Capacity);
                found.Add(new WindowInfo
                {
                    Handle = h, Title = title.ToString(), Process = ProcessName(h),
                    Bounds = FrameBounds(h, wr.ToRectangle()), Minimized = IsIconic(h),
                });
                return true;
            };
            EnumWindows(callback, IntPtr.Zero);
            GC.KeepAlive(callback);
            return found;
        }

        public static WindowInfo Find(string handle, string title)
        {
            var all = Windows();
            if (!string.IsNullOrEmpty(handle))
            {
                long h = Convert.ToInt64(handle.StartsWith("0x", StringComparison.OrdinalIgnoreCase) ? handle.Substring(2) : handle, 16);
                return all.FirstOrDefault(w => w.Handle.ToInt64() == h);
            }
            return all.FirstOrDefault(w => w.Title.IndexOf(title ?? "", StringComparison.OrdinalIgnoreCase) >= 0);
        }

        // Bicubic downscale. Takes ownership of the source: the result is the only bitmap left to dispose.
        public static Bitmap Scale(Bitmap src, double scale)
        {
            int w = Math.Max(1, (int)Math.Round(src.Width * scale)), h = Math.Max(1, (int)Math.Round(src.Height * scale));
            if (w == src.Width && h == src.Height) return src;
            var dst = new Bitmap(w, h, PixelFormat.Format32bppRgb);
            using (src)
            using (var g = Graphics.FromImage(dst))
            using (var attributes = new ImageAttributes())
            {
                g.InterpolationMode = InterpolationMode.HighQualityBicubic;
                g.PixelOffsetMode = PixelOffsetMode.HighQuality;
                g.CompositingQuality = CompositingQuality.HighQuality;
                g.SmoothingMode = SmoothingMode.HighQuality;
                attributes.SetWrapMode(WrapMode.TileFlipXY);    // no half-transparent border from sampling outside
                g.DrawImage(src, new Rectangle(0, 0, w, h), 0, 0, src.Width, src.Height, GraphicsUnit.Pixel, attributes);
            }
            return dst;
        }

        public static byte[] Encode(Bitmap bmp, string format, int quality, out string contentType)
        {
            using (var ms = new MemoryStream())
            {
                if (format == "jpg")
                {
                    var codec = ImageCodecInfo.GetImageEncoders().First(c => c.MimeType == "image/jpeg");
                    using (var p = new EncoderParameters(1))
                    {
                        p.Param[0] = new EncoderParameter(System.Drawing.Imaging.Encoder.Quality, (long)quality);
                        bmp.Save(ms, codec, p);
                    }
                    contentType = "image/jpeg";
                }
                else
                {
                    bmp.Save(ms, ImageFormat.Png);
                    contentType = "image/png";
                }
                return ms.ToArray();
            }
        }

        // What the DWM actually paints. GetWindowRect adds an invisible grab border on sizeable windows.
        static Rectangle FrameBounds(IntPtr hwnd, Rectangle fallback)
        {
            RECT r;
            if (DwmGetWindowAttribute(hwnd, DWMWA_EXTENDED_FRAME_BOUNDS, out r, Marshal.SizeOf(typeof(RECT))) != 0) return fallback;
            var frame = r.ToRectangle();
            return frame.Width > 0 && frame.Height > 0 ? frame : fallback;
        }

        static string ProcessName(IntPtr hwnd)
        {
            int pid;
            GetWindowThreadProcessId(hwnd, out pid);
            try { using (var p = Process.GetProcessById(pid)) return p.ProcessName; }
            catch { return "?"; }
        }

        // A window that ignored PrintWindow leaves the bitmap as it was: one single colour. Sampling a grid is
        // enough to tell that apart from any real window content.
        static bool LooksBlank(Bitmap bmp)
        {
            var data = bmp.LockBits(new Rectangle(0, 0, bmp.Width, bmp.Height), ImageLockMode.ReadOnly, PixelFormat.Format32bppRgb);
            try
            {
                int stepX = Math.Max(1, bmp.Width / 48), stepY = Math.Max(1, bmp.Height / 48);
                int first = Marshal.ReadInt32(data.Scan0);
                for (int y = 0; y < bmp.Height; y += stepY)
                    for (int x = 0; x < bmp.Width; x += stepX)
                        if (Marshal.ReadInt32(data.Scan0, y * data.Stride + x * 4) != first) return false;
                return true;
            }
            finally { bmp.UnlockBits(data); }
        }
    }
}
