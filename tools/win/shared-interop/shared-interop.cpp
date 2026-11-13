// SPDX-License-Identifier: MIT
// shared-interop.exe: D3D11 creates shared resources, D3D12 opens them on the same adapter, in one process
// (BD-373). The buffer part: D3D11 writes a pattern and D3D12 reads every byte, then D3D12 writes a second pattern and
// D3D11 reads every byte, with a shared fence for the order in both directions. The texture part opens a shared
// texture of 7 mips: with --texture-open-expected the open must succeed (engine ABI 1.11), else it must fail, and both
// devices must live. The created-buffer part (step 5): D3D12 creates shared buffers, D3D12 opens them and reads every
// byte in both directions (with --buffer-open-expected the open must succeed), and the D3D11 open of them must be
// refused (the runtime's rule). The last case times 256 creates of a committed GPU-only buffer.
// A console process with no window. Every wait has a timeout, and a watchdog ends the run at 55 s.
// Output: one "CASE <name> PASS|FAIL <detail>" line per case, then "SHARED-INTEROP PASS|FAIL cases=<n> failed=<n>".
// The debugger output of this process (OutputDebugString of the drivers, for example the D3D12 shell's
// "OpenHeapAndResource: ... declined at <step>" line) is read through the DBWIN buffer and printed as "DEBUG <text>",
// at most kDebugLines lines. When a debugger is attached or another DBWIN reader (DebugView) runs, the probe prints
// "DEBUG-CAPTURE off <reason>" and the lines go there.
// Exit codes: 0 every case passed, 1 a case failed, 2 the arguments were wrong, 3 no device of one API.
#include <windows.h>
#include <d3d11_4.h>
#include <d3d12.h>
#include <dxgi1_6.h>
#include <wrl/client.h>

#include <algorithm>
#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <functional>
#include <string>
#include <vector>

using Microsoft::WRL::ComPtr;

