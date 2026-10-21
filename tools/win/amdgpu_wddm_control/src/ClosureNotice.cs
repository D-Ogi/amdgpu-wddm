// BD-181: what the driver closed by itself after an unclean end, the plain-words notice about it, and the one
// action that opens all of it again.
//
// The rule is the release installer's, value for value: tools/release/installer/common.ps1, $script:DriverClosures
// and Get-DriverClosure. A value counts as closed by the driver only when
//   - the installed release names a default for it (manifest.json "defaults"), that default is in range, and it is
//     not the value the driver writes when it closes: where the release default IS that value, nothing was taken
//     away and there is nothing to reopen;
//   - the value in the key is the one the driver writes when it closes (0 for all three here);
//   - a record of the driver's own sits next to it: the durable record with any reason other than 0, or, for the
//     clock of a driver before 0.7.208.1, the legacy record with one of the reasons of a fallback.
// A 0 that a person wrote by hand has no such record, so it is a choice and nothing here touches it (BD-069, and
// the rule the owner asked for in BD-181: a closure the user chose is never reverted).
//
// What the driver writes at a start after an unclean end (driver/kmd/interop_policy.c bc250_interop_decide,
// driver/shim/bc250_dpm.c bc250_dpm_decide): EnableGpuPresentBlit 0, EnableCddDwmInterop 0, InteropClosedReason 4,
// DpmMode 0, DpmClosedReason 4. The driver deletes a record by itself at the next start that asks for the thing
// again (clear_closed), so writing the default is enough, and the notice goes with the default at once because
// Find wants the closed value. The action deletes only what the state catalog lets an administrator delete
// (driver/state/catalog.json): InteropClosedReason (access admin_clear) goes after the values; DpmClosedReason
// (access "driver") stays for the driver, because the state client refuses that delete and a refused delete would
// fail the whole action (lab, 2026-10-21).
//
// Not here: the graphics cores. The driver closes them with a CuMode of 24 and the release default of tester.31 is
// 24 as well, so Get-DriverClosure returns nothing for CuMode (the CuMode note in common.ps1) and a repair install
// reopens nothing there either. The CU values also go through the CU setter and stay outside Recovery.Allowed on
// purpose, so this action may not write them; the Graphics page is the way back to All (40).
using System;
using System.Collections.Generic;
using System.Globalization;
using System.Linq;

namespace AmdgpuWddmControl
{
    // One value the driver closed, with everything the plan and the notice need.
    public sealed class DriverClosure
    {
        public string Name;                 // the value the driver closed
        public uint Closed;                 // the value it writes when it closes
        public uint Default;                // the installed release's default for it
        public string Subject;              // what a person notices: ClosureNotice.Desktop or ClosureNotice.Clock
        public string Record;               // the record that named this closure
        public uint Code;                   // its reason
        public string[] Clear;              // the records this reopen deletes (empty: the record belongs to the last start)

        public override string ToString()
        {
            return Name + " " + Closed + " -> " + Default + " (" + Record + " " + Code.ToString(CultureInfo.InvariantCulture) + ")";
        }
    }

    // The notice card of Home in the window's language (WU-007, R1): ordinary words, no driver internals.
    public sealed class ClosureCard
    {
        public string Title, What, Cost, Effect, Action, ActionLabel;
        public string StatusId;                                         // the status item that carries the verdict
        public readonly List<string> Items = new List<string>();        // one line per thing that was closed
        public readonly List<DriverClosure> Closures = new List<DriverClosure>();

        // Every sentence of the card, for the no-internals gate and for the window's redraw check.
        public IEnumerable<string> Texts
        {
            get { return new[] { Title, What, Cost, Effect, ActionLabel }.Concat(Items).Where(t => t != null); }
        }
    }

    public static class ClosureNotice
    {
        public const string Desktop = "desktop", Clock = "clock";
        public const string ActionName = "reopen-closed";

        sealed class Rule
        {
            public string Name, Record, Legacy, Subject;
            public uint Closed;
            public uint[] Defaults;         // the values a release default may have for this name
            public uint[] LegacyReasons;    // the reasons of a fallback, the only ones a legacy record proves
            public string[] Clear;
        }

