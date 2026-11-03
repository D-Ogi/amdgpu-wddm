// The GPU telemetry line (2026-09-30): Tctl, GPU load, GFX clock and VRAM in use, cheap enough
// to stay on the screen during a game. The desktop is composed on the CPU by DWM, so repaints are the expensive
// part: a 230x22 heartbeat window repainted every 250 ms ran through trials 139/140 with DWM at 28 % of the
// machine against 4 % without it. This line is therefore sampled four times a second but published at most
// every two seconds, OverlayForm repaints only its strip and only when the text changed, and it does so in the
// same repaint as the header clock, so the line never costs DWM a frame of its own.
//
// Sources, all read-only; none sends an SMU message on the overlay's behalf while the governor runs:
//   Tctl, GFX clock  the KMD clock governor's snapshot (BC250_ESCAPE_RUN_DPM READ via bc250control.dll Bc250Dpm):
//                    the Tctl of its last 25 ms tick and the SMU clock readback of the last second. Without the
//                    escape (KMD before 0.7.175), without a running governor, or with the flags saying the values
//                    are old, the typed clock read (BC250_ESCAPE_RUN_CLOCK) GpuProvider makes anyway stands in.
//   load             the governor's GRBM_STATUS.GUI_ACTIVE share, KMD 0.7.177 and later (FLAG_HW_BUSY): the mean
//                    of its ~100 ms average over the window's samples. Before 0.7.177 the only busy figure is the
//                    ring's submit-to-fence share, which read 7 % while ETW saw 91 % (trial 139), so the line
//                    says n/a instead of showing it.
//   VRAM             dxgkrnl's segment statistics (D3DKMTQueryStatistics via Bc250VideoMemory): bytes resident in
//                    the memory (non-aperture) segments over their commit limit, as Task Manager counts
//                    dedicated memory. Standard WDDM: it does not depend on what the KMD reports about itself.
using System;
using System.Collections.Generic;
using System.Diagnostics;
using System.Drawing;
using System.Drawing.Drawing2D;
using System.Globalization;

namespace Bc250Mon
{
    // Samples of one publishing window, and what they add up to.
    public sealed class TelemetryWindow
    {
        public const uint FlagRunning = 1, GrbmBusyVersion = 0x000700B1;
        long _busySum;
        int _busySamples, _samples;
        DpmSnapshot _last;
        string _dpmError;

        // The hottest Tctl of the window's samples, and the last fresh SMU GFX temperature.
        int? _maxTctlMc;
        double? _gfxC;

        public void Add(DpmSnapshot s) { Add(s, default(DpmMetrics)); }

        public void Add(DpmSnapshot s, DpmMetrics metrics)
        {
            _dpmError = null;
            // A stopped governor leaves its last tick behind: old values with fresh-looking flags.
            if ((s.Flags & FlagRunning) == 0) { _dpmError = "DPM governor not running"; return; }
            _samples++;
            _last = s;
            if ((s.Flags & DpmSnapshot.FlagTemperature) != 0 && (!_maxTctlMc.HasValue || s.TemperatureMc > _maxTctlMc.Value))
                _maxTctlMc = s.TemperatureMc;
            double? gfx = DpmMetrics.GfxC(s, metrics);
            if (gfx.HasValue) _gfxC = gfx;
            if ((s.Flags & DpmSnapshot.FlagHwBusy) != 0) { _busySum += Math.Min(1000u, s.BusyAvgPermille); _busySamples++; }
        }

        public void Fail(string error) { _dpmError = error; }

