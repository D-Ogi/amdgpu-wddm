// G-CU: the CU fixtures of GUI plan v7 section 7 (F1-F16 with F5b, F14b, F14c a-c, F14d, F16b), each with its exact
// flags, the text shown and the next step; the setter's step order and partial failures through a fake registry; the
// reply layout from the header.
using System;
using System.Collections.Generic;
using System.Linq;
using System.Text.RegularExpressions;
using AmdgpuWddmControl;

static partial class UnitTests
{
    // A registry with injected failures. Every operation is recorded in Ops.
    sealed class FakeCu : ICuRegistry
    {
        public readonly Dictionary<string, uint> Values = new Dictionary<string, uint>();
        public readonly List<string> Ops = new List<string>();
        public HashSet<string> FailDelete = new HashSet<string>();
        public bool FailSet;
        public int FailFlush = -1, FailReadAfterFlush = -1;    // the 0-based flush call that fails / after which reads fail
        int _flushes;
        bool _readsFail;

        public uint? Read(string name)
        {
            if (_readsFail) { Ops.Add("read-fail " + name); throw new InvalidOperationException("read failed"); }
            uint v;
            return Values.TryGetValue(name, out v) ? (uint?)v : null;
        }

        public void Delete(string name)
        {
            Ops.Add("delete " + name);
            if (FailDelete.Contains(name)) throw new InvalidOperationException("delete failed");
            Values.Remove(name);
        }

        public void SetDword(string name, uint value)
        {
            Ops.Add("set " + name + "=" + value);
            if (FailSet) throw new InvalidOperationException("set failed");
            Values[name] = value;
        }

        public void Flush()
        {
            int n = _flushes++;
            Ops.Add("flush");
            if (n == FailFlush) throw new InvalidOperationException("flush failed");
            if (n == FailReadAfterFlush) _readsFail = true;
        }

        public CuStored Stored()
        {
            Func<string, uint?> g = k => { uint v; return Values.TryGetValue(k, out v) ? (uint?)v : null; };
            return new CuStored { Mode = g("CuMode"), Disable = g("CuDisableWgp"), Confirmed = g("CuModeConfirmed"), Pending = g("CuModePending") };
        }
    }

    // Native return codes injected under CheckedCuRegistry (review 927 R1).
    sealed class FakeRegApi : IRegApi
    {
        public readonly Dictionary<string, uint> Values = new Dictionary<string, uint>();
        public int QueryStatus, DeleteStatus, SetStatus, FlushStatus, Flushes;
        public uint QueryType = 4;
        public byte[] QueryData;
        public int Query(string name, out uint type, out byte[] data)
        {
            type = QueryType; data = QueryData;
            if (QueryStatus != 0) return QueryStatus;
            uint v;
            if (QueryData == null && !Values.TryGetValue(name, out v)) return CheckedCuRegistry.FileNotFound;
            if (QueryData == null) data = BitConverter.GetBytes(Values[name]);
            return 0;
        }
        public int Delete(string name) { if (DeleteStatus != 0) return DeleteStatus; return Values.Remove(name) ? 0 : CheckedCuRegistry.FileNotFound; }
        public int SetDword(string name, uint value) { if (SetStatus == 0) Values[name] = value; return SetStatus; }
        public int Flush() { Flushes++; return FlushStatus; }
    }