        // The three values of $script:DriverClosures that this app may write (Recovery.ParameterNames), in the order
        // the notice reads: the desktop first, because that is what a person sees.
        static readonly Rule[] Rules =
        {
            new Rule { Name = "EnableGpuPresentBlit", Closed = 0, Defaults = new uint[] { 0, 1 }, Subject = Desktop,
                Record = "InteropClosedReason", Clear = new[] { "InteropClosedReason" } },
            new Rule { Name = "EnableCddDwmInterop", Closed = 0, Defaults = new uint[] { 0, 1 }, Subject = Desktop,
                Record = "InteropClosedReason", Clear = new[] { "InteropClosedReason" } },
            // DpmLastReason is the reason of the last start, so only the reasons of a fallback prove a closure there
            // (3 unconfirmed, 4 unclean, 8 the firmware refused the clock); common.ps1 says why that is sound.
            // The clock's record is the driver's alone (catalog access "driver"): it deletes it at the next start
            // that reads a DpmMode other than 0, so nothing is cleared here.
            new Rule { Name = "DpmMode", Closed = 0, Defaults = new uint[] { 0, 1 }, Subject = Clock,
                Record = "DpmClosedReason", Legacy = "DpmLastReason", LegacyReasons = new uint[] { 3, 4, 8 },
                Clear = new string[0] },
        };

        // Every closure the driver made itself, in the order of the table. Empty when nothing is closed, when the
        // reading failed or when the installed release names no defaults (without them nothing says what the value
        // should be, and the app never invents one: reset-defaults refuses for the same reason).
        public static List<DriverClosure> Find(RecoverySnapshot s)
        {
            var found = new List<DriverClosure>();
            if (s == null || s.ReadFailed || !s.DriverInstalled || s.DefaultParameters == null) return found;
            foreach (var r in Rules)
            {
                long def;
                if (!s.DefaultParameters.TryGetValue(r.Name, out def)) continue;
                if (def < 0 || def > uint.MaxValue || !r.Defaults.Contains((uint)def)) continue;
                if ((uint)def == r.Closed) continue;                    // the release wants the closed value: nothing was taken away
                var now = s.P(r.Name);
                if (now == null || now.Value != r.Closed) continue;
                var durable = r.Record == null ? null : s.P(r.Record);
                if (durable != null && durable.Value != 0)
                {
                    found.Add(Closure(r, (uint)def, r.Record, durable.Value, r.Clear));
                    continue;
                }
                if (r.Legacy == null) continue;
                var legacy = s.P(r.Legacy);
                if (legacy == null || !r.LegacyReasons.Contains(legacy.Value)) continue;
                // A repair deletes nothing there: the record is the reason of the last start and that start owns it.
                found.Add(Closure(r, (uint)def, r.Legacy, legacy.Value, new string[0]));
            }
            return found;
        }

        static DriverClosure Closure(Rule r, uint def, string record, uint code, string[] clear)
        {
            return new DriverClosure { Name = r.Name, Closed = r.Closed, Default = def, Subject = r.Subject, Record = record, Code = code, Clear = clear };
        }

        // The subjects of a set of closures, each once, in the table's order.
        public static List<string> Subjects(IEnumerable<DriverClosure> closures)
        {
            var list = new List<string>();
            foreach (var c in closures ?? Enumerable.Empty<DriverClosure>())
                if (!list.Contains(c.Subject)) list.Add(c.Subject);
            return list;
        }

        // The notice card, or null when the driver closed nothing. Plain words only: what happened, what it costs
        // and the one button that reverts every closure.
        public static ClosureCard Card(RecoverySnapshot s)
        {
            var closures = Find(s);
            if (closures.Count == 0) return null;
            var subjects = Subjects(closures);
            var card = new ClosureCard
            {
                Title = Strings.T("home.closed.title"),
                What = Strings.T("home.closed.what"),
                Cost = Strings.T("home.closed.cost"),
                Effect = Strings.T("home.closed.effect"),
                Action = ActionName,
                ActionLabel = Strings.T("status.action." + ActionName),
                StatusId = subjects.Contains(Desktop) ? "gpu-desktop-closed" : "clock-closed",
            };
            card.Closures.AddRange(closures);
            foreach (var subject in subjects) card.Items.Add(Strings.T("home.closed." + subject));
            return card;
        }

        // The technical one-liner of the support view and the log: what is closed and what named it.
        public static string Text(IEnumerable<DriverClosure> closures)
        {
            var list = (closures ?? Enumerable.Empty<DriverClosure>()).ToList();
            return list.Count == 0 ? "nothing closed by the driver" : string.Join(", ", list.Select(c => c.ToString()));
        }
    }
}
