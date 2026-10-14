// "Record game logs": a switch that turns the driver's diagnostic lines on for a few minutes, so that a person who
// is asked for a log of a game can hand one over.
//
// Why it exists (owner, 2026-10-13): the driver's sink has always been the environment variable AMDGPU_WDDM_LOG
// (driver/umd/d3d12/stdio-log.h), which nobody can set for a game that Steam or Epic starts. The switch replaces
// it with two clicks: the app writes the instant at which logging stops, and every shell and engine of every game
// that starts before that instant opens a log file of its own.
//
// What is written:
//   HKLM\SOFTWARE\amdgpu-wddm\Diagnostics  GameLogUntil  REG_QWORD, the UTC FILETIME at which recording stops.
//     The value is removed when the person switches recording off, so an absent value is plainly "off".
//   %ProgramData%\amdgpu-wddm\logs\games   the folder the logs go into, created by the elevated helper with an
//     access list that lets any user create files there and nobody but their owner change them afterwards.
//
// Why an instant and not a duration: a duration needs something to count from, and the only clock a game's shell
// can read without asking anybody is the system clock. One QWORD the driver compares against the clock needs no
// service, no timer and no process of ours in the game, and a switch somebody forgets ends by itself.
//
// Pure: no registry and no file access here. RecoveryProbe reads the value into the snapshot, Recovery.Plan plans
// the write through PlanWrites, and the elevated helper writes it and prepares the folder.
using System;
using System.Collections.Generic;
using System.Globalization;
using System.Linq;

namespace AmdgpuWddmControl
{
    public static class GameLog
    {
        public const string RegistryPath = @"SOFTWARE\amdgpu-wddm\Diagnostics";
        public const string ValueName = "GameLogUntil";

        // Below %ProgramData%; the same two constants the sink carries (stdio-log.h, namespace game_log).
        public const string FolderBelowProgramData = @"amdgpu-wddm\logs\games";

        // What the person can choose. One minute is enough to start a game that crashes at once; an hour of every
        // game's lines is already a lot of disk, and the folder is pruned on top of that.
        public const uint MinMinutes = 1, MaxMinutes = 60;
        // The one the button offers (owner: "na 10 minut czy coś").
        public const uint DefaultMinutes = 10;

        // The choices the list offers, shortest first.
        public static readonly uint[] Choices = { 1, 5, 10, 20, 30, 60 };

        // What the folder may hold in total. The helper prunes the oldest files above this before it switches
        // recording on, so a person who records many times never fills the system disk.
        public const long FolderBytesLimit = 512L << 20;
        // One file stops itself at this size (stdio-log.h kFileCapBytes). Named here so the window and the report
        // can say it.
        public const long FileBytesLimit = 64L << 20;

        // At most this many log files and this many bytes of them go into a support report.
        public const int ReportFileCount = 5;
        public const long ReportBytesLimit = 20L << 20;

        public static bool Allowed(string path, string name)
        {
            return string.Equals(path, RegistryPath, StringComparison.OrdinalIgnoreCase) && name == ValueName;
        }

        public static bool IsValidMinutes(uint minutes) { return minutes >= MinMinutes && minutes <= MaxMinutes; }

        // The stored instant as a UTC time, or null when nothing is stored or the number is not a time at all. A
        // FILETIME outside the range DateTime takes is not a time, and reads as "nothing is stored".
        public static DateTime? Until(long? stored)
        {
            if (stored == null || stored.Value <= 0) return null;
            try { return DateTime.FromFileTimeUtc(stored.Value); }
            catch (ArgumentOutOfRangeException) { return null; }
        }

        // True while recording runs. nowUtc is passed in, so the window, the plan and the tests all read one clock.
        public static bool Recording(long? stored, DateTime nowUtc)
        {
            var until = Until(stored);
            return until != null && until.Value > nowUtc;
        }

        // What is left of the recording, rounded up to the next minute (a person reads "1 minute left", not "0").
        // Zero when nothing is recording.
        public static int MinutesLeft(long? stored, DateTime nowUtc)
        {
            var until = Until(stored);
            if (until == null || until.Value <= nowUtc) return 0;
            double minutes = (until.Value - nowUtc).TotalMinutes;
            return minutes >= MaxMinutes ? (int)MaxMinutes : Math.Max(1, (int)Math.Ceiling(minutes));
        }

        public static string Number(uint minutes) { return minutes.ToString(CultureInfo.InvariantCulture); }