    static void CuNativeAdapter()
    {
        var api = new FakeRegApi(); api.Values["CuMode"] = 40;
        var reg = new CheckedCuRegistry(api);
        Equal((uint?)40, reg.Read("CuMode"), "R1 a REG_DWORD reads");
        Equal((uint?)null, reg.Read("CuModeConfirmed"), "R1 ERROR_FILE_NOT_FOUND is absent");
        api.QueryStatus = 5;
        Throws<CuRegistryException>(() => reg.Read("CuMode"), "R1 ERROR_ACCESS_DENIED on read is an error, never absent");
        try { reg.Read("CuMode"); } catch (CuRegistryException x) { Equal(5, x.Status, "R1 the LSTATUS is kept"); }
        api.QueryStatus = 1018;
        Throws<CuRegistryException>(() => reg.Read("CuMode"), "R1 ERROR_KEY_DELETED on read is an error");
        api.QueryStatus = 0; api.QueryType = 1; api.QueryData = new byte[] { 0x34, 0, 0x30, 0 };
        Throws<CuRegistryException>(() => reg.Read("CuMode"), "R1 a REG_SZ is not a REG_DWORD");
        api.QueryType = 4; api.QueryData = new byte[8];
        Throws<CuRegistryException>(() => reg.Read("CuMode"), "R1 eight bytes are not a REG_DWORD");
        api.QueryData = null;
        reg.Delete("CuModeConfirmed");
        Check(true, "R1 deleting an absent value succeeds");
        api.DeleteStatus = 5;
        Throws<CuRegistryException>(() => reg.Delete("CuDisableWgp"), "R1 a delete that fails is an error, not 'removed'");
        api.DeleteStatus = 0; api.SetStatus = 1021;
        Throws<CuRegistryException>(() => reg.SetDword("CuMode", 40), "R1 ERROR_CANTWRITE on set is an error");
        api.SetStatus = 0; api.FlushStatus = 1016;
        Throws<CuRegistryException>(() => reg.Flush(), "R1 a failed RegFlushKey is an error");
        Throws<InvalidOperationException>(() => reg.Delete("CuModePending"), "R1 CuModePending is never removed");
        Throws<InvalidOperationException>(() => reg.SetDword("CuModeConfirmed", 40), "R1 CuModeConfirmed is never written");

        // Through the setter: the native codes decide the result.
        api = new FakeRegApi(); api.Values["CuMode"] = 24; api.Values["CuModeConfirmed"] = CuMode.Encode(40, 8); api.Values["CuDisableWgp"] = 8;
        api.FlushStatus = 1016;
        var plan = CuMode.Plan(CuMode.ReadAll(new CheckedCuRegistry(api)), 40);
        var res = CuMode.Execute(plan, new CheckedCuRegistry(api));
        Check(res.FlushFailed && !res.Completed && api.Flushes == 1, "R1 the flush always runs and its failure is reported");
        Equal(CuNext.Unknown, CuMode.PredictAfter(res), "R1 a failed native flush predicts nothing");
        api = new FakeRegApi(); api.Values["CuMode"] = 40; api.Values["CuDisableWgp"] = 8; api.DeleteStatus = 5;
        plan = CuMode.Plan(CuMode.ReadAll(new CheckedCuRegistry(api)), 24);
        res = CuMode.Execute(plan, new CheckedCuRegistry(api));
        Check(!res.Completed && res.FailedStep == 0 && res.Error.Contains("LSTATUS 5"), "R1 a native delete failure stops at S1 with its LSTATUS: " + res.Error);
        Equal((uint?)40, CuMode.ChoiceOf(res.ReadBack) == 40 ? (uint?)40 : null, "R1 the read-back after the failure shows the old mode");
        api = new FakeRegApi(); api.Values["CuMode"] = 40; api.QueryStatus = 5;
        Throws<CuRegistryException>(() => CuMode.ReadAll(new CheckedCuRegistry(api)), "R1 an unreadable value never reads as absent");
    }

    static CuModeState Snap(uint flags, uint applied, uint reason, uint active, ulong generation = 5, uint disable = 0)
    {
        return new CuModeState { Flags = flags, Applied = applied, Reason = reason, ActiveCus = active, Generation = generation, DisableMask = disable, Requested = applied };
    }

    const uint V = CuModeState.FlagValid, P = CuModeState.FlagPending, C = CuModeState.FlagConfirmed;

    static CuStored St(uint? mode = null, uint? disable = null, uint? confirmed = null, uint? pending = null)
    {
        return new CuStored { Mode = mode, Disable = disable, Confirmed = confirmed, Pending = pending };
    }

    static void CuLayout(string header)
    {
        int size;
        var cu = Layout(header.Replace("[BC250_CU_MODE_SA_COUNT]", "[4]"), "BC250_ESCAPE_CU_MODE", out size);
        Equal(KmdReply.CuModeBytes, size, "CU reply size from the header");
        Equal(160, cu["Generation"], "CU Generation offset");
        var b = new byte[KmdReply.CuModeBytes];
        Put(b, cu["Magic"], KmdReply.Magic); Put(b, cu["Command"], KmdReply.CmdCuMode); Put(b, cu["AbiVersion"], 1u); Put(b, cu["Version"], 0x000700C7u);
        Put(b, cu["Flags"], 1u | 2u); Put(b, cu["Requested"], 40u); Put(b, cu["Applied"], 40u); Put(b, cu["Reason"], 0u);
        Put(b, cu["ActiveCus"], 38u); Put(b, cu["DisableMask"], 1u); Put(b, cu["PciId"], 0x13FE1002u); Put(b, cu["Generation"], 99UL);
        var s = KmdReply.ParseCuMode(b);
        Equal(40u, s.Applied, "CU applied"); Equal(38u, s.ActiveCus, "CU active"); Equal(99UL, s.Generation, "CU generation");
        Equal(1u, s.DisableMask, "CU mask"); Check(s.Has(CuModeState.FlagPending) && !s.Has(CuModeState.FlagConfirmed), "CU flags");
        Check(Regex.IsMatch(header, @"#define BC250_CU_MODE_FLAG_VALID 1u") && Regex.IsMatch(header, @"#define BC250_CU_MODE_FLAG_PENDING 2u") &&
            Regex.IsMatch(header, @"#define BC250_CU_MODE_FLAG_CONFIRMED 4u"), "CU flag values in the header");
        Put(b, cu["AbiVersion"], 2u);
        Throws<FormatException>(() => KmdReply.ParseCuMode(b), "CU ABI 2 refused");
    }

