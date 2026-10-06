// bc250mon - monitor and overlay for the BC-250 lab machine. Composition root: wires state, driver, actions,
// providers, API and UI together. Must run elevated in the interactive session (bc250rd is admin-only).
using System;
using System.Threading;
using System.Windows.Forms;

namespace Bc250Mon
{
    static class Program
    {
        [STAThread]
        static void Main(string[] argv)
        {
            bool first;
            using (new Mutex(true, @"Global\bc250mon", out first))
            {
                if (!first) return;
                // bc250mon [data directory] [--kmd-key HKLM\...|HKCU\...]
                string dataDir = @"C:\BC250\mon", kmdKey = Environment.GetEnvironmentVariable("BC250MON_KMD_KEY");
                for (int i = 0; i < argv.Length; i++)
                {
                    if (argv[i] == "--kmd-key" && i + 1 < argv.Length) kmdKey = argv[++i];
                    else if (!argv[i].StartsWith("--")) dataDir = argv[i];
                }
                var state = new State(dataDir);
                Application.ThreadException += (s, e) => state.Log("ui", Level.Error, e.Exception.ToString());
                AppDomain.CurrentDomain.UnhandledException += (s, e) => state.Log("fatal", Level.Error, Convert.ToString(e.ExceptionObject));

                var driver = new Driver();
                // --kmd-key (or BC250MON_KMD_KEY) points the KMD provider at another registry key: a debug switch
                // that lets the "installed" paths be exercised where no such service exists (see KmdProvider).
                var kmd = new KmdRegistry(kmdKey);
                // The pipeline provider is built before the action table, because "graphics.summary" asks it for
                // one KMD log summary (nothing asks for one on a schedule any more, C55).
                var pipeline = new GraphicsPipelineProvider(dataDir);
                var actions = new Actions(state, driver, kmd, pipeline);
                try { new Api(state, actions).Start(); state.Log("api", Level.Info, "listening on " + Api.Prefix); }
                catch (Exception e) { state.Log("api", Level.Error, "API not started: " + e.Message); }
                // One DPM snapshot serves two panels: TelemetryProvider takes it anyway, the operating-point
                // and measurement-guard panels read the last one instead of sending escapes of their own.
                var dpmFeed = new DpmFeed();
                ProviderHost.Start(state, new GpuProvider(driver), new SystemProvider(), new KmdProvider(kmd, dataDir),
                                   new KmdInfoProvider(driver), pipeline, new GraphicsApiProvider(dataDir),
                                   new VulkanInventoryProvider(dataDir), new TelemetryProvider(driver, dpmFeed),
                                   new OperatingPointProvider(driver, dpmFeed, dataDir),
                                   new MeasurementGuardProvider(dataDir, dpmFeed));

                Application.EnableVisualStyles();
                Application.Run(new OverlayForm(state, actions));
            }
        }
    }
}
