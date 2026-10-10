// Host tests of the tuning cards' views (src/TunerView.cs) against a fake driver.
//
// FakeTunerDriver keeps the trial rules of docs/design/tuner.md the way the driver does (driver/shim/bc250_dpm.c for
// the curve, driver/kmd/cpu.c for the processor): a set starts a trial, the governor applies it at its next tick, a
// keep stores what ran, a cancel and the end of the window put the stored setting back, a reset removes it, and
// processor tuning is read once at the driver's start. The window's plans (TunerPlan via Recovery.Plan) run against
// it the way the elevated helper runs them, so each scenario walks the same path a person does: the card shows a
// state, a button is enabled, its plan stands, the fake driver answers, the card shows the next state.
//
// One rule is checked at every step of every scenario: a button is enabled exactly when the plan behind it stands.
// An enabled button that the plan refuses is a dead end for a person; a disabled button whose plan stands hides an
// action the driver would take.
using System;
using System.Collections.Generic;
using System.Linq;
using AmdgpuWddmControl;

sealed class FakeTunerDriver
{
    public readonly RecoverySnapshot Snap;
    public ulong Generation = 5;
    public bool Governing = true, Busy, RevertOwed;
    public uint LevelMHz = 1500;
    public int TemperatureMc = 62000;

    // The curve: the stored one (null: none), the active one, the trial.
    uint[] _stored, _active = Tuner.Table(), _candidate;
    bool _curveTrial, _curveApplied;
    uint _curveLeft, _curveWindow, _serial, _sets, _keeps, _cancels, _reverts;

    // The processor: what was sent, what is stored, what this start recorded before its first change.
    bool _tuneOn, _proven, _cpuTrial, _cpuStored;
    uint _aMax, _aUv, _aTemp, _sMax, _sUv, _sTemp, _bMax, _bUv, _bTemp, _tbMax, _tbUv, _tbTemp;
    uint _cpuLeft, _cpuWindow, _cpuSerial, _cpuReverts, _reads, _writes;
    public uint CoreMaskStored;

    public FakeTunerDriver(bool cpuTuneStored)
    {
        Snap = UnitTests.TunedSnapshot();
        if (!cpuTuneStored) Snap.Parameters.Remove("CpuTune");
        Start();
    }

    // A start of the driver: the stored settings run, processor tuning follows the value read now, no readback yet.
    public void Restart()
    {
        Generation++;
        Start();
    }

    void Start()
    {
        _active = _stored != null ? (uint[])_stored.Clone() : Tuner.Table();
        _curveTrial = false; _candidate = null;
        long tune;
        _tuneOn = Snap.Parameters.TryGetValue("CpuTune", out tune) && tune == 1;
        _proven = false; _cpuTrial = false;
        _aMax = _sMax; _aUv = _sUv; _aTemp = _sTemp;
        _bMax = _bUv = _bTemp = 0;
        Snap.Health.Generation = Generation;
    }

    public RecoverySnapshot Read()
    {
        Snap.Curve = Curve();
        Snap.Cpu = Cpu();
        return Snap;
    }

    CurveState Curve()
    {
        uint flags = CurveState.FlagValid;
        if (Governing) flags |= CurveState.FlagGoverning;
        if (_curveTrial) flags |= CurveState.FlagOnTrial;
        if (_curveTrial && _curveApplied) flags |= CurveState.FlagApplied;
        if (_stored != null) flags |= CurveState.FlagStored;
        if (Tuner.IsDefault(_active, Tuner.Table())) flags |= CurveState.FlagDefault;
        int level = TunerView.IndexOf(LevelMHz);
        return new CurveState
        {
            Version = 0x000700D5, Flags = flags, TrialMs = _curveWindow, TrialRemainingMs = _curveTrial ? _curveLeft : 0, Serial = _serial,
            FirstMHz = Tuner.FirstMHz, StepMHz = Tuner.StepMHz, Points = (uint)Tuner.Points,
            Candidate = _curveTrial ? (uint[])_candidate.Clone() : new uint[Tuner.Points], Active = (uint[])_active.Clone(),
            Stored = _stored != null ? (uint[])_stored.Clone() : new uint[Tuner.Points], Default = Tuner.Table(), Floor = Tuner.Floors(),
            LevelMHz = LevelMHz, LevelMv = level >= 0 ? _active[level] : Tuner.FloorMv, CeilingMHz = 1500, Mode = Governing ? 1u : 0u,
            TemperatureMc = TemperatureMc, Sets = _sets, Keeps = _keeps, Cancels = _cancels, Reverts = _reverts, Generation = Generation,
        };
    }

