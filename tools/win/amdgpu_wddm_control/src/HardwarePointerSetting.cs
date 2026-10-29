// The hardware mouse pointer (docs/design/hardware-pointer.md): the display engine draws the pointer (the DCN cursor)
// instead of Windows drawing it into every frame. Its switch is the REG_DWORD EnableHardwarePointer under the driver's
// Parameters key. Bit 0 is the pointer; bits 1 and 2 are A/B switches of the lab. The driver reads the value once, in
// DriverEntry, so a change applies at the next restart of Windows. Absent, or a value of another type, is on: the
// driver then uses 1.
//
// Why the app writes the raw value and not through the driver state client: the value is not a setting of the state
// catalog (driver/state/catalog.json). A catalog entry changes the catalog identity, and every release component that
// pins the catalog would have to be built again for one switch. So the value goes the way TdrDelay goes: read and
// written as a plain registry value by the elevated helper, with its backup, read-back and undo. The state client
// would refuse a name that is not in its catalog. For the same reason the driver writes no Features record for it,
// and the window cannot tell which value this start uses: the row says that a change works after a restart.
//
// "On" removes the value and "off" writes 0, the shape of DriverFeatures: the default is on, and no installation can
// undo the choice by writing a default over it.
//
// Pure: no registry access here. RecoveryProbe reads the value into the snapshot, Recovery.Plan plans the write, and
// the elevated helper writes it.
namespace AmdgpuWddmControl
{
    public static class HardwarePointerSetting
    {
        public const string RegistryPath = DpmSettings.RegistryPath;
        public const string ValueName = "EnableHardwarePointer";
        public const string Id = "hw-pointer";                      // graphics.<Id>, help.setting.<Id>
        public const string ActionOn = "hw-pointer-on", ActionOff = "hw-pointer-off";
        public const uint OnBit = 1;                                // BC250_CURSOR_SETTING_ON, driver/kmd/dcn_cursor_shape.h

        public static readonly string[] Actions = { ActionOn, ActionOff };

        // The one value of the Parameters key that the app reads and writes raw, past the state client.
        public static bool Allowed(string path, string name)
        {
            return string.Equals(path, RegistryPath, System.StringComparison.OrdinalIgnoreCase) && name == ValueName;
        }

        // The stored choice as a yes or no, the way the driver reads it: absent is on, else bit 0.
        public static bool On(uint? stored) { return stored == null || (stored.Value & OnBit) != 0; }

        // The window shows the switch wherever the driver is installed: the driver reads the value on every start.
        public static bool Shown(RecoverySnapshot s) { return s != null && s.DriverInstalled; }

        // The line under the check box: the value that is stored now, read back from the registry.
        public static string StateText(RecoverySnapshot s)
        {
            if (s.HardwarePointerError != null) return Strings.T("graphics.hw-pointer.unreadable");
            var stored = s.HardwarePointer();
            if (stored == null) return Strings.T("graphics.hw-pointer.now.default");
            return Strings.T(On(stored) ? "graphics.hw-pointer.now.on" : "graphics.hw-pointer.now.off");
        }

        // Fills the plan for one of the two actions. The return value is the refusal, or null when the plan stands.
        public static string Fill(string action, RecoverySnapshot s, ActionPlan p)
        {
            bool on = action == ActionOn;
            p.Title = on ? "Turn the hardware mouse pointer on" : "Turn the hardware mouse pointer off";
            p.Change = on ? "the display engine draws the mouse pointer, from the driver's next start"
                : "Windows draws the mouse pointer into the picture, from the driver's next start";
            p.Effect = "at the next restart of Windows";
            p.Undoable = true;
            if (s.HardwarePointerError != null)
            {
                p.PlainRefusal = Strings.T("graphics.hw-pointer.refuse.unreadable");
                return "The hardware pointer setting cannot be read, so it is not changed: " + s.HardwarePointerError;
            }
            if (on == On(s.HardwarePointer()))
            {
                p.PlainRefusal = Strings.T(on ? "graphics.feature.refuse.on-already" : "graphics.feature.refuse.off-already");
                return on ? ValueName + " is on already." : ValueName + " is off already.";
            }
            if (on) p.Writes.Add(RegWrite.Remove(RegistryPath, ValueName));
            else p.Writes.Add(RegWrite.Dword(RegistryPath, ValueName, 0));
            p.Preview.Add(Strings.T("plan.line." + action));
            p.OfferRestart = true;
            return null;
        }
    }
}
