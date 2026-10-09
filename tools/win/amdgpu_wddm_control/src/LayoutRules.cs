// The layout rules of --smoke-render (G-RENDER) that need no window, so that the unit tests cover them.
// Today one rule lives here: a page must fit the page column it was built at.
using System;

namespace AmdgpuWddmControl
{
    public static class LayoutRules
    {
        // A built page wider than the column it was given is cut on the right, so it is a finding.
        //
        // The measure is the column the page was built at, never the window's column width of this moment. The two
        // differ whenever the client size changed after the page was built: the window manager clamps a client size
        // larger than the desktop to the maximum tracking size, so the 1240 px client that --smoke-render asks for
        // becomes 1028 px on a 1024x768 session-0 desktop, and the page built before the window handle existed keeps
        // the wider column. Measured against the live column, the first page rendered then reported a finding
        // (home: 661 px page, 449 px column) although it was built correctly; b24 lab round 3,
        // scratch\train\b24\validation\r3\control-render-why.txt.
        //
        // slack absorbs the rounding of a fractional DPI scale (the caller passes Theme.S(2)).
        // Returns the finding text, or null when the page fits.
        public static string PageWidthFinding(string where, int pageWidth, int builtColumn, int slack)
        {
            if (where == null) throw new ArgumentNullException("where");
            if (slack < 0) throw new ArgumentOutOfRangeException("slack", "the slack of a width check is never negative");
            if (pageWidth <= builtColumn + slack) return null;
            return where + ": the page is " + pageWidth + " px wide, the column it was built at has " + builtColumn;
        }
    }
}
