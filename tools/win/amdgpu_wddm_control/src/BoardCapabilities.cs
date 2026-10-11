// Board envelope from the driver's software-only capability query. No legacy-driver defaults.
using System;
using System.Linq;

namespace AmdgpuWddmControl
{
    public sealed class BoardCapabilities
    {
        public const int Bytes = 768;
        public const uint Fan = 2, Gpu = 4, Cpu = 8, Memory = 16;
        readonly uint[] words;
        BoardCapabilities(uint[] value) { words = value; }
        public static BoardCapabilities Current { get; private set; } = new BoardCapabilities(new uint[192]);
        public static void Replace(BoardCapabilities value) { Current = value ?? new BoardCapabilities(new uint[192]); }
        public uint this[int i] { get { return words[i]; } }
        public bool Allows(uint permission) { return words[5] == 1 && words[7] == 0 && (words[6] & (1 | permission)) == (1 | permission); }
        public uint[] GpuVoltages { get { return Enumerable.Range(0, (int)words[12]).Where(i => words[38 + i * 3] >= words[9]).Select(i => words[39 + i * 3]).ToArray(); } }
        public uint[] GpuClocks { get { return Enumerable.Range(0, (int)words[12]).Where(i => words[38 + i * 3] >= words[9]).Select(i => words[38 + i * 3]).ToArray(); } }
        public bool FanPreset(uint profile, out uint[] c, out uint[] pct)
        {
            c = pct = null;
            if (profile < 1 || profile > 3 || !Allows(Fan)) return false;
            int n = (int)words[85 + profile], start = 89 + ((int)profile - 1) * 16;
            c = Enumerable.Range(0, n).Select(i => words[start + 2 * i]).ToArray();
            pct = Enumerable.Range(0, n).Select(i => words[start + 2 * i + 1]).ToArray();
            return true;
        }
        public static uint ActionPermission(string action)
        {
            if (action.StartsWith("fan-", StringComparison.Ordinal)) return Fan;
            if (action.StartsWith("cpu-", StringComparison.Ordinal) || action == "core-mask") return Cpu;
            if (action.StartsWith("tune-", StringComparison.Ordinal) || action.StartsWith("cu-", StringComparison.Ordinal) || action == "enable-dpm" || action == "set-clocks") return Gpu;
            // These broad operations can restore/remove board policy and need all policy owners admitted.
            if (action == "undo" || action == "reset-defaults") return Fan | Gpu | Cpu;
            return 0;
        }
        static void Require(bool condition) { if (!condition) throw new FormatException("Invalid board capabilities"); }
        public static BoardCapabilities Parse(byte[] b)
        {
            Require(b != null && b.Length == Bytes);
            var w = new uint[192];
            for (int i = 0; i < w.Length; ++i) w[i] = BitConverter.ToUInt32(b, i * 4);
            Require(w[0] == KmdReply.Magic && w[1] == 33 && w[2] == 0 && w[3] == 0 && w[4] == 1);
            Require((w[6] & ~31u) == 0 && w[7] <= 2 && w.Skip(139).All(x => x == 0));
            if (w[5] == 0) { Require(w[6] == 0 && w[7] == 1 && w.Skip(8).All(x => x == 0)); return new BoardCapabilities(w); }
            Require(w[5] == 1 && (w[6] & 1) != 0 && w[7] != 1 && (w[7] == 0 || w[6] == 1));
            Require(w[8] > 0 && w[8] <= w[9] && w[9] <= w[10] && w[11] > 0 && w[12] > 0 && w[12] <= 16);
            Require(w[13] > 0 && w[13] <= w[14] && w[14] <= 1550 && w[15] <= w[14] && w[16] > 0);
            Require(w[18] <= int.MaxValue && w[18] - w[17] <= 102400 && w[23] <= int.MaxValue && w[23] - w[22] <= 256);
            Require(w[17] > 0 && w[17] <= w[18] && w[18] <= w[19] && w[20] > 0 && w[22] <= w[23] && w[24] > 0);
            Require(w[25] != 0 && (w[25] & ~w[26]) == 0 && w[27] > 0 && w[27] <= 32 && w[138] > 0 && w[138] <= w[27]);
            Require(w[29] <= int.MaxValue && w[29] - w[28] <= 256);
            Require(w[28] < w[29] && w[30] <= w[31] && w[31] <= 100 && w[32] > 0 && w[33] >= 2 && w[33] <= w[34] && w[34] <= 8);
            Require(w[35] > 0 && w[35] <= w[37] && w[37] <= w[36]);
            for (int i = 0; i < 16; i++) {
                int o = 38 + i * 3;
                if (i >= w[12]) { Require(w[o] == 0 && w[o+1] == 0 && w[o+2] == 0); continue; }
                Require(w[o] == (ulong)w[8] + (ulong)i * w[11] && w[o] <= w[10] && w[o+1] >= w[13] && w[o+1] <= w[14]);
                Require(w[o+2] == (1550 - w[o+1]) * 160 / 1000);
                if (i != 0) Require(w[o+1] >= w[o-2]);
            }
            Require(w[38 + ((int)w[12]-1)*3] == w[10] && w[137] >= w[9] && w[137] <= w[10] && (w[137] - w[8]) % w[11] == 0 && (w[9] - w[8]) % w[11] == 0);
            for (int p = 0; p < 3; p++) {
                Require(w[86+p] >= w[33] && w[86+p] <= w[34]);
                for (int i = 0; i < 8; i++) {
                    int o = 89 + p*16 + i*2;
                    if (i >= w[86+p]) { Require(w[o] == 0 && w[o+1] == 0); continue; }
                    Require(w[o] >= w[28] && w[o] <= w[29] && w[o+1] >= w[30] && w[o+1] <= w[31]);
                    if (i != 0) Require(w[o] > w[o-2] && w[o+1] >= w[o-1]);
                }
            }
            return new BoardCapabilities(w);
        }
    }
}
