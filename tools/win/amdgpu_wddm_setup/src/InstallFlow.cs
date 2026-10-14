// What the installer does without asking (owner, 2026-10-12: "Przestan wymagac potwierdzen od uzytkownika").
//
// Pressing Install in the window, or running install.cmd on the command line, IS the consent for what an
// installation of this driver needs: Windows test mode, and a pause of BitLocker protection for the restarts the
// install makes. The window has no check box, no option button and no greyed-out Install waiting for an answer; the
// plan screen states these changes as information, and the engine does them by itself.
//
// This file holds the rules of that behaviour as plain functions, so the unit tests hold them too: which arguments a
// run carries, which sentences the plan screen states, and how long the countdown of the restart screen is. The
// engine's own side of the same decision is Get-BitLockerPlan in tools/release/installer/common.ps1.
using System.Collections.Generic;

namespace AmdgpuWddmSetup
{
    public static class InstallFlow
    {
        // The countdown of the restart screen, in seconds: it runs down and Windows restarts at its end, whether
        // anybody presses anything or not. "Restart now" shortens it, "Later" calls it off; neither is required.
        public const int RestartCountdownSeconds = 15;

        // The BitLocker states the engine reports in its decision ('off', 'on', 'suspended', 'unknown').
        public const string BitLockerOn = "on", BitLockerUnknown = "unknown";

        // Protection that has to be paused before the start options change. Only a state that was read and says
        // "on": 'off' and 'suspended' need nothing, and a state nobody could read is not pausable (the engine says
        // so in one line and goes on, because a person who cannot read the state cannot answer a question about it
        // either).
        public static bool SuspendBitLocker(string state) { return state == BitLockerOn; }

        // The arguments of an install run that used to carry the user's answers. They are given on every run now:
        // the engine of this package no longer needs them, and an engine of an older release - which a repair set
        // kept on the PC may still hold - reads them and asks nothing either.
        public static string[] ConsentArguments(string bitLockerState)
        {
            var args = new List<string> { "-AcceptTestSigning" };
            if (SuspendBitLocker(bitLockerState)) { args.Add("-BitLocker"); args.Add("Suspend"); }
            return args.ToArray();
        }

        // The sentences the plan screen states about the way Windows starts, in the order it shows them. Information,
        // never a question.
        //   testModeOff: the plan found Windows test mode off, so the install turns it on (check testsigning.inactive)
        //   bitLockerState: the state the engine's decision reports
        //   vcRuntimeMissing: the plan found the Microsoft Visual C++ runtime missing or too old, so the install
        //                     installs it from the file the package carries (check vcruntime.missing, owner
        //                     2026-10-12: "powinien byc auto-installer!!!")
        public static string[] PlanChangeIds(bool testModeOff, string bitLockerState, bool vcRuntimeMissing = false)
        {
            var ids = new List<string>();
            if (!testModeOff) ids.Add("plan.change.test-mode.on");
            else
            {
                ids.Add("plan.change.test-mode");
                if (bitLockerState == BitLockerOn) ids.Add("plan.change.bitlocker");
                else if (bitLockerState == BitLockerUnknown) ids.Add("plan.change.bitlocker.unknown");
            }
            if (vcRuntimeMissing) ids.Add("plan.change.vcruntime");
            return ids.ToArray();
        }

        // The shortcuts of the control application (owner, 2026-10-12: "pytajmy tez czy zrobic skrot na pulpicie
        // (domyslnie i menu start i pulpit)"). Both are on, and neither answer holds the install back: the switches
        // below only choose where the shortcuts go. An answer the window shows is stated on the command line, so
        // this run decides over the answer an earlier run of the same install saved.
        // An answer nobody stated (null) passes no switch, exactly as the Windows tuning does: the run that
        // continues an install after a restart has no screen and no answer of its own, and a positive switch would
        // clear the saved negative one, which would put back a shortcut the tester turned off.
        // Get-ControlShortcutPlan in installer\common.ps1 is the engine's side.
        public static string[] ShortcutArguments(bool? startMenu, bool? desktop)
        {
            var args = new List<string>();
            if (startMenu.HasValue) args.Add(startMenu.Value ? "-StartMenuShortcut" : "-NoStartMenuShortcut");
            if (desktop.HasValue) args.Add(desktop.Value ? "-DesktopShortcut" : "-NoDesktopShortcut");
            return args.ToArray();
        }

        // The Nagi wallpaper (owner, 2026-10-13): set once for each person after the install is verified, unless the
        // box is cleared. The same rule as the shortcuts: an answer nobody stated passes no switch. The engine only
        // records the answer (install.ps1, Release\Wallpaper); Nagi Control App sets the picture.
        public static string[] WallpaperArguments(bool? wallpaper)
        {
            return wallpaper.HasValue ? new[] { wallpaper.Value ? "-Wallpaper" : "-NoWallpaper" } : new string[0];
        }

        // The picture on the start screen (owner, 2026-10-14), on in the same way. The same null rule carries the
        // choice of an earlier install over a reinstall or an upgrade: an answer nobody stated passes no switch, and
        // the engine then leaves the value the installed driver already has (Get-BootLogoPlan in installer\common.ps1).
        public static string[] BootLogoArguments(bool? bootLogo)
        {
            return bootLogo.HasValue ? new[] { bootLogo.Value ? "-BootLogo" : "-NoBootLogo" } : new string[0];
        }

        // The options the plan screen may carry, as string ids. They are choices between good outcomes, not
        // permissions: each has a default, and the Install button never waits for one (G-CONSENT allows exactly
        // these controls and no other).
        public static readonly string[] PlanOptionIds = new[] { "tuning.opt-in", "shortcuts.start-menu", "shortcuts.desktop", "shortcuts.wallpaper", "shortcuts.boot-logo" };

        // The options that are on when the screen opens: a person who installs the driver wants to find it
        // afterwards. The optional Windows tuning is not one of them.
        public static readonly string[] PlanOptionsOnByDefault = new[] { "shortcuts.start-menu", "shortcuts.desktop", "shortcuts.wallpaper", "shortcuts.boot-logo" };

        // The ids of the controls the plan screen must never carry again: the test-signing check box, the two
        // BitLocker option buttons and the sentence that told the user to answer them. A table that still has words
        // for one of them is a finding of the unit tests, so the words cannot come back without the control.
        public static readonly string[] RemovedConsentIds = new[]
        {
            "consent.title", "consent.test-signing", "consent.test-signing.note",
            "consent.bitlocker", "consent.bitlocker.have-key", "consent.bitlocker.suspend",
            "plan.consent.needed",
        };
    }
}
