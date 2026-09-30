// Local HTTP/JSON API, bound to 127.0.0.1 only: it is reached through SSH, never over the network.
//
//   GET    /state                 everything the overlay shows
//   GET    /telemetry             the GPU line: Tctl, load, GFX clock, VRAM, their sources (TelemetryProvider)
//   GET    /flags                 {"stop": bool}              test scripts poll this (or the STOP file)
//   POST   /status                {"text": "...", "level": "info|good|warn|error"}
//   POST   /log                   {"text": "...", "level": "...", "source": "..."}
//   PUT    /panel/<name>          {"title": "...", "order": 100, "rows": [["label", "value", "level"], ...]}
//   DELETE /panel/<name>
//   GET    /actions               the action table
//   POST   /action/<name>         {arguments}
//   GET    /screenshot            the primary screen as image/png or image/jpeg
//                                 ?scale=0.5 &format=png|jpg &quality=80 &overlay=1|0
//   GET    /screenshot/window     one window, same parameters plus ?handle=0x... or ?title=<substring>
//   GET    /windows               visible top-level windows with a title
using System;
using System.Collections;
using System.Collections.Generic;
using System.Drawing;
using System.Globalization;
using System.IO;
using System.Linq;
using System.Net;
using System.Text;
using System.Threading;
using System.Web.Script.Serialization;

namespace Bc250Mon
{
    public sealed class Api
    {
        public const string Prefix = "http://127.0.0.1:2250/";
        readonly State _state;
        readonly Actions _actions;
        readonly JavaScriptSerializer _json = new JavaScriptSerializer();
        readonly HttpListener _listener = new HttpListener();

        public Api(State state, Actions actions) { _state = state; _actions = actions; }

        public void Start()
        {
            _listener.Prefixes.Add(Prefix);
            _listener.Start();
            new Thread(Loop) { IsBackground = true, Name = "api" }.Start();
        }

        void Loop()
        {
            while (true)
            {
                HttpListenerContext ctx;
                try { ctx = _listener.GetContext(); } catch { return; }
                try { Handle(ctx); }
                catch (Exception e) { Reply(ctx, 500, new { error = e.Message }); }
            }
        }

        static Level ParseLevel(object o)
        {
            Level l;
            return o != null && Enum.TryParse(o.ToString(), true, out l) ? l : Level.Info;
        }

        void Handle(HttpListenerContext ctx)
        {
            var req = ctx.Request;
            string[] path = req.Url.AbsolutePath.Trim('/').Split('/');
            string body = new StreamReader(req.InputStream, Encoding.UTF8).ReadToEnd();
            var args = body.Length > 0 ? _json.Deserialize<Dictionary<string, object>>(body) : new Dictionary<string, object>();
            string method = req.HttpMethod, head = path[0].ToLowerInvariant(), arg = path.Length > 1 ? path[1] : null;

            if (method == "GET" && head == "state")
            {
                var s = _state.Take(State.LogCapacity);
                Reply(ctx, 200, new
                {
                    status = s.Status, statusLevel = s.StatusLevel.ToString(), stop = s.Stop,
                    panels = s.Panels.Select(p => new { name = p.Name, title = p.Title, order = p.Order,
                        rows = p.Rows.Select(r => new[] { r.Label, r.Value, r.Level.ToString() }) }),
                    log = s.Log.Select(l => new { time = l.Time.ToString("s"), level = l.Level.ToString(), source = l.Source, text = l.Text }),
                    telemetry = TelemetryProvider.Describe(_state.Telemetry, DateTime.Now),
                });
            }
            else if (method == "GET" && head == "telemetry") Reply(ctx, 200, TelemetryProvider.Describe(_state.Telemetry, DateTime.Now));
            else if (method == "GET" && head == "flags") Reply(ctx, 200, new { stop = _state.StopRequested });
            else if (method == "GET" && head == "actions") Reply(ctx, 200, _actions.All.Select(a => new { name = a.Name, label = a.Label }));
            else if (method == "POST" && head == "status")
            {
                _state.SetStatus(Convert.ToString(args["text"]), ParseLevel(args.ContainsKey("level") ? args["level"] : null));
                Reply(ctx, 200, new { ok = true });
            }
            else if (method == "POST" && head == "log")
            {
                _state.Log(args.ContainsKey("source") ? Convert.ToString(args["source"]) : "remote",
                           ParseLevel(args.ContainsKey("level") ? args["level"] : null), Convert.ToString(args["text"]));
                Reply(ctx, 200, new { ok = true });
            }
            else if (method == "PUT" && head == "panel" && arg != null)
            {
                var p = new Panel { Name = arg, Title = args.ContainsKey("title") ? Convert.ToString(args["title"]) : arg,
                                    Order = args.ContainsKey("order") ? Convert.ToInt32(args["order"]) : 100 };
                foreach (IList row in (IEnumerable)args["rows"])
                    p.Rows.Add(new Row(Convert.ToString(row[0]), row.Count > 1 ? Convert.ToString(row[1]) : "", ParseLevel(row.Count > 2 ? row[2] : null)));
                _state.SetPanel(p);
                Reply(ctx, 200, new { ok = true });
            }
            else if (method == "DELETE" && head == "panel" && arg != null) { _state.RemovePanel(arg); Reply(ctx, 200, new { ok = true }); }
            else if (method == "POST" && head == "action" && arg != null)
            {
                string error;
                bool ok = _actions.Invoke(arg, args, "api", out error);
                Reply(ctx, ok ? 200 : 400, new { ok, error });
            }
            else if (method == "GET" && head == "windows")
                Reply(ctx, 200, Screenshot.Windows().Select(w => new
                {
                    handle = "0x" + w.Handle.ToInt64().ToString("x8"), title = w.Title, process = w.Process,
                    x = w.Bounds.X, y = w.Bounds.Y, width = w.Bounds.Width, height = w.Bounds.Height, minimized = w.Minimized,
                }));
            else if (method == "GET" && head == "screenshot") Capture(ctx, arg);
            else Reply(ctx, 404, new { error = "no such endpoint" });
        }

