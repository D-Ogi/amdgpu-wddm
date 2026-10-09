// Display mode and GPU scaling changes of the Display page, by Windows' own rule: a new setting applies at once and
// is saved only when the person keeps it within 15 s (KeepDisplayDialog); otherwise the configuration from before the
// change comes back. The only file of this app that changes a display setting (G-SRC). The change is per user and
// needs no administrator: ChangeDisplaySettingsEx for the mode (dynamic first, CDS_UPDATEREGISTRY on keep) and the
// connecting and configuring displays API for the scaling (SetDisplayConfig without SDC_SAVE_TO_DATABASE first, with
// it on keep). Only modes Windows lists are offered (DisplayInfo.Modes), and every mode is tested (CDS_TEST) first.
using System;
using System.Runtime.InteropServices;

namespace AmdgpuWddmControl
{
    // One change in progress: what to save on keep, and the configuration to put back on revert.
    sealed class DisplayChange
    {
        public string Device;
        public bool ModeChanged, ScalingChanged;
        internal DisplayInfo.DEVMODE Mode;
        public uint Scaling;
        internal DisplayModes.PathInfo[] OldPaths;
        internal DisplayModes.ModeInfo[] OldModes;
    }

    static class DisplayModes
    {
        const int DmPelsWidth = 0x80000, DmPelsHeight = 0x100000, DmDisplayFrequency = 0x400000;
        const uint CdsUpdateRegistry = 0x1, CdsTest = 0x2;
        const uint QdcOnlyActivePaths = 0x2;
        const uint SdcUseDatabaseCurrent = 0xF, SdcUseSuppliedDisplayConfig = 0x20, SdcApply = 0x80, SdcSaveToDatabase = 0x200, SdcAllowChanges = 0x400;
        const uint DeviceInfoGetSourceName = 1;
        const int ErrorInsufficientBuffer = 122;

        [StructLayout(LayoutKind.Sequential)] internal struct Luid { public uint Low; public int High; }
        [StructLayout(LayoutKind.Sequential)] internal struct Rational { public uint Numerator, Denominator; }

        [StructLayout(LayoutKind.Sequential)]
        internal struct SourceInfo { public Luid AdapterId; public uint Id, ModeInfoIdx, StatusFlags; }

        [StructLayout(LayoutKind.Sequential)]
        internal struct TargetInfo
        {
            public Luid AdapterId;
            public uint Id, ModeInfoIdx, OutputTechnology, Rotation, Scaling;
            public Rational RefreshRate;
            public uint ScanLineOrdering;
            public int TargetAvailable;
            public uint StatusFlags;
        }

        // DISPLAYCONFIG_PATH_INFO, 72 bytes.
        [StructLayout(LayoutKind.Sequential)]
        internal struct PathInfo { public SourceInfo Source; public TargetInfo Target; public uint Flags; }

        // DISPLAYCONFIG_MODE_INFO, 64 bytes: the union is passed back as it came (48 bytes, 8-byte aligned).
        [StructLayout(LayoutKind.Sequential)]
        internal struct ModeInfo { public uint InfoType, Id; public Luid AdapterId; public ulong U0, U1, U2, U3, U4, U5; }

        [StructLayout(LayoutKind.Sequential)]
        struct DeviceInfoHeader { public uint Type, Size; public Luid AdapterId; public uint Id; }

        [StructLayout(LayoutKind.Sequential, CharSet = CharSet.Unicode)]
        struct SourceDeviceName
        {
            public DeviceInfoHeader Header;
            [MarshalAs(UnmanagedType.ByValTStr, SizeConst = 32)] public string ViewGdiDeviceName;
        }

        [DllImport("user32.dll")]
        static extern int GetDisplayConfigBufferSizes(uint flags, out uint numPaths, out uint numModes);

        [DllImport("user32.dll")]
        static extern int QueryDisplayConfig(uint flags, ref uint numPaths, [Out] PathInfo[] paths, ref uint numModes, [Out] ModeInfo[] modes, IntPtr topology);

        [DllImport("user32.dll")]
        static extern int SetDisplayConfig(uint numPaths, [In] PathInfo[] paths, uint numModes, [In] ModeInfo[] modes, uint flags);

        [DllImport("user32.dll")]
        static extern int DisplayConfigGetDeviceInfo(ref SourceDeviceName request);

        [DllImport("user32.dll", CharSet = CharSet.Unicode, EntryPoint = "ChangeDisplaySettingsExW")]
        static extern int ChangeDisplaySettingsEx(string device, ref DisplayInfo.DEVMODE mode, IntPtr hwnd, uint flags, IntPtr param);

        [DllImport("user32.dll", CharSet = CharSet.Unicode, EntryPoint = "ChangeDisplaySettingsExW")]
        static extern int ChangeDisplaySettingsExToSaved(string device, IntPtr mode, IntPtr hwnd, uint flags, IntPtr param);

        static bool Query(out PathInfo[] paths, out ModeInfo[] modes)
        {
            paths = null; modes = null;
            for (int attempt = 0; attempt < 3; attempt++)
            {
                uint np, nm;
                if (GetDisplayConfigBufferSizes(QdcOnlyActivePaths, out np, out nm) != 0) return false;
                var p = new PathInfo[np];
                var m = new ModeInfo[nm];
                int rc = QueryDisplayConfig(QdcOnlyActivePaths, ref np, p, ref nm, m, IntPtr.Zero);
                if (rc == ErrorInsufficientBuffer) continue;
                if (rc != 0) return false;
                Array.Resize(ref p, (int)np);
                Array.Resize(ref m, (int)nm);
                paths = p; modes = m;
                return true;
            }
            return false;
        }

