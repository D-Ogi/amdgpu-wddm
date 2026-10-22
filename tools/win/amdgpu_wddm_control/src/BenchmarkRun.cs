// Runs the ray tracing benchmark of src/Benchmark.cs: one child process per pass, watched from a worker thread.
// Nothing here runs on the window's UI thread, and only one run exists at a time.
//
// How a pass ends. Each pass gets a job object of its own with JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE. The job is built
// before the game is started and the game is put into it at once, because the job is the only way a pass can end:
// a game outside it would keep the path tracing load with no thermal stop and no time bound behind it. A pass with
// no job is therefore never started, and a game that could not join its job is ended straight away, by the handle
// this program started it with. That one ending is the only process this file ends; nothing here is ever ended by
// name (G-SRC), because a job holds exactly our own children, which a search by name never could.
//
// A pass also waits for its game to be gone before the run goes on. TerminateJobObject only asks, and a thread
// stuck in the driver can outlive the ask (BD-102 hangs this GPU in path tracing), so a pass whose game is still
// there ends the whole run: two copies of Quake II RTX must never trace at once.
//
// What the run reads while it waits: the driver's DPM snapshot, the same Level-One read the Performance page polls,
// every two seconds. It gives the chip temperature for the thermal stop (89 C at once, or 87 C
// held for 10 s) and the clock it measured for the run's average. A reading the driver cannot give is never a
// temperature, and a clock the driver only asked for is never part of the average.
using System;
using System.Collections.Generic;
using System.Diagnostics;
using System.IO;
using System.Runtime.InteropServices;
using System.Text;

namespace AmdgpuWddmControl
{
    public static class BenchRunner
    {
        static readonly object Gate = new object();
        static BenchProgress _progress = new BenchProgress();
        static bool _busy, _cancel;

        public static bool Busy { get { lock (Gate) return _busy; } }
        public static BenchProgress Progress { get { lock (Gate) return _progress; } }
        public static void Cancel() { lock (Gate) if (_busy) _cancel = true; }

        // Starts the whole run on a worker thread. false: a run is already going. report runs on that worker thread
        // after every change, so the window marshals it itself.
        public static bool Start(InstallState install, string driver, Action<BenchProgress> report)
        {
            if (install == null || !install.Ready) return false;
            lock (Gate)
            {
                if (_busy) return false;
                _busy = true; _cancel = false;
                _progress = new BenchProgress { Stage = BenchProgress.Starting, Busy = true, Run = NewRun(driver) };
            }
            System.Threading.ThreadPool.QueueUserWorkItem(_ =>
            {
                // BD-174: the failure of a run is recorded and told to the person. Crash.Guard never throws, so
                // the run is always finished below, and the window never waits for a benchmark that died on its
                // own worker thread.
                Crash.Guard("benchmark run", () => Run(install, driver, report));
                BenchProgress last;
                lock (Gate)
                {
                    _busy = false; _cancel = false;
                    last = new BenchProgress { Stage = BenchProgress.Finished, Busy = false, Run = _progress.Run };
                }
                Publish(report, last);
            });
            return true;
        }

        static RunRecord NewRun(string driver)
        {
            return new RunRecord { Utc = Recovery.Stamp(DateTime.UtcNow), Driver = driver, Width = BenchPlan.Width, Height = BenchPlan.Height };
        }

        // The two passes in order. A pass the person stopped, or one the chip was too hot for, ends the run: the
        // remaining pass is recorded with that same reason, so a history entry always says what happened to both.
        static void Run(InstallState install, string driver, Action<BenchProgress> report)
        {
            var head = NewRun(driver);
            var done = new List<PassRecord>();
            // Every progress snapshot carries its own list, so the window reads a finished copy and never a list
            // that the worker thread is adding to.
            Func<RunRecord> snapshot = () => new RunRecord
            {
                Utc = head.Utc, Driver = head.Driver, Width = head.Width, Height = head.Height, Passes = new List<PassRecord>(done),
            };
            string stopped = null;
            foreach (var api in BenchPlan.Passes)
            {
                if (stopped != null) { done.Add(new PassRecord { Api = BenchPlan.ApiName(api), Outcome = stopped }); continue; }
                var record = Pass(install, api, report, snapshot);
                done.Add(record);
                Publish(report, new BenchProgress { Stage = BenchProgress.Reading, Busy = true, Run = snapshot() });
                // A pass the person stopped, one the chip was too hot for, one that ran out of time, and one whose
                // game was still there when the pass gave up on it: each of them ends the run. The next pass would
                // otherwise meet a GPU the first game has not released, and measure a number that means nothing.
                if (record.Alive || record.Outcome == PassOutcome.Cancelled || record.Outcome == PassOutcome.TooHot ||
                    record.Outcome == PassOutcome.TimedOut) stopped = record.Outcome;
            }
            var run = snapshot();
            Publish(report, new BenchProgress { Stage = BenchProgress.Reading, Busy = true, Run = run });
            BenchPaths.SaveRun(run);
        }

