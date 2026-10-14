// The graphics-core (CU) setting of the Performance page (GUI plan v7 section 7, WU-042, WU-052): three things kept
// apart at all times - the user's stored choice (CuMode), the prediction for the next start (from the choice and the
// KMD's own marks), and the running start (the CU READ snapshot of the current start). Pure functions; the elevated
// helper (RecoveryActions.cs) runs the setter plan through ICuRegistry, so the same code is tested here with injected
// failures.
//
// Sources (bc250-win 1b75781a): bc250_cu_decide (driver/shim/bc250_cu_mode.c:59-103), the KMD's fallback and CONFIRM
// (driver/kmd/cumode.c), the escape (driver/kmd/bc250kmd_escape.h BC250_ESCAPE_CU_MODE), the operator's reference
// setter (tools/win/cumode/cumode.cpp:264-273).
using System;
using System.Collections.Generic;
using System.Globalization;
using System.Linq;

namespace AmdgpuWddmControl
{
    public enum CuClass { Unknown, Standard, Waiting, Confirmed, ConfirmedFewer, NotSaved, Fallback, Refused }

    // The next start as bc250_cu_decide will take it: Refused = the KMD refuses the stored values (INVALID_SETTING or
    // INVALID_DISABLE) and starts with 24.
    public enum CuNext { Unknown, Standard, Refused, Fallback, Ask40, Confirmed40 }

    // The stored choice, the "Your choice" line.
    public enum CuChoice { Unknown, Standard24, All40, Invalid }

    // The next-start prediction in full: the mode the KMD applies, the cores that start, the class that start will
    // have, its reason and its marks. Null from CuMode.Prediction when nothing can be predicted.
    public sealed class CuPrediction
    {
        public CuNext Next;
        public uint Mode, ActiveCus, Reason;
        public CuClass State;
        public bool Pending, Confirmed;
    }

    // The values the setter and the prediction look at. Null: absent. Unreadable: a value that exists but could
    // not be read as a REG_DWORD (or the key could not be read): nothing is predicted from it. NotDurable: a setter run
    // of this boot could not flush or verify its writes (cu-unflushed.json, plan 497 decision 1): the values read now
    // may not be what the next start reads, so the next start is not predicted until a restart or a completed run.
    // Closed is CuModeClosedReason, the record the driver writes next to a CuMode = 24 of its own fallback
    // (driver/kmd/cumode.c PersistFallback, BD-077): with it, a stored 24 is the driver's act, not a choice of the
    // user, and the page says so instead of calling it "Your choice".
    // Chosen is CuModeChosen, the mirror of that record for the person: the encoded request (mode | disable << 8)
    // that a person asked for, written by this setter and by install.cmd -CuMode. The release installer reads it
    // (Get-UserChoice, tools/release/installer/common.ps1) and leaves CuMode alone while it matches the two values
    // next to it. Without it a release default of 24 would take a chosen 40 or a chosen map away at every upgrade,
    // which is BD-077 with the sign reversed. The driver neither reads nor writes it.
    public sealed class CuStored
    {
        public uint? Mode, Disable, Confirmed, Pending, Closed, Chosen;
        public bool Unreadable, NotDurable;

        public CuStored Copy() { return new CuStored { Mode = Mode, Disable = Disable, Confirmed = Confirmed, Pending = Pending, Closed = Closed, Chosen = Chosen, Unreadable = Unreadable, NotDurable = NotDurable }; }

        public override string ToString()
        {
            Func<uint?, string> v = x => x == null ? "absent" : x.Value.ToString(CultureInfo.InvariantCulture) + " (0x" + x.Value.ToString("X", CultureInfo.InvariantCulture) + ")";
            return Unreadable ? "unreadable" : "CuMode " + v(Mode) + ", CuDisableWgp " + v(Disable) + ", CuModeConfirmed " + v(Confirmed) + ", CuModePending " + v(Pending)
                + ", CuModeClosedReason " + v(Closed) + ", CuModeChosen " + v(Chosen);
        }
    }

    // What the page shows for the setting: each line already in the window's language.
    public sealed class CuView
    {
        public CuClass Class;
        public CuNext Next;
        public CuChoice Stored;
        public string Choice, Running, NextStart, Note;
        public bool OfferConfirm, OfferTry40, OfferChoose24, OfferReport;
        public bool DriverChose24;          // a stored 24 with the driver's own record next to it (CuModeClosedReason)
        public uint ChoiceMode;             // 24, 40, or 0 when the stored value is not valid or unreadable
        public uint ChoiceCores;            // the cores the stored choice gives (24..40), 0 when ChoiceMode is 0
        public uint ChoiceDisable;          // the WGPs the stored choice keeps masked, 0 for the stock choice
    }

    // The outcome of a setter run or a [Confirm now], as one word for the log and the tests.
    public enum CuTransaction { NotApplicable, Complete, Failed, PartialFailure }

    public interface ICuRegistry
    {
        // null when the value is absent; throws when it cannot be read or is not a REG_DWORD.
        uint? Read(string name);
        void Delete(string name);
        void SetDword(string name, uint value);
        void Flush();
    }

    public sealed class CuStep
    {
        // remove-confirmed, remove-disable, write-disable, write-40, write-24, remove-closed, write-chosen,
        // remove-chosen, verify-stock, flush-readback
        public string Kind;
        public string Name;      // the registry value, null for verify/flush
        public uint Value;
        public bool Changes { get { return Kind != "verify-stock" && Kind != "flush-readback"; } }

