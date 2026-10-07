// wasapi-probe: how fast does the default render endpoint really consume samples, and how are its
// notifications paced? Read-only towards the hardware: it opens one ordinary WASAPI stream on the default
// render endpoint (the DP monitor on unit A), plays a 1 kHz tone (or silence with -amp 0) and measures
//   - the device position from IAudioClock::GetPosition against QueryPerformanceCounter (Stopwatch),
//   - in event mode, the interval between the stream events (exclusive mode: one event per driver
//     notification, that is per DMA IOC interrupt of the HD Audio stream),
//   - how long Activate, Initialize and Start take (a bus driver that waits for codec responses which
//     never arrive shows up here as seconds, not milliseconds).
// Build (C# 5, .NET Framework 4, no extra references):
//   C:\Windows\Microsoft.NET\Framework64\v4.0.30319\csc.exe /nologo /optimize /platform:x64 /out:wasapi-probe.exe wasapi-probe.cs
// Usage:
//   wasapi-probe.exe <mode> [-secs N] [-rate R] [-amp A] [-periodms P]
//   mode: info | shared-event | shared-poll | excl-event | excl-poll
//   -rate is used in the exclusive modes only (16-bit stereo PCM); shared mode uses the mix format.
// Output: key=value lines, one "sample" line every 250 ms, a "result" line at the end.
using System;
using System.Collections.Generic;
using System.Diagnostics;
using System.Runtime.InteropServices;
using System.Threading;

namespace Bc250AudioProbe
{
    [ComImport, Guid("BCDE0395-E52F-467C-8E3D-C4579291692E")]
    class MMDeviceEnumeratorComObject { }

    [ComImport, Guid("A95664D2-9614-4F35-A746-DE8DB63617E6"), InterfaceType(ComInterfaceType.InterfaceIsIUnknown)]
    interface IMMDeviceEnumerator
    {
        [PreserveSig] int EnumAudioEndpoints(int dataFlow, uint stateMask, out IntPtr devices);
        [PreserveSig] int GetDefaultAudioEndpoint(int dataFlow, int role, out IMMDevice device);
        [PreserveSig] int GetDevice([MarshalAs(UnmanagedType.LPWStr)] string id, out IMMDevice device);
        [PreserveSig] int RegisterEndpointNotificationCallback(IntPtr client);
        [PreserveSig] int UnregisterEndpointNotificationCallback(IntPtr client);
    }

    [ComImport, Guid("D666063F-1587-4E43-81F1-B948E807363F"), InterfaceType(ComInterfaceType.InterfaceIsIUnknown)]
    interface IMMDevice
    {
        [PreserveSig] int Activate(ref Guid iid, uint clsCtx, IntPtr activationParams, [MarshalAs(UnmanagedType.IUnknown)] out object iface);
        [PreserveSig] int OpenPropertyStore(uint access, out IntPtr props);
        [PreserveSig] int GetId([MarshalAs(UnmanagedType.LPWStr)] out string id);
        [PreserveSig] int GetState(out uint state);
    }

    [ComImport, Guid("1CB9AD4C-DBFA-4c32-B178-C2F568A703B2"), InterfaceType(ComInterfaceType.InterfaceIsIUnknown)]
    interface IAudioClient
    {
        [PreserveSig] int Initialize(int shareMode, uint streamFlags, long hnsBufferDuration, long hnsPeriodicity, IntPtr format, IntPtr sessionGuid);
        [PreserveSig] int GetBufferSize(out uint frames);
        [PreserveSig] int GetStreamLatency(out long latency);
        [PreserveSig] int GetCurrentPadding(out uint padding);
        [PreserveSig] int IsFormatSupported(int shareMode, IntPtr format, out IntPtr closest);
        [PreserveSig] int GetMixFormat(out IntPtr format);
        [PreserveSig] int GetDevicePeriod(out long defaultPeriod, out long minimumPeriod);
        [PreserveSig] int Start();
        [PreserveSig] int Stop();
        [PreserveSig] int Reset();
        [PreserveSig] int SetEventHandle(IntPtr handle);
        [PreserveSig] int GetService(ref Guid iid, [MarshalAs(UnmanagedType.IUnknown)] out object service);
    }

    [ComImport, Guid("F294ACFC-3146-4483-A7BF-ADDCA7C260E2"), InterfaceType(ComInterfaceType.InterfaceIsIUnknown)]
    interface IAudioRenderClient
    {
        [PreserveSig] int GetBuffer(uint frames, out IntPtr data);
        [PreserveSig] int ReleaseBuffer(uint frames, uint flags);
    }

