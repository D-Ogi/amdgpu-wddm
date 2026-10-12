// BD-093 CPU work meter. Read-only: it only runs instructions and reads clocks.
//
// Why it exists: K223 measured one .NET ALU chain 2.23x slower in the K137 state while the APERF/MPERF
// counter said only 1.25x. That gap is the open question. This probe turns the chain into a set of
// kernels with different (and, in the healthy state, self-calibrating) cycle costs, adds a chunk
// histogram that separates "the clock is lower" from "time is being stolen", sweeps every logical CPU,
// and measures memory latency in ns next to it.
//
// Every mode prints one line per measurement, flushed, prefixed with the mode name, so one invocation is
// one step of the lab plan.
//
// Build:  csc.exe /nologo /optimize+ /platform:x64 /out:bd093probe.exe bd093probe.cs
using System;
using System.Collections.Generic;
using System.Diagnostics;
using System.Globalization;
using System.Runtime.InteropServices;
using System.Threading;

internal static class Probe
{
    [DllImport("kernel32.dll")] private static extern IntPtr GetCurrentThread();
    [DllImport("kernel32.dll")] private static extern UIntPtr SetThreadAffinityMask(IntPtr h, UIntPtr mask);
    [DllImport("kernel32.dll")] private static extern bool SetThreadPriority(IntPtr h, int prio);
    [DllImport("kernel32.dll")] private static extern uint GetCurrentProcessorNumber();

    private const int THREAD_PRIORITY_HIGHEST = 2;

    // Keeps every kernel result alive so the JIT cannot delete the work.
    public static ulong Sink;
    public static double SinkD;

    private static readonly double TicksPerUs = Stopwatch.Frequency / 1000000.0;