        // The label of one choice in the window's language. The recommended one says so, because a person who does
        // not know what to pick should pick that one.
        public static string Label(uint minutes)
        {
            if (minutes == DefaultMinutes) return Strings.T("gamelog.choice.recommended", Number(minutes));
            if (minutes == 1) return Strings.T("gamelog.choice.one-minute");
            return Strings.T("gamelog.choice.minutes", Number(minutes));
        }

        public static List<string> Items()
        {
            return Choices.Select(Label).ToList();
        }

        public static int Selected(uint minutes)
        {
            for (int i = 0; i < Choices.Length; i++) if (Choices[i] == minutes) return i;
            return Array.IndexOf(Choices, DefaultMinutes);
        }

        // The line under the switch: what is happening now.
        public static string StateText(long? stored, DateTime nowUtc)
        {
            if (!Recording(stored, nowUtc)) return Strings.T("gamelog.now.off");
            int left = MinutesLeft(stored, nowUtc);
            return left == 1 ? Strings.T("gamelog.now.on.one-minute") : Strings.T("gamelog.now.on", left.ToString(CultureInfo.InvariantCulture));
        }

        // The folder below a %ProgramData% the caller read; pure, so the tests pass a directory of their own.
        public static string Folder(string programData)
        {
            return System.IO.Path.Combine(programData ?? "", FolderBelowProgramData);
        }

        // ---- the plans -------------------------------------------------------------------------------------------

        // Switching recording on: one QWORD, the instant it stops. The value is always written, never left as it
        // is: pressing the button again while recording runs is how a person adds time.
        public static List<RegWrite> PlanOn(uint minutes, DateTime nowUtc)
        {
            if (!IsValidMinutes(minutes))
                throw new ArgumentException("The recording time must be between " + MinMinutes + " and " + MaxMinutes + " minutes.");
            return new List<RegWrite> { RegWrite.Qword(RegistryPath, ValueName, nowUtc.AddMinutes(minutes).ToFileTimeUtc()) };
        }

        // Switching recording off: the value is removed, so nothing is left to read.
        public static List<RegWrite> PlanOff(long? stored)
        {
            var writes = new List<RegWrite>();
            if (stored != null) writes.Add(RegWrite.Remove(RegistryPath, ValueName));
            return writes;
        }

        // The English sentence of the plan, for the log, the dry run and the support report.
        public static string DescribeOn(uint minutes, DateTime nowUtc)
        {
            return "Records the driver's game logs for " + Number(minutes) + " minute" + (minutes == 1 ? "" : "s") +
                ", until " + nowUtc.AddMinutes(minutes).ToString("yyyy-MM-dd'T'HH:mm:ss'Z'", CultureInfo.InvariantCulture) +
                ". Every game that starts before then writes a log file in " + FolderBelowProgramData + " below ProgramData.";
        }

        public static string DescribeOff()
        {
            return "Stops recording the driver's game logs: " + ValueName + " is removed, so a game that starts from now on writes no log. The files already written are kept.";
        }

        // ---- the folder ------------------------------------------------------------------------------------------

        // One file of the folder, as the helper and the report see it.
        public sealed class LogFile
        {
            public string Name;
            public long Bytes;
            public DateTime WrittenUtc;
        }

        // The files to delete so that the folder holds at most limit bytes: the oldest first, and never the newest
        // file, whatever its size (a single file above the limit is the one somebody is recording into right now).
        // Pure; the helper does the deleting.
        public static List<string> PlanPrune(IEnumerable<LogFile> files, long limit)
        {
            var newestFirst = Order(files);
            var doomed = new List<string>();
            long total = 0;
            for (int i = 0; i < newestFirst.Count; i++)
            {
                total += newestFirst[i].Bytes;
                if (i > 0 && total > limit) doomed.Add(newestFirst[i].Name);
            }
            return doomed;
        }

        // The files a support report takes: the newest first, at most count of them and at most bytes in total. A
        // single file above the whole allowance is left out, not cut in half: half a log misleads the reader.
        public static List<LogFile> PlanReport(IEnumerable<LogFile> files, int count, long bytes)
        {
            var taken = new List<LogFile>();
            long total = 0;
            foreach (var f in Order(files))
            {
                if (taken.Count >= count) break;
                if (total + f.Bytes > bytes) continue;
                taken.Add(f);
                total += f.Bytes;
            }
            return taken;
        }

        // Newest first, by the time written and then by name, so the order never depends on the file system's.
        static List<LogFile> Order(IEnumerable<LogFile> files)
        {
            return (files ?? Enumerable.Empty<LogFile>()).Where(f => f != null && f.Name != null)
                .OrderByDescending(f => f.WrittenUtc).ThenByDescending(f => f.Name, StringComparer.OrdinalIgnoreCase).ToList();
        }
    }
}