        // The active path whose source is the monitor's GDI device (\\.\DISPLAY1), or -1.
        static int PathOf(PathInfo[] paths, string device)
        {
            for (int i = 0; i < paths.Length; i++)
            {
                var request = new SourceDeviceName();
                request.Header.Type = DeviceInfoGetSourceName;
                request.Header.Size = (uint)Marshal.SizeOf(typeof(SourceDeviceName));
                request.Header.AdapterId = paths[i].Source.AdapterId;
                request.Header.Id = paths[i].Source.Id;
                if (DisplayConfigGetDeviceInfo(ref request) == 0 && string.Equals(request.ViewGdiDeviceName, device, StringComparison.OrdinalIgnoreCase)) return i;
            }
            return -1;
        }

        // The GPU scaling of the monitor's path now (DISPLAYCONFIG_SCALING), or null when it cannot be read.
        public static uint? ReadScaling(string device)
        {
            try
            {
                PathInfo[] paths; ModeInfo[] modes;
                if (!Query(out paths, out modes)) return null;
                int i = PathOf(paths, device);
                return i < 0 ? (uint?)null : paths[i].Target.Scaling;
            }
            catch (Exception) { return null; }
        }

        static int SetScaling(string device, uint scaling, bool save)
        {
            PathInfo[] paths; ModeInfo[] modes;
            if (!Query(out paths, out modes)) return -1;
            int i = PathOf(paths, device);
            if (i < 0) return -1;
            paths[i].Target.Scaling = scaling;
            return SetDisplayConfig((uint)paths.Length, paths, (uint)modes.Length, modes, SdcApply | SdcUseSuppliedDisplayConfig | SdcAllowChanges | (save ? SdcSaveToDatabase : 0));
        }

        // Applies the change without saving it. mode null: the mode stays; scaling null: the scaling stays. Returns
        // null and an English reason for the log when Windows refuses; nothing stays changed then.
        public static DisplayChange Apply(string device, DisplayChoice mode, uint? scaling, out string error)
        {
            error = null;
            var change = new DisplayChange { Device = device };
            try
            {
                if (!Query(out change.OldPaths, out change.OldModes)) { error = "the display configuration cannot be read"; return null; }
                if (mode != null)
                {
                    DisplayInfo.DEVMODE dm;
                    if (!DisplayInfo.TryCurrent(device, out dm)) { error = "the current mode cannot be read"; return null; }
                    dm.dmPelsWidth = mode.Width; dm.dmPelsHeight = mode.Height; dm.dmDisplayFrequency = mode.RefreshHz;
                    dm.dmFields = DmPelsWidth | DmPelsHeight | DmDisplayFrequency;
                    int test = ChangeDisplaySettingsEx(device, ref dm, IntPtr.Zero, CdsTest, IntPtr.Zero);
                    if (test != 0) { error = "Windows does not take this mode (test " + test + ")"; return null; }
                    int rc = ChangeDisplaySettingsEx(device, ref dm, IntPtr.Zero, 0, IntPtr.Zero);
                    if (rc != 0) { error = "the mode change failed (" + rc + ")"; Revert(change); return null; }
                    change.ModeChanged = true; change.Mode = dm;
                }
                if (scaling != null)
                {
                    int rc = SetScaling(device, scaling.Value, false);
                    if (rc != 0) { error = "the scaling change failed (" + rc + ")"; Revert(change); return null; }
                    change.ScalingChanged = true; change.Scaling = scaling.Value;
                }
                return change;
            }
            catch (Exception e) { error = e.Message; Revert(change); return null; }
        }

        // Saves the change: the mode in the registry, the scaling in Windows' display database. Null when saved.
        public static string Keep(DisplayChange c)
        {
            try
            {
                if (c.ModeChanged)
                {
                    var dm = c.Mode;
                    int rc = ChangeDisplaySettingsEx(c.Device, ref dm, IntPtr.Zero, CdsUpdateRegistry, IntPtr.Zero);
                    if (rc != 0) return "the mode was not saved (" + rc + ")";
                }
                if (c.ScalingChanged)
                {
                    int rc = SetScaling(c.Device, c.Scaling, true);
                    if (rc != 0) return "the scaling was not saved (" + rc + ")";
                }
                return null;
            }
            catch (Exception e) { return e.Message; }
        }

        // Puts back the configuration from before the change; when that no longer fits (a monitor changed meanwhile),
        // the saved settings. Null when done.
        public static string Revert(DisplayChange c)
        {
            if (!c.ModeChanged && !c.ScalingChanged) return null;
            try
            {
                if (c.OldPaths != null && SetDisplayConfig((uint)c.OldPaths.Length, c.OldPaths, (uint)c.OldModes.Length, c.OldModes, SdcApply | SdcUseSuppliedDisplayConfig | SdcAllowChanges) == 0)
                    return null;
                if (c.ModeChanged) ChangeDisplaySettingsExToSaved(c.Device, IntPtr.Zero, IntPtr.Zero, 0, IntPtr.Zero);
                int rc = SetDisplayConfig(0, null, 0, null, SdcApply | SdcUseDatabaseCurrent);
                return rc == 0 ? null : "the previous settings did not come back (" + rc + ")";
            }
            catch (Exception e) { return e.Message; }
        }
    }
}
