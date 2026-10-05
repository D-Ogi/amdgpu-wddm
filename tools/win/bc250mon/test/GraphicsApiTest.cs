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
        var r = C("kernel32.dll", "dxgi.dll", "d3d12.dll", "d3d12core.dll", "amdgpu_wddm_d3d12.dll", "d3d11.dll");
        Check(One(r, "D3D12") == "GPU (amdgpu-wddm vkd3d + RADV)" && r[0].Level == Level.Good, "D3D12 on our UMD");
        Check(One(r, "D3D11") == "no UMD loaded", "D3D11 side-load in a D3D12 game makes no claim");
        r = C("d3d11.dll", "bc250d3d_router.dll", "amdgpu_wddm_d3d11.dll", "amdgpu_wddm_dxvk.dll");
        Check(r.Count == 1 && One(r, "D3D11") == "GPU (amdgpu-wddm DXVK + RADV)" && r[0].Level == Level.Good, "D3D11 app route on GPU");
        r = C("d3d11.dll", "bc250d3d_router.dll", "bc250d3d.dll");
        Check(One(r, "D3D11") == "CPU (llvmpipe, desktop route)" && r[0].Level == Level.Warn, "D3D11 on llvmpipe");
        r = C("d3d11.dll", "bc250d3d_router.dll", "bc250d3d_zink.dll");
        Check(One(r, "D3D11") == "GPU (zink, desktop route)", "D3D11 on zink");
        r = C("d3d11.dll", "bc250d3d_router.dll");
        Check(One(r, "D3D11") == "router loaded, no backend yet", "router without backend");
        r = C("d3d12.dll", "d3d10warp.dll");
        Check(One(r, "D3D12") == "CPU (WARP)" && r[0].Level == Level.Warn, "D3D12 on WARP");
        r = C("d3d9.dll", "bc250umd.dll");
        Check(One(r, "D3D9") == "stub UMD, no D3D9 renderer", "D3D9 on the stub");
        r = C("d3d9.dll", "d3d9on12.dll", "d3d12.dll", "amdgpu_wddm_d3d12.dll");
        Check(One(r, "D3D9") == "via D3D9On12 (D3D12 path)" && One(r, "D3D12").StartsWith("GPU"), "D3D9On12");
        r = C("vulkan-1.dll", "vulkan_radeon.dll");
        Check(One(r, "Vulkan") == "GPU (RADV ICD)", "Vulkan on RADV");
        r = C("d3d10_1.dll");
        Check(r.Count == 1 && r[0].Api == "D3D10", "D3D10 alone");
        Check(C("opengl32.dll").Single().Api == "OpenGL" && C("opengl32.dll", "d3d11.dll").All(a => a.Api != "OpenGL"), "OpenGL only without D3D");
        Check(C("kernel32.dll", "dxgi.dll").Count == 0, "DXGI alone is no API");

        Check(GraphicsApiProvider.IsExcluded("dwm") && GraphicsApiProvider.IsExcluded("SteamWebHelper") && !GraphicsApiProvider.IsExcluded("witcher3"), "exclusions");

        var apps = new List<GraphicsApiProvider.AppReport>
        {
            new GraphicsApiProvider.AppReport { Pid = 30, Name = "notepad.exe" },
            new GraphicsApiProvider.AppReport { Pid = 20, Name = "other.exe", Apis = C("d3d11.dll") },
            new GraphicsApiProvider.AppReport { Pid = 40, Name = "game.exe", Apis = C("d3d12.dll", "amdgpu_wddm_d3d12.dll") },
            new GraphicsApiProvider.AppReport { Pid = 50, Name = "front.exe", Foreground = true, Apis = C("vulkan-1.dll") },
            new GraphicsApiProvider.AppReport { Pid = 60, Name = "guarded.exe", AccessDenied = true },
        };
        var shown = GraphicsApiProvider.Select(apps);
        Check(shown.Count == 3 && shown[0].Pid == 50 && shown[1].Pid == 40 && shown[2].Pid == 20, "order: foreground, ours, rest; max 3");
        Check(shown.All(a => a.Pid != 30), "no API, not shown");

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
