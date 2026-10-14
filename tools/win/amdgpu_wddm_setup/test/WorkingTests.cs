// Host tests of the progress screen's model (src/WorkingView.cs): what the Checking and Working screens show for a run,
// and which engine events change the screen in place. The events that change nothing on the screen (a 'step' for
// every change of an install, the install action) leave its key as it was, so the window draws nothing for them; a
// new stage changes the marks and the strip's value but not the frame, so the window builds nothing for it.
using System;
using System.Linq;
using AmdgpuWddmSetup;

static partial class UnitTests
{
    static EngineRun InstallRun(params string[] stages)
    {
        var r = new EngineRun(Id);
        int seq = 0;
        r.Apply(Ev(++seq, "start", "\"mode\":\"run\",\"gui\":true,\"dry_run\":false,\"package\":\"P\",\"contract\":\"" + EngineRun.Contract + "\",\"phase\":null"));
        r.Apply(Ev(++seq, "decision", "\"action\":\"install\""));
        r.Apply(Ev(++seq, "cancel", "\"available\":true,\"where\":\"before-changes\""));
        foreach (var s in stages) r.Apply(Ev(++seq, "stage", "\"id\":\"" + s + "\""));
        return r;
    }

    static string Marks(WorkingView v) { return string.Join(",", v.Marks.Select(m => m.ToString())); }