namespace {

constexpr DWORD kWaitMs = 5000;     // one GPU or fence wait
constexpr DWORD kRunMs = 55000;     // the whole run

int g_cases = 0;
int g_failed = 0;
bool g_stuck = false;               // a D3D12 submission did not complete: no later case can use the queue

void out(const char* format, ...) {
    va_list args;
    va_start(args, format);
    std::vprintf(format, args);
    va_end(args);
    std::fflush(stdout);
}

void report(const char* name, bool pass, const char* format, ...) {
    char detail[768];
    va_list args;
    va_start(args, format);
    std::vsnprintf(detail, sizeof(detail), format, args);
    va_end(args);
    ++g_cases;
    if (!pass) ++g_failed;
    out("CASE %s %s %s\n", name, pass ? "PASS" : "FAIL", detail);
}

void summary_and_exit(int code) {
    Sleep(250); // the DEBUG lines of the last case: the reader thread prints them as they arrive
    out("SHARED-INTEROP %s cases=%d failed=%d\n", g_failed || code ? "FAIL" : "PASS", g_cases, g_failed);
    std::exit(g_failed ? 1 : code);
}

// The DBWIN protocol of OutputDebugString without a debugger: the writer waits for DBWIN_BUFFER_READY, writes its
// process id and the text into the DBWIN_BUFFER section, and sets DBWIN_DATA_READY. One reader per session.
constexpr int kDebugLines = 200;

struct DbwinBuffer {
    DWORD process_id;
    char text[4096 - sizeof(DWORD)];
};

struct DebugCapture {
    HANDLE buffer_ready = nullptr;
    HANDLE data_ready = nullptr;
    HANDLE section = nullptr;
    const DbwinBuffer* view = nullptr;
};

DWORD WINAPI debug_reader(void* parameter) {
    const DebugCapture* capture = static_cast<const DebugCapture*>(parameter);
    const DWORD self = GetCurrentProcessId();
    int lines = 0;
    for (;;) {
        SetEvent(capture->buffer_ready);
        if (WaitForSingleObject(capture->data_ready, INFINITE) != WAIT_OBJECT_0) return 0;
        if (capture->view->process_id != self || lines >= kDebugLines) continue;
        char text[sizeof(capture->view->text)];
        std::memcpy(text, capture->view->text, sizeof(text));
        text[sizeof(text) - 1] = 0;
        size_t n = std::strlen(text);
        while (n && (text[n - 1] == '\n' || text[n - 1] == '\r')) text[--n] = 0;
        if (++lines == kDebugLines)
            out("DEBUG %s\nDEBUG-CAPTURE stopped after %d lines\n", text, kDebugLines);
        else
            out("DEBUG %s\n", text);
    }
}

void start_debug_capture() {
    if (IsDebuggerPresent()) {
        out("DEBUG-CAPTURE off a debugger is attached\n");
        return;
    }
    static DebugCapture capture;
    capture.buffer_ready = CreateEventA(nullptr, FALSE, FALSE, "DBWIN_BUFFER_READY");
    const bool other = capture.buffer_ready && GetLastError() == ERROR_ALREADY_EXISTS;
    capture.data_ready = other ? nullptr : CreateEventA(nullptr, FALSE, FALSE, "DBWIN_DATA_READY");
    capture.section = capture.data_ready ? CreateFileMappingA(INVALID_HANDLE_VALUE, nullptr, PAGE_READWRITE, 0,
                                                              sizeof(DbwinBuffer), "DBWIN_BUFFER")
                                         : nullptr;
    capture.view = capture.section ? static_cast<const DbwinBuffer*>(
                                         MapViewOfFile(capture.section, FILE_MAP_READ, 0, 0, sizeof(DbwinBuffer)))
                                   : nullptr;
    if (!capture.view || !CreateThread(nullptr, 0, debug_reader, &capture, 0, nullptr)) {
        out("DEBUG-CAPTURE off %s (error %lu)\n", other ? "another DBWIN reader runs" : "no DBWIN objects",
            static_cast<unsigned long>(GetLastError()));
        return;
    }
    out("DEBUG-CAPTURE on\n");
    // The positive control of the channel: this line must come back as "DEBUG shared-interop: capture check".
    OutputDebugStringA("shared-interop: capture check\n");
}

DWORD WINAPI watchdog(void*) {
    Sleep(kRunMs);
    out("CASE watchdog FAIL the run did not end within %lu ms\n", static_cast<unsigned long>(kRunMs));
    out("SHARED-INTEROP FAIL cases=%d failed=%d\n", g_cases + 1, g_failed + 1);
    TerminateProcess(GetCurrentProcess(), 1);
    return 0;
}

// Every byte is a function of its index and of the seed, so a shifted, truncated or stale copy shows.
uint8_t pattern(uint32_t seed, size_t i) {
    uint32_t x = static_cast<uint32_t>(i) * 2654435761u ^ (seed * 0x9E3779B9u + 0x7F4A7C15u);
    x ^= x >> 15;
    x *= 0x2C1B3C6Du;
    x ^= x >> 12;
    return static_cast<uint8_t>(x);
}

void fill(uint8_t* data, size_t bytes, uint32_t seed) {
    for (size_t i = 0; i < bytes; ++i) data[i] = pattern(seed, i);
}

// The number of bytes that differ; *first is the first such index.
size_t compare(const uint8_t* data, size_t bytes, uint32_t seed, size_t* first, unsigned* got, unsigned* want) {
    size_t bad = 0;
    for (size_t i = 0; i < bytes; ++i) {
        if (data[i] == pattern(seed, i)) continue;
        if (!bad) {
            *first = i;
            *got = data[i];
            *want = pattern(seed, i);
        }
        ++bad;
    }
    return bad;
}

struct Gpu {
    ComPtr<IDXGIAdapter1> adapter;
    LUID luid{};
    ComPtr<ID3D11Device5> d11;
    ComPtr<ID3D11DeviceContext4> c11;
    ComPtr<ID3D12Device> d12;
    ComPtr<ID3D12CommandQueue> queue;
    ComPtr<ID3D12CommandAllocator> allocator;
    ComPtr<ID3D12GraphicsCommandList> list;
    ComPtr<ID3D12Fence> own;            // the D3D12 queue's own completion fence
    UINT64 own_value = 0;
    HANDLE own_event = nullptr;
    ComPtr<ID3D11Fence> f11;            // the shared fence, created by D3D11
    ComPtr<ID3D12Fence> f12;            // and opened by D3D12
    UINT64 shared_value = 0;
    HANDLE shared_event = nullptr;
};

std::string removed(Gpu& g) {
    const HRESULT r11 = g.d11->GetDeviceRemovedReason(), r12 = g.d12->GetDeviceRemovedReason();
    if (r11 == S_OK && r12 == S_OK) return {};
    char text[96];
    std::snprintf(text, sizeof(text), "device removed: d3d11 %08lx d3d12 %08lx", static_cast<unsigned long>(r11),
                  static_cast<unsigned long>(r12));
    return text;
}

// One D3D12 submission: the recording, an optional queue wait on the shared fence before it, an optional queue signal
// of the shared fence after it, and a bounded CPU wait for the queue's own fence. A wait that does not complete is
// released by a CPU signal of the shared fence, so the queue is not left blocked for the next case.
bool submit12(Gpu& g, const std::function<void(ID3D12GraphicsCommandList*)>& record, UINT64 wait_shared,
              UINT64 signal_shared, std::string& why) {
    char text[160];
    if (g_stuck) {
        why = "the D3D12 queue is stuck from an earlier case";
        return false;
    }
    HRESULT hr = g.allocator->Reset();
    if (SUCCEEDED(hr)) hr = g.list->Reset(g.allocator.Get(), nullptr);
    if (SUCCEEDED(hr)) {
        record(g.list.Get());
        hr = g.list->Close();
    }
    if (FAILED(hr)) {
        std::snprintf(text, sizeof(text), "command list %08lx", static_cast<unsigned long>(hr));
        why = text;
        return false;
    }
    if (wait_shared && FAILED(hr = g.queue->Wait(g.f12.Get(), wait_shared))) {
        std::snprintf(text, sizeof(text), "queue wait %08lx", static_cast<unsigned long>(hr));
        why = text;
        return false;
    }
    ID3D12CommandList* lists[] = {g.list.Get()};
    g.queue->ExecuteCommandLists(1, lists);
    if (signal_shared && FAILED(hr = g.queue->Signal(g.f12.Get(), signal_shared))) {
        std::snprintf(text, sizeof(text), "queue signal of the shared fence %08lx", static_cast<unsigned long>(hr));
        why = text;
    }
    const UINT64 value = ++g.own_value;
    if (FAILED(hr = g.queue->Signal(g.own.Get(), value)) ||
        FAILED(hr = g.own->SetEventOnCompletion(value, g.own_event))) {
        std::snprintf(text, sizeof(text), "own fence %08lx", static_cast<unsigned long>(hr));
        why = text;
        g_stuck = true;
        return false;
    }
    if (WaitForSingleObject(g.own_event, kWaitMs) == WAIT_OBJECT_0) return why.empty();
    std::snprintf(text, sizeof(text), "the D3D12 submission did not complete in %lu ms (shared fence at %llu, wait %llu)",
                  static_cast<unsigned long>(kWaitMs), static_cast<unsigned long long>(g.f12->GetCompletedValue()),
                  static_cast<unsigned long long>(wait_shared));
    why = text;
    if (wait_shared) (void)g.f12->Signal(wait_shared);
    if (WaitForSingleObject(g.own_event, kWaitMs) != WAIT_OBJECT_0) g_stuck = true;
    return false;
}

ComPtr<ID3D12Resource> buffer12(Gpu& g, D3D12_HEAP_TYPE type, UINT64 bytes, D3D12_RESOURCE_STATES state) {
    D3D12_HEAP_PROPERTIES heap{};
    heap.Type = type;
    D3D12_RESOURCE_DESC desc{};
    desc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    desc.Width = bytes;
    desc.Height = 1;
    desc.DepthOrArraySize = 1;
    desc.MipLevels = 1;
    desc.SampleDesc = {1, 0};
    desc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    ComPtr<ID3D12Resource> r;
    if (FAILED(g.d12->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &desc, state, nullptr, IID_PPV_ARGS(&r))))
        return nullptr;
    return r;
}