        public override string ToString()
        {
            string where = @"HKLM\" + CuMode.ParametersPath + " ";
            string hex = "0x" + Value.ToString("X", CultureInfo.InvariantCulture);
            switch (Kind)
            {
                case "write-40": return "S1 set " + where + "CuMode = 40 (DWord)";
                case "write-24": return "S1 set " + where + "CuMode = 24 (DWord)";
                case "remove-closed": return "S1b delete " + where + "CuModeClosedReason";
                case "write-chosen": return "S1c set " + where + "CuModeChosen = " + hex + " (DWord)";
                case "remove-chosen": return "S1c delete " + where + "CuModeChosen";
                case "remove-confirmed": return "S2 delete " + where + "CuModeConfirmed";
                case "remove-disable": return "S3 delete " + where + "CuDisableWgp";
                case "write-disable": return "S3 set " + where + "CuDisableWgp = " + hex + " (DWord)";
                case "verify-stock": return "S1 flush the key, read back CuMode and verify the stock selection (24)";
                default: return "S4 flush the key, read back CuMode, CuModeConfirmed, CuDisableWgp and CuModeChosen";
            }
        }
    }

    public sealed class CuSetterPlan
    {
        public uint Target;                              // 24 or 40
        public uint Disable;                             // the WGPs 40 keeps masked; 0 for the stock target
        // Whether the run records the request as a person's choice (CuModeChosen). True for the window and the
        // command line; false for "reset to defaults", which hands the setting back to the installed release.
        public bool Record = true;
        public string Refusal;                           // the helper refuses before any step
        public readonly List<CuStep> Steps = new List<CuStep>();
        public readonly List<string> Preview = new List<string>();     // plain-language lines, one per changing step
        public CuStored Before;
        public bool Refused { get { return Refusal != null; } }
        public uint Cores { get { return Target == CuMode.Full ? CuMode.ActiveCus(Disable) : CuMode.Stock; } }
    }

    public sealed class CuSetterResult
    {
        // FlushFailed: a flush of the run failed; VerifyFailed: the read that verifies the stock selection failed.
        // Either way the run established no durable state (plan 497 decisions 1 and 2).
        public bool Completed, AnyStepRan, FlushFailed, VerifyFailed;
        public int FailedStep = -1;                      // index into the plan's steps
        public string Error;
        public CuStored ReadBack;                        // null when the read-back failed
        public readonly List<string> Log = new List<string>();
    }

    public static class CuMode
    {
        public const string ParametersPath = DpmSettings.RegistryPath;
        public const uint Stock = 24, Full = 40;
        public const uint AllowedDisable = 0xFFFFF;      // 4 shader arrays x 5 WGPs (BC250_CU_WGP_MAX), the BC-250's topology
        // The BC-250's topology, as docs/design/cu-mode.md records it: four shader arrays of five WGPs, one WGP is
        // two compute units, and the firmware leaves WGPs 0, 1 and 2 of every array active (stock CC 0xFFF80000,
        // stock SPI 0x7). The bit of a WGP in CuDisableWgp is sa * WgpPerSa + wgp.
        public const int SaCount = 4, WgpPerSa = 5, CuPerWgp = 2;
        public const uint StockActiveWgps = 0x7;         // per shader array, the firmware's active set

        // The cores a count may have: 24 to 40 in twos, because one WGP is two cores and the hardware has no
        // smaller step. 24 is the stock set, which needs no unlock at all.
        public static readonly uint[] CoreChoices = { 24, 26, 28, 30, 32, 34, 36, 38, 40 };

        // The per-array active WGP mask the firmware of THIS board left, from the CU READ snapshot
        // (StockCc, the same rule as bc250_cu_active_wgps_cc: ~INACTIVE_WGPS of the array's own WGPs). Null when
        // no valid snapshot exists or the board is not the topology this page was written for: the map is then
        // read-only and only the documented stock set is offered.
        public static uint[] StockActive(CuModeState s)
        {
            if (s == null || !s.Has(CuModeState.FlagValid) || s.StockCc == null || s.StockCc.Length != SaCount) return null;
            var all = new uint[SaCount];
            uint wgps = (1u << WgpPerSa) - 1u;
            for (int i = 0; i < SaCount; i++)
            {
                all[i] = ~((s.StockCc[i] & 0xFFFF0000u) >> 16) & wgps;
                if (all[i] == 0) return null;
            }
            // The page's own arithmetic holds only for a board whose stock set is 24 cores and whose free WGPs are
            // the 16 cores above it (Stock and Full below). Anything else is a board this page cannot map.
            if (Cores(all) != Stock || Bits(Maskable(all)) * CuPerWgp != Full - Stock) return null;
            return all;
        }

        // The documented stock set, for a PC whose snapshot cannot be read.
        public static uint[] DocumentedStockActive()
        {
            var all = new uint[SaCount];
            for (int i = 0; i < SaCount; i++) all[i] = StockActiveWgps;
            return all;
        }

        public static uint Bits(uint v) { uint n = 0; for (; v != 0; v &= v - 1) n++; return n; }