        // One pass: the game in its own job object, watched on the sampling interval.
        static PassRecord Pass(InstallState install, RtApi api, Action<BenchProgress> report, Func<RunRecord> snapshot)
        {
            var record = new PassRecord { Api = BenchPlan.ApiName(api), Outcome = PassOutcome.Failed };
            var guard = new ThermalGuard();
            int maxMc = int.MinValue;
            long clockSum = 0, clockSamples = 0;
            Action<int?, uint?> sample = (mc, mhz) =>
            {
                if (mc != null && mc.Value > maxMc) maxMc = mc.Value;
                if (mhz != null && mhz.Value > 0) { clockSum += mhz.Value; clockSamples++; }
            };
            // Set as soon as the game of this pass runs; before that the samples belong to no pass
            // (BenchState rule PassTelemetry.Keep, b23 lab round 2, F8).
            bool gameStarted = false;
            Action store = () =>
            {
                bool keep = PassTelemetry.Keep(record.Outcome, gameStarted);
                record.MaxTempC = !keep || maxMc == int.MinValue ? (int?)null : (int)Math.Round(maxMc / 1000.0);
                record.MeanClockMHz = !keep || clockSamples == 0 ? (uint?)null : (uint)Math.Round(clockSum / (double)clockSamples);
            };

            int? first; uint? firstClock;
            Telemetry(out first, out firstClock);
            sample(first, firstClock);
            if (guard.Hot(0, first)) { record.Outcome = PassOutcome.TooHot; store(); return record; }
            lock (Gate) { if (_cancel) { record.Outcome = PassOutcome.Cancelled; store(); return record; } }

            var home = BenchPaths.Home;
            var log = BenchPlan.LogPath(home, api);
            try
            {
                Directory.CreateDirectory(Path.GetDirectoryName(log));
                if (File.Exists(log)) File.Delete(log);
            }
            catch (Exception) { }
            // A log this program cannot remove is held open by another copy of the game: a second instance of this
            // app, or a game of an earlier pass that is still there. Its result line would then be read as this
            // pass's measurement, so the pass is refused instead. A number that was never measured here is worse
            // than no number at all.
            if (File.Exists(log)) { record.Outcome = PassOutcome.Failed; store(); return record; }

            var psi = new ProcessStartInfo(install.ExePath, BenchPlan.Arguments(install.Folder, home, api))
            {
                UseShellExecute = false, WorkingDirectory = install.Folder,
            };
            // The stamp of this pass, read again right before the game starts: the result log must be newer than it.
            var started = DateTime.UtcNow;
            using (var job = BenchJob.Create())
            {
                // The job is the pass's only way to end the game, so without one the game is never started.
                if (job == null) { record.Outcome = PassOutcome.Failed; store(); return record; }
                Process game = null;
                var clock = Stopwatch.StartNew();
                started = DateTime.UtcNow;
                try { game = Process.Start(psi); }
                catch (Exception) { record.Outcome = PassOutcome.Failed; store(); return record; }
                using (game)
                {
                    // The assignment is the pass's whole hold on the game: without it the job's end would terminate
                    // nothing, the thermal stop and the time bound would both be a silent no-op, and the game would
                    // outlive the card. The game this program started a moment ago is therefore ended here, by its
                    // own handle, and the pass is reported as one that did not run.
                    if (!job.Take(game))
                    {
                        try { game.Kill(); } catch (Exception) { }
                        record.Outcome = PassOutcome.Failed; store(); return record;
                    }
                    gameStarted = true;
                    Publish(report, new BenchProgress { Stage = BenchProgress.Running, Busy = true, Pass = record.Api, Run = snapshot(), ElapsedMs = 0 });
                    string stop = null;
                    while (!game.WaitForExit(BenchPlan.SampleMs))
                    {
                        int? mc; uint? mhz;
                        Telemetry(out mc, out mhz);
                        sample(mc, mhz);
                        long now = clock.ElapsedMilliseconds;
                        bool cancelled;
                        lock (Gate) cancelled = _cancel;
                        if (guard.Hot(now, mc)) stop = PassOutcome.TooHot;
                        else if (cancelled) stop = PassOutcome.Cancelled;
                        else if (now >= BenchPlan.PassBoundMs) stop = PassOutcome.TimedOut;
                        if (stop != null) break;
                        Publish(report, new BenchProgress { Stage = BenchProgress.Running, Busy = true, Pass = record.Api, Run = snapshot(), ElapsedMs = (int)now });
                    }
                    if (stop != null)
                    {
                        job.End();
                        // The end is an ask, not a fact: a thread stuck in the driver can outlive it. A game still
                        // there after this wait is recorded as such, and the run goes no further.
                        if (!game.WaitForExit(BenchPlan.ExitBoundMs)) record.Alive = true;
                    }
                    record.Outcome = stop ?? PassOutcome.NoResult;
                }
            }

            // The result line, read after the pass ended: the game flushes every line (logfile_flush 1). Only a log
            // this pass wrote is a measurement of this pass.
            var result = ReadResult(log, started);
            if (record.Outcome == PassOutcome.NoResult && result != null)
            {
                record.Outcome = PassOutcome.Done;
                record.Fps = result.Fps; record.Frames = result.Frames; record.Seconds = result.Seconds;
            }
            store();
            return record;
        }

