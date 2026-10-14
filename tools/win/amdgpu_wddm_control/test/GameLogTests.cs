// Host tests of "Record game logs" (src/GameLog.cs, the game-log-on and game-log-off actions): the range of the
// recording time, the instant that is written, the countdown, the folder's prune and the report's choice of files,
// the plans and their refusals, the plain sentences, and the contract with the sink that reads the value
// (driver/umd/d3d12/stdio-log.h).
using System;
using System.Collections.Generic;
using System.Globalization;
using System.IO;
using System.Linq;
using System.Text.RegularExpressions;
using AmdgpuWddmControl;

static partial class UnitTests
{
    static GameLog.LogFile Log(string name, long bytes, string utc)
    {
        return new GameLog.LogFile { Name = name, Bytes = bytes, WrittenUtc = DateTime.Parse(utc, CultureInfo.InvariantCulture, System.Globalization.DateTimeStyles.AdjustToUniversal | System.Globalization.DateTimeStyles.AssumeUniversal) };
    }

    static void GameLogTests(string root)
    {
        Strings.Language = "en";
        var now = new DateTime(2026, 10, 13, 12, 0, 0, DateTimeKind.Utc);

        // ---- the contract with the sink that reads this value --------------------------------------------------
        // The app writes what the shells and the engines read. Both halves name the key, the value and the folder
        // in one place each, and these have to be the same strings, or the switch writes where nothing looks.
        Equal(@"SOFTWARE\amdgpu-wddm\Diagnostics", GameLog.RegistryPath, "the switch lives under the driver's diagnostics key");
        Equal("GameLogUntil", GameLog.ValueName, "the value name");
        Equal(@"amdgpu-wddm\logs\games", GameLog.FolderBelowProgramData, "the folder below ProgramData");
        var sink = Path.Combine(root, @"driver\umd\d3d12\stdio-log.h");
        if (File.Exists(sink))
        {
            var text = File.ReadAllText(sink);
            Check(text.Contains("\"" + GameLog.RegistryPath.Replace("\\", "\\\\") + "\""), "the sink reads the key this app writes");
            Check(text.Contains("\"" + GameLog.ValueName + "\""), "the sink reads the value this app writes");
            Check(text.Contains("\"" + GameLog.FolderBelowProgramData.Replace("\\", "\\\\") + "\""), "the sink opens its files in the folder this app prepares");
            Check(text.Contains("REG_QWORD") || text.Contains("RRF_RT_QWORD"), "the sink reads the value as a QWORD");
            // The cap one file stops itself at, named in both halves so the window can say it.
            var cap = Regex.Match(text, @"kFileCapBytes=(\d+)ull<<(\d+)");
            Check(cap.Success, "the sink names its own file cap");
            if (cap.Success)
                Equal(GameLog.FileBytesLimit, long.Parse(cap.Groups[1].Value, CultureInfo.InvariantCulture) << int.Parse(cap.Groups[2].Value, CultureInfo.InvariantCulture),
                    "the file cap of the sink and of this app agree");
        }

        // ---- the range -----------------------------------------------------------------------------------------
        Check(GameLog.MinMinutes == 1 && GameLog.MaxMinutes == 60, "the range is 1 to 60 minutes");
        Equal(10u, GameLog.DefaultMinutes, "the button offers 10 minutes, as the owner asked");
        foreach (var n in GameLog.Choices) Check(GameLog.IsValidMinutes(n), "offered recording time " + n + " is inside the range");
        Check(GameLog.Choices.Contains(GameLog.DefaultMinutes), "the list offers the recommended length");
        Check(GameLog.Choices.SequenceEqual(GameLog.Choices.OrderBy(n => n)), "the list is shortest first");
        foreach (var n in new uint[] { 0, 61, 600, uint.MaxValue }) Check(!GameLog.IsValidMinutes(n), n + " minutes is outside the range");
        Throws<ArgumentException>(() => GameLog.PlanOn(0, now), "0 minutes is refused");
        Throws<ArgumentException>(() => GameLog.PlanOn(61, now), "61 minutes is refused");

        // ---- the allow-list ------------------------------------------------------------------------------------
        Check(Recovery.Allowed(GameLog.RegistryPath, GameLog.ValueName), "the allow-list has the switch");
        foreach (var n in new[] { "GameLogFolder", "DdiTrace", "Experiment", "GameLogUntil2" })
            Check(!Recovery.Allowed(GameLog.RegistryPath, n), "the allow-list refuses " + n + " of the same key");
        Check(!Recovery.Allowed(@"SOFTWARE\amdgpu-wddm", GameLog.ValueName), "the allow-list refuses the same name in another key");
        Check(Recovery.Actions.Contains("game-log-on") && Recovery.Actions.Contains("game-log-off"), "the action list has both actions");
        // Both begin with "game-", and the per-game actions are the three that take a game's file name. The prefix
        // is not the test: it made the helper demand --image for these two.
        Check(!Recovery.AboutOneGame("game-log-on") && !Recovery.AboutOneGame("game-log-off"), "the game-log actions are not about one game");
        foreach (var a in new[] { "game-profile", "game-undo", "game-redo" }) Check(Recovery.AboutOneGame(a), a + " is about one game");

        // ---- the instant and the countdown ---------------------------------------------------------------------
        var on = GameLog.PlanOn(10, now);
        Equal(1, on.Count, "switching on is one write");
        Check(on[0].Path == GameLog.RegistryPath && on[0].Name == GameLog.ValueName && on[0].Kind == "QWord" && !on[0].Delete,
            "the write is GameLogUntil as a QWORD");
        Equal(now.AddMinutes(10), DateTime.FromFileTimeUtc(on[0].Number), "the number written is the instant recording stops");
        Equal(now.AddMinutes(1), DateTime.FromFileTimeUtc(GameLog.PlanOn(1, now)[0].Number), "one minute");
        Equal(now.AddMinutes(60), DateTime.FromFileTimeUtc(GameLog.PlanOn(60, now)[0].Number), "sixty minutes");
        // Pressing the button again while recording runs is how a person adds time: the value is written, not kept.
        var running = now.AddMinutes(10).ToFileTimeUtc();
        Equal(1, GameLog.PlanOn(30, now).Count, "switching on again writes the new instant");
        Check(GameLog.PlanOn(30, now)[0].Number > running, "the new instant is later than the one running");

        long stored = on[0].Number;
        Check(GameLog.Recording(stored, now), "recording runs before the instant");
        Check(GameLog.Recording(stored, now.AddMinutes(9.9)), "recording runs a breath before the instant");
        Check(!GameLog.Recording(stored, now.AddMinutes(10)), "recording is over at the instant");
        Check(!GameLog.Recording(stored, now.AddHours(1)), "recording is over after it");
        Check(!GameLog.Recording(null, now), "nothing stored: recording is off");
        Check(!GameLog.Recording(0, now), "a zero: recording is off");
        Check(!GameLog.Recording(-1, now), "a negative number is not an instant");
        Equal(null, GameLog.Until(long.MaxValue), "a number outside the range of a time reads as nothing stored");
        Equal(null, GameLog.Until(null), "nothing stored has no instant");

        Equal(10, GameLog.MinutesLeft(stored, now), "ten minutes left at the start");
        Equal(10, GameLog.MinutesLeft(stored, now.AddSeconds(1)), "a second later it still reads ten, not nine");
        Equal(1, GameLog.MinutesLeft(stored, now.AddMinutes(9)), "one minute left");
        Equal(1, GameLog.MinutesLeft(stored, now.AddSeconds(599)), "a second left reads as one minute, never zero");
        Equal(0, GameLog.MinutesLeft(stored, now.AddMinutes(10)), "nothing left when it is over");
        Equal(0, GameLog.MinutesLeft(null, now), "nothing stored: nothing left");
        Equal((int)GameLog.MaxMinutes, GameLog.MinutesLeft(now.AddDays(1).ToFileTimeUtc(), now), "an instant far ahead reads as the longest recording");

        // ---- switching off -------------------------------------------------------------------------------------
        var off = GameLog.PlanOff(stored);
        Equal(1, off.Count, "switching off is one write");
        Check(off[0].Delete && off[0].Name == GameLog.ValueName, "switching off removes the value, so nothing is left to read");
        Equal(0, GameLog.PlanOff(null).Count, "nothing stored: nothing to switch off");

        // ---- the folder's prune --------------------------------------------------------------------------------
        var files = new List<GameLog.LogFile>
        {
            Log("witcher3-100-20261013T100000Z.log", 300, "2026-10-13T10:00:00Z"),
            Log("q2rtx-200-20261013T110000Z.log", 300, "2026-10-13T11:00:00Z"),
            Log("rottr-300-20261013T120000Z.log", 300, "2026-10-13T12:00:00Z"),
        };
        Equal(0, GameLog.PlanPrune(files, 1000).Count, "below the limit: nothing is pruned");
        var pruned = GameLog.PlanPrune(files, 700);
        Equal(1, pruned.Count, "one file above the limit");
        Equal("witcher3-100-20261013T100000Z.log", pruned[0], "the oldest goes first");
        Equal(2, GameLog.PlanPrune(files, 400).Count, "two files above a smaller limit");
        Check(!GameLog.PlanPrune(files, 400).Contains("rottr-300-20261013T120000Z.log"), "the newest file is never pruned");
        // One file larger than the whole limit is the one being recorded into right now: it stays.
        Equal(0, GameLog.PlanPrune(new[] { Log("big.log", 5000, "2026-10-13T12:00:00Z") }, 700).Count, "a single file above the limit is kept");
        Equal(0, GameLog.PlanPrune(new GameLog.LogFile[0], 700).Count, "an empty folder prunes nothing");
        Equal(0, GameLog.PlanPrune(null, 700).Count, "no reading at all prunes nothing");
        Equal(0, GameLog.PlanPrune(new GameLog.LogFile[] { null }, 700).Count, "a damaged entry is skipped");
        Check(GameLog.FolderBytesLimit == 512L << 20 && GameLog.FileBytesLimit == 64L << 20, "the folder holds 512 MiB and one file 64 MiB");

        // ---- the report's choice -------------------------------------------------------------------------------
        var report = GameLog.PlanReport(files, 2, 10000);
        Equal(2, report.Count, "at most the count asked for");
        Equal("rottr-300-20261013T120000Z.log", report[0].Name, "newest first");
        Equal("q2rtx-200-20261013T110000Z.log", report[1].Name, "then the one before it");
        Equal(3, GameLog.PlanReport(files, 5, 10000).Count, "a bigger allowance takes them all");
        Equal(2, GameLog.PlanReport(files, 5, 650).Count, "the byte allowance stops the list");
        // A file above the whole allowance is left out, not cut in half: half a log reads as a game that stopped.
        var huge = new List<GameLog.LogFile> { Log("huge.log", 99999, "2026-10-13T13:00:00Z") };
        huge.AddRange(files);
        var withHuge = GameLog.PlanReport(huge, 5, 1000);
        Check(!withHuge.Any(f => f.Name == "huge.log"), "a file above the allowance is left out");
        Equal(3, withHuge.Count, "and the smaller ones still go in");
        Equal(0, GameLog.PlanReport(new GameLog.LogFile[0], 5, 1000).Count, "an empty folder gives no files");
        Check(GameLog.ReportFileCount == 5 && GameLog.ReportBytesLimit == 20L << 20, "the report takes at most 5 files and 20 MiB");

        // ---- the plans -----------------------------------------------------------------------------------------
        var s = Open();
        var p = Recovery.Plan("game-log-on", s, more: new Recovery.PlanArgs { Minutes = 10 });
        Check(!p.Refused, "the plan to switch on: " + p.Refusal);
        Equal(1, p.Writes.Count, "the plan has the one write");
        Check(p.Writes[0].Name == GameLog.ValueName && p.Writes[0].Kind == "QWord", "the plan writes the switch as a QWORD");
        Check(p.GameLogFolder, "the plan also prepares the folder");
        Check(p.Effect == "the next time a game starts" && p.Undoable && !p.OfferRestart, "it applies the next time a game starts and needs no restart");
        Check(p.Change.Contains("10 minutes"), "the plan's English sentence names the length: " + p.Change);
        Check(p.Text().Contains("prune that folder") && p.Text().Contains("Users create files"), "the dry run names the folder steps");

        var dialog = PlainPlan.Describe(p);
        Equal(1, dialog.Changes.Count, "the dialog lists the one change: " + string.Join(" / ", dialog.Changes));
        Check(!dialog.Title.StartsWith("[", StringComparison.Ordinal) && dialog.Title.Length > 0, "the dialog has a title: " + dialog.Title);
        Check(!dialog.Restart, "no restart of Windows is offered");
        Check(dialog.Notes.Any(n => n == Strings.T("plan.when.games")), "the dialog says it applies the next time a game starts");
        Check(PlainPlan.LineId(p.Writes[0]) == "plan.line.gamelog-on", "G-PLAN: the write has its own plain sentence");
        foreach (var t in new[] { dialog.Title }.Concat(dialog.Changes).Concat(dialog.Notes))
            foreach (var x in PlainWords.Findings(new[] { t })) Check(false, "G-NOINT: the game log dialog: " + x);

        var recording = Open(); recording.GameLogUntil = DateTime.UtcNow.AddMinutes(5).ToFileTimeUtc();
        var stop = Recovery.Plan("game-log-off", recording);
        Check(!stop.Refused && stop.Writes.Count == 1 && stop.Writes[0].Delete, "while recording runs, switching off removes the value: " + stop.Refusal);
        Check(PlainPlan.LineId(stop.Writes[0]) == "plan.line.gamelog-off", "G-PLAN: the removal has its own plain sentence");
        Check(Recovery.Plan("game-log-off", Open()).Refusal.Contains("not being recorded"), "switching off with nothing recording is refused");
        Check(Recovery.Plan("game-log-on", Open()).Refusal.Contains("No recording time"), "no recording time given is refused");
        foreach (var bad in new uint[] { 0, 61, 600 })
        {
            var refused = Recovery.Plan("game-log-on", Open(), more: new Recovery.PlanArgs { Minutes = bad });
            Check(refused.Refusal.Contains("between 1 and 60 minutes"), bad + " minutes is refused by the plan");
            Equal(0, refused.Writes.Count, "a refusal plans no write");
            Check(!PlainPlan.Refusal(refused).StartsWith("[", StringComparison.Ordinal), "WU-061: the refusal of " + bad + " minutes has a plain text");
        }
        Check(Recovery.Plan("game-log-off", recording, more: new Recovery.PlanArgs { Minutes = 10 }).Refusal.Contains("takes no recording time"),
            "a recording time does not belong to switching off");
        var unreadable = Open(); unreadable.GameLogError = "GameLogUntil has registry type Binary";
        Check(Recovery.Plan("game-log-on", unreadable, more: new Recovery.PlanArgs { Minutes = 10 }).Refusal.Contains("cannot be read"),
            "an unreadable switch is not overwritten");
        Check(Recovery.Plan("game-log-off", unreadable).Refusal.Contains("cannot be read"), "and it is not removed either");
        // The driver does not have to answer: a person whose game does not start at all is the one who needs this.
        var noDriver = new RecoverySnapshot { DriverInstalled = true };
        Check(!Recovery.Plan("game-log-on", noDriver, more: new Recovery.PlanArgs { Minutes = 10 }).Refused,
            "the switch works without a driver that answers");

        // ---- the card's own words ------------------------------------------------------------------------------
        var texts = new List<string> { Strings.T("gamelog.title"), Strings.T("gamelog.intro"), Strings.T("gamelog.label"),
            Strings.T("gamelog.stop"), Strings.T("gamelog.open"), Strings.T("gamelog.unreadable"), Strings.T("gamelog.open-failed"),
            Strings.T("gamelog.start", "10"), Strings.T("gamelog.where", GameLog.FolderBelowProgramData, "0", "0"),
            GameLog.StateText(null, now), GameLog.StateText(stored, now), GameLog.StateText(stored, now.AddMinutes(9.5)),
            GameLog.StateText(stored, now.AddHours(1)), Strings.T("help.setting.gamelog") };
        texts.AddRange(GameLog.Items());
        foreach (var t in texts) Check(!t.StartsWith("[", StringComparison.Ordinal) && t.Length > 0, "every text of the card is there: " + t);
        foreach (var t in texts) foreach (var x in PlainWords.Findings(new[] { t })) Check(false, "G-NOINT: the game log card: " + x);
        Check(GameLog.StateText(stored, now).Contains("10"), "the card says how long is left");
        Check(GameLog.StateText(stored, now.AddSeconds(599)) == Strings.T("gamelog.now.on.one-minute"), "one minute left has its own sentence");
        Check(GameLog.Label(1) == Strings.T("gamelog.choice.one-minute"), "one minute has its own label");
        Check(GameLog.Label(10) != GameLog.Label(20) && GameLog.Label(10).Contains("10"), "each length has its own label");
        Equal(GameLog.Choices.Length, GameLog.Items().Count, "one entry per length");
        Equal(Array.IndexOf(GameLog.Choices, 10u), GameLog.Selected(10), "the chosen length is selected");
        Equal(Array.IndexOf(GameLog.Choices, GameLog.DefaultMinutes), GameLog.Selected(7), "a length the list does not offer selects the recommended one");

        // The folder a person is sent to.
        Equal(@"C:\ProgramData\amdgpu-wddm\logs\games", GameLog.Folder(@"C:\ProgramData"), "the folder below a given ProgramData");

        // ---- the state line of the support view ----------------------------------------------------------------
        Func<RecoverySnapshot, StateLine> line = x => Recovery.Describe(x).First(l => l.Topic == "Game logs");
        Equal("ok", line(Open()).Severity, "nothing recorded: all good");
        Check(line(Open()).Text.Contains("is absent"), "and the line says the value is absent");
        Equal("info", line(recording).Severity, "while recording runs: a note, not a warning");
        Equal("game-log-off", line(recording).Action, "and the view offers to stop it");
        Equal("warn", line(unreadable).Severity, "an unreadable switch: a warning");
        var over = Open(); over.GameLogUntil = DateTime.UtcNow.AddMinutes(-5).ToFileTimeUtc();
        Equal("ok", line(over).Severity, "a recording that is over: all good");
        Equal(null, line(over).Action, "and nothing to stop");
        var withFiles = Open(); withFiles.GameLogFiles = 3; withFiles.GameLogBytes = 7L << 20;
        Check(line(withFiles).Text.Contains("3 files"), "the line says what the folder holds: " + line(withFiles).Text);

        // ---- the search entry ----------------------------------------------------------------------------------
        Check(SettingsSearch.Index.Any(e => e.Id == "help.gamelog" && e.Page == "help"), "the switch is searchable on the Help page");
        Check(SettingsSearch.Find("log", "en").Any(h => h.Entry.Id == "help.gamelog"), "a search for a log finds it");
        Check(SettingsSearch.Find("record", "en").Any(h => h.Entry.Id == "help.gamelog"), "a search for recording finds it");

        // ---- the source rules ----------------------------------------------------------------------------------
        var src = Path.Combine(root, @"tools\win\amdgpu_wddm_control\src");
        foreach (var f in Directory.GetFiles(src, "MainForm*.cs").Concat(new[] { Path.Combine(src, "RecoveryView.cs") }))
        {
            var text = Regex.Replace(File.ReadAllText(f), @"//[^\n]*", "");
            Check(!text.Contains("\"GameLogUntil\""), "G-SRC: " + Path.GetFileName(f) + " does not name the value itself");
        }
        // G-ARG: the window plans in its own process and then writes a command line for the elevated helper, so
        // every option the helper reads has to be one the window writes.
        var parse = File.ReadAllText(Path.Combine(src, "RecoveryActions.cs"));
        var emit = File.ReadAllText(Path.Combine(src, "MainForm.Actions.cs"));
        Check(Regex.IsMatch(parse, @"a == ""--minutes""[^\n]*o\.More\."), "the helper reads --minutes");
        Check(emit.Contains("\"--minutes\""), "G-ARG: the window writes --minutes on the helper's command line");
    }
}
