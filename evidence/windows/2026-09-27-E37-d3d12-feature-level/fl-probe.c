/* fl-probe: print the D3D12 feature level and the option tiers vkd3d-proton (or any d3d12.dll next to the
 * executable) reports for every DXGI adapter, as one JSON document on stdout. No rendering, no files.
 * Exit 0 when at least one device was created, 2 otherwise. */
#define COBJMACROS
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <initguid.h>
#include <dxgi1_6.h>
#include <d3d12.h>
#include <stdio.h>

static const char *fl_name(D3D_FEATURE_LEVEL fl)
{
   switch (fl) {
   case D3D_FEATURE_LEVEL_11_0: return "11_0";
   case D3D_FEATURE_LEVEL_11_1: return "11_1";
   case D3D_FEATURE_LEVEL_12_0: return "12_0";
   case D3D_FEATURE_LEVEL_12_1: return "12_1";
   case D3D_FEATURE_LEVEL_12_2: return "12_2";
   default: return "unknown";
   }
}

static void print_module(const char *name)
{
   char path[MAX_PATH] = "";
   HMODULE m = GetModuleHandleA(name);
   if (m)
      GetModuleFileNameA(m, path, sizeof path);
   for (char *p = path; *p; p++)
      if (*p == '\\')
         *p = '/';
   printf("  \"%s\": \"%s\",\n", name, path);
}

