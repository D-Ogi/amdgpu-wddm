// The Performance page's readings (WU-038, WU-041, F-SENS): only Level-One escapes, and "No reading" wherever the
// driver gives no value, never a guessed one. Power comes from KMD 0.7.215 on (the SMU metrics table, RUN_DPM ABI 3);
// an older driver has none, so that row says "No reading" there. Voltage goes only into the support report.
//
// The fan row became a real reading with KMD 0.7.213.1, from the board's own hardware monitor (docs/design/fan.md).
// The board still turns the fan from its BIOS curve; the driver only reads it. Two rules this row keeps: a reading
// needs the reader online AND the sample fresh, and the duty percentage appears only where a lab trial has proved
// the read-back (HwmonDutyProven), because at E01 the chip reported 96 % duty while the fan turned at about half
// of this board's measured full speed.
using System;
using System.Collections.Generic;
using System.Globalization;

namespace AmdgpuWddmControl
{
    public sealed class SensorRow
    {
        public string Id, Label, Value, Explain;
        public bool NoReading;
        public string Level;      // null, "warn" or "hot"
    }

    public static class Sensors
    {
        public static readonly string[] Ids = { "load", "temperature", "clock", "memory", "power", "fan" };

        // G-PERF (WU-078): the 2 s poll runs only while the window is visible, not minimised, and on a page with live
        // values (Home, Performance). A hidden or minimised window asks the driver nothing.
        public static bool PollWanted(bool visible, bool minimized, string page)
        {
            return visible && !minimized && (page == "home" || page == "performance");
        }

        // The graphics page has live values only in its tuning cards: the countdown of a trial, the clock, voltage and
        // temperature of the curve card. The window passes true while those cards are open or a trial runs; a closed
        // page polls nothing (G-PERF: no escape every two seconds for a reading nobody looks at).
        public static bool PollWanted(bool visible, bool minimized, string page, bool tuningLive)
        {
            return PollWanted(visible, minimized, page) || (visible && !minimized && page == "graphics" && tuningLive);
        }

        public static List<SensorRow> Rows(DpmState d, VideoMemoryState vram, HwmonState fan) { return Rows(d, vram, fan, null); }

        // The fan speed in words, one source for the "Now" card and the Case fan card, so the two never disagree. The
        // reader's own sample comes first (it alone carries the proven duty and the stopped fan). The fan control's
        // reply carries the same reader's RPM, so a window that has only that one (a poll that missed the reader, a
        // fixture) still shows the speed. Null: no reading. level: null or "hot".
        public static string FanValue(HwmonState fan, FanState control, out string level)
        {
            level = null;
            if (fan != null && fan.Reading)
            {
                if (fan.Has(HwmonState.FlagStopped)) { level = "hot"; return Strings.T("perf.fan.stopped"); }
                if (fan.Has(HwmonState.FlagDutyProven)) return Strings.T("perf.fan.value", fan.FastestRpm, fan.DutyPercent);
                return Strings.T("perf.fan.rpm-only", fan.FastestRpm);
            }
            if (control != null && control.Rpm != 0) return Strings.T("perf.fan.rpm-only", control.Rpm);
            return null;
        }

        public static List<SensorRow> Rows(DpmState d, VideoMemoryState vram, HwmonState fan, FanState control)
        {
            var rows = new List<SensorRow>();
            Func<string, string, SensorRow> add = (id, value) =>
            {
                var r = new SensorRow
                {
                    Id = id, Label = Strings.T("perf." + id), Explain = Strings.T("perf." + id + ".explain"),
                    Value = value ?? Strings.T("perf.no-reading"), NoReading = value == null,
                };
                rows.Add(r);
                return r;
            };
            add("load", d != null && d.Has(DpmState.FlagHwBusy) ? (d.BusyAvgPermille / 10.0).ToString("0", CultureInfo.InvariantCulture) + " %" : null);
            var t = add("temperature", d != null && d.Has(DpmState.FlagTemperature) ? (d.TemperatureMc / 1000.0).ToString("0", CultureInfo.InvariantCulture) + " °C" : null);
            if (d != null && d.Has(DpmState.FlagTemperature)) t.Level = d.TemperatureMc >= 87000 ? "hot" : d.TemperatureMc >= 80000 ? "warn" : null;
            uint mhz = d == null ? 0 : d.Has(DpmState.FlagClock) && d.ObservedMHz != 0 ? d.ObservedMHz : d.CurrentMHz;
            add("clock", mhz != 0 ? mhz + " MHz" : null);
            ulong total = vram == null ? 0 : vram.Dedicated != 0 ? vram.Dedicated : vram.LocalLimit;
            add("memory", vram != null && total != 0 && total != ulong.MaxValue ? Strings.T("perf.memory.value", Gib(vram.LocalResident), Gib(total)) : null);
            // The chip's own power figure from the SMU metrics table (KMD 0.7.215, RUN_DPM ABI 3). The driver sets the
            // flag only for a table at most three seconds old; an older driver, EnableSmuMetrics 0 or a refusal by the
            // firmware leave it clear, and the row then says "No reading".
            add("power", d != null && d.Has(DpmState.FlagPower) ? (d.SocketPowerMw / 1000.0).ToString("0", CultureInfo.InvariantCulture) + " W" : null);
            // A stopped fan (a duty output runs and nothing turns) is "hot": the owner must see it at a glance.
            string level;
            add("fan", FanValue(fan, control, out level)).Level = level;
            return rows;
        }

        static string Gib(ulong bytes) { return (bytes / (double)(1UL << 30)).ToString("0.0", CultureInfo.InvariantCulture); }
    }
}
