// The tuning model: the GPU voltage curve and the processor settings, as plain data and pure functions. No
// P/Invoke and no WinForms here, so test/UnitTests.cs runs all of it on any PC.
//
// The board capability query supplies the envelope and default vectors. The driver remains the
// authority. A missing or malformed query supplies no tuning range and grants no mutation.
using System;
using System.Globalization;
using System.Linq;

namespace AmdgpuWddmControl
{
    // enum bc250_clock_curve_error, in its own numbering.
    // BC250_CLOCK_CURVE_* of driver/shim/include/bc250_clock.h. Untried is the driver's alone: this app never
    // returns it from Check, and the driver answers it to a Keep of a candidate the governor has not applied.
    public enum CurveError { Ok = 0, Range = 1, Depth = 2, Order = 3, Floor = 4, None = 5, Untried = 6 }


    public static class Tuner
    {
        public static int Points { get { return BoardCapabilities.Current.GpuVoltages.Length; } }
        public static uint FirstMHz { get { return BoardCapabilities.Current[9]; } }
        public static uint StepMHz { get { return BoardCapabilities.Current[11]; } }
        public static uint FloorMv { get { return BoardCapabilities.Current[13]; } }
        public static uint CeilingMv { get { return BoardCapabilities.Current[14]; } }
        public static uint BandMv { get { return BoardCapabilities.Current[15]; } }
        public static uint[] TableMv { get { return BoardCapabilities.Current.GpuVoltages; } }

        public static uint MHzAt(int index) { return FirstMHz + (uint)index * StepMHz; }

        // The lowest voltage admitted at a clock: the table's line less the band, never under the lab floor.
        public static uint FloorAt(int index)
        {
            uint line = TableMv[index];
            return line > FloorMv + BandMv ? line - BandMv : FloorMv;
        }

        // AMD's SVI2 encoding, as the driver computes it. Two neighbours may round to the same VID, never invert.
        public static uint Vid(uint mv) { return (1550 - mv) * 160 / 1000; }

        public static uint[] Table() { return (uint[])TableMv.Clone(); }

        public static uint[] Floors()
        {
            var f = new uint[Points];
            for (int i = 0; i < Points; i++) f[i] = FloorAt(i);
            return f;
        }

        // bc250_clock_curve_check, value for value and in the same order, so that the window's refusal and the
        // driver's refusal are the same sentence. level is the index of the value that broke a rule, -1 when none.
        public static CurveError Check(uint[] mv, uint[] floor, out int level)
        {
            level = -1;
            if (!BoardCapabilities.Current.Allows(BoardCapabilities.Gpu) || mv == null || mv.Length != Points) return CurveError.None;
            floor = floor != null && floor.Length == Points ? floor : Floors();
            for (int i = 0; i < Points; i++)
            {
                level = i;
                if (mv[i] < FloorMv || mv[i] > CeilingMv) return CurveError.Range;
                if (i == 0 && mv[i] != FloorMv) return CurveError.Floor;
                if (mv[i] < floor[i]) return CurveError.Depth;
                if (i > 0 && (mv[i] < mv[i - 1] || Vid(mv[i]) > Vid(mv[i - 1]))) return CurveError.Order;
            }
            level = -1;
            return CurveError.Ok;
        }

        public static bool IsDefault(uint[] mv, uint[] table)
        {
            table = table != null && table.Length == Points ? table : TableMv;
            return mv != null && mv.Length == Points && !mv.Where((v, i) => v != table[i]).Any();
        }

        // The presets of the window: how deep under the table's line each one goes. "standard" is the line itself.
        public static readonly string[] PresetNames = { "standard", "mild", "medium", "deep" };

        // The same three depths as the command line (bc250kmd_cli dpm curve preset, docs/design/tuner.md), so that a
        // person and an operator mean the same curve by "mild".
        public static uint PresetDepth(string name)
        {
            return name == "mild" ? 10u : name == "medium" ? 20u : name == "deep" ? BandMv : 0u;
        }

