// The picture on the start screen: the driver draws the mascot into the firmware's framebuffer while Windows
// starts, so the start screen is not only a spinning ring on black (docs/design/boot-logo.md, the driver's
// BootLogo value under HKLM\SYSTEM\CurrentControlSet\Services\bc250kmd\Parameters).
//
// The driver reads the value when it starts, so a change applies at the next restart of Windows. On is the
// release default, so "on" removes the value and "off" writes 0 - the same shape as FanLoadBoost, and for the
// same reason: a removed value reads as absent, which is the default, and no install can undo the person's
// choice by writing the default back over it.
//
// Pure: no registry access here. RecoveryProbe reads the value into the snapshot, Recovery.Plan plans the write,
// and the elevated helper writes it.
namespace AmdgpuWddmControl
{
    public static class BootLogoSetting
    {
        public const string ValueName = "BootLogo";

        // What the driver does when the value is absent: it draws the picture.
        public const bool ReleaseDefault = true;

        // The stored value read as a yes or no. Absent is on; anything other than 0 is on, because the driver
        // itself treats only the exact value 0 as off.
        public static bool On(uint? stored) { return stored == null || stored.Value != 0; }

        public static string Describe(uint? stored)
        {
            return On(stored) ? Strings.T("display.boot-logo.state-on") : Strings.T("display.boot-logo.state-off");
        }

        public static readonly string[] Actions = { "boot-logo-on", "boot-logo-off" };

        // Fills the plan for one of the two actions. The return value is the refusal, or null when the plan stands.
        public static string Fill(string action, RecoverySnapshot s, ActionPlan p)
        {
            bool on = action == "boot-logo-on";
            p.Title = on ? "Show the picture on the start screen" : "Do not show the picture on the start screen";
            p.Change = on ? "the driver draws the picture while Windows starts, from its next start"
                : "the driver leaves the start screen as Windows draws it, from its next start";
            p.Effect = "at the next restart of Windows";
            p.Undoable = true;
            if (on == On(s.P(ValueName)))
            {
                p.PlainRefusal = Strings.T(on ? "display.boot-logo.refuse.on-already" : "display.boot-logo.refuse.off-already");
                return on ? "The picture is shown on the start screen already." : "The picture is not shown on the start screen already.";
            }
            if (on) p.Writes.Add(RegWrite.Remove(Recovery.ParametersPath, ValueName));
            else p.Writes.Add(RegWrite.Dword(Recovery.ParametersPath, ValueName, 0));
            p.Preview.Add(Strings.T(on ? "plan.line.boot-logo-on" : "plan.line.boot-logo-off"));
            p.OfferRestart = true;
            return null;
        }
    }
}
