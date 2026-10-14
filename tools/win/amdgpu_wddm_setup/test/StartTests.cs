// Host tests of setup's start (src/StartSequence.cs, src/LoadingWindow.cs, the --loading-handover argument): the
// loading window comes before every slow step, the administrator prompt and the unpacking come after it is drawn, a
// problem is told before the loading window closes, the setup window opens before it closes, and a person who closes
// the loading window during the unpacking stops the unpacking first. No window: the order runs against a recorder.
using System;
using System.Collections.Generic;
using System.IO;
using AmdgpuWddmSetup;

static partial class UnitTests
{
    sealed class StartLog : IStartSteps
    {
        public readonly List<string> Calls = new List<string>();
        public bool RunFree = true;
        public void ShowLoading() { Calls.Add("loading"); }
        public void HandOver() { Calls.Add("handover"); }
        public void Inspect() { Calls.Add("inspect"); }
        public void SayPermission() { Calls.Add("say-permission"); }
        public void Elevate() { Calls.Add("elevate"); }
        public void WaitHandover() { Calls.Add("wait"); }
        public bool ClaimRun() { Calls.Add("claim"); return RunFree; }
        public void SayUnpacking() { Calls.Add("say-unpacking"); }
        public void Unpack() { Calls.Add("unpack"); }
        public void StopUnpacking() { Calls.Add("stop"); }
        public void ShowSetup() { Calls.Add("setup"); }
        public void Tell(string messageId) { Calls.Add("tell " + messageId); }
        public void CloseLoading() { Calls.Add("close"); }
        public string Text { get { return string.Join(",", Calls); } }
    }

    // A sequence whose loading window is drawn and whose file was looked at.
    static StartSequence Started(StartLog log, bool needsElevation, bool carries)
    {
        var s = new StartSequence(log);
        s.Begin();
        s.LoadingShown();
        s.Inspected(needsElevation, carries);
        return s;
    }