enum class Form { Nt, Kmt };

struct BufferCase {
    const char* name;
    UINT bytes;
    UINT bind;
    UINT misc;
    UINT stride;
};

// The handle a D3D12 device opens: an NT handle (closed by the caller) or the legacy global handle (never closed).
HANDLE share(ID3D11Resource* r, Form form, HRESULT* hr) {
    HANDLE h = nullptr;
    if (form == Form::Nt) {
        ComPtr<IDXGIResource1> dxgi;
        *hr = r->QueryInterface(IID_PPV_ARGS(&dxgi));
        if (SUCCEEDED(*hr))
            *hr = dxgi->CreateSharedHandle(nullptr, DXGI_SHARED_RESOURCE_READ | DXGI_SHARED_RESOURCE_WRITE, nullptr, &h);
    } else {
        ComPtr<IDXGIResource> dxgi;
        *hr = r->QueryInterface(IID_PPV_ARGS(&dxgi));
        if (SUCCEEDED(*hr)) *hr = dxgi->GetSharedHandle(&h);
    }
    return SUCCEEDED(*hr) ? h : nullptr;
}

UINT share_flags(Form form) {
    return form == Form::Nt ? D3D11_RESOURCE_MISC_SHARED | D3D11_RESOURCE_MISC_SHARED_NTHANDLE
                            : D3D11_RESOURCE_MISC_SHARED;
}

// Waits on the CPU until the D3D11 immediate context has done everything before an event query.
bool drain11(Gpu& g, std::string& why) {
    D3D11_QUERY_DESC qd{D3D11_QUERY_EVENT, 0};
    ComPtr<ID3D11Query> query;
    if (FAILED(g.d11->CreateQuery(&qd, &query))) {
        why = "event query";
        return false;
    }
    g.c11->End(query.Get());
    g.c11->Flush();
    const ULONGLONG deadline = GetTickCount64() + kWaitMs;
    BOOL done = FALSE;
    while (g.c11->GetData(query.Get(), &done, sizeof(done), D3D11_ASYNC_GETDATA_DONOTFLUSH) != S_OK || !done) {
        if (GetTickCount64() > deadline) {
            why = "the D3D11 context did not drain in 5000 ms";
            return false;
        }
        Sleep(1);
    }
    return true;
}

