// Host tests of the start of the window (src/LaunchSequence.cs, src/SingleInstance.cs, src/Launch.cs): the loading
// window comes before every slow step, the main window opens only with the reading and before the loading window
// goes, and a second start of the program finds the first one and asks it to come forward, also from another
// process. No window: the order runs against a recorder, the guard against objects with names of this test only.
using System;
using System.Collections.Generic;
using System.Diagnostics;
using System.IO;
using System.Linq;
using System.Threading;
using AmdgpuWddmControl;

static partial class UnitTests
{
    // Records the steps in the order the sequence asks for them. BuildMain reports its own end when told to, as
    // Launch.BuildMain does: the window is built on the same thread, inside the call.
    sealed class StepLog : ILaunchSteps
    {
        public readonly List<string> Calls = new List<string>();
        public LaunchSequence Sequence;
        public bool BuildReports = true;
        public void ShowLoading() { Calls.Add("loading"); }
        public void StartReading() { Calls.Add("read"); }
        public void BuildMain() { Calls.Add("build"); if (BuildReports) Sequence.MainBuilt(); }
        public void ShowMain() { Calls.Add("main"); }
        public void CloseLoading() { Calls.Add("close"); }
        public void SaySlow() { Calls.Add("slow"); }
        public string Text { get { return string.Join(",", Calls); } }
    }

    static StepLog Steps(bool buildReports = true)
    {
        var log = new StepLog { BuildReports = buildReports };
        log.Sequence = new LaunchSequence(log);
        return log;
    }

