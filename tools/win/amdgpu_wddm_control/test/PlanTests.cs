// G-PLAN additions of GUI phase 1: the CU setter as a Recovery action (cu-mode, cu-confirm), reset-defaults with its
// CU part and with or without the per-game exceptions (WU-055), per-game changes with their own undo and redo
// (WU-020a), and the allow-lists.
using System;
using System.Collections.Generic;
using System.IO;
using System.Linq;
using AmdgpuWddmControl;

static partial class UnitTests
{
    static RecoverySnapshot WithDefaults()
    {
        var s = Open();
        s.DefaultParameters = new Dictionary<string, long> { { "EnableGpuPresentBlit", 1 }, { "EnableCddDwmInterop", 1 }, { "DpmMode", 1 }, { "DpmMaxMHz", 1500 }, { "KeepLog", 0 } };
        s.DefaultRouter = new Dictionary<string, long> { { "DwmForceCpu", 0 }, { "RequireKmdSwitches", 1 } };
        s.DefaultApplications = new Dictionary<string, string>(StringComparer.OrdinalIgnoreCase) { { "witcher3.exe", "present-noprimary,present-cached,raytracing-tier,recording-bind,retire-handoff,deferred-replay" } };
        s.GameProfiles = new Dictionary<string, string>(StringComparer.OrdinalIgnoreCase) { { "witcher3.exe", "raytracing-tier" }, { "game.exe", "deferred-replay" } };
        s.CuBadValues = new List<string>();
        return s;
    }

    static BackupRecord Rec(string file, string utc, string action, string image, string undoes = null)
    {
        return new BackupRecord { File = file, Utc = utc, Action = action, Args = new[] { "image=" + image }, Undoable = true, Undoes = undoes };
    }

