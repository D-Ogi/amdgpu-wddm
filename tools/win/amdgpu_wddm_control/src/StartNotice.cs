// The notice the person sees at sign-in when the graphics driver is not running, and the rule that decides which
// notice it is (owner, 2026-10-13: "Uzytkownik musi byc tego swiadomym od razu"). Pure: no window, no registry, no
// driver. NoticeForm.cs draws it, Program.cs (--start-notice) reads the computer and calls Decide here.
//
// Why a notice exists at all. The driver counts every start that nothing confirmed (driver/kmd/guard.c
// UnconfirmedStarts); at BC250_MAX_UNCONFIRMED_STARTS it refuses to start itself and writes LastStage 90, and
// Windows draws the screen with Microsoft Basic Display from then on. That is a safe state, and it is also a silent
// one: games stop working, everything is slower, and nothing on the screen says why or how to get back. The notice
// says both, in the words of an ordinary user, in the session of the person who just signed in.
//
// Two cases, because they need two different answers:
//   Guard    the driver's own guard refused the start (LastStage 90, or the count has reached the limit and the
//            driver is not running). One click puts it back: clear the count and start the device again
//            (the enable-driver action of Recovery.cs).
//   Failed   the driver is installed and did not start for some other reason. There is nothing one click can fix
//            here, so the notice sends the person to the control application, whose Recovery page names the step
//            and the reason the driver recorded.
// Everything else (no driver installed, a driver that answers, a reading that failed) shows nothing: a notice that
// appears when the machine is healthy is a notice people learn to close without reading.
//
// Why the reading is retried. The confirmation task and the window task both start at the logon, which is early:
// a driver that is still inside its own start answers nothing yet, and a notice from that moment would be false.
// The caller therefore polls (NoticeProbeSeconds, NoticePollMs) and keeps the notice only while the state stays
// bad. A refused start is already decided in the registry, but it costs nothing to let it pass the same poll.
using System;

namespace AmdgpuWddmControl
{
    // What the machine says about the driver's start, as the notice needs it. Read: the driver's settings key could
    // be read at all; without that nothing is known and nothing is shown.
    public sealed class StartReading
    {
        public bool Read;                       // the settings key answered
        // The driver's settings key is there: Services\bc250kmd\Parameters, which the INF writes with the service
        // itself (the AddReg of its AddService, UnconfirmedStarts among the values), so it exists from the install
        // on and not only from the first start of the driver image. This is the same reading of "installed" that
        // RecoverySnapshot.DriverInstalled uses, so the notice, the Home banner and the support view agree.
        public bool Installed;
        public bool Running;                    // the driver answered a reading of its own state
        public uint? UnconfirmedStarts;         // Parameters\UnconfirmedStarts, null when absent
        public uint? LastStage;                 // Parameters\LastStage, null when absent
        public string DriverError;              // why the driver gave no answer; null when it did
    }

    public enum StartNoticeCase
    {
        None = 0,       // nothing to tell the person
        Guard = 1,      // the driver's guard refused the start: Windows is on its basic display
        Failed = 2,     // the driver did not start for another reason
    }

    public static class StartNotice
    {
        // How long the caller keeps asking before it believes that the driver is not running, and how often. A
        // driver that starts normally answers long before this; the two numbers are here so that the window, the
        // tests and the documentation name one bound.
        public const int ProbeSeconds = 30, PollMs = 3000;

        // The one rule. It is the rule Recovery.Describe and the Home card apply as well (Recovery.GuardRefused),
        // so the notice, the banner and the support view never disagree about the same machine.
        public static StartNoticeCase Decide(StartReading r)
        {
            if (r == null || !r.Read || !r.Installed) return StartNoticeCase.None;
            if (r.Running) return StartNoticeCase.None;
            return (r.UnconfirmedStarts ?? 0) >= Recovery.MaxUnconfirmedStarts || r.LastStage == Recovery.StageGuardRefused
                ? StartNoticeCase.Guard : StartNoticeCase.Failed;
        }

        // The window's title and body for one case, in the window's language.
        public static string Title(StartNoticeCase c)
        {
            return Strings.T(c == StartNoticeCase.Guard ? "notice.guard.title" : "notice.failed.title");
        }

        public static string Text(StartNoticeCase c)
        {
            return Strings.T(c == StartNoticeCase.Guard ? "notice.guard.text" : "notice.failed.text");
        }

        // Case A offers the one-click repair; case B does not, because nothing here knows what to repair.
        public static bool OffersEnable(StartNoticeCase c) { return c == StartNoticeCase.Guard; }
    }

    // The one write the enable-driver action makes on the driver's settings: the count of starts that nothing
    // confirmed back to zero, flushed, and read again. Pure over IRegApi (CuRegistry.cs), so the tests inject the
    // native return codes and the elevated helper uses Win32RegApi on the live key.
    //
    // This value is deliberately NOT in Recovery.ParameterNames: an undo must never be able to put a refusal count
    // back on a machine, and no other action may touch this counter. The reset therefore runs here, with its own
    // narrow rule, exactly as the graphics-core setter does.
    public sealed class GuardResetResult
    {
        public uint? Before, After;
        public bool Written;                    // the value reads back as 0
        public string Error;                    // null when the whole sequence worked
        public override string ToString()
        {
            return "guard count " + (Before == null ? "absent" : Before.Value.ToString(System.Globalization.CultureInfo.InvariantCulture)) +
                " -> " + (After == null ? "absent" : After.Value.ToString(System.Globalization.CultureInfo.InvariantCulture)) +
                (Written ? "" : "; NOT written") + (Error == null ? "" : "; " + Error);
        }
    }

    public static class GuardReset
    {
        public const string ValueName = "UnconfirmedStarts";

        // Reads the count, writes 0, flushes the key and reads the value again. A value that is absent counts as
        // written: the driver reads an absent value as zero, which is what this action wants. Nothing throws.
        public static GuardResetResult Run(IRegApi api)
        {
            var r = new GuardResetResult();
            if (api == null) { r.Error = "no registry access"; return r; }
            try
            {
                r.Before = ReadCount(api);
                int s = api.SetDword(ValueName, 0);
                if (s != CheckedCuRegistry.Success) { r.Error = new CuRegistryException("set", ValueName, s).Message; return r; }
                s = api.Flush();
                if (s != CheckedCuRegistry.Success) { r.Error = new CuRegistryException("flush", null, s).Message; return r; }
                r.After = ReadCount(api);
                r.Written = r.After == null || r.After.Value == 0;
                if (!r.Written) r.Error = "the count reads back as " + r.After.Value.ToString(System.Globalization.CultureInfo.InvariantCulture);
                return r;
            }
            catch (Exception e) { r.Error = e.Message; return r; }
        }

        // The count as a number, or null when the value is absent, of another type or unreadable. The caller only
        // reports this number, so an unreadable value is not an error of its own.
        public static uint? ReadCount(IRegApi api)
        {
            uint type; byte[] data;
            if (api.Query(ValueName, out type, out data) != CheckedCuRegistry.Success) return null;
            return type == 4 && data != null && data.Length == 4 ? (uint?)BitConverter.ToUInt32(data, 0) : null;
        }
    }
}