int main(void)
{
   IDXGIFactory4 *factory = NULL;
   int devices = 0;
   HRESULT hr = CreateDXGIFactory2(0, &IID_IDXGIFactory4, (void **)&factory);
   printf("{\n");
   if (FAILED(hr)) {
      printf("  \"error\": \"CreateDXGIFactory2\", \"hr\": \"0x%08lx\"\n}\n", (unsigned long)hr);
      return 2;
   }
   print_module("dxgi.dll");
   printf("  \"adapters\": [\n");
   for (UINT i = 0;; i++) {
      IDXGIAdapter1 *adapter = NULL;
      DXGI_ADAPTER_DESC1 desc;
      ID3D12Device *dev = NULL;
      if (IDXGIFactory4_EnumAdapters1(factory, i, &adapter) != S_OK)
         break;
      IDXGIAdapter1_GetDesc1(adapter, &desc);
      printf("%s    {\n      \"index\": %u, \"description\": \"%ls\", \"vendor\": \"0x%04x\", \"device\": \"0x%04x\",\n"
             "      \"software\": %s, \"dedicated_video_memory\": %llu, \"shared_system_memory\": %llu,\n",
             i ? ",\n" : "", i, desc.Description, desc.VendorId, desc.DeviceId,
             (desc.Flags & DXGI_ADAPTER_FLAG_SOFTWARE) ? "true" : "false",
             (unsigned long long)desc.DedicatedVideoMemory, (unsigned long long)desc.SharedSystemMemory);
      hr = D3D12CreateDevice((IUnknown *)adapter, D3D_FEATURE_LEVEL_11_0, &IID_ID3D12Device, (void **)&dev);
      if (FAILED(hr)) {
         printf("      \"create_device_hr\": \"0x%08lx\"\n    }", (unsigned long)hr);
         IDXGIAdapter1_Release(adapter);
         continue;
      }
      devices++;
      printf("      \"create_device_hr\": \"0x%08lx\",\n", (unsigned long)hr);

      {
         D3D_FEATURE_LEVEL levels[] = { D3D_FEATURE_LEVEL_11_0, D3D_FEATURE_LEVEL_11_1, D3D_FEATURE_LEVEL_12_0,
                                        D3D_FEATURE_LEVEL_12_1, D3D_FEATURE_LEVEL_12_2 };
         D3D12_FEATURE_DATA_FEATURE_LEVELS fls = { 5, levels, 0 };
         hr = ID3D12Device_CheckFeatureSupport(dev, D3D12_FEATURE_FEATURE_LEVELS, &fls, sizeof fls);
         printf("      \"feature_levels_hr\": \"0x%08lx\", \"max_feature_level\": \"%s\",\n",
                (unsigned long)hr, SUCCEEDED(hr) ? fl_name(fls.MaxSupportedFeatureLevel) : "n/a");
      }
      {
         /* Highest shader model the runtime accepts: ask downwards until it answers. */
         static const D3D_SHADER_MODEL models[] = { (D3D_SHADER_MODEL)0x68, (D3D_SHADER_MODEL)0x67,
                                                    (D3D_SHADER_MODEL)0x66, (D3D_SHADER_MODEL)0x65,
                                                    (D3D_SHADER_MODEL)0x60, (D3D_SHADER_MODEL)0x51 };
         D3D12_FEATURE_DATA_SHADER_MODEL sm = { 0 };
         hr = E_FAIL;
         for (size_t k = 0; k < sizeof models / sizeof models[0] && FAILED(hr); k++) {
            sm.HighestShaderModel = models[k];
            hr = ID3D12Device_CheckFeatureSupport(dev, D3D12_FEATURE_SHADER_MODEL, &sm, sizeof sm);
         }
         printf("      \"shader_model_hr\": \"0x%08lx\", \"highest_shader_model\": \"0x%02x\",\n",
                (unsigned long)hr, SUCCEEDED(hr) ? (unsigned)sm.HighestShaderModel : 0u);
      }
      {
         D3D12_FEATURE_DATA_D3D12_OPTIONS o = { 0 };
         hr = ID3D12Device_CheckFeatureSupport(dev, D3D12_FEATURE_D3D12_OPTIONS, &o, sizeof o);
         printf("      \"options_hr\": \"0x%08lx\",\n"
                "      \"tiled_resources_tier\": %d, \"resource_binding_tier\": %d, \"conservative_rasterization_tier\": %d,\n"
                "      \"rovs_supported\": %s, \"typed_uav_load_additional_formats\": %s, \"output_merger_logic_op\": %s,\n"
                "      \"resource_heap_tier\": %d, \"double_precision\": %s, \"min_precision\": %d,\n",
                (unsigned long)hr, (int)o.TiledResourcesTier, (int)o.ResourceBindingTier,
                (int)o.ConservativeRasterizationTier, o.ROVsSupported ? "true" : "false",
                o.TypedUAVLoadAdditionalFormats ? "true" : "false", o.OutputMergerLogicOp ? "true" : "false",
                (int)o.ResourceHeapTier, o.DoublePrecisionFloatShaderOps ? "true" : "false",
                (int)o.MinPrecisionSupport);
      }
      {
         D3D12_FEATURE_DATA_D3D12_OPTIONS1 o1 = { 0 };
         hr = ID3D12Device_CheckFeatureSupport(dev, D3D12_FEATURE_D3D12_OPTIONS1, &o1, sizeof o1);
         printf("      \"options1_hr\": \"0x%08lx\", \"wave_ops\": %s, \"wave_lane_count_min\": %u, \"wave_lane_count_max\": %u, \"total_lane_count\": %u,\n",
                (unsigned long)hr, o1.WaveOps ? "true" : "false", o1.WaveLaneCountMin, o1.WaveLaneCountMax,
                o1.TotalLaneCount);
      }
      {
         D3D12_FEATURE_DATA_D3D12_OPTIONS5 o5 = { 0 };
         hr = ID3D12Device_CheckFeatureSupport(dev, D3D12_FEATURE_D3D12_OPTIONS5, &o5, sizeof o5);
         printf("      \"options5_hr\": \"0x%08lx\", \"raytracing_tier\": %d, \"render_passes_tier\": %d,\n",
                (unsigned long)hr, (int)o5.RaytracingTier, (int)o5.RenderPassesTier);
      }
      {
         D3D12_FEATURE_DATA_D3D12_OPTIONS6 o6 = { 0 };
         hr = ID3D12Device_CheckFeatureSupport(dev, D3D12_FEATURE_D3D12_OPTIONS6, &o6, sizeof o6);
         printf("      \"options6_hr\": \"0x%08lx\", \"variable_shading_rate_tier\": %d,\n",
                (unsigned long)hr, (int)o6.VariableShadingRateTier);
      }
      {
         D3D12_FEATURE_DATA_D3D12_OPTIONS7 o7 = { 0 };
         hr = ID3D12Device_CheckFeatureSupport(dev, D3D12_FEATURE_D3D12_OPTIONS7, &o7, sizeof o7);
         printf("      \"options7_hr\": \"0x%08lx\", \"mesh_shader_tier\": %d, \"sampler_feedback_tier\": %d,\n",
                (unsigned long)hr, (int)o7.MeshShaderTier, (int)o7.SamplerFeedbackTier);
      }
      {
         D3D12_FEATURE_DATA_D3D12_OPTIONS12 o12 = { 0 };
         hr = ID3D12Device_CheckFeatureSupport(dev, D3D12_FEATURE_D3D12_OPTIONS12, &o12, sizeof o12);
         printf("      \"options12_hr\": \"0x%08lx\", \"enhanced_barriers\": %s\n    }",
                (unsigned long)hr, o12.EnhancedBarriersSupported ? "true" : "false");
      }
      ID3D12Device_Release(dev);
      IDXGIAdapter1_Release(adapter);
   }
   printf("\n  ],\n");
   print_module("d3d12.dll");
   print_module("d3d12core.dll");
   print_module("vulkan-1.dll");
   printf("  \"devices_created\": %d\n}\n", devices);
   IDXGIFactory4_Release(factory);
   return devices ? 0 : 2;
}