        // The cores of a per-array WGP mask: two per WGP.
        public static uint Cores(uint[] perSa)
        {
            uint n = 0;
            foreach (var m in perSa) n += Bits(m);
            return n * CuPerWgp;
        }

        // The WGPs a person may turn off: the ones the firmware left inactive, over all arrays, as one CuDisableWgp
        // mask. A bit over a stock-active WGP is INVALID_DISABLE in the driver (bc250_cu_targets), so the window
        // never offers one.
        public static uint Maskable(uint[] stockActive)
        {
            uint m = 0, wgps = (1u << WgpPerSa) - 1u;
            for (int i = 0; i < stockActive.Length; i++) m |= (~stockActive[i] & wgps) << (i * WgpPerSa);
            return m;
        }

        // The order the default set takes WGPs away in: the highest free WGP of array 0, then of array 1, 2, 3, then
        // the next one down, round-robin, so the cores stay as even across the four arrays as the step allows.
        public static List<int> DefaultOrder(uint[] stockActive)
        {
            var order = new List<int>();
            for (int wgp = WgpPerSa - 1; wgp >= 0; wgp--)
                for (int sa = 0; sa < stockActive.Length; sa++)
                    if ((stockActive[sa] & (1u << wgp)) == 0) order.Add(sa * WgpPerSa + wgp);
            return order;
        }

        // The default CuDisableWgp of a core count (24 to 40, even). 24 is the stock set and has no mask.
        public static uint DefaultDisable(uint cores, uint[] stockActive)
        {
            uint m = 0;
            if (cores <= Stock || cores > Full) return 0;
            int take = (int)(Full - cores) / CuPerWgp;
            var order = DefaultOrder(stockActive);
            for (int i = 0; i < take && i < order.Count; i++) m |= 1u << order[i];
            return m;
        }

        // The request of a core count: the stock path for 24, else 40 with the default set of that count.
        public static bool RequestFor(uint cores, uint[] stockActive, out uint mode, out uint disable)
        {
            mode = Stock;
            disable = 0;
            if (!CoreChoices.Contains(cores)) return false;
            if (cores == Stock) return true;
            mode = Full;
            disable = DefaultDisable(cores, stockActive);
            return ActiveCus(disable) == cores;
        }

        // The request of a hand-picked map: the per-array WGPs the person left on. Every stock-active WGP has to be
        // on and nothing outside the array's own WGPs may be set; an empty extra set is the stock path.
        public static bool RequestForMap(uint[] on, uint[] stockActive, out uint mode, out uint disable)
        {
            mode = Stock;
            disable = 0;
            uint wgps = (1u << WgpPerSa) - 1u;
            if (on == null || stockActive == null || on.Length != stockActive.Length) return false;
            for (int i = 0; i < on.Length; i++)
            {
                if ((on[i] & ~wgps) != 0) return false;
                if ((stockActive[i] & ~on[i]) != 0) return false;
            }
            uint m = 0;
            for (int i = 0; i < on.Length; i++) m |= (~on[i] & wgps) << (i * WgpPerSa);
            m &= Maskable(stockActive);
            if (m == Maskable(stockActive)) return true;      // nothing above the stock set: the stock path
            mode = Full;
            disable = m;
            return true;
        }

        // The mask the map draws for a stored choice. The stock choice stores no mask at all (the driver reads a 24
        // and returns before CuDisableWgp, so the setter writes none), and a map drawn from that 0 shows every pair
        // on: 40 cores under a picker that says 24, which is what the lab arm of 2026-10-14 found. The stock choice
        // runs the firmware's own set, so every pair a person may turn on is drawn off. A choice that cannot be read
        // or that the driver would refuse draws the same stock set, because that is what the next start applies.
        public static uint ChoiceMapMask(CuView v, uint[] stockActive)
        {
            if (stockActive == null) return 0;
            uint maskable = Maskable(stockActive);
            if (v == null || v.ChoiceCores <= Stock) return maskable;
            return v.ChoiceDisable & maskable;
        }

        // The cores a map with this mask draws: the pairs the firmware keeps on are always on, the others follow the
        // mask. The map and the number above it have to agree, so this is the number the tests count.
        public static uint MapCores(uint mask, uint[] stockActive)
        {
            if (stockActive == null) return 0;
            var on = MapOf(mask, stockActive);
            var drawn = new uint[stockActive.Length];
            for (int i = 0; i < drawn.Length; i++) drawn[i] = on[i] | stockActive[i];
            return Cores(drawn);
        }

        // The per-array WGPs a 40 request with this mask leaves on, for the map.
        public static uint[] MapOf(uint disable, uint[] stockActive)
        {
            uint wgps = (1u << WgpPerSa) - 1u;
            var on = new uint[stockActive.Length];
            for (int i = 0; i < on.Length; i++) on[i] = wgps & ~((disable >> (i * WgpPerSa)) & wgps);
            return on;
        }

        // The cores of each array, "6, 8, 8, 8", for the sentence that names them.
        public static string PerSaText(uint[] on)
        {
            return string.Join(", ", on.Select(m => (Bits(m) * CuPerWgp).ToString(CultureInfo.InvariantCulture)).ToArray());
        }

