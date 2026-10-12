// BD-093 state B: the instruction rate law probe (D1 of scratch\bd093\next\MECHANISM.md).
//
// Why a second probe. The 2026-10-12 arm fitted all seven managed kernels of bd093probe.cs with one law,
// cycles per iteration = max(latency chain, k * instruction count), and found k = 1.1 cycles per x86
// instruction in state B against 0.227 in state A. Two numbers in that fit were estimates, not
// measurements: the instruction count of each RyuJIT loop body, and the state B clock. This probe removes
// both. It emits its own machine code, so it knows every instruction and every byte it executes, and it
// carries two latency-bound clock witnesses whose cost cannot be touched by an instruction rate cap.
//
// It is read only on the hardware: it runs instructions and reads two time bases (QPC and RDTSC). It sends
// no SMU message, writes no register and allocates no GPU resource.
//
// Build:  csc.exe /nologo /optimize+ /platform:x64 /out:bd093law.exe bd093law.cs
// Modes:  selftest | law <cpu> <ms> | quick <cpu> <ms> | pairs <ms> | cpuid | cache <cpu> <ms>
//
// Every line is one measurement, flushed, invariant-formatted, prefixed with the mode name.
using System;
using System.Collections.Generic;
using System.Diagnostics;
using System.Globalization;
using System.Runtime.InteropServices;
using System.Threading;

internal static class Probe
{
    // ---- platform ------------------------------------------------------------------------------
    [DllImport("kernel32.dll")] private static extern IntPtr GetCurrentThread();
    [DllImport("kernel32.dll")] private static extern UIntPtr SetThreadAffinityMask(IntPtr h, UIntPtr mask);
    [DllImport("kernel32.dll")] private static extern bool SetThreadPriority(IntPtr h, int prio);
    [DllImport("kernel32.dll")] private static extern uint GetCurrentProcessorNumber();
    [DllImport("kernel32.dll")] private static extern IntPtr VirtualAlloc(IntPtr addr, UIntPtr size, uint type, uint protect);
    [DllImport("kernel32.dll")] private static extern bool FlushInstructionCache(IntPtr process, IntPtr addr, UIntPtr size);
    [DllImport("kernel32.dll")] private static extern IntPtr GetCurrentProcess();

    private const uint MEM_COMMIT = 0x1000, MEM_RESERVE = 0x2000, PAGE_EXECUTE_READWRITE = 0x40;
    private const int THREAD_PRIORITY_HIGHEST = 2;

    [UnmanagedFunctionPointer(CallingConvention.Cdecl)] private delegate long LoopFn(long loops);
    [UnmanagedFunctionPointer(CallingConvention.Cdecl)] private delegate ulong TscFn();
    [UnmanagedFunctionPointer(CallingConvention.Cdecl)] private delegate void CpuidFn(uint leaf, uint sub, IntPtr outp);
    [UnmanagedFunctionPointer(CallingConvention.Cdecl)] private delegate ulong XgetbvFn(uint index);

    public static ulong Sink;
    private static readonly double TicksPerUs = Stopwatch.Frequency / 1000000.0;
    private static readonly List<object> Keep = new List<object>();   // delegates must outlive the calls

    private static void Line(string s) { Console.WriteLine(s); Console.Out.Flush(); }
    private static string F(double v, int d)
    { return v.ToString("F" + d.ToString(CultureInfo.InvariantCulture), CultureInfo.InvariantCulture); }
    private static string H(uint v) { return "0x" + v.ToString("X8", CultureInfo.InvariantCulture); }