        // /screenshot and /screenshot/window. The picture is taken on this thread, so captures never overlap.
        void Capture(HttpListenerContext ctx, string what)
        {
            var q = ctx.Request.QueryString;
            double scale = 0.5;
            if (q["scale"] != null && (!double.TryParse(q["scale"], NumberStyles.Float, CultureInfo.InvariantCulture, out scale) || scale < 0.1 || scale > 1.0))
            { Reply(ctx, 400, new { error = "scale must be between 0.1 and 1.0" }); return; }

            string format = (q["format"] ?? "png").ToLowerInvariant();
            if (format == "jpeg") format = "jpg";
            if (format != "png" && format != "jpg") { Reply(ctx, 400, new { error = "format must be png or jpg" }); return; }

            int quality = 80;
            if (q["quality"] != null && (!int.TryParse(q["quality"], out quality) || quality < 1 || quality > 100))
            { Reply(ctx, 400, new { error = "quality must be between 1 and 100" }); return; }

            bool overlay = q["overlay"] != "0";
            Bitmap shot;
            string subject;
            if (what == null)
            {
                using (var scope = new Screenshot.CaptureScope(overlay))
                {
                    shot = Screenshot.CaptureScreen();
                    subject = "screen" + (overlay ? "" : scope.OverlayHidden ? ", overlay hidden" : ", overlay could not be hidden");
                }
            }
            else if (string.Equals(what, "window", StringComparison.OrdinalIgnoreCase))
            {
                if (q["handle"] == null && q["title"] == null) { Reply(ctx, 400, new { error = "give handle=0x... or title=<substring>" }); return; }
                WindowInfo w;
                try { w = Screenshot.Find(q["handle"], q["title"]); }
                catch (Exception e) { Reply(ctx, 400, new { error = "bad handle: " + e.Message }); return; }
                if (w == null) { Reply(ctx, 404, new { error = "no such window" }); return; }
                string method;
                using (var scope = new Screenshot.CaptureScope(overlay))
                {
                    shot = Screenshot.CaptureWindow(w.Handle, out method);
                    subject = "window '" + w.Title + "' (" + w.Process + ") via " + method
                            + (overlay || method == "printwindow" || scope.OverlayHidden ? "" : ", overlay could not be hidden");
                }
            }
            else { Reply(ctx, 404, new { error = "no such endpoint" }); return; }

            int rawWidth = shot.Width, rawHeight = shot.Height;
            byte[] data;
            string contentType;
            int width, height;
            var scaled = Screenshot.Scale(shot, scale);     // takes ownership of shot
            using (scaled)
            {
                width = scaled.Width; height = scaled.Height;
                data = Screenshot.Encode(scaled, format, quality, out contentType);
            }
            _state.Log("screenshot", Level.Info, string.Format(CultureInfo.InvariantCulture, "{0} {1}x{2} -> {3}x{4} {5} {6} kB",
                subject, rawWidth, rawHeight, width, height, format, Math.Max(1, data.Length / 1024)));

            try
            {
                ctx.Response.StatusCode = 200;
                ctx.Response.ContentType = contentType;
                ctx.Response.Headers["X-Capture-Size"] = width + "x" + height;
                ctx.Response.ContentLength64 = data.Length;
                ctx.Response.OutputStream.Write(data, 0, data.Length);
                ctx.Response.Close();
            }
            catch { }
        }

        void Reply(HttpListenerContext ctx, int code, object payload)
        {
            try
            {
                byte[] data = Encoding.UTF8.GetBytes(_json.Serialize(payload));
                ctx.Response.StatusCode = code;
                ctx.Response.ContentType = "application/json; charset=utf-8";
                ctx.Response.OutputStream.Write(data, 0, data.Length);
                ctx.Response.Close();
            }
            catch { }
        }
    }
}