        public Telemetry Close(DateTime now, ITelemetrySource source)
        {
            var t = new Telemetry { Time = now };
            var notes = new List<string>();
            bool dpm = _samples > 0;
            if (dpm)
            {
                t.KmdVersion = _last.Version;
                if ((_last.Flags & DpmSnapshot.FlagTemperature) != 0) { t.TemperatureC = _last.TemperatureMc / 1000.0; t.TemperatureSource = "dpm"; }
                if ((_last.Flags & DpmSnapshot.FlagClock) != 0) { t.GfxMHz = _last.ObservedMHz; t.ClockSource = "dpm"; }
            }
            if (_busySamples > 0)
            {
                t.LoadPercent = _busySum / 10.0 / _busySamples;
                t.LoadSamples = _busySamples;
                t.LoadSource = "grbm";
            }
            else if (dpm && _last.Version < GrbmBusyVersion)
                notes.Add("load n/a: KMD 0x" + _last.Version.ToString("X8") + " has no GRBM busy share (0.7.177)");
            else if (dpm) notes.Add("load n/a: no GRBM busy samples");
            else notes.Add("load n/a: " + (_dpmError ?? "no DPM snapshot"));

            if (!t.TemperatureC.HasValue || !t.GfxMHz.HasValue)
            {
                try
                {
                    ClockSnapshot c = source.ReadClock();
                    if (!t.TemperatureC.HasValue) { t.TemperatureC = c.TemperatureMc / 1000.0; t.TemperatureSource = "clock"; }
                    if (!t.GfxMHz.HasValue) { t.GfxMHz = c.ObservedMHz; t.ClockSource = "clock"; }
                }
                catch (Exception e) { notes.Add("Tctl/clock n/a: " + e.Message); }
            }

            try
            {
                VideoMemorySnapshot m = source.ReadVideoMemory();
                t.VramUsedBytes = m.LocalResident;
                if (m.LocalLimit != 0 && m.LocalLimit != ulong.MaxValue) { t.VramTotalBytes = m.LocalLimit; t.VramTotalSource = "commit-limit"; }
                else if (m.DedicatedVideoMemory != 0) { t.VramTotalBytes = m.DedicatedVideoMemory; t.VramTotalSource = "dedicated"; }
                t.ApertureUsedBytes = m.ApertureResident;
                if (m.ApertureLimit != 0 && m.ApertureLimit != ulong.MaxValue) t.ApertureTotalBytes = m.ApertureLimit;
            }
            catch (Exception e) { notes.Add("VRAM n/a: " + e.Message); }

            // Once per publishing window, not once per sample: the fan changes over seconds, and the lesson of
            // the summary poll (trial 41, 300 ms of game stall) is that an escape four times a second is never
            // free, software snapshot or not.
            try
            {
                HwmonSnapshot f = source.ReadHwmon();
                if (Hwmon.Reading(f))
                {
                    t.FanStopped = Hwmon.Has(f, HwmonSnapshot.FlagStopped);
                    t.FanRpm = Hwmon.Rpm(f);
                    if (Hwmon.Has(f, HwmonSnapshot.FlagDutyProven)) t.FanDutyPercent = Hwmon.DutyPercent(f);
                    t.FanApuC = Hwmon.ApuC(f);
                }
                // A closed gate is the configured state of every install, not a fault, so it says nothing. The two
                // other ways to have no reading are worth one note each, and they are not the same thing: a stale
                // sample means the sampler stopped, a refusal means the reader never got going or gave up.
                else if (!Hwmon.Has(f, HwmonSnapshot.FlagGated))
                    notes.Add(Hwmon.Has(f, HwmonSnapshot.FlagValid)
                        ? "fan n/a: last reading " + f.AgeMs + " ms old"
                        : "fan n/a: reader offline, reason " + f.Reason);
            }
            catch (Exception e) { notes.Add("fan n/a: " + e.Message); }

            t.TemperatureLevel = TctlLevel(t.TemperatureC);
            t.GfxTemperatureC = _gfxC;
            // The window's own peak: what TelemetryProvider feeds into the 60 s peak. Without DPM samples the one
            // clock-escape reading is the only value there is.
            t.TctlPeakC = _maxTctlMc.HasValue ? _maxTctlMc.Value / 1000.0 : t.TemperatureC;
            t.TctlPeakLevel = TctlLevel(t.TctlPeakC);
            t.Note = notes.Count > 0 ? string.Join("; ", notes) : null;
            _busySum = 0; _busySamples = 0; _samples = 0; _maxTctlMc = null; _gfxC = null;
            return t;
        }