    CpuState Cpu()
    {
        uint flags = CpuState.FlagValid;
        if (_tuneOn) flags |= CpuState.FlagTuneOn;
        if (_proven) flags |= CpuState.FlagQueue3Proven | CpuState.FlagTempValid;
        if (_cpuTrial) flags |= CpuState.FlagOnTrial;
        if (_cpuStored) flags |= CpuState.FlagStored;
        if (Busy) flags |= CpuState.FlagBusy;
        if (RevertOwed) flags |= CpuState.FlagRevertOwed;
        return new CpuState
        {
            Version = 0x000700D5, Flags = flags, TrialMs = _cpuWindow, TrialRemainingMs = _cpuTrial ? _cpuLeft : 0, Serial = _cpuSerial,
            AppliedMaxMHz = _aMax, AppliedUvSteps = _aUv, AppliedTempC = _aTemp, StoredMaxMHz = _sMax, StoredUvSteps = _sUv, StoredTempC = _sTemp,
            BaselineMaxMHz = _bMax, BaselineUvSteps = _bUv, BaselineTempC = _bTemp,
            VoltageMv = _proven ? 1050u - 3 * _aUv : 0, CapC = _proven ? (_aTemp != 0 ? _aTemp : 95) : 0, TemperatureMc = _proven ? 58000 : 0,
            CoreMHz = _proven ? new uint[] { 2790, 2790, 2780, 2790, 2790, 2780, 0, 0 } : new uint[8],
            PstateMHz = _proven ? new uint[] { 3600, 2800, 1600, 0, 0, 0, 0, 0 } : new uint[8],
            Cores = 6, Threads = 12, CoreMask = CpuTuning.MaskStock, CoreMaskStored = CoreMaskStored,
            Reads = _reads, Writes = _writes, Reverts = _cpuReverts, Generation = Generation,
        };
    }

    // The elevated helper's part: the plan's registry writes, then its escapes in order. False when the plan was
    // refused or the driver refused a step, as the helper reports it.
    public bool Run(ActionPlan p)
    {
        if (p.Refused) return false;
        foreach (var w in p.Writes)
        {
            if (w.Delete) Snap.Parameters.Remove(w.Name);
            else Snap.Parameters[w.Name] = w.Number;
        }
        if (p.Tune != null && !Send(p.Tune)) return false;
        foreach (var t in p.TuneSteps) if (!Send(t)) return false;
        return true;
    }

    public bool Send(TuneRequest t)
    {
        int level;
        switch (t.Kind)
        {
            case "curve-trial":
                if (!Governing || Tuner.Check(t.Mv, Tuner.Floors(), out level) != CurveError.Ok) return false;
                _candidate = (uint[])t.Mv.Clone(); _active = (uint[])t.Mv.Clone();
                _curveTrial = true; _curveApplied = false; _curveWindow = t.WindowMs; _curveLeft = t.WindowMs;
                _serial++; _sets++;
                return true;
            case "curve-keep":
                if (!_curveTrial || !_curveApplied) return false;
                _stored = (uint[])_candidate.Clone(); _curveTrial = false; _keeps++;
                return true;
            case "curve-stop":
                if (!_curveTrial) return false;
                CurveRevert(); _cancels++;
                return true;
            case "curve-reset":
                if (_curveTrial) _cancels++;
                _curveTrial = false; _stored = null; _active = Tuner.Table(); _serial++;
                return true;
            case "cpu-readback":
                if (!_tuneOn || Busy) return false;
                _proven = true; _reads++;
                if (_bMax == 0) { _bMax = 3600; _bUv = 0; _bTemp = 95; }
                return true;
            case "cpu-trial":
                if (!_tuneOn || !_proven || Busy) return false;
                if (!_cpuTrial) { _tbMax = _aMax; _tbUv = _aUv; _tbTemp = _aTemp; }
                if (t.MaxMHz != null) _aMax = t.MaxMHz.Value;
                if (t.UvSteps != null) _aUv = t.UvSteps.Value;
                if (t.TempC != null) _aTemp = t.TempC.Value;
                _cpuTrial = true; _cpuWindow = t.WindowMs; _cpuLeft = t.WindowMs; _cpuSerial++; _writes++;
                return true;
            case "cpu-keep":
                if (!_cpuTrial) return false;
                _sMax = _aMax; _sUv = _aUv; _sTemp = _aTemp; _cpuStored = true; _cpuTrial = false; _cpuSerial++;
                return true;
            case "cpu-stop":
                if (!_cpuTrial) return false;
                CpuRevert();
                return true;
            case "cpu-reset":
                if (!_proven) return false;
                _aMax = _aUv = _aTemp = 0; _sMax = _sUv = _sTemp = 0; _cpuStored = false; _cpuTrial = false; _cpuSerial++;
                return true;
            case "core-mask":
                if (!_proven) return false;
                CoreMaskStored = t.CoreMask;
                return true;
            default:
                return false;
        }
    }

    void CurveRevert()
    {
        _active = _stored != null ? (uint[])_stored.Clone() : Tuner.Table();
        _curveTrial = false; _serial++;
    }

    void CpuRevert()
    {
        _aMax = _tbMax; _aUv = _tbUv; _aTemp = _tbTemp; _cpuTrial = false; _cpuSerial++; _cpuReverts++;
    }

