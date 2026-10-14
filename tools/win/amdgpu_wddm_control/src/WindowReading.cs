// Everything the window reads from the computer when it opens and on F5, in one object: the installed files, the
// driver's answers, the session's DWM, the scheduled task, the cached update check, the recent launches and the
// benchmark's game folder. It reads and decides nothing about the window, so the start can run it off the
// user-interface thread while the loading window shows (Launch.cs, LaunchSequence.cs) and the window only applies the
// result (MainForm.Apply). The reads, their order and their fallbacks are the ones RefreshAll made on the
// user-interface thread before; each part that fails leaves its empty value, as it did there.
using System;
using System.Collections.Generic;

namespace AmdgpuWddmControl
{
    public sealed class WindowReading
    {
        public InventoryState Inventory = new InventoryState();
        public RecoverySnapshot Snapshot = new RecoverySnapshot();
        public VideoMemoryState Vram;
        public UmaState Uma;
        public HwmonState Fan;
        public UpdateCache Update;
        public RecentList Recent = new RecentList();
        public InstallState Bench = new InstallState();
        public List<RunRecord> BenchHistory = new List<RunRecord>();

        // gate: a headless run (--smoke, --smoke-perf): the benchmark searches no folder of the PC that builds this.
        // prefs: an AppPrefs of the caller's own; the start gives this thread an instance of its own, because the
        // window reads its preferences on the other thread at the same time.
        public static WindowReading Read(AppPrefs prefs, bool gate)
        {
            var r = new WindowReading();
            try { r.Inventory = AmdgpuWddmControl.Inventory.Read(false); } catch (Exception) { r.Inventory = new InventoryState(); }
            try
            {
                Kmd.RefreshBoardCapabilities();
                r.Snapshot = RecoveryProbe.Read("window");
            }
            catch (Exception) { r.Snapshot = new RecoverySnapshot { DriverError = "unreadable", ReadFailed = true }; }
            r.Vram = Kmd.VideoMemory().Value;
            r.Uma = Kmd.BoardMemoryQuery().Value;
            r.Fan = Kmd.Hwmon().Value;
            r.Update = UpdateCheck.LoadCache();
            r.Recent = RecentLaunches.Read(new LocalRecentFiles());
            if (!gate)
            {
                var files = new LocalBenchFiles();
                try { r.Bench = BenchLocator.Find(prefs.BenchmarkFolder, files); } catch (Exception) { r.Bench = new InstallState(); }
                try { r.BenchHistory = BenchPaths.ReadHistory(); } catch (Exception) { r.BenchHistory = new List<RunRecord>(); }
            }
            return r;
        }
    }
}
