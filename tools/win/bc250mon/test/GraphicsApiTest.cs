using System;
using System.Collections.Generic;
using System.Linq;
using Bc250Mon;

namespace Bc250Mon
{
    public interface IProvider { string Name { get; } TimeSpan Period { get; } void Poll(State state); }
}

// Fake module lists against the real classification, selection and panel rows. No process is opened.
static class GraphicsApiTest
{
    static int checks, failures;
    static void Check(bool value, string name) { checks++; if (!value) { failures++; Console.WriteLine("FAIL " + name); } }
    static List<GraphicsApiProvider.ApiPath> C(params string[] modules)
    {
        return GraphicsApiProvider.Classify(new HashSet<string>(modules, StringComparer.OrdinalIgnoreCase));
    }
    static string One(List<GraphicsApiProvider.ApiPath> r, string api)
    {
        var p = r.FirstOrDefault(a => a.Api == api);
        return p == null ? null : p.Path;
    }
    static int Main()
    {
        // The module names are the ones the current release installs (tools/release/release-sources.json):
        // shell, engine and ICD of one render path, all three, are the evidence for a GPU claim.
        var r = C("kernel32.dll", "dxgi.dll", "d3d12.dll", "d3d12core.dll",
                  "amdgpu_wddm_d3d12.dll", "amdgpu_wddm_vkd3d.dll", "amdgpu_wddm_radv.dll", "d3d11.dll");
        Check(One(r, "D3D12") == "GPU (amdgpu-wddm vkd3d + RADV)" && r[0].Level == Level.Good, "D3D12 on our UMD");
        Check(One(r, "D3D11") == "none of our UMDs loaded", "D3D11 side-load in a D3D12 game makes no claim");
        r = C("d3d12.dll", "amdgpu_wddm_d3d12.dll");
        Check(One(r, "D3D12") == "our D3D12 shell loaded, no engine yet" && r[0].Level == Level.Info, "D3D12 shell alone");
        r = C("d3d12.dll", "amdgpu_wddm_d3d12.dll", "amdgpu_wddm_vkd3d.dll");
        Check(One(r, "D3D12") == "our D3D12 shell and engine loaded, no ICD yet", "D3D12 without the ICD");
        r = C("d3d11.dll", "bc250d3d_router.dll", "amdgpu_wddm_d3d11.dll", "amdgpu_wddm_dxvk.dll", "amdgpu_wddm_radv.dll");
        Check(r.Count == 1 && One(r, "D3D11") == "GPU (amdgpu-wddm DXVK + RADV)" && r[0].Level == Level.Good, "D3D11 app route on GPU");
        r = C("d3d11.dll", "bc250d3d_router.dll", "amdgpu_wddm_d3d11.dll");
        Check(One(r, "D3D11") == "our D3D11 shell loaded, no engine yet", "D3D11 shell alone");
        r = C("d3d11.dll", "bc250d3d_router.dll", "amdgpu_wddm_d3d11.dll", "amdgpu_wddm_dxvk.dll");
        Check(One(r, "D3D11") == "our D3D11 shell and engine loaded, no ICD yet", "D3D11 without the ICD");
        r = C("d3d11.dll", "bc250d3d_router.dll", "bc250d3d.dll");
        Check(One(r, "D3D11") == "CPU (llvmpipe, app route)" && r[0].Level == Level.Warn, "D3D11 on llvmpipe, app route");
        // The same CPU UMD without the router is not the application decision (an injected or hosted load).
        r = C("d3d11.dll", "bc250d3d.dll");
        Check(One(r, "D3D11") == "CPU (llvmpipe)", "the llvmpipe row names no route without the router");
        r = C("d3d11.dll", "bc250d3d_router.dll", "bc250d3d_zink.dll", "amdgpu_wddm_radv.dll");
        Check(One(r, "D3D11") == "GPU (zink, desktop route)", "D3D11 on zink");
        r = C("d3d11.dll", "bc250d3d_router.dll", "bc250d3d_zink.dll");
        Check(One(r, "D3D11") == "our zink UMD loaded, no ICD yet", "zink without the ICD");
        r = C("d3d11.dll", "bc250d3d_router.dll");
        Check(One(r, "D3D11") == "router loaded, no backend yet", "router without backend");
        r = C("d3d12.dll", "d3d10warp.dll");
        Check(One(r, "D3D12") == "CPU (WARP)" && r[0].Level == Level.Warn, "D3D12 on WARP");
        r = C("d3d12.dll", "amdgpu_wddm_d3d12.dll", "d3d10warp.dll");
        Check(One(r, "D3D12") == "CPU (WARP)", "a failed UMD load leaves WARP drawing");
        r = C("d3d9.dll", "bc250umd.dll");
        Check(One(r, "D3D9") == "stub UMD, no D3D9 renderer", "D3D9 on the stub");
        r = C("d3d9.dll", "d3d9on12.dll", "d3d12.dll", "amdgpu_wddm_d3d12.dll", "amdgpu_wddm_vkd3d.dll", "amdgpu_wddm_radv.dll");
        Check(One(r, "D3D9") == "via D3D9On12 (D3D12 path)" && One(r, "D3D12").StartsWith("GPU"), "D3D9On12");
        r = C("vulkan-1.dll", "vulkan_radeon.dll");
        Check(One(r, "Vulkan") == "GPU (RADV ICD)", "Vulkan on RADV");
        // The ICD our D3D shells load is not reached through the Vulkan loader, so it is no Vulkan evidence.
        r = C("d3d11.dll", "amdgpu_wddm_d3d11.dll", "amdgpu_wddm_dxvk.dll", "amdgpu_wddm_radv.dll", "vulkan-1.dll");
        Check(One(r, "Vulkan") == "none of our ICDs loaded" && One(r, "D3D11").StartsWith("GPU"), "our ICD is not a Vulkan claim");
        r = C("d3d10_1.dll");
        Check(r.Count == 1 && r[0].Api == "D3D10", "D3D10 alone");
        // Most OpenGL engines map d3d11.dll or dxgi.dll for the display, so a D3D row must not hide the GL row.
        // Only a row that claims one of our GPU paths does: there the D3D path draws and opengl32 is Microsoft's.
        Check(C("opengl32.dll").Single().Api == "OpenGL", "OpenGL alone");
        Check(One(C("opengl32.dll", "d3d11.dll"), "OpenGL") == "no OpenGL ICD in this package",
              "OpenGL is reported next to a D3D runtime that makes no claim");
        Check(C("opengl32.dll", "d3d12.dll", "amdgpu_wddm_d3d12.dll", "amdgpu_wddm_vkd3d.dll", "amdgpu_wddm_radv.dll")
              .All(a => a.Api != "OpenGL"), "no OpenGL row next to one of our GPU paths");
        Check(C("kernel32.dll", "dxgi.dll").Count == 0, "DXGI alone is no API");

        // 32-bit processes: the x86 payload has the D3D11 and desktop routes under the same file names, and no
        // D3D12 at all (BD-064), so a 32-bit D3D12 application can never reach our driver.
        r = GraphicsApiProvider.Classify(new[] { "d3d11.dll", "amdgpu_wddm_d3d11.dll", "amdgpu_wddm_dxvk.dll", "amdgpu_wddm_radv.dll" }, true);
        Check(One(r, "D3D11") == "GPU (amdgpu-wddm DXVK + RADV)", "x86 D3D11 uses the same module names");
        r = GraphicsApiProvider.Classify(new[] { "d3d12.dll" }, true);
        Check(One(r, "D3D12") == "no x86 D3D12 in this package" && r[0].Level == Level.Warn, "x86 D3D12 is not served");
        Check(One(C("d3d12.dll"), "D3D12") == "none of our UMDs loaded", "x64 D3D12 without a UMD makes no such claim");

        // A translation layer copied next to the application replaces the system runtime: the application is
        // not using the installed Windows driver, whatever else is mapped in it.
        r = GraphicsApiProvider.Classify(new[] { "dxgi.dll", "d3d11.dll", "vulkan-1.dll", "vulkan_radeon.dll" }, false,
                     new[] { "dxgi.dll", "d3d11.dll" });
        Check(One(r, "D3D11") == GraphicsApiProvider.SideLoaded && r[0].Api == "DXGI"
              && One(r, "DXGI") == GraphicsApiProvider.SideLoaded, "side-loaded D3D11 and DXGI");
        Check(r.First(a => a.Api == "D3D11").Level == Level.Warn && One(r, "Vulkan") == "GPU (RADV ICD)",
              "the Vulkan path under a side-loaded runtime is still reported");
        r = GraphicsApiProvider.Classify(new[] { "d3d12.dll", "amdgpu_wddm_d3d12.dll", "amdgpu_wddm_vkd3d.dll", "amdgpu_wddm_radv.dll" },
                     false, new[] { "d3d12.dll" });
        Check(One(r, "D3D12") == GraphicsApiProvider.SideLoaded, "a side-loaded d3d12.dll outranks our modules");
        // The merged D3D11+10 row covers two runtimes: either of them, side-loaded, is a replacement.
        r = GraphicsApiProvider.Classify(new[] { "d3d11.dll", "d3d10.dll", "amdgpu_wddm_d3d11.dll",
                     "amdgpu_wddm_dxvk.dll", "amdgpu_wddm_radv.dll" }, false, new[] { "d3d10.dll" });
        Check(One(r, "D3D11+10") == GraphicsApiProvider.SideLoaded, "a side-loaded d3d10.dll in the merged row");
        r = GraphicsApiProvider.Classify(new[] { "d3d11.dll", "d3d10core.dll" }, false, new[] { "d3d10core.dll" });
        Check(One(r, "D3D11") == GraphicsApiProvider.SideLoaded, "a side-loaded d3d10core.dll (DXVK layout)");
        Check(GraphicsApiProvider.IsRuntime("d3d10core.dll"), "d3d10core.dll is a system runtime name");
        Check(GraphicsApiProvider.IsRuntime("d3d11.dll") && GraphicsApiProvider.IsRuntime("dxgi.dll")
              && !GraphicsApiProvider.IsRuntime("amdgpu_wddm_d3d11.dll") && !GraphicsApiProvider.IsRuntime("vulkan-1.dll"),
              "runtime names that must come from the Windows directory");
        Check(GraphicsApiProvider.IsSystemImage(@"\Device\HarddiskVolume3\Windows\System32\d3d11.dll")
              && GraphicsApiProvider.IsSystemImage(@"\Device\HarddiskVolume3\WINDOWS\SysWOW64\d3d11.dll")
              && !GraphicsApiProvider.IsSystemImage(@"\Device\HarddiskVolume3\Games\witcher3\bin\d3d11.dll")
              && !GraphicsApiProvider.IsSystemImage(null), "system image paths");

        Check(GraphicsApiProvider.IsExcluded("dwm") && GraphicsApiProvider.IsExcluded("SteamWebHelper") && !GraphicsApiProvider.IsExcluded("witcher3"), "exclusions");

        // D3D12 has no implicit WARP fallback: a failed engine or ICD load leaves the shell mapped and nothing
        // else, so an incomplete stack may be grey only while the device can still be starting.
        var stalling = new GraphicsApiProvider.AppReport { Pid = 70, Name = "game.exe", Apis = C("d3d12.dll", "amdgpu_wddm_d3d12.dll") };
        string stalledKey = GraphicsApiProvider.IncompleteKey(stalling);
        Check(stalledKey.Length > 0 && GraphicsApiProvider.IncompleteKey(
                  new GraphicsApiProvider.AppReport { Apis = C("d3d12.dll", "amdgpu_wddm_d3d12.dll", "amdgpu_wddm_vkd3d.dll", "amdgpu_wddm_radv.dll") }).Length == 0,
              "a complete stack has no incomplete state");
        GraphicsApiProvider.Escalate(stalling, TimeSpan.FromSeconds(4));
        Check(One(stalling.Apis, "D3D12") == "our D3D12 shell loaded, no engine yet" && stalling.Apis[0].Level == Level.Info,
              "inside the grace period a starting device stays grey");
        GraphicsApiProvider.Escalate(stalling, GraphicsApiProvider.StalledAfter);
        Check(One(stalling.Apis, "D3D12") == "our D3D12 shell loaded, engine never loaded (load failed)"
              && stalling.Apis[0].Level == Level.Warn, "a persisting incomplete stack is a failed load");
        Check(GraphicsApiProvider.IncompleteKey(stalling).Length == 0, "an escalated row is not escalated twice");
        var noIcd = new GraphicsApiProvider.AppReport { Apis = C("d3d11.dll", "bc250d3d_router.dll", "amdgpu_wddm_d3d11.dll", "amdgpu_wddm_dxvk.dll") };
        GraphicsApiProvider.Escalate(noIcd, TimeSpan.FromMinutes(1));
        Check(One(noIcd.Apis, "D3D11") == "our D3D11 shell and engine loaded, ICD never loaded (load failed)", "a missing ICD escalates too");

        var apps = new List<GraphicsApiProvider.AppReport>
        {
            new GraphicsApiProvider.AppReport { Pid = 30, Name = "notepad.exe" },
            new GraphicsApiProvider.AppReport { Pid = 20, Name = "other.exe", Apis = C("d3d11.dll") },
            new GraphicsApiProvider.AppReport { Pid = 40, Name = "game.exe", Apis = C("d3d12.dll", "amdgpu_wddm_d3d12.dll", "amdgpu_wddm_vkd3d.dll", "amdgpu_wddm_radv.dll") },
            new GraphicsApiProvider.AppReport { Pid = 50, Name = "front.exe", Foreground = true, Apis = C("vulkan-1.dll") },
            new GraphicsApiProvider.AppReport { Pid = 60, Name = "guarded.exe", AccessDenied = true },
        };
        var shown = GraphicsApiProvider.Select(apps);
        Check(shown.Count == 3 && shown[0].Pid == 50 && shown[1].Pid == 40 && shown[2].Pid == 20, "order: foreground, ours, rest; max 3");
        Check(shown.All(a => a.Pid != 30), "no API, not shown");
        // A process we may not open says nothing about 3D; it must never take the slot of a classified app.
        var crowded = new List<GraphicsApiProvider.AppReport>
        {
            new GraphicsApiProvider.AppReport { Pid = 11, Name = "guard1.exe", AccessDenied = true },
            new GraphicsApiProvider.AppReport { Pid = 12, Name = "guard2.exe", AccessDenied = true },
            new GraphicsApiProvider.AppReport { Pid = 13, Name = "guard3.exe", AccessDenied = true },
            new GraphicsApiProvider.AppReport { Pid = 99, Name = "game.exe", Apis = C("d3d11.dll") },
        };
        Check(GraphicsApiProvider.Select(crowded)[0].Pid == 99, "the classified app outranks unreadable processes");

        var panel = new Panel();
        GraphicsApiProvider.AddRows(panel, new[] { apps[2], apps[4] });
        Check(panel.Rows[0].Label == "game.exe" && panel.Rows[0].Value == "D3D12; pid 40", "app row");
        Check(panel.Rows[1].Label == "  D3D12" && panel.Rows[1].Level == Level.Good, "path row");
        Check(panel.Rows[2].Value.StartsWith("API unknown (access)") && panel.Rows[2].Level == Level.Warn, "access denied row");
        panel = new Panel();
        GraphicsApiProvider.AddRows(panel, new List<GraphicsApiProvider.AppReport>());
        Check(panel.Rows.Single().Value == "none with a window", "empty panel row");
        panel = new Panel();
        GraphicsApiProvider.AddRows(panel, new[] { new GraphicsApiProvider.AppReport { Pid = 1, Name = "a-very-long-game-name.exe", Wow64 = true, Foreground = true, Apis = C("d3d11.dll") } });
        Check(panel.Rows[0].Label.Length == 17 && panel.Rows[0].Value.EndsWith("pid 1, x86, foreground"), "long name and tags");

        Console.WriteLine("graphics api: {0} checks, {1} failures", checks, failures);
        return failures == 0 ? 0 : 1;
    }
}