    static void StartTests(string repo)
    {
        // ---- the loading window first, the slow work only after it is drawn ------------------------------------
        var log = new StartLog();
        var s = new StartSequence(log);
        s.Begin();
        Equal("loading", log.Text, "start: the loading window is the first step");
        s.Inspected(true, true);
        s.Elevated(true);
        s.HandedOver();
        s.Unpacked(null, false);
        Equal("loading", log.Text, "start: nothing reads the file, asks Windows or unpacks before the loading window is drawn");
        s.LoadingShown();
        s.LoadingShown();
        s.Begin();
        Equal("loading,handover,inspect", log.Text, "start: when it is drawn, the copy that started this one hears of it, then the file is looked at, once");

        // ---- a run that needs administrator rights -------------------------------------------------------------
        log = new StartLog();
        s = Started(log, true, true);
        Equal("loading,handover,inspect,say-permission,elevate", log.Text, "start: the window says what Windows asks before it asks");
        s.Elevated(true);
        Equal("loading,handover,inspect,say-permission,elevate,wait", log.Text, "start: the loading window waits for the elevated copy's");
        s.HandedOver();
        Equal("loading,handover,inspect,say-permission,elevate,wait,close", log.Text, "start: and closes when that copy's window is drawn");
        Equal(0, s.ExitCode, "start: a handed-over start ends with 0");
        s.HandedOver();
        s.Unpacked(null, false);
        Equal("loading,handover,inspect,say-permission,elevate,wait,close", log.Text, "start: nothing follows the handover in this copy");

        log = new StartLog();
        s = Started(log, true, false);
        s.Elevated(false);
        Equal("loading,handover,inspect,say-permission,elevate,tell elevation.declined,close", log.Text, "start: a declined prompt is told, then the window closes");
        Equal(5, s.ExitCode, "start: a declined prompt ends with 5, as before");

        // ---- a run that unpacks the single file -------------------------------------------------------------------
        log = new StartLog();
        s = Started(log, false, true);
        Equal("loading,handover,inspect,claim,say-unpacking,unpack", log.Text, "start: the lock, then the unpacking with the loading window showing it");
        s.Unpacked(null, false);
        Equal("loading,handover,inspect,claim,say-unpacking,unpack,setup,close", log.Text, "start: the setup window opens, then the loading window closes");
        Equal(StartPhase.Window, s.Phase, "start: the setup window runs");
        Equal(0, s.ExitCode, "start: and the start's own code is 0");
        Check(s.CloseRequested(), "start: the loading window of a finished start may close");

        log = new StartLog { RunFree = false };
        s = Started(log, false, true);
        Equal("loading,handover,inspect,claim,tell payload.already-running,close", log.Text, "start: another unpacking run is told and nothing is unpacked");
        Equal(8, s.ExitCode, "start: another run ends with 8, as before");

        foreach (var problem in new[] { "payload.damaged", "payload.folder" })
        {
            log = new StartLog();
            s = Started(log, false, true);
            s.Unpacked(problem, false);
            Equal("loading,handover,inspect,claim,say-unpacking,unpack,tell " + problem + ",close", log.Text, "start: " + problem + " is told, no setup window");
            Equal(7, s.ExitCode, "start: " + problem + " ends with 7, as before");
        }

        // The person closes the loading window while the files are unpacked: the unpacking stops first, once, and the
        // window closes when it has, with no setup window after it.
        log = new StartLog();
        s = Started(log, false, true);
        Check(!s.CloseRequested(), "start: the loading window stays while the unpacking stops");
        Check(!s.CloseRequested(), "start: a second close changes nothing");
        Equal("loading,handover,inspect,claim,say-unpacking,unpack,stop", log.Text, "start: the unpacking is told to stop, once");
        s.Unpacked(null, true);
        Equal("loading,handover,inspect,claim,say-unpacking,unpack,stop,close", log.Text, "start: the window closes when the unpacking stopped");
        Equal(0, s.ExitCode, "start: a start the person stopped ends with 0");
        log = new StartLog();
        s = Started(log, false, true);
        s.CloseRequested();
        s.Unpacked(null, false);
        Equal("loading,handover,inspect,claim,say-unpacking,unpack,stop,close", log.Text, "start: an unpacking that ended at the moment of the close opens no setup window");

        // A run with its files beside it (the zip, a prepared folder) and the rights it needs: the setup window.
        log = new StartLog();
        s = Started(log, false, false);
        Equal("loading,handover,inspect,setup,close", log.Text, "start: no payload, no prompt: the setup window opens");

        // Closed while the file is looked at: nothing follows.
        log = new StartLog();
        s = new StartSequence(log);
        s.Begin();
        s.LoadingShown();
        Check(s.CloseRequested(), "start: the loading window may close while the file is looked at");
        s.Inspected(true, true);
        s.Inspected(false, true);
        Equal("loading,handover,inspect", log.Text, "start: a closed start asks nothing and unpacks nothing");

        // ---- the code that runs the order (static checks of the sources) -------------------------------------
        string dir = Path.Combine(repo, @"tools\win\amdgpu_wddm_setup\src");
        string program = File.ReadAllText(Path.Combine(dir, "Program.cs")), loading = File.ReadAllText(Path.Combine(dir, "LoadingWindow.cs"));
        Check(program.Contains("Application.Run(start)") && !program.Contains("Application.Run(new SetupForm(") && !program.Contains("Payload.Extract()"),
            "static check: the window path runs through the loading window, and Main unpacks nothing itself");
        int update = loading.IndexOf("_loading.Update();", StringComparison.Ordinal), post = loading.IndexOf("BeginInvoke((Action)_sequence.LoadingShown)", StringComparison.Ordinal);
        Check(update > 0 && post > update, "static check: the slow work is posted after the loading window is drawn");
        int i0 = loading.IndexOf("public void Inspect()", StringComparison.Ordinal), i1 = loading.IndexOf("public void SayPermission()", StringComparison.Ordinal);
        string inspect = i0 > 0 && i1 > i0 ? loading.Substring(i0, i1 - i0) : "";
        int run = inspect.IndexOf("Task.Run(", StringComparison.Ordinal), present = inspect.IndexOf("Payload.Present()", StringComparison.Ordinal);
        Check(run > 0 && present > run && loading.Contains("Task.Run(() => Payload.Extract(Payload.Root, true, progress, cancel))") &&
            loading.Contains("Task.Run(() => Native.RelaunchElevated(argv, owner))"),
            "static check: the file, the prompt and the unpacking run off the user-interface thread");

        // ---- the handover argument ----------------------------------------------------------------------------
        var name = SetupArgs.HandoverPrefix + "0123456789abcdef0123456789abcdef";
        var a = SetupArgs.Parse(new[] { "--loading-handover", name });
        Check(a.Error == null && a.Handover == name && a.Mode == "welcome", "args: --loading-handover with a name of this program");
        a = SetupArgs.Parse(new[] { "--repair", "--loading-handover", name });
        Check(a.Error == null && a.Mode == "repair", "args: --loading-handover with another mode");
        foreach (var bad in new[] { null, "", "x", SetupArgs.HandoverPrefix, SetupArgs.HandoverPrefix + "0123456789ABCDEF0123456789ABCDEF",
            SetupArgs.HandoverPrefix + "0123456789abcdef0123456789abcde", @"Global\" + name, "..\\" + name })
            Check(SetupArgs.Parse(bad == null ? new[] { "--loading-handover" } : new[] { "--loading-handover", bad }).Error != null,
                "args: --loading-handover refuses " + (bad == null ? "a missing name" : bad.Length == 0 ? "(empty)" : bad));
        Check(SetupArgs.Parse(new[] { "--smoke-payload", "d", "--loading-handover", name }).Error != null, "args: --loading-handover only with a window");
        a = SetupArgs.Parse(new[] { "--smoke-loading", "d" });
        Check(a.Error == null && a.Mode == "smoke-loading" && a.SmokeDir == "d" && a.Headless, "args: --smoke-loading is a headless entry");
        Check(SetupArgs.Parse(new[] { "--smoke-loading" }).Error != null, "args: --smoke-loading needs a folder");
    }
}