        // Tctl colours of the line: amber from 85 C, red from 87 C, the lab's stop limit.
        public const double TctlWarnC = 85, TctlErrorC = 87;
        public static Level TctlLevel(double? c)
        {
            return !c.HasValue ? Level.Info : c.Value >= TctlErrorC ? Level.Error : c.Value >= TctlWarnC ? Level.Warn : Level.Good;
        }
    }

    // The highest value over a sliding time span. Values arrive in time order; a monotonic queue keeps only the
    // values that can still become the maximum, so Add and Peak cost O(1) amortised.
    public sealed class SlidingPeak
    {
        readonly TimeSpan _span;
        readonly LinkedList<KeyValuePair<TimeSpan, double>> _q = new LinkedList<KeyValuePair<TimeSpan, double>>();
        public SlidingPeak(TimeSpan span) { _span = span; }

        public void Add(TimeSpan at, double value)
        {
            while (_q.Count > 0 && _q.Last.Value.Value <= value) _q.RemoveLast();
            _q.AddLast(new KeyValuePair<TimeSpan, double>(at, value));
        }

        // The peak of the values added in (now - span, now]; null when there is none.
        public double? Peak(TimeSpan now)
        {
            while (_q.Count > 0 && _q.First.Value.Key <= now - _span) _q.RemoveFirst();
            return _q.Count > 0 ? (double?)_q.First.Value.Value : null;
        }
    }

    public sealed class TelemetryProvider : IProvider
    {
        public static readonly TimeSpan SamplePeriod = TimeSpan.FromMilliseconds(250), PublishPeriod = TimeSpan.FromSeconds(2);
        readonly ITelemetrySource _source;
        readonly Func<TimeSpan> _clock;
        readonly TelemetryWindow _window = new TelemetryWindow();
        public static readonly TimeSpan PeakSpan = TimeSpan.FromSeconds(60);
        readonly SlidingPeak _peak = new SlidingPeak(PeakSpan);
        // The operating-point panel reads the DPM governor's mode, cap and throttle out of the same snapshot
        // this provider already takes four times a second, so that panel costs no escape of its own.
        readonly DpmFeed _feed;
        TimeSpan? _due;
        string _lastNote;

        public TelemetryProvider(ITelemetrySource source) : this(source, Stopwatch.StartNew(), null) { }
        public TelemetryProvider(ITelemetrySource source, DpmFeed feed) : this(source, Stopwatch.StartNew(), feed) { }
        TelemetryProvider(ITelemetrySource source, Stopwatch watch, DpmFeed feed) : this(source, () => watch.Elapsed, feed) { }
        public TelemetryProvider(ITelemetrySource source, Func<TimeSpan> clock) : this(source, clock, null) { }
        public TelemetryProvider(ITelemetrySource source, Func<TimeSpan> clock, DpmFeed feed)
        { _source = source; _clock = clock; _feed = feed; }
        public string Name { get { return "telemetry"; } }
        public TimeSpan Period { get { return SamplePeriod; } }

        public void Poll(State state)
        {
            try
            {
                DpmSnapshot sample = _source.ReadDpm();
                _window.Add(sample, _source.LastMetrics);
                // Published whatever the flags say: a stopped governor is exactly what the other panel reports.
                if (_feed != null) _feed.Publish(sample);
            }
            catch (Exception e)
            {
                _window.Fail(e.Message);
                if (_feed != null) _feed.Fail(e.Message);
            }
            TimeSpan now = _clock();
            if (_due.HasValue && now < _due.Value) return;
            _due = now + PublishPeriod;
            Telemetry t = _window.Close(DateTime.Now, _source);
            // The window's hottest sample enters the 60 s peak, so a short spike between two publications is
            // still on the screen for a minute (2026-11-03: the guard stopped a game at 90.9 C after a 2 s spike).
            if (t.TctlPeakC.HasValue) _peak.Add(now, t.TctlPeakC.Value);
            t.TctlPeakC = _peak.Peak(now);
            t.TctlPeakLevel = TelemetryWindow.TctlLevel(t.TctlPeakC);
            state.SetTelemetry(t);
            // Logged on change only: an old KMD would otherwise say the same thing every two seconds forever.
            if (t.Note != _lastNote && t.Note != null) state.Log(Name, Level.Info, t.Note);
            _lastNote = t.Note;
        }

