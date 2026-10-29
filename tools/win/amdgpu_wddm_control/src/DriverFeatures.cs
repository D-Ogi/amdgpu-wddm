// Two switches of the driver itself, on the Graphics page: the compute queue (WDDM node 2, a second hardware queue
// for Vulkan compute work, docs/design/compute-node.md) and the hardware acceleration of classic Windows drawing (GDI,
// docs/design/gdi-hw-accel.md). Both are settings of the driver state with an installation default of 1
// (driver/state/catalog.json: EnableComputeQueue, EnableGdiHwAccel). The driver reads them when it starts, so a change
// applies at the next restart of Windows. "On" removes the person's value, because the installation default is on;
// "off" writes 0: the shape of BootLogo and FanLoadBoost, so that no install can undo the choice by writing the
// default back over it.
//
// The driver says at every start whether a switch has an effect on this computer, in the volatile records
// Features\ComputeQueue and Features\GdiHwAccel: 1 on, 2 off by the switch, 0 not possible on this start (another
// gate is closed, or the driver has no GDI programs). The window shows a switch only for 1 or 2. A driver without the
// record writes nothing, so the window shows nothing there.
//
// Pure: no registry access here. RecoveryProbe reads the values into the snapshot, Recovery.Plan plans the write,
// and the elevated helper writes it.
using System.Linq;

namespace AmdgpuWddmControl
{
    public sealed class DriverFeature
    {
        public string Id;           // the window's id: graphics.<Id>, help.setting.<Id>, plan.title.<action>
        public string ValueName;    // the setting in the driver state (root of Parameters)
        public string FeatureName;  // the volatile record under Features
        public string ActionOn, ActionOff;
        public string TitleOn, TitleOff, ChangeOn, ChangeOff;    // English, for the log and the support report
    }

    public static class DriverFeatures
    {
        public const string FeaturePath = "Features";
        public const uint FeatureOn = 1, FeatureOffBySetting = 2;

        public static readonly DriverFeature Compute = new DriverFeature
        {
            Id = "compute", ValueName = "EnableComputeQueue", FeatureName = "ComputeQueue",
            ActionOn = "compute-queue-on", ActionOff = "compute-queue-off",
            TitleOn = "Turn the second queue for calculations on", TitleOff = "Turn the second queue for calculations off",
            ChangeOn = "the driver opens its second hardware queue (node 2) for Vulkan calculations, from its next start",
            ChangeOff = "the driver keeps its second hardware queue (node 2) closed, from its next start",
        };

        public static readonly DriverFeature Gdi = new DriverFeature
        {
            Id = "gdi", ValueName = "EnableGdiHwAccel", FeatureName = "GdiHwAccel",
            ActionOn = "gdi-accel-on", ActionOff = "gdi-accel-off",
            TitleOn = "Turn hardware acceleration of classic Windows drawing on",
            TitleOff = "Turn hardware acceleration of classic Windows drawing off",
            ChangeOn = "the graphics chip does the classic Windows drawing (GDI), from the driver's next start",
            ChangeOff = "the processor does the classic Windows drawing (GDI), from the driver's next start",
        };

        public static readonly DriverFeature[] All = { Compute, Gdi };

        public static readonly string[] ValueNames = All.Select(f => f.ValueName).ToArray();

        public static readonly string[] Actions = All.SelectMany(f => new[] { f.ActionOn, f.ActionOff }).ToArray();

        public static DriverFeature Of(string action) { return All.FirstOrDefault(f => action == f.ActionOn || action == f.ActionOff); }

        // The stored choice as a yes or no. Absent is on (the installation default); only the exact value 0 is off,
        // because the driver itself treats only 0 as off.
        public static bool On(uint? stored) { return stored == null || stored.Value != 0; }

        // What the driver wrote for this start, or null when it wrote nothing (an older driver, or no start yet).
        public static uint? Running(RecoverySnapshot s, DriverFeature f) { return s.Feature(f.FeatureName); }

        // Whether the switch has an effect on this computer: the window shows it, and the helper plans it, only then.
        public static bool Shown(RecoverySnapshot s, DriverFeature f)
        {
            var v = Running(s, f);
            return v == FeatureOn || v == FeatureOffBySetting;
        }

        // The stored choice and this start differ: the change waits for a restart of Windows.
        public static bool Pending(RecoverySnapshot s, DriverFeature f)
        {
            var v = Running(s, f);
            return Shown(s, f) && On(s.P(f.ValueName)) != (v == FeatureOn);
        }

        // Whether settings search may list the entry: the entry of a switch goes only where the window shows the
        // switch, so that no search leads to a page without it. Every other entry stays.
        public static bool Searchable(RecoverySnapshot s, string entryId)
        {
            if (entryId == "graphics." + HardwarePointerSetting.Id) return HardwarePointerSetting.Shown(s);
            return !All.Any(f => entryId == "graphics." + f.Id && !(s != null && Shown(s, f)));
        }

        // Fills the plan for one of the four actions. The return value is the refusal, or null when the plan stands.
        public static string Fill(string action, RecoverySnapshot s, ActionPlan p)
        {
            var f = Of(action);
            bool on = action == f.ActionOn;
            p.Title = on ? f.TitleOn : f.TitleOff;
            p.Change = on ? f.ChangeOn : f.ChangeOff;
            p.Effect = "at the next restart of Windows";
            p.Undoable = true;
            if (!Shown(s, f))
            {
                p.PlainRefusal = Strings.T("graphics.feature.refuse.unavailable");
                return "The running driver has no use for " + f.ValueName + " on this computer (Features\\" + f.FeatureName + " is not 1 or 2).";
            }
            if (on == On(s.P(f.ValueName)))
            {
                p.PlainRefusal = Strings.T(on ? "graphics.feature.refuse.on-already" : "graphics.feature.refuse.off-already");
                return on ? f.ValueName + " is on already." : f.ValueName + " is off already.";
            }
            if (on) p.Writes.Add(RegWrite.Remove(Recovery.ParametersPath, f.ValueName));
            else p.Writes.Add(RegWrite.Dword(Recovery.ParametersPath, f.ValueName, 0));
            p.Preview.Add(Strings.T("plan.line." + (on ? f.ActionOn : f.ActionOff)));
            p.OfferRestart = true;
            return null;
        }
    }
}
