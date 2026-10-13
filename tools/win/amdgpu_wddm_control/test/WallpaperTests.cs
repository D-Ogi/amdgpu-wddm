// The Nagi wallpaper (owner, 2026-10-13; src/Wallpaper.cs): the .nwp decoder against a good file, a wrong magic, a
// wrong hash and a truncated file, the release file of art/wallpaper with its two pinned hashes, the rule that sets
// the picture once per install after a verified install, the saved background and its restore message, the
// preference values, and the words of the Settings card in the four languages.
using System;
using System.IO;
using System.Linq;
using System.Security.Cryptography;
using AmdgpuWddmControl;

static partial class UnitTests
{
    static string Hex(byte[] b) { return BitConverter.ToString(b).Replace("-", ""); }

    static void WallpaperTests(string root)
    {
        string problem;
        using (var sha = SHA256.Create())
        {
            Equal("8B8AF7BEFE513BE505997B1438A98FC294DD91E17783FF14FB24E08171C13CE6", Hex(NagiWallpaperFile.Key()), "wallpaper: the key is the SHA-256 of the expansion of the name");

            // A good file of a few bytes, made by the encoder of the tests, and the four ways it can be damaged.
            var plain = Enumerable.Range(0, 100).Select(i => (byte)(i * 7 + 3)).ToArray();
            var good = NagiWallpaperFile.Encode(plain);
            Equal(NagiWallpaperFile.HeaderBytes + plain.Length, good.Length, "wallpaper: the container is the header and the payload");
            var back = NagiWallpaperFile.Decode(good, out problem);
            Check(back != null && back.SequenceEqual(plain) && problem == null, "wallpaper: a good file decodes to its picture");
            Check(!good.Skip(NagiWallpaperFile.HeaderBytes).SequenceEqual(plain), "wallpaper: the payload is not the plain picture");

            var magic = (byte[])good.Clone(); magic[0] = (byte)'X';
            Check(NagiWallpaperFile.Decode(magic, out problem) == null && problem.Contains("not a Nagi wallpaper"), "wallpaper: a wrong magic is refused");
            var hash = (byte[])good.Clone(); hash[NagiWallpaperFile.HeaderBytes + 10] ^= 0x01;
            Check(NagiWallpaperFile.Decode(hash, out problem) == null && problem.Contains("SHA-256"), "wallpaper: a changed byte of the picture is refused (wrong hash)");
            var header = (byte[])good.Clone(); header[9] ^= 0x80;
            Check(NagiWallpaperFile.Decode(header, out problem) == null && problem.Contains("SHA-256"), "wallpaper: a changed hash in the header is refused");
            var cut = good.Take(good.Length - 1).ToArray();
            Check(NagiWallpaperFile.Decode(cut, out problem) == null && problem.Contains("bytes"), "wallpaper: a truncated file is refused");
            var longer = good.Concat(new byte[] { 0 }).ToArray();
            Check(NagiWallpaperFile.Decode(longer, out problem) == null, "wallpaper: a file with bytes after its picture is refused");
            Check(NagiWallpaperFile.Decode(good.Take(20).ToArray(), out problem) == null && problem.Contains("header"), "wallpaper: a file shorter than its header is refused");
            Check(NagiWallpaperFile.Decode(null, out problem) == null, "wallpaper: no file is refused");

            // The release file: the bytes art/wallpaper/PROVENANCE.md records, and the picture inside them.
            var nwp = Path.Combine(root, @"tools\win\amdgpu_wddm_control\art\wallpaper", NagiWallpaperFile.Name);
            Check(File.Exists(nwp), "wallpaper: " + nwp + " is in the repository");
            if (File.Exists(nwp))
            {
                var bytes = File.ReadAllBytes(nwp);
                Equal("69E96B66F0FCB58DA00E2CBB6A07B3630B99D32D8FBD895FE346787ECE8258B2", Hex(sha.ComputeHash(bytes)), "wallpaper: the release container is the file PROVENANCE.md pins");
                var picture = NagiWallpaperFile.Decode(bytes, out problem);
                Check(picture != null, "wallpaper: the release container decodes" + (problem != null ? " (" + problem + ")" : ""));
                if (picture != null)
                {
                    Equal("90A41BB7EFE615BDBFFA2FF3D90A17B99D732553424E83E23B4BC0477607D605", Hex(sha.ComputeHash(picture)), "wallpaper: the picture inside is the JPEG PROVENANCE.md pins");
                    Check(picture.Length > 3 && picture[0] == 0xFF && picture[1] == 0xD8 && picture[2] == 0xFF, "wallpaper: the picture inside is a JPEG");
                }
                // No plain picture next to it in the repository.
                Check(!Directory.GetFiles(Path.GetDirectoryName(nwp)).Any(f => new[] { ".jpg", ".jpeg", ".png", ".webp", ".bmp" }.Contains(Path.GetExtension(f).ToLowerInvariant())),
                    "wallpaper: the art folder holds no plain picture");
            }
        }

        // The rule: once per install stamp, after the installer's state says verified for the installed release.
        Equal(WallpaperDue.Due, NagiWallpaper.Decide(1, "1.0 20261013T120000Z", null, "verified", "1.0", "1.0"), "wallpaper: chosen, new stamp, verified: due");
        Equal(WallpaperDue.Due, NagiWallpaper.Decide(1u, "1.0 s", "1.0 older", "verified", "1.0", "1.0"), "wallpaper: a new install stamp is due again");
        Equal(WallpaperDue.AlreadySet, NagiWallpaper.Decide(1, "1.0 s", "1.0 s", "verified", "1.0", "1.0"), "wallpaper: not twice for one install");
        Equal(WallpaperDue.NotChosen, NagiWallpaper.Decide(0, "1.0 s", null, "verified", "1.0", "1.0"), "wallpaper: a cleared box sets nothing");
        Equal(WallpaperDue.NotChosen, NagiWallpaper.Decide(null, "1.0 s", null, "verified", "1.0", "1.0"), "wallpaper: no record (an older installer) sets nothing");
        Equal(WallpaperDue.NotChosen, NagiWallpaper.Decide(1, "", null, "verified", "1.0", "1.0"), "wallpaper: no stamp sets nothing");
        Equal(WallpaperDue.NotChosen, NagiWallpaper.Decide("1", "1.0 s", null, "verified", "1.0", "1.0"), "wallpaper: a value of another type is not a choice");
        foreach (var phase in new[] { null, "files-copied", "driver-installed", "driver-pending-restart", "verify-failed", "install-incomplete" })
            Equal(WallpaperDue.NotVerified, NagiWallpaper.Decide(1, "1.0 s", null, phase, "1.0", "1.0"), "wallpaper: phase " + (phase ?? "none") + " is not a verified install");
        Equal(WallpaperDue.NotVerified, NagiWallpaper.Decide(1, "1.1 s", null, "verified", "1.0", "1.1"), "wallpaper: a verification of another release does not count");
        Equal(WallpaperDue.NotVerified, NagiWallpaper.Decide(1, "1.0 s", null, "verified", "1.0", null), "wallpaper: no installed release, no wallpaper");
        Check(NagiWallpaper.SignInWaitSeconds <= 300 && NagiWallpaper.SignInPollMs >= 1000, "wallpaper: the sign-in run waits at most five minutes and polls gently");

        // The saved background.
        Equal(BackgroundKind.Picture, NagiWallpaper.KindOf(0, "C:\\a.jpg"), "wallpaper: BackgroundType 0 is a picture");
        Equal(BackgroundKind.Solid, NagiWallpaper.KindOf(1, ""), "wallpaper: BackgroundType 1 is a solid colour");
        Equal(BackgroundKind.Slideshow, NagiWallpaper.KindOf(2, "C:\\x"), "wallpaper: BackgroundType 2 is a slideshow");
        Equal(BackgroundKind.Spotlight, NagiWallpaper.KindOf(3, "C:\\x"), "wallpaper: BackgroundType 3 is Windows Spotlight");
        Equal(BackgroundKind.Unknown, NagiWallpaper.KindOf(7, "C:\\x"), "wallpaper: another BackgroundType is unknown");
        Equal(BackgroundKind.Picture, NagiWallpaper.KindOf(null, "C:\\a.jpg"), "wallpaper: without BackgroundType a path is a picture");
        Equal(BackgroundKind.Solid, NagiWallpaper.KindOf(null, ""), "wallpaper: without BackgroundType no path is a solid colour");
        Check(NagiWallpaper.KeepsCopy(BackgroundKind.Slideshow, "C:\\x.jpg") && NagiWallpaper.KeepsCopy(BackgroundKind.Spotlight, "C:\\x.jpg") &&
            NagiWallpaper.KeepsCopy(BackgroundKind.Picture, @"C:\Users\u\AppData\Roaming\Microsoft\Windows\Themes\TranscodedWallpaper") &&
            !NagiWallpaper.KeepsCopy(BackgroundKind.Picture, "C:\\Pictures\\mine.jpg"), "wallpaper: a picture Windows rewrites is copied before it is replaced, the person's own file is not");
        Equal("settings.wallpaper.nothing-saved", NagiWallpaper.RestoreMessageId(null, false), "wallpaper: nothing saved says so");
        Equal("settings.wallpaper.restored", NagiWallpaper.RestoreMessageId(new SavedBackground { Kind = BackgroundKind.Picture }, true), "wallpaper: a picture comes back");
        Equal("settings.wallpaper.restore-failed", NagiWallpaper.RestoreMessageId(new SavedBackground { Kind = BackgroundKind.Picture }, false), "wallpaper: a missing picture says so");
        foreach (var k in new[] { BackgroundKind.Slideshow, BackgroundKind.Spotlight })
            foreach (var done in new[] { true, false })
                Equal("settings.wallpaper.restored-partial", NagiWallpaper.RestoreMessageId(new SavedBackground { Kind = k }, done), "wallpaper: a " + k + " says plainly that only a picture comes back");
        foreach (var pos in new[] { NagiWallpaper.PositionCenter, NagiWallpaper.PositionTile, NagiWallpaper.PositionStretch, NagiWallpaper.PositionFit, NagiWallpaper.PositionFill, NagiWallpaper.PositionSpan })
            Equal(pos, NagiWallpaper.PositionOf(NagiWallpaper.StyleOf(pos), pos == NagiWallpaper.PositionTile ? "1" : "0"), "wallpaper: position " + pos + " survives WallpaperStyle and TileWallpaper");
        Equal("10", NagiWallpaper.StyleOf(NagiWallpaper.PositionFill), "wallpaper: Fill is WallpaperStyle 10");
        var saved = new SavedBackground { Kind = BackgroundKind.Spotlight, Path = @"C:\x\previous-wallpaper.jpg", Style = "10", Tile = "0", Color = "0 0 0" };
        var again = SavedBackground.FromValue(saved.ToValue());
        Check(again != null && again.Kind == saved.Kind && again.Path == saved.Path && again.Style == "10" && again.Tile == "0" && again.Color == "0 0 0", "wallpaper: the saved background survives its value");
        var empty = SavedBackground.FromValue(new SavedBackground { Kind = BackgroundKind.Solid }.ToValue());
        Check(empty != null && empty.Kind == BackgroundKind.Solid && empty.Path == "", "wallpaper: empty fields survive their value");
        Check(SavedBackground.FromValue(null) == null && SavedBackground.FromValue("picture|a") == null && SavedBackground.FromValue("other|a|b|c|d") == null && SavedBackground.FromValue("2|a|b|c|d") == null,
            "wallpaper: a damaged value is no saved background");
        Check(NagiWallpaper.SamePath(@"C:\A\b.jpg", @"c:\a\B.JPG") && !NagiWallpaper.SamePath(@"C:\A\b.jpg", "") && !NagiWallpaper.SamePath(null, null), "wallpaper: our own picture is recognised by its path");

        // The preferences: written and removed again without leaving anything behind (R4).
        var store = new MemoryPrefStore();
        var p = new AppPrefs(store);
        Check(p.WallpaperApplied == null && p.WallpaperPrevious == null, "wallpaper: nothing is stored before the first run");
        p.WallpaperApplied = "1.0 s";
        p.WallpaperPrevious = saved;
        Equal("1.0 s", p.WallpaperApplied, "wallpaper: the stamp is stored");
        Check(p.WallpaperPrevious != null && p.WallpaperPrevious.Path == saved.Path, "wallpaper: the previous background is stored");
        p.WallpaperApplied = null; p.WallpaperPrevious = null;
        Equal(0, store.Values.Count, "wallpaper: removing both leaves no value");

        // The words of the Settings card and of the restore results, in every language.
        foreach (var lang in Strings.Languages)
            foreach (var id in new[] { "search.settings.wallpaper", "search.settings.wallpaper.terms", "settings.wallpaper.note", "settings.wallpaper.set", "settings.wallpaper.restore",
                "settings.wallpaper.done", "settings.wallpaper.failed", "settings.wallpaper.restored", "settings.wallpaper.restored-partial", "settings.wallpaper.restore-failed",
                "settings.wallpaper.nothing-saved", "help.setting.wallpaper" })
                Check(!string.IsNullOrEmpty(Strings.In(lang, id)), "wallpaper: " + lang + " has " + id);
        Check(SettingsSearch.Index.Any(e => e.Id == "settings.wallpaper"), "wallpaper: the settings search finds the card");
    }
}