    // Time passes: the governor applies a candidate at its next tick, and a window that runs out reverts.
    public void Tick(uint ms)
    {
        if (_curveTrial)
        {
            _curveApplied = true;
            if (ms >= _curveLeft) { CurveRevert(); _reverts++; }
            else _curveLeft -= ms;
        }
        if (_cpuTrial)
        {
            if (ms >= _cpuLeft) CpuRevert();
            else _cpuLeft -= ms;
        }
    }
}

static partial class UnitTests
{
    public static RecoverySnapshot TunedSnapshot() { return Tuned(); }

    static void TunerViewTests()
    {
        Strings.Language = "en";
        CurveKept();
        CurveExpiredStoppedReset();
        CurveLimits();
        CpuOffToReady();
        CpuTrials();
        CpuLimitsAndStates();
        TuningSummaryAndPoll();
    }

    static CurveView CurveOf(FakeTunerDriver d, TrialWatch w, uint[] edit = null, uint? lastReason = null)
    {
        var s = d.Read();
        w.Observe(s.Curve, s.Cpu);
        return TunerView.Curve(s.Curve, edit, true, lastReason, w.Curve);
    }

    static CpuView CpuOf(FakeTunerDriver d, TrialWatch w, uint? clock = null, uint? uv = null, uint? temp = null, uint? lastReason = null)
    {
        var s = d.Read();
        w.Observe(s.Curve, s.Cpu);
        return TunerView.Cpu(s.Cpu, true, s.P("CpuTune"), lastReason, clock, uv, temp, w.Cpu);
    }

    // A button is enabled exactly when the plan behind it stands.
    static void CurveAgree(FakeTunerDriver d, CurveView v, string what)
    {
        var s = d.Read();
        Equal(v.ApplyEnabled, !TunePlan("tune-trial", s, TunerView.CurveRequest(v)).Refused, what + ": Apply and its plan agree");
        Equal(v.KeepEnabled, !TunePlan("tune-keep", s).Refused, what + ": Keep and its plan agree");
        Equal(v.StopEnabled, !TunePlan("tune-stop", s).Refused, what + ": Stop and its plan agree");
        Equal(v.ResetEnabled, !TunePlan("tune-reset", s).Refused, what + ": Standard voltage and its plan agree");
    }

    static void CpuAgree(FakeTunerDriver d, CpuView v, uint? clock, uint? uv, uint? temp, string what)
    {
        var s = d.Read();
        if (v.Card != TuneCard.Ready)
        {
            Check(!v.ApplyEnabled && !v.KeepEnabled && !v.StopEnabled && !v.ResetEnabled && !v.ReadEnabled, what + ": no write button outside the ready card");
            Check(TunePlan("cpu-readback", s).Refused, what + ": the readback plan is refused outside the ready card");
        }
        else
        {
            Equal(v.ReadEnabled, !TunePlan("cpu-readback", s).Refused, what + ": Read and its plan agree");
            Equal(v.ApplyEnabled, !TunePlan("cpu-trial", s, TunerView.CpuRequest(clock, uv, temp)).Refused, what + ": Apply and its plan agree");
            Equal(v.KeepEnabled, !TunePlan("cpu-keep", s).Refused, what + ": Keep and its plan agree");
            Equal(v.StopEnabled, !TunePlan("cpu-stop", s).Refused, what + ": Stop and its plan agree");
            Equal(v.ResetEnabled, !TunePlan("cpu-reset", s).Refused, what + ": Standard settings and its plan agree");
        }
        Equal(v.EnableOffered, !TunePlan("cpu-enable", s).Refused, what + ": Turn on and its plan agree");
        Equal(v.DisableOffered, !TunePlan("cpu-disable", s).Refused, what + ": Turn off and its plan agree");
    }