        // The WGPs a mask turns off, in the words of the window: "group 1 pair 5, group 2 pair 5".
        public static string MaskText(uint disable)
        {
            var parts = new List<string>();
            for (int sa = 0; sa < SaCount; sa++)
                for (int wgp = 0; wgp < WgpPerSa; wgp++)
                    if ((disable & (1u << (sa * WgpPerSa + wgp))) != 0) parts.Add(Strings.T("cu.map.item", sa + 1, wgp + 1));
            return string.Join(", ", parts.ToArray());
        }

        // enum bc250_cu_reason, driver/shim/include/bc250_cu_mode.h.
        public const uint ReasonNone = 0, ReasonInvalidSetting = 1, ReasonInvalidDisable = 2, ReasonPendingUnconfirmed = 3, ReasonRegistry = 4,
            ReasonNotThisDevice = 5, ReasonPowerGating = 6, ReasonStockUnexpected = 7, ReasonReadback = 8, ReasonRestoreFailed = 9, ReasonNotRun = 10,
            ReasonTopology = 11;

        public static readonly string[] ValueNames = { "CuMode", "CuDisableWgp", "CuModeConfirmed", "CuModePending", "CuModeClosedReason", "CuModeChosen" };

        // The reason, for the support report only.
        public static string ReasonName(uint reason)
        {
            switch (reason)
            {
                case ReasonNone: return "NONE";
                case ReasonInvalidSetting: return "INVALID_SETTING";
                case ReasonInvalidDisable: return "INVALID_DISABLE";
                case ReasonPendingUnconfirmed: return "PENDING_UNCONFIRMED";
                case ReasonRegistry: return "REGISTRY";
                case ReasonNotThisDevice: return "NOT_THIS_DEVICE";
                case ReasonPowerGating: return "POWER_GATING";
                case ReasonStockUnexpected: return "STOCK_UNEXPECTED";
                case ReasonReadback: return "READBACK";
                case ReasonRestoreFailed: return "RESTORE_FAILED";
                case ReasonNotRun: return "NOT_RUN";
                case ReasonTopology: return "TOPOLOGY";
                default: return "reason " + reason;
            }
        }

        public static uint Encode(uint mode, uint disable) { return (mode & 0xFF) | (disable << 8); }

        // ---- the running start -------------------------------------------------------------------------------------

        // From the CURRENT valid snapshot only: the stored choice never decides it. startGeneration is the start-health
        // Generation of this start; a snapshot of another start, or no generation to check it against, is Unknown.
        public static CuClass Classify(CuModeState s, ulong? startGeneration)
        {
            if (s == null || !s.Has(CuModeState.FlagValid) || s.Applied == 0) return CuClass.Unknown;
            if (s.Reason == ReasonNotRun || s.Reason == ReasonRestoreFailed) return CuClass.Unknown;
            if (startGeneration == null || s.Generation == 0 || s.Generation != startGeneration.Value) return CuClass.Unknown;
            if (s.Applied == Stock)
                return s.Reason == ReasonNone ? CuClass.Standard : s.Reason == ReasonPendingUnconfirmed ? CuClass.Fallback : CuClass.Refused;
            if (s.Applied != Full) return CuClass.Unknown;
            if (s.Has(CuModeState.FlagPending)) return CuClass.Waiting;
            if (s.Has(CuModeState.FlagConfirmed)) return s.ActiveCus == 40 ? CuClass.Confirmed : CuClass.ConfirmedFewer;
            return CuClass.NotSaved;
        }

        // ---- the next start ----------------------------------------------------------------------------------------

        // bc250_cu_decide over the stored values, for the BC-250's topology: absent or 24 -> stock before the mask is
        // read; another mode -> INVALID_SETTING; a mask outside the topology -> INVALID_DISABLE; a live Pending -> the
        // KMD's fallback; a Confirmed that matches the request -> a confirmed 40 start; otherwise a 40 start that asks.
        public static CuNext Predict(CuStored v)
        {
            var p = Prediction(v);
            return p == null ? CuNext.Unknown : p.Next;
        }

        public static CuPrediction Prediction(CuStored v)
        {
            if (v == null || v.Unreadable || v.NotDurable) return null;
            Func<CuNext, CuClass, uint, CuPrediction> stock = (next, state, reason) =>
                new CuPrediction { Next = next, Mode = Stock, ActiveCus = Stock, State = state, Reason = reason };
            if (v.Mode == null || v.Mode == Stock) return stock(CuNext.Standard, CuClass.Standard, ReasonNone);
            if (v.Mode != Full) return stock(CuNext.Refused, CuClass.Refused, ReasonInvalidSetting);
            uint disable = v.Disable ?? 0;
            if ((disable & ~AllowedDisable) != 0) return stock(CuNext.Refused, CuClass.Refused, ReasonInvalidDisable);
            if ((v.Pending ?? 0) != 0) return stock(CuNext.Fallback, CuClass.Fallback, ReasonPendingUnconfirmed);
            uint cus = ActiveCus(disable);
            if (v.Confirmed == Encode(Full, disable))
                return new CuPrediction { Next = CuNext.Confirmed40, Mode = Full, ActiveCus = cus, State = cus == Full ? CuClass.Confirmed : CuClass.ConfirmedFewer, Confirmed = true };
            return new CuPrediction { Next = CuNext.Ask40, Mode = Full, ActiveCus = cus, State = CuClass.Waiting, Pending = true };
        }