    private static void Pin(int cpu)
    {
        Thread.BeginThreadAffinity();
        UIntPtr prev = SetThreadAffinityMask(GetCurrentThread(), (UIntPtr)(1UL << cpu));
        if (prev == UIntPtr.Zero)
            Console.Error.WriteLine("affinity: SetThreadAffinityMask failed for cpu " + cpu.ToString(CultureInfo.InvariantCulture));
        SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_HIGHEST);
    }

    private static void Unpin() { Thread.EndThreadAffinity(); }

    // ---- kernels -------------------------------------------------------------------------------
    // Each kernel returns the number of OPERATIONS retired, and the caller divides by microseconds.
    // ops_per_us / core_MHz = ops per cycle, which is a property of the instruction mix only. The
    // healthy state fixes that constant for every kernel; the K137 state then reads the clock from it.

    // loop_add: the constants fold, so this one measures the loop and the front end, not an add chain.
    private static long DepAdd(long loops)
    {
        ulong x = Sink | 1UL;
        for (long i = 0; i < loops; i++) { x += 3; x += 5; x += 7; x += 11; x += 13; x += 17; x += 19; x += 23; }
        Sink = x; return loops * 8;
    }

    // mix_xoradd: partly folded as well (0.55 cycles per counted op on the reference machine).
    private static long DepXorAdd(long loops)
    {
        ulong x = Sink | 1UL;
        for (long i = 0; i < loops; i++)
        {
            x ^= 0x5555555555555555UL; x += 3;
            x ^= 0x3333333333333333UL; x += 5;
            x ^= 0x0F0F0F0F0F0F0F0FUL; x += 7;
            x ^= 0x00FF00FF00FF00FFUL; x += 11;
        }
        Sink = x; return loops * 8;
    }

    private static long DepImul(long loops)             // 4 dependent multiplies, 3 cycles each
    {
        ulong x = Sink | 1UL;
        for (long i = 0; i < loops; i++)
        {
            x *= 6364136223846793005UL; x *= 2862933555777941757UL;
            x *= 3202034522624059733UL; x *= 1181783497276652981UL;
        }
        Sink = x; return loops * 4;
    }

    private static long DepMix(long loops)              // the K223 kernel: imul + add + shr + xor
    {
        ulong x = Sink | 0x9E3779B97F4A7C15UL;
        for (long i = 0; i < loops; i++) { x = x * 6364136223846793005UL + 1442695040888963407UL; x ^= x >> 29; }
        Sink = x; return loops;
    }

    private static long TputAdd(long loops)             // 8 independent adds: ALU throughput bound
    {
        ulong a = Sink | 1UL, b = a + 1, c = a + 2, d = a + 3, e = a + 4, f = a + 5, g = a + 6, h = a + 7;
        for (long i = 0; i < loops; i++)
        { a += 3; b += 5; c += 7; d += 11; e += 13; f += 17; g += 19; h += 23; }
        Sink = a ^ b ^ c ^ d ^ e ^ f ^ g ^ h; return loops * 8;
    }

    private static long TputImul(long loops)            // 4 independent multiplies: multiplier throughput
    {
        ulong a = Sink | 1UL, b = a + 1, c = a + 2, d = a + 3;
        for (long i = 0; i < loops; i++)
        {
            a *= 6364136223846793005UL; b *= 2862933555777941757UL;
            c *= 3202034522624059733UL; d *= 1181783497276652981UL;
        }
        Sink = a ^ b ^ c ^ d; return loops * 4;
    }

    private static long DepFpAdd(long loops)            // 8 dependent double adds, 3 cycles each
    {
        double x = SinkD + 1.0;
        for (long i = 0; i < loops; i++)
        { x += 1.5; x += 2.5; x += 3.5; x += 4.5; x += 5.5; x += 6.5; x += 7.5; x += 8.5; }
        SinkD = x; return loops * 8;
    }

    private delegate long Kernel(long loops);

    private static readonly string[] KernelNames =
        { "loop_add", "mix_xoradd", "dep_imul", "dep_mix", "tput_add", "tput_imul", "dep_fpadd" };

    private static Kernel KernelByName(string name)
    {
        switch (name)
        {
            case "loop_add": return DepAdd;
            case "mix_xoradd": return DepXorAdd;
            case "dep_imul": return DepImul;
            case "dep_mix": return DepMix;
            case "tput_add": return TputAdd;
            case "tput_imul": return TputImul;
            case "dep_fpadd": return DepFpAdd;
            default: return null;
        }
    }

    // Runs a kernel for about ms milliseconds and returns operations per microsecond.
    private static double Rate(Kernel k, int ms, out long ops, out double us)
    {
        long loops = 1000;
        for (;;)                                        // grow the loop count until the run is long enough
        {
            long t0 = Stopwatch.GetTimestamp();
            long n = k(loops);
            long t1 = Stopwatch.GetTimestamp();
            double elapsed = (t1 - t0) / TicksPerUs;
            if (elapsed >= ms * 1000.0 || loops > 1L << 40) { ops = n; us = elapsed; return n / elapsed; }
            double want = ms * 1000.0 * 1.15;
            double grow = elapsed > 1.0 ? want / elapsed : 64.0;
            if (grow < 2.0) grow = 2.0;
            if (grow > 1000.0) grow = 1000.0;
            loops = (long)(loops * grow) + 1;
        }
    }

    private static void Warm(Kernel k)
    {
        long ops; double us;
        Rate(k, 1, out ops, out us);
    }

    private static string F(double v, int digits)
    {
        return v.ToString("F" + digits.ToString(CultureInfo.InvariantCulture), CultureInfo.InvariantCulture);
    }

    private static void Line(string s) { Console.WriteLine(s); Console.Out.Flush(); }

    // ---- modes ---------------------------------------------------------------------------------

    private static int ModeKernels(int cpu, int ms, int reps)
    {
        Pin(cpu);
        foreach (string name in KernelNames)            // warm-up pass: the JIT compiles each kernel once
            Warm(KernelByName(name));
        for (int r = 0; r < reps; r++)
            foreach (string name in KernelNames)
            {
                long ops; double us;
                double rate = Rate(KernelByName(name), ms, out ops, out us);
                Line("kernel cpu=" + cpu + " on=" + GetCurrentProcessorNumber() + " rep=" + r + " name=" + name +
                     " ops=" + ops + " us=" + F(us, 1) + " ops_per_us=" + F(rate, 2));
            }
        Unpin();
        return 0;
    }

    // Chunked histogram. The median chunk is the speed of the fast path; the mean over all chunks is the
    // speed the work actually gets. Those two are equal when a clock is simply lower, and they diverge
    // when something steals time (a halt, an SMI, a competing thread).
    private static int ModeHist(int cpu, int chunkOps, int ms, bool contend, bool steal)
    {
        Thread noise = null;
        volatileStop = 0; noiseOps = 0;
        if (contend)
        {
            noise = new Thread(delegate ()
            {
                Pin(cpu);                               // the same logical CPU on purpose: a known 50 % thief
                long nOps = 0;
                while (Thread.VolatileRead(ref volatileStop) == 0) nOps += DepMix(20000);
                Interlocked.Add(ref noiseOps, nOps);
                Unpin();
            });
            noise.IsBackground = true;
            noise.Start();
        }
        Pin(cpu);
        long warmOps; double warmUs;
        double warmRate = Rate(DepMix, 1, out warmOps, out warmUs);   // warm-up, and the size estimate
        long chunkLoops = chunkOps;
        // The fine arm produces millions of chunks, and a List growing inside the timing loop would copy
        // tens of megabytes mid-measurement and show up as slow chunks. Size it up front from the
        // warm-up rate and stop recording at the cap; the mean keeps counting either way.
        const int RateCap = 2000000;
        double expectUs = chunkOps / (warmRate > 0.0 ? warmRate : 500.0);
        long expect = (long)(ms * 1000.0 / (expectUs > 0.0 ? expectUs : 1.0) * 1.2) + 1024;
        int capacity = (int)(expect > RateCap ? RateCap : expect);
        List<double> rates = new List<double>(capacity);
        long dropped = 0;
        long t0 = Stopwatch.GetTimestamp();
        double budget = ms * 1000.0;
        double total = 0.0; long totalOps = 0;
        while (total < budget)
        {
            long a = Stopwatch.GetTimestamp();
            long n = DepMix(chunkLoops);
            long b = Stopwatch.GetTimestamp();
            double us = (b - a) / TicksPerUs;
            if (us > 0.0) { if (rates.Count < RateCap) rates.Add(n / us); else dropped++; }
            if (steal)                                  // positive control: a gap of the chunk's own length,
            {                                           // so mean / median must come out at about 0.5
                long want = b - a;
                while (Stopwatch.GetTimestamp() - b < want) { }
            }
            total = (Stopwatch.GetTimestamp() - t0) / TicksPerUs; totalOps += n;
        }
        Unpin();
        Thread.VolatileWrite(ref volatileStop, 1);
        if (noise != null) noise.Join(2000);
        if (rates.Count == 0) { Line("hist cpu=" + cpu + " chunk_ops=" + chunkOps + " NO CHUNK MEASURED (the timer resolution is coarser than one chunk)"); return 0; }
        rates.Sort();
        double median = rates[rates.Count / 2];
        double p01 = rates[(int)(rates.Count * 0.01)];
        double p99 = rates[(int)(rates.Count * 0.99)];
        double mean = totalOps / total;
        int slow = 0;
        for (int i = 0; i < rates.Count; i++) if (rates[i] < median * 0.67) slow++;
        Line("hist cpu=" + cpu + " contend=" + (contend ? 1 : 0) + " steal=" + (steal ? 1 : 0) + " chunks=" + rates.Count +
             " dropped=" + dropped + " qpc_ticks_per_chunk=" + F(rates.Count > 0 ? (chunkOps / median) * TicksPerUs : 0.0, 1) +
             " chunk_ops=" + chunkOps + " total_us=" + F(total, 0) +
             " ops_per_us_mean=" + F(mean, 2) + " median=" + F(median, 2) +
             " p01=" + F(p01, 2) + " p99=" + F(p99, 2) + " max=" + F(rates[rates.Count - 1], 2) +
             " slow_chunk_pct=" + F(100.0 * slow / rates.Count, 2) +
             " mean_over_median=" + F(mean / median, 3) +
             " noise_ops=" + Thread.VolatileRead(ref noiseOps));
        return 0;
    }

    private static int volatileStop;
    private static long noiseOps;

    private static void Topology(string mode)
    {
        string mask = "unknown";
        try { mask = "0x" + ((long)Process.GetCurrentProcess().ProcessorAffinity).ToString("X", CultureInfo.InvariantCulture); }
        catch (Exception e) { mask = "unreadable(" + e.GetType().Name + ")"; }
        Line("topo mode=" + mode + " processors=" + Environment.ProcessorCount + " process_affinity=" + mask);
    }

    private static int ModeSweep(int ms)
    {
        int n = Environment.ProcessorCount;
        Topology("sweep");
        for (int cpu = 0; cpu < n; cpu++)
        {
            Pin(cpu);
            Warm(DepMix);
            long ops; double us;
            double rate = Rate(DepMix, ms, out ops, out us);
            uint on = GetCurrentProcessorNumber();
            Unpin();
            Line("sweep cpu=" + cpu + " on=" + on + " name=dep_mix ops=" + ops + " us=" + F(us, 1) +
                 " ops_per_us=" + F(rate, 2));
        }
        return 0;
    }

    private static int ModeThreads(int ms, string list)
    {
        string[] parts = list.Split(',');
        Topology("threads");
        foreach (string p in parts)
        {
            int count = int.Parse(p, CultureInfo.InvariantCulture);
            if (count < 1) continue;
            // The lab ships 6 of 8 cores enabled, so a count above the processor count used to wrap two
            // threads onto CPUs 0 and 1 (me % ProcessorCount) and the aggregate could not scale by
            // construction. Say so instead of printing a number nobody can read.
            if (count > Environment.ProcessorCount)
            {
                Line("threads n=" + count + " SKIPPED processors=" + Environment.ProcessorCount +
                     " reason=more_threads_than_logical_cpus");
                continue;
            }
            double[] rate = new double[count];
            Thread[] th = new Thread[count];
            for (int i = 0; i < count; i++)
            {
                int me = i;
                th[i] = new Thread(delegate ()
                {
                    Pin(me);                            // one thread per logical CPU, never two on one
                    Warm(DepMix);
                    long ops; double us;
                    rate[me] = Rate(DepMix, ms, out ops, out us);
                    Unpin();
                });
                th[i].IsBackground = true;
            }
            for (int i = 0; i < count; i++) th[i].Start();
            for (int i = 0; i < count; i++) th[i].Join();
            double sum = 0.0, min = double.MaxValue, max = 0.0;
            for (int i = 0; i < count; i++)
            { sum += rate[i]; if (rate[i] < min) min = rate[i]; if (rate[i] > max) max = rate[i]; }
            Line("threads n=" + count + " name=dep_mix sum_ops_per_us=" + F(sum, 2) +
                 " per_thread_mean=" + F(sum / count, 2) + " min=" + F(min, 2) + " max=" + F(max, 2));
        }
        return 0;
    }

    // Pointer chase over a random cycle: latency in nanoseconds, which is clock-dependent inside the core
    // and mostly clock-independent in DRAM. L1 up and DRAM flat means the core clock fell; DRAM up and L1
    // flat means the uncore or the memory path fell.
    private static int ModeLatency(int cpu, int ms)
    {
        Pin(cpu);
        int[] sizes = { 16 * 1024, 256 * 1024, 4 * 1024 * 1024, 64 * 1024 * 1024 };
        string[] names = { "l1_16k", "l2_256k", "l3_or_dram_4m", "dram_64m" };
        for (int s = 0; s < sizes.Length; s++)
        {
            int n = sizes[s] / 64;                      // one node per 64-byte line
            int[] next = new int[n * 16];               // 16 ints = 64 bytes, only slot 0 is used
            int[] order = new int[n];
            for (int i = 0; i < n; i++) order[i] = i;
            Random rnd = new Random(12345);
            for (int i = n - 1; i > 0; i--)             // Fisher-Yates, then link into one cycle
            { int j = rnd.Next(i + 1); int t = order[i]; order[i] = order[j]; order[j] = t; }
            for (int i = 0; i < n; i++) next[order[i] * 16] = order[(i + 1) % n];
            int p = 0;
            for (int i = 0; i < n; i++) p = next[p * 16];            // first touch
            long steps = 0; double us = 0.0;
            long budget = (long)(ms * 1000.0);
            while (us < budget)
            {
                long a = Stopwatch.GetTimestamp();
                for (int i = 0; i < 100000; i++) p = next[p * 16];
                long b = Stopwatch.GetTimestamp();
                us += (b - a) / TicksPerUs; steps += 100000;
            }
            Sink += (ulong)p;
            Line("latency cpu=" + cpu + " name=" + names[s] + " bytes=" + sizes[s] + " steps=" + steps +
                 " us=" + F(us, 1) + " ns_per_access=" + F(us * 1000.0 / steps, 3));
        }
        Unpin();
        return 0;
    }

    // Reports the clock sources this process can see, so the lab plan can compare them with an off-CPU
    // counter (GOLDEN_TSC through bc250kmd_cli read) over the same window.
    private static int ModeTimebase(int ms)
    {
        Line("timebase qpc_frequency=" + Stopwatch.Frequency + " is_high_resolution=" +
             (Stopwatch.IsHighResolution ? 1 : 0) + " processors=" + Environment.ProcessorCount);
        long t0 = Stopwatch.GetTimestamp();
        DateTime d0 = DateTime.UtcNow;
        Thread.Sleep(ms);
        long t1 = Stopwatch.GetTimestamp();
        DateTime d1 = DateTime.UtcNow;
        double qpcMs = (t1 - t0) / (Stopwatch.Frequency / 1000.0);
        double wallMs = (d1 - d0).TotalMilliseconds;
        Line("timebase slept_ms=" + ms + " qpc_ms=" + F(qpcMs, 3) + " systemtime_ms=" + F(wallMs, 3) +
             " qpc_over_systemtime=" + F(qpcMs / wallMs, 5) +
             " utc_start=" + d0.ToString("yyyy-MM-ddTHH:mm:ss.fffZ", CultureInfo.InvariantCulture) +
             " utc_end=" + d1.ToString("yyyy-MM-ddTHH:mm:ss.fffZ", CultureInfo.InvariantCulture));
        return 0;
    }

    private static int Usage()
    {
        Console.Error.WriteLine(
            "usage: bd093probe <mode> [args]\n" +
            "  kernels <cpu> <ms_per_kernel> <reps>   every kernel on one logical CPU\n" +
            "  hist <cpu> <chunk_ops> <ms> [contend|steal]  chunk histogram; steal injects a known 50 pct\n" +
            "                                         gap (the detector positive control); contend adds a\n" +
            "                                         second thread on the same logical CPU\n" +
            "  sweep <ms>                             dep_mix on every logical CPU\n" +
            "  threads <ms> <n,n,...>                 dep_mix with n threads, aggregate throughput\n" +
            "  latency <cpu> <ms_per_size>            pointer-chase latency, ns per access\n" +
            "  timebase <ms>                          QPC and system time over one window\n");
        return 2;
    }

    public static int Main(string[] argv)
    {
        try { Process.GetCurrentProcess().PriorityClass = ProcessPriorityClass.High; }
        catch (Exception e) { Console.Error.WriteLine("priority: " + e.Message); }
        if (argv.Length < 1) return Usage();
        try
        {
            switch (argv[0])
            {
                case "kernels":
                    if (argv.Length < 4) return Usage();
                    return ModeKernels(int.Parse(argv[1], CultureInfo.InvariantCulture),
                                       int.Parse(argv[2], CultureInfo.InvariantCulture),
                                       int.Parse(argv[3], CultureInfo.InvariantCulture));
                case "hist":
                    if (argv.Length < 4) return Usage();
                    return ModeHist(int.Parse(argv[1], CultureInfo.InvariantCulture),
                                    int.Parse(argv[2], CultureInfo.InvariantCulture),
                                    int.Parse(argv[3], CultureInfo.InvariantCulture),
                                    argv.Length > 4 && argv[4] == "contend",
                                    argv.Length > 4 && argv[4] == "steal");
                case "sweep":
                    if (argv.Length < 2) return Usage();
                    return ModeSweep(int.Parse(argv[1], CultureInfo.InvariantCulture));
                case "threads":
                    if (argv.Length < 3) return Usage();
                    return ModeThreads(int.Parse(argv[1], CultureInfo.InvariantCulture), argv[2]);
                case "latency":
                    if (argv.Length < 3) return Usage();
                    return ModeLatency(int.Parse(argv[1], CultureInfo.InvariantCulture),
                                       int.Parse(argv[2], CultureInfo.InvariantCulture));
                case "timebase":
                    if (argv.Length < 2) return Usage();
                    return ModeTimebase(int.Parse(argv[1], CultureInfo.InvariantCulture));
                default:
                    return Usage();
            }
        }
        catch (FormatException)
        {
            return Usage();
        }
    }
}