    // Apply, wait for the governor, Keep: the card walks from the standard curve to a saved one.
    static void CurveKept()
    {
        var d = new FakeTunerDriver(true);
        var w = new TrialWatch();
        var v = CurveOf(d, w);
        Equal(TuneCard.Ready, v.Card, "curve: ready on a governing start");
        Check(!v.ApplyEnabled && !v.KeepEnabled && !v.StopEnabled && !v.ResetEnabled, "curve: nothing to do at the standard line");
        Equal("standard", v.Preset, "curve: the standard preset is chosen");
        Equal("Now: 1500 MHz at 919 mV, 62 °C", v.Live, "curve: the live row names clock, voltage and temperature");
        Equal(5, v.NowIndex, "curve: the ring sits at 1500 MHz");
        Equal(5, v.CeilingIndex, "curve: the dashed line sits at the ceiling");
        Check(v.Saved.Contains(Strings.T("tuner.curve.summary.line")), "curve: nothing saved reads as the standard voltage");
        Equal(Tuner.Points, v.Rows.Count, "curve: one row per speed");
        Check(v.Outcome == null && v.GuardNote == null && v.Waiting == null, "curve: no outcome, guard note or waiting line");
        CurveAgree(d, v, "curve standard");

        var edit = Tuner.Preset("medium", Tuner.Table(), Tuner.Floors());
        v = CurveOf(d, w, edit);
        Check(v.ApplyEnabled && v.Changed, "curve: an edit can be tested");
        Equal("medium", v.Preset, "curve: the edit is the medium preset");
        Check(v.Rows[5].Value.Contains("(now 919 mV)"), "curve: an edited row also shows the voltage in use: " + v.Rows[5].Value);
        Check(v.Rows[0].Value == "820 mV, standard", "curve: the floor row is unchanged: " + v.Rows[0].Value);
        CurveAgree(d, v, "curve edited");

        var plan = TunePlan("tune-trial", d.Read(), TunerView.CurveRequest(v));
        Check(!plan.Refused && plan.Tune != null && plan.Tune.WindowMs == TunerPlan.DefaultWindowMs, "curve: the trial plan stands with the app's window");
        Check(plan.Preview.Any(l => l.Contains("120 s")), "curve: the dialog says when it goes back by itself");
        Check(d.Run(plan), "curve: the fake driver takes the trial");
        v = CurveOf(d, w);
        Check(v.OnTrial && v.StatusWarn && v.Status.Contains("120 s left"), "curve: the countdown is the status: " + v.Status);
        Check(v.Waiting != null && !v.KeepEnabled && v.StopEnabled, "curve: Keep waits for the governor");
        Check(!v.ApplyEnabled, "curve: the curve on trial is the one shown, nothing new to test");
        CurveAgree(d, v, "curve on trial, not applied");

        d.Tick(25);
        v = CurveOf(d, w);
        Check(v.KeepEnabled && v.Waiting == null, "curve: Keep after the governor's tick");
        Equal("Now: 1500 MHz at " + edit[5] + " mV, 62 °C", v.Live, "curve: the live row follows the curve in force");
        CurveAgree(d, v, "curve on trial, applied");

        Check(d.Run(TunePlan("tune-keep", d.Read())), "curve: keep runs");
        v = CurveOf(d, w);
        Equal(TrialEnd.Kept, w.Curve, "curve: the watch names a keep");
        Equal(Strings.T("tuner.end.kept"), v.Outcome, "curve: the card says it is kept");
        Check(v.OutcomeGood && !v.OnTrial, "curve: a keep is good news");
        Check(!v.Saved.Contains(Strings.T("tuner.curve.summary.line")), "curve: the saved line names the own curve: " + v.Saved);
        Check(v.ResetEnabled, "curve: a saved curve can go back to standard");
        CurveAgree(d, v, "curve kept");

        d.Restart();
        v = CurveOf(d, w);
        Equal(TrialEnd.None, w.Curve, "curve: a new start forgets the old end");
        Check(v.Outcome == null && !v.Changed, "curve: the saved curve runs after a restart");
    }

    static void CurveExpiredStoppedReset()
    {
        var d = new FakeTunerDriver(true);
        var w = new TrialWatch();
        CurveOf(d, w);
        var edit = Tuner.Preset("mild", Tuner.Table(), Tuner.Floors());
        Check(d.Run(TunePlan("tune-trial", d.Read(), TunerView.CurveRequest(CurveOf(d, w, edit)))), "expired: trial runs");
        CurveOf(d, w);
        d.Tick(TunerPlan.DefaultWindowMs);
        var v = CurveOf(d, w);
        Equal(TrialEnd.Expired, w.Curve, "expired: the watch names the window that ran out");
        Equal(Strings.T("tuner.end.expired"), v.Outcome, "expired: the card says it went back by itself");
        Check(!v.OutcomeGood && !v.Changed && Tuner.IsDefault(v.Active, Tuner.Table()), "expired: the standard line is back");
        CurveAgree(d, v, "expired");
        v = CurveOf(d, w, edit);
        Check(v.Outcome == null, "expired: an edit hides the old end");

        Check(d.Run(TunePlan("tune-trial", d.Read(), TunerView.CurveRequest(v))), "stopped: trial runs");
        v = CurveOf(d, w);
        CurveAgree(d, v, "stopped: on trial");
        Check(d.Run(TunePlan("tune-stop", d.Read())), "stopped: stop runs");
        v = CurveOf(d, w);
        Equal(TrialEnd.Stopped, w.Curve, "stopped: the watch names the stop");
        Equal(Strings.T("tuner.end.stopped"), v.Outcome, "stopped: the card says the old setting is back");
        CurveAgree(d, v, "stopped");

        // A saved curve, then a trial over it, then Standard voltage during the trial.
        Check(d.Run(TunePlan("tune-trial", d.Read(), TunerView.CurveRequest(CurveOf(d, w, edit)))), "reset: trial runs");
        d.Tick(25);
        Check(d.Run(TunePlan("tune-keep", d.Read())), "reset: keep runs");
        var deeper = Tuner.Preset("deep", Tuner.Table(), Tuner.Floors());
        Check(d.Run(TunePlan("tune-trial", d.Read(), TunerView.CurveRequest(CurveOf(d, w, deeper)))), "reset: second trial runs");
        CurveOf(d, w);
        Check(d.Run(TunePlan("tune-reset", d.Read())), "reset: reset runs");
        v = CurveOf(d, w);
        Equal(TrialEnd.Reset, w.Curve, "reset: the watch names a reset that removed the saved curve");
        Equal(Strings.T("tuner.end.reset.curve"), v.Outcome, "reset: the card says standard is back");
        Check(!v.ResetEnabled, "reset: nothing left to reset");
        CurveAgree(d, v, "reset");

        // A reset outside any trial: only the window knows, and says so through Did.
        Check(d.Run(TunePlan("tune-trial", d.Read(), TunerView.CurveRequest(CurveOf(d, w, edit)))), "reset 2: trial runs");
        d.Tick(25);
        Check(d.Run(TunePlan("tune-keep", d.Read())), "reset 2: keep runs");
        CurveOf(d, w);
        Check(d.Run(TunePlan("tune-reset", d.Read())), "reset 2: reset runs");
        CurveOf(d, w);
        w.Did(false, TrialEnd.Reset);
        Equal(Strings.T("tuner.end.reset.curve"), CurveOf(d, w).Outcome, "reset 2: the window's own reset is named");
    }

