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

    // The four values the setter and the prediction look at. Null: absent. Unreadable: a value that exists but could
    // not be read as a REG_DWORD (or the key could not be read): nothing is predicted from it. NotDurable: a setter run
    // of this boot could not flush or verify its writes (cu-unflushed.json, plan 497 decision 1): the values read now
    // may not be what the next start reads, so the next start is not predicted until a restart or a completed run.
    public sealed class CuStored
    {
        public uint? Mode, Disable, Confirmed, Pending;
        public bool Unreadable, NotDurable;

        public CuStored Copy() { return new CuStored { Mode = Mode, Disable = Disable, Confirmed = Confirmed, Pending = Pending, Unreadable = Unreadable, NotDurable = NotDurable }; }

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
        public CuChoice Stored;
        public string Choice, Running, NextStart, Note;
        public bool OfferConfirm, OfferTry40, OfferChoose24, OfferReport;
        public uint ChoiceMode;             // 24, 40, or 0 when the stored value is not valid or unreadable
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

        // A 40 start that would run with an earlier diagnostic core limit (never for Standard: absent/24 returns
        // before the mask is read, bc250_cu_mode.c:68-82).
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

        public static uint ChoiceOf(CuStored v)
        {
            var c = Choice(v);
            return c == CuChoice.Standard24 ? Stock : c == CuChoice.All40 ? Full : 0;
        }

        public static string ChoiceText(CuChoice c)
        {
            switch (c)
            {
                case CuChoice.Standard24: return Strings.T("cu.choice.standard");
                case CuChoice.All40: return Strings.T("cu.choice.all");
                case CuChoice.Invalid: return Strings.T("cu.choice.invalid");
                default: return Strings.T("cu.choice.unreadable");
            }
        }

        // confirmFailed: the user pressed [Confirm now] in this start and the start is still Waiting (deleting Pending
        // failed): the Waiting sentence stays and the failure is said next to it.
        public static CuView View(CuModeState snapshot, ulong? startGeneration, CuStored stored, bool confirmFailed = false)
        {
            var v = new CuView { Class = Classify(snapshot, startGeneration), Next = Predict(stored), Stored = Choice(stored), ChoiceMode = ChoiceOf(stored) };
            int cores = snapshot != null ? (int)snapshot.ActiveCus : 0;
            uint applied = v.Class == CuClass.Unknown ? 0 : snapshot.Applied;

            // Your choice.
            v.Choice = ChoiceText(v.Stored);
            if (v.ChoiceMode != 0 && applied != 0 && applied != v.ChoiceMode) v.Choice += " " + Strings.T("cu.choice.after-restart");

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
            // The actual state after a failure (S6): read back CuMode, CuModeConfirmed, CuDisableWgp and CuModePending.
            // After a failed verifying read (F14c (c)) the choice stays unknown whatever this read gives.
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
            var choice = ChoiceText(ChoiceAfter(r));
            if (r.Completed) return Strings.T("cu.result.saved") + " " + choice + " " + Strings.T("cu.choice.after-restart") + " " + NextText(r.ReadBack, next);
            // F14d: stock verified, the cleanup stopped: the next start uses 24 cores, a remaining mask does not reduce them.
            bool cleanup = plan.Target == Stock && next == CuNext.Standard && r.ReadBack != null && (r.ReadBack.Confirmed != null || r.ReadBack.Disable != null);
            return Strings.T("cu.result.failed") + " " + choice + " " + (cleanup ? Strings.T("cu.result.cleanup-incomplete") : NextText(r.ReadBack, next));
        }

        // The effect text before the switch (section 7 S6).
        public static string EffectText() { return Strings.T("cu.effect"); }
    }
}