    static void CuReasons(string root)
    {
        var h = System.IO.File.ReadAllText(System.IO.Path.Combine(root, @"driver\shim\include\bc250_cu_mode.h"));
        var names = new[] { "NONE", "INVALID_SETTING", "INVALID_DISABLE", "PENDING_UNCONFIRMED", "REGISTRY", "NOT_THIS_DEVICE", "POWER_GATING",
            "STOCK_UNEXPECTED", "READBACK", "RESTORE_FAILED", "NOT_RUN", "TOPOLOGY" };
        var m = Regex.Match(h, @"enum bc250_cu_reason\s*\{(?<b>.*?)\}", RegexOptions.Singleline);
        Check(m.Success, "bc250_cu_reason found");
        var listed = Regex.Matches(m.Groups["b"].Value, @"BC250_CU_REASON_(\w+)").Cast<Match>().Select(x => x.Groups[1].Value).ToList();
        for (int i = 0; i < names.Length; i++) Equal(names[i], i < listed.Count ? listed[i] : null, "CU reason " + i);
        for (uint i = 0; i < names.Length; i++) Equal(names[i], CuMode.ReasonName(i), "CU reason name " + i);
        Check(Regex.IsMatch(h, @"#define BC250_CU_WGP_MAX\s+5u"), "CU WGP per array 5 (the mask the prediction allows)");
    }