    static void CurveLimits()
    {
        var d = new FakeTunerDriver(true);
        var w = new TrialWatch();
        var tooDeep = Tuner.Table();
        tooDeep[5] = Tuner.FloorMv;
        var v = CurveOf(d, w, tooDeep);
        Equal(CurveError.Depth, v.Error, "limits: a voltage under the driver's floor is refused");
        Equal(5, v.ErrorLevel, "limits: at 1500 MHz");
        Check(!v.ApplyEnabled && v.ErrorText != null && v.Rows[5].Warn, "limits: no Apply, a plain message, the row marked");
        CurveAgree(d, v, "limits: too deep");
        Check(!PlainWords.Findings(new[] { v.ErrorText }).Any(), "limits: the message names no internals");

        // Every value the nudge buttons can reach stays inside the driver's band at that speed.
        var line = Tuner.Table(); var floor = Tuner.Floors();
        for (int i = 0; i < Tuner.Points; i++)
            foreach (int delta in new[] { -1000, -5, 5, 1000 })
            {
                uint mv = Tuner.Nudge(line[i], delta, i, line, floor);
                Check(mv >= floor[i] && mv <= line[i] && mv >= Tuner.FloorMv && mv <= Tuner.CeilingMv, "limits: nudge at " + Tuner.MHzAt(i) + " stays in band: " + mv);
            }

        v = CurveOf(d, w, null, 3);
        Equal(Strings.T("tuner.curve.guard.unfinished"), v.GuardNote, "guard: an unconfirmed start is said in plain words");
        v = CurveOf(d, w, null, 2);
        Equal(Strings.T("tuner.curve.guard.refused"), v.GuardNote, "guard: a refused curve is said in plain words");
        Check(CurveOf(d, w, null, 1).GuardNote == null, "guard: a saved curve that runs needs no note");

        d.Governing = false;
        v = CurveOf(d, w);
        Equal(TuneCard.NeedAuto, v.Card, "limits: a start without automatic clocks asks for them");
        Check(TunePlan("tune-trial", d.Read(), new Recovery.PlanArgs { Curve = TunerPlan.CurveText(Tuner.Preset("mild", line, floor)) }).Refused, "limits: and the plan refuses a trial");
        Equal(TuneCard.NotInstalled, TunerView.Curve(null, null, false, null, TrialEnd.None).Card, "limits: no driver installed");
        Equal(TuneCard.NoReading, TunerView.Curve(null, null, true, null, TrialEnd.None).Card, "limits: no reading");

        // A temperature the driver did not read is left out of the live row, never shown as 0.
        d.Governing = true;
        d.TemperatureMc = 0;
        Equal("Now: 1500 MHz at 919 mV", CurveOf(d, w).Live, "live: no temperature without a reading");
        d.LevelMHz = 500;
        v = CurveOf(d, w);
        Check(v.NowIndex == -1, "live: an idle clock under the grid draws no ring");
    }

