// The Performance page's readings (WU-038, WU-041, F-SENS): only Level-One escapes, and "No reading" wherever the
// driver gives no value, never a guessed one. The current driver reports no power and no fan reading: those rows
// always say "No reading". Voltage goes only into the support report.
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

        // The graphics page has one live value: the countdown of a tuning trial, which the driver keeps. Without a
        // trial that page polls nothing (G-PERF: no escape every two seconds for a reading nobody looks at).
        public static bool PollWanted(bool visible, bool minimized, string page, bool tuningTrial)
        {
            return PollWanted(visible, minimized, page) || (visible && !minimized && page == "graphics" && tuningTrial);
        }

        public static List<SensorRow> Rows(DpmState d, VideoMemoryState vram)
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
            add("power", null);
            add("fan", null);
            return rows;
        }

        static string Gib(ulong bytes) { return (bytes / (double)(1UL << 30)).ToString("0.0", CultureInfo.InvariantCulture); }
    }
}