void buffer_case(Gpu& g, const BufferCase& c, Form form, uint32_t seed) {
    char name[96];
    const char* suffix = form == Form::Nt ? "nt" : "kmt";
    char text[256];
    HRESULT hr = S_OK;

    // D3D11 creates the shared buffer with the first pattern and signals the shared fence after it.
    std::snprintf(name, sizeof(name), "%s-%s-11to12", c.name, suffix);
    std::vector<uint8_t> first(c.bytes), second(c.bytes);
    fill(first.data(), first.size(), seed);
    fill(second.data(), second.size(), seed + 1000);
    D3D11_BUFFER_DESC bd{};
    bd.ByteWidth = c.bytes;
    bd.Usage = D3D11_USAGE_DEFAULT;
    bd.BindFlags = c.bind;
    bd.MiscFlags = c.misc | share_flags(form);
    bd.StructureByteStride = c.stride;
    D3D11_SUBRESOURCE_DATA init{first.data(), 0, 0};
    ComPtr<ID3D11Buffer> b11;
    if (FAILED(hr = g.d11->CreateBuffer(&bd, &init, &b11))) {
        report(name, false, "d3d11 CreateBuffer %08lx (ByteWidth %u, bind 0x%x, misc 0x%x)",
               static_cast<unsigned long>(hr), c.bytes, c.bind, bd.MiscFlags);
        std::snprintf(name, sizeof(name), "%s-%s-12to11", c.name, suffix);
        report(name, false, "no D3D11 buffer");
        return;
    }
    const HANDLE handle = share(b11.Get(), form, &hr);
    const UINT64 created = ++g.shared_value;
    (void)g.c11->Signal(g.f11.Get(), created);
    g.c11->Flush();
    ComPtr<ID3D12Resource> r12;
    if (handle) hr = g.d12->OpenSharedHandle(handle, IID_PPV_ARGS(&r12));
    if (handle && form == Form::Nt) CloseHandle(handle);
    if (!r12) {
        report(name, false, "%s %08lx%s%s", handle ? "d3d12 OpenSharedHandle" : "d3d11 shared handle",
               static_cast<unsigned long>(hr), removed(g).empty() ? "" : ", ", removed(g).c_str());
        std::snprintf(name, sizeof(name), "%s-%s-12to11", c.name, suffix);
        report(name, false, "not opened");
        return;
    }
    const D3D12_RESOURCE_DESC d = r12->GetDesc();
    const bool uav = (c.bind & D3D11_BIND_UNORDERED_ACCESS) != 0;
    const bool desc_ok = d.Dimension == D3D12_RESOURCE_DIMENSION_BUFFER && d.Width == c.bytes && d.Height == 1 &&
                         d.DepthOrArraySize == 1 && d.MipLevels == 1 && d.Format == DXGI_FORMAT_UNKNOWN &&
                         d.Layout == D3D12_TEXTURE_LAYOUT_ROW_MAJOR &&
                         ((d.Flags & D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS) != 0) == uav;
    std::snprintf(text, sizeof(text), "GetDesc dimension %d width %llu height %u layout %d flags 0x%x",
                  static_cast<int>(d.Dimension), static_cast<unsigned long long>(d.Width), d.Height,
                  static_cast<int>(d.Layout), static_cast<unsigned>(d.Flags));
    const std::string desc_text = text;

    // D3D12 reads the bytes after the shared fence.
    ComPtr<ID3D12Resource> readback = buffer12(g, D3D12_HEAP_TYPE_READBACK, c.bytes, D3D12_RESOURCE_STATE_COPY_DEST);
    std::string why;
    bool ok = readback && submit12(g, [&](ID3D12GraphicsCommandList* l) {
        l->CopyBufferRegion(readback.Get(), 0, r12.Get(), 0, c.bytes);
    }, created, 0, why);
    if (!readback) why = "READBACK buffer";
    size_t bad = c.bytes, at = 0;
    unsigned got = 0, want = 0;
    if (ok) {
        void* cpu = nullptr;
        const D3D12_RANGE range{0, c.bytes};
        if (SUCCEEDED(readback->Map(0, &range, &cpu)) && cpu) {
            bad = compare(static_cast<const uint8_t*>(cpu), c.bytes, seed, &at, &got, &want);
            const D3D12_RANGE none{0, 0};
            readback->Unmap(0, &none);
        } else {
            why = "READBACK map";
        }
    }
    std::string gone = removed(g);
    if (!ok || !why.empty() || bad || !desc_ok || !gone.empty()) {
        std::snprintf(text, sizeof(text), "%s; %zu of %u bytes differ", why.empty() ? "read" : why.c_str(),
                      bad, c.bytes);
        std::string detail = text;
        if (bad && bad != c.bytes) {
            std::snprintf(text, sizeof(text), " (first at %zu: 0x%02x, want 0x%02x)", at, got, want);
            detail += text;
        }
        report(name, false, "%s; %s%s%s", detail.c_str(), desc_text.c_str(), gone.empty() ? "" : "; ", gone.c_str());
    } else {
        report(name, true, "%u bytes equal; %s", c.bytes, desc_text.c_str());
    }

    // The reverse: D3D12 writes the second pattern into the opened buffer and signals the shared fence; D3D11 waits
    // for it and reads every byte.
    std::snprintf(name, sizeof(name), "%s-%s-12to11", c.name, suffix);
    why.clear();
    ComPtr<ID3D12Resource> upload = buffer12(g, D3D12_HEAP_TYPE_UPLOAD, c.bytes, D3D12_RESOURCE_STATE_GENERIC_READ);
    void* cpu = nullptr;
    if (!upload || FAILED(upload->Map(0, nullptr, &cpu)) || !cpu) {
        report(name, false, "UPLOAD buffer");
        return;
    }
    std::memcpy(cpu, second.data(), c.bytes);
    upload->Unmap(0, nullptr);
    const UINT64 written = ++g.shared_value;
    ok = submit12(g, [&](ID3D12GraphicsCommandList* l) {
        l->CopyBufferRegion(r12.Get(), 0, upload.Get(), 0, c.bytes);
    }, 0, written, why);
    if (ok) {
        if (FAILED(hr = g.f11->SetEventOnCompletion(written, g.shared_event)) ||
            WaitForSingleObject(g.shared_event, kWaitMs) != WAIT_OBJECT_0) {
            std::snprintf(text, sizeof(text), "d3d11 did not see the shared fence reach %llu in %lu ms (at %llu)",
                          static_cast<unsigned long long>(written), static_cast<unsigned long>(kWaitMs),
                          static_cast<unsigned long long>(g.f11->GetCompletedValue()));
            why = text;
            ok = false;
        }
    }
    bad = c.bytes;
    if (ok) {
        // The GPU wait as well: the D3D11 copy is ordered after the fence on the D3D11 queue.
        (void)g.c11->Wait(g.f11.Get(), written);
        D3D11_BUFFER_DESC sd{};
        sd.ByteWidth = c.bytes;
        sd.Usage = D3D11_USAGE_STAGING;
        sd.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
        sd.MiscFlags = c.misc;
        sd.StructureByteStride = c.stride;
        ComPtr<ID3D11Buffer> staging;
        if (FAILED(hr = g.d11->CreateBuffer(&sd, nullptr, &staging))) {
            std::snprintf(text, sizeof(text), "d3d11 staging buffer %08lx", static_cast<unsigned long>(hr));
            why = text;
        } else {
            g.c11->CopyResource(staging.Get(), b11.Get());
            if (drain11(g, why)) {
                D3D11_MAPPED_SUBRESOURCE mapped{};
                if (SUCCEEDED(hr = g.c11->Map(staging.Get(), 0, D3D11_MAP_READ, 0, &mapped)) && mapped.pData) {
                    bad = compare(static_cast<const uint8_t*>(mapped.pData), c.bytes, seed + 1000, &at, &got, &want);
                    g.c11->Unmap(staging.Get(), 0);
                } else {
                    std::snprintf(text, sizeof(text), "d3d11 staging map %08lx", static_cast<unsigned long>(hr));
                    why = text;
                }
            }
        }
    }
    gone = removed(g);
    if (!why.empty() || bad || !gone.empty()) {
        std::snprintf(text, sizeof(text), "%s; %zu of %u bytes differ", why.empty() ? "read" : why.c_str(), bad,
                      c.bytes);
        std::string detail = text;
        if (bad && bad != c.bytes) {
            std::snprintf(text, sizeof(text), " (first at %zu: 0x%02x, want 0x%02x)", at, got, want);
            detail += text;
        }
        report(name, false, "%s%s%s", detail.c_str(), gone.empty() ? "" : "; ", gone.c_str());
    } else {
        report(name, true, "%u bytes equal", c.bytes);
    }
}

// The NT handle form of a buffer. The D3D11 runtime refuses SHARED_NTHANDLE on a buffer with E_INVALIDARG before any
// driver sees the create (the development PC's stock driver, 2026-11-13: every buffer of this program). The case
// holds that rule, so a runtime that admits the form shows here, and the device must live.
void buffer_nt_case(Gpu& g, const BufferCase& c) {
    char name[96];
    std::snprintf(name, sizeof(name), "%s-nt-create", c.name);
    D3D11_BUFFER_DESC bd{};
    bd.ByteWidth = c.bytes;
    bd.Usage = D3D11_USAGE_DEFAULT;
    bd.BindFlags = c.bind;
    bd.MiscFlags = c.misc | share_flags(Form::Nt);
    bd.StructureByteStride = c.stride;
    ComPtr<ID3D11Buffer> b11;
    const HRESULT hr = g.d11->CreateBuffer(&bd, nullptr, &b11);
    const std::string gone = removed(g);
    report(name, hr == E_INVALIDARG && gone.empty(),
           "d3d11 CreateBuffer %08lx (expected: E_INVALIDARG, the runtime's rule for an NT handle on a buffer)%s%s",
           static_cast<unsigned long>(hr), gone.empty() ? "" : "; ", gone.c_str());
}