    // Processor tuning off, turned on (a registry value and a restart), then the read-first order.
    static void CpuOffToReady()
    {
        var d = new FakeTunerDriver(false);
        var w = new TrialWatch();
        var v = CpuOf(d, w);
        Equal(TuneCard.Off, v.Card, "cpu: off by default");
        Check(v.EnableOffered && !v.DisableOffered, "cpu: the card offers to turn it on");
        CpuAgree(d, v, null, null, null, "cpu off");
        var plan = TunePlan("cpu-enable", d.Read());
        Check(!plan.Refused && plan.OfferRestart && plan.Undoable, "cpu: turning on is an undoable write with a restart offer");
        Check(plan.Writes.Count == 1 && plan.Writes[0].Name == "CpuTune" && plan.Writes[0].Number == 1, "cpu: the one write is CpuTune 1");
        Check(d.Run(plan), "cpu: the helper writes it");

        v = CpuOf(d, w);
        Equal(TuneCard.OnAfterRestart, v.Card, "cpu: written but waiting for the restart");
        Equal(Strings.T("tuner.cpu.on-after-restart"), v.Status, "cpu: the card says it turns on after the restart");
        Check(!v.EnableOffered && v.DisableOffered, "cpu: turning on again is not offered, turning off is");
        CpuAgree(d, v, null, null, null, "cpu after enable");

        d.Restart();
        v = CpuOf(d, w);
        Equal(TuneCard.Ready, v.Card, "cpu: on after the restart");
        Check(!v.Proven && v.ReadEnabled && !v.ResetEnabled, "cpu: read first");
        v = CpuOf(d, w, 3000);
        Check(v.Edited && !v.ApplyEnabled, "cpu: an edit cannot be tested before the readback");
        CpuAgree(d, v, 3000, null, null, "cpu: edit before readback");
        Check(v.InForce.Last().Value == Strings.T("tuner.cpu.baseline.none"), "cpu: the standard is not known before a readback");
        Check(v.Readings.All(r => r.Value == Strings.T("perf.no-reading") || r.Key == Strings.T("graphics.cpu.count.label")), "cpu: no reading is guessed before the readback");

        Check(d.Run(TunePlan("cpu-readback", d.Read())), "cpu: the readback runs");
        v = CpuOf(d, w);
        Check(v.Proven, "cpu: proven after one readback");
        var cores = v.Readings.First(r => r.Key == Strings.T("tuner.cpu.cores-now.label")).Value;
        Equal("2790, 2790, 2780, 2790, 2790, 2780 MHz", cores, "cpu: each core's speed, without the cores that did not answer");
        Equal("3600, 2800, 1600 MHz", v.Readings.First(r => r.Key == Strings.T("tuner.cpu.steps-now.label")).Value, "cpu: the speed steps");
        Equal("58 °C", v.Readings.First(r => r.Key == Strings.T("tuner.cpu.temp.label")).Value, "cpu: the temperature");
        Equal("1050 mV", v.Readings.First(r => r.Key == Strings.T("graphics.cpu.voltage.label")).Value, "cpu: the voltage");
        Check(v.InForce.Last().Value.Contains("3600 MHz") && v.InForce.Last().Value.Contains("95 °C"), "cpu: the standard of this start is named: " + v.InForce.Last().Value);
        CpuAgree(d, v, null, null, null, "cpu proven");

        // The pickers offer the driver's lists and nothing else; a baseline above the release's highest limit
        // shows as the nearest allowed value, never as a choice of its own.
        Check(v.ClockChoices.All(CpuTuning.ValidClock) && v.ClockChoices.Length == (CpuTuning.MaxMHz - CpuTuning.MinMHz) / 100 + 1, "cpu: every clock choice is one the driver takes, and all of them");
        Check(v.StepChoices.All(CpuTuning.ValidSteps) && v.StepChoices.Length == CpuTuning.MaxSteps + 1, "cpu: every undervolt choice is one the driver takes");
        Check(v.TempChoices.All(CpuTuning.ValidTemp) && v.TempChoices.Length == CpuTuning.MaxTempC - CpuTuning.MinTempC + 1, "cpu: every temperature choice is one the driver takes");
        Check(v.ClockChoices.Contains(v.Clock) && v.StepChoices.Contains(v.Steps) && v.TempChoices.Contains(v.Temp), "cpu: the pickers start inside their lists");
        Equal(CpuTuning.MaxMHz, v.Clock, "cpu: no limit in force starts the clock picker at the highest allowed limit");
    }