        // GET /telemetry and the "telemetry" member of GET /state; mon.py telemetry prints it.
        public static Dictionary<string, object> Describe(Telemetry t, DateTime now)
        {
            if (t == null) return new Dictionary<string, object> { { "available", false } };
            return new Dictionary<string, object>
            {
                { "available", true }, { "time", t.Time.ToString("o", CultureInfo.InvariantCulture) },
                { "ageSeconds", Math.Round((now - t.Time).TotalSeconds, 1) }, { "text", t.Text },
                { "temperatureC", t.TemperatureC.HasValue ? (object)Math.Round(t.TemperatureC.Value, 1) : null },
                { "temperatureLevel", t.TemperatureLevel.ToString() }, { "temperatureSource", t.TemperatureSource },
                { "tctlPeak60C", t.TctlPeakC.HasValue ? (object)Math.Round(t.TctlPeakC.Value, 1) : null },
                { "tctlPeak60Level", t.TctlPeakLevel.ToString() },
                { "gfxTemperatureC", t.GfxTemperatureC.HasValue ? (object)Math.Round(t.GfxTemperatureC.Value, 1) : null },
                { "temperatureText", t.TemperatureText },
                { "loadPercent", t.LoadPercent.HasValue ? (object)Math.Round(t.LoadPercent.Value, 1) : null },
                { "loadSource", t.LoadSource }, { "loadSamples", t.LoadSamples },
                { "gfxMHz", t.GfxMHz }, { "clockSource", t.ClockSource },
                { "vramUsedMB", Telemetry.Megabytes(t.VramUsedBytes) }, { "vramTotalMB", Telemetry.Megabytes(t.VramTotalBytes) },
                { "vramUsedBytes", t.VramUsedBytes }, { "vramTotalBytes", t.VramTotalBytes }, { "vramTotalSource", t.VramTotalSource },
                { "apertureUsedMB", Telemetry.Megabytes(t.ApertureUsedBytes) }, { "apertureTotalMB", Telemetry.Megabytes(t.ApertureTotalBytes) },
                { "fanRpm", t.FanRpm }, { "fanDutyPercent", t.FanDutyPercent }, { "fanStopped", t.FanStopped },
                { "fanApuC", t.FanApuC.HasValue ? (object)Math.Round(t.FanApuC.Value, 1) : null },
                { "kmdVersion", t.KmdVersion != 0 ? "0x" + t.KmdVersion.ToString("X8") : null }, { "note", t.Note },
            };
        }
    }

    // The line itself, in OverlayForm's 96-dpi layout units, right under the title. Everything it draws stays
    // inside Bounds (it clips to it), which is what OverlayForm invalidates when the line changes.
    public static class TelemetryLine
    {
        // Two rows since 2026-11-03: the temperatures on top, then load, clock and VRAM. One row cannot hold both
        // temperatures and the peak next to the widest load line inside the 460-unit panel.
        public const int Top = 36, RowHeight = 18, Height = 2 * RowHeight, Bottom = Top + Height + 2;

        public static RectangleF Bounds(float x, float width) { return new RectangleF(x, Top, width, Height); }

        static KeyValuePair<string, Color> Seg(string text, Color c) { return new KeyValuePair<string, Color>(text, c); }