    [ComImport, Guid("CD63314F-3FBA-4a1b-812C-EF96358728E7"), InterfaceType(ComInterfaceType.InterfaceIsIUnknown)]
    interface IAudioClock
    {
        [PreserveSig] int GetFrequency(out ulong frequency);
        [PreserveSig] int GetPosition(out ulong position, out ulong qpcPosition);
        [PreserveSig] int GetCharacteristics(out uint characteristics);
    }

    class Format
    {
        public int Tag, Channels, Rate, BlockAlign, Bits;
        public bool IsFloat;
        public IntPtr Ptr;
        public override string ToString()
        {
            return string.Format("tag=0x{0:X4} ch={1} rate={2} bits={3} align={4} float={5}", Tag, Channels, Rate, Bits, BlockAlign, IsFloat);
        }
        public static Format Read(IntPtr p)
        {
            Format f = new Format();
            f.Ptr = p;
            f.Tag = (ushort)Marshal.ReadInt16(p, 0);
            f.Channels = (ushort)Marshal.ReadInt16(p, 2);
            f.Rate = Marshal.ReadInt32(p, 4);
            f.BlockAlign = (ushort)Marshal.ReadInt16(p, 12);
            f.Bits = (ushort)Marshal.ReadInt16(p, 14);
            if (f.Tag == 3) f.IsFloat = true;
            if (f.Tag == 0xFFFE) f.IsFloat = Marshal.ReadInt32(p, 24) == 3;   // SubFormat.Data1: 1 PCM, 3 IEEE float
            return f;
        }
        // WAVEFORMATEXTENSIBLE, 16-bit stereo PCM: the format the HDMI/DP endpoint lists for exclusive mode
        public static Format Pcm16Stereo(int rate)
        {
            IntPtr p = Marshal.AllocHGlobal(40);
            for (int i = 0; i < 40; i++) Marshal.WriteByte(p, i, 0);
            Marshal.WriteInt16(p, 0, unchecked((short)0xFFFE));
            Marshal.WriteInt16(p, 2, 2);
            Marshal.WriteInt32(p, 4, rate);
            Marshal.WriteInt32(p, 8, rate * 4);
            Marshal.WriteInt16(p, 12, 4);
            Marshal.WriteInt16(p, 14, 16);
            Marshal.WriteInt16(p, 16, 22);
            Marshal.WriteInt16(p, 18, 16);
            Marshal.WriteInt32(p, 20, 3);                                  // SPEAKER_FRONT_LEFT | SPEAKER_FRONT_RIGHT
            byte[] pcm = new Guid("00000001-0000-0010-8000-00aa00389b71").ToByteArray();
            Marshal.Copy(pcm, 0, new IntPtr(p.ToInt64() + 24), 16);
            return Read(p);
        }
    }

    class Program
    {
        const uint CLSCTX_ALL = 0x17;
        const int SHARED = 0, EXCLUSIVE = 1;
        const uint FLAG_EVENTCALLBACK = 0x00040000;
        const int E_NOT_ALIGNED = unchecked((int)0x88890019);
        static readonly Guid IID_IAudioClient = new Guid("1CB9AD4C-DBFA-4c32-B178-C2F568A703B2");
        static readonly Guid IID_IAudioRenderClient = new Guid("F294ACFC-3146-4483-A7BF-ADDCA7C260E2");
        static readonly Guid IID_IAudioClock = new Guid("CD63314F-3FBA-4a1b-812C-EF96358728E7");

        static double Now() { return (double)Stopwatch.GetTimestamp() / Stopwatch.Frequency; }
        static void Out(string s) { Console.WriteLine(s); Console.Out.Flush(); }
        static string Hr(int hr) { return string.Format("0x{0:X8}", hr); }

        static double phase;
        static void Fill(IntPtr dst, uint frames, Format f, double amp)
        {
            int n = (int)frames * f.Channels;
            double step = 2 * Math.PI * 1000.0 / f.Rate;
            if (f.IsFloat && f.Bits == 32)
            {
                float[] buf = new float[n];
                for (int i = 0; i < (int)frames; i++) { float v = (float)(amp * Math.Sin(phase)); phase += step; for (int c = 0; c < f.Channels; c++) buf[i * f.Channels + c] = v; }
                Marshal.Copy(buf, 0, dst, n);
            }
            else if (f.Bits == 16)
            {
                short[] buf = new short[n];
                for (int i = 0; i < (int)frames; i++) { short v = (short)(amp * 32767 * Math.Sin(phase)); phase += step; for (int c = 0; c < f.Channels; c++) buf[i * f.Channels + c] = v; }
                Marshal.Copy(buf, 0, dst, n);
            }
            else
            {   // 24-in-32 or 32-bit integer containers
                int[] buf = new int[n];
                for (int i = 0; i < (int)frames; i++) { int v = (int)(amp * 2147483647.0 * Math.Sin(phase)); phase += step; for (int c = 0; c < f.Channels; c++) buf[i * f.Channels + c] = v; }
                Marshal.Copy(buf, 0, dst, n);
            }
            if (phase > 2 * Math.PI * 1000) phase -= 2 * Math.PI * 1000;
        }

