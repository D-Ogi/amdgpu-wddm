// The guide character's art in this exe: build.ps1 embeds nagi.<expression>@256.png from
// tools\win\amdgpu_wddm_control\art\nagi, under the same resource names the control app uses, so one map of
// expression to file serves both programs. This window embeds that one size, because its smallest slot is larger
// than the @128 picture (EngineModel.cs, SetupGuide.ArtPixels). The setup window draws one still picture for the screen it shows; it
// never animates. A resource that is absent or does not decode leaves the text-only guide panel, which carries the
// same words.
using System;
using System.Collections.Generic;
using System.Drawing;
using System.IO;
using System.Reflection;

namespace AmdgpuWddmSetup
{
    public static class NagiArt
    {
        static readonly Dictionary<string, Image> Cache = new Dictionary<string, Image>();

        public static bool Has(string expression, int size)
        {
            return Assembly.GetExecutingAssembly().GetManifestResourceInfo(SetupGuide.Resource(expression, size)) != null;
        }

        public static Image Load(string expression, int size)
        {
            var resource = SetupGuide.Resource(expression, size);
            lock (Cache)
            {
                Image img;
                if (Cache.TryGetValue(resource, out img)) return img;
                try
                {
                    using (var s = Assembly.GetExecutingAssembly().GetManifestResourceStream(resource))
                    {
                        if (s == null || s.Length > 4 << 20) return null;
                        var ms = new MemoryStream();
                        s.CopyTo(ms);
                        img = Image.FromStream(ms);
                    }
                }
                catch (Exception) { img = null; }
                if (img != null) Cache[resource] = img;
                return img;
            }
        }
    }
}