        // The cores of a 40 start with a valid mask: each disabled WGP takes two CUs.
        public static uint ActiveCus(uint disable)
        {
            uint bits = 0;
            for (uint m = disable & AllowedDisable; m != 0; m &= m - 1) bits++;
            return Full - 2 * bits;
        }

        // A 40 start that would run with a map of its own, so the sentence names the number of cores instead of 40
        // (never for Standard: absent/24 returns before the mask is read, bc250_cu_mode.c:68-82).
        static bool Limited(CuStored v, CuNext next)
        {
            return (next == CuNext.Ask40 || next == CuNext.Confirmed40) && (v.Disable ?? 0) != 0;
        }

        // ---- the view ----------------------------------------------------------------------------------------------

        // The stored choice only: never the running start and never the prediction.
        public static CuChoice Choice(CuStored v)
        {
            if (v == null || v.Unreadable) return CuChoice.Unknown;
            if (v.Mode == null || v.Mode == Stock) return CuChoice.Standard24;
            return v.Mode == Full ? CuChoice.All40 : CuChoice.Invalid;
        }

        // A stored 24 that the driver wrote itself, with its record still next to it (BD-077). The record is the
        // driver's alone, so the page must not call that 24 a choice of the user. Reason 0 is no closure, the same
        // rule the release installer's Get-DriverClosure uses.
        public static bool DriverChose24(CuStored v)
        {
            return v != null && !v.Unreadable && v.Mode == Stock && (v.Closed ?? 0) != 0;
        }

        public static uint ChoiceOf(CuStored v)
        {
            var c = Choice(v);
            return c == CuChoice.Standard24 ? Stock : c == CuChoice.All40 ? Full : 0;
        }

        // cores: what the stored choice gives, so a 40 with a map of its own says its own number instead of "All".
        public static string ChoiceText(CuChoice c, uint cores = Full)
        {
            switch (c)
            {
                case CuChoice.Standard24: return Strings.T("cu.choice.standard");
                case CuChoice.All40: return cores == Full ? Strings.T("cu.choice.all") : Strings.T("cu.choice.some", cores);
                case CuChoice.Invalid: return Strings.T("cu.choice.invalid");
                default: return Strings.T("cu.choice.unreadable");
            }
        }

        // The cores the stored choice gives: 24 for the stock choice, and for a 40 the cores its mask leaves.
        public static uint ChoiceCores(CuStored v)
        {
            var c = Choice(v);
            if (c == CuChoice.Standard24) return Stock;
            if (c != CuChoice.All40) return 0;
            uint disable = v.Disable ?? 0;
            return (disable & ~AllowedDisable) != 0 ? Full : ActiveCus(disable);
        }

        // confirmFailed: the user pressed [Confirm now] in this start and the start is still Waiting (deleting Pending
        // failed): the Waiting sentence stays and the failure is said next to it.
        public static CuView View(CuModeState snapshot, ulong? startGeneration, CuStored stored, bool confirmFailed = false)
        {
            var v = new CuView { Class = Classify(snapshot, startGeneration), Next = Predict(stored), Stored = Choice(stored), ChoiceMode = ChoiceOf(stored),
                DriverChose24 = DriverChose24(stored), ChoiceCores = ChoiceCores(stored), ChoiceDisable = stored == null ? 0 : (stored.Disable ?? 0) };
            int cores = snapshot != null ? (int)snapshot.ActiveCus : 0;
            uint applied = v.Class == CuClass.Unknown ? 0 : snapshot.Applied;

            // Your choice, or the driver's own 24: a stored 24 with the driver's record next to it is not a choice of
            // the user (BD-077). The note names the two ways back, the same two the release installer names.
            if (v.DriverChose24)
            {
                v.Choice = Strings.T("cu.choice.driver-24");
                v.Note = Strings.T("cu.note.driver-24");
            }
            else
            {
                v.Choice = ChoiceText(v.Stored, v.ChoiceCores);
                // The restart line follows the cores, not the mode alone: 30 cores chosen on a 40-core start is
                // also a choice that waits for the restart.
                bool same = v.ChoiceMode != 0 && applied == v.ChoiceMode && (applied != Full || snapshot.ActiveCus == v.ChoiceCores);
                if (v.ChoiceMode != 0 && applied != 0 && !same) v.Choice += " " + Strings.T("cu.choice.after-restart");
            }

            // This start.
            switch (v.Class)
            {
                case CuClass.Standard: v.Running = Strings.T("cu.run.standard", cores); break;
                case CuClass.Waiting:
                    v.Running = confirmFailed ? Strings.T("cu.run.waiting-short", cores) + " " + Strings.T("cu.run.confirm-failed") : Strings.T("cu.run.waiting", cores);
                    v.OfferConfirm = true;
                    v.OfferReport = confirmFailed;
                    break;
                case CuClass.Confirmed: v.Running = Strings.T("cu.run.confirmed"); break;
                case CuClass.ConfirmedFewer: v.Running = Strings.T("cu.run.confirmed-fewer", cores); break;
                case CuClass.NotSaved:
                    // The second sentence follows the prediction, never the choice alone (review 927 R3).
                    v.Running = Strings.T("cu.run.not-saved", cores) + " " + (v.Next == CuNext.Ask40 ? Strings.T("cu.run.not-saved.next40")
                        : v.Next == CuNext.Standard ? Strings.T("cu.run.not-saved.next24") : NextText(stored, v.Next));
                    v.OfferReport = true;
                    v.OfferChoose24 = v.Stored == CuChoice.All40;
                    break;
                case CuClass.Fallback:
                    // [Try 40 again] is the Fallback state's next step whatever is stored (plan v7 section 7).
                    v.Running = Strings.T("cu.run.fallback");
                    v.OfferTry40 = true;
                    break;
                case CuClass.Refused: v.Running = Strings.T("cu.run.refused"); break;
                default: v.Running = Strings.T("cu.run.unknown"); break;
            }

            // A start that runs a set of its own names the cores of each group as well, so a person can see which
            // pairs run. ActiveWgps is what the registers read back, not what was asked for.
            if (snapshot != null && snapshot.ActiveWgps != null && snapshot.ActiveWgps.Length == SaCount
                && v.Class != CuClass.Unknown && snapshot.DisableMask != 0)
                v.Running += " " + Strings.T("cu.run.groups", PerSaText(snapshot.ActiveWgps));

            v.NextStart = NextText(stored, v.Next);
            return v;
        }

