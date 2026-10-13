// The screen rule the owner asked for on 2026-10-12: on a 1024x768 or 800x600 screen, and on every smaller one, the
// guide character and the whole right-hand panel she sits in do not show, and the content column takes the width
// they leave. A screen that small has no room for a 300 px column beside the page: on the tester's 800x600 screen
// (GitHub #38) the panel covered the text of the screen.
//
// The same rule is in the control application (LayoutRules.SmallScreen); the two programs have one table of numbers
// and no shared assembly, so each carries its own copy of six lines.
namespace AmdgpuWddmSetup
{
    public static class SmallScreen
    {
        // The largest working area that still counts as small, in logical units (pixels at 96 DPI). 1024x768 and
        // 800x600 are both inside it, and so is every screen smaller than either of them.
        public const int MaxWidth = 1024, MaxHeight = 768;

        // workWidth, workHeight: the working area of the screen that holds the window, in logical units. A size that
        // is not known (zero or less in either direction) is not small, so the panel stays where it always was.
        public static bool Is(int workWidth, int workHeight)
        {
            if (workWidth <= 0 || workHeight <= 0) return false;
            return workWidth <= MaxWidth || workHeight <= MaxHeight;
        }

        // The same rule from a working area in device pixels and the scale the window draws at: at 150 % a 1920x1032
        // working area is 1280x688 logical units, which this rule counts as small by its height.
        public static bool IsDevice(int workWidth, int workHeight, float scale)
        {
            if (scale <= 0.01f) return false;
            return Is((int)(workWidth / scale), (int)(workHeight / scale));
        }
    }
}