    static void CpuTrials()
    {
        var d = new FakeTunerDriver(true);
        var w = new TrialWatch();
        d.Run(TunePlan("cpu-readback", d.Read()));
        var v = CpuOf(d, w, 3000u, 4u);
        Check(v.ApplyEnabled, "cpu trial: an edit after the readback can be tested");
        CpuAgree(d, v, 3000u, 4u, null, "cpu trial: edited");
        var plan = TunePlan("cpu-trial", d.Read(), TunerView.CpuRequest(3000u, 4u, null));
        Check(!plan.Refused && plan.Tune.MaxMHz == 3000 && plan.Tune.UvSteps == 4 && plan.Tune.TempC == null, "cpu trial: only the stepped values travel");
        Check(plan.Preview.Count >= 3 && plan.PlainNotes.Count == 1, "cpu trial: the dialog lists the values, the way back and the risk");
        Check(d.Run(plan), "cpu trial: runs");
        v = CpuOf(d, w);
        Check(v.OnTrial && v.KeepEnabled && v.StopEnabled && v.Status.Contains("120 s left"), "cpu trial: countdown, Keep and Stop");
        Check(v.InForce[0].Value.Contains("3000 MHz") && v.InForce[0].Value.Contains("4 steps lower"), "cpu trial: the values in force: " + v.InForce[0].Value);
        CpuAgree(d, v, null, null, null, "cpu trial: on trial");

        d.Tick(TunerPlan.DefaultWindowMs);
        v = CpuOf(d, w);
        Equal(TrialEnd.Expired, w.Cpu, "cpu trial: the window ran out");
        Equal(Strings.T("tuner.end.expired"), v.Outcome, "cpu trial: the card says it went back by itself");
        Check(v.InForce[0].Value.Contains(Strings.T("tuner.cpu.own")) && v.InForce[0].Value.Contains(Strings.T("tuner.cpu.uv.none")), "cpu trial: the old values are back: " + v.InForce[0].Value);
        CpuAgree(d, v, null, null, null, "cpu trial: expired");

        Check(d.Run(TunePlan("cpu-trial", d.Read(), TunerView.CpuRequest(null, null, 90u))), "cpu stop: trial runs");
        CpuOf(d, w);
        Check(d.Run(TunePlan("cpu-stop", d.Read())), "cpu stop: stop runs");
        v = CpuOf(d, w);
        Equal(TrialEnd.Expired, w.Cpu, "cpu stop: the counters alone cannot tell a stop from a window that ran out");
        w.Did(true, TrialEnd.Stopped);
        v = CpuOf(d, w);
        Equal(Strings.T("tuner.end.stopped"), v.Outcome, "cpu stop: the window names its own stop");

        Check(d.Run(TunePlan("cpu-trial", d.Read(), TunerView.CpuRequest(3200u, 2u, 90u))), "cpu keep: trial runs");
        CpuOf(d, w);
        Check(d.Run(TunePlan("cpu-keep", d.Read())), "cpu keep: keep runs");
        v = CpuOf(d, w);
        Equal(TrialEnd.Kept, w.Cpu, "cpu keep: the watch names a keep");
        Check(v.OutcomeGood && v.Status == Strings.T("tuner.cpu.saved"), "cpu keep: the card says saved");
        Check(v.InForce.Any(r => r.Key == Strings.T("tuner.cpu.saved.label") && r.Value.Contains("3200 MHz")), "cpu keep: the saved row");
        Check(v.ResetEnabled, "cpu keep: the standard settings can come back");
        CpuAgree(d, v, null, null, null, "cpu kept");

        Check(d.Run(TunePlan("cpu-reset", d.Read())), "cpu reset: runs");
        CpuOf(d, w);
        w.Did(true, TrialEnd.Reset);
        v = CpuOf(d, w);
        Equal(Strings.T("tuner.end.reset.cpu"), v.Outcome, "cpu reset: the card says standard is back");
        Check(!v.ResetEnabled && v.Status == Strings.T("tuner.cpu.standard"), "cpu reset: nothing left to reset");
        CpuAgree(d, v, null, null, null, "cpu reset");

        // The core count: a restart setting with its own refusal when it is chosen already.
        var cores = TunePlan("core-mask", d.Read(), new Recovery.PlanArgs { Cores = CpuTuning.FullCores });
        Check(!cores.Refused && cores.OfferRestart && cores.Tune.CoreMask == CpuTuning.MaskFull, "cores: eight cores is a restart setting");
        Check(d.Run(cores), "cores: runs");
        Check(TunePlan("core-mask", d.Read(), new Recovery.PlanArgs { Cores = CpuTuning.FullCores }).Refused, "cores: chosen already");
    }