// The negative texture case: a shared mip chain is the ENGINE LAYOUT form, which the D3D12 open refuses until it reads
// textures. The open must fail and both devices must live. With --texture-open-expected (another driver as a positive
// control of this program) the open must succeed with the mip count.
void texture_case(Gpu& g, bool open_expected) {
    const char* name = "texture-mips-nt-open";
    D3D11_TEXTURE2D_DESC td{};
    td.Width = td.Height = 64;
    td.MipLevels = 7;
    td.ArraySize = 1;
    td.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    td.SampleDesc = {1, 0};
    td.Usage = D3D11_USAGE_DEFAULT;
    td.BindFlags = D3D11_BIND_SHADER_RESOURCE;
    td.MiscFlags = share_flags(Form::Nt);
    ComPtr<ID3D11Texture2D> t11;
    HRESULT hr = g.d11->CreateTexture2D(&td, nullptr, &t11);
    if (FAILED(hr)) {
        report(name, false, "d3d11 CreateTexture2D %08lx (64x64, 7 mips, shared NT)", static_cast<unsigned long>(hr));
        return;
    }
    const HANDLE handle = share(t11.Get(), Form::Nt, &hr);
    if (!handle) {
        report(name, false, "d3d11 shared handle %08lx", static_cast<unsigned long>(hr));
        return;
    }
    ComPtr<ID3D12Resource> r12;
    hr = g.d12->OpenSharedHandle(handle, IID_PPV_ARGS(&r12));
    CloseHandle(handle);
    const std::string gone = removed(g);
    const UINT16 mips = r12 ? r12->GetDesc().MipLevels : 0;
    if (open_expected) {
        report(name, r12 && mips == 7 && gone.empty(), "open %08lx, %u mips (expected: opened, 7 mips)%s%s",
               static_cast<unsigned long>(hr), mips, gone.empty() ? "" : "; ", gone.c_str());
    } else {
        report(name, !r12 && gone.empty(), "open %08lx (expected: refused, both devices live)%s%s",
               static_cast<unsigned long>(hr), gone.empty() ? "" : "; ", gone.c_str());
    }
}

// The text of a failed compare: the step or "read", the count of bytes that differ and the first one.
std::string compare_text(const std::string& why, size_t bad, UINT bytes, size_t at, unsigned got, unsigned want) {
    char text[256];
    std::snprintf(text, sizeof(text), "%s; %zu of %u bytes differ", why.empty() ? "read" : why.c_str(), bad, bytes);
    std::string detail = text;
    if (bad && bad != bytes) {
        std::snprintf(text, sizeof(text), " (first at %zu: 0x%02x, want 0x%02x)", at, got, want);
        detail += text;
    }
    return detail;
}

// D3D12 copies a D3D12 buffer to a READBACK buffer after the queue waits for the shared fence value wait_shared (0: no
// wait), and compares every byte with the pattern of seed. Returns the count of bytes that differ (bytes when the
// read did not occur, with *why set).
size_t read12(Gpu& g, ID3D12Resource* r, UINT bytes, UINT64 wait_shared, uint32_t seed, std::string& why,
              size_t* at, unsigned* got, unsigned* want) {
    ComPtr<ID3D12Resource> readback = buffer12(g, D3D12_HEAP_TYPE_READBACK, bytes, D3D12_RESOURCE_STATE_COPY_DEST);
    if (!readback) {
        why = "READBACK buffer";
        return bytes;
    }
    if (!submit12(g, [&](ID3D12GraphicsCommandList* l) { l->CopyBufferRegion(readback.Get(), 0, r, 0, bytes); },
                  wait_shared, 0, why))
        return bytes;
    void* cpu = nullptr;
    const D3D12_RANGE range{0, bytes};
    if (FAILED(readback->Map(0, &range, &cpu)) || !cpu) {
        why = "READBACK map";
        return bytes;
    }
    const size_t bad = compare(static_cast<const uint8_t*>(cpu), bytes, seed, at, got, want);
    const D3D12_RANGE none{0, 0};
    readback->Unmap(0, &none);
    return bad;
}

// D3D12 copies the bytes of an UPLOAD buffer into a D3D12 buffer and signals the shared fence value signal_shared.
bool write12(Gpu& g, ID3D12Resource* r, const std::vector<uint8_t>& bytes, UINT64 signal_shared, std::string& why) {
    const UINT size = static_cast<UINT>(bytes.size());
    ComPtr<ID3D12Resource> upload = buffer12(g, D3D12_HEAP_TYPE_UPLOAD, size, D3D12_RESOURCE_STATE_GENERIC_READ);
    void* cpu = nullptr;
    if (!upload || FAILED(upload->Map(0, nullptr, &cpu)) || !cpu) {
        why = "UPLOAD buffer";
        return false;
    }
    std::memcpy(cpu, bytes.data(), size);
    upload->Unmap(0, nullptr);
    return submit12(g, [&](ID3D12GraphicsCommandList* l) { l->CopyBufferRegion(r, 0, upload.Get(), 0, size); }, 0,
                    signal_shared, why);
}

struct CreatedCase {
    const char* name;
    UINT bytes;
    bool uav;   // D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS
};