        // A preset applied to the line the driver sent: each value goes down by the preset's depth, never under
        // that clock's own lowest voltage, and the first level stays at the floor. The result is a legal curve by
        // construction, which is why the window can offer it without a check.
        public static uint[] Preset(string name, uint[] table, uint[] floor)
        {
            table = table != null && table.Length == Points ? table : TableMv;
            floor = floor != null && floor.Length == Points ? floor : Floors();
            uint depth = PresetDepth(name);
            var mv = new uint[Points];
            for (int i = 0; i < Points; i++)
            {
                uint low = Math.Max(floor[i], FloorMv);
                mv[i] = table[i] > depth && table[i] - depth > low ? table[i] - depth : low;
                if (i > 0 && mv[i] < mv[i - 1]) mv[i] = mv[i - 1];
            }
            mv[0] = FloorMv;
            return mv;
        }

        // What a control has to tell the page after an input handler ends.
        public enum Raise { None = 0, Picked = 1, Changed = 2 }

        // The page rebuilds itself when a knot moves, which disposes the control that moved it. A control therefore
        // holds its events while a mouse drag or a key press runs, and reports once, at the end, when nothing of the
        // control is touched any more. A moved knot outranks a moved selection, because the page reads both.
        public struct ChangeGate
        {
            bool _hold, _moved, _picked;

            public bool Held { get { return _hold; } }

            // An input handler starts. Everything it records waits for Release.
            public void Hold() { _hold = true; }

            // A knot moved (moved = true) or the selection moved (false). Returns what to report now, which is
            // nothing while a handler holds the gate.
            public Raise Mark(bool moved)
            {
                if (moved) _moved = true; else _picked = true;
                return _hold ? Raise.None : Take();
            }

            // The handler ended: one report stands for everything inside it, and a second call reports nothing.
            public Raise Release() { _hold = false; return Take(); }

            Raise Take()
            {
                Raise r = _moved ? Raise.Changed : _picked ? Raise.Picked : Raise.None;
                _moved = false;
                _picked = false;
                return r;
            }
        }

        // Which preset a curve is, or null when it is none of them (an edited curve).
        public static string PresetOf(uint[] mv, uint[] table, uint[] floor)
        {
            foreach (var name in PresetNames)
            {
                var p = Preset(name, table, floor);
                if (mv != null && mv.Length == Points && !mv.Where((v, i) => v != p[i]).Any()) return name;
            }
            return null;
        }

        // One value moved by delta millivolts, clamped to that clock's band. The neighbours are left alone: the
        // check says whether the result is legal, so a person can see what is wrong instead of being steered.
        public static uint Nudge(uint mv, int delta, int index, uint[] table, uint[] floor)
        {
            table = table != null && table.Length == Points ? table : TableMv;
            floor = floor != null && floor.Length == Points ? floor : Floors();
            long v = (long)mv + delta;
            uint low = Math.Max(FloorMv, floor[index]);
            uint high = Math.Min(CeilingMv, table[index]);
            if (index == 0) return FloorMv;
            return (uint)Math.Max(low, Math.Min(high, v));
        }

        public static string DeltaText(uint mv, uint line)
        {
            if (mv == line) return Strings.T("tuner.curve.delta.line");
            return mv < line ? Strings.T("tuner.curve.delta.under", line - mv) : Strings.T("tuner.curve.delta.over", mv - line);
        }

        public static string ErrorText(CurveError error, uint mhz)
        {
            switch (error)
            {
                case CurveError.Range: return Strings.T("tuner.curve.error.range", mhz);
                case CurveError.Depth: return Strings.T("tuner.curve.error.depth", mhz);
                case CurveError.Order: return Strings.T("tuner.curve.error.order", mhz);
                case CurveError.Floor: return Strings.T("tuner.curve.error.floor");
                case CurveError.None: return Strings.T("tuner.curve.error.none");
                case CurveError.Untried: return Strings.T("tuner.curve.error.untried");
                default: return "";
            }
        }

