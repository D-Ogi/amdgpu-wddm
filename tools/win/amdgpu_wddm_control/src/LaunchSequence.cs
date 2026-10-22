// The order of the window's start. Launch.cs does the steps; this class decides when, so a host test can hold the order
// without a window.
//
// What the start costs, measured without a window on the PC that builds this (2026-10-14, a PC with no BC-250): the
// runtime 0.27 s before Main, the main window's constructor 0.44 s on its first run (0.02 s on a second one: the rest
// is the compilation of its code), and the reading of the computer 0.67 s, most of it one WMI query for the device
// (Inventory.Read, 0.5 s) and the scheduled task through COM and WMI (RecoveryProbe.Read, 0.13 to 0.19 s). All of it
// ran before the window was first drawn, and all of the reading on the thread that draws it. On the BC-250 every
// driver question is a real one, and a WMI service that has gone idle starts its provider again first.
//
// The order:
//   1. the loading window, before anything slow (Program.Main shows it before the reading and before the main
//      window's constructor);
//   2. when it is on the screen: the reading starts off the user-interface thread (WindowReading.Read) and the main
//      window is built on it, both at the same time;
//   3. when both are done: the main window opens with the reading, and the loading window closes after it, so there
//      is always a window of this program on the screen.
// A start that is not done after SlowSeconds says so in the loading window and offers to close it. It never opens the
// main window without the reading: a window built from nothing would report a driver that is not there.
namespace AmdgpuWddmControl
{
    public interface ILaunchSteps
    {
        void ShowLoading();
        void StartReading();
        void BuildMain();
        void ShowMain();
        void CloseLoading();
        void SaySlow();
    }

    public sealed class LaunchSequence
    {
        public const double SlowSeconds = 8;

        readonly ILaunchSteps _steps;
        bool _begun, _loadingShown, _read, _built, _mainShown, _slow, _cancelled;

        public LaunchSequence(ILaunchSteps steps) { _steps = steps; }

        // Whether the main window is open: a later start of the program brings the main window forward from then on,
        // and the loading window before it.
        public bool MainShown { get { return _mainShown; } }

        public void Begin()
        {
            if (_begun) return;
            _begun = true;
            _steps.ShowLoading();
        }

        // The loading window is on the screen: the slow work may start now, and not before.
        public void LoadingShown()
        {
            if (!_begun || _loadingShown || _cancelled) return;
            _loadingShown = true;
            _steps.StartReading();
            _steps.BuildMain();
        }

        public void ReadingDone() { if (!_loadingShown) return; _read = true; Finish(); }

        public void MainBuilt() { if (!_loadingShown) return; _built = true; Finish(); }

        // The person closed the loading window: nothing opens after it.
        public void Cancel() { if (!_mainShown) _cancelled = true; }

        // seconds since Begin, from the loading window's timer.
        public void Elapsed(double seconds)
        {
            if (_slow || _mainShown || _cancelled || !_loadingShown || seconds < SlowSeconds) return;
            _slow = true;
            _steps.SaySlow();
        }

        void Finish()
        {
            if (_mainShown || _cancelled || !_read || !_built) return;
            _mainShown = true;
            _steps.ShowMain();
            _steps.CloseLoading();
        }
    }
}