// BD-373 step 5: D3D12 creates the shared buffer (a committed DEFAULT buffer with D3D12_HEAP_FLAG_SHARED and an NT
// handle) and writes the first pattern into it. By default a create that fails with E_OUTOFMEMORY passes and ends the
// cases of that buffer (the D3D12 shell refuses a shared buffer today).
//   <name>-12to12       D3D12 opens the handle and reads every byte. In one process D3D12CreateDevice gives one
//                       device per adapter, so the creator's device is the opener; the open still goes through the
//                       driver's OpenHeapAndResource. By default a refusal passes and an open with other bytes fails.
//                       With --buffer-open-expected (a driver that writes the ENGINE LAYOUT record of a D3D12 buffer,
//                       or another vendor's driver as a positive control) the open must succeed.
//   <name>-12to12-back  only after an open: D3D12 writes the second pattern through the opened resource and reads it
//                       through the creator's resource.
//   <name>-12to11       D3D11 opens the handle with OpenSharedResource1. The D3D11 runtime refused this open of a
//                       buffer with E_INVALIDARG on the development PC (another vendor's driver, 2026-11-13; its debug
//                       layer gives no reason, and the same runtime admits an NT handle only for a 2D texture at
//                       create). The case holds that rule: the open must be refused with both devices alive. An open
//                       fails the case and prints the D3D11 description, because then the D3D11 binds of the record
//                       (driver/umd/d3d12/shared-buffer-record.h) have a reader.
void created_case(Gpu& g, const CreatedCase& c, bool open_expected, uint32_t seed) {
    char name[96];
    char text[256];
    std::vector<uint8_t> first(c.bytes), second(c.bytes);
    fill(first.data(), first.size(), seed);
    fill(second.data(), second.size(), seed + 1000);

    std::snprintf(name, sizeof(name), "%s-12to12", c.name);
    D3D12_HEAP_PROPERTIES heap{};
    heap.Type = D3D12_HEAP_TYPE_DEFAULT;
    D3D12_RESOURCE_DESC desc{};
    desc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    desc.Width = c.bytes;
    desc.Height = 1;
    desc.DepthOrArraySize = 1;
    desc.MipLevels = 1;
    desc.SampleDesc = {1, 0};
    desc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    desc.Flags = c.uav ? D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS : D3D12_RESOURCE_FLAG_NONE;
    ComPtr<ID3D12Resource> r12;
    HRESULT hr = g.d12->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_SHARED, &desc, D3D12_RESOURCE_STATE_COMMON,
                                                nullptr, IID_PPV_ARGS(&r12));
    if (!r12) {
        // The D3D12 shell documents this refusal (driver/umd/d3d12/README.md, "Shared resources"): E_OUTOFMEMORY,
        // the device lives. By default it passes; with --buffer-open-expected it fails.
        const std::string gone = removed(g);
        report(name, !open_expected && hr == E_OUTOFMEMORY && gone.empty(),
               "d3d12 shared CreateCommittedResource %08lx (Width %u, flags 0x%x; expected: %s)%s%s",
               static_cast<unsigned long>(hr), c.bytes, static_cast<unsigned>(desc.Flags),
               open_expected ? "created" : "created, or E_OUTOFMEMORY with both devices live", gone.empty() ? "" : "; ",
               gone.c_str());
        return;
    }
    std::string why;
    bool ok = write12(g, r12.Get(), first, 0, why);
    HANDLE handle = nullptr;
    if (ok && FAILED(hr = g.d12->CreateSharedHandle(r12.Get(), nullptr, GENERIC_ALL, nullptr, &handle))) {
        std::snprintf(text, sizeof(text), "d3d12 CreateSharedHandle %08lx", static_cast<unsigned long>(hr));
        why = text;
        ok = false;
    }
    if (!ok) {
        report(name, false, "%s", why.c_str());
        return;
    }

    ComPtr<ID3D12Resource> opened;
    hr = g.d12->OpenSharedHandle(handle, IID_PPV_ARGS(&opened));
    size_t at = 0, bad = c.bytes;
    unsigned got = 0, want = 0;
    std::string desc_text;
    if (opened) {
        const D3D12_RESOURCE_DESC d = opened->GetDesc();
        std::snprintf(text, sizeof(text), "GetDesc dimension %d width %llu flags 0x%x", static_cast<int>(d.Dimension),
                      static_cast<unsigned long long>(d.Width), static_cast<unsigned>(d.Flags));
        desc_text = text;
        bad = read12(g, opened.Get(), c.bytes, 0, seed, why, &at, &got, &want);
        if (d.Dimension != D3D12_RESOURCE_DIMENSION_BUFFER || d.Width != c.bytes || d.Flags != desc.Flags)
            why = "the description differs";
    }
    std::string gone = removed(g);
    if (!opened)
        report(name, !open_expected && gone.empty(), "open %08lx (expected: %s)%s%s", static_cast<unsigned long>(hr),
               open_expected ? "opened" : "refused or opened, both devices live", gone.empty() ? "" : "; ",
               gone.c_str());
    else if (!why.empty() || bad || !gone.empty())
        report(name, false, "opened; %s; %s%s%s", compare_text(why, bad, c.bytes, at, got, want).c_str(),
               desc_text.c_str(), gone.empty() ? "" : "; ", gone.c_str());
    else
        report(name, true, "opened, %u bytes equal; %s", c.bytes, desc_text.c_str());

    if (opened) {
        std::snprintf(name, sizeof(name), "%s-12to12-back", c.name);
        why.clear();
        bad = write12(g, opened.Get(), second, 0, why) ? read12(g, r12.Get(), c.bytes, 0, seed + 1000, why, &at, &got,
                                                                &want)
                                                       : c.bytes;
        gone = removed(g);
        if (!why.empty() || bad || !gone.empty())
            report(name, false, "%s%s%s", compare_text(why, bad, c.bytes, at, got, want).c_str(),
                   gone.empty() ? "" : "; ", gone.c_str());
        else
            report(name, true, "%u bytes equal", c.bytes);
        opened.Reset();
    }

    std::snprintf(name, sizeof(name), "%s-12to11", c.name);
    ComPtr<ID3D11Buffer> b11;
    hr = g.d11->OpenSharedResource1(handle, IID_PPV_ARGS(&b11));
    CloseHandle(handle);
    desc_text.clear();
    if (b11) {
        D3D11_BUFFER_DESC bd{};
        b11->GetDesc(&bd);
        std::snprintf(text, sizeof(text), "; GetDesc ByteWidth %u Usage %d BindFlags 0x%x CPUAccessFlags 0x%x "
                      "MiscFlags 0x%x StructureByteStride %u", bd.ByteWidth, static_cast<int>(bd.Usage), bd.BindFlags,
                      bd.CPUAccessFlags, bd.MiscFlags, bd.StructureByteStride);
        desc_text = text;
    }
    gone = removed(g);
    report(name, hr == E_INVALIDARG && !b11 && gone.empty(),
           "d3d11 OpenSharedResource1 %08lx (expected: E_INVALIDARG, the runtime's rule for a D3D12 buffer)%s%s%s",
           static_cast<unsigned long>(hr), desc_text.c_str(), gone.empty() ? "" : "; ", gone.c_str());
}