    static void LaunchTests(string root)
    {
        // ---- the order: the loading window first, the slow work only after it is drawn ---------------------------
        var s = Steps();
        s.Sequence.Begin();
        Equal("loading", s.Text, "start: the loading window is the first step, and the only one before it is drawn");
        s.Sequence.Elapsed(30);
        s.Sequence.ReadingDone();
        s.Sequence.MainBuilt();
        Equal("loading", s.Text, "start: nothing opens and nothing is said before the loading window is drawn");
        s.Sequence.LoadingShown();
        Equal("loading,read,build", s.Text, "start: the reading and the main window start only after the loading window is drawn");
        Check(!s.Sequence.MainShown, "start: the built main window waits for the reading");
        s.Sequence.LoadingShown();
        s.Sequence.Begin();
        Equal("loading,read,build", s.Text, "start: a second paint or begin starts nothing again");
        s.Sequence.Elapsed(LaunchSequence.SlowSeconds - 0.1);
        Equal("loading,read,build", s.Text, "start: nothing is said before the slow bound");
        s.Sequence.Elapsed(LaunchSequence.SlowSeconds);
        s.Sequence.Elapsed(LaunchSequence.SlowSeconds + 5);
        Equal("loading,read,build,slow", s.Text, "start: a slow start says so, once");
        s.Sequence.ReadingDone();
        Equal("loading,read,build,slow,main,close", s.Text, "start: the main window opens with the reading, the loading window closes after it");
        Check(s.Sequence.MainShown, "start: the main window is open");
        s.Sequence.ReadingDone();
        s.Sequence.MainBuilt();
        s.Sequence.Elapsed(60);
        s.Sequence.Cancel();
        Equal("loading,read,build,slow,main,close", s.Text, "start: the main window opens once, and nothing follows it");
        Check(s.Sequence.MainShown, "start: a close after the start does not undo it");

        var r = Steps(false);
        r.Sequence.Begin();
        r.Sequence.LoadingShown();
        r.Sequence.ReadingDone();
        Equal("loading,read,build", r.Text, "start: a reading that ends first waits for the main window to be built");
        r.Sequence.MainBuilt();
        Equal("loading,read,build,main,close", r.Text, "start: the main window opens when it is built");

        var quick = Steps();
        quick.Sequence.Begin();
        quick.Sequence.LoadingShown();
        quick.Sequence.ReadingDone();
        quick.Sequence.Elapsed(60);
        Equal("loading,read,build,main,close", quick.Text, "start: a quick start never says it is slow");

        var closed = Steps(false);
        closed.Sequence.Begin();
        closed.Sequence.LoadingShown();
        closed.Sequence.Cancel();
        closed.Sequence.MainBuilt();
        closed.Sequence.ReadingDone();
        closed.Sequence.Elapsed(60);
        Equal("loading,read,build", closed.Text, "start: a loading window the person closed opens nothing after it");
        Check(!closed.Sequence.MainShown, "start: closed during the start: no main window");

        var early = Steps();
        early.Sequence.Begin();
        early.Sequence.Cancel();
        early.Sequence.LoadingShown();
        Equal("loading", early.Text, "start: closed before it was drawn: no slow work starts");

        // ---- the code that runs the order (static checks of the sources) -------------------------------------
        string dir = Path.Combine(root, @"tools\win\amdgpu_wddm_control\src");
        string program = File.ReadAllText(Path.Combine(dir, "Program.cs")), launch = File.ReadAllText(Path.Combine(dir, "Launch.cs"));
        int claim = program.IndexOf("SingleInstance.Claim()", StringComparison.Ordinal), run = program.IndexOf("Application.Run(new Launch(guard))", StringComparison.Ordinal);
        Check(claim > 0 && run > claim, "static check: the window path claims the window before it builds anything");
        Check(!program.Contains("Application.Run(new MainForm())"), "static check: no path opens the main window without the loading window");
        Check(program.IndexOf("guard.Signal()", StringComparison.Ordinal) > claim && program.IndexOf("WindowFront.AllowAny()", StringComparison.Ordinal) > claim,
            "static check: a later start hands on the right to the front and signals the first one");
        int update = launch.IndexOf("_loading.Update();", StringComparison.Ordinal), post = launch.IndexOf("BeginInvoke((Action)_sequence.LoadingShown)", StringComparison.Ordinal);
        Check(update > 0 && post > update, "static check: the slow work is posted after the loading window is drawn");
        int w0 = launch.IndexOf("sealed class LoadingWindow", StringComparison.Ordinal), w1 = launch.IndexOf("sealed class ActivityStrip", StringComparison.Ordinal);
        string window = w0 > 0 && w1 > w0 ? launch.Substring(w0, w1 - w0) : "";
        Check(window.Length > 0, "static check: the loading window is in Launch.cs");
        foreach (var slow in new[] { "Kmd.", "WindowReading", "Inventory", "RecoveryProbe", "UpdateCheck", "BenchLocator", "AppPrefs" })
            Check(!window.Contains(slow), "static check: the loading window reads nothing from the computer (" + slow + ")");
        // The body of a background task goes through Crash.Watched, so that an exception in it is recorded and the
        // window lives on (BD-174, G-SRC in GuiTests.cs holds every such body to it). The reading is inside that
        // guard, inside Task.Run: off the user-interface thread either way.
        Check(launch.Contains("Task.Run(() => Crash.Watched(\"start reading\", () => WindowReading.Read(prefs, false)))"),
            "static check: the reading runs off the user-interface thread, inside the guard");

        // ---- the guard: one window per session --------------------------------------------------------------
        string name = "amdgpu-wddm-control-test-" + Process.GetCurrentProcess().Id + "-" + Guid.NewGuid().ToString("N").Substring(0, 8);
        using (var heard = new SemaphoreSlim(0))
        {
            using (var first = SingleInstance.Claim(name))
            {
                Check(first.First, "guard: the first start owns the window");
                Check(first.MutexName == @"Local\" + name && first.EventName.StartsWith(@"Local\", StringComparison.Ordinal),
                    "guard: both names are in the namespace of the person's own session");
                using (var second = SingleInstance.Claim(name))
                {
                    Check(!second.First, "guard: a second start in the same session does not own the window");
                    Check(second.Signal(), "guard: the second start reaches the first one");
                    second.Listen(() => heard.Release());     // a later start never listens
                }
                // The first start is still loading: it listens only now, and the signal from before is not lost.
                first.Listen(() => heard.Release());
                Check(heard.Wait(5000), "guard: a signal sent while the first start was still loading is not lost");
                Check(!heard.Wait(300), "guard: one signal brings the window forward once");

                // A second process: this test program in the mode of a second start.
                var exe = System.Reflection.Assembly.GetExecutingAssembly().Location;
                using (var p = Process.Start(new ProcessStartInfo(exe, "--single-instance-second " + name) { UseShellExecute = false, CreateNoWindow = true }))
                {
                    bool ended = p.WaitForExit(20000);
                    if (!ended) p.Kill();
                    Check(ended, "guard: the second process ends by itself");
                    if (ended) Equal(0, p.ExitCode, "guard: the second process finds the first one and signals it (3: it owned the window, 4: no signal)");
                }
                Check(heard.Wait(5000), "guard: the first start hears the second process");
                Check(!heard.Wait(300), "guard: one start of another process, one activation");
            }
            using (var next = SingleInstance.Claim(name))
                Check(next.First, "guard: after the first start ends, the next start owns the window again");
        }
    }

    // The second process of the guard test: 0 when it found the first start and signalled it.
    static int SecondStart(string name)
    {
        using (var g = SingleInstance.Claim(name))
        {
            if (g.First) return 3;
            return g.Signal() ? 0 : 4;
        }
    }
}