    static void CpuLimitsAndStates()
    {
        var d = new FakeTunerDriver(true);
        var w = new TrialWatch();
        d.Run(TunePlan("cpu-readback", d.Read()));
        d.Busy = true;
        var v = CpuOf(d, w, 3000u);
        Check(!v.ApplyEnabled && !v.ReadEnabled && !v.ResetEnabled, "cpu busy: no write while the driver is busy");
        CpuAgree(d, v, 3000u, null, null, "cpu busy");
        d.Busy = false;

        d.RevertOwed = true;
        v = CpuOf(d, w);
        Equal(Strings.T("graphics.cpu.revert-owed"), v.RevertOwed, "cpu: an owed way back is said at the top");
        Check(TunerView.MustShow(null, d.Read().Cpu), "cpu: an owed way back keeps the cards open");
        d.RevertOwed = false;

        Equal(Strings.T("tuner.cpu.guard.unfinished"), CpuOf(d, w, null, null, null, 3).GuardNote, "cpu guard: unconfirmed start");
        foreach (uint reason in new uint[] { 2, 6, 7 })
            Equal(Strings.T("tuner.cpu.guard.refused"), CpuOf(d, w, null, null, null, reason).GuardNote, "cpu guard: reason " + reason);
        Check(CpuOf(d, w, null, null, null, 1).GuardNote == null, "cpu guard: applied needs no note");

        // Turned off while on: the card keeps working for this start and says what the restart does.
        d.Snap.Parameters.Remove("CpuTune");
        v = CpuOf(d, w);
        Equal(TuneCard.Ready, v.Card, "cpu: still on in this start");
        Equal(Strings.T("tuner.cpu.off-after-restart"), v.AfterRestart, "cpu: the restart turns it off");
        Check(v.EnableOffered && !v.DisableOffered, "cpu: and it can be turned on again");
        CpuAgree(d, v, null, null, null, "cpu turned off, still on");

        Equal(TuneCard.NotInstalled, TunerView.Cpu(null, false, null, null, null, null, null, TrialEnd.None).Card, "cpu: no driver");
        Equal(TuneCard.NoReading, TunerView.Cpu(null, true, null, null, null, null, null, TrialEnd.None).Card, "cpu: no reading");

        // A voltage over the app's line is shown as a warning, never as a plain number.
        var high = new CpuState { Flags = CpuState.FlagValid | CpuState.FlagTuneOn | CpuState.FlagQueue3Proven, VoltageMv = 1310, CoreMHz = new uint[8], PstateMHz = new uint[8] };
        var hv = TunerView.Cpu(high, true, 1, null, null, null, null, TrialEnd.None);
        Check(hv.Readings[0].Warn && hv.Readings[0].Value.Contains("1310"), "cpu: a voltage over 1300 mV is a warning");

        // No text of either card names a driver internal.
        var texts = new List<string>();
        var dv = new FakeTunerDriver(true);
        dv.Run(TunePlan("cpu-readback", dv.Read()));
        var cv = TunerView.Curve(dv.Read().Curve, null, true, 3, TrialEnd.Expired);
        var uv = TunerView.Cpu(dv.Read().Cpu, true, 1, 3, 3000, 2, 90, TrialEnd.Kept);
        texts.AddRange(new[] { cv.Status, cv.Saved, cv.Live, cv.GuardNote, cv.Outcome, uv.Status, uv.GuardNote, uv.Outcome }.Where(t => t != null));
        texts.AddRange(cv.Rows.SelectMany(r => new[] { r.Key, r.Value }));
        texts.AddRange(uv.Readings.Concat(uv.InForce).SelectMany(r => new[] { r.Key, r.Value }));
        foreach (var lang in Strings.Languages)
        {
            foreach (var id in new[] { "tuner.end.kept", "tuner.end.stopped", "tuner.end.expired", "tuner.end.reset.cpu", "tuner.end.reset.curve",
                "tuner.cpu.on-after-restart", "tuner.cpu.off-after-restart", "tuner.curve.guard.unfinished", "tuner.cpu.guard.refused" })
                texts.Add(Strings.In(lang, id));
        }
        Equal("", string.Join(" | ", PlainWords.Findings(texts)), "G-NOINT: the tuning cards name no internals");
        Check(PlainWords.Findings(new[] { "CpuTune" }).Any(), "G-NOINT: the processor setting's own name counts as an internal");
    }

    static void TuningSummaryAndPoll()
    {
        var d = new FakeTunerDriver(true);
        var s = d.Read();
        Equal(Strings.T("tuner.summary.standard"), string.Join("|", TunerView.Summary(s.Curve, s.Cpu)), "summary: standard");
        Check(!TunerView.MustShow(s.Curve, s.Cpu), "summary: nothing forces the cards open");
        d.Run(TunePlan("tune-trial", d.Read(), new Recovery.PlanArgs { Curve = TunerPlan.CurveText(Tuner.Preset("mild", Tuner.Table(), Tuner.Floors())), Window = TunerPlan.DefaultWindowMs }));
        s = d.Read();
        Check(TunerView.Summary(s.Curve, s.Cpu).Contains(Strings.T("tuner.summary.curve-trial")), "summary: a curve test is named");
        Check(TunerView.MustShow(s.Curve, s.Cpu), "summary: a running test keeps the cards open");
        d.Tick(25);
        d.Run(TunePlan("tune-keep", d.Read()));
        d.Run(TunePlan("cpu-readback", d.Read()));
        d.Run(TunePlan("cpu-trial", d.Read(), TunerView.CpuRequest(3000u, null, null)));
        d.Run(TunePlan("cpu-keep", d.Read()));
        s = d.Read();
        var lines = TunerView.Summary(s.Curve, s.Cpu);
        Check(lines.Contains(Strings.T("tuner.summary.curve-own")) && lines.Contains(Strings.T("tuner.summary.cpu-own")) && lines.Count == 2, "summary: both own settings are named");

        // G-PERF: the graphics page polls only while its tuning cards are open or a test runs.
        Check(Sensors.PollWanted(true, false, "graphics", true), "G-PERF: open tuning cards poll");
        Check(!Sensors.PollWanted(true, false, "graphics", false), "G-PERF: closed tuning cards poll nothing");
        Check(!Sensors.PollWanted(true, true, "graphics", true), "G-PERF: a minimised window polls nothing");
        Check(!Sensors.PollWanted(false, false, "graphics", true), "G-PERF: a hidden window polls nothing");
        Check(!Sensors.PollWanted(true, false, "games", true), "G-PERF: the switch is the graphics page's alone");
    }
}
