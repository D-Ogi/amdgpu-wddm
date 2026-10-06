// Host test of the telemetry line: the real TelemetryWindow, TelemetryProvider, State and TelemetryLine against a
// scripted ITelemetrySource. No control DLL, adapter, escape, window or UI thread is involved. The last block
// times the managed work per sample and per paint (the overlay's own CPU, minus the kernel calls).
using System;
using System.Collections.Generic;
using System.Diagnostics;
using System.Drawing;
using System.Globalization;
using System.IO;
using System.Linq;
using System.Threading;
using System.Web.Script.Serialization;
using Bc250Mon;

sealed class FakeSource : ITelemetrySource
{
    public Queue<object> Dpm = new Queue<object>();
    public DpmSnapshot Default;
    public object Clock, Vram, Fan;
    public int DpmCalls, ClockCalls, VramCalls, FanCalls;
    public DpmSnapshot ReadDpm()
    {
        DpmCalls++;
        object next = Dpm.Count > 0 ? Dpm.Dequeue() : Default;
        if (next is Exception) throw (Exception)next;
        return (DpmSnapshot)next;
    }
    public ClockSnapshot ReadClock()
    {
        ClockCalls++;
        if (Clock is Exception) throw (Exception)Clock;
        return (ClockSnapshot)Clock;
    }
    public VideoMemorySnapshot ReadVideoMemory()
    {
        VramCalls++;
        if (Vram is Exception) throw (Exception)Vram;
        return (VideoMemorySnapshot)Vram;
    }
    public HwmonSnapshot ReadHwmon()
    {
        FanCalls++;
        if (Fan is Exception) throw (Exception)Fan;
        return (HwmonSnapshot)Fan;
    }
}

static class TelemetryTest
{
    static int checks, failures;
    static void Check(bool value, string name) { checks++; if (!value) { failures++; Console.WriteLine("FAIL " + name); } }
    const uint Running = 1, Temp = DpmSnapshot.FlagTemperature, Clk = DpmSnapshot.FlagClock, Hw = DpmSnapshot.FlagHwBusy;

    static DpmSnapshot Dpm(uint version, uint flags, int mc, uint mhz, uint avg)
    {
        return new DpmSnapshot { Version = version, Flags = flags, TemperatureMc = mc, ObservedMHz = mhz, BusyAvgPermille = avg, BusyPermille = 999 };
    }
    static VideoMemorySnapshot Vram(ulong resident, ulong limit, ulong dedicated)
    {
        return new VideoMemorySnapshot { Size = 264, Segments = 3, LocalResident = resident, LocalLimit = limit, DedicatedVideoMemory = dedicated,
                                         ApertureResident = 64ul << 20, ApertureLimit = 256ul << 20 };
    }
    // Unit A's hardware monitor as E01 read it: five channels, one fan at 1589 rpm, duty read-back 961 permille,
    // the APU at 83.0 C over the chip's own SB-TSI link.
    const uint FanValid = HwmonSnapshot.FlagValid, FanFresh = HwmonSnapshot.FlagFresh,
               FanProven = HwmonSnapshot.FlagDutyProven, FanGated = HwmonSnapshot.FlagGated,
               FanStopped = HwmonSnapshot.FlagStopped;
    static HwmonSnapshot FanSnap(uint flags, uint rpm, uint dutyPermille, uint reason)
    {
        var h = new HwmonSnapshot
        {
            Magic = 0x30353242, Command = 27, AbiVersion = 1, Op = 0, Flags = flags, Reason = reason,
            BasePort = 0x0A20, CustomerId = 0x0E2C, EcVersion = 0x0100, FanPresentMask = 0x1F, DutyPresentMask = 0x1F,
            Rpm = new uint[8], DutyPermille = new uint[8], TemperatureMc = new int[4], TemperatureSource = new uint[4],
        };
        h.Rpm[1] = rpm;
        for (int i = 0; i < 5; i++) h.DutyPermille[i] = dutyPermille;
        h.TemperatureSource[0] = HwmonSnapshot.SourceApu; h.TemperatureMc[0] = 83000;
        h.TemperatureSource[1] = 8; h.TemperatureMc[1] = 59500;
        return h;
    }
    static FakeSource Source()
    {
        return new FakeSource { Default = Dpm(0x000700B1, Running | Temp | Clk | Hw, 67500, 1000, 250),
                                Clock = new ClockSnapshot { TemperatureMc = 70250, ObservedMHz = 1200, AbiVersion = 1, Ready = 1 },
                                Vram = Vram(1234ul << 20, 2048ul << 20, 2080ul << 20),
                                Fan = FanSnap(FanValid | FanFresh | FanProven, 1589, 961, 0) };
    }
    static Telemetry Window(FakeSource s, int samples)
    {
        var w = new TelemetryWindow();
        for (int i = 0; i < samples; i++)
        {
            try { w.Add(s.ReadDpm()); } catch (Exception e) { w.Fail(e.Message); }
        }
        return w.Close(new DateTime(2026, 9, 30, 12, 0, 0), s);
    }

