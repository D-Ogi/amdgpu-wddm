// Named actions. The overlay's buttons, the hotkeys and the HTTP API all go through this table, so a new
// control added here is reachable from everywhere at once.
using System;
using System.Collections.Generic;

namespace Bc250Mon
{
    public sealed class ActionInfo
    {
        public string Name, Label;
        public bool ShowButton;
        public Func<IDictionary<string, object>, string> Run;   // returns a message for the log
    }

    public sealed class Actions
    {
        readonly Dictionary<string, ActionInfo> _actions = new Dictionary<string, ActionInfo>(StringComparer.OrdinalIgnoreCase);
        readonly State _state;
        public event Action<string> UiRequest;     // "show", "hide", "interactive", "passive"

        public Actions(State state, Driver driver, KmdRegistry kmd)
        {
            _state = state;
            Add("stop.set", "STOP tests", true, a => { state.StopRequested = true; return null; });
            Add("stop.clear", "Clear stop", true, a => { state.StopRequested = false; return null; });
            Add("clock.cool", "1000 MHz / 820 mV", true, a => SetClock(driver, 1000, 820));
            Add("clock.set", "Set clock", false, a => SetClock(driver, Convert.ToUInt32(a["mhz"]), Convert.ToUInt32(a["mv"])));
            // The miniport's start budget (ADR 0006 point 3). KmdProvider clears it by itself once the desktop
            // has been up long enough; these two are for driving it by hand from mon.py during an install.
            Add("kmd.confirm", "Confirm KMD start", false, a => { kmd.Confirm(); return "bc250kmd start confirmed by hand: UnconfirmedStarts = 0"; });
            Add("kmd.budget", "KMD start budget", false, a => kmd.Summary());
            Add("overlay.hide", "Hide", true, a => { Ui("hide"); return null; });
            Add("overlay.show", "Show", false, a => { Ui("show"); return null; });
            Add("overlay.interactive", "Controls", false, a => { Ui("interactive"); return null; });
            Add("overlay.passive", "Click-through", true, a => { Ui("passive"); return null; });
        }

        static string SetClock(Driver driver, uint mhz, uint mv)
        {
            // KMD owns ordering, limits, temperature and readback for the whole transaction.
            driver.SetClock(mhz, mv);
            return "clock " + mhz + " MHz, " + mv + " mV";
        }

        void Add(string name, string label, bool button, Func<IDictionary<string, object>, string> run)
        {
            _actions[name] = new ActionInfo { Name = name, Label = label, ShowButton = button, Run = run };
        }

        void Ui(string what) { var h = UiRequest; if (h != null) h(what); }

        public IEnumerable<ActionInfo> All { get { return _actions.Values; } }

        public bool Invoke(string name, IDictionary<string, object> args, string origin, out string error)
        {
            error = null;
            ActionInfo a;
            if (!_actions.TryGetValue(name, out a)) { error = "unknown action " + name; return false; }
            try
            {
                string msg = a.Run(args ?? new Dictionary<string, object>());
                if (msg != null) _state.Log("action", Level.Info, msg + " (" + origin + ")");
                return true;
            }
            catch (Exception e)
            {
                error = e.Message;
                _state.Log("action", Level.Error, name + " failed: " + e.Message + " (" + origin + ")");
                return false;
            }
        }
    }
}
