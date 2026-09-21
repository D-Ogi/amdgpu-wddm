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
                string dataDir = argv.Length > 0 ? argv[0] : @"C:\BC250\mon";
                var state = new State(dataDir);
                Application.ThreadException += (s, e) => state.Log("ui", Level.Error, e.Exception.ToString());
                AppDomain.CurrentDomain.UnhandledException += (s, e) => state.Log("fatal", Level.Error, Convert.ToString(e.ExceptionObject));

                var driver = new Driver();
                var actions = new Actions(state, driver);
                try { new Api(state, actions).Start(); state.Log("api", Level.Info, "listening on " + Api.Prefix); }
                catch (Exception e) { state.Log("api", Level.Error, "API not started: " + e.Message); }
                ProviderHost.Start(state, new GpuProvider(driver), new SystemProvider());

                Application.EnableVisualStyles();
                Application.Run(new OverlayForm(state, actions));
            }
        }
    }
}