    static void WorkingViews()
    {
        // ---- events that change nothing on the screen ----------------------------------------------------------
        var run = InstallRun("preflight");
        var first = WorkingView.Of(run, "install", false, false, false);
        Equal("Current,Waiting,Waiting,Waiting,Waiting,Waiting,Waiting,Waiting", Marks(first), "working: the first stage runs");
        Equal(0.0625f, first.Progress, "working: the strip shows half of the first of eight stages");
        int seq = 100;
        run.Apply(Ev(++seq, "install-action", "\"action\":\"install\",\"boot_id\":41"));
        for (int i = 0; i < 5; i++) run.Apply(Ev(++seq, "step", "\"description\":\"x\",\"dry_run\":false"));
        var steps = WorkingView.Of(run, "install", false, false, false);
        Equal(first.Key, steps.Key, "working: 'step' and 'install-action' events change nothing on the screen");

        // ---- a new stage: the marks and the strip change, the frame stays ---------------------------------------
        run.Apply(Ev(++seq, "stage", "\"id\":\"test-signing\""));
        var next = WorkingView.Of(run, "install", false, false, false);
        Equal(first.Frame, next.Frame, "working: a known stage keeps the frame (no control is made again)");
        Check(next.Key != first.Key, "working: a known stage changes the key");
        Equal("Done,Current,Waiting,Waiting,Waiting,Waiting,Waiting,Waiting", Marks(next), "working: the stage before is done");
        Equal(0.1875f, next.Progress, "working: the strip moves on");

        // ---- the Cancel button ---------------------------------------------------------------------------------
        Check(next.CancelShown && next.CancelEnabled && next.NoteId == "working.cancel.available", "working: cancel offered");
        var asked = WorkingView.Of(run, "install", false, true, false);
        Equal(next.Frame, asked.Frame, "working: a cancel request keeps the frame");
        Check(!asked.CancelEnabled && asked.NoteId == "working.cancel.requested", "working: a cancel request turns the button off and says so");
        run.Apply(Ev(++seq, "cancel", "\"available\":false,\"where\":\"driver-install\""));
        var withdrawn = WorkingView.Of(run, "install", false, false, false);
        Check(!withdrawn.CancelEnabled && withdrawn.NoteId == "working.cancel.unavailable" && withdrawn.Frame == next.Frame, "working: a withdrawn offer turns the button off in place");

        // ---- what changes the frame ----------------------------------------------------------------------------
        Check(WorkingView.Of(run, "install", false, false, true).Frame != next.Frame, "working: the notice of cancelled removals changes the frame");
        var prepare = WorkingView.Of(null, "prepare", false, false, false);
        Check(!prepare.CancelShown && prepare.TitleId == "working.prepare.title" && prepare.StageIds.SequenceEqual(EngineRun.ExpectedStages("prepare-offline", null)),
            "working: a prepare run has its own title and stages and no Cancel button");
        var extras = InstallRun("preflight", "test-signing", "install", "firmware", "files", "extras");
        var withExtras = WorkingView.Of(extras, "install", false, false, false);
        Equal("preflight,test-signing,install,firmware,files,extras,driver,settings,finish", string.Join(",", withExtras.StageIds),
            "working: a stage the list does not know goes where the run reached it");
        Equal("Done,Done,Done,Done,Done,Current,Waiting,Waiting,Waiting", Marks(withExtras), "working: the stages before it are done");
        Check(withExtras.Frame != WorkingView.Of(InstallRun("preflight", "test-signing", "install", "firmware", "files"), "install", false, false, false).Frame,
            "working: a stage the list does not know changes the frame (the screen is built again)");

        // ---- the end of a run ----------------------------------------------------------------------------------
        var stopped = InstallRun("preflight", "test-signing");
        stopped.Apply(Ev(200, "result", "\"outcome\":\"failed\",\"exit_code\":1,\"message_id\":\"result.other\",\"mutated\":false"));
        Equal("Done,Failed,Waiting,Waiting,Waiting,Waiting,Waiting,Waiting", Marks(WorkingView.Of(stopped, "install", false, false, false)), "working: the stage a run stopped in is crossed out");
        var restart = InstallRun("preflight", "test-signing");
        restart.Apply(Ev(200, "result", "\"outcome\":\"restart-required\",\"exit_code\":0,\"message_id\":\"result.restart-driver\",\"mutated\":true"));
        Equal("Done,Done,Waiting,Waiting,Waiting,Waiting,Waiting,Waiting", Marks(WorkingView.Of(restart, "install", false, false, false)), "working: a restart ends the stage it reached as done");
        var none = WorkingView.Of(InstallRun(), "install", false, false, false);
        Check(!none.Progress.HasValue && none.Marks.All(m => m == StageMark.Waiting), "working: no stage yet is the moving band");

        // ---- the checks of a plan run --------------------------------------------------------------------------
        var plan = new EngineRun(Id);
        plan.Apply(Ev(1, "start", "\"mode\":\"plan\",\"gui\":true,\"dry_run\":false,\"package\":\"P\",\"contract\":\"" + EngineRun.Contract + "\",\"phase\":null"));
        var empty = WorkingView.Of(plan, "install", true, false, false);
        plan.Apply(Ev(2, "check", "\"id\":\"package.ok\",\"result\":\"ok\",\"name\":\"x\",\"detail\":\"d\""));
        var one = WorkingView.Of(plan, "install", true, false, false);
        plan.Apply(Ev(3, "check", "\"id\":\"admin.ok\",\"result\":\"warn\",\"name\":\"x\",\"detail\":\"d\""));
        var two = WorkingView.Of(plan, "install", true, false, false);
        Check(empty.Frame == two.Frame && !two.Progress.HasValue && two.TitleId == "checking.title", "checking: the checks keep the frame and the moving band");
        Check(one.ExtendsChecks(empty) && two.ExtendsChecks(one) && two.ExtendsChecks(two), "checking: new checks are added after the listed ones");
        Check(!one.ExtendsChecks(two) && !two.ExtendsChecks(null), "checking: fewer checks are not an addition");
        var other = new EngineRun(Id);
        other.Apply(Ev(1, "check", "\"id\":\"package.damaged\",\"result\":\"fail\",\"name\":\"x\",\"detail\":\"d\""));
        other.Apply(Ev(2, "check", "\"id\":\"admin.ok\",\"result\":\"warn\",\"name\":\"x\",\"detail\":\"d\""));
        Check(!WorkingView.Of(other, "install", true, false, false).ExtendsChecks(one), "checking: another first check builds the list again");
        Check(two.Key != one.Key, "checking: a new check changes the key");
    }
}
