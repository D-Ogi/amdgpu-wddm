// The Display page's facts (WU-030, WU-031 read-only, WU-034 links, WU-036 window recovery). Phase 1 only reads the
// current mode: the driver exposes one output with one inherited mode (F-MODE); choosing a mode comes with KMD mode
// setting (phase 4). Nothing here changes a display setting.
using System;
using System.Collections.Generic;
using System.Drawing;
using System.Linq;
using System.Runtime.InteropServices;

namespace AmdgpuWddmControl
{
    public sealed class DisplayMode
    {
        public string Device;
        public int Width, Height, RefreshHz, Bits;
        public Rectangle Bounds;
        public bool Primary;
    }

    public static class DisplayInfo
    {
        // Links to Windows' own settings (WU-034): scale and layout, and the advanced display page.
        public const string WindowsDisplaySettings = "ms-settings:display";
        public const string WindowsAdvancedDisplay = "ms-settings:display-advanced";
        public static readonly string[] AllowedLinks = { WindowsDisplaySettings, WindowsAdvancedDisplay };

        public static List<DisplayMode> Current()
        {
            var list = new List<DisplayMode>();
            foreach (var s in System.Windows.Forms.Screen.AllScreens)
            {
                var m = new DisplayMode { Device = s.DeviceName, Bounds = s.Bounds, Primary = s.Primary, Width = s.Bounds.Width, Height = s.Bounds.Height };
                var dm = new DEVMODE { dmSize = (short)Marshal.SizeOf(typeof(DEVMODE)) };
                try
                {
                    if (EnumDisplaySettings(s.DeviceName, EnumCurrentSettings, ref dm))
                    {
                        m.Width = dm.dmPelsWidth; m.Height = dm.dmPelsHeight; m.RefreshHz = dm.dmDisplayFrequency; m.Bits = dm.dmBitsPerPel;
                    }
                }
                catch (Exception) { }
                list.Add(m);
            }
            return list.OrderByDescending(m => m.Primary).ThenBy(m => m.Bounds.X).ToList();
        }

        // Where the window goes after a start or a display change (WU-036): it stays where it is when at least a
        // usable part of its title bar is on a work area; otherwise it moves to the primary work area (the first
        // one), shrunk to fit and centred. workAreas[0] is the primary screen's.
        public static Rectangle Fit(Rectangle window, IList<Rectangle> workAreas, Size minimum, int titleHeight)
        {
            if (workAreas == null || workAreas.Count == 0) return window;
            var title = new Rectangle(window.X, window.Y, window.Width, Math.Max(1, titleHeight));
            int need = Math.Min(window.Width, Math.Max(100, titleHeight * 3));
            foreach (var a in workAreas)
            {
                var visible = Rectangle.Intersect(title, a);
                if (visible.Width >= need && visible.Height >= Math.Min(titleHeight, 8)) return window;
            }
            var area = workAreas[0];
            int w = Math.Max(Math.Min(window.Width, area.Width), Math.Min(minimum.Width, area.Width));
            int h = Math.Max(Math.Min(window.Height, area.Height), Math.Min(minimum.Height, area.Height));
            return new Rectangle(area.X + (area.Width - w) / 2, area.Y + (area.Height - h) / 2, w, h);
        }

        const int EnumCurrentSettings = -1;

        [DllImport("user32.dll", CharSet = CharSet.Unicode)]
        static extern bool EnumDisplaySettings(string deviceName, int modeNum, ref DEVMODE devMode);

        [StructLayout(LayoutKind.Sequential, CharSet = CharSet.Unicode)]
        struct DEVMODE
        {
            [MarshalAs(UnmanagedType.ByValTStr, SizeConst = 32)] public string dmDeviceName;
            public short dmSpecVersion, dmDriverVersion, dmSize, dmDriverExtra;
            public int dmFields, dmPositionX, dmPositionY, dmDisplayOrientation, dmDisplayFixedOutput;
            public short dmColor, dmDuplex, dmYResolution, dmTTOption, dmCollate;
            [MarshalAs(UnmanagedType.ByValTStr, SizeConst = 32)] public string dmFormName;
            public short dmLogPixels;
            public int dmBitsPerPel, dmPelsWidth, dmPelsHeight, dmDisplayFlags, dmDisplayFrequency;
            public int dmICMMethod, dmICMIntent, dmMediaType, dmDitherType, dmReserved1, dmReserved2, dmPanningWidth, dmPanningHeight;
        }
    }
}
