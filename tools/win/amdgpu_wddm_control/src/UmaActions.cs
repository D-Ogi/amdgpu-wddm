using System;
using System.Globalization;

namespace AmdgpuWddmControl
{
    // Mutations require an unchanged confirmed observation and a qualified provider.
    public static class UmaActions
    {
        public static int Run(string[] args)
        {
            if (args.Length != 3 || args[0] != "--bc250-board-memory-action" ||
                (args[1] != "8192" && args[1] != "12288" && args[1] != "restore")) return 2;
            if (!Program.IsElevated()) return 5;
            Kmd.RefreshBoardCapabilities();
            if (!BoardCapabilities.Current.Allows(BoardCapabilities.Memory)) return 1;
            var before = Kmd.BoardMemory();
            if (before.Value == null || !UmaSetting.Confirmed(before.Value, args[2])) return 1;
            bool restore = args[1] == "restore";
            uint target = restore ? 0 : uint.Parse(args[1], CultureInfo.InvariantCulture);
            if (restore ? !UmaSetting.CanRestore(before.Value) : !UmaSetting.CanSet(before.Value, target)) return 1;
            var result = Kmd.BoardMemory(restore ? 2u : 1u, target, before.Value);
            uint wanted = restore ? before.Value.PreviousMiB : target;
            return UmaSetting.Verified(result.Value, restore ? 2u : 1u, wanted) ? 0 : 1;
        }
    }
}