        public static string NextText(CuStored stored, CuNext next)
        {
            switch (next)
            {
                case CuNext.Standard:
                case CuNext.Refused: return Strings.T("cu.next.standard");
                case CuNext.Fallback: return Strings.T("cu.next.fallback");
                case CuNext.Ask40: return Limited(stored, next) ? Strings.T("cu.next.ask40-limited", ChoiceCores(stored)) : Strings.T("cu.next.ask40");
                case CuNext.Confirmed40: return Limited(stored, next) ? Strings.T("cu.next.confirmed40-limited", ChoiceCores(stored)) : Strings.T("cu.next.confirmed40");
                default: return Strings.T("cu.next.unknown");
            }
        }

        // ---- the setter (S1-S6) ------------------------------------------------------------------------------------

        // The plan from the stored values the helper read itself. target 24 or 40; disable the WGPs a 40 keeps
        // masked (0 for all of them on, and never anything but stock-inactive WGPs: the caller builds it from
        // RequestFor or RequestForMap). record false hands the setting back to the installed release, so the
        // person's own record goes instead of being written. Only steps that change something are listed in the
        // preview; the verify and flush steps are part of the plan the helper runs.
        public static CuSetterPlan Plan(CuStored before, uint target, uint disable = 0, bool record = true)
        {
            var p = new CuSetterPlan { Target = target, Disable = target == Full ? disable : 0, Record = record, Before = before };
            if (target != Stock && target != Full) { p.Refusal = Strings.T("cu.refuse.target"); return p; }
            if (before == null || before.Unreadable) { p.Refusal = Strings.T("cu.refuse.unreadable"); return p; }
            if ((p.Disable & ~AllowedDisable) != 0) { p.Refusal = Strings.T("cu.refuse.map"); return p; }
            uint chosen = Encode(p.Target, p.Disable);
            Func<string, string, uint, CuStep> step = (kind, name, value) => new CuStep { Kind = kind, Name = name, Value = value };
            if (target == Full)
            {
                if (before.Mode == Full && (before.Disable ?? 0) == p.Disable && (record ? before.Chosen == chosen : before.Chosen == null))
                {
                    p.Refusal = Strings.T(p.Disable == 0 ? "cu.refuse.already40" : "cu.refuse.already-some", p.Cores);
                    return p;
                }
                // S2, S3, S1, S1c, S4: a failure before S1 leaves the old mode, never a new 40 request next to an
                // old confirmation. The mask of this request is written before CuMode, so a stopped run never
                // leaves a 40 of its own next to the mask of the request before it.
                if (before.Confirmed != null) p.Steps.Add(step("remove-confirmed", "CuModeConfirmed", 0));
                if (p.Disable != 0) { if ((before.Disable ?? 0) != p.Disable) p.Steps.Add(step("write-disable", "CuDisableWgp", p.Disable)); }
                else if (before.Disable != null) p.Steps.Add(step("remove-disable", "CuDisableWgp", 0));
                if (before.Mode != Full) p.Steps.Add(step("write-40", "CuMode", Full));
                AddChosen(p, step, chosen);
                p.Steps.Add(step("flush-readback", null, 0));
            }
            else
            {
                // A stored 24 is the only state that needs no write: an absent CuMode is "never configured" to the
                // release installer, which then writes the release default over it, so the stock choice has to be
                // written out (BD-077, review 2 finding 1). The driver reads 24 and an absent value alike.
                bool stored24 = before.Mode == Stock;
                if (stored24 && before.Confirmed == null && before.Disable == null && before.Closed == null
                    && (record ? before.Chosen == chosen : before.Chosen == null)) { p.Refusal = Strings.T("cu.refuse.already24"); return p; }
                // S1, flush, verify the stock selection; only then S1b, S2, S3, S1c, S4. S1b takes the driver's own
                // fallback record away, so the 24 that stays is the user's choice and no later install reads it as
                // a closure of the driver.
                if (!stored24) p.Steps.Add(step("write-24", "CuMode", Stock));
                p.Steps.Add(step("verify-stock", null, 0));
                if (before.Closed != null) p.Steps.Add(step("remove-closed", "CuModeClosedReason", 0));
                if (before.Confirmed != null) p.Steps.Add(step("remove-confirmed", "CuModeConfirmed", 0));
                if (before.Disable != null) p.Steps.Add(step("remove-disable", "CuDisableWgp", 0));
                AddChosen(p, step, chosen);
                p.Steps.Add(step("flush-readback", null, 0));
            }
            foreach (var s in p.Steps.Where(s => s.Changes))
                switch (s.Kind)
                {
                    case "write-40": p.Preview.Add(p.Disable == 0 ? Strings.T("cu.preview.write40") : Strings.T("cu.preview.write40-some", p.Cores)); break;
                    case "write-24": p.Preview.Add(Strings.T("cu.preview.write24")); break;
                    case "remove-closed": p.Preview.Add(Strings.T("cu.preview.remove-closed")); break;
                    case "write-chosen": p.Preview.Add(Strings.T("cu.preview.write-chosen")); break;
                    case "remove-chosen": p.Preview.Add(Strings.T("cu.preview.remove-chosen")); break;
                    case "remove-confirmed": p.Preview.Add(Strings.T("cu.preview.remove-confirmed")); break;
                    case "remove-disable": p.Preview.Add(Strings.T("cu.preview.remove-disable")); break;
                    case "write-disable": p.Preview.Add(Strings.T("cu.preview.set-disable", MaskText(p.Disable))); break;
                }
            // The number of cores comes first, before the pairs the request turns off: the steps write the mask
            // before CuMode, but the person reads the number first. A request whose CuMode is 40 already has no
            // write-40 step at all and still names the number.
            if (p.Target == Full && p.Disable != 0)
            {
                string head = Strings.T("cu.preview.write40-some", p.Cores);
                p.Preview.Remove(head);
                p.Preview.Insert(0, head);
            }
            return p;
        }

