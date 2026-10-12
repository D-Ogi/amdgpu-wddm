// The layout rules that need no window, so that the unit tests cover them: a page must fit the page column it was
// built at, a rebuilt page must put the keyboard focus back where the person had it, and it must stay where the
// person was reading it (--smoke-render G-RENDER and --smoke-scroll).
using System;
using System.Collections.Generic;

namespace AmdgpuWddmControl
{
    // One control of a rebuilt page, as the focus rule sees it: what it is called (type and accessible name, the key
    // MainForm.FocusKey makes), which card it belongs to, whether it can take the keyboard focus now, and whether it
    // is a control the person reaches with Tab (a panel can take the focus, but Tab never stops on one).
    public struct FocusCandidate
    {
        public string Key, Card;
        public bool CanFocus, CanTab;

        public FocusCandidate(string key, string card, bool canFocus, bool canTab)
        {
            Key = key; Card = card; CanFocus = canFocus; CanTab = canTab;
        }
    }

    // Where the keyboard focus goes after a page is rebuilt under the person's hands.
    public static class PageFocus
    {
        // The index in page of the control that gets the focus, or -1 for none.
        //
        // The control that had the focus takes it back. Where that control cannot have it any more the focus goes to
        // the first control of the same card that Tab stops on: a card rebuilds itself while the person works with
        // it, and a button that turns itself off must not throw the focus out of the page. The benchmark card does
        // exactly that (b23 lab round, F1): pressing "Start the test" disables that button, and with no rule of its
        // own the focus left for the first item of the navigation bar, which took the person away from the running
        // test and left "Stop the test" out of reach. With this rule the focus lands on "Stop the test" instead.
        public static int After(string key, string card, IList<FocusCandidate> page)
        {
            if (page == null) throw new ArgumentNullException("page");
            if (key == null) return -1;
            for (int i = 0; i < page.Count; i++)
                if (page[i].CanFocus && page[i].Key == key) return i;
            if (card == null) return -1;
            for (int i = 0; i < page.Count; i++)
                if (page[i].CanFocus && page[i].CanTab && page[i].Card == card) return i;
            return -1;
        }
    }


    // One card of a page, as the place rules see it: the name it is marked with, where its top edge is in the page
    // (the same number whatever the page is scrolled to) and how tall it is.
    public struct CardBox
    {
        public string Id;
        public int Top, Height;

        public CardBox(string id, int top, int height)
        {
            Id = id; Top = top; Height = height;
        }
    }

    // Where a page stands after it was rebuilt, and where it goes when a card must come into reach.
    public static class PagePlace
    {
        // The scroll offset of a page that was rebuilt under the person's hands.
        //
        // Keeping the offset itself is not enough. A card above the one the person works with can get a line while
        // the page is open, and everything under it then moves by the height of that line. The lab saw this during
        // a benchmark run (b23 lab round 2, F7): the case fan card gets the line "The fan is at full speed now"
        // while the game loads the chip, and "Start the test" and "Stop the test" moved 23 px down, 9 to 18 s into
        // the run, under the pointer of a person who was aiming at Stop. So the rule keeps the card instead of the
        // offset: the card at the top of the view stays where it is, and the page follows it.
        //
        // scroll: the offset before the rebuild. before, after: the cards of the page, before and after it.
        // Returns the offset for the rebuilt page; the caller clamps it to the new scroll range.
        public static int Keep(int scroll, IList<CardBox> before, IList<CardBox> after)
        {
            if (before == null) throw new ArgumentNullException("before");
            if (after == null) throw new ArgumentNullException("after");
            if (scroll <= 0) return scroll;
            // The card the person is looking at: the outermost one that covers the top edge of the view, because a
            // page marks sections inside cards as well. Where the top edge is in a gap between cards, the first card
            // under it keeps the place instead.
            bool have = false;
            CardBox mark = new CardBox();
            foreach (var b in before)
            {
                if (b.Id == null || b.Top > scroll || scroll >= b.Top + b.Height) continue;
                if (!have || b.Height > mark.Height || (b.Height == mark.Height && string.CompareOrdinal(b.Id, mark.Id) < 0)) { mark = b; have = true; }
            }
            if (!have)
                foreach (var b in before)
                {
                    if (b.Id == null || b.Top < scroll) continue;
                    if (!have || b.Top < mark.Top || (b.Top == mark.Top && b.Height > mark.Height)) { mark = b; have = true; }
                }
            if (!have) return scroll;
            foreach (var a in after)
                if (a.Id == mark.Id) return Math.Max(0, a.Top - (mark.Top - scroll));
            return scroll;
        }

        // The scroll offset that puts a row of controls of a card in reach. Nothing moves while the row is in the
        // view already: the page must not jump under a pointer that is aiming at a button (b23 lab round 2, F7).
        // Otherwise the page goes to the top of the card, which is where a person reads it from, and where that
        // would leave the row below the view, to the least offset that shows the row.
        //
        // scroll: the offset now. view: the height of the view. cardTop, rowTop: top edges in the page.
        public static int Bring(int scroll, int view, int cardTop, int rowTop, int rowHeight)
        {
            if (view <= 0) throw new ArgumentOutOfRangeException("view", "a view with no height shows nothing");
            if (rowHeight < 0) throw new ArgumentOutOfRangeException("rowHeight", "a row is never shorter than nothing");
            if (rowTop >= scroll && rowTop + rowHeight <= scroll + view) return scroll;
            if (rowTop + rowHeight <= cardTop + view) return Math.Max(0, cardTop);
            return Math.Max(0, rowTop + rowHeight - view);
        }
    }
    public static class LayoutRules
    {
        // The screen rule the owner asked for on 2026-10-12: on a 1024x768 or 800x600 screen, and on every smaller one,
        // the guide panel with the character and the whole right-hand panel it sits in do not show. A screen that small
        // has no room for a 300 px column beside the page: on the tester's 800x600 screen (GitHub #38) the panel covered
        // the text of the page. The user's own "Show Nagi" choice is untouched by this; it decides on a larger screen.
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
