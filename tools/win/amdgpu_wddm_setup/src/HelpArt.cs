// The pictures of the help cards (HelpCards.cs) in this exe: build.ps1 embeds each file of -HelpArt that a card names
// as help.<file>. A picture that is absent, too large or does not decode leaves its step as text.
using System;
using System.Collections.Generic;
using System.Drawing;
using System.IO;
using System.Reflection;

namespace AmdgpuWddmSetup
{
    public static class HelpArt
    {
        static readonly Dictionary<string, Image> Cache = new Dictionary<string, Image>();

        public static Image Load(string image)
        {
            if (image == null) return null;
            var resource = HelpCards.Resource(image);
            lock (Cache)
            {
                Image img;
                if (Cache.TryGetValue(resource, out img)) return img;
                try
                {
                    using (var s = Assembly.GetExecutingAssembly().GetManifestResourceStream(resource))
                    {
                        if (s == null || s.Length > HelpCards.ImageBytes) return null;
                        var ms = new MemoryStream();
                        s.CopyTo(ms);
                        img = Image.FromStream(ms);
                    }
                }
                catch (Exception) { img = null; }
                if (img != null && (img.Width <= 0 || img.Height <= 0 || img.Width > HelpCards.ImagePixels)) { img.Dispose(); img = null; }
                if (img != null) Cache[resource] = img;
                return img;
            }
        }
    }
}
