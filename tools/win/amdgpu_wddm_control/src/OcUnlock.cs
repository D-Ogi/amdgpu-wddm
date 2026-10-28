// The overclock unlock by cooling type (docs/design/dpm.md, "Overclock tiers"; docs/hardware.md). The person answers
// one question (the cooler on the board) and three warnings; the app then stores a tier, never a clock or a voltage:
// the driver takes every number from its own compiled envelope of that tier (driver/shim/include/bc250_oc.h) and
// reports the envelopes in the board capability query (ABI 2, BoardCapabilities.cs). The tier is tied to this board:
// OcTierBoard holds the board fingerprint the driver reports, and a start on another board deletes both values.
// A positively identified BC-250 only: every other device gets "not supported" and no control.
using System;
using System.Collections.Generic;
using System.Linq;

namespace AmdgpuWddmControl
{
    public static class OcUnlock
    {
        public const string TierName = "OcTier", BoardName = "OcTierBoard";
        public const uint None = 0, Air = 1, Liquid = 2;
        public const string Action = "oc-tier";

        static BoardCapabilities C { get { return BoardCapabilities.Current; } }
        static bool Abi2 { get { return C[4] == 2; } }

        // The board can take a tier: a supported BC-250 with the GPU and CPU controls admitted, and a driver that
        // reports the overclock words and a board fingerprint.
        public static bool Available { get { return C.Allows(BoardCapabilities.Gpu | BoardCapabilities.Cpu) && Abi2 && C[142] != 0; } }
        public static uint Effective { get { return Available ? C[139] : None; } }
        public static uint Stored { get { return Abi2 ? C[140] : None; } }
        public static uint Reason { get { return Abi2 ? C[141] : 0; } }
        public static uint Board { get { return Abi2 ? C[142] : 0; } }

        public static bool IsTier(uint tier) { return tier == None || tier == Air || tier == Liquid; }

        // The envelope of a tier as the driver reports it; the plain envelope without ABI 2.
        public static uint GpuMaxMHz(uint tier) { return Abi2 && tier < 3 ? C[144 + (int)tier * 4] : C[10]; }
        public static uint GpuMaxMv(uint tier) { return Abi2 && tier < 3 ? C[145 + (int)tier * 4] : C[14]; }
        public static uint CpuMaxMHz(uint tier) { return Abi2 && tier < 3 ? C[146 + (int)tier * 4] : C[18]; }
        public static uint CpuUvMaxSteps(uint tier) { return Abi2 && tier < 3 ? C[147 + (int)tier * 4] : C[21]; }

        // The clock points above the plain ceiling that a tier admits, lowest first.
        public static uint[] GpuClocks(uint tier)
        {
            if (!Abi2) return new uint[0];
            uint top = GpuMaxMHz(tier);
            return Enumerable.Range(0, 3).Select(i => C[156 + i * 3]).Where(mhz => mhz > C[10] && mhz <= top).ToArray();
        }

        // The least CPU undervolt, in steps, that a clock limit needs under a tier. 0 up to the release bound;
        // uint.MaxValue for a clock no tier admits.
        public static uint CpuMinUvSteps(uint mhz)
        {
            if (mhz <= C[18]) return 0;
            if (!Abi2 || mhz < 3600 || mhz > 4100 || mhz % 100 != 0) return uint.MaxValue;
            return C[165 + (int)((mhz - 3600) / 100)];
        }

        // ---- the questions ------------------------------------------------------------------------------------

        public enum Step { Cooling, Warning1, Warning2, Warning3, Done, Stopped }

        // The order of the questions: the cooler first, then three warnings, each of which the person must accept.
        // Any "no" stops the whole run and nothing is written. A board with the stock passive heatsink and no fan,
        // or a person who does not know, gets no tier.
        public sealed class Questions
        {
            public Step Step = Step.Cooling;
            public uint Tier = None;

            public void Cooling(uint tier)
            {
                if (Step != Step.Cooling) return;
                if (tier != Air && tier != Liquid) { Step = Step.Stopped; return; }
                Tier = tier; Step = Step.Warning1;
            }

            public void Answer(bool accepted)
            {
                if (Step != Step.Warning1 && Step != Step.Warning2 && Step != Step.Warning3) return;
                if (!accepted) { Step = Step.Stopped; Tier = None; return; }
                Step = Step == Step.Warning1 ? Step.Warning2 : Step == Step.Warning2 ? Step.Warning3 : Step.Done;
            }

            public string WarningKey { get { return Step == Step.Warning1 ? "oc.warn.1" : Step == Step.Warning2 ? "oc.warn.2" : Step == Step.Warning3 ? "oc.warn.3" : null; } }
        }

        // ---- the writes ---------------------------------------------------------------------------------------

        // A tier and the board it belongs to, both or neither. A lower tier also brings a stored GPU ceiling above
        // its own down to its ceiling, so the automatic clocks stay on with a value the driver takes as written.
        public static List<RegWrite> PlanWrites(uint? storedTier, uint? storedBoard, uint? storedCeiling, uint tier, uint board)
        {
            var writes = new List<RegWrite>();
            if (tier == None)
            {
                if (storedTier != null) writes.Add(RegWrite.Remove(DpmSettings.RegistryPath, TierName));
                if (storedBoard != null) writes.Add(RegWrite.Remove(DpmSettings.RegistryPath, BoardName));
            }
            else
            {
                if (storedTier != tier) writes.Add(RegWrite.Dword(DpmSettings.RegistryPath, TierName, tier));
                if (storedBoard != board) writes.Add(RegWrite.Dword(DpmSettings.RegistryPath, BoardName, board));
            }
            uint top = GpuMaxMHz(tier);
            if (storedCeiling != null && storedCeiling.Value > top) writes.Add(RegWrite.Dword(DpmSettings.RegistryPath, "DpmMaxMHz", top));
            return writes;
        }

        // ---- the texts ----------------------------------------------------------------------------------------

        public static string TierText(uint tier)
        {
            return Strings.T(tier == Air ? "oc.tier.air" : tier == Liquid ? "oc.tier.liquid" : "oc.tier.none");
        }

        // What this start runs, and why when that is not what was chosen.
        public static string StateText()
        {
            if (!Available) return Strings.T("board.unsupported");
            uint e = Effective, s = Stored, r = Reason;
            if (r == 5) return Strings.T("oc.state.board-changed");
            if (r == 4) return Strings.T("oc.state.unbound");
            if (r == 2) return Strings.T("oc.state.invalid");
            return Strings.T("oc.state.runs", TierText(e), GpuMaxMHz(e), CpuMaxMHz(e)) + (s != e ? " " + Strings.T("oc.state.pending") : "");
        }

        // The text of the plan's one write of the tier (PlainPlan.LineId).
        public static string LineId(RegWrite w)
        {
            if (w.Name == TierName) return w.Delete || w.Number == None ? "plan.line.oc-off" : w.Number == Liquid ? "plan.line.oc-liquid" : "plan.line.oc-air";
            if (w.Name == BoardName) return w.Delete ? "plan.line.oc-off" : "plan.line.oc-board";
            return null;
        }
    }
}