        static PassResult ReadResult(string log, DateTime notBefore)
        {
            try
            {
                var info = new FileInfo(log);
                if (!info.Exists || info.Length > BenchLog.MaxBytes) return null;
                // A file older than the start of this pass was written by another run. File times are coarse on some
                // file systems, so the comparison leaves the slack of BenchPlan.FileTimeSlackMs and nothing more.
                if (info.LastWriteTimeUtc < notBefore.AddMilliseconds(-BenchPlan.FileTimeSlackMs)) return null;
                using (var s = new FileStream(log, FileMode.Open, FileAccess.Read, FileShare.ReadWrite | FileShare.Delete))
                using (var r = new StreamReader(s, Encoding.UTF8)) return BenchLog.Last(r.ReadToEnd());
            }
            catch (Exception) { return null; }
        }

        // The chip's temperature and clock from the driver's DPM snapshot; null where the driver gives no value.
        static void Telemetry(out int? milliCelsius, out uint? clockMHz)
        {
            milliCelsius = null; clockMHz = null;
            DpmState d;
            try { d = Kmd.Dpm().Value; } catch (Exception) { return; }
            if (d == null) return;
            if (d.Has(DpmState.FlagTemperature)) milliCelsius = (int)d.TemperatureMc;
            // One source only, the clock the driver measured. The clock a mode asks for is another quantity, and an
            // average of the two together would be the average of nothing.
            if (d.Has(DpmState.FlagClock) && d.ObservedMHz != 0) clockMHz = d.ObservedMHz;
        }

        // The one place the shared state changes, so a reader always sees a whole snapshot.
        static void Publish(Action<BenchProgress> report, BenchProgress p)
        {
            lock (Gate) _progress = p;
            if (report == null) return;
            try { report(p); } catch (Exception) { }
        }
    }

    // One job object per pass. Closing it kills every process of that pass, so a pass can never outlive the window.
    sealed class BenchJob : IDisposable
    {
        // JOBOBJECT_EXTENDED_LIMIT_INFORMATION, flat. Only LimitFlags is set; the rest is the shape the call needs,
        // so the compiler is told not to ask who assigns them.
#pragma warning disable 0649
        [StructLayout(LayoutKind.Sequential)]
        struct ExtendedLimits
        {
            public long PerProcessUserTime, PerJobUserTime;
            public uint LimitFlags;
            public UIntPtr MinimumWorkingSetSize, MaximumWorkingSetSize;
            public uint ActiveProcessLimit;
            public UIntPtr Affinity;
            public uint PriorityClass, SchedulingClass;
            public ulong ReadOperationCount, WriteOperationCount, OtherOperationCount, ReadTransferCount, WriteTransferCount, OtherTransferCount;
            public UIntPtr ProcessMemoryLimit, JobMemoryLimit, PeakProcessMemoryUsed, PeakJobMemoryUsed;
        }
#pragma warning restore 0649

        [DllImport("kernel32.dll", CharSet = CharSet.Unicode, SetLastError = true)]
        static extern IntPtr CreateJobObjectW(IntPtr security, string name);
        [DllImport("kernel32.dll", SetLastError = true)]
        static extern bool SetInformationJobObject(IntPtr job, int kind, ref ExtendedLimits info, int bytes);
        [DllImport("kernel32.dll", SetLastError = true)]
        static extern bool AssignProcessToJobObject(IntPtr job, IntPtr process);
        [DllImport("kernel32.dll", SetLastError = true)]
        static extern bool TerminateJobObject(IntPtr job, uint code);
        [DllImport("kernel32.dll", SetLastError = true)]
        static extern bool CloseHandle(IntPtr handle);

        const int ExtendedLimitInformation = 9;
        const uint KillOnJobClose = 0x2000;

        IntPtr _job;

        BenchJob(IntPtr job) { _job = job; }

        public static BenchJob Create()
        {
            IntPtr job;
            try { job = CreateJobObjectW(IntPtr.Zero, null); }
            catch (Exception) { return null; }
            if (job == IntPtr.Zero) return null;
            var info = new ExtendedLimits();
            info.LimitFlags = KillOnJobClose;
            try
            {
                if (!SetInformationJobObject(job, ExtendedLimitInformation, ref info, Marshal.SizeOf(typeof(ExtendedLimits))))
                { CloseHandle(job); return null; }
            }
            catch (Exception) { CloseHandle(job); return null; }
            return new BenchJob(job);
        }

        public bool Take(Process p)
        {
            try { return _job != IntPtr.Zero && p != null && AssignProcessToJobObject(_job, p.Handle); }
            catch (Exception) { return false; }
        }

        // Ends every process of this pass at once.
        public void End()
        {
            try { if (_job != IntPtr.Zero) TerminateJobObject(_job, 1); } catch (Exception) { }
        }

        public void Dispose()
        {
            var job = _job;
            _job = IntPtr.Zero;
            if (job == IntPtr.Zero) return;
            try { CloseHandle(job); } catch (Exception) { }
        }
    }
}