    static void PlanAdditions(string root)
    {
        Strings.Language = "en";
        var cumode = File.ReadAllText(Path.Combine(root, @"driver\kmd\cumode.c"));
        foreach (var n in new[] { "CuMode", "CuDisableWgp", "CuModePending", "CuModeConfirmed" })
            Check(cumode.Contains("L\"" + n + "\""), "cumode.c names " + n);

        // The allow-lists: Allowed is unchanged, the CU setter has its own narrow set.
        foreach (var n in CuMode.ValueNames) Check(!Recovery.Allowed(Recovery.ParametersPath, n), "Recovery.Allowed keeps " + n + " out");
        Check(Recovery.CuAllowed(RegWrite.Dword(Recovery.ParametersPath, "CuMode", 40)) && Recovery.CuAllowed(RegWrite.Remove(Recovery.ParametersPath, "CuMode")) &&
            Recovery.CuAllowed(RegWrite.Remove(Recovery.ParametersPath, "CuDisableWgp")) && Recovery.CuAllowed(RegWrite.Remove(Recovery.ParametersPath, "CuModeConfirmed")), "CuAllowed: the setter's set");
        foreach (var w in new[] { RegWrite.Dword(Recovery.ParametersPath, "CuMode", 24), RegWrite.Dword(Recovery.ParametersPath, "CuModeConfirmed", 40),
            RegWrite.Dword(Recovery.ParametersPath, "CuDisableWgp", 0), RegWrite.Remove(Recovery.ParametersPath, "CuModePending"), RegWrite.Dword(Recovery.ParametersPath, "CuModePending", 0),
            RegWrite.Remove(Recovery.ParametersPath, "CuModeLastReason"), RegWrite.Dword(Recovery.RouterPath, "CuMode", 40) })
            Check(!Recovery.CuAllowed(w), "CuAllowed refuses " + w);

        // cu-mode: preview = dry-run plan = the helper's steps.
        var s = WithDefaults(); s.Parameters["CuMode"] = 40; s.Parameters["CuDisableWgp"] = 0x21; s.Parameters["CuModeConfirmed"] = CuMode.Encode(40, 0x21);
        var p = Recovery.Plan("cu-mode", s, more: new Recovery.PlanArgs { Cu = 24 });
        Check(!p.Refused && p.Cu != null && !p.Undoable && p.OfferRestart, "cu-mode 24: planned, not undoable, restart offered");
        var text = p.Text();
        Check(text.Contains("S1 delete HKLM\\" + Recovery.ParametersPath + " CuMode") && text.Contains("S2 delete") && text.Contains("S3 delete") && text.Contains("never restored"), "cu-mode plan text names every step");
        Check(text.IndexOf("S1 delete", StringComparison.Ordinal) < text.IndexOf("S1 flush", StringComparison.Ordinal) &&
            text.IndexOf("S1 flush", StringComparison.Ordinal) < text.IndexOf("S2 delete", StringComparison.Ordinal), "cu-mode 24 plan order: S1, verify, then S2");
        Equal(p.Cu.Preview.Count, p.Preview.Count, "cu-mode 24: the dialog's preview is the plan's change list");
        Check(p.Notes.Any(n => n.Contains("0x21")), "cu-mode: the exact old mask goes to the plan");
        p = Recovery.Plan("cu-mode", s, more: new Recovery.PlanArgs { Cu = 40 });
        Check(!p.Refused && p.Cu.Steps.Select(x => x.Kind).SequenceEqual(new[] { "remove-confirmed", "remove-disable", "flush-readback" }), "cu-mode 40 with a mask: S2, S3, S4");
        Check(p.Preview.Contains(CuMode.EffectText()), "cu-mode 40: the effect text before the switch");
        Check(Recovery.Plan("cu-mode", WithDefaults(), more: new Recovery.PlanArgs { Cu = 24 }).Refusal == "Standard (24) is selected already.", "cu-mode 24 when stock: refused");
        Check(Recovery.Plan("cu-mode", WithDefaults()).Refused, "cu-mode without a choice: refused");
        var bad = WithDefaults(); bad.CuBadValues.Add("CuMode");
        Check(Recovery.Plan("cu-mode", bad, more: new Recovery.PlanArgs { Cu = 40 }).Refusal == "The stored setting cannot be read.", "cu-mode with a non-DWORD value: refused");
        Check(bad.StoredCu().Unreadable && CuMode.Predict(bad.StoredCu()) == CuNext.Unknown, "a non-DWORD CU value is unreadable, never absent");

        // cu-confirm: Waiting only.
        s = WithDefaults(); s.Health.Generation = 9; s.Cu = new CuModeState { Flags = 3, Applied = 40, ActiveCus = 40, Generation = 9 };
        p = Recovery.Plan("cu-confirm", s);
        Check(!p.Refused && p.CuConfirm && !p.Undoable && p.Writes.Count == 0, "cu-confirm in Waiting: one confirm, no writes");
        s.Cu.Flags = 5;
        Check(Recovery.Plan("cu-confirm", s).Refused, "cu-confirm when confirmed already: refused");
        s.Cu.Flags = 3; s.Cu.Generation = 8;
        Check(Recovery.Plan("cu-confirm", s).Refused, "cu-confirm with another start's snapshot: refused");

        // reset-defaults: CU through the Standard (24) setter; games kept by default, reset on request.
        s = WithDefaults(); s.Parameters["CuMode"] = 40;
        p = Recovery.Plan("reset-defaults", s);
        Check(!p.Refused && p.Cu != null && p.Cu.Target == 24 && p.GameWrites.Count == 0 && p.Notes.Contains("Per-game settings are kept."), "reset keeps the game settings, CU via the setter");
        p = Recovery.Plan("reset-defaults", s, more: new Recovery.PlanArgs { Games = "reset" });
        Check(p.GameWrites["witcher3.exe"].StartsWith("present-noprimary") && p.GameWrites["game.exe"] == "", "reset with the games: the recommended profile back, others removed");
        Check(p.Text().Contains("delete HKLM\\" + Profiles.RegistryPath + "\\game.exe"), "reset with the games: the plan names each game");
        var none = WithDefaults(); none.DefaultApplications = null;
        Check(Recovery.Plan("reset-defaults", none, more: new Recovery.PlanArgs { Games = "reset" }).Refused, "reset with the games but no recommended list: refused");
        var clean = WithDefaults(); clean.Parameters["DpmMode"] = 1; clean.Parameters["DpmMaxMHz"] = 1500; clean.DwmForceCpu = 0;
        Check(Recovery.Plan("reset-defaults", clean).Refused, "reset when everything is default (CU stock, games kept): refused");
        clean.Parameters["CuModeConfirmed"] = 40;
        Check(!Recovery.Plan("reset-defaults", clean).Refused, "reset with a leftover CU confirmation: the cleanup runs");

        // game-profile: the scope note, effect at the next game start, undoable.
        s = WithDefaults();
        p = Recovery.Plan("game-profile", s, more: new Recovery.PlanArgs { Image = "game.exe", Value = "deferred-replay,recording-bind" });
        Check(!p.Refused && p.GameWrites["game.exe"] == "recording-bind,deferred-replay" && p.Undoable, "game-profile writes the composed list");
        Check(p.Effect == "the next time game.exe starts" && p.Notes.Any(n => n.Contains("every game named game.exe")), "game-profile: next start of the game, file-name scope");
        Check(Recovery.Plan("game-profile", s, more: new Recovery.PlanArgs { Image = "game.exe", Value = "deferred-replay" }).Refused, "game-profile without a change: refused");
        Check(Recovery.Plan("game-profile", s, more: new Recovery.PlanArgs { Image = "game.exe", Value = "" }).GameWrites["game.exe"] == "", "game-profile with nothing checked removes the key");
        Check(Recovery.Plan("game-profile", s, more: new Recovery.PlanArgs { Image = "..\\x.exe", Value = "" }).Refused, "game-profile refuses a path");

        // Per-game undo and redo.
        var list = new List<BackupRecord> { Rec("b1", "1", "game-profile", "game.exe"), Rec("b2", "2", "game-profile", "game.exe"), Rec("o1", "3", "game-profile", "other.exe") };
        Equal("b2", Recovery.GameUndoTarget(list, "game.exe").File, "undo: the newest change of that game");
        Check(Recovery.GameRedoTarget(list, "game.exe") == null, "redo: nothing undone");
        list.Add(Rec("u2", "4", "game-undo", "game.exe", "b2"));
        Equal("b1", Recovery.GameUndoTarget(list, "game.exe").File, "undo after an undo: the older change");
        Equal("u2", Recovery.GameRedoTarget(list, "game.exe").File, "redo: the undo");
        list.Add(Rec("r2", "5", "game-redo", "game.exe", "u2"));
        Equal("r2", Recovery.GameUndoTarget(list, "game.exe").File, "undo after a redo: the redo");
        Check(Recovery.GameRedoTarget(list, "game.exe") == null, "redo after a redo: nothing");
        list.Add(Rec("u1", "6", "game-undo", "game.exe", "r2"));
        list.Add(Rec("b3", "7", "game-profile", "game.exe"));
        Check(Recovery.GameRedoTarget(list, "game.exe") == null, "a new change clears the redo");
        Check(Recovery.UndoTarget(list) == null, "the global undo never takes a game change");
        list[0].Values.Add(new BackupValue { Path = Recovery.GamePath("game.exe"), Name = Profiles.ValueName, Existed = false });
        p = Recovery.Plan("game-undo", WithDefaults(), backups: new[] { list[0] }, more: new Recovery.PlanArgs { Image = "game.exe" });
        Check(!p.Refused && p.GameWrites["game.exe"] == "" && p.UndoOf == "b1", "game-undo restores the absence of the profile");

        // A global undo of a reset with games restores the games, never the CU values.
        var reset = new BackupRecord { File = "r", Utc = "9", Action = "reset-defaults", Undoable = true };
        reset.Values.Add(new BackupValue { Path = Recovery.ParametersPath, Name = "DpmMode", Existed = true, Kind = "DWord", Number = 0 });
        reset.Values.Add(new BackupValue { Path = Recovery.GamePath("game.exe"), Name = Profiles.ValueName, Existed = true, Kind = "String", Text = "deferred-replay" });
        reset.Diagnosis.Add(new BackupValue { Path = Recovery.ParametersPath, Name = "CuModeConfirmed", Existed = true, Kind = "DWord", Number = 40 });
        p = Recovery.Plan("undo", WithDefaults(), backups: new[] { reset });
        Check(!p.Refused && p.GameWrites["game.exe"] == "deferred-replay" && p.Writes.All(w => w.Name != "CuModeConfirmed") && p.Notes.Any(n => n.Contains("graphics-core part")),
            "undo of a reset: games restored, CU values never");
        var forged = new BackupRecord { File = "f", Utc = "10", Action = "reopen-gpu-path", Undoable = true };
        forged.Values.Add(new BackupValue { Path = Recovery.ParametersPath, Name = "CuModeConfirmed", Existed = true, Kind = "DWord", Number = 40 });
        Check(Recovery.Plan("undo", WithDefaults(), backups: new[] { forged }).Refused, "undo refuses a backup that names CuModeConfirmed (S6)");
    }
}
