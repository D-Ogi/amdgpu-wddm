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

    public enum CuNext { Unknown, Standard, Fallback, Ask40, Confirmed40 }

    // The four values the setter and the prediction look at. Null: absent. Unreadable: a value that exists but could
    // not be read as a REG_DWORD (or the key could not be read): nothing is predicted from it.
    public sealed class CuStored
    {
        public uint? Mode, Disable, Confirmed, Pending;
        public bool Unreadable;

        public CuStored Copy() { return new CuStored { Mode = Mode, Disable = Disable, Confirmed = Confirmed, Pending = Pending, Unreadable = Unreadable }; }

        public override string ToString()
        {
            Func<uint?, string> v = x => x == null ? "absent" : x.Value.ToString(CultureInfo.InvariantCulture) + " (0x" + x.Value.ToString("X", CultureInfo.InvariantCulture) + ")";
            return Unreadable ? "unreadable" : "CuMode " + v(Mode) + ", CuDisableWgp " + v(Disable) + ", CuModeConfirmed " + v(Confirmed) + ", CuModePending " + v(Pending);
        }
    }

    // What the page shows for the setting: each line already in the window's language.
    public sealed class CuView
    {
        public CuClass Class;
        public CuNext Next;
        public string Choice, Running, NextStart, Note;
        public bool OfferConfirm, OfferTry40, OfferChoose24, OfferReport;
        public uint ChoiceMode;             // 24, 40, or 0 when the stored value is not valid or unreadable
    }

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
        public string Kind;      // remove-confirmed, remove-disable, write-40, remove-mode, verify-stock, flush-readback
        public string Name;      // the registry value, null for verify/flush
        public uint Value;
        public bool Changes { get { return Kind != "verify-stock" && Kind != "flush-readback"; } }

        public override string ToString()
        {
            string where = @"HKLM\" + CuMode.ParametersPath + " ";
            switch (Kind)
            {
                case "write-40": return "S1 set " + where + "CuMode = 40 (DWord)";
                case "remove-mode": return "S1 delete " + where + "CuMode";
                case "remove-confirmed": return "S2 delete " + where + "CuModeConfirmed";
                case "remove-disable": return "S3 delete " + where + "CuDisableWgp";
                case "verify-stock": return "S1 flush the key, read back CuMode and verify the stock selection (absent or 24)";
                default: return "S4 flush the key, read back CuMode, CuModeConfirmed and CuDisableWgp";
            }
        }
    }

    public sealed class CuSetterPlan
    {
        public uint Target;                              // 24 or 40
        public string Refusal;                           // the helper refuses before any step
        public readonly List<CuStep> Steps = new List<CuStep>();
        public readonly List<string> Preview = new List<string>();     // plain-language lines, one per changing step
        public CuStored Before;
        public bool Refused { get { return Refusal != null; } }
    }

    public sealed class CuSetterResult
    {
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

        // enum bc250_cu_reason, driver/shim/include/bc250_cu_mode.h.
        public const uint ReasonNone = 0, ReasonInvalidSetting = 1, ReasonInvalidDisable = 2, ReasonPendingUnconfirmed = 3, ReasonRegistry = 4,
            ReasonNotThisDevice = 5, ReasonPowerGating = 6, ReasonStockUnexpected = 7, ReasonReadback = 8, ReasonRestoreFailed = 9, ReasonNotRun = 10,
            ReasonTopology = 11;

        public static readonly string[] ValueNames = { "CuMode", "CuDisableWgp", "CuModeConfirmed", "CuModePending" };

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

        // bc250_cu_decide over the stored values, for the BC-250's topology. A live Pending -> the KMD's fallback.
        public static CuNext Predict(CuStored v)
        {
            if (v == null || v.Unreadable) return CuNext.Unknown;
            if (v.Mode == null || v.Mode == Stock) return CuNext.Standard;
            if (v.Mode != Full) return CuNext.Standard;                    // INVALID_SETTING: stock
            uint disable = v.Disable ?? 0;
            if ((disable & ~AllowedDisable) != 0) return CuNext.Standard;  // INVALID_DISABLE: stock
            if ((v.Pending ?? 0) != 0) return CuNext.Fallback;
            return v.Confirmed == Encode(Full, disable) ? CuNext.Confirmed40 : CuNext.Ask40;
        }

        // A 40 start that would run with an earlier diagnostic core limit (never for Standard: absent/24 returns
        // before the mask is read, bc250_cu_mode.c:68-82).
        static bool Limited(CuStored v, CuNext next)
        {
            return (next == CuNext.Ask40 || next == CuNext.Confirmed40) && (v.Disable ?? 0) != 0;
        }

        // ---- the view ----------------------------------------------------------------------------------------------

        public static uint ChoiceOf(CuStored v)
        {
            if (v == null || v.Unreadable) return 0;
            if (v.Mode == null || v.Mode == Stock) return Stock;
            return v.Mode == Full ? Full : 0;
        }

        // confirmFailed: the user pressed [Confirm now] in this start and Pending is still there.
        public static CuView View(CuModeState snapshot, ulong? startGeneration, CuStored stored, bool confirmFailed = false)
        {
            var v = new CuView { Class = Classify(snapshot, startGeneration), Next = Predict(stored), ChoiceMode = ChoiceOf(stored) };
            int cores = snapshot != null ? (int)snapshot.ActiveCus : 0;
            uint applied = v.Class == CuClass.Unknown ? 0 : snapshot.Applied;

            // Your choice.
            if (stored == null || stored.Unreadable) v.Choice = Strings.T("cu.choice.unreadable");
            else if (v.ChoiceMode == 0) v.Choice = Strings.T("cu.choice.invalid");
            else
            {
                v.Choice = Strings.T(v.ChoiceMode == Full ? "cu.choice.all" : "cu.choice.standard");
                if (applied != 0 && applied != v.ChoiceMode) v.Choice += " " + Strings.T("cu.choice.after-restart");
            }

            // This start.
            switch (v.Class)
            {
                case CuClass.Standard: v.Running = Strings.T("cu.run.standard", cores); break;
                case CuClass.Waiting:
                    v.Running = confirmFailed ? Strings.T("cu.run.confirm-failed") : Strings.T("cu.run.waiting", cores);
                    v.OfferConfirm = true;
                    v.OfferReport = confirmFailed;
                    break;
                case CuClass.Confirmed: v.Running = Strings.T("cu.run.confirmed"); break;
                case CuClass.ConfirmedFewer: v.Running = Strings.T("cu.run.confirmed-fewer", cores); break;
                case CuClass.NotSaved:
                    v.Running = Strings.T("cu.run.not-saved", cores) + " " + Strings.T(v.ChoiceMode == Full ? "cu.run.not-saved.next40" : "cu.run.not-saved.next24");
                    v.OfferReport = true;
                    v.OfferChoose24 = v.ChoiceMode == Full;
                    break;
                case CuClass.Fallback:
                    v.Running = Strings.T("cu.run.fallback");
                    v.OfferTry40 = v.ChoiceMode != Full;
                    break;
                case CuClass.Refused: v.Running = Strings.T("cu.run.refused"); break;
                default: v.Running = Strings.T("cu.run.unknown"); break;
            }

            v.NextStart = NextText(stored, v.Next);
            return v;
        }

        public static string NextText(CuStored stored, CuNext next)
        {
            switch (next)
            {
                case CuNext.Standard: return Strings.T("cu.next.standard");
                case CuNext.Fallback: return Strings.T("cu.next.fallback");
                case CuNext.Ask40: return Strings.T(Limited(stored, next) ? "cu.next.ask40-limited" : "cu.next.ask40");
                case CuNext.Confirmed40: return Strings.T(Limited(stored, next) ? "cu.next.confirmed40-limited" : "cu.next.confirmed40");
                default: return Strings.T("cu.next.unknown");
            }
        }

        // ---- the setter (S1-S6) ------------------------------------------------------------------------------------

        // The plan from the stored values the helper read itself. target 24 or 40. Only steps that change something
        // are listed in the preview; the verify and flush steps are part of the plan the helper runs.
        public static CuSetterPlan Plan(CuStored before, uint target)
        {
            var p = new CuSetterPlan { Target = target, Before = before };
            if (target != Stock && target != Full) { p.Refusal = Strings.T("cu.refuse.target"); return p; }
            if (before == null || before.Unreadable) { p.Refusal = Strings.T("cu.refuse.unreadable"); return p; }
            Func<string, string, uint, CuStep> step = (kind, name, value) => new CuStep { Kind = kind, Name = name, Value = value };
            if (target == Full)
            {
                if (before.Mode == Full && (before.Disable ?? 0) == 0) { p.Refusal = Strings.T("cu.refuse.already40"); return p; }
                // S2, S3, S1, S4: a failure before S1 leaves the old mode, never a new 40 request next to an old
                // confirmation or mask.
                if (before.Confirmed != null) p.Steps.Add(step("remove-confirmed", "CuModeConfirmed", 0));
                if (before.Disable != null) p.Steps.Add(step("remove-disable", "CuDisableWgp", 0));
                if (before.Mode != Full) p.Steps.Add(step("write-40", "CuMode", Full));
                p.Steps.Add(step("flush-readback", null, 0));
            }
            else
            {
                bool stockNow = before.Mode == null || before.Mode == Stock;
                if (stockNow && before.Confirmed == null && before.Disable == null) { p.Refusal = Strings.T("cu.refuse.already24"); return p; }
                // S1 (a KMD-stored 24 is left as it is), flush, verify the stock selection; only then S2, S3, S4.
                if (!stockNow) p.Steps.Add(step("remove-mode", "CuMode", 0));
                p.Steps.Add(step("verify-stock", null, 0));
                if (before.Confirmed != null) p.Steps.Add(step("remove-confirmed", "CuModeConfirmed", 0));
                if (before.Disable != null) p.Steps.Add(step("remove-disable", "CuDisableWgp", 0));
                p.Steps.Add(step("flush-readback", null, 0));
            }
            foreach (var s in p.Steps.Where(s => s.Changes))
                switch (s.Kind)
                {
                    case "write-40": p.Preview.Add(Strings.T("cu.preview.write40")); break;
                    case "remove-mode": p.Preview.Add(Strings.T("cu.preview.remove-mode")); break;
                    case "remove-confirmed": p.Preview.Add(Strings.T("cu.preview.remove-confirmed")); break;
                    case "remove-disable": p.Preview.Add(Strings.T("cu.preview.remove-disable")); break;
                }
            return p;
        }

        // The only registry operations the CU setter may make (G-PLAN, G-SRC): CuMode set to 40 or deleted, and the
        // deletion of CuDisableWgp and CuModeConfirmed. Never CuModePending, never the KMD's last-reason values.
        public static bool Allowed(CuStep s)
        {
            switch (s.Kind)
            {
                case "write-40": return s.Name == "CuMode" && s.Value == Full;
                case "remove-mode": return s.Name == "CuMode";
                case "remove-confirmed": return s.Name == "CuModeConfirmed";
                case "remove-disable": return s.Name == "CuDisableWgp";
                case "verify-stock":
                case "flush-readback": return s.Name == null;
                default: return false;
            }
        }

        public static CuStored ReadAll(ICuRegistry r)
        {
            return new CuStored { Mode = r.Read("CuMode"), Disable = r.Read("CuDisableWgp"), Confirmed = r.Read("CuModeConfirmed"), Pending = r.Read("CuModePending") };
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
                        case "remove-mode":
                        case "remove-confirmed":
                        case "remove-disable": r.Delete(s.Name); break;
                        case "verify-stock":
                        case "flush-readback":
                            stage = "flush";
                            r.Flush();
                            stage = "read";
                            var back = ReadAll(r);
                            res.ReadBack = back;
                            stage = "compare";
                            if (s.Kind == "verify-stock" && !(back.Mode == null || back.Mode == Stock))
                                throw new InvalidOperationException("CuMode reads back as " + back.Mode + ", not the stock selection");
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
            // F14c (c): the verifying read failed, the state is unknown.
            if (res.VerifyFailed) return res;
            // The actual state after a failure: read back CuMode, CuModeConfirmed, CuDisableWgp and CuModePending.
            try { res.ReadBack = ReadAll(r); res.Log.Add("read back after the failure: " + res.ReadBack); }
            catch (Exception e) { res.ReadBack = null; res.Log.Add("read back after the failure FAILED: " + e.Message); }
            return res;
        }

        static bool Matches(CuSetterPlan plan, CuStored back)
        {
            if (back.Confirmed != null && plan.Before.Confirmed != null) return false;
            if (back.Disable != null && plan.Before.Disable != null) return false;
            return plan.Target == Full ? back.Mode == Full : back.Mode == null || back.Mode == Stock;
        }

        // The prediction after a setter run: a failed flush or a failed verifying read establishes no durable state.
        public static CuNext PredictAfter(CuSetterResult r)
        {
            if (r.FlushFailed || r.VerifyFailed || r.ReadBack == null) return CuNext.Unknown;
            return Predict(r.ReadBack);
        }

        // The result text (A3): "Nothing was changed" only when the helper refused before any step.
        public static string ResultText(CuSetterPlan plan, CuSetterResult r)
        {
            if (plan.Refused || !r.AnyStepRan) return Strings.T("cu.result.nothing") + (plan.Refused ? " " + plan.Refusal : "");
            var next = PredictAfter(r);
            var choice = r.ReadBack == null || next == CuNext.Unknown ? Strings.T("cu.choice.unreadable")
                : ChoiceOf(r.ReadBack) == Full ? Strings.T("cu.choice.all") : ChoiceOf(r.ReadBack) == Stock ? Strings.T("cu.choice.standard") : Strings.T("cu.choice.invalid");
            if (r.Completed) return Strings.T("cu.result.saved") + " " + choice + " " + Strings.T("cu.choice.after-restart") + " " + NextText(r.ReadBack, next);
            // F14d: stock verified, the cleanup stopped: the next start uses 24 cores, a remaining mask does not reduce them.
            bool cleanup = plan.Target == Stock && next == CuNext.Standard && r.ReadBack != null && (r.ReadBack.Confirmed != null || r.ReadBack.Disable != null);
            return Strings.T("cu.result.failed") + " " + choice + " " + (cleanup ? Strings.T("cu.result.cleanup-incomplete") : NextText(r.ReadBack, next));
        }

        // The effect text before the switch (section 7 S6).
        public static string EffectText() { return Strings.T("cu.effect"); }
    }
}
