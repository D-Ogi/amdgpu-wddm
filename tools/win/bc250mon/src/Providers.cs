// Data providers: each one owns a panel and refreshes it on its own period. To add a data source (our
// future KMD's debug channel, ETW, TDR events ...) implement IProvider and register it in Program.cs.
using System;
using System.Diagnostics;
using System.Linq;
using System.Net.NetworkInformation;
using System.Net.Sockets;
using System.Threading;

namespace Bc250Mon
{
    public interface IProvider
    {
        string Name { get; }
        TimeSpan Period { get; }
        void Poll(State state);
    }

    public sealed class GpuProvider : IProvider
    {
        readonly Driver _driver;
        Level _lastTempLevel = Level.Info;
        string _lastError;
        public GpuProvider(Driver driver) { _driver = driver; }
        public string Name { get { return "gpu"; } }
        public TimeSpan Period { get { return TimeSpan.FromSeconds(2); } }

        public const double WarnC = 87, ErrorC = 92;     // amber at the lab's stop limit (87 C since 2026-10-01, was 85)

        public void Poll(State state)
        {
            var p = new Panel { Name = Name, Title = "SoC / GPU", Order = 10 };
            try
            {
                ClockSnapshot sample = _driver.ReadClock();
                double t = sample.TemperatureMc / 1000.0;
                uint mhz = sample.ObservedMHz, vid = sample.ObservedVid;
                p.Rows.Add(new Row("Control", "KMD / serialized SMU"));
                Level lvl = t >= ErrorC ? Level.Error : t >= WarnC ? Level.Warn : Level.Good;
                p.Rows.Add(new Row("Tctl", t.ToString("0.0") + " C", lvl));
                p.Rows.Add(new Row("GFX clock", mhz + " MHz"));
                p.Rows.Add(new Row("GFX voltage", Driver.MillivoltsFromVid(vid).ToString("0") + " mV (vid " + vid + ")"));
                AddFanRow(p);
                if (lvl != _lastTempLevel && (lvl > Level.Good || _lastTempLevel > Level.Good))
                    state.Log(Name, lvl, "temperature " + t.ToString("0.0") + " C");
                _lastTempLevel = lvl;
                _lastError = null;
            }
            catch (Exception e)
            {
                p.Rows.Add(new Row("driver", e.Message, Level.Error));
                if (_lastError != e.Message) state.Log(Name, Level.Error, e.Message);
                _lastError = e.Message;
            }
            state.SetPanel(p);
        }

        // The case fan, from the board's own hardware monitor (docs/design/fan.md). Its own try/catch: the fan is
        // information, and an old control DLL or a closed gate must not cost the panel its temperature row. The
        // driver still does not turn the fan - the BIOS curve does, and the owner keeps that setting as it is.
        void AddFanRow(Panel p)
        {
            try
            {
                HwmonSnapshot f = _driver.ReadHwmon();
                if (Hwmon.Has(f, HwmonSnapshot.FlagGated)) { p.Rows.Add(new Row("Fan", "not read (gate closed)")); return; }
                if (!Hwmon.Reading(f))
                {
                    p.Rows.Add(new Row("Fan", "no reading (reason " + f.Reason + ", age " + f.AgeMs + " ms)", Level.Warn));
                    return;
                }
                if (Hwmon.Has(f, HwmonSnapshot.FlagStopped))
                {
                    p.Rows.Add(new Row("Fan", "not turning, duty " + Hwmon.DutyPercent(f) + " %", Level.Error));
                    return;
                }
                string duty = Hwmon.Has(f, HwmonSnapshot.FlagDutyProven) ? " (" + Hwmon.DutyPercent(f) + " %)" : "";
                p.Rows.Add(new Row("Fan", Hwmon.Rpm(f) + " rpm" + duty));
                double? apu = Hwmon.ApuC(f);
                // The chip reads the APU die over SB-TSI, independently of the SMU. Two readings that disagree
                // are worth seeing side by side, which is why this row says where its number came from.
                if (apu.HasValue) p.Rows.Add(new Row("Tctl (board EC)", apu.Value.ToString("0.0") + " C"));
            }
            catch (Exception e) { p.Rows.Add(new Row("Fan", e.Message, Level.Warn)); }
        }
    }

    public sealed class SystemProvider : IProvider
    {
        SystemCpuTimes? _cpuPrevious;
        public string Name { get { return "system"; } }
        public TimeSpan Period { get { return TimeSpan.FromSeconds(2); } }

        public void Poll(State state)
        {
            var p = new Panel { Name = Name, Title = "System", Order = 20 };
            try
            {
                var current = SystemMetrics.ReadCpu();
                double? busy = _cpuPrevious.HasValue ? SystemMetrics.CpuPercent(_cpuPrevious.Value, current) : null;
                _cpuPrevious = current;
                // GetSystemTimes covers the calling thread's group on machines with more than one group.
                string label = SystemMetrics.GetActiveProcessorGroupCount() > 1 ? "CPU (processor group)" : "CPU";
                p.Rows.Add(new Row(label, busy.HasValue ? busy.Value.ToString("0") + " %" : "waiting for next sample"));
            }
            catch (Exception e) { _cpuPrevious = null; p.Rows.Add(new Row("CPU", "unavailable: " + e.Message, Level.Warn)); }
            try { p.Rows.Add(new Row("Free memory", SystemMetrics.AvailableMemoryMiB() + " MB")); }
            catch (Exception e) { p.Rows.Add(new Row("Free memory", "unavailable: " + e.Message, Level.Warn)); }
            var ips = NetworkInterface.GetAllNetworkInterfaces()
                .Where(n => n.OperationalStatus == OperationalStatus.Up && n.NetworkInterfaceType != NetworkInterfaceType.Loopback)
                .SelectMany(n => n.GetIPProperties().UnicastAddresses)
                .Where(a => a.Address.AddressFamily == AddressFamily.InterNetwork)
                .Select(a => a.Address.ToString());
            p.Rows.Add(new Row("Address", string.Join(", ", ips)));
            p.Rows.Add(new Row("Uptime", TimeSpan.FromMilliseconds(Environment.TickCount & int.MaxValue).ToString(@"d\.hh\:mm\:ss")));
            state.SetPanel(p);
        }
    }

    public sealed class ProviderHost
    {
        public static void Start(State state, params IProvider[] providers)
        {
            foreach (var provider in providers)
            {
                var p = provider;
                var t = new Thread(() =>
                {
                    while (true)
                    {
                        try { p.Poll(state); }
                        catch (Exception e) { state.Log(p.Name, Level.Error, "provider failed: " + e.Message); }
                        Thread.Sleep(p.Period);
                    }
                }) { IsBackground = true, Name = "provider:" + p.Name };
                t.Start();
            }
        }
    }
}
