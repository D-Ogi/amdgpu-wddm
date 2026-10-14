// The driver's hang record, as a bug report shows it (BD-168, GitHub #40).
//
// The record is a SUBKEY of the driver's Parameters key (driver/kmd/guard.c GuardRecordHangRecovery), so a report
// that read the values of Parameters alone never showed it, and a field report of a DWM crash needs it: it says
// whether the driver attempted a stage-1 hang recovery in this boot, and when.
//
// Only the formatting lives here, because that is the part worth a test: LastTime is a REG_QWORD system time, and
// a reader who has to convert it by hand to compare it with a crash time will not do it. The reading of the key
// belongs to BugReport.cs, which the unit tests cannot compile (it reaches the inventory, and through it the whole
// application), so the one rule that can be wrong sits in a file of its own.
using System;
using System.Globalization;

namespace AmdgpuWddmControl
{
    public static class HangRecord
    {
        public const string SubKey = "HangRecovery";

        // The value as the report prints it: LastTime with its UTC instant beside the number, everything else as
        // it is. A value that is not a system time (zero, negative, not a number, out of range) is printed as it
        // is as well: a report says what the registry holds, and never replaces it with a guess.
        public static string Value(string name, string value)
        {
            long ticks;
            if (name != "LastTime" || !long.TryParse(value, NumberStyles.Integer, CultureInfo.InvariantCulture, out ticks) || ticks <= 0) return value;
            try { return value + " (" + DateTime.FromFileTimeUtc(ticks).ToString("yyyy-MM-ddTHH:mm:ssZ", CultureInfo.InvariantCulture) + ")"; }
            catch (Exception) { return value; }
        }
    }
}