        static IAudioClient Activate(IMMDevice dev, out double ms)
        {
            object o; Guid iid = IID_IAudioClient;
            double t = Now();
            int hr = dev.Activate(ref iid, CLSCTX_ALL, IntPtr.Zero, out o);
            ms = (Now() - t) * 1000;
            if (hr != 0) throw new Exception("Activate " + Hr(hr));
            return (IAudioClient)o;
        }

        static int Main(string[] args)
        {
            string mode = args.Length > 0 ? args[0] : "info";
            double secs = 10, amp = 0.25; int rate = 48000; int periodMs = 0;
            for (int i = 1; i + 1 < args.Length; i += 2)
            {
                if (args[i] == "-secs") secs = double.Parse(args[i + 1], System.Globalization.CultureInfo.InvariantCulture);
                else if (args[i] == "-rate") rate = int.Parse(args[i + 1]);
                else if (args[i] == "-amp") amp = double.Parse(args[i + 1], System.Globalization.CultureInfo.InvariantCulture);
                else if (args[i] == "-periodms") periodMs = int.Parse(args[i + 1]);
            }
            if (secs > 60) secs = 60;   // lab trials stay short
            try { return Run(mode, secs, rate, amp, periodMs); }
            catch (Exception e) { Out("error=" + e.Message.Replace(' ', '_')); return 2; }
        }