        // Row 0: "Tctl 82.1 C  60s max 90.9 C  GFX 70.6 C". Tctl and its peak take the 85/87 C colours; the label
        // text stays dim so the number carries the colour.
        internal static List<KeyValuePair<string, Color>> TemperatureSegments(Telemetry t, Func<Level, Color> color, Color dim)
        {
            var s = new List<KeyValuePair<string, Color>>();
            if (t == null) return s;
            s.Add(Seg("Tctl", dim));
            s.Add(Seg(t.Temperature, t.TemperatureC.HasValue ? color(t.TemperatureLevel) : dim));
            s.Add(Seg("60s max", dim));
            s.Add(Seg(t.TctlPeak, t.TctlPeakC.HasValue ? color(t.TctlPeakLevel) : dim));
            s.Add(Seg("GFX", dim));
            s.Add(Seg(t.GfxTemperature, t.GfxTemperatureC.HasValue ? color(Level.Info) : dim));
            return s;
        }

        static List<KeyValuePair<string, Color>> Segments(Telemetry t, Func<Level, Color> color, Color dim)
        {
            var s = new List<KeyValuePair<string, Color>> { new KeyValuePair<string, Color>("GPU", dim) };
            if (t == null) { s.Add(new KeyValuePair<string, Color>("waiting for the first sample", dim)); return s; }
            s.Add(new KeyValuePair<string, Color>("load " + t.Load, t.LoadPercent.HasValue ? color(Level.Info) : dim));
            s.Add(new KeyValuePair<string, Color>(t.Clock, t.GfxMHz.HasValue ? color(Level.Info) : dim));
            s.Add(new KeyValuePair<string, Color>("VRAM " + t.Vram, t.VramUsedBytes.HasValue ? color(Level.Info) : dim));
            // The fan is NOT a segment here, deliberately. This strip is clipped to a 460-unit panel, and the
            // widest line the lab can produce (105.5 C, 100 % load, 2000 MHz, 16384/16384 MB) already fills it;
            // "fan 1589 rpm (96 %)" would push the right-hand end past the clip and silently cut the VRAM figure
            // off. The fan lives in the SoC / GPU panel row, in Telemetry.Text, in GET /telemetry and in the
            // support report. Before adding it back, widen the panel - do not quietly make the line longer.
            return s;
        }

        // Consolas is monospaced: segments sit on a character grid, two columns apart.
        static float Advance(Graphics g, Font font)
        {
            return g.MeasureString("0000000000", font, PointF.Empty, StringFormat.GenericTypographic).Width / 10f;
        }

        static int Columns(List<KeyValuePair<string, Color>> segments)
        {
            int columns = -2;
            foreach (var s in segments) columns += s.Key.Length + 2;
            return Math.Max(0, columns);
        }

        // The wider of the two rows.
        public static float Width(Graphics g, Font font, Telemetry t)
        {
            int columns = Math.Max(Columns(Segments(t, l => Color.Empty, Color.Empty)),
                                   Columns(TemperatureSegments(t, l => Color.Empty, Color.Empty)));
            return columns * Advance(g, font);
        }

        static void PaintRow(Graphics g, Font font, List<KeyValuePair<string, Color>> segments, float x, float y, float advance)
        {
            int column = 0;
            foreach (var s in segments)
            {
                using (var b = new SolidBrush(s.Value)) g.DrawString(s.Key, font, b, x + column * advance, y);
                column += s.Key.Length + 2;
            }
        }

        public static void Paint(Graphics g, Font font, Telemetry t, float x, float width, Func<Level, Color> color, Color dim)
        {
            float advance = Advance(g, font);
            GraphicsState saved = g.Save();
            g.SetClip(Bounds(x, width), CombineMode.Intersect);
            PaintRow(g, font, TemperatureSegments(t, color, dim), x, Top, advance);
            PaintRow(g, font, Segments(t, color, dim), x, Top + RowHeight, advance);
            g.Restore(saved);
        }
    }
}