        // The trial's remaining time, for the countdown next to the Keep button. Whole seconds, rounded up, so
        // that "1 second left" is never shown for a window that has already closed.
        public static string Countdown(uint remainingMs)
        {
            uint seconds = (remainingMs + 999) / 1000;
            return Strings.T("tuner.trial.left", seconds);
        }
    }

    // The processor side: the clock limit, the undervolt in the firmware's own curve steps, the temperature cap
    // and the core mask. Every bound is the driver's (driver/shim/include/bc250_cpu.h); every one of them is a
    // community report and measured by nobody on this part, which is why the window says so and offers a trial.
    public static class CpuTuning
    {
        // BC250_CPU_MIN_MHZ, BC250_CPU_MAX_MHZ, BC250_CPU_UV_MAX_STEPS, BC250_CPU_TEMP_MIN_C and
        // BC250_CPU_TEMP_MAX_C of driver/shim/include/bc250_cpu.h. MaxMHz is the highest limit the release build
        // of the driver takes, not a stock clock: nobody has measured this part's own boost ceiling.
        public static uint MinMHz { get { return BoardCapabilities.Current[17]; } }
        public static uint MaxMHz { get { return BoardCapabilities.Current[18]; } }
        public static uint MaxSteps { get { return BoardCapabilities.Current[21]; } }
        public static uint MinTempC { get { return BoardCapabilities.Current[22]; } }
        public static uint MaxTempC { get { return BoardCapabilities.Current[23]; } }
        public static uint MaskStock { get { return BoardCapabilities.Current[25]; } }
        public static uint MaskFull { get { return BoardCapabilities.Current[26]; } }
        public static uint StockCores { get { return BoardCapabilities.Current[138]; } }
        public static uint FullCores { get { return BoardCapabilities.Current[27]; } }
        public static uint RefuseMv { get { return BoardCapabilities.Current[24]; } }

        public static bool ValidClock(uint mhz) { return BoardCapabilities.Current.Allows(BoardCapabilities.Cpu) && mhz >= MinMHz && mhz <= MaxMHz && mhz % 100 == 0; }
        public static bool ValidSteps(uint steps) { return BoardCapabilities.Current.Allows(BoardCapabilities.Cpu) && steps <= MaxSteps; }
        public static bool ValidTemp(uint c) { return BoardCapabilities.Current.Allows(BoardCapabilities.Cpu) && c >= MinTempC && c <= MaxTempC; }
        public static bool ValidMask(uint mask) { return BoardCapabilities.Current.Allows(BoardCapabilities.Cpu) && (mask == MaskStock || mask == MaskFull); }
        public static bool ValidCores(uint cores) { return BoardCapabilities.Current.Allows(BoardCapabilities.Cpu) && (cores == StockCores || cores == FullCores); }

        public static uint MaskFor(uint cores) { return cores == FullCores ? MaskFull : MaskStock; }
        public static uint CoresFor(uint mask) { return mask == MaskFull ? FullCores : StockCores; }

        // The clock choices the window offers: the whole admitted band on the 100 MHz grid, highest first.
        public static uint[] ClockChoices()
        {
            if (!BoardCapabilities.Current.Allows(BoardCapabilities.Cpu)) return new uint[0];
            int count = (int)((MaxMHz - MinMHz) / 100) + 1;
            var v = new uint[count];
            for (int i = 0; i < count; i++) v[i] = MaxMHz - (uint)i * 100;
            return v;
        }

        // What a voltage readback means. The driver undoes a change above the refusal line by itself; the window
        // only has to say what it saw.
        public static string VoltageText(uint mv)
        {
            if (mv == 0) return Strings.T("perf.no-reading");
            return mv >= RefuseMv ? Strings.T("tuner.cpu.voltage.high", mv) : Strings.T("tuner.cpu.voltage", mv);
        }

        public static string StepsText(uint steps)
        {
            return steps == 0 ? Strings.T("tuner.cpu.uv.none") : Strings.T("tuner.cpu.uv.steps", steps);
        }

        public static string Number(uint value) { return value.ToString(CultureInfo.CurrentCulture); }
    }
}
