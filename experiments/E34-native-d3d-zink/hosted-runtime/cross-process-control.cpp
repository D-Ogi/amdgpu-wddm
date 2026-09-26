#include <windows.h>
#include <d3d11.h>
#include <dxgi1_2.h>
#include <wrl/client.h>
#include <cstdio>
#include <cstdlib>
#include <cwchar>
#include <stdexcept>
#include <cstdint>

using Microsoft::WRL::ComPtr;
static void require(bool value, const char* message) {
   if (!value) throw std::runtime_error(message);
}
static void check(HRESULT hr, const char* operation) {
   if (FAILED(hr)) {
      printf("pid=%lu %s=%08lx\n", GetCurrentProcessId(), operation, hr);
      throw std::runtime_error(operation);
   }
}
struct Handle {
   HANDLE h = nullptr;
   explicit Handle(HANDLE value = nullptr) : h(value) {}
   ~Handle() { if (h && h != INVALID_HANDLE_VALUE) CloseHandle(h); }
   Handle(const Handle&) = delete;
   Handle& operator=(const Handle&) = delete;
};
enum Command : LONG { Open = 1, Exchange, Close, Exit };
struct Shared {
   LONG command;
   LONG status;
   HANDLE texture;
   UINT width, height, iteration;
   uint64_t checkedPixels;
};
struct Device {
   ComPtr<ID3D11Device> device;
   ComPtr<ID3D11DeviceContext> context;
   ComPtr<ID3D11Query> completion;
   explicit Device(bool baseline) {
      if (!baseline) SetEnvironmentVariableA("BC250_D3D_RUNTIME_PROBE", "1");
      ComPtr<IDXGIFactory1> factory;
      check(CreateDXGIFactory1(IID_PPV_ARGS(&factory)), "CreateDXGIFactory1");
      ComPtr<IDXGIAdapter1> adapter;
      for (UINT i = 0; ; ++i) {
         ComPtr<IDXGIAdapter1> candidate;
         HRESULT hr = factory->EnumAdapters1(i, &candidate);
         if (hr == DXGI_ERROR_NOT_FOUND) break;
         check(hr, "EnumAdapters1");
         DXGI_ADAPTER_DESC1 desc = {};
         check(candidate->GetDesc1(&desc), "GetDesc1");
         if (desc.VendorId == 0x1002 && desc.DeviceId == 0x13fe) {
            require(!adapter, "ambiguous adapter");
            adapter = candidate;
            uint64_t luid = 0; memcpy(&luid, &desc.AdapterLuid, sizeof(luid));
            char text[32]; sprintf_s(text, "%016llx", static_cast<unsigned long long>(luid));
            SetEnvironmentVariableA("BC250_D3D_ZINK_LUID", text);
         }
      }
      require(adapter != nullptr, "adapter missing");
      D3D_FEATURE_LEVEL level = D3D_FEATURE_LEVEL_10_0;
      check(D3D11CreateDevice(adapter.Get(), D3D_DRIVER_TYPE_UNKNOWN, nullptr, 0,
         &level, 1, D3D11_SDK_VERSION, &device, nullptr, &context), "D3D11CreateDevice");
      const char* name = baseline ? "bc250d3d.dll" : "bc250d3d_zink.dll";
      HMODULE module = GetModuleHandleA(name); require(module != nullptr, "UMD witness missing");
      char path[MAX_PATH] = {}; GetModuleFileNameA(module, path, MAX_PATH);
      printf("pid=%lu UMD=%s\n", GetCurrentProcessId(), path);
      if (!baseline) require(GetModuleHandleA("vulkan_radeon.dll") != nullptr, "ICD witness missing");
      D3D11_QUERY_DESC query = {D3D11_QUERY_EVENT, 0};
      check(device->CreateQuery(&query, &completion), "CreateQuery");
   }
   void finish() {
      context->End(completion.Get()); context->Flush();
      const ULONGLONG deadline = GetTickCount64() + 10000;
      for (;;) {
         BOOL ready = FALSE;
         HRESULT hr = context->GetData(completion.Get(), &ready, sizeof(ready), 0);
         check(hr, "GetData");
         if (hr == S_OK && ready) return;
         require(GetTickCount64() < deadline, "GPU completion timeout");
         Sleep(1);
      }
   }
};
struct Surface {
   ComPtr<ID3D11Texture2D> texture, staging;
   ComPtr<ID3D11RenderTargetView> view;
   UINT width = 0, height = 0;
   void reset() { view.Reset(); staging.Reset(); texture.Reset(); }
   void setup(Device& d, UINT w, UINT h, HANDLE shared = nullptr) {
      reset(); width = w; height = h;
      D3D11_TEXTURE2D_DESC desc = {};
      desc.Width = w; desc.Height = h; desc.MipLevels = desc.ArraySize = 1;
      desc.Format = DXGI_FORMAT_R8G8B8A8_UNORM; desc.SampleDesc.Count = 1;
      desc.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
      desc.MiscFlags = D3D11_RESOURCE_MISC_SHARED;
      if (shared) check(d.device->OpenSharedResource(shared, IID_PPV_ARGS(&texture)), "OpenSharedResource");
      else check(d.device->CreateTexture2D(&desc, nullptr, &texture), "CreateTexture2D shared");
      D3D11_TEXTURE2D_DESC actual = {}; texture->GetDesc(&actual);
      require(actual.Width == w && actual.Height == h && actual.Format == desc.Format, "shared descriptor mismatch");
      check(d.device->CreateRenderTargetView(texture.Get(), nullptr, &view), "CreateRenderTargetView");
      desc.MiscFlags = desc.BindFlags = 0;
      desc.Usage = D3D11_USAGE_STAGING; desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
      check(d.device->CreateTexture2D(&desc, nullptr, &staging), "CreateTexture2D staging");
   }
   static void expected(UINT iteration, bool reply, unsigned char (&color)[4]) {
      color[0] = static_cast<unsigned char>(iteration & 255);
      color[1] = static_cast<unsigned char>((iteration >> 8) & 255);
      color[2] = reply ? 213 : 37; color[3] = 255;
   }
   void write(Device& d, UINT iteration, bool reply) {
      unsigned char bytes[4]; expected(iteration, reply, bytes);
      float color[4]; for (UINT i = 0; i < 4; ++i) color[i] = bytes[i] / 255.0f;
      d.context->ClearRenderTargetView(view.Get(), color); d.finish();
   }
   uint64_t verify(Device& d, UINT iteration, bool reply) {
      d.context->CopyResource(staging.Get(), texture.Get()); d.finish();
      D3D11_MAPPED_SUBRESOURCE map = {};
      check(d.context->Map(staging.Get(), 0, D3D11_MAP_READ, 0, &map), "Map staging");
      unsigned char color[4]; expected(iteration, reply, color);
      uint64_t bad = 0;
      for (UINT y = 0; y < height; ++y) for (UINT x = 0; x < width; ++x) {
         const auto* pixel = static_cast<const unsigned char*>(map.pData) + y * map.RowPitch + x * 4;
         if (memcmp(pixel, color, 4)) {
            if (!bad) printf("pid=%lu iteration=%u reply=%u first=%u,%u got=%u,%u,%u,%u expected=%u,%u,%u,%u\n",
               GetCurrentProcessId(), iteration, reply, x, y, pixel[0], pixel[1], pixel[2], pixel[3], color[0], color[1], color[2], color[3]);
            ++bad;
         }
      }
      d.context->Unmap(staging.Get(), 0);
      require(!bad, "pixel mismatch"); return uint64_t(width) * height;
   }
};
static int child(Shared* shared, HANDLE go, HANDLE done, bool baseline) {
   try {
      Device device(baseline); Surface surface;
      shared->status = 0; SetEvent(done);
      for (;;) {
         require(WaitForSingleObject(go, 15000) == WAIT_OBJECT_0, "parent timeout");
         const LONG command = shared->command;
         if (command == Open) surface.setup(device, shared->width, shared->height, shared->texture);
         else if (command == Exchange) {
            shared->checkedPixels += surface.verify(device, shared->iteration, false);
            surface.write(device, shared->iteration, true);
         } else if (command == Close) { device.finish(); surface.reset(); check(device.device->GetDeviceRemovedReason(), "after imported release"); }
         else require(command == Exit, "invalid IPC command");
         shared->status = 0; SetEvent(done);
         if (command == Exit) break;
      }
      printf("child PASS checked_pixels=%llu\n", static_cast<unsigned long long>(shared->checkedPixels));
      return 0;
   } catch (const std::exception& error) {
      printf("child FAIL %s\n", error.what()); shared->status = 1; SetEvent(done); return 1;
   }
}
int wmain(int argc, wchar_t** argv) {
   setvbuf(stdout, nullptr, _IONBF, 0);
   if (argc == 6 && !wcscmp(argv[1], L"--child")) {
      HANDLE mapping = reinterpret_cast<HANDLE>(_wcstoui64(argv[2], nullptr, 16));
      HANDLE go = reinterpret_cast<HANDLE>(_wcstoui64(argv[3], nullptr, 16));
      HANDLE done = reinterpret_cast<HANDLE>(_wcstoui64(argv[4], nullptr, 16));
      auto* shared = static_cast<Shared*>(MapViewOfFile(mapping, FILE_MAP_ALL_ACCESS, 0, 0, sizeof(Shared)));
      if (!shared) return 2;
      int code = child(shared, go, done, !wcscmp(argv[5], L"baseline"));
      UnmapViewOfFile(shared); return code;
   }
   try {
      const bool baseline = argc > 1 && !wcscmp(argv[1], L"baseline");
      const UINT iterations = argc > 2 ? static_cast<UINT>(_wtoi(argv[2])) : 1000;
      require(iterations > 0 && iterations <= 1000, "iteration range");
      SECURITY_ATTRIBUTES security = {sizeof(security), nullptr, TRUE};
      Handle mapping(CreateFileMappingW(INVALID_HANDLE_VALUE, &security, PAGE_READWRITE, 0, sizeof(Shared), nullptr));
      Handle go(CreateEventW(&security, FALSE, FALSE, nullptr)), done(CreateEventW(&security, FALSE, FALSE, nullptr));
      require(mapping.h && go.h && done.h, "IPC creation");
      auto* shared = static_cast<Shared*>(MapViewOfFile(mapping.h, FILE_MAP_ALL_ACCESS, 0, 0, sizeof(Shared)));
      require(shared != nullptr, "MapViewOfFile"); ZeroMemory(shared, sizeof(*shared));
      Handle job(CreateJobObjectW(nullptr, nullptr)); require(job.h != nullptr, "CreateJobObject");
      JOBOBJECT_EXTENDED_LIMIT_INFORMATION limits = {};
      limits.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
      require(SetInformationJobObject(job.h, JobObjectExtendedLimitInformation, &limits, sizeof(limits)) != FALSE, "job limits");
      wchar_t exe[MAX_PATH] = {}; GetModuleFileNameW(nullptr, exe, MAX_PATH);
      wchar_t command[1024]; swprintf_s(command, L"\"%s\" --child %llx %llx %llx %s", exe,
         reinterpret_cast<uint64_t>(mapping.h), reinterpret_cast<uint64_t>(go.h), reinterpret_cast<uint64_t>(done.h), baseline ? L"baseline" : L"hosted");
      STARTUPINFOW startup = {}; startup.cb = sizeof(startup);
      startup.dwFlags = STARTF_USESTDHANDLES;
      startup.hStdInput = GetStdHandle(STD_INPUT_HANDLE); startup.hStdOutput = GetStdHandle(STD_OUTPUT_HANDLE); startup.hStdError = GetStdHandle(STD_ERROR_HANDLE);
      PROCESS_INFORMATION process = {};
      require(CreateProcessW(exe, command, nullptr, nullptr, TRUE, CREATE_SUSPENDED | CREATE_NO_WINDOW, nullptr, nullptr, &startup, &process) != FALSE, "CreateProcess");
      Handle childProcess(process.hProcess), childThread(process.hThread);
      if (!AssignProcessToJobObject(job.h, childProcess.h)) { TerminateProcess(childProcess.h, 123); throw std::runtime_error("AssignProcessToJobObject"); }
      require(ResumeThread(childThread.h) != DWORD(-1), "ResumeThread");
      auto wait = [&]() {
         HANDLE handles[2] = {done.h, childProcess.h};
         require(WaitForMultipleObjects(2, handles, FALSE, 15000) == WAIT_OBJECT_0, "child failed or timed out");
         require(shared->status == 0, "child rejected command");
      };
      wait(); Device device(baseline); Surface surface; uint64_t pixels = 0;
      auto send = [&](Command operation) { shared->command = operation; require(SetEvent(go.h) != FALSE, "SetEvent"); wait(); };
      UINT generations = 0;
      for (UINT i = 0; i < iterations; ++i) {
         if (i % 100 == 0) {
            if (i) { send(Close); surface.reset(); check(device.device->GetDeviceRemovedReason(), "after owner release"); }
            shared->width = 64 + (generations % 3) * 4; shared->height = 32 + (generations % 5) * 4;
            surface.setup(device, shared->width, shared->height);
            ComPtr<IDXGIResource> resource; check(surface.texture.As(&resource), "IDXGIResource");
            check(resource->GetSharedHandle(&shared->texture), "GetSharedHandle");
            require(shared->texture != nullptr, "null shared handle"); send(Open); ++generations;
         }
         shared->iteration = i; surface.write(device, i, false); send(Exchange);
         pixels += surface.verify(device, i, true);
         if ((i + 1) % 100 == 0) printf("parent progress=%u generations=%u checked_pixels=%llu\n", i + 1, generations, static_cast<unsigned long long>(pixels));
      }
      send(Close); surface.reset(); check(device.device->GetDeviceRemovedReason(), "after final owner release"); send(Exit);
      require(WaitForSingleObject(childProcess.h, 10000) == WAIT_OBJECT_0, "child exit timeout");
      DWORD exitCode = 125; require(GetExitCodeProcess(childProcess.h, &exitCode) && exitCode == 0, "child exit failure");
      printf("PASS iterations=%u generations=%u parent_pixels=%llu child_pixels=%llu distinct_pids=%lu,%lu\n", iterations, generations,
         static_cast<unsigned long long>(pixels), static_cast<unsigned long long>(shared->checkedPixels), GetCurrentProcessId(), process.dwProcessId);
      UnmapViewOfFile(shared); return 0;
   } catch (const std::exception& error) { printf("parent FAIL %s\n", error.what()); return 1; }
}
