// One window of Nagi Control App per person. The first start of the window owns a named mutex in the namespace of its own
// session (Local\), so a person who is signed in twice through fast user switching still has one window in each
// session. A later start finds the mutex taken, sets the named event of the first start and ends without a window.
// The first start waits on that event and brings its window to the front: the loading window while it still starts,
// the main window after that, restored when it is minimised (Launch.cs). Only the window path claims the mutex: the
// elevated helper (--action), the notice after sign-in, the recovery view and the headless gates never do.
//
// The event is made before the mutex. A later start that sets the event and ends at once would otherwise destroy
// it together with its signal when the first start had not opened it yet; made first, the event always has the
// first start's handle on it by the time anybody can see the mutex. A signal that comes before the first start
// listens stays on the event (it resets only when a wait takes it), so a start during the loading window is never
// lost.
//
// Both objects carry an access list of their own: SYSTEM, the administrators and the person's own account have full
// access (a start that finds an object already made opens it with full access, so less would refuse every later
// start). A copy started with "Run as administrator" would otherwise make them with the default list of an elevated
// token, which names the administrators only, and a later normal start by the same person could neither see that the
// window is open nor ask it to come forward.
using System;
using System.Security.AccessControl;
using System.Security.Principal;
using System.Threading;

namespace AmdgpuWddmControl
{
    public sealed class SingleInstance : IDisposable
    {
        public const string WindowName = "amdgpu-wddm-control-window";

        readonly Mutex _mutex;
        readonly EventWaitHandle _activate;
        RegisteredWaitHandle _wait;

        // True for the start that owns the window; false for a later start, which must end without one.
        public bool First { get; private set; }

        // The kernel names of the two objects, for the tests and the support view.
        public string MutexName { get; private set; }
        public string EventName { get; private set; }

        SingleInstance(string name)
        {
            MutexName = @"Local\" + name;
            EventName = @"Local\" + name + "-activate";
            _activate = OpenEvent(EventName);
            bool created = false;
            try
            {
                var security = new MutexSecurity();
                foreach (var sid in Allowed())
                    security.AddAccessRule(new MutexAccessRule(sid, MutexRights.FullControl, AccessControlType.Allow));
                _mutex = new Mutex(false, MutexName, out created, security);
            }
            // The mutex is there and this account may not open it: a window of somebody this account cannot reach
            // is open in this session. That is still an open window, so this start does not open another one.
            catch (UnauthorizedAccessException) { _mutex = null; created = false; }
            First = created;
        }

        // Claims the window for this process, or finds that another start has it.
        public static SingleInstance Claim(string name = WindowName) { return new SingleInstance(name); }

        static SecurityIdentifier[] Allowed()
        {
            var list = new System.Collections.Generic.List<SecurityIdentifier>
            {
                new SecurityIdentifier(WellKnownSidType.LocalSystemSid, null),
                new SecurityIdentifier(WellKnownSidType.BuiltinAdministratorsSid, null),
            };
            using (var me = WindowsIdentity.GetCurrent()) if (me.User != null) list.Add(me.User);
            return list.ToArray();
        }

        static EventWaitHandle OpenEvent(string name)
        {
            try
            {
                var security = new EventWaitHandleSecurity();
                foreach (var sid in Allowed())
                    security.AddAccessRule(new EventWaitHandleAccessRule(sid, EventWaitHandleRights.FullControl, AccessControlType.Allow));
                bool created;
                return new EventWaitHandle(false, EventResetMode.AutoReset, name, out created, security);
            }
            catch (UnauthorizedAccessException) { return null; }
            catch (WaitHandleCannotBeOpenedException) { return null; }
        }

        // A later start asks the first one to come forward. False when the first one cannot be reached; this start
        // ends without a window either way.
        public bool Signal()
        {
            if (_activate == null) return false;
            try { return _activate.Set(); }
            catch (Exception) { return false; }
        }

        // The first start: onActivate runs on a thread-pool thread once for every signal, including a signal that
        // came before this call. The caller moves it to the thread of its windows.
        public void Listen(Action onActivate)
        {
            if (!First || _activate == null || _wait != null || onActivate == null) return;
            _wait = ThreadPool.RegisterWaitForSingleObject(_activate, (state, timedOut) => { if (!timedOut) onActivate(); }, null, Timeout.Infinite, false);
        }

        public void Dispose()
        {
            if (_wait != null) { _wait.Unregister(null); _wait = null; }
            if (_activate != null) _activate.Dispose();
            if (_mutex != null) _mutex.Dispose();
        }
    }
}