// The cost of a committed GPU-only buffer create, which the ENGINE LAYOUT record of the D3D12 shell changes (the
// lab switch engine-layout-buffers writes it for every such buffer). The case creates kCostBuffers buffers of 4096
// bytes with unordered access, times each create with QueryPerformanceCounter, and prints the mean, the median and
// the longest. It passes when every create succeeds; the lab compares two runs, the switch off and on.
constexpr int kCostBuffers = 256;

void create_cost_case(Gpu& g) {
    const char* name = "buffer-create-cost";
    D3D12_HEAP_PROPERTIES heap{};
    heap.Type = D3D12_HEAP_TYPE_DEFAULT;
    D3D12_RESOURCE_DESC desc{};
    desc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    desc.Width = 4096;
    desc.Height = 1;
    desc.DepthOrArraySize = 1;
    desc.MipLevels = 1;
    desc.SampleDesc = {1, 0};
    desc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    desc.Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
    LARGE_INTEGER frequency{};
    QueryPerformanceFrequency(&frequency);
    std::vector<ComPtr<ID3D12Resource>> buffers(kCostBuffers);
    std::vector<double> us;
    us.reserve(kCostBuffers);
    HRESULT hr = S_OK;
    for (ComPtr<ID3D12Resource>& b : buffers) {
        LARGE_INTEGER t0{}, t1{};
        QueryPerformanceCounter(&t0);
        hr = g.d12->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &desc, D3D12_RESOURCE_STATE_COMMON, nullptr,
                                            IID_PPV_ARGS(&b));
        QueryPerformanceCounter(&t1);
        if (FAILED(hr)) break;
        us.push_back(static_cast<double>(t1.QuadPart - t0.QuadPart) * 1e6 / static_cast<double>(frequency.QuadPart));
    }
    buffers.clear();
    const std::string gone = removed(g);
    if (us.size() != static_cast<size_t>(kCostBuffers) || !gone.empty()) {
        report(name, false, "create %zu of %d failed with %08lx%s%s", us.size() + 1, kCostBuffers,
               static_cast<unsigned long>(hr), gone.empty() ? "" : "; ", gone.c_str());
        return;
    }
    double sum = 0, longest = 0;
    for (const double v : us) {
        sum += v;
        if (v > longest) longest = v;
    }
    std::vector<double> sorted = us;
    std::sort(sorted.begin(), sorted.end());
    report(name, true, "%d creates of 4096 bytes: mean %.1f us, median %.1f us, longest %.1f us, first %.1f us",
           kCostBuffers, sum / kCostBuffers, sorted[kCostBuffers / 2], longest, us[0]);
}

void usage(FILE* to) {
    std::fprintf(to, "usage: shared-interop.exe [--adapter <index>] [--texture-open-expected]\n"
                     "                          [--buffer-open-expected] [--help]\n"
                     "  D3D11 creates shared buffers, D3D12 opens them on the same adapter and reads every byte,\n"
                     "  then the reverse through a shared fence; one negative shared-texture open; D3D12 creates\n"
                     "  shared buffers that D3D12 opens and D3D11 must not; the cost of a D3D12 buffer create.\n"
                     "  --adapter <index>          the DXGI adapter (default: the first hardware adapter)\n"
                     "  --texture-open-expected    the texture open must succeed (a driver that opens textures)\n"
                     "  --buffer-open-expected     the D3D12 open of a D3D12 shared buffer must succeed\n"
                     "  exit 0 every case passed, 1 a case failed, 2 bad arguments, 3 no device of one API\n");
}

} // namespace