        static void AddChosen(CuSetterPlan p, Func<string, string, uint, CuStep> step, uint chosen)
        {
            if (p.Record) { if (p.Before.Chosen != chosen) p.Steps.Add(step("write-chosen", "CuModeChosen", chosen)); }
            else if (p.Before.Chosen != null) p.Steps.Add(step("remove-chosen", "CuModeChosen", 0));
        }

        // The only registry operations the CU setter may make (G-PLAN, G-SRC): CuMode set to 40 or to 24,
        // CuDisableWgp set to a mask of the board's own topology, CuModeChosen set to an encoded request, and the
        // deletion of CuDisableWgp, CuModeConfirmed, CuModeClosedReason and CuModeChosen. Never CuModePending,
        // never the KMD's last-reason values, and CuMode is never deleted: the stock choice is written out (BD-077).
        public static bool Allowed(CuStep s)
        {
            switch (s.Kind)
            {
                case "write-40": return s.Name == "CuMode" && s.Value == Full;
                case "write-24": return s.Name == "CuMode" && s.Value == Stock;
                case "write-disable": return s.Name == "CuDisableWgp" && s.Value != 0 && (s.Value & ~AllowedDisable) == 0;
                case "write-chosen": return s.Name == "CuModeChosen" && EncodedRequest(s.Value);
                case "remove-chosen": return s.Name == "CuModeChosen";
                case "remove-closed": return s.Name == "CuModeClosedReason";
                case "remove-confirmed": return s.Name == "CuModeConfirmed";
                case "remove-disable": return s.Name == "CuDisableWgp";
                case "verify-stock":
                case "flush-readback": return s.Name == null;
                default: return false;
            }
        }

        // A value of CuModeChosen the setter may write: an encoded request of a mode the driver takes, with a mask
        // inside the board's topology and none at all with the stock mode.
        public static bool EncodedRequest(uint v)
        {
            uint mode = v & 0xFF, disable = v >> 8;
            if ((disable & ~AllowedDisable) != 0) return false;
            return mode == Full || (mode == Stock && disable == 0);
        }

        public static CuStored ReadAll(ICuRegistry r)
        {
            return new CuStored { Mode = r.Read("CuMode"), Disable = r.Read("CuDisableWgp"), Confirmed = r.Read("CuModeConfirmed"), Pending = r.Read("CuModePending"),
                Closed = r.Read("CuModeClosedReason"), Chosen = r.Read("CuModeChosen") };
        }

