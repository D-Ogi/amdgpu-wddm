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

        public const double WarnC = 85, ErrorC = 92;

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
    }

    public sealed class SystemProvider : IProvider
    {
        readonly PerformanceCounter _cpu = new PerformanceCounter("Processor", "% Processor Time", "_Total");
        readonly PerformanceCounter _mem = new PerformanceCounter("Memory", "Available MBytes");
        public string Name { get { return "system"; } }
        public TimeSpan Period { get { return TimeSpan.FromSeconds(2); } }

        public void Poll(State state)
        {
            var p = new Panel { Name = Name, Title = "System", Order = 20 };
            p.Rows.Add(new Row("CPU", _cpu.NextValue().ToString("0") + " %"));
            p.Rows.Add(new Row("Free memory", _mem.NextValue().ToString("0") + " MB"));
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