    private static void Pin(int cpu)
    {
        // A negative or out-of-range index would be masked by the shift and pin the thread to an arbitrary CPU
        // instead of failing, so it is refused here and the caller's argument is reported as given.
        if (cpu < 0 || cpu >= 64 || cpu >= Environment.ProcessorCount)
        {
            Console.Error.WriteLine("affinity: cpu " + cpu + " is outside 0.." +
                                    (Environment.ProcessorCount - 1) + ", pinning to 0 instead");
            Line("affinity cpu_requested=" + cpu + " cpu_used=0 processors=" + Environment.ProcessorCount);
            cpu = 0;
        }
        Thread.BeginThreadAffinity();
        if (SetThreadAffinityMask(GetCurrentThread(), (UIntPtr)(1UL << cpu)) == UIntPtr.Zero)
            Console.Error.WriteLine("affinity: SetThreadAffinityMask failed for cpu " + cpu);
        SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_HIGHEST);
    }
    private static void Unpin() { Thread.EndThreadAffinity(); }

    // ---- the emitter ---------------------------------------------------------------------------
    // Win64 volatile registers only: rax, rdx, r8, r9, r10, r11 as accumulators (rcx is the loop counter,
    // and it is the first argument), xmm0 to xmm5 for the vector arms. Nothing non-volatile is touched, so
    // no prologue and no register save is needed and the stub cannot corrupt the managed caller.
    private sealed class Emit
    {
        private readonly IntPtr region;
        private readonly int capacity;
        private int off;

        public Emit(int bytes)
        {
            region = VirtualAlloc(IntPtr.Zero, (UIntPtr)bytes, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE);
            capacity = bytes;
            if (region == IntPtr.Zero)
                throw new InvalidOperationException("VirtualAlloc PAGE_EXECUTE_READWRITE refused (error " +
                                                    Marshal.GetLastWin32Error().ToString(CultureInfo.InvariantCulture) + ")");
        }

        public int Mark() { return off; }

        public void B(params int[] bytes)
        {
            foreach (int b in bytes)
            {
                if (off >= capacity) throw new InvalidOperationException("the emit region is full");
                Marshal.WriteByte(region, off++, (byte)(b & 0xFF));
            }
        }
        public void B32(long v) { B((int)(v & 0xFF), (int)((v >> 8) & 0xFF), (int)((v >> 16) & 0xFF), (int)((v >> 24) & 0xFF)); }
        public void B64(ulong v) { B32((long)(v & 0xFFFFFFFFUL)); B32((long)((v >> 32) & 0xFFFFFFFFUL)); }

        public void Align(int n) { while ((off % n) != 0) B(0x90); }

        public void Patch8(int at, int value) { Marshal.WriteByte(region, at, (byte)(value & 0xFF)); }

        public void Done()
        { FlushInstructionCache(GetCurrentProcess(), region, (UIntPtr)capacity); }

        public IntPtr At(int mark) { return (IntPtr)(region.ToInt64() + mark); }

        // --- integer -----------------------------------------------------------------------------
        private static int RexRm(int rm) { return 0x48 | ((rm >= 8) ? 1 : 0); }
        private static int RexRegRm(int reg, int rm) { return 0x48 | ((reg >= 8) ? 4 : 0) | ((rm >= 8) ? 1 : 0); }

        public void AddImm8(int r, int imm) { B(RexRm(r), 0x83, 0xC0 | (r & 7), imm); }            // 4 bytes
        public void AddImm32(int r, long imm) { B(RexRm(r), 0x81, 0xC0 | (r & 7)); B32(imm); }     // 7 bytes
        public void AddRR(int dst, int src) { B(RexRegRm(src, dst), 0x01, 0xC0 | ((src & 7) << 3) | (dst & 7)); }
        public void ImulRR(int dst, int src) { B(RexRegRm(dst, src), 0x0F, 0xAF, 0xC0 | ((dst & 7) << 3) | (src & 7)); }
        public void MovImm64(int r, ulong v) { B(0x48 | ((r >= 8) ? 1 : 0), 0xB8 | (r & 7)); B64(v); }
        public void SubRcx1() { B(0x48, 0x83, 0xE9, 0x01); }
        public void SubRspImm8(int n) { B(0x48, 0x83, 0xEC, n); }
        public void AddRspImm8(int n) { B(0x48, 0x83, 0xC4, n); }
        public void LoadFromRsp(int r, int disp) { B(RexRegRm(r, 0), 0x8B, 0x40 | ((r & 7) << 3) | 0x04, 0x24, disp); }
        public void StoreToRsp(int r, int disp) { B(RexRegRm(r, 0), 0x89, 0x40 | ((r & 7) << 3) | 0x04, 0x24, disp); }
        public void Ret() { B(0xC3); }
        public void Nop(int size)
        {
            // 0F 1F 84 00 <disp32> is eight bytes; 0x66 prefixes lengthen it. One byte is 0x90.
            if (size == 1) { B(0x90); return; }
            if (size < 8 || size > 15) throw new ArgumentException("nop size must be 1 or 8 to 15");
            for (int i = 0; i < size - 8; i++) B(0x66);
            B(0x0F, 0x1F, 0x84, 0x00); B32(0);
        }

        // --- SSE and AVX -------------------------------------------------------------------------
        public void Addsd(int dst, int src) { B(0xF2, 0x0F, 0x58, 0xC0 | ((dst & 7) << 3) | (src & 7)); }
        public void Paddq(int dst, int src) { B(0x66, 0x0F, 0xD4, 0xC0 | ((dst & 7) << 3) | (src & 7)); }
        public void VpaddqY(int dst, int src1, int src2)
        { B(0xC5, 0x80 | ((~src1 & 0xF) << 3) | 0x04 | 0x01, 0xD4, 0xC0 | ((dst & 7) << 3) | (src2 & 7)); }
        public void Vzeroupper() { B(0xC5, 0xF8, 0x77); }
        public void MovqXmmFromR(int x, int r) { B(0x66, 0x48 | ((r >= 8) ? 1 : 0), 0x0F, 0x6E, 0xC0 | ((x & 7) << 3) | (r & 7)); }
        public void MovqRFromXmm(int r, int x) { B(0x66, 0x48 | ((r >= 8) ? 1 : 0), 0x0F, 0x7E, 0xC0 | ((x & 7) << 3) | (r & 7)); }

        // --- control -----------------------------------------------------------------------------
        public void JnzBack(int top)
        {
            int rel = top - (off + 2);
            if (rel >= -128 && rel <= 127) { B(0x75, rel & 0xFF); return; }
            rel = top - (off + 6);
            B(0x0F, 0x85); B32(rel);
        }
    }

    // One built kernel: what it is, how many instructions and bytes one iteration runs, what work it does.
    private sealed class Kernel
    {
        public string Name;
        public int Instr;          // x86 instructions per iteration, the loop control included
        public int Bytes;          // bytes per iteration, the loop control included
        public double Work;        // semantic work units per iteration (qword adds, multiplies, ...)
        public LoopFn Fn;
        public double Latency;     // the dependent chain of one iteration in cycles, 0 when unknown
    }

    private static Emit code;
    private static TscFn tsc;
    private static CpuidFn cpuid;
    private static XgetbvFn xgetbv;

    private const int RAX = 0, RDX = 2, R8 = 8, R9 = 9, R10 = 10, R11 = 11;
    private static readonly int[] Acc = { RAX, RDX, R8, R9, R10, R11 };

    private static void BuildHelpers()
    {
        int m = code.Mark();
        code.Align(16);
        m = code.Mark();
        code.B(0x0F, 0x31);                       // rdtsc
        code.B(0x48, 0xC1, 0xE2, 0x20);           // shl rdx, 32
        code.B(0x48, 0x09, 0xD0);                 // or rax, rdx
        code.Ret();
        tsc = (TscFn)Marshal.GetDelegateForFunctionPointer(code.At(m), typeof(TscFn));
        Keep.Add(tsc);

        code.Align(16);
        m = code.Mark();
        // void cpuid(uint leaf /*ecx*/, uint sub /*edx*/, uint* out /*r8*/)
        code.B(0x53);                             // push rbx
        code.B(0x89, 0xC8);                       // mov eax, ecx
        code.B(0x89, 0xD1);                       // mov ecx, edx
        code.B(0x0F, 0xA2);                       // cpuid
        code.B(0x41, 0x89, 0x00);                 // mov [r8], eax
        code.B(0x41, 0x89, 0x58, 0x04);           // mov [r8+4], ebx
        code.B(0x41, 0x89, 0x48, 0x08);           // mov [r8+8], ecx
        code.B(0x41, 0x89, 0x50, 0x0C);           // mov [r8+12], edx
        code.B(0x5B);                             // pop rbx
        code.Ret();
        cpuid = (CpuidFn)Marshal.GetDelegateForFunctionPointer(code.At(m), typeof(CpuidFn));
        Keep.Add(cpuid);

        code.Align(16);
        m = code.Mark();
        // ulong xgetbv(uint index /*ecx*/)
        code.B(0x0F, 0x01, 0xD0);                 // xgetbv
        code.B(0x48, 0xC1, 0xE2, 0x20);           // shl rdx, 32
        code.B(0x48, 0x09, 0xD0);                 // or rax, rdx
        code.Ret();
        xgetbv = (XgetbvFn)Marshal.GetDelegateForFunctionPointer(code.At(m), typeof(XgetbvFn));
        Keep.Add(xgetbv);
    }

    private enum Init { Gpr, Fp, Vec, Frame }

    // Normal doubles, so no arm ever runs on denormals: 1.0, 1.5, 2.0, 2.5, 3.0 and 3.5. A chain that
    // reaches +inf keeps running at full speed; a denormal chain would not on every core.
    private static readonly ulong[] FpBits =
    { 0x3FF0000000000000UL, 0x3FF8000000000000UL, 0x4000000000000000UL,
      0x4004000000000000UL, 0x4008000000000000UL, 0x400C000000000000UL };
    private static readonly ulong[] VecBits =
    { 0x0000000100000001UL, 0x0000000200000002UL, 0x0000000300000003UL,
      0x0000000400000004UL, 0x0000000500000005UL, 0x0000000600000006UL };

    // The one builder. `body` emits the loop body and returns the instruction count it emitted.
    private static Kernel Build(string name, double work, double latency, Init init, Func<Emit, int> body)
    {
        code.Align(64);
        int entry = code.Mark();
        bool frame = init == Init.Frame;
        if (frame) code.SubRspImm8(0x78);                       // 120 bytes of private stack for the memory arms
        if (init == Init.Fp || init == Init.Vec)
            for (int i = 0; i < 6; i++)
            {
                code.MovImm64(Acc[i], init == Init.Fp ? FpBits[i] : VecBits[i]);
                code.MovqXmmFromR(i, Acc[i]);
            }
        for (int i = 0; i < Acc.Length; i++) code.MovImm64(Acc[i], (ulong)(0x1000003 + i * 7));
        // The 128-bit init above is legacy SSE. vzeroupper leaves the upper halves clean, so the 256-bit arm
        // never runs with a dirty upper state and the AVX-SSE transition cost cannot enter the measurement.
        if (init == Init.Vec) code.Vzeroupper();
        if (frame)
            for (int i = 0; i < 6; i++) code.StoreToRsp(Acc[i], 8 + i * 8);    // the frame is written before it is read
        code.Align(32);                                                         // the loop top on a fetch boundary
        int top = code.Mark();
        int instr = body(code);
        int bodyEnd = code.Mark();
        code.SubRcx1();
        code.JnzBack(top);
        int iterBytes = bodyEnd - top + (code.Mark() - bodyEnd);
        if (init == Init.Vec) code.Vzeroupper();
        if (init == Init.Fp || init == Init.Vec)
        {
            code.MovqRFromXmm(RAX, 0);
            for (int i = 1; i < 6; i++) { code.MovqRFromXmm(RDX, i); code.AddRR(RAX, RDX); }
        }
        else
            for (int i = 1; i < Acc.Length; i++) code.AddRR(RAX, Acc[i]);
        if (frame) code.AddRspImm8(0x78);
        code.Ret();
        Kernel k = new Kernel();
        k.Name = name; k.Instr = instr + 2; k.Bytes = iterBytes; k.Work = work; k.Latency = latency;
        k.Fn = (LoopFn)Marshal.GetDelegateForFunctionPointer(code.At(entry), typeof(LoopFn));
        Keep.Add(k.Fn);
        return k;
    }

    // ---- the kernel menu ------------------------------------------------------------------------

    // n independent adds with the four-byte imm8 encoding: the slot slope (D1a).
    private static Kernel IndepAdd(int n, bool imm32)
    {
        return Build((imm32 ? "add32_x" : "add_x") + n, n, 1, Init.Gpr, delegate (Emit e)
        {
            for (int i = 0; i < n; i++)
            { if (imm32) e.AddImm32(Acc[i % 6], 0x01020304 + i); else e.AddImm8(Acc[i % 6], 3 + (i & 7)); }
            return n;
        });
    }

    // n nops of a fixed size plus one add, so the stream still retires state: slots against bytes (D1b).
    private static Kernel IndepNop(int n, int size)
    {
        return Build("nop" + size + "_x" + n, 1, 1, Init.Gpr, delegate (Emit e)
        {
            for (int i = 0; i < n; i++) e.Nop(size);
            e.AddImm8(RAX, 1);
            return n + 1;
        });
    }

    // A dependent chain of n multiplies: three cycles each on every x86-64 core since Nehalem and Zen 1,
    // so this is the clock witness that an instruction rate cap cannot reach (3 cycles per instruction is
    // far above the state B k of about 1.1).
    private static Kernel DepImul(int n)
    {
        return Build("dep_imul_x" + n, n, 3.0 * n, Init.Gpr, delegate (Emit e)
        {
            for (int i = 0; i < n; i++) e.ImulRR(RAX, RDX);
            return n;
        });
    }

    // n independent multiplies over six accumulators: one per cycle on Zen 2.
    private static Kernel IndepImul(int n)
    {
        return Build("ind_imul_x" + n, n, 3, Init.Gpr, delegate (Emit e)
        {
            for (int i = 0; i < n; i++) e.ImulRR(Acc[i % 6], Acc[(i + 1) % 6]);
            return n;
        });
    }

    // The second clock witness: eight dependent addsd. Three cycles each on Zen 2 (the 2026-10-12 arm
    // measured 24.00 cycles per iteration in both states), four on several Intel cores, so the two
    // witnesses agreeing is the Zen 2 cross-check and their ratio is printed, never assumed.
    private static Kernel DepFp(int n)
    {
        return Build("dep_fpadd_x" + n, n, 3.0 * n, Init.Fp, delegate (Emit e)
        {
            for (int i = 0; i < n; i++) e.Addsd(0, 1);
            return n;
        });
    }

    // The knee: a 24-cycle dependent FP chain plus m independent integer adds. The fillers are free until
    // k * instructions passes the chain, so the knee position measures k with no instruction count estimate
    // and the flat part is the state A positive control (D1a and D1d).
    private static Kernel Knee(int m)
    {
        return Build("knee_m" + m, 8, 24, Init.Fp, delegate (Emit e)
        {
            for (int i = 0; i < 8; i++) e.Addsd(0, 1);
            for (int i = 0; i < m; i++) e.AddImm8(Acc[1 + (i % 5)], 3 + (i & 7));
            return 8 + m;
        });
    }

    // The port map: n independent instructions of one kind, the same count for every kind. A uniform
    // dispatch gate gives every row the same cycles per instruction; a lost pipe, token or queue gives one
    // row away. This is the no-driver stand-in for the Zen 2 dispatch counter triplet (D4).
    private static Kernel Port(string kind, int n)
    {
        switch (kind)
        {
            case "alu": return Build("port_alu_x" + n, n, 1, Init.Gpr, delegate (Emit e)
                { for (int i = 0; i < n; i++) e.AddImm8(Acc[i % 6], 3 + (i & 7)); return n; });
            case "mul": return Build("port_mul_x" + n, n, 3, Init.Gpr, delegate (Emit e)
                { for (int i = 0; i < n; i++) e.ImulRR(Acc[i % 6], Acc[(i + 1) % 6]); return n; });
            case "fp": return Build("port_fp_x" + n, n, 3, Init.Fp, delegate (Emit e)
                { for (int i = 0; i < n; i++) e.Addsd(i % 6, i % 6); return n; });
            case "vec128": return Build("port_vec128_x" + n, 2 * n, 1, Init.Vec, delegate (Emit e)
                { for (int i = 0; i < n; i++) e.Paddq(i % 6, i % 6); return n; });
            case "vec256": return Build("port_vec256_x" + n, 4 * n, 1, Init.Vec, delegate (Emit e)
                { for (int i = 0; i < n; i++) e.VpaddqY(i % 6, i % 6, i % 6); return n; });
            case "load": return Build("port_load_x" + n, n, 1, Init.Frame, delegate (Emit e)
                { for (int i = 0; i < n; i++) e.LoadFromRsp(Acc[i % 6], 8 + (i % 6) * 8); return n; });
            case "store": return Build("port_store_x" + n, n, 1, Init.Frame, delegate (Emit e)
                { for (int i = 0; i < n; i++) e.StoreToRsp(Acc[i % 6], 8 + (i % 6) * 8); return n; });
            default: throw new ArgumentException("unknown port kind " + kind);
        }
    }

    // The execution check of the emitter itself: one add per iteration from zero, so the return value is
    // the iteration count and a stub that did not run cannot pass.
    private static Kernel Verify()
    {
        code.Align(64);
        int entry = code.Mark();
        code.MovImm64(RAX, 0);
        code.Align(32);
        int top = code.Mark();
        code.AddImm8(RAX, 1);
        code.SubRcx1();
        code.JnzBack(top);
        code.Ret();
        Kernel k = new Kernel();
        k.Name = "verify"; k.Instr = 3; k.Bytes = 10; k.Work = 1; k.Latency = 1;
        k.Fn = (LoopFn)Marshal.GetDelegateForFunctionPointer(code.At(entry), typeof(LoopFn));
        Keep.Add(k.Fn);
        return k;
    }

    // ---- measurement ----------------------------------------------------------------------------

    private sealed class Run
    {
        public long Iters; public double Us; public double TscTicks; public long Value;
        public double NsPerIter { get { return Us * 1000.0 / Iters; } }
    }

    private static Run Once(Kernel k, long loops)
    {
        Run r = new Run();
        ulong a = tsc();
        long q0 = Stopwatch.GetTimestamp();
        long v = k.Fn(loops);
        long q1 = Stopwatch.GetTimestamp();
        ulong b = tsc();
        Sink += (ulong)v;
        r.Iters = loops; r.Us = (q1 - q0) / TicksPerUs; r.TscTicks = b - a; r.Value = v;
        return r;
    }

    // Grows the iteration count until the window is long enough, then takes the best of three: the fastest
    // window is the one least disturbed by an interrupt, and the law is about the undisturbed core.
    private static Run Best(Kernel k, int ms)
    {
        long loops = 1000;
        Run r = Once(k, loops);
        while (r.Us < ms * 1000.0 && loops < (1L << 40))
        {
            double grow = r.Us > 1.0 ? ms * 1000.0 * 1.15 / r.Us : 64.0;
            if (grow < 2.0) grow = 2.0;
            if (grow > 1000.0) grow = 1000.0;
            loops = (long)(loops * grow) + 1;
            r = Once(k, loops);
        }
        Run best = r;
        for (int i = 0; i < 2; i++)
        {
            Run t = Once(k, loops);
            if (t.NsPerIter < best.NsPerIter) best = t;
        }
        return best;
    }

    private static void Warm(Kernel k) { Once(k, 20000); }

    // The clock, from the two latency witnesses. 16 dependent multiplies are 48 cycles, which no plausible
    // instruction rate cap can reach, and the FP chain is the second opinion.
    private static double ClockMHz(int ms, out double fpMHz, out double tscMHz)
    {
        Kernel ki = DepImul(16), kf = DepFp(8);
        code.Done();
        Warm(ki); Warm(kf);
        Run ri = Best(ki, ms), rf = Best(kf, ms);
        double imulMHz = 48.0 / ri.NsPerIter * 1000.0;
        fpMHz = 24.0 / rf.NsPerIter * 1000.0;
        tscMHz = ri.TscTicks / ri.Us;
        Line("clock imul16_ns=" + F(ri.NsPerIter, 4) + " imul16_mhz=" + F(imulMHz, 1) +
             " fp8_ns=" + F(rf.NsPerIter, 4) + " fp8_mhz_at_3cyc=" + F(fpMHz, 1) +
             " fp_over_imul=" + F(fpMHz / imulMHz, 4) + " tsc_mhz=" + F(tscMHz, 2) +
             " qpc_hz=" + Stopwatch.Frequency.ToString(CultureInfo.InvariantCulture));
        return imulMHz;
    }

    private static double Report(string tag, Kernel k, int ms, double mhz)
    {
        Warm(k);
        Run r = Best(k, ms);
        double cyc = r.NsPerIter * mhz / 1000.0;
        Line(tag + " name=" + k.Name + " instr=" + k.Instr + " bytes=" + k.Bytes +
             " iters=" + r.Iters.ToString(CultureInfo.InvariantCulture) + " us=" + F(r.Us, 1) +
             " ns_per_iter=" + F(r.NsPerIter, 4) +
             " cyc_per_iter=" + F(cyc, 3) + " cyc_per_instr=" + F(cyc / k.Instr, 4) +
             " cyc_per_byte=" + F(cyc / k.Bytes, 4) +
             " latency_cyc=" + F(k.Latency, 1) +
             " work_per_us=" + F(k.Work * r.Iters / r.Us, 2) +
             " tsc_per_iter=" + F(r.TscTicks / r.Iters, 4));
        return cyc;
    }

    // Least squares slope of cycles per iteration against instruction count: k, with no estimate in it.
    private static void Slope(string tag, List<int> counts, List<double> cycles)
    {
        int n = counts.Count;
        double sx = 0, sy = 0, sxx = 0, sxy = 0;
        for (int i = 0; i < n; i++) { sx += counts[i]; sy += cycles[i]; sxx += (double)counts[i] * counts[i]; sxy += counts[i] * cycles[i]; }
        double slope = (n * sxy - sx * sy) / (n * sxx - sx * sx);
        double icept = (sy - slope * sx) / n;
        double ssTot = 0, ssRes = 0, mean = sy / n;
        for (int i = 0; i < n; i++)
        { double fit = icept + slope * counts[i]; ssRes += (cycles[i] - fit) * (cycles[i] - fit); ssTot += (cycles[i] - mean) * (cycles[i] - mean); }
        Line(tag + " points=" + n + " slope_cyc_per_instr=" + F(slope, 4) + " intercept_cyc=" + F(icept, 3) +
             " r2=" + F(ssTot > 0 ? 1.0 - ssRes / ssTot : 1.0, 5) +
             " instr_per_cyc=" + F(slope > 0 ? 1.0 / slope : 0.0, 3));
    }

    // ---- modes ----------------------------------------------------------------------------------

    private static readonly int[] SweepN = { 6, 12, 18, 24, 36, 48 };
    private static readonly int[] KneeM = { 0, 4, 8, 12, 16, 20, 24, 32, 40 };
    private static readonly string[] Ports = { "alu", "mul", "fp", "vec128", "vec256", "load", "store" };

    private static bool Avx2Usable(out string why)
    {
        uint[] r = Leaf(1, 0);
        bool osxsave = (r[2] & (1u << 27)) != 0, avx = (r[2] & (1u << 28)) != 0;
        uint[] r7 = Leaf(7, 0);
        bool avx2 = (r7[1] & (1u << 5)) != 0;
        bool ymm = false;
        if (osxsave) { ulong mask = xgetbv(0); ymm = (mask & 0x6UL) == 0x6UL; }
        why = "osxsave=" + (osxsave ? 1 : 0) + " avx=" + (avx ? 1 : 0) + " avx2=" + (avx2 ? 1 : 0) +
              " xcr0_ymm=" + (ymm ? 1 : 0);
        return avx && avx2 && ymm;
    }

    private static uint[] Leaf(uint leaf, uint sub)
    {
        uint[] outv = new uint[4];
        IntPtr p = Marshal.AllocHGlobal(16);
        try { cpuid(leaf, sub, p); for (int i = 0; i < 4; i++) outv[i] = (uint)Marshal.ReadInt32(p, i * 4); }
        finally { Marshal.FreeHGlobal(p); }
        return outv;
    }

    private static int ModeLaw(int cpu, int ms, bool full)
    {
        Pin(cpu);
        Line("law_begin cpu=" + cpu + " on=" + GetCurrentProcessorNumber() + " ms=" + ms +
             " full=" + (full ? 1 : 0) + " processors=" + Environment.ProcessorCount);
        string why;
        bool avx2 = Avx2Usable(out why);
        Line("law_isa " + why + " vec256=" + (avx2 ? "run" : "skipped"));
        double fp, tscMHz;
        double mhz = ClockMHz(ms, out fp, out tscMHz);

        // (a) the slot slope, and (b) the same instruction count in seven-byte encodings
        List<int> ci = new List<int>(); List<double> cy = new List<double>();
        List<int> ci32 = new List<int>(); List<double> cy32 = new List<double>();
        foreach (int n in SweepN)
        {
            Kernel k = IndepAdd(n, false); code.Done();
            ci.Add(k.Instr); cy.Add(Report("sweep8", k, ms, mhz));
            Kernel k2 = IndepAdd(n, true); code.Done();
            ci32.Add(k2.Instr); cy32.Add(Report("sweep32", k2, ms, mhz));
        }
        Slope("slope_imm8", ci, cy);
        Slope("slope_imm32", ci32, cy32);

        // the knee: the free fillers of state A against the priced fillers of state B
        foreach (int m in KneeM)
        {
            Kernel k = Knee(m); code.Done();
            Report("knee", k, ms, mhz);
        }

        // the port map
        foreach (string p in Ports)
        {
            if (p == "vec256" && !avx2) { Line("port name=port_vec256 SKIPPED " + why); continue; }
            Kernel k = Port(p, 24); code.Done();
            Report("port", k, ms, mhz);
        }

        // the dependency structure proof: the same instruction stream, two dependency structures
        Kernel d4 = DepImul(4), i4 = IndepImul(4); code.Done();
        Report("pair", d4, ms, mhz);
        Report("pair", i4, ms, mhz);

        if (full)
        {
            foreach (int size in new int[] { 1, 8, 15 })
                foreach (int n in new int[] { 8, 16, 32 })
                { Kernel k = IndepNop(n, size); code.Done(); Report("nop", k, ms, mhz); }
            foreach (int n in new int[] { 8, 16, 32, 48 })
            { Kernel k = DepImul(n); code.Done(); Report("deplen", k, ms, mhz); }
        }

        double fp2, tsc2;
        double mhz2 = ClockMHz(ms, out fp2, out tsc2);
        Line("law_end cpu=" + cpu + " clock_start_mhz=" + F(mhz, 1) + " clock_end_mhz=" + F(mhz2, 1) +
             " drift_pct=" + F(100.0 * (mhz2 - mhz) / mhz, 3));
        Unpin();
        return 0;
    }

    // Pairwise interference: the measured topology. One throughput kernel alone on cpu 0, then on cpu 0
    // together with every other logical CPU. A sibling pair shares one core's slots and sums to about one
    // single-thread rate; two cores sum to two. State A names the siblings; state B then says whether the
    // budget is per core (N6).
    private static int ModePairs(int ms)
    {
        int n = Environment.ProcessorCount;
        Line("pairs_begin processors=" + n + " ms=" + ms);
        Kernel k = Port("alu", 24); code.Done();
        Pin(0); Warm(k); Run solo = Best(k, ms); Unpin();
        double soloRate = 24.0 * solo.Iters / solo.Us;
        Line("pairs solo cpu=0 work_per_us=" + F(soloRate, 2) + " ns_per_iter=" + F(solo.NsPerIter, 4));
        for (int j = 1; j < n; j++)
        {
            double[] rate = new double[2];
            int other = j;
            Thread a = new Thread(delegate () { Pin(0); Warm(k); Run r = Best(k, ms); rate[0] = 24.0 * r.Iters / r.Us; Unpin(); });
            Thread b = new Thread(delegate () { Pin(other); Warm(k); Run r = Best(k, ms); rate[1] = 24.0 * r.Iters / r.Us; Unpin(); });
            a.IsBackground = b.IsBackground = true;
            a.Start(); b.Start(); a.Join(); b.Join();
            double sum = rate[0] + rate[1];
            Line("pairs cpu_a=0 cpu_b=" + j + " a_work_per_us=" + F(rate[0], 2) + " b_work_per_us=" + F(rate[1], 2) +
                 " sum=" + F(sum, 2) + " sum_over_solo=" + F(sum / soloRate, 3) +
                 " verdict=" + (sum / soloRate < 1.5 ? "shares_a_core"
                                : (sum / soloRate > 1.75 ? "separate_cores" : "unclear")));
        }
        return 0;
    }

    // The A/B diff of this dump is read as a hint for M2, so every field in it must be a property of the
    // part and not of the thread's placement. Three fields are per logical CPU by architecture: the initial
    // APIC ID in CPUID.1:EBX[31:24], the x2APIC ID in CPUID.0xB:EDX, and the extended APIC ID in
    // CPUID.8000001E:EAX. Unpinned they change between two runs on the same machine (measured on the
    // development PC: EBX 0x08800800, 0x19800800 and 0x20800800 in three consecutive runs), which is exactly
    // the signature the plan reads as M2. So the mode pins itself, prints the CPU it ran on, and prints those
    // three fields again on a line of their own, with the core-invariant form of EBX beside them.
    private static int ModeCpuid(int cpu)
    {
        Pin(cpu);
        Line("cpuid_begin pinned_cpu=" + cpu + " on=" + GetCurrentProcessorNumber() +
             " processors=" + Environment.ProcessorCount);
        uint[] leaves = { 0, 1, 7, 0xB, 0xD, 0x80000000, 0x80000001, 0x80000002, 0x80000003, 0x80000004,
                          0x80000007, 0x80000008, 0x8000001E, 0x8000001F };
        foreach (uint l in leaves)
        {
            uint[] r = Leaf(l, 0);
            Line("cpuid leaf=" + H(l) + " sub=0 eax=" + H(r[0]) + " ebx=" + H(r[1]) + " ecx=" + H(r[2]) + " edx=" + H(r[3]));
        }
        uint[] one = Leaf(1, 0);
        uint[] topo = Leaf(0xB, 0);
        uint[] ext = Leaf(0x8000001E, 0);
        Line("cpuid percpu pinned_cpu=" + cpu + " apic_id_leaf1_ebx_31_24=" + H(one[1] >> 24) +
             " x2apic_id_leaf0xB_edx=" + H(topo[3]) + " ext_apic_id_leaf8000001E_eax=" + H(ext[0]) +
             " (these three follow the logical CPU, not the state: compare them only at the same pinned_cpu)");
        Line("cpuid core_invariant leaf1_ebx_masked=" + H(one[1] & 0x00FFFFFFu) +
             " (leaf 1 EBX without the initial APIC ID byte: this one is comparable between A and B)");
        foreach (uint sub in new uint[] { 0, 1, 2, 3 })
        {
            uint[] r = Leaf(0x8000001D, sub);
            Line("cpuid leaf=0x8000001D sub=" + sub + " eax=" + H(r[0]) + " ebx=" + H(r[1]) + " ecx=" + H(r[2]) + " edx=" + H(r[3]));
        }
        foreach (uint sub in new uint[] { 0, 1 })
        {
            uint[] r = Leaf(7, sub);
            Line("cpuid leaf=0x00000007 sub=" + sub + " eax=" + H(r[0]) + " ebx=" + H(r[1]) + " ecx=" + H(r[2]) + " edx=" + H(r[3]));
        }
        string brand = "";
        foreach (uint l in new uint[] { 0x80000002, 0x80000003, 0x80000004 })
        {
            uint[] r = Leaf(l, 0);
            foreach (uint w in r)
                for (int s = 0; s < 4; s++) { char c = (char)((w >> (8 * s)) & 0xFF); if (c != '\0') brand += c; }
        }
        string whyAvx; bool avx2 = Avx2Usable(out whyAvx);
        Line("cpuid brand=\"" + brand.Trim() + "\" " + whyAvx + " avx2_usable=" + (avx2 ? 1 : 0));
        ulong xcr0 = 0; try { xcr0 = xgetbv(0); } catch (Exception e) { Console.Error.WriteLine("xgetbv: " + e.Message); }
        Line("cpuid xcr0=0x" + xcr0.ToString("X16", CultureInfo.InvariantCulture));
        Unpin();
        return 0;
    }

    // The one memory residual of the 2026-10-12 arm: the 4 MiB pointer chase rose 7.2 % in nanoseconds and
    // the rate law does not explain it (N8b). A size sweep across the L3 boundary settles it.
    private static int ModeCache(int cpu, int ms)
    {
        Pin(cpu);
        // 16 KiB is the first point and not 32 KiB, because 16 KiB is the size the 2026-10-12 arm measured at
        // 2.086 ns (bd093probe.cs:340 names that row `l1_16k`). The chase array is exactly `kib` KiB, so a
        // 32 KiB point fills a 32 KiB L1D on its own and must read higher: it is not a comparable control.
        int[] kib = { 16, 32, 512, 2048, 3072, 4096, 6144, 8192, 65536 };
        foreach (int size in kib)
        {
            int nodes = size * 1024 / 64;
            int[] next = new int[nodes * 16];
            int[] order = new int[nodes];
            for (int i = 0; i < nodes; i++) order[i] = i;
            Random rnd = new Random(12345);
            for (int i = nodes - 1; i > 0; i--) { int j = rnd.Next(i + 1); int t = order[i]; order[i] = order[j]; order[j] = t; }
            for (int i = 0; i < nodes; i++) next[order[i] * 16] = order[(i + 1) % nodes];
            int p = 0;
            for (int i = 0; i < nodes; i++) p = next[p * 16];
            long steps = 0; double us = 0.0;
            while (us < ms * 1000.0)
            {
                long a = Stopwatch.GetTimestamp();
                for (int i = 0; i < 100000; i++) p = next[p * 16];
                long b = Stopwatch.GetTimestamp();
                us += (b - a) / TicksPerUs; steps += 100000;
            }
            Sink += (ulong)p;
            Line("cache cpu=" + cpu + " kib=" + size + " steps=" + steps.ToString(CultureInfo.InvariantCulture) +
                 " us=" + F(us, 1) + " ns_per_access=" + F(us * 1000.0 / steps, 3));
        }
        Unpin();
        return 0;
    }

    // The instrument's own positive control. It fails loudly instead of printing a null: the 2026-10-12 arm
    // lost a whole capture to a counter that did not count.
    private static int ModeSelftest(int cpu, int ms)
    {
        int bad = 0;
        Pin(cpu);
        Kernel v = Verify(); code.Done();
        Run r = Once(v, 1000000);
        bool exec = r.Value == 1000000;
        Line("selftest emit_executes=" + (exec ? "yes" : "NO value=" + r.Value) + " iters=1000000");
        if (!exec) bad++;
        double fp, tscMHz;
        double mhz = ClockMHz(ms, out fp, out tscMHz);
        bool plausible = mhz > 300.0 && mhz < 6000.0;
        Line("selftest clock_plausible=" + (plausible ? "yes" : "NO") + " imul_mhz=" + F(mhz, 1));
        if (!plausible) bad++;
        // The ratio is the assumed addsd latency over the real one: 1.00 means three cycles (Zen 2, and the
        // 24.00 cycles the 2026-10-12 arm measured), 1.50 two cycles, 0.75 four cycles. Any of them passes
        // here; the lab leg asserts 1.00, which is how a wrong clock witness would be caught.
        Line("selftest fp_over_imul=" + F(fp / mhz, 4) +
             " ratio_means_addsd_latency=1.00->3cyc(zen2,lab) 1.50->2cyc 0.75->4cyc");
        bool tscOk = tscMHz > 300.0 && tscMHz < 6000.0;
        Line("selftest tsc_plausible=" + (tscOk ? "yes" : "NO") + " tsc_mhz=" + F(tscMHz, 2));
        if (!tscOk) bad++;
        Kernel alu = Port("alu", 24); code.Done();
        Warm(alu); Run ra = Best(alu, ms);
        double k = ra.NsPerIter * mhz / 1000.0 / alu.Instr;
        Line("selftest alu_cyc_per_instr=" + F(k, 4) + " healthy_band=0.15..0.45 verdict=" +
             (k < 0.45 ? "healthy-like" : (k > 0.8 ? "capped-like" : "between")));
        Kernel d4 = DepImul(4), i4 = IndepImul(4); code.Done();
        Warm(d4); Warm(i4);
        double cd = Best(d4, ms).NsPerIter * mhz / 1000.0, cz = Best(i4, ms).NsPerIter * mhz / 1000.0;
        Line("selftest dep4_cyc=" + F(cd, 3) + " ind4_cyc=" + F(cz, 3) + " dep_over_ind=" + F(cd / cz, 3) +
             " healthy_expect=about_3");
        bool depOk = cd > 11.0 && cd < 13.5;
        Line("selftest dep4_is_12_cycles=" + (depOk ? "yes" : "NO"));
        if (!depOk) bad++;
        string why; bool avx2 = Avx2Usable(out why);
        Line("selftest avx2=" + (avx2 ? 1 : 0) + " " + why);
        Unpin();
        Line("selftest failures=" + bad);
        return bad == 0 ? 0 : 1;
    }

    private static int Usage()
    {
        Console.Error.WriteLine(
            "usage: bd093law <mode> [args]\n" +
            "  selftest [cpu] [ms]      the emitter, the two clock witnesses and the healthy bands\n" +
            "  law <cpu> <ms>           the full rate law suite (slope, bytes, knee, ports, nops, chains)\n" +
            "  quick <cpu> <ms>         the short suite: clock, slope, knee, ports, the dependency pair\n" +
            "  pairs <ms>               measured sibling map: cpu 0 against every other logical CPU\n" +
            "  cpuid [cpu]              CPUID leaves and XCR0, pinned, for the state A against state B diff\n" +
            "  cache <cpu> <ms>         pointer chase 16 KiB to 64 MiB, the L3 boundary included\n");
        return 2;
    }

    public static int Main(string[] argv)
    {
        try { Process.GetCurrentProcess().PriorityClass = ProcessPriorityClass.High; }
        catch (Exception e) { Console.Error.WriteLine("priority: " + e.Message); }
        if (argv.Length < 1) return Usage();
        try
        {
            code = new Emit(1 << 20);
            BuildHelpers();
            code.Done();
        }
        catch (Exception e)
        {
            Console.Error.WriteLine("EMIT UNAVAILABLE " + e.GetType().Name + " " + e.Message);
            Line("emit unavailable=1 reason=" + e.GetType().Name);
            return 3;
        }
        try
        {
            int cpu, ms;
            switch (argv[0])
            {
                case "selftest":
                    cpu = argv.Length > 1 ? int.Parse(argv[1], CultureInfo.InvariantCulture) : 0;
                    ms = argv.Length > 2 ? int.Parse(argv[2], CultureInfo.InvariantCulture) : 40;
                    return ModeSelftest(cpu, ms);
                case "law":
                case "quick":
                    if (argv.Length < 3) return Usage();
                    cpu = int.Parse(argv[1], CultureInfo.InvariantCulture);
                    ms = int.Parse(argv[2], CultureInfo.InvariantCulture);
                    return ModeLaw(cpu, ms, argv[0] == "law");
                case "pairs":
                    if (argv.Length < 2) return Usage();
                    return ModePairs(int.Parse(argv[1], CultureInfo.InvariantCulture));
                case "cpuid":
                    cpu = argv.Length > 1 ? int.Parse(argv[1], CultureInfo.InvariantCulture) : 0;
                    return ModeCpuid(cpu);
                case "cache":
                    if (argv.Length < 3) return Usage();
                    return ModeCache(int.Parse(argv[1], CultureInfo.InvariantCulture),
                                     int.Parse(argv[2], CultureInfo.InvariantCulture));
                default:
                    return Usage();
            }
        }
        // A bad numeric argument must print the usage text, not a .NET stack trace: the lab reads stdout.
        catch (FormatException) { return Usage(); }
        catch (OverflowException) { return Usage(); }
        catch (ArgumentException e) { Console.Error.WriteLine("argument: " + e.Message); return Usage(); }
        catch (OutOfMemoryException)
        {
            Console.Error.WriteLine("out of memory: the size or iteration argument is too large");
            return Usage();
        }
    }
}
