using System;
using System.Drawing;
using System.IO;
using System.Linq;
using System.Web.Script.Serialization;
using Bc250Mon;

// No Program.Main, WinForms window, providers thread or GPU access is started.
namespace Bc250Mon { public interface IProvider { string Name { get; } TimeSpan Period { get; } void Poll(State state); } }
static class VulkanInventoryTest
{
    public sealed class CaptureState
    {
        public string status { get; set; }
        public bool stop { get; set; }
        public CapturePanel[] panels { get; set; }
        public object[] log { get; set; }
    }
    public sealed class CapturePanel
    {
        public string name { get; set; }
        public string title { get; set; }
        public int order { get; set; }
        public string[][] rows { get; set; }
    }
    static void ReplayActual(string inventoryPath, string statePath, string output)
    {
        var inventory = VulkanInventoryProvider.Parse(File.ReadAllText(inventoryPath));
        var capture = new JavaScriptSerializer().Deserialize<CaptureState>(File.ReadAllText(statePath));
        var panels = capture.panels.Where(p => p.name != "kmdinfo" && p.name != "vulkan").Select(p => new Panel {
            Name = p.name, Title = p.title, Order = p.order,
            Rows = p.rows.Select(r => new Row(r[0], r[1], (Level)Enum.Parse(typeof(Level), r[2]))).ToList()
        }).ToList();
        var currentKmd = capture.panels.Single(p => p.name == "kmdinfo").rows.Single(r => r[0] == "Version")[1].Split(' ')[0];
        var vk = VulkanInventoryProvider.BuildPanel(inventory, null, DateTime.UtcNow, currentKmd);
        panels.Add(vk); panels = panels.OrderBy(p => p.Order).ToList();
        File.WriteAllText(Path.Combine(output, "actual-inventory-panel.json"), new JavaScriptSerializer().Serialize(vk));
        using (var bitmap = new Bitmap(1920, 1200))
        using (var graphics = Graphics.FromImage(bitmap))
        using (var font = new Font("Consolas", 13.3f, GraphicsUnit.Pixel))
        using (var small = new Font("Consolas", 11.3f, GraphicsUnit.Pixel))
        {
            graphics.TextRenderingHint = System.Drawing.Text.TextRenderingHint.ClearTypeGridFit;
            int top = 36 + (capture.stop ? 30 : 0) + (int)graphics.MeasureString(capture.status, font, 432).Height + 8;
            var heights = panels.Select(p => OverlayLayout.PanelHeight(graphics, font, p, 302)).ToList();
            heights.Add(19 + Math.Min(capture.log.Length, 9) * 15 + 28);
            int columns, bottom;
            var places = OverlayLayout.Flow(heights, top, 1140, 4, out columns, out bottom);
            Check(bottom <= 1140 && columns * 460 <= 1896, "actual captured panels fit 1920x1200");
            graphics.Clear(Color.FromArgb(16, 18, 22));
            graphics.DrawString("HEADLESS LAYOUT REPLAY - not a lab screenshot", small, Brushes.White, 14, 10);
            for (int i = 0; i < panels.Count; ++i)
            {
                int x = 14 + places[i].X * 460, y = places[i].Y;
                graphics.DrawString(panels[i].Title, small, Brushes.LightBlue, x, y); y += 19;
                foreach (var row in panels[i].Rows)
                {
                    graphics.DrawString(row.Label, font, Brushes.Gray, x, y);
                    graphics.DrawString(row.Value, font, row.Level == Level.Warn ? Brushes.Gold : Brushes.White, new RectangleF(x + 130, y, 302, 400));
                    y += Math.Max(18, (int)graphics.MeasureString(row.Value, font, 302).Height);
                }
                Console.WriteLine("Actual panel {0}: column {1}, y {2}, height {3}", panels[i].Name, places[i].X, places[i].Y, heights[i]);
            }
            graphics.DrawString("LOG / existing hotkey footer", small, Brushes.Gray, 14 + places[panels.Count].X * 460, places[panels.Count].Y);
            using (var crop = bitmap.Clone(new Rectangle(0, 0, columns * 460, bottom), bitmap.PixelFormat))
                crop.Save(Path.Combine(output, "actual-layout-replay.png"), System.Drawing.Imaging.ImageFormat.Png);
            Console.WriteLine("Actual replay: {0}, {1} columns, {2}x{3} logical pixels", inventory.Status, columns, columns * 460, bottom);
        }
    }
    static int checks;
    static void Check(bool condition, string name) { ++checks; if (!condition) throw new Exception(name); }
    static string Value(Panel panel, string label) { return panel.Rows.Single(r => r.Label == label).Value; }
    static int Main(string[] args)
    {
        try
        {
            string fixtures = args[0], output = args[1];
            Directory.CreateDirectory(output);
            string goodJson = File.ReadAllText(Path.Combine(fixtures, "vulkan-ok.json"));
            var good = VulkanInventoryProvider.Parse(goodJson);
            var now = new DateTime(2026, 9, 24, 12, 5, 0, DateTimeKind.Utc);
            var p = VulkanInventoryProvider.BuildPanel(good, null, now, "0.7.136.1");
            Check(Value(p, "Advertised").Contains("sparse:no"), "explicit false stays false");
            Check(Value(p, "Advertised").Contains("FP16:yes"), "positive feature");
            Check(Value(p, "Formats") == "100/200 supported; storage 40, color 70", "format counts");
            Check(Value(p, "ICD observed") == good.IcdLibraryPath, "observed ICD identity");
            Check(Value(p, "Capture").Contains("5m 0s ago"), "capture age");
            Check(!Value(VulkanInventoryProvider.BuildPanel(good, null, now, "0x00070088"), "Capture").Contains("KMD changed"), "packed current version equals DriverVer");
            Check(Value(VulkanInventoryProvider.BuildPanel(good, null, now, "0x0007008B"), "Capture").Contains("KMD changed"), "packed newer revision detected");
            Check(!Value(VulkanInventoryProvider.BuildPanel(good, null, now, "not available"), "Capture").Contains("KMD changed"), "unknown version not a change");
            var unknown = VulkanInventoryProvider.Parse(File.ReadAllText(Path.Combine(fixtures, "vulkan-unknown.json")));
            p = VulkanInventoryProvider.BuildPanel(unknown, null, now, null);
            Check(Value(p, "Advertised").Contains("sparse:?") && !Value(p, "Advertised").Contains(":no"), "unknown not unsupported");
            Check(Value(p, "Formats").Contains("?/?"), "unknown counts not zero");
            Check(Value(p, "ICD requested") == good.IcdPath, "unverified ICD not observed");
            var failure = VulkanInventoryProvider.Parse(File.ReadAllText(Path.Combine(fixtures, "vulkan-error.json")));
            p = VulkanInventoryProvider.BuildPanel(failure, null, now, null);
            Check(Value(p, "Result").Contains("timeout"), "failed capture visible");
            Check(!p.Rows.Any(r => r.Label == "Advertised"), "no capabilities from failed capture");
            Check(Value(p, "Full report") == "vulkan-inventory.txt", "partial report retained");
            var partial = VulkanInventoryProvider.Parse(File.ReadAllText(Path.Combine(fixtures, "vulkan-partial.json")));
            p = VulkanInventoryProvider.BuildPanel(partial, null, now, null);
            Check(Value(p, "Partial capture").Contains("timeout"), "partial capture warning");
            Check(Value(p, "ICD observed").Contains("actual-system"), "observed ICD differs from requested");
            Check(Value(p, "Formats").Contains("?/?"), "truncated formats unknown");
            Check(Value(p, "Advertised").Contains("FP16:yes"), "partial known features retained");
            failure.Status = "unsupported";
            Check(VulkanInventoryProvider.BuildPanel(failure, null, now, null).Rows.Single(r => r.Label == "Result").Level == Level.Warn, "unsupported distinct");
            p = VulkanInventoryProvider.BuildPanel(good, null, now.AddDays(2), "0.7.139.1");
            Check(p.Rows[0].Level == Level.Warn && p.Rows[0].Value.Contains("KMD changed"), "stale version warning");
            bool rejected = false;
            try { VulkanInventoryProvider.Parse(goodJson.Replace("\"SchemaVersion\": 1", "\"SchemaVersion\": 99")); }
            catch (InvalidDataException) { rejected = true; }
            Check(rejected, "unknown schema rejected");
            Environment.SetEnvironmentVariable("BC250MON_VULKAN_INVENTORY", null);
            string cache = Path.Combine(output, "vulkan-inventory.json");
            if (File.Exists(cache)) File.Delete(cache);
            var state = new State(output);
            var provider = new VulkanInventoryProvider(output);
            provider.Poll(state);
            Check(Value(state.Take(0).Panels.Single(), "Capture") == "not captured", "missing file");
            File.WriteAllText(cache, goodJson); provider.Poll(state);
            Check(state.Take(0).Panels.Single().Rows.Any(r => r.Label == "Formats"), "file positive control");
            File.WriteAllText(cache, "{"); provider.Poll(state);
            Check(!state.Take(0).Panels.Single().Rows.Any(r => r.Label == "Formats"), "malformed replacement clears old success");
            File.WriteAllText(cache, goodJson); provider.Poll(state);
            Check(state.Take(0).Panels.Single().Rows.Any(r => r.Label == "Formats"), "valid replacement recovers");
            File.Delete(cache); provider.Poll(state);
            Check(Value(state.Take(0).Panels.Single(), "Capture") == "not captured", "removed capture cleared");
            using (var bitmap = new Bitmap(1920, 1200))
            using (var graphics = Graphics.FromImage(bitmap))
            using (var font = new Font("Consolas", 13.3f, GraphicsUnit.Pixel))
            {
                p = VulkanInventoryProvider.BuildPanel(good, null, now, null);
                int inventoryHeight = OverlayLayout.PanelHeight(graphics, font, p, 302);
                // Representative existing panels + measured real inventory + nine logs/footer.
                int[] heights = { 99, 153, 423, inventoryHeight, 99, 182 };
                int columns, bottom;
                var positions = OverlayLayout.Flow(heights, 70, 1140, 4, out columns, out bottom);
                Check(columns == 2 && bottom <= 1140, "1920x1200 column flow");
                for (int i = 0; i < heights.Length; ++i)
                    Check(positions[i].Y >= 70 && positions[i].Y + heights[i] <= 1140, "panel fits " + i);
                Check(columns * 460 <= 1896, "columns fit screen width");
                Console.WriteLine("Layout: inventory height {0}px; {1} columns, {2}x{3}px logical", inventoryHeight, columns, columns * 460, bottom);
            }
            if (args.Length >= 4) ReplayActual(args[2], args[3], output);
            Console.WriteLine("Vulkan inventory: {0} checks passed", checks);
            return 0;
        }
        catch (Exception e) { Console.Error.WriteLine(e); return 1; }
    }
}
