// Host tests of the overclock unlock by cooling type (src/OcUnlock.cs, MainForm.Oc.cs, the oc-tier action): the ABI 2
// words of the board capability query, the ranges they open, the questions, the writes and their plain sentences, the
// reset, and the board gate.
using System;
using System.IO;
using System.Linq;
using AmdgpuWddmControl;

static partial class UnitTests
{
    static byte[] OcCaps(byte[] raw, uint tier, uint stored, uint reason, uint board)
    {
        var b = (byte[])raw.Clone();
        Array.Copy(BitConverter.GetBytes(tier), 0, b, 139 * 4, 4);
        Array.Copy(BitConverter.GetBytes(stored), 0, b, 140 * 4, 4);
        Array.Copy(BitConverter.GetBytes(reason), 0, b, 141 * 4, 4);
        Array.Copy(BitConverter.GetBytes(board), 0, b, 142 * 4, 4);
        return b;
    }

    static bool ParseRefuses(byte[] b)
    {
        try { BoardCapabilities.Parse(b); return false; } catch (FormatException) { return true; }
    }

    static void OcUnlockTests()
    {
        Strings.Language = "en";
        var raw = File.ReadAllBytes(Environment.GetEnvironmentVariable("AMDGPU_WDDM_BOARD_FIXTURE"));
        var fixture = BoardCapabilities.Parse(raw);
        Equal(2u, fixture[4], "the provider fixture is ABI 2");
        Equal(3u, fixture[143], "three tiers");
        // The envelopes as the driver compiles them (driver/shim/include/bc250_board_envelope.h).
        BoardCapabilities.Replace(fixture);
        Equal(2000u, OcUnlock.GpuMaxMHz(0), "tier none: 2000 MHz"); Equal(3500u, OcUnlock.CpuMaxMHz(0), "tier none: 3500 MHz");
        Equal(2200u, OcUnlock.GpuMaxMHz(1), "air: 2200 MHz"); Equal(3900u, OcUnlock.CpuMaxMHz(1), "air: 3900 MHz");
        Equal(2300u, OcUnlock.GpuMaxMHz(2), "liquid: 2300 MHz"); Equal(4100u, OcUnlock.CpuMaxMHz(2), "liquid: 4100 MHz");
        Equal(1050u, OcUnlock.GpuMaxMv(1), "air: 1050 mV"); Equal(1075u, OcUnlock.GpuMaxMv(2), "liquid: 1075 mV");
        Equal(40u, OcUnlock.CpuUvMaxSteps(2), "a tier: 40 steps"); Equal(16u, OcUnlock.CpuUvMaxSteps(0), "no tier: 16 steps");
        Equal(0u, OcUnlock.CpuMinUvSteps(3500), "the standard limit needs no undervolt");
        Equal(24u, OcUnlock.CpuMinUvSteps(3900), "3900 MHz needs 24 steps");
        Equal(40u, OcUnlock.CpuMinUvSteps(4100), "4100 MHz needs 40 steps");
        Equal(uint.MaxValue, OcUnlock.CpuMinUvSteps(4200), "no tier admits 4200 MHz");
        // The fixture has no board fingerprint (the provider test runs on a mock without a board identity).
        Check(!OcUnlock.Available, "no board fingerprint: no unlock");

        // The tier none on a BC-250: nothing moves. The defaults stay at 2000 MHz and 3500 MHz under every tier.
        BoardCapabilities.Replace(BoardCapabilities.Parse(OcCaps(raw, 0, 0, 0, 0x1234)));
        Check(OcUnlock.Available && OcUnlock.Effective == 0, "a BC-250 with a fingerprint can take a tier");
        Equal(2000u, DpmSettings.HardMaxMHz, "no tier: the plain ceiling");
        Equal(2000u, DpmSettings.CeilingChoices.Max(), "no tier: no clock above 2000 MHz");
        Equal(3500u, CpuTuning.MaxMHz, "no tier: the release bound"); Equal(16u, CpuTuning.MaxSteps, "no tier: 16 steps");
        // Air and liquid: the ranges of the two controls grow to the tier, the defaults do not move.
        BoardCapabilities.Replace(BoardCapabilities.Parse(OcCaps(raw, 1, 1, 1, 0x1234)));
        Equal(2200u, DpmSettings.HardMaxMHz, "air: 2200 MHz ceiling");
        Check(DpmSettings.IsValidCeiling(2100) && DpmSettings.IsValidCeiling(2200) && !DpmSettings.IsValidCeiling(2300), "air: 2100 and 2200 MHz");
        Equal(3900u, CpuTuning.MaxMHz, "air: 3900 MHz processor limit");
        Equal(2000u, DpmSettings.DefaultMaxMHz, "air: the default ceiling stays 2000 MHz");
        BoardCapabilities.Replace(BoardCapabilities.Parse(OcCaps(raw, 2, 2, 1, 0x1234)));
        Equal(2300u, DpmSettings.HardMaxMHz, "liquid: 2300 MHz ceiling");
        Check(DpmSettings.IsValidCeiling(2300) && !DpmSettings.IsValidCeiling(2400), "liquid: up to 2300 MHz");
        Equal(4100u, CpuTuning.MaxMHz, "liquid: 4100 MHz processor limit"); Equal(40u, CpuTuning.MaxSteps, "liquid: 40 steps");
        Check(CpuTuning.ClockChoices()[0] == 4100u, "liquid: the processor list starts at 4100 MHz");
        Equal(2000u, DpmSettings.DefaultMaxMHz, "liquid: the default ceiling stays 2000 MHz");
        var cpu = new CpuState { Flags = CpuState.FlagValid | CpuState.FlagQueue3Proven | CpuState.FlagTuneOn, CoreMHz = new uint[8], PstateMHz = new uint[8], Cores = 6, Threads = 12, CoreMask = 0x77 };
        var cv = TunerView.Cpu(cpu, true, 1, null, null, null, null, TrialEnd.None);
        Check(cv.ClockChoices[0] == 4100u, "liquid: the processor picker offers 4100 MHz");
        Equal(3500u, cv.Clock, "liquid: with nothing in force the processor picker starts at the standard 3500 MHz");
        Check(OcUnlock.StateText().Contains("2300") && OcUnlock.StateText().Contains("4100"), "the card names the limits: " + OcUnlock.StateText());
        Check(OcUnlock.StateText(OcUnlock.ReasonUncleanStop).Contains("4100"), "a tier in force hides an old record of an unexpected stop");
        // The start after an unexpected stop: the driver deleted the tier, and the card says why in plain words.
        BoardCapabilities.Replace(BoardCapabilities.Parse(OcCaps(raw, 0, 0, OcUnlock.ReasonUncleanStop, 0x1234)));
        Equal(Strings.T("oc.state.unclean"), OcUnlock.StateText(), "unexpected stop: the card says the limits were turned off");
        // A later start with nothing stored (reason none) keeps the notice from the driver's record.
        BoardCapabilities.Replace(BoardCapabilities.Parse(OcCaps(raw, 0, 0, 0, 0x1234)));
        Equal(Strings.T("oc.state.unclean"), OcUnlock.StateText(OcUnlock.ReasonUncleanStop), "a later start: the notice stays from the record");
        Check(OcUnlock.StateText(null).Contains("2000"), "no record: the standard limits");
        BoardCapabilities.Replace(BoardCapabilities.Parse(OcCaps(raw, 0, 1, 0, 0x1234)));
        Check(OcUnlock.StateText(OcUnlock.ReasonUncleanStop) != Strings.T("oc.state.unclean"), "a new choice waits for the restart: no old notice");
        BoardCapabilities.Replace(BoardCapabilities.Parse(OcCaps(raw, 2, 2, 1, 0x1234)));

        // A driver older than the tiers (ABI 1): no unlock, the plain ranges.
        var abi1 = (byte[])raw.Clone();
        Array.Copy(BitConverter.GetBytes(1u), 0, abi1, 16, 4);
        for (int i = 139; i < 192; i++) Array.Copy(BitConverter.GetBytes(0u), 0, abi1, i * 4, 4);
        BoardCapabilities.Replace(BoardCapabilities.Parse(abi1));
        Check(!OcUnlock.Available && DpmSettings.HardMaxMHz == 2000u && CpuTuning.MaxMHz == 3500u, "ABI 1: no unlock, the plain ranges");
        Check(ParseRefuses(OcCaps(abi1, 0, 0, 0, 0x1234)), "ABI 1 with overclock words is refused");
        // Malformed overclock words.
        Check(ParseRefuses(OcCaps(raw, 3, 0, 0, 0x1234)), "tier 3 is refused");
        Check(ParseRefuses(OcCaps(raw, 2, 2, 1, 0)), "a tier without a board is refused");
        Check(ParseRefuses(OcCaps(raw, 0, 0, 7, 0x1234)), "reason 7 is refused");
        Check(!ParseRefuses(OcCaps(raw, 0, 0, 6, 0x1234)), "reason 6 (an unexpected stop) is reported");
        Check(!ParseRefuses(OcCaps(raw, 0, 7, 2, 0x1234)), "a hand-written stored tier is reported as it is");
        foreach (int word in new[] { 143, 144, 148, 152, 156, 157, 158, 170, 171, 191 })
        {
            var bad = (byte[])raw.Clone();
            Array.Copy(BitConverter.GetBytes(bad[word * 4] == 7 ? 8u : 7u), 0, bad, word * 4, 4);
            Check(ParseRefuses(bad), "a changed overclock word " + word + " is refused");
        }

        // The questions: the cooler, then three warnings; any stop ends the run with no tier.
        var q = new OcUnlock.Questions();
        q.Cooling(OcUnlock.None);
        Check(q.Step == OcUnlock.Step.Stopped && q.Tier == 0, "not sure: stopped");
        q = new OcUnlock.Questions(); q.Cooling(OcUnlock.Air);
        Check(q.Step == OcUnlock.Step.Warning1 && q.WarningKey == "oc.warn.1", "air: the first warning");
        q.Answer(true); q.Answer(true);
        Check(q.WarningKey == "oc.warn.3", "the third warning");
        q.Answer(true);
        Check(q.Step == OcUnlock.Step.Done && q.Tier == OcUnlock.Air && q.WarningKey == null, "three yes: air");
        q = new OcUnlock.Questions(); q.Cooling(OcUnlock.Liquid); q.Answer(true); q.Answer(false);
        Check(q.Step == OcUnlock.Step.Stopped && q.Tier == 0 && q.WarningKey == null, "a stop at the second warning: no tier");
        q.Answer(true);
        Check(q.Step == OcUnlock.Step.Stopped, "a stopped run stays stopped");
        q = new OcUnlock.Questions(); q.Cooling(7);
        Check(q.Step == OcUnlock.Step.Stopped, "an unknown cooler stops");

        // The plan, on a BC-250 with a fingerprint.
        BoardCapabilities.Replace(BoardCapabilities.Parse(OcCaps(raw, 0, 0, 0, 0x1234)));
        var s = Open();
        var p = Recovery.Plan(OcUnlock.Action, s, more: new Recovery.PlanArgs { OcTier = OcUnlock.Liquid });
        Check(!p.Refused && Writes(p, "OcTier=2", "OcTierBoard=4660"), "liquid: the tier and this board: " + p.Refusal);
        Check(p.OfferRestart && p.Effect == "at the next restart of Windows" && p.Undoable, "the unlock applies at the next restart of Windows");
        var d = PlainPlan.Describe(p);
        Check(d.Changes.Count == 2 && d.Changes.Contains(Strings.T("plan.line.oc-liquid")) && d.Changes.Contains(Strings.T("plan.line.oc-board")), "the dialog names the tier and the board");
        Check(d.Notes.Contains(Strings.T("oc.note.limits")), "the dialog says that the limits stay until raised");
        foreach (var w in p.Writes) Check(PlainPlan.LineId(w) != null, "G-PLAN: " + w.Name + " has a plain sentence");
        s.Parameters["OcTier"] = 2; s.Parameters["OcTierBoard"] = 0x1234;
        Check(Recovery.Plan(OcUnlock.Action, s, more: new Recovery.PlanArgs { OcTier = OcUnlock.Liquid }).Refusal.Contains("already"), "the same tier is refused");
        s.Parameters["OcTierBoard"] = 0x9999;
        Check(Writes(Recovery.Plan(OcUnlock.Action, s, more: new Recovery.PlanArgs { OcTier = OcUnlock.Liquid }), "OcTierBoard=4660"), "another board's binding is replaced");
        // Back to the standard limits: both go, and a stored ceiling above the new top comes down to it.
        s.Parameters["DpmMaxMHz"] = 2300;
        var off = Recovery.Plan(OcUnlock.Action, s, more: new Recovery.PlanArgs { OcTier = OcUnlock.None });
        Check(!off.Refused && Writes(off, "OcTier-", "OcTierBoard-", "DpmMaxMHz=2000"), "standard limits: the tier and the binding go, the ceiling comes to 2000 MHz");
        s.Parameters["DpmMaxMHz"] = 2300;
        Check(Writes(Recovery.Plan(OcUnlock.Action, s, more: new Recovery.PlanArgs { OcTier = OcUnlock.Air }), "OcTier=1", "OcTierBoard=4660", "DpmMaxMHz=2200"), "air under a 2300 ceiling: 2200 MHz");
        Check(Recovery.Plan(OcUnlock.Action, s, more: new Recovery.PlanArgs { OcTier = OcUnlock.Air }).PlainNotes.Contains(Strings.T("oc.note.lowered")),
            "a lowered ceiling: the dialog says so");
        // The processor limit and undervolt above the new tier come down to its limits too, and the dialog says so.
        s.Parameters["OcTier"] = 2; s.Parameters["OcTierBoard"] = 0x1234; s.Parameters["DpmMaxMHz"] = 2300;
        s.Parameters["CpuMaxMHz"] = 4100; s.Parameters["CpuUvSteps"] = 40;
        uint airUv = OcUnlock.CpuUvMaxSteps(OcUnlock.Air), noneUv = OcUnlock.CpuUvMaxSteps(OcUnlock.None);
        var down = Recovery.Plan(OcUnlock.Action, s, more: new Recovery.PlanArgs { OcTier = OcUnlock.Air });
        Check(!down.Refused && Writes(down, new[] { "OcTier=1", "DpmMaxMHz=2200", "CpuMaxMHz=3900" }.Concat(40 > airUv ? new[] { "CpuUvSteps=" + airUv } : new string[0]).ToArray()),
            "liquid to air: the ceiling and the processor limit come down: " + down.Refusal);
        var dd = PlainPlan.Describe(down);
        Check(dd.Changes.Contains(Strings.T("plan.line.cpu-clock", "3900")) && dd.Notes.Contains(Strings.T("oc.note.lowered")), "the dialog names the new processor limit");
        foreach (var w in down.Writes) Check(PlainPlan.LineId(w) != null && Recovery.Allowed(w.Path, w.Name), "G-PLAN: " + w.Name + " has a plain sentence and is allowed");
        var none = Recovery.Plan(OcUnlock.Action, s, more: new Recovery.PlanArgs { OcTier = OcUnlock.None });
        Check(!none.Refused && Writes(none, new[] { "OcTier-", "OcTierBoard-", "DpmMaxMHz=2000", "CpuMaxMHz=3500" }.Concat(40 > noneUv ? new[] { "CpuUvSteps=" + noneUv } : new string[0]).ToArray()),
            "liquid to standard: everything comes to the standard limits");
        // Values inside the new limits stay as they are.
        s.Parameters["CpuMaxMHz"] = 3400; s.Parameters["CpuUvSteps"] = 0; s.Parameters["DpmMaxMHz"] = 1800;
        var keep = Recovery.Plan(OcUnlock.Action, s, more: new Recovery.PlanArgs { OcTier = OcUnlock.Air });
        Check(Writes(keep, "OcTier=1") && !keep.PlainNotes.Contains(Strings.T("oc.note.lowered")), "values inside the limits: only the tier");
        s.Parameters.Remove("CpuMaxMHz"); s.Parameters.Remove("CpuUvSteps");
        s.Parameters.Remove("OcTier"); s.Parameters.Remove("OcTierBoard"); s.Parameters.Remove("DpmMaxMHz");
        Check(Recovery.Plan(OcUnlock.Action, s, more: new Recovery.PlanArgs { OcTier = OcUnlock.None }).Refusal.Contains("already"), "nothing stored: the standard limits are in force already");
        Check(Recovery.Plan(OcUnlock.Action, s).Refusal.Contains("No cooling type"), "no cooling type given");
        Check(Recovery.Plan(OcUnlock.Action, s, more: new Recovery.PlanArgs { OcTier = 3 }).Refusal.Contains("No cooling type"), "an unknown cooling type");
        // The reset takes the unlock away.
        var reset = ResetFixture(); reset.Parameters["OcTier"] = 1; reset.Parameters["OcTierBoard"] = 0x1234;
        var rp = Recovery.Plan("reset-defaults", reset, more: new Recovery.PlanArgs { Games = "keep" });
        Check(rp.Writes.Any(w => w.Name == "OcTier" && w.Delete) && rp.Writes.Any(w => w.Name == "OcTierBoard" && w.Delete), "the reset removes the unlock: " + rp.Refusal);
        // The board gate: another board, or a driver without the fingerprint, has no unlock.
        BoardCapabilities.Replace(fixture);
        Check(Recovery.Plan(OcUnlock.Action, Open(), more: new Recovery.PlanArgs { OcTier = OcUnlock.Air }).Refused, "no fingerprint: refused");
        Check(BoardCapabilities.ActionPermission(OcUnlock.Action) == (BoardCapabilities.Gpu | BoardCapabilities.Cpu), "the action needs the graphics and processor controls");
        BoardCapabilities.Replace(null);
        Check(Recovery.Plan(OcUnlock.Action, Open(), more: new Recovery.PlanArgs { OcTier = OcUnlock.Air }).Refused, "an unknown board: refused");
        Check(!OcUnlock.Available && OcUnlock.StateText() == Strings.T("board.unsupported"), "an unknown board: not supported");

        // Every text of the unlock exists in every language and names no driver internals.
        foreach (var lang in Strings.Languages)
        {
            Strings.Language = lang;
            foreach (var id in Strings.Tables["en"].Entries.Keys.Where(k => k.StartsWith("oc.", StringComparison.Ordinal) || k.StartsWith("plan.line.oc-", StringComparison.Ordinal) ||
                k == "plan.line.cpu-clock" || k == "plan.line.cpu-uv"))
            {
                Check(Strings.Tables[lang].Entries.ContainsKey(id), lang + " has " + id);
                foreach (var x in PlainWords.Findings(new[] { Strings.T(id, 1, 2, 3) })) Check(false, "G-NOINT: " + lang + " " + id + ": " + x);
            }
        }
        Strings.Language = "en";
        // The warning about the power lock says what to do in plain words.
        var lockText = Strings.T("oc.warn.2");
        Check(lockText.Contains("power cable") && lockText.Contains("40") && lockText.Contains("1850"), "the lock warning names the cable, the 40 cores and the band");
        BoardCapabilities.Replace(fixture);
    }
}
