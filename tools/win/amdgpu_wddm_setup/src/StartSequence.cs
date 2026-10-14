// The order of setup's start (owner, 2026-10-14: "instalator z exe także ładuje się dość długo - także potrzebuje od
// razu okienka ładowania"). LoadingWindow.cs does the steps; this class decides them, so a host test can hold the
// order without a window.
//
// What ran before the first window of the single file, measured without a window on the PC that builds this
// (2026-10-14, the 131 MB file of 0.7.216.100-tester.31-rc3 that unpacks to 368 MB): the copy started by the person
// opened the file to read its trailer (Windows Defender looks at the whole file at its first opening, up to 1.2 s)
// and asked for administrator rights, and the prompt of Windows came when Windows had looked at the file. The copy
// started as administrator then hashed the package, unpacked it and hashed every file again, 4 to 12 s in all
// (Defender looks at every new file), and only then built the setup window. Nothing was on the screen meanwhile.
//
// The order now:
//   1. the loading window, before anything reads the file (Program.Main shows it first);
//   2. when it is drawn: a copy started as administrator tells the copy that started it that its own loading window
//      is on the screen (HandOver), and the file is looked at off the user-interface thread (Inspect: whether it
//      carries the package, whether this run needs administrator rights);
//   3a. a run that needs administrator rights says what Windows is about to ask, starts the elevated copy (Elevate)
//       and keeps its loading window until the loading window of that copy is on the screen (WaitHandover);
//   3b. a run that unpacks takes the one-run-at-a-time lock (ClaimRun), then unpacks and checks off the user-interface
//       thread while the loading window shows how far it is (Unpack);
//   4. the setup window opens, and the loading window closes after it.
// A problem is told while the loading window is still there (the message belongs to it), and the window closes after
// it. A person who closes the loading window while the files are unpacked stops the unpacking first: the window stays
// until the half-written folder is removed.
namespace AmdgpuWddmSetup
{
    public interface IStartSteps
    {
        void ShowLoading();
        void HandOver();
        void Inspect();
        void SayPermission();
        void Elevate();
        void WaitHandover();
        bool ClaimRun();
        void SayUnpacking();
        void Unpack();
        void StopUnpacking();
        void ShowSetup();
        void Tell(string messageId);
        void CloseLoading();
    }

    public enum StartPhase { None, Loading, Inspecting, Elevating, HandingOver, Unpacking, Window, Ended }

    public sealed class StartSequence
    {
        readonly IStartSteps _steps;
        bool _stopping;

        public StartSequence(IStartSteps steps) { _steps = steps; }

        public StartPhase Phase { get; private set; }

        // What the process returns: 0 for a window or a handed-over start, 5 when the administrator prompt was
        // declined, 7 for a package that is damaged or cannot be unpacked safely, 8 when another run unpacks already
        // (the codes Program.Main returned before the loading window).
        public int ExitCode { get; private set; }

        public void Begin()
        {
            if (Phase != StartPhase.None) return;
            Phase = StartPhase.Loading;
            _steps.ShowLoading();
        }

        // The loading window is on the screen: the slow work may start now, and not before.
        public void LoadingShown()
        {
            if (Phase != StartPhase.Loading) return;
            Phase = StartPhase.Inspecting;
            _steps.HandOver();
            _steps.Inspect();
        }

        // needsElevation: this run must run as administrator and does not. carries: it installs from the package
        // inside the file.
        public void Inspected(bool needsElevation, bool carries)
        {
            if (Phase != StartPhase.Inspecting) return;
            if (needsElevation)
            {
                Phase = StartPhase.Elevating;
                _steps.SayPermission();
                _steps.Elevate();
                return;
            }
            if (carries)
            {
                if (!_steps.ClaimRun()) { End("payload.already-running", 8); return; }
                Phase = StartPhase.Unpacking;
                _steps.SayUnpacking();
                _steps.Unpack();
                return;
            }
            OpenWindow();
        }

        // started: the elevated copy runs; false when the person declined the prompt.
        public void Elevated(bool started)
        {
            if (Phase != StartPhase.Elevating) return;
            if (!started) { End("elevation.declined", 5); return; }
            Phase = StartPhase.HandingOver;
            _steps.WaitHandover();
        }

        // The elevated copy's loading window is on the screen, or that copy ended, or the wait's bound passed.
        public void HandedOver()
        {
            if (Phase != StartPhase.HandingOver) return;
            End(null, 0);
        }

        // problemId: null when the package is ready, else the message to tell. cancelled: the unpacking stopped
        // because the person closed the loading window.
        public void Unpacked(string problemId, bool cancelled)
        {
            if (Phase != StartPhase.Unpacking) return;
            if (cancelled || _stopping) { End(null, 0); return; }
            if (problemId != null) { End(problemId, 7); return; }
            OpenWindow();
        }

        // The person closes the loading window. True when it may close now; while the files are unpacked the
        // unpacking stops first and the window closes when it has (Unpacked).
        public bool CloseRequested()
        {
            switch (Phase)
            {
                case StartPhase.Unpacking:
                    if (!_stopping) { _stopping = true; _steps.StopUnpacking(); }
                    return false;
                case StartPhase.Window:
                case StartPhase.Ended:
                    return true;
                default:
                    Phase = StartPhase.Ended;
                    ExitCode = 0;
                    return true;
            }
        }

        void OpenWindow()
        {
            Phase = StartPhase.Window;
            _steps.ShowSetup();
            _steps.CloseLoading();
        }

        void End(string messageId, int exitCode)
        {
            Phase = StartPhase.Ended;
            ExitCode = exitCode;
            if (messageId != null) _steps.Tell(messageId);
            _steps.CloseLoading();
        }
    }
}