        static int Run(string mode, double secs, int rate, double amp, int periodMs)
        {
            IMMDeviceEnumerator en = (IMMDeviceEnumerator)new MMDeviceEnumeratorComObject();
            IMMDevice dev; int hr = en.GetDefaultAudioEndpoint(0, 1, out dev);   // eRender, eMultimedia
            if (hr != 0) { Out("error=GetDefaultAudioEndpoint_" + Hr(hr)); return 2; }
            string id; dev.GetId(out id);
            Out("device=" + id);
            double actMs; IAudioClient ac = Activate(dev, out actMs);
            long defPeriod, minPeriod; ac.GetDevicePeriod(out defPeriod, out minPeriod);
            IntPtr mixPtr; ac.GetMixFormat(out mixPtr);
            Format mix = Format.Read(mixPtr);
            Out(string.Format("activate_ms={0:F1} device_period_default_100ns={1} min_100ns={2} mix {3}", actMs, defPeriod, minPeriod, mix));
            if (mode == "info") return 0;

            bool excl = mode.StartsWith("excl"), evt = mode.EndsWith("event");
            Format f = excl ? Format.Pcm16Stereo(rate) : mix;
            long period = periodMs > 0 ? periodMs * 10000L : defPeriod;
            long bufferDur = excl ? period : (evt ? 0 : 2 * period);             // shared event: 0 = engine minimum
            long periodicity = excl ? period : 0;
            uint flags = evt ? FLAG_EVENTCALLBACK : 0;
            double t = Now();
            hr = ac.Initialize(excl ? EXCLUSIVE : SHARED, flags, bufferDur, periodicity, f.Ptr, IntPtr.Zero);
            if (hr == E_NOT_ALIGNED)
            {
                uint fr; ac.GetBufferSize(out fr);
                Marshal.ReleaseComObject(ac);
                period = (long)(10000000.0 * fr / f.Rate + 0.5);
                ac = Activate(dev, out actMs);
                t = Now();
                hr = ac.Initialize(EXCLUSIVE, flags, period, period, f.Ptr, IntPtr.Zero);
                Out("realigned_period_100ns=" + period);
            }
            double initMs = (Now() - t) * 1000;
            if (hr != 0) { Out(string.Format("error=Initialize_{0} init_ms={1:F1} format {2}", Hr(hr), initMs, f)); return 3; }
            uint bufFrames; ac.GetBufferSize(out bufFrames);
            long latency; ac.GetStreamLatency(out latency);
            object o; Guid iid = IID_IAudioRenderClient; ac.GetService(ref iid, out o); IAudioRenderClient rc = (IAudioRenderClient)o;
            iid = IID_IAudioClock; ac.GetService(ref iid, out o); IAudioClock clk = (IAudioClock)o;
            ulong freq; clk.GetFrequency(out freq);
            EventWaitHandle ev = null;
            if (evt) { ev = new EventWaitHandle(false, EventResetMode.AutoReset); ac.SetEventHandle(ev.SafeWaitHandle.DangerousGetHandle()); }
            Out(string.Format("mode={0} format {1} init_ms={2:F1} buffer_frames={3} latency_100ns={4} clock_freq={5} period_100ns={6}",
                mode, f, initMs, bufFrames, latency, freq, period));

            IntPtr data;
            if (rc.GetBuffer(bufFrames, out data) == 0) { Fill(data, bufFrames, f, amp); rc.ReleaseBuffer(bufFrames, 0); }
            ulong written = bufFrames;
            t = Now();
            hr = ac.Start();
            double startMs = (Now() - t) * 1000;
            double t0 = Now(), lastWake = t0, nextSample = t0, firstMove = -1;
            int events = 0, timeouts = 0, writeFails = 0;
            int[] hist = new int[8];  // wake interval in ms: <2 <5 <15 <25 <35 <60 <150 >=150
            double maxGap = 0, sumGap = 0;
            List<double[]> samples = new List<double[]>();
            ulong pos0, q0; clk.GetPosition(out pos0, out q0);
            Out(string.Format("start_ms={0:F1} start_hr={1}", startMs, Hr(hr)));
            int pollMs = Math.Max(1, (int)(period / 20000));   // half a period
            while (Now() - t0 < secs)
            {
                if (evt)
                {
                    bool got = ev.WaitOne(2000);
                    double w = Now(), gap = (w - lastWake) * 1000; lastWake = w;
                    if (!got) timeouts++; else events++;
                    maxGap = Math.Max(maxGap, gap); sumGap += gap;
                    int b = gap < 2 ? 0 : gap < 5 ? 1 : gap < 15 ? 2 : gap < 25 ? 3 : gap < 35 ? 4 : gap < 60 ? 5 : gap < 150 ? 6 : 7;
                    hist[b]++;
                }
                else Thread.Sleep(pollMs);
                uint n;
                if (excl && evt) n = bufFrames;
                else { uint pad; ac.GetCurrentPadding(out pad); n = bufFrames - pad; }
                if (n > 0)
                {
                    if (rc.GetBuffer(n, out data) == 0) { Fill(data, n, f, amp); rc.ReleaseBuffer(n, 0); written += n; }
                    else writeFails++;
                }
                double now = Now();
                if (now >= nextSample)
                {
                    ulong pos, qpc; clk.GetPosition(out pos, out qpc);
                    uint pad2 = 0; if (!(excl && evt)) ac.GetCurrentPadding(out pad2);
                    if (firstMove < 0 && pos != pos0) firstMove = (now - t0) * 1000;
                    samples.Add(new double[] { now - t0, (double)pos / freq, pad2 });
                    Out(string.Format("sample t={0:F3} pos_s={1:F4} padding={2} written_s={3:F4}", now - t0, (double)pos / freq, pad2, (double)written / f.Rate));
                    nextSample += 0.25;
                }
            }
            ac.Stop();
            double elapsed = Now() - t0;
            double rateAll = 0, rateSteady = 0;
            if (samples.Count > 1)
            {
                double[] a = samples[0], z = samples[samples.Count - 1];
                rateAll = (z[1] - a[1]) / (z[0] - a[0]);
                double[] s1 = null;
                foreach (double[] s in samples) if (s[0] >= 2.0) { s1 = s; break; }
                if (s1 != null && z[0] > s1[0]) rateSteady = (z[1] - s1[1]) / (z[0] - s1[0]);
            }
            Out(string.Format("result mode={0} rate={1} elapsed_s={2:F3} written_s={3:F3} device_rate_all={4:F4} device_rate_after2s={5:F4} first_move_ms={6:F1} events={7} timeouts={8} mean_gap_ms={9:F2} max_gap_ms={10:F1} write_fails={11}",
                mode, f.Rate, elapsed, (double)written / f.Rate, rateAll, rateSteady, firstMove, events, timeouts,
                events + timeouts > 0 ? sumGap / (events + timeouts) : 0, maxGap, writeFails));
            if (evt) Out(string.Format("gap_hist_ms <2:{0} <5:{1} <15:{2} <25:{3} <35:{4} <60:{5} <150:{6} >=150:{7}", hist[0], hist[1], hist[2], hist[3], hist[4], hist[5], hist[6], hist[7]));
            return 0;
        }
    }
}
