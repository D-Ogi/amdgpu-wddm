// How long Windows waits for the graphics before it resets them: the Windows value TdrDelay (seconds) under
// HKLM\SYSTEM\CurrentControlSet\Control\GraphicsDrivers. Microsoft documents it in the display section of the
// Windows driver documentation ("Testing and debugging TDR during driver development", tdr-registry-keys.md):
// REG_DWORD, "the number of seconds that the GPU can delay the preempt request from the GPU scheduler", default
// 2 seconds. Windows reads it when it starts, so a change applies at the next restart of Windows.
//
// Why this driver owns it (owner, 2026-10-08; BD-079): this part has no working GPU reset, so a picture that needs
// more than the waiting time does not end as a reset engine but as a stopped machine. The lab has run 10 seconds
// since 2026-09-28; a tester machine kept Windows' 2 seconds, and the release notes asked the tester to write the
// value by hand. The release now writes 10 itself (the installer's defaults table, group graphics_drivers), and this
// app shows and changes it.
//
// Three rules hold the installer's promise "The installation keeps the settings that you changed":
//   1. The chosen number is always written, never removed. A removed value would read as absent at the next
//      install, which writes the release default again and would undo the person's choice.
//   2. Every choice of this app is inside Min..Max, so the installer's "a value outside the table is the tester's"
//      rule keeps it.
//   3. A stored value this app does not offer is shown as it is and is only replaced when the person chooses.
//
// Pure: no registry access here. RecoveryProbe reads the value into the snapshot, Recovery.Plan plans the write
// through PlanWrites, and the elevated helper writes it.
using System;
using System.Collections.Generic;
using System.Globalization;
using System.Linq;

namespace AmdgpuWddmControl
{
    public static class TdrSetting
    {
        public const string RegistryPath = @"SYSTEM\CurrentControlSet\Control\GraphicsDrivers";
        public const string ValueName = "TdrDelay";

        // Windows' own waiting time when the value is absent (tdr-registry-keys.md, TdrDelay).
        public const uint WindowsDefault = 2;
        // What this release writes and what the lab runs.
        public const uint ReleaseDefault = 10;
        // The documentation gives no limit for TdrDelay. Under 2 seconds is below what Windows itself uses, and a
        // minute is far past any frame this part can draw, so nothing outside 2..60 is offered or accepted here.
        public const uint Min = 2, Max = 60;

        // The waiting times offered, shortest first. WindowsDefault and ReleaseDefault are both in the list, so the
        // person can always go back to either.
        public static readonly uint[] Choices = { 2, 5, 10, 20, 30, 60 };

        public static bool Allowed(string path, string name)
        {
            return string.Equals(path, RegistryPath, StringComparison.OrdinalIgnoreCase) && name == ValueName;
        }

        public static bool IsValid(uint seconds) { return seconds >= Min && seconds <= Max; }

        // A number from anywhere (a stored value, a command line) brought inside the range.
        public static uint Clamp(uint seconds) { return seconds < Min ? Min : seconds > Max ? Max : seconds; }

        // What Windows waits now: the stored value, or Windows' own when nothing is stored. A stored value outside
        // the range is what Windows reads, so it is reported as it is and not clamped here.
        public static uint Effective(uint? stored) { return stored ?? WindowsDefault; }

        // The entries of the choice list. Seconds null is the state of a machine where nothing is stored: it is offered
        // only while that is the state, so that picking a number is always a real change, including the number Windows
        // uses by itself (a person who wants 2 seconds has to store 2, or the next installation writes the release
        // default over an absent value).
        public sealed class Item
        {
            public uint? Seconds;
            public string Text;
        }

        public static List<Item> Items(uint? stored)
        {
            var list = new List<Item>();
            if (stored == null) list.Add(new Item { Seconds = null, Text = Strings.T("tdr.choice.not-set", Number(WindowsDefault)) });
            var numbers = Choices.ToList();
            if (stored != null && !numbers.Contains(stored.Value)) { numbers.Add(stored.Value); numbers.Sort(); }
            foreach (var n in numbers) list.Add(new Item { Seconds = n, Text = Label(n) });
            return list;
        }

        // The entry the list shows as chosen: the person's unsaved choice, else what is stored, else "not set".
        public static int Selected(List<Item> items, uint? stored, uint? edit)
        {
            var want = edit ?? stored;
            for (int i = 0; i < items.Count; i++) if (items[i].Seconds == want) return i;
            return 0;
        }

        // The label of one waiting time, in the window's language. tdr.choice.windows is used for WindowsDefault only
        // and tdr.choice.recommended for ReleaseDefault only, so a translation of those two may use the grammatical
        // form that belongs to that one number (Polish "2 sekundy", "10 sekund").
        public static string Label(uint seconds)
        {
            if (seconds == WindowsDefault) return Strings.T("tdr.choice.windows", Number(seconds));
            if (seconds == ReleaseDefault) return Strings.T("tdr.choice.recommended", Number(seconds));
            return Strings.T("tdr.choice.seconds", Number(seconds));
        }

        public static string Number(uint seconds) { return seconds.ToString(CultureInfo.InvariantCulture); }

        // The line under the choice: what is in force now, and whether it is a value this app does not offer.
        public static string StateText(uint? stored)
        {
            if (stored == null) return Strings.T("tdr.now.absent", Number(WindowsDefault));
            if (!IsValid(stored.Value)) return Strings.T("tdr.now.not-valid", Number(stored.Value));
            return Strings.T("tdr.now", Number(stored.Value));
        }

        // The one write of a change, or an empty list when the stored value is the wanted one already.
        public static List<RegWrite> PlanWrites(uint? stored, uint wanted)
        {
            if (!IsValid(wanted)) throw new ArgumentException("The waiting time must be between " + Min + " and " + Max + " seconds.");
            var writes = new List<RegWrite>();
            if (stored != null && stored.Value == wanted) return writes;
            writes.Add(RegWrite.Dword(RegistryPath, ValueName, wanted));
            return writes;
        }

        // The English sentence of the plan, for the log, the dry run and the support report.
        public static string Describe(uint? stored, uint wanted)
        {
            return "Sets " + ValueName + " = " + Number(wanted) + ": Windows waits " + Number(wanted) +
                " seconds for the graphics before it resets them (it waits " +
                (stored == null ? Number(WindowsDefault) + " by itself" : Number(stored.Value)) + " now).";
        }
    }
}