int main(int argc, char** argv) {
    int adapter_index = -1;
    bool texture_open_expected = false;
    bool buffer_open_expected = false;
    for (int i = 1; i < argc; ++i) {
        if (!std::strcmp(argv[i], "--help")) {
            usage(stdout);
            return 0;
        }
        if (!std::strcmp(argv[i], "--texture-open-expected")) {
            texture_open_expected = true;
        } else if (!std::strcmp(argv[i], "--buffer-open-expected")) {
            buffer_open_expected = true;
        } else if (!std::strcmp(argv[i], "--adapter") && i + 1 < argc) {
            char* end = nullptr;
            adapter_index = static_cast<int>(std::strtol(argv[++i], &end, 10));
            if (!end || *end || adapter_index < 0) {
                usage(stderr);
                return 2;
            }
        } else {
            usage(stderr);
            return 2;
        }
    }
    if (!CreateThread(nullptr, 0, watchdog, nullptr, 0, nullptr)) {
        out("SHARED-INTEROP FAIL cases=0 failed=0 (no watchdog thread)\n");
        return 1;
    }
    start_debug_capture();

    Gpu g;
    ComPtr<IDXGIFactory1> factory;
    HRESULT hr = CreateDXGIFactory1(IID_PPV_ARGS(&factory));
    for (UINT i = 0; SUCCEEDED(hr) && !g.adapter; ++i) {
        ComPtr<IDXGIAdapter1> a;
        if (factory->EnumAdapters1(i, &a) == DXGI_ERROR_NOT_FOUND) break;
        DXGI_ADAPTER_DESC1 ad{};
        a->GetDesc1(&ad);
        if (adapter_index >= 0 ? static_cast<int>(i) == adapter_index : !(ad.Flags & DXGI_ADAPTER_FLAG_SOFTWARE))
            g.adapter = a;
    }
    if (!g.adapter) {
        out("SHARED-INTEROP FAIL cases=0 failed=0 (no adapter, factory %08lx)\n", static_cast<unsigned long>(hr));
        return 3;
    }
    DXGI_ADAPTER_DESC1 ad{};
    g.adapter->GetDesc1(&ad);
    g.luid = ad.AdapterLuid;
    out("ADAPTER luid=%08lx:%08lx vendor=0x%04x device=0x%04x desc=%ls\n", static_cast<unsigned long>(g.luid.HighPart),
        static_cast<unsigned long>(g.luid.LowPart), ad.VendorId, ad.DeviceId, ad.Description);

    const D3D_FEATURE_LEVEL levels[] = {D3D_FEATURE_LEVEL_11_1, D3D_FEATURE_LEVEL_11_0};
    ComPtr<ID3D11Device> base11;
    ComPtr<ID3D11DeviceContext> context11;
    D3D_FEATURE_LEVEL got11{};
    hr = D3D11CreateDevice(g.adapter.Get(), D3D_DRIVER_TYPE_UNKNOWN, nullptr, 0, levels, 2, D3D11_SDK_VERSION, &base11,
                           &got11, &context11);
    if (SUCCEEDED(hr)) hr = base11.As(&g.d11);
    if (SUCCEEDED(hr)) hr = context11.As(&g.c11);
    if (FAILED(hr)) {
        out("DEVICE11 none hr=%08lx\n", static_cast<unsigned long>(hr));
        out("SHARED-INTEROP FAIL cases=0 failed=0 (no D3D11 device)\n");
        return 3;
    }
    out("DEVICE11 luid=%08lx:%08lx feature_level=0x%x desc=%ls\n", static_cast<unsigned long>(g.luid.HighPart),
        static_cast<unsigned long>(g.luid.LowPart), static_cast<unsigned>(got11), ad.Description);
    hr = D3D12CreateDevice(g.adapter.Get(), D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&g.d12));
    if (FAILED(hr)) {
        out("DEVICE12 none hr=%08lx\n", static_cast<unsigned long>(hr));
        out("SHARED-INTEROP FAIL cases=0 failed=0 (no D3D12 device)\n");
        return 3;
    }
    const LUID luid12 = g.d12->GetAdapterLuid();
    out("DEVICE12 luid=%08lx:%08lx desc=%ls\n", static_cast<unsigned long>(luid12.HighPart),
        static_cast<unsigned long>(luid12.LowPart), ad.Description);
    report("same-adapter", luid12.HighPart == g.luid.HighPart && luid12.LowPart == g.luid.LowPart,
           "d3d11 %08lx:%08lx d3d12 %08lx:%08lx", static_cast<unsigned long>(g.luid.HighPart),
           static_cast<unsigned long>(g.luid.LowPart), static_cast<unsigned long>(luid12.HighPart),
           static_cast<unsigned long>(luid12.LowPart));

    D3D12_COMMAND_QUEUE_DESC qd{};
    qd.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
    g.own_event = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    g.shared_event = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    hr = g.d12->CreateCommandQueue(&qd, IID_PPV_ARGS(&g.queue));
    if (SUCCEEDED(hr)) hr = g.d12->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&g.allocator));
    if (SUCCEEDED(hr))
        hr = g.d12->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, g.allocator.Get(), nullptr,
                                      IID_PPV_ARGS(&g.list));
    if (SUCCEEDED(hr)) hr = g.list->Close();
    if (SUCCEEDED(hr)) hr = g.d12->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&g.own));
    if (FAILED(hr) || !g.own_event || !g.shared_event) {
        report("d3d12-queue", false, "queue, list or fence %08lx", static_cast<unsigned long>(hr));
        summary_and_exit(1);
    }

    // The shared fence of both directions: created by D3D11, opened by D3D12.
    HANDLE fence_handle = nullptr;
    hr = g.d11->CreateFence(0, D3D11_FENCE_FLAG_SHARED, IID_PPV_ARGS(&g.f11));
    if (SUCCEEDED(hr)) hr = g.f11->CreateSharedHandle(nullptr, GENERIC_ALL, nullptr, &fence_handle);
    if (SUCCEEDED(hr)) hr = g.d12->OpenSharedHandle(fence_handle, IID_PPV_ARGS(&g.f12));
    if (fence_handle) CloseHandle(fence_handle);
    report("shared-fence", SUCCEEDED(hr) && g.f12, "d3d11 creates, d3d12 opens: %08lx", static_cast<unsigned long>(hr));
    if (FAILED(hr) || !g.f12) summary_and_exit(1);

    const BufferCase cases[] = {
        {"buffer-100", 100, D3D11_BIND_VERTEX_BUFFER | D3D11_BIND_SHADER_RESOURCE, 0, 0},
        {"buffer-4096", 4096, D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_UNORDERED_ACCESS, 0, 0},
        {"buffer-65536", 65536, D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_UNORDERED_ACCESS, 0, 0},
        {"structured-4096-s16", 4096, D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_UNORDERED_ACCESS,
         D3D11_RESOURCE_MISC_BUFFER_STRUCTURED, 16},
        // The two variables of buffer-100, one at a time: its size with the binds of buffer-4096, and its binds
        // with the size of buffer-4096. The order above stays, so each earlier case keeps its seed.
        {"buffer-100-uav", 100, D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_UNORDERED_ACCESS, 0, 0},
        {"buffer-4096-vb", 4096, D3D11_BIND_VERTEX_BUFFER | D3D11_BIND_SHADER_RESOURCE, 0, 0},
    };
    uint32_t seed = 1;
    for (const BufferCase& c : cases) buffer_case(g, c, Form::Kmt, seed++);
    buffer_nt_case(g, cases[1]);
    texture_case(g, texture_open_expected);
    const CreatedCase created[] = {
        {"d12-buffer-100", 100, false},
        {"d12-buffer-4096-uav", 4096, true},
        {"d12-buffer-65536-uav", 65536, true},
    };
    seed = 101;
    for (const CreatedCase& c : created) created_case(g, c, buffer_open_expected, seed++);
    create_cost_case(g);
    summary_and_exit(0);
}