    static int Main(string[] args)
    {
        // The lab runs a Polish Windows: the line and the API must still say 67.5, not 67,5.
        Thread.CurrentThread.CurrentCulture = new CultureInfo("pl-PL");

        // 0.7.177: everything from one escape, load is the mean of the samples' averages.
        var s = Source();
        foreach (uint avg in new uint[] { 100, 200, 300, 400, 500, 600, 700, 800 }) s.Dpm.Enqueue(Dpm(0x000700B1, Running | Temp | Clk | Hw, 67500, 1000, avg));
        var t = Window(s, 8);
        Check(t.LoadPercent == 45.0 && t.LoadSamples == 8 && t.LoadSource == "grbm", "grbm load is the mean of BusyAvgPermille");
        Check(t.TemperatureC == 67.5 && t.TemperatureSource == "dpm" && t.GfxMHz == 1000 && t.ClockSource == "dpm", "Tctl and clock from the snapshot");
        Check(s.ClockCalls == 0 && s.VramCalls == 1, "no SMU clock read while the snapshot has both");
        Check(t.VramUsedBytes == 1234ul << 20 && t.VramTotalBytes == 2048ul << 20 && t.VramTotalSource == "commit-limit", "VRAM from the memory segments");
        Check(t.ApertureUsedBytes == 64ul << 20 && t.ApertureTotalBytes == 256ul << 20, "aperture kept apart");
        Check(t.Note == null && t.TemperatureLevel == Level.Good && t.KmdVersion == 0x000700B1, "clean sample has no note");
        Check(t.FanRpm == 1589 && t.FanDutyPercent == 96 && !t.FanStopped && t.FanApuC == 83.0, "fan from the hardware monitor");
        Check(s.FanCalls == 1, "the fan is read once per publication, not once per sample");
        Check(t.Text == "Tctl 67.5 C  load 45 %  GFX 1000 MHz  VRAM 1234/2048 MB  fan 1589 rpm (96 %)", "text: " + t.Text);

        // 0.7.176 (deployed on 2026-09-30): only the submit share exists, so load is n/a, the rest still comes.
        s = Source(); s.Default = Dpm(0x000700B0, Running | Temp | Clk, 66000, 1000, 74);
        t = Window(s, 8);
        Check(t.LoadPercent == null && t.Note != null && t.Note.Contains("0x000700B0") && t.Note.Contains("0.7.177"), "0.7.176 load n/a: " + t.Note);
        Check(t.TemperatureC == 66.0 && t.GfxMHz == 1000 && s.ClockCalls == 0, "0.7.176 Tctl and clock from the snapshot");
        Check(t.Text.Contains("load n/a") && t.Text.StartsWith("Tctl 66.0 C"), "0.7.176 text: " + t.Text);

        // 0.7.177 whose sampler is missing: the submit share again, and n/a again.
        s = Source(); s.Default = Dpm(0x000700B1, Running | Temp | Clk, 66000, 1000, 74);
        t = Window(s, 4);
        Check(t.LoadPercent == null && t.Note == "load n/a: no GRBM busy samples", "no hardware samples: " + t.Note);

        // Mixed window: only the hardware samples count.
        s = Source();
        s.Dpm.Enqueue(Dpm(0x000700B1, Running | Temp | Clk | Hw, 67500, 1000, 600));
        s.Dpm.Enqueue(Dpm(0x000700B1, Running | Temp | Clk, 67500, 1000, 10));
        s.Dpm.Enqueue(new InvalidOperationException("transient"));
        s.Dpm.Enqueue(Dpm(0x000700B1, Running | Temp | Clk | Hw, 67500, 1000, 200));
        t = Window(s, 4);
        Check(t.LoadPercent == 40.0 && t.LoadSamples == 2, "mixed window averages hardware samples only");
        Check(t.Note == null, "a transient failure inside a good window is not reported");

        // Clamp: an average above 1000 permille would be a KMD bug, not 120 % load.
        s = Source(); s.Default = Dpm(0x000700B1, Running | Temp | Clk | Hw, 67500, 1000, 1200);
        Check(Window(s, 2).LoadPercent == 100.0, "load clamps at 100 %");

        // KMD before 0.7.175 (or an old control DLL): the typed clock read stands in, load is n/a with the reason.
        s = Source(); s.Default = default(DpmSnapshot);
        for (int i = 0; i < 8; i++) s.Dpm.Enqueue(new InvalidOperationException("KMD DPM snapshot unavailable (0xC00000A3)"));
        t = Window(s, 8);
        Check(t.TemperatureC == 70.25 && t.TemperatureSource == "clock" && t.GfxMHz == 1200 && t.ClockSource == "clock" && s.ClockCalls == 1, "fallback to the clock escape");
        Check(t.Note == "load n/a: KMD DPM snapshot unavailable (0xC00000A3)" && t.KmdVersion == 0, "fallback note: " + t.Note);

        // A stopped governor leaves stale values behind: not used.
        s = Source(); s.Default = Dpm(0x000700B1, Temp | Clk | Hw, 99000, 1500, 900);
        t = Window(s, 8);
        Check(t.TemperatureSource == "clock" && t.TemperatureC == 70.25 && t.LoadPercent == null && t.Note.Contains("not running"), "stopped governor ignored");

        // Flags say the temperature is old: that one alone comes from the clock read.
        s = Source(); s.Default = Dpm(0x000700B1, Running | Clk | Hw, 99000, 1000, 500);
        t = Window(s, 8);
        Check(t.TemperatureSource == "clock" && t.TemperatureC == 70.25 && t.ClockSource == "dpm" && t.GfxMHz == 1000 && t.LoadPercent == 50.0, "stale temperature only");

        // Nothing answers: every value n/a, each with its reason, level neutral.
        s = Source(); s.Clock = new InvalidOperationException("KMD clock unavailable (0xC000000E)"); s.Vram = new InvalidOperationException("segment statistics unavailable (0xC000000E)");
        s.Fan = new EntryPointNotFoundException("no Bc250Hwmon");
        for (int i = 0; i < 8; i++) s.Dpm.Enqueue(new EntryPointNotFoundException("no Bc250Dpm"));
        t = Window(s, 8);
        Check(t.TemperatureC == null && t.GfxMHz == null && t.LoadPercent == null && t.VramUsedBytes == null && t.TemperatureLevel == Level.Info, "all n/a");
        Check(t.FanRpm == null && t.FanDutyPercent == null && !t.FanStopped, "fan n/a when the export is missing");
        Check(t.Note.Contains("no Bc250Dpm") && t.Note.Contains("0xC000000E") && t.Note.Contains("VRAM n/a")
              && t.Note.Contains("fan n/a: no Bc250Hwmon"), "all reasons: " + t.Note);
        Check(t.Text == "Tctl n/a  load n/a  GFX n/a MHz  VRAM n/a  fan n/a", "n/a text: " + t.Text);

        // The fan, state by state. The gate is closed by every install, and that is not worth a note every two
        // seconds: it is the configured state of the machine, not a fault.
        s = Source(); s.Fan = FanSnap(FanGated, 0, 0, 1);
        t = Window(s, 1);
        Check(t.FanRpm == null && t.Fan == "n/a" && t.Note == null, "closed gate is silent: " + t.Note);
        // The reader gave up on the window (reason 7, "port"). That one IS a note.
        s = Source(); s.Fan = FanSnap(0, 0, 0, 7);
        t = Window(s, 1);
        Check(t.FanRpm == null && t.Note == "fan n/a: reader offline, reason 7", "offline note: " + t.Note);
        // Online but the last sample is older than three periods: a stale reading is not a reading, and it says
        // something different from a refusal, because the operator fixes the two in different ways.
        s = Source();
        HwmonSnapshot old = FanSnap(FanValid | FanProven, 1589, 961, 0);
        old.AgeMs = 7200;
        s.Fan = old;
        t = Window(s, 1);
        Check(t.FanRpm == null && t.Note == "fan n/a: last reading 7200 ms old", "stale is not fresh: " + t.Note);
        // No lab trial has proved the duty read-back yet: the speed is shown, the percentage is not.
        s = Source(); s.Fan = FanSnap(FanValid | FanFresh, 1589, 961, 0);
        t = Window(s, 1);
        Check(t.FanRpm == 1589 && t.FanDutyPercent == null && t.Fan == "1589 rpm", "unproven duty is withheld: " + t.Fan);
        // A duty output runs and nothing turns.
        s = Source(); s.Fan = FanSnap(FanValid | FanFresh | FanProven | FanStopped, 0, 961, 0);
        t = Window(s, 1);
        Check(t.FanStopped && t.Fan == "stopped" && t.FanRpm == 0, "stopped fan: " + t.Fan);

        // VRAM totals: a missing or unbounded commit limit falls back to the dedicated size, then to none.
        s = Source(); s.Vram = Vram(300ul << 20, 0, 2080ul << 20);
        t = Window(s, 1);
        Check(t.VramTotalBytes == 2080ul << 20 && t.VramTotalSource == "dedicated", "limit 0 -> dedicated");
        s = Source(); s.Vram = Vram(300ul << 20, ulong.MaxValue, 0);
        t = Window(s, 1);
        Check(t.VramTotalBytes == null && t.Vram == "300 MB", "no total: " + t.Vram);
        Check(Telemetry.Megabytes((1ul << 20) + (1ul << 19)) == 2 && Telemetry.Megabytes((1ul << 20) + (1ul << 19) - 1) == 1 && Telemetry.Megabytes(null) == null, "MB rounding");

        // Temperature colours follow GpuProvider's thresholds.
        int warnMilli = (int)(GpuProvider.WarnC * 1000), errorMilli = (int)(GpuProvider.ErrorC * 1000);
        foreach (var c in new[] { Tuple.Create(warnMilli - 100, Level.Good), Tuple.Create(warnMilli, Level.Warn),
                                  Tuple.Create(errorMilli, Level.Error) })
        {
            s = Source(); s.Default = Dpm(0x000700B1, Running | Temp | Clk | Hw, c.Item1, 1000, 1);
            Check(Window(s, 1).TemperatureLevel == c.Item2, "level at " + c.Item1);
        }

        // Provider cadence: a sample every poll, a publication on the first poll and then every two seconds.
        var clock = TimeSpan.Zero;
        s = Source();
        string dir = Path.Combine(args.Length > 0 ? args[0] : Path.GetTempPath(), "telemetry-" + Guid.NewGuid().ToString("N"));
        var state = new State(dir);
        int telemetryEvents = 0, panelEvents = 0;
        state.TelemetryChanged += () => telemetryEvents++;
        state.Changed += () => panelEvents++;
        var provider = new TelemetryProvider(s, () => clock);
        Check(provider.Period == TimeSpan.FromMilliseconds(250) && provider.Name == "telemetry", "sample period");
        provider.Poll(state);
        Check(state.Telemetry != null && state.Telemetry.LoadSamples == 1 && s.VramCalls == 1 && telemetryEvents == 1, "first poll publishes");
        for (int i = 1; i < 8; i++) { clock = TimeSpan.FromMilliseconds(250 * i); provider.Poll(state); }
        Check(s.DpmCalls == 8 && s.VramCalls == 1 && telemetryEvents == 1, "no publication inside the window");
        clock = TimeSpan.FromSeconds(2); provider.Poll(state);
        Check(s.VramCalls == 2 && s.FanCalls == 2 && state.Telemetry.LoadSamples == 8, "second publication after 2 s with 8 samples");
        Check(telemetryEvents == 1, "same text: the screen is not told");
        s.Default = Dpm(0x000700B1, Running | Temp | Clk | Hw, 68500, 1000, 250);
        clock = TimeSpan.FromSeconds(4); provider.Poll(state);
        Check(telemetryEvents == 2 && state.Telemetry.TemperatureC == 68.5, "changed text: the screen is told");
        Check(panelEvents == 0, "telemetry never triggers a whole-window repaint");

        // The note reaches the log once, not every two seconds.
        s.Default = Dpm(0x000700B0, Running | Temp | Clk, 66000, 1000, 74);
        for (int i = 3; i < 7; i++) { clock = TimeSpan.FromSeconds(2 * i); provider.Poll(state); }
        int noteLines = state.Take(State.LogCapacity).Log.Count(l => l.Source == "telemetry");
        Check(noteLines == 1, "note logged once, got " + noteLines);

        // JSON for mon.py telemetry.
        var json = new JavaScriptSerializer();
        var d = json.Deserialize<Dictionary<string, object>>(json.Serialize(TelemetryProvider.Describe(state.Telemetry, state.Telemetry.Time.AddSeconds(1.26))));
        Check((bool)d["available"] && Convert.ToDouble(d["temperatureC"], CultureInfo.InvariantCulture) == 66.0 && d["loadPercent"] == null, "json values");
        Check(Convert.ToInt32(d["vramUsedMB"]) == 1234 && Convert.ToInt32(d["vramTotalMB"]) == 2048 && (string)d["kmdVersion"] == "0x000700B0", "json vram and version");
        Check(Convert.ToDouble(d["ageSeconds"], CultureInfo.InvariantCulture) == 1.3 && ((string)d["note"]).Contains("0.7.177"), "json age and note");
        Check(Convert.ToInt32(d["fanRpm"]) == 1589 && Convert.ToInt32(d["fanDutyPercent"]) == 96
              && !(bool)d["fanStopped"] && Convert.ToDouble(d["fanApuC"], CultureInfo.InvariantCulture) == 83.0, "json fan");
        var none = json.Deserialize<Dictionary<string, object>>(json.Serialize(TelemetryProvider.Describe(null, DateTime.Now)));
        Check(none.Count == 1 && !(bool)none["available"], "json before the first sample");

        // The painter stays inside the strip OverlayForm invalidates, at the widest line the lab can produce.
        var colours = new Func<Level, Color>(l => l == Level.Good ? Color.Lime : Color.White);
        using (var font = new Font("Consolas", 13.3f, GraphicsUnit.Pixel))
        using (var bmp = new Bitmap(460, 90))
        using (var g = Graphics.FromImage(bmp))
        {
            g.TextRenderingHint = System.Drawing.Text.TextRenderingHint.ClearTypeGridFit;
            var widest = new Telemetry { TemperatureC = 105.5, TemperatureLevel = Level.Error, LoadPercent = 100, GfxMHz = 2000,
                                         VramUsedBytes = 16384ul << 20, VramTotalBytes = 16384ul << 20 };
            float width = TelemetryLine.Width(g, font, widest);
            Check(width > 300 && width <= 460 - 2 * 14, "widest line fits the panel: " + width.ToString("0.0", CultureInfo.InvariantCulture) + " px");
            foreach (var line in new[] { widest, state.Telemetry, null })
            {
                g.Clear(Color.Black);
                TelemetryLine.Paint(g, font, line, 14, 460 - 2 * 14, colours, Color.Gray);
                var bounds = Rectangle.Round(TelemetryLine.Bounds(14, 460 - 2 * 14));
                int inside = 0, outside = 0;
                for (int y = 0; y < bmp.Height; y++)
                    for (int x = 0; x < bmp.Width; x++)
                        if (bmp.GetPixel(x, y).ToArgb() != Color.Black.ToArgb()) { if (bounds.Contains(x, y)) inside++; else outside++; }
                Check(inside > 100 && outside == 0, "paint inside its strip only (" + inside + " in, " + outside + " out)");
            }
            Check(TelemetryLine.Bottom == TelemetryLine.Top + TelemetryLine.Height + 2 && TelemetryLine.Top == 36, "strip geometry");

            // Cost of the managed work: 4 samples a second, one publication and at most one line paint every 2 s.
            var cpu = Process.GetCurrentProcess();
            var w = new TelemetryWindow();
            var src = Source();
            const int samples = 2000000, closes = 200000, paints = 5000;    // well above the 15.6 ms clock grain
            TimeSpan before = cpu.TotalProcessorTime;
            for (int i = 0; i < samples; i++) w.Add(src.Default);
            TimeSpan afterSamples = cpu.TotalProcessorTime;
            for (int i = 0; i < closes; i++) w.Close(DateTime.Now, src);
            TimeSpan afterCloses = cpu.TotalProcessorTime;
            for (int i = 0; i < paints; i++) TelemetryLine.Paint(g, font, widest, 14, 432, colours, Color.Gray);
            cpu.Refresh();
            TimeSpan afterPaints = cpu.TotalProcessorTime;
            double sampleUs = (afterSamples - before).TotalMilliseconds * 1000 / samples;
            double closeUs = (afterCloses - afterSamples).TotalMilliseconds * 1000 / closes;
            double paintUs = (afterPaints - afterCloses).TotalMilliseconds * 1000 / paints;
            double perSecondUs = 4 * sampleUs + 0.5 * closeUs + 0.5 * paintUs;
            Console.WriteLine(string.Format(CultureInfo.InvariantCulture,
                "managed cost: sample {0:0.00} us, publish {1:0.00} us, paint {2:0.0} us; per second {3:0.0} us = {4:0.0000} % of one core",
                sampleUs, closeUs, paintUs, perSecondUs, perSecondUs / 1e4));
            Check(perSecondUs < 10000, "managed telemetry work stays under 1 % of one core");
        }
        try { Directory.Delete(dir, true); } catch { }
        Console.WriteLine("Telemetry: " + checks + " checks, " + failures + " failures");
        return failures == 0 ? 0 : 1;
    }
}
