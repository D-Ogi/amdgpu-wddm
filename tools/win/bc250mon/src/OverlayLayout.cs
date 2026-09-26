using System;
using System.Collections.Generic;
using System.Drawing;

namespace Bc250Mon
{
    // Pure geometry shared with the headless inventory/layout check. Keep whole
    // panels together, retaining font size when a second column is available.
    public static class OverlayLayout
    {
        public static int PanelHeight(Graphics graphics, Font font, Panel panel, int valueWidth)
        {
            int height = 27;
            foreach (var row in panel.Rows)
                height += Math.Max(18, (int)graphics.MeasureString(row.Value, font, valueWidth).Height);
            return height;
        }
        public static Point[] Flow(IList<int> heights, int top, int availableHeight, int maxColumns,
                                   out int columns, out int bottom)
        {
            var points = new Point[heights.Count];
            int column = 0, y = top;
            bottom = top;
            for (int i = 0; i < heights.Count; ++i)
            {
                if (y > top && y + heights[i] > availableHeight && column + 1 < maxColumns)
                { ++column; y = top; }
                points[i] = new Point(column, y);
                y += heights[i];
                bottom = Math.Max(bottom, y);
            }
            columns = column + 1;
            return points;
        }
    }
}
