// Local HTTP/JSON API, bound to 127.0.0.1 only: it is reached through SSH, never over the network.
//
//   GET    /state                 everything the overlay shows
//   GET    /flags                 {"stop": bool}              test scripts poll this (or the STOP file)
//   POST   /status                {"text": "...", "level": "info|good|warn|error"}
//   POST   /log                   {"text": "...", "level": "...", "source": "..."}
//   PUT    /panel/<name>          {"title": "...", "order": 100, "rows": [["label", "value", "level"], ...]}
//   DELETE /panel/<name>
//   GET    /actions               the action table
//   POST   /action/<name>         {arguments}
using System;
using System.Collections;
using System.Collections.Generic;
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
                });
            }
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
            else Reply(ctx, 404, new { error = "no such endpoint" });
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