    static void CuFixtures()
    {
        Strings.Language = "en";
        // F1 Standard.
        var v = CuMode.View(Snap(V, 24, 0, 24), 5, St());
        Equal(CuClass.Standard, v.Class, "F1 class"); Equal("Your choice: Standard (24).", v.Choice, "F1 choice");
        Equal("Running now: 24 cores.", v.Running, "F1 running"); Equal(CuNext.Standard, v.Next, "F1 next");
        Check(!v.OfferConfirm && !v.OfferTry40 && !v.OfferReport, "F1 no next step");

        // F2 Waiting, 40 active.
        v = CuMode.View(Snap(V | P, 40, 0, 40), 5, St(40, null, null, 40));
        Equal(CuClass.Waiting, v.Class, "F2 class");
        Equal("Running now: 40 cores, not confirmed yet. If the new 40-core setting is not confirmed, the next start returns to 24 cores.", v.Running, "F2 text");
        Check(v.OfferConfirm, "F2 [Confirm now]"); Equal("Your choice: All (40).", v.Choice, "F2 choice (no restart note: running = choice)");
        Equal(CuNext.Fallback, v.Next, "F2 next start with the live Pending = 24");

        // F3 Confirm OK.
        v = CuMode.View(Snap(V | C, 40, 0, 40), 5, St(40, null, 40, null));
        Equal(CuClass.Confirmed, v.Class, "F3 class"); Equal("All 40 cores, confirmed.", v.Running, "F3 text"); Check(!v.OfferConfirm, "F3 no confirm");
        Equal(CuNext.Confirmed40, v.Next, "F3 next start confirmed");

        // F4 Confirm with delete-Pending failure: PENDING=1, CONFIRMED=0 after.
        v = CuMode.View(Snap(V | P, 40, 0, 40), 5, St(40, null, null, 40), true);
        Equal("Running now: 40 cores, not confirmed yet. Confirming 40 cores did not work. If it stays unconfirmed, the next start returns to 24 cores.", v.Running, "F4 text: the Waiting sentence and the failure");
        Equal(CuTransaction.Failed, CuMode.ConfirmOutcome(v.Class), "F4 a CONFIRM that leaves Waiting failed");
        Check(v.OfferConfirm && v.OfferReport, "F4 manual retry and report offered");

        // F5 store-Confirmed failure, 40 still selected.
        v = CuMode.View(Snap(V, 40, 0, 40), 5, St(40));
        Equal(CuClass.NotSaved, v.Class, "F5 class");
        Equal("Running now: 40 cores. The confirmation could not be saved. The next start will try 40 cores again and ask for confirmation again.", v.Running, "F5 text");
        Check(!v.OfferConfirm && v.OfferReport && v.OfferChoose24, "F5 next step: report, choose 24, no confirm");
        Check(!v.Running.Contains("returns to 24"), "F5 no fallback promise");
        Equal(CuNext.Ask40, v.Next, "F5 next start = a fresh Pending (Waiting), not Fallback");
        Equal(CuClass.Waiting, CuMode.Classify(Snap(V | P, 40, 0, 40, 6), 6), "F5 next start fixture is Waiting");

        // F5b the same snapshot after the user chose Standard (24).
        v = CuMode.View(Snap(V, 40, 0, 40), 5, St());
        Equal(CuClass.NotSaved, v.Class, "F5b class");
        Check(v.Running.EndsWith("The next start uses 24 cores, as you chose."), "F5b text: " + v.Running);
        Equal("Your choice: Standard (24). It applies after the next Windows restart.", v.Choice, "F5b choice");
        Check(!v.OfferChoose24, "F5b Standard not offered again"); Equal(CuNext.Standard, v.Next, "F5b next start Standard");

        // F6 Fallback with CuMode already 24 (the KMD's own fallback).
        v = CuMode.View(Snap(V, 24, CuMode.ReasonPendingUnconfirmed, 24), 5, St(24));
        Equal(CuClass.Fallback, v.Class, "F6 class"); Equal("The last 40-core start was not confirmed, so this start uses 24 cores.", v.Running, "F6 text");
        Equal("Your choice: Standard (24).", v.Choice, "F6 choice"); Check(v.OfferTry40, "F6 [Try 40 again]");

        // F7 the user's explicit retry: stored 40, the snapshot stays Fallback until the restart.
        v = CuMode.View(Snap(V, 24, CuMode.ReasonPendingUnconfirmed, 24), 5, St(40));
        Equal(CuClass.Fallback, v.Class, "F7 class"); Equal("Your choice: All (40). It applies after the next Windows restart.", v.Choice, "F7 choice");
        Check(v.OfferTry40, "F7 [Try 40 again] stays the Fallback state's next step"); Equal(CuNext.Ask40, v.Next, "F7 next start asks again");
        var plan = CuMode.Plan(St(24), 40);
        Check(!plan.Refused && plan.Steps.Select(s => s.Kind).SequenceEqual(new[] { "write-40", "flush-readback" }), "F7 [Try 40 again] = the setter (S1, S4)");

        // F8 / F9: the start-confirm task's fallback path or no task run: the snapshot is still Pending.
        foreach (var label in new[] { "F8", "F9" })
        {
            Equal(CuClass.Waiting, CuMode.Classify(Snap(V | P, 40, 0, 40), 5), label + " stays Waiting");
            Check(CuMode.Classify(Snap(V | P, 40, 0, 40), 5) != CuClass.Confirmed, label + " never Confirmed");
        }

        // F10 an early restart after a healthy 40 start without confirmation: the next start is Fallback.
        Equal(CuNext.Fallback, CuMode.Predict(St(40, null, null, 40)), "F10 prediction");
        Equal(CuClass.Fallback, CuMode.Classify(Snap(V, 24, CuMode.ReasonPendingUnconfirmed, 24, 6), 6), "F10 next start class");

        // F11 an already confirmed request at a later start.
        Equal(CuNext.Confirmed40, CuMode.Predict(St(40, null, 40)), "F11 prediction"); Equal(CuClass.Confirmed, CuMode.Classify(Snap(V | C, 40, 0, 40), 5), "F11 class");

        // F12 40 -> 24 -> 40 with an old CuModeConfirmed: S2 removes it; the next start is Waiting, not Confirmed.
        var reg = new FakeCu(); reg.Values["CuModeConfirmed"] = 40;
        plan = CuMode.Plan(reg.Stored(), 40);
        Check(plan.Steps.Select(s => s.Kind).SequenceEqual(new[] { "remove-confirmed", "write-40", "flush-readback" }), "F12 steps S2, S1, S4");
        var res = CuMode.Execute(plan, reg);
        Check(res.Completed, "F12 completed"); Equal(CuNext.Ask40, CuMode.PredictAfter(res), "F12 next start asks (Waiting)");
        Check(!reg.Values.ContainsKey("CuModeConfirmed") && reg.Values["CuMode"] == 40, "F12 registry");

        // F13 a non-zero mask with All (40): the preview names its removal, the read-back shows it absent.
        reg = new FakeCu(); reg.Values["CuMode"] = 40; reg.Values["CuDisableWgp"] = 0x21; reg.Values["CuModeConfirmed"] = CuMode.Encode(40, 0x21);
        plan = CuMode.Plan(reg.Stored(), 40);
        Check(plan.Preview.Contains("Removes an earlier diagnostic core limit."), "F13 preview names the removal");
        Check(plan.Steps.Select(s => s.Kind).SequenceEqual(new[] { "remove-confirmed", "remove-disable", "flush-readback" }), "F13 steps S2, S3, S4");
        res = CuMode.Execute(plan, reg);
        Check(res.Completed && res.ReadBack.Disable == null, "F13 read-back without the mask"); Equal(CuNext.Ask40, CuMode.PredictAfter(res), "F13 next start 40, asks");
        v = CuMode.View(Snap(V | C, 40, 0, 38), 5, St(40, 0x21, CuMode.Encode(40, 0x21)));
        Equal(CuClass.ConfirmedFewer, v.Class, "F13 retained mask: Confirmed, fewer cores"); Equal("Running now: 38 cores (confirmed).", v.Running, "F13 fewer text");
        Check(!v.Running.Contains("All 40"), "F13 never All 40 with fewer cores");
        v = CuMode.View(Snap(V | P, 40, 0, 38), 5, St(40, 0x21, null, 1));
        Check(v.Running.StartsWith("Running now: 38 cores, not confirmed yet."), "F13 retained mask while Waiting shows 38");

        // F14 All (40): S2 fails -> stop; CuMode and CuDisableWgp untouched; nothing restored.
        reg = new FakeCu(); reg.Values["CuModeConfirmed"] = 40; reg.Values["CuDisableWgp"] = 3; reg.FailDelete.Add("CuModeConfirmed");
        plan = CuMode.Plan(reg.Stored(), 40);
        res = CuMode.Execute(plan, reg);
        Check(!res.Completed && res.FailedStep == 0, "F14 stops at S2");
        Check(reg.Values["CuDisableWgp"] == 3 && !reg.Values.ContainsKey("CuMode"), "F14 CuMode and CuDisableWgp untouched");
        Check(!reg.Ops.Any(o => o.StartsWith("set CuModeConfirmed") || o.StartsWith("set CuDisableWgp")), "F14 nothing restored");
        var text = CuMode.ResultText(plan, res);
        Check(text.StartsWith("The change could not be completed. Your choice: Standard (24). Next start: 24 cores."), "F14 text from the read-back: " + text);

        // F14b All (40): after S2 deleted CuModeConfirmed, S3 fails (and the S1 write or S4 flush variants).
        foreach (var variant in new[] { "S3", "S1", "S4" })
        {
            reg = new FakeCu(); reg.Values["CuMode"] = 7; reg.Values["CuModeConfirmed"] = CuMode.Encode(40, 3); reg.Values["CuDisableWgp"] = 3;
            if (variant == "S3") reg.FailDelete.Add("CuDisableWgp");
            if (variant == "S1") reg.FailSet = true;
            if (variant == "S4") reg.FailFlush = 0;
            plan = CuMode.Plan(reg.Stored(), 40);
            res = CuMode.Execute(plan, reg);
            Check(!res.Completed && !reg.Values.ContainsKey("CuModeConfirmed"), "F14b " + variant + ": stopped, the deleted confirmation stays deleted");
            Check(!reg.Ops.Any(o => o.StartsWith("set CuModeConfirmed") || o.StartsWith("set CuDisableWgp")), "F14b " + variant + ": nothing restored from the backup");
            Check(CuMode.ResultText(plan, res).StartsWith("The change could not be completed."), "F14b " + variant + " text");
            if (variant == "S4") Equal(CuNext.Unknown, CuMode.PredictAfter(res), "F14b S4: a failed flush predicts nothing");
        }
        // The example of the plan: old CuMode=40 without its confirmation -> a 40-core start that asks again.
        reg = new FakeCu(); reg.Values["CuMode"] = 40; reg.Values["CuModeConfirmed"] = CuMode.Encode(40, 3); reg.Values["CuDisableWgp"] = 3; reg.FailDelete.Add("CuDisableWgp");
        plan = CuMode.Plan(reg.Stored(), 40);
        res = CuMode.Execute(plan, reg);
        Equal(CuNext.Ask40, CuMode.PredictAfter(res), "F14b old 40 without confirmation asks again");
        Check(CuMode.ResultText(plan, res).Contains("earlier diagnostic core limit"), "F14b the remaining limit is named");
        Check(res.Log.Any(l => l.StartsWith("read back after the failure: CuMode 40")), "F14b read-back logged");

        // F14c Standard (24) from CuMode=40 with a valid mask and its confirmation, no Pending; S1 fails in three ways.
        foreach (var sub in new[] { "a", "b", "c" })
        {
            reg = new FakeCu(); reg.Values["CuMode"] = 40; reg.Values["CuDisableWgp"] = 3; reg.Values["CuModeConfirmed"] = CuMode.Encode(40, 3);
            if (sub == "a") reg.FailDelete.Add("CuMode");
            if (sub == "b") reg.FailFlush = 0;
            if (sub == "c") reg.FailReadAfterFlush = 0;
            plan = CuMode.Plan(reg.Stored(), 24);
            Check(plan.Steps.Select(s => s.Kind).SequenceEqual(new[] { "remove-mode", "verify-stock", "remove-confirmed", "remove-disable", "flush-readback" }), "F14c steps S1, verify, S2, S3, S4");
            res = CuMode.Execute(plan, reg);
            Check(!reg.Ops.Contains("delete CuModeConfirmed") && !reg.Ops.Contains("delete CuDisableWgp"), "F14c(" + sub + ") stops before S2 and S3");
            Check(reg.Values["CuDisableWgp"] == 3 && reg.Values["CuModeConfirmed"] == CuMode.Encode(40, 3), "F14c(" + sub + ") mask and confirmation untouched");
            Check(CuMode.ResultText(plan, res).StartsWith("The change could not be completed."), "F14c(" + sub + ") text");
            var next = CuMode.PredictAfter(res);
            if (sub == "a") { Equal(CuNext.Confirmed40, next, "F14c(a) the masked 40 start"); Check(CuMode.NextText(res.ReadBack, next).Contains("diagnostic core limit"), "F14c(a) never an unmasked 40"); }
            else Equal(CuNext.Unknown, next, "F14c(" + sub + ") Unknown");
            // Review 927 R2: the choice comes from the read-back, independent of the prediction; 497.2: unknown only
            // when the verifying read failed.
            Equal(sub == "a" ? CuChoice.All40 : sub == "b" ? CuChoice.Standard24 : CuChoice.Unknown, CuMode.ChoiceAfter(res), "F14c(" + sub + ") choice");
            Check(CuMode.ResultText(plan, res).Contains(sub == "a" ? "Your choice: All (40)." : sub == "b" ? "Your choice: Standard (24)." : "Your choice cannot be read."), "F14c(" + sub + ") choice text");
            Equal(CuTransaction.PartialFailure, CuMode.Transaction(plan, res), "F14c(" + sub + ") partial failure");
        }
        // R2 / 497.1: a failed S4 flush of All (40) still shows the read-back choice All (40); the next start is unknown.
        reg = new FakeCu(); reg.Values["CuMode"] = 24; reg.Values["CuModeConfirmed"] = CuMode.Encode(40, 8); reg.Values["CuDisableWgp"] = 8; reg.FailFlush = 0;
        plan = CuMode.Plan(reg.Stored(), 40);
        res = CuMode.Execute(plan, reg);
        Check(res.FlushFailed && res.ReadBack != null && res.ReadBack.Mode == 40, "F14b flush failure: the read-back shows 40");
        Equal(CuChoice.All40, CuMode.ChoiceAfter(res), "R2 F14b flush failure: choice All (40) from the read-back");
        Check(CuMode.ResultText(plan, res).Contains("Your choice: All (40).") && CuMode.ResultText(plan, res).Contains("Next start: cannot be predicted."), "R2 F14b flush failure text: " + CuMode.ResultText(plan, res));
        Check(CuMode.StoredAfter(res).NotDurable, "497.1 the read-back of an unflushed run is not durable");
        // R3: the Not-saved sentence follows the prediction: unknown or refused never says 24 or 40 by itself.
        v = CuMode.View(Snap(V, 40, 0, 40), 5, new CuStored { Mode = 40, NotDurable = true });
        Check(v.Running.EndsWith("Next start: cannot be predicted.") && !v.Running.Contains("24 cores, as you chose") && !v.Running.Contains("try 40 cores again"), "R3 Not saved with an unknown next start: " + v.Running);
        Check(v.OfferChoose24, "R3 Choose Standard (24) while 40 is chosen");
        v = CuMode.View(Snap(V, 40, 0, 40), 5, St(40, null, null, 7));
        Check(v.Running.EndsWith("Next start: 24 cores, because the 40-core setting was not confirmed."), "R3 Not saved with a live Pending: the fallback, from the prediction: " + v.Running);
        v = CuMode.View(Snap(V, 40, 0, 40), 5, new CuStored { Unreadable = true });
        Check(v.Running.EndsWith("Next start: cannot be predicted.") && !v.OfferChoose24, "R3 Not saved with unreadable values: " + v.Running);
        Equal(CuTransaction.Complete, CuMode.ConfirmOutcome(CuClass.Confirmed), "F3 complete");
        Equal(CuTransaction.PartialFailure, CuMode.ConfirmOutcome(CuClass.NotSaved), "F5 partial failure");
        // Negative check: no failure point of the 24 sequence deletes the mask or confirmation while CuMode=40 remains.
        for (int fail = 0; fail < 12; fail++)
        {
            reg = new FakeCu(); reg.Values["CuMode"] = 40; reg.Values["CuDisableWgp"] = 3; reg.Values["CuModeConfirmed"] = CuMode.Encode(40, 3);
            var failing = new FailAt(reg, fail);
            CuMode.Execute(CuMode.Plan(reg.Stored(), 24), failing);
            bool forty = reg.Values.ContainsKey("CuMode") && reg.Values["CuMode"] == 40;
            Check(!forty || (reg.Values.ContainsKey("CuDisableWgp") && reg.Values.ContainsKey("CuModeConfirmed")), "F14c negative check, failure at operation " + fail);
        }

        // F14d Standard (24): S1 verified, then S2 or S3 fails -> stock with a remaining confirmation or mask.
        foreach (var name in new[] { "CuModeConfirmed", "CuDisableWgp" })
        {
            reg = new FakeCu(); reg.Values["CuMode"] = 40; reg.Values["CuDisableWgp"] = 3; reg.Values["CuModeConfirmed"] = CuMode.Encode(40, 3); reg.FailDelete.Add(name);
            plan = CuMode.Plan(reg.Stored(), 24);
            res = CuMode.Execute(plan, reg);
            text = CuMode.ResultText(plan, res);
            Check(!reg.Values.ContainsKey("CuMode") && reg.Values.ContainsKey(name), "F14d " + name + ": stock with the remaining value");
            Equal(CuNext.Standard, CuMode.PredictAfter(res), "F14d " + name + " next start Standard");
            Check(text.StartsWith("The change could not be completed. Your choice: Standard (24). The cleanup is incomplete. The next start uses the standard 24 cores"), "F14d text: " + text);
            Check(!text.Contains("fewer cores"), "F14d never fewer cores for Standard");
        }
        Equal(CuNext.Standard, CuMode.Predict(St(null, 0xFF)), "F14d a mask without CuMode does not reduce 24");
        Equal("Next start: 24 cores.", CuMode.NextText(St(null, 0xFF), CuNext.Standard), "F14d text of a stock start with a mask");

        // F15 refused reasons, RESTORE_FAILED and NOT_RUN, a READ failure, a Generation mismatch.
        foreach (var r in new[] { CuMode.ReasonInvalidSetting, CuMode.ReasonInvalidDisable, CuMode.ReasonPowerGating, CuMode.ReasonTopology, CuMode.ReasonStockUnexpected,
            CuMode.ReasonReadback, CuMode.ReasonNotThisDevice, CuMode.ReasonRegistry })
        {
            v = CuMode.View(Snap(V, 24, r, 24), 5, St(40));
            Equal(CuClass.Refused, v.Class, "F15 reason " + CuMode.ReasonName(r)); Equal("40 cores could not be used on this start.", v.Running, "F15 text " + r);
        }
        foreach (var r in new[] { CuMode.ReasonRestoreFailed, CuMode.ReasonNotRun })
            Equal(CuClass.Unknown, CuMode.Classify(Snap(r == CuMode.ReasonNotRun ? 0 : V, 0, r, 0), 5), "F15 " + CuMode.ReasonName(r) + " Unknown");
        Equal(CuClass.Unknown, CuMode.Classify(Snap(V, 24, CuMode.ReasonRestoreFailed, 24), 5), "F15 RESTORE_FAILED with Applied 24 is still Unknown");
        Equal(CuClass.Unknown, CuMode.Classify(null, 5), "F15 READ failure");
        Equal(CuClass.Unknown, CuMode.Classify(Snap(V, 24, 0, 24, 4), 5), "F15 Generation mismatch");
        Equal(CuClass.Unknown, CuMode.Classify(Snap(V, 24, 0, 24), null), "F15 no start generation to check");
        Equal(CuClass.Unknown, CuMode.Classify(Snap(0, 24, 0, 24), 5), "F15 VALID=0");
        v = CuMode.View(null, 5, St());
        Equal("The number of graphics cores cannot be read.", v.Running, "F15 Unknown text"); Check(!v.Running.Contains("24"), "F15 never 24 working cores");

        // F16 the current start read an invalid CuMode: Refused; the choice is not valid; either choice repairs it.
        v = CuMode.View(Snap(V, 24, CuMode.ReasonInvalidSetting, 24), 5, St(7));
        Equal(CuClass.Refused, v.Class, "F16 class"); Equal("Your choice is not valid; choose Standard (24) or All (40).", v.Choice, "F16 choice");
        Check(CuMode.Plan(St(7), 24).Steps[0].Kind == "remove-mode" && CuMode.Plan(St(7), 40).Steps.Any(s => s.Kind == "write-40"), "F16 either choice repairs it");

        // F16b a valid start, then an invalid CuMode written afterwards: the running class stays.
        v = CuMode.View(Snap(V, 24, 0, 24), 5, St(7));
        Equal(CuClass.Standard, v.Class, "F16b Standard stays"); Check(v.Choice.StartsWith("Your choice is not valid"), "F16b choice"); Equal(CuNext.Refused, v.Next, "F16b next 24 (refused)");
        v = CuMode.View(Snap(V | P, 40, 0, 40), 5, St(7, null, null, 40));
        Equal(CuClass.Waiting, v.Class, "F16b Waiting stays"); Equal(CuNext.Refused, v.Next, "F16b next start 24 (INVALID_SETTING)");
        Equal(CuMode.ReasonInvalidSetting, CuMode.Prediction(St(32, null, null, 40)).Reason, "F16b an invalid mode is refused before the live Pending");
        Equal("Next start: 24 cores.", v.NextStart, "F16b a refused next start reads as 24 cores");

        // S5: the setter never writes CuModeConfirmed, never touches CuModePending, never the last-reason value.
        foreach (var start in new[] { St(), St(40), St(40, 3, 40, 1), St(7, 0x100000, 9, 2), St(24, 1, 40) })
            foreach (uint target in new uint[] { 24, 40 })
            {
                var p = CuMode.Plan(start, target);
                Check(p.Steps.All(CuMode.Allowed), "S5 every step allowed");
                Check(!p.Steps.Any(s => s.Name == "CuModePending" || s.Name == "CuModeLastReason" || (s.Kind == "write-40" && s.Name != "CuMode")), "S5 no Pending, no last-reason, writes only CuMode");
            }
        Check(!CuMode.Allowed(new CuStep { Kind = "remove-mode", Name = "CuModePending" }) && !CuMode.Allowed(new CuStep { Kind = "write-40", Name = "CuModeConfirmed", Value = 40 }),
            "S5 Allowed refuses the KMD's marks");
        Check(CuMode.Plan(St(40), 40).Refused && CuMode.Plan(St(), 24).Refused && CuMode.Plan(St(24), 24).Refused, "S6 nothing to change -> refused before any step");
        Equal("Nothing was changed. All (40) is selected already.", CuMode.ResultText(CuMode.Plan(St(40), 40), new CuSetterResult()), "A3 nothing changed only on refusal");
        Check(!CuMode.Plan(St(24, null, 40), 24).Refused, "a KMD-stored 24 with a leftover confirmation: the cleanup can run");
        Check(CuMode.Plan(St(24, null, 40), 24).Steps.All(s => s.Kind != "remove-mode"), "S1 leaves a KMD-stored 24");
        Check(CuMode.Plan(new CuStored { Unreadable = true }, 24).Refused, "unreadable values: refused");
        Equal(CuNext.Ask40, CuMode.Predict(St(40, 3, CuMode.Encode(40, 1))), "a confirmation of another mask does not match");
        Equal(CuNext.Refused, CuMode.Predict(St(40, 0x100000)), "an invalid mask: INVALID_DISABLE -> 24");
        Equal(CuMode.ReasonInvalidDisable, CuMode.Prediction(St(40, 0x100000)).Reason, "INVALID_DISABLE reason");
        var masked = CuMode.Prediction(St(40, 8, CuMode.Encode(40, 8)));
        Check(masked.Next == CuNext.Confirmed40 && masked.ActiveCus == 38 && masked.State == CuClass.ConfirmedFewer && masked.Confirmed && !masked.Pending, "a confirmed masked request: 38 cores, confirmed with fewer cores");
        var asks = CuMode.Prediction(St(40, 8));
        Check(asks.Next == CuNext.Ask40 && asks.ActiveCus == 38 && asks.State == CuClass.Waiting && asks.Pending && !asks.Confirmed, "an unconfirmed masked request waits with 38 cores");
        Equal(38u, CuMode.ActiveCus(8), "one disabled WGP takes two CUs"); Equal(40u, CuMode.ActiveCus(0), "no mask: 40");
        Check(CuMode.Prediction(new CuStored { Mode = 40, NotDurable = true }) == null, "497.1: values of a run that was not flushed predict nothing");
        Equal(CuChoice.All40, CuMode.Choice(new CuStored { Mode = 40, NotDurable = true }), "497.1: the choice still reads from the values");
    }

    // Wraps a fake so that its n-th mutating or flush operation throws.
    sealed class FailAt : ICuRegistry
    {
        readonly FakeCu _r; int _left;
        public FailAt(FakeCu r, int n) { _r = r; _left = n; }
        void Tick() { if (_left-- == 0) throw new InvalidOperationException("injected"); }
        public uint? Read(string name) { return _r.Read(name); }
        public void Delete(string name) { Tick(); _r.Delete(name); }
        public void SetDword(string name, uint value) { Tick(); _r.SetDword(name, value); }
        public void Flush() { Tick(); _r.Flush(); }
    }
}
