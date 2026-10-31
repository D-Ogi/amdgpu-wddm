// The Display page's facts (WU-030, WU-031, WU-034 links, WU-036 window recovery): the current mode, the modes Windows
// lists for a monitor (the same list as Windows' own display settings: the current colour depth, no interlaced
// modes) and the GPU scaling choices. Nothing here changes a display setting; DisplayModes.cs does, with the 15 s
// "keep these settings?" rule.
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

    // One mode Windows lists for a monitor.
    public sealed class DisplayChoice
    {
        public int Width, Height, RefreshHz;
    }

    public static class DisplayInfo
    {
        // Windows' own rule: a new display setting goes back after 15 s unless the person keeps it.
        public const int KeepSeconds = 15;

        // DISPLAYCONFIG_SCALING (wingdi.h) and the three choices the page offers: full screen (stretched), keep the
        // aspect ratio (aspect-ratio-centred-max) and centred (no scaling). Identity, custom and preferred are what
        // Windows chose by itself; the page shows them as such and never writes them.
        public const uint ScalingIdentity = 1, ScalingCentered = 2, ScalingStretched = 3, ScalingAspect = 4, ScalingCustom = 5, ScalingPreferred = 128;
        public static readonly string[] ScalingChoices = { "full", "aspect", "center" };

        public static string ScalingOf(uint value)
        {
            switch (value)
            {
                case ScalingStretched: return "full";
                case ScalingAspect: return "aspect";
                case ScalingCentered: return "center";
                case 0: return null;
                default: return "windows";
            }
        }

        public static uint ScalingValue(string choice)
        {
            switch (choice)
            {
                case "full": return ScalingStretched;
                case "aspect": return ScalingAspect;
                case "center": return ScalingCentered;
                default: throw new ArgumentException("not a scaling choice: " + choice);
            }
        }

        // The modes as Windows lists them, once each, largest first.
        public static List<DisplayChoice> Distinct(IEnumerable<DisplayChoice> raw)
        {
            return raw.Where(m => m.Width > 0 && m.Height > 0).GroupBy(m => new { m.Width, m.Height, m.RefreshHz }).Select(g => g.First())
                .OrderByDescending(m => m.Width).ThenByDescending(m => m.Height).ThenByDescending(m => m.RefreshHz).ToList();
        }

        public static List<Size> Resolutions(IEnumerable<DisplayChoice> modes)
        {
            return modes.Select(m => new Size(m.Width, m.Height)).Distinct().OrderByDescending(s => s.Width).ThenByDescending(s => s.Height).ToList();
        }

        public static List<int> Rates(IEnumerable<DisplayChoice> modes, int width, int height)
        {
            return modes.Where(m => m.Width == width && m.Height == height).Select(m => m.RefreshHz).Distinct().OrderByDescending(r => r).ToList();
        }

        // The refresh rate after a change of resolution: the one in use when the new resolution has it, else its highest.
        public static int RateFor(IEnumerable<DisplayChoice> modes, int width, int height, int preferred)
        {
            var rates = Rates(modes, width, height);
            return rates.Contains(preferred) ? preferred : rates.Count > 0 ? rates[0] : preferred;
        }

        public static bool Listed(IEnumerable<DisplayChoice> modes, int width, int height, int hz)
        {
            return modes.Any(m => m.Width == width && m.Height == height && m.RefreshHz == hz);
        }

        // The modes of one monitor (EnumDisplaySettingsEx without EDS_RAWMODE: what the monitor takes, in the current
        // orientation), at the current colour depth and without interlaced modes, as Windows' settings list them.
        public static List<DisplayChoice> Modes(string device)
        {
            var raw = new List<DisplayChoice>();
            try
            {
                DEVMODE now;
                if (!TryCurrent(device, out now)) return raw;
                for (int i = 0; i < 4096; i++)
                {
                    var dm = new DEVMODE { dmSize = (short)Marshal.SizeOf(typeof(DEVMODE)) };
                    if (!EnumDisplaySettingsEx(device, i, ref dm, 0)) break;
                    if (dm.dmBitsPerPel != now.dmBitsPerPel || (dm.dmDisplayFlags & DmInterlaced) != 0) continue;
                    raw.Add(new DisplayChoice { Width = dm.dmPelsWidth, Height = dm.dmPelsHeight, RefreshHz = dm.dmDisplayFrequency });
                }
            }
            catch (Exception) { }
            return Distinct(raw);
        }

        // The native mode of the monitor on one display device (\\.\DISPLAY1), from its EDID (ScaledStart.NativeSize),
        // or null when it cannot be read. The monitor's interface (EnumDisplayDevices with EDD_GET_DEVICE_INTERFACE_NAME)
        // gives its device instance (DEVPKEY_Device_InstanceId), and the hardware key of that instance (CM_Open_DevNode_Key,
        // CM_REGISTRY_HARDWARE) holds the EDID value that Windows read from the monitor. Every user may read it.
        public static Size? EdidNativeSize(string device)
        {
            try
            {
                var dd = new DISPLAY_DEVICE { cb = Marshal.SizeOf(typeof(DISPLAY_DEVICE)) };
                if (!EnumDisplayDevices(device, 0, ref dd, EddGetDeviceInterfaceName) || string.IsNullOrEmpty(dd.DeviceID)) return null;
                var key = new DevPropKey { fmtid = new Guid(0x78c34fc8, 0x104a, 0x4aca, 0x9e, 0xa4, 0x52, 0x4d, 0x52, 0x99, 0x6e, 0x57), pid = 256 };
                uint type, size = 512;
                var buffer = new byte[size];
                if (CM_Get_Device_Interface_Property(dd.DeviceID, ref key, out type, buffer, ref size, 0) != CrSuccess || size < 2) return null;
                string instance = System.Text.Encoding.Unicode.GetString(buffer, 0, (int)size).TrimEnd('\0');
                uint node;
                if (CM_Locate_DevNode(out node, instance, 0) != CrSuccess) return null;
                IntPtr hkey;
                if (CM_Open_DevNode_Key(node, KeyRead, 0, RegDispositionOpenExisting, out hkey, CmRegistryHardware) != CrSuccess) return null;
                using (var k = Microsoft.Win32.RegistryKey.FromHandle(new Microsoft.Win32.SafeHandles.SafeRegistryHandle(hkey, true)))
                    return ScaledStart.NativeSize(k.GetValue("EDID") as byte[]);
            }
            catch (Exception) { return null; }
        }

        internal static bool TryCurrent(string device, out DEVMODE dm)
        {
            dm = new DEVMODE { dmSize = (short)Marshal.SizeOf(typeof(DEVMODE)) };
            try { return EnumDisplaySettings(device, EnumCurrentSettings, ref dm); }
            catch (Exception) { return false; }
        }
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
        const int DmInterlaced = 0x2;
        // WinUser.h, cfgmgr32.h and winnt.h of SDK 10.0.26100.
        const uint EddGetDeviceInterfaceName = 0x1, CrSuccess = 0, RegDispositionOpenExisting = 1, CmRegistryHardware = 0, KeyRead = 0x20019;

        [StructLayout(LayoutKind.Sequential, CharSet = CharSet.Unicode)]
        struct DISPLAY_DEVICE
        {
            public int cb;
            [MarshalAs(UnmanagedType.ByValTStr, SizeConst = 32)] public string DeviceName;
            [MarshalAs(UnmanagedType.ByValTStr, SizeConst = 128)] public string DeviceString;
            public int StateFlags;
            [MarshalAs(UnmanagedType.ByValTStr, SizeConst = 128)] public string DeviceID;
            [MarshalAs(UnmanagedType.ByValTStr, SizeConst = 128)] public string DeviceKey;
        }

        [StructLayout(LayoutKind.Sequential)]
        struct DevPropKey { public Guid fmtid; public uint pid; }

        [DllImport("user32.dll", CharSet = CharSet.Unicode, EntryPoint = "EnumDisplayDevicesW")]
        static extern bool EnumDisplayDevices(string device, uint devNum, ref DISPLAY_DEVICE displayDevice, uint flags);

        [DllImport("cfgmgr32.dll", CharSet = CharSet.Unicode, EntryPoint = "CM_Get_Device_Interface_PropertyW")]
        static extern uint CM_Get_Device_Interface_Property(string deviceInterface, ref DevPropKey key, out uint type, byte[] buffer, ref uint size, uint flags);

        [DllImport("cfgmgr32.dll", CharSet = CharSet.Unicode, EntryPoint = "CM_Locate_DevNodeW")]
        static extern uint CM_Locate_DevNode(out uint devInst, string deviceId, uint flags);

        [DllImport("cfgmgr32.dll")]
        static extern uint CM_Open_DevNode_Key(uint devNode, uint samDesired, uint hardwareProfile, uint disposition, out IntPtr key, uint flags);

        [DllImport("user32.dll", CharSet = CharSet.Unicode)]
        static extern bool EnumDisplaySettings(string deviceName, int modeNum, ref DEVMODE devMode);

        [DllImport("user32.dll", CharSet = CharSet.Unicode)]
        static extern bool EnumDisplaySettingsEx(string deviceName, int modeNum, ref DEVMODE devMode, uint flags);

        [StructLayout(LayoutKind.Sequential, CharSet = CharSet.Unicode)]
        internal struct DEVMODE
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