        // Runs the plan. Any failed step stops the sequence (S6); nothing is ever restored from the backup; the state is
        // read back after the last step or the failure.
        public static CuSetterResult Execute(CuSetterPlan plan, ICuRegistry r)
        {
            var res = new CuSetterResult();
            if (plan.Refused) { res.Error = plan.Refusal; return res; }
            for (int i = 0; i < plan.Steps.Count; i++)
            {
                var s = plan.Steps[i];
                if (!Allowed(s)) { res.FailedStep = i; res.Error = "step not allowed: " + s; break; }
                res.AnyStepRan = true;
                string stage = "write";
                try
                {
                    switch (s.Kind)
                    {
                        case "write-40": r.SetDword("CuMode", Full); break;
                        case "write-24": r.SetDword("CuMode", Stock); break;
                        case "write-disable":
                        case "write-chosen": r.SetDword(s.Name, s.Value); break;
                        case "remove-closed":
                        case "remove-confirmed":
                        case "remove-chosen":
                        case "remove-disable": r.Delete(s.Name); break;
                        case "verify-stock":
                        case "flush-readback":
                            stage = "flush";
                            r.Flush();
                            stage = "read";
                            var back = ReadAll(r);
                            res.ReadBack = back;
                            stage = "compare";
                            if (s.Kind == "verify-stock" && back.Mode != Stock)
                                throw new InvalidOperationException("CuMode reads back as " + (back.Mode == null ? "absent" : back.Mode.ToString()) + ", not the stock selection 24");
                            if (s.Kind == "flush-readback" && !Matches(plan, back))
                                throw new InvalidOperationException("the read-back does not match the plan: " + back);
                            break;
                    }
                    res.Log.Add("done: " + s);
                }
                catch (Exception e)
                {
                    res.FailedStep = i;
                    res.Error = s + ": " + e.Message;
                    if (stage == "flush") res.FlushFailed = true;
                    if (stage == "read" && s.Kind == "verify-stock") res.VerifyFailed = true;
                    if (stage == "read") res.ReadBack = null;
                    res.Log.Add("FAILED: " + res.Error);
                    break;
                }
            }
            if (res.FailedStep < 0) { res.Completed = true; return res; }
            // The actual state after a failure (S6): read back CuMode, CuModeConfirmed, CuDisableWgp and CuModePending.
            // After a failed verifying read (F14c (c)) the choice stays unknown whatever this read gives.
            try { res.ReadBack = ReadAll(r); res.Log.Add("read back after the failure: " + res.ReadBack); }
            catch (Exception e) { res.ReadBack = null; res.Log.Add("read back after the failure FAILED: " + e.Message); }
            return res;
        }

        static bool Matches(CuSetterPlan plan, CuStored back)
        {
            if (back.Confirmed != null && plan.Before.Confirmed != null) return false;
            if (plan.Record ? back.Chosen != Encode(plan.Target, plan.Disable) : back.Chosen != null) return false;
            if (plan.Target == Full) return back.Mode == Full && (back.Disable ?? 0) == plan.Disable;
            // The stock run writes the 24 out and takes the driver's record and the mask with it, so none of the
            // three may be left over.
            return back.Mode == Stock && back.Closed == null && back.Disable == null;
        }

        // The prediction after a setter run: a failed flush or a failed verifying read establishes no durable state
        // (plan 497 decisions 1 and 2), even when the read-back shows the mode.
        public static CuNext PredictAfter(CuSetterResult r)
        {
            return Predict(StoredAfter(r));
        }

        // The stored values after a run as the window should treat them: the read-back, not durable after a failed
        // flush or verifying read; null when the read-back failed.
        public static CuStored StoredAfter(CuSetterResult r)
        {
            if (r.ReadBack == null) return null;
            var s = r.ReadBack.Copy();
            if (r.FlushFailed || r.VerifyFailed) s.NotDurable = true;
            return s;
        }

        // The stored choice after a run: from the read-back alone, independent of the prediction (review 927 R2);
        // unknown when the verifying read failed (plan 497 decision 2) or no read-back exists.
        public static CuChoice ChoiceAfter(CuSetterResult r)
        {
            return r.VerifyFailed || r.ReadBack == null ? CuChoice.Unknown : Choice(r.ReadBack);
        }

        public static CuTransaction Transaction(CuSetterPlan plan, CuSetterResult r)
        {
            if (plan.Refused || !r.AnyStepRan) return CuTransaction.NotApplicable;
            return r.Completed ? CuTransaction.Complete : CuTransaction.PartialFailure;
        }

        // [Confirm now], from the class the fresh READ after the CONFIRM gives: Confirmed (or with fewer cores) =
        // complete; still Waiting (deleting Pending failed) = failed; Not saved (Pending deleted, Confirmed not stored) =
        // a partial failure; anything else is not a known outcome of the CONFIRM.
        public static CuTransaction ConfirmOutcome(CuClass after)
        {
            switch (after)
            {
                case CuClass.Confirmed:
                case CuClass.ConfirmedFewer: return CuTransaction.Complete;
                case CuClass.Waiting: return CuTransaction.Failed;
                case CuClass.NotSaved: return CuTransaction.PartialFailure;
                default: return CuTransaction.NotApplicable;
            }
        }

        // The result text (A3): "Nothing was changed" only when the helper refused before any step.
        public static string ResultText(CuSetterPlan plan, CuSetterResult r)
        {
            if (plan.Refused || !r.AnyStepRan) return Strings.T("cu.result.nothing") + (plan.Refused ? " " + plan.Refusal : "");
            var next = PredictAfter(r);
            var choice = ChoiceText(ChoiceAfter(r), r.ReadBack == null ? Full : ChoiceCores(r.ReadBack));
            if (r.Completed) return Strings.T("cu.result.saved") + " " + choice + " " + Strings.T("cu.choice.after-restart") + " " + NextText(r.ReadBack, next);
            // F14d: stock verified, the cleanup stopped: the next start uses 24 cores, a remaining mask does not reduce them.
            bool cleanup = plan.Target == Stock && next == CuNext.Standard && r.ReadBack != null
                && (r.ReadBack.Confirmed != null || r.ReadBack.Disable != null || r.ReadBack.Closed != null);
            return Strings.T("cu.result.failed") + " " + choice + " " + (cleanup ? Strings.T("cu.result.cleanup-incomplete") : NextText(r.ReadBack, next));
        }

        // The effect text before the switch (section 7 S6).
        public static string EffectText() { return Strings.T("cu.effect"); }
    }
}
