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
        public static string[] PlanChangeIds(bool testModeOff, string bitLockerState)
        {
            var ids = new List<string>();
            if (!testModeOff) { ids.Add("plan.change.test-mode.on"); return ids.ToArray(); }
            ids.Add("plan.change.test-mode");
            if (bitLockerState == BitLockerOn) ids.Add("plan.change.bitlocker");
            else if (bitLockerState == BitLockerUnknown) ids.Add("plan.change.bitlocker.unknown");
            return ids.ToArray();
        }

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
