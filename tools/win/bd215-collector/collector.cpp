// Bounded Win32 user-mode debug-event recorder. No driver, kernel debugger or context writes.
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <dbghelp.h>
#include <psapi.h>
#include <bcrypt.h>
#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cwctype>
#include <iomanip>
#include <map>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>

using U64 = unsigned long long;
static constexpr int NO_MATCH = 10, RECORDED = 11, FAILED = 20;
static constexpr ULONG NATIVE_STREAM = 0xB2150001, WOW_STREAM = 0xB2150002, EVENT_STREAM = 0xB2150003;

struct Handle {
    HANDLE h = nullptr;
    Handle() = default;
    explicit Handle(HANDLE p) : h(p) {}
    ~Handle() { if (h && h != INVALID_HANDLE_VALUE) CloseHandle(h); }
    Handle(const Handle&) = delete;
    Handle& operator=(const Handle&) = delete;
};
static std::string hex(U64 n) { std::ostringstream s; s << "0x" << std::hex << n; return s.str(); }
static std::string utf8(const std::wstring& w) {
    if (w.empty()) return {};
    int n = WideCharToMultiByte(CP_UTF8, 0, w.data(), (int)w.size(), nullptr, 0, nullptr, nullptr);
    std::string s((size_t)n, '\0');
    WideCharToMultiByte(CP_UTF8, 0, w.data(), (int)w.size(), s.data(), n, nullptr, nullptr);
    return s;
}
static std::string quote(const std::string& s) {
    std::ostringstream o; o << '"';
    for (unsigned char c : s) {
        if (c == '"' || c == '\\') o << '\\' << c;
        else if (c < 32) o << "\\u" << std::hex << std::setw(4) << std::setfill('0') << (unsigned)c << std::dec;
        else o << c;
    }
    o << '"'; return o.str();
}
static std::string hq(U64 n) { return quote(hex(n)); }
static std::string boolean(bool b) { return b ? "true" : "false"; }
static std::string bytes(const void* p, size_t n) {
    static const char digits[] = "0123456789abcdef";
    auto b = (const unsigned char*)p; std::string s; s.reserve(n * 2);
    for (size_t i = 0; i < n; ++i) { s += digits[b[i] >> 4]; s += digits[b[i] & 15]; }
    return s;
}
struct Obj {
    std::string s = "{"; bool first = true;
    Obj& add(const char* key, const std::string& value) {
        if (!first) s += ','; first = false; s += quote(key) + ':' + value; return *this;
    }
    std::string str() const { return s + '}'; }
};
static std::string array(const std::vector<std::string>& rows) {
    std::string s = "[";
    for (size_t i = 0; i < rows.size(); ++i) { if (i) s += ','; s += rows[i]; }
    return s + ']';
}
static std::string utc() {
    SYSTEMTIME t; GetSystemTime(&t); char b[64];
    snprintf(b, sizeof(b), "%04u-%02u-%02uT%02u:%02u:%02u.%03uZ", t.wYear, t.wMonth, t.wDay,
             t.wHour, t.wMinute, t.wSecond, t.wMilliseconds); return b;
}
static void check(bool ok, const char* message) {
    if (!ok) throw std::runtime_error(std::string(message) + " error=" + std::to_string(GetLastError()));
}
static void write_new(const std::wstring& path, const std::string& s) {
    Handle f(CreateFileW(path.c_str(), GENERIC_WRITE, FILE_SHARE_READ, nullptr, CREATE_NEW, 0, nullptr));
    check(f.h != INVALID_HANDLE_VALUE, "CreateFile(CREATE_NEW)");
    DWORD n = 0; check(WriteFile(f.h, s.data(), (DWORD)s.size(), &n, nullptr) && n == s.size(), "WriteFile");
    check(FlushFileBuffers(f.h) != FALSE, "FlushFileBuffers");
}
static std::wstring lower(std::wstring s) { for (auto& c : s) c = (wchar_t)towlower(c); return s; }
static std::wstring leaf(const std::wstring& s) { return s.substr(s.find_last_of(L"\\/") + 1); }
static std::wstring path_of(HANDLE file) {
    if (!file || file == INVALID_HANDLE_VALUE) return {};
    std::vector<wchar_t> b(32768);
    DWORD n = GetFinalPathNameByHandleW(file, b.data(), (DWORD)b.size(), FILE_NAME_NORMALIZED);
    if (!n || n >= b.size()) return {};
    std::wstring s(b.data(), n);
    if (s.compare(0, 8, L"\\\\?\\UNC\\") == 0) return L"\\\\" + s.substr(8);
    if (s.compare(0, 4, L"\\\\?\\") == 0) s.erase(0, 4);
    return s;
}
static std::wstring win_arg(const std::wstring& s) {
    std::wstring out = L"\""; unsigned slash = 0;
    for (wchar_t c : s) {
        if (c == L'\\') { ++slash; continue; }
        out.append(c == L'"' ? slash * 2 + 1 : slash, L'\\'); slash = 0; out += c;
    }
    out.append(slash * 2, L'\\'); return out + L'"';
}
struct Read {
    U64 address; unsigned char data[64]{}; SIZE_T count = 0; DWORD error = 0; bool ok = false;
    Read(HANDLE process, U64 a) : address(a) {
        if (!a) { error = ERROR_INVALID_ADDRESS; return; }
        ok = ReadProcessMemory(process, (LPCVOID)(uintptr_t)a, data, sizeof(data), &count) != FALSE;
        if (!ok) error = GetLastError();
    }
    std::string json() const {
        return Obj().add("address", hq(address)).add("requested", "64").add("bytes_read", std::to_string(count))
            .add("ok", boolean(ok)).add("error", std::to_string(error)).add("hex", quote(bytes(data, count))).str();
    }
};
static std::string region(HANDLE process, U64 a) {
    MEMORY_BASIC_INFORMATION m{}; SIZE_T n = VirtualQueryEx(process, (LPCVOID)(uintptr_t)a, &m, sizeof(m));
    DWORD e = n ? 0 : GetLastError();
    return Obj().add("address", hq(a)).add("ok", boolean(n != 0)).add("error", std::to_string(e))
        .add("base", hq((U64)m.BaseAddress)).add("allocation_base", hq((U64)m.AllocationBase))
        .add("size", hq(m.RegionSize)).add("state", hq(m.State)).add("protect", hq(m.Protect))
        .add("allocation_protect", hq(m.AllocationProtect)).add("type", hq(m.Type)).str();
}
static std::string sha_file(const std::wstring& path, ULONGLONG deadline, DWORD& error) {
    Handle f(CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                         nullptr, OPEN_EXISTING, FILE_FLAG_SEQUENTIAL_SCAN, nullptr));
    if (f.h == INVALID_HANDLE_VALUE) { error = GetLastError(); return {}; }
    BCRYPT_ALG_HANDLE alg{}; BCRYPT_HASH_HANDLE hash{}; unsigned char digest[32]{}; std::string result;
    NTSTATUS status = BCryptOpenAlgorithmProvider(&alg, BCRYPT_SHA256_ALGORITHM, nullptr, 0);
    if (status >= 0) status = BCryptCreateHash(alg, &hash, nullptr, 0, nullptr, 0, 0);
    unsigned char block[65536]; DWORD n = 0;
    while (status >= 0) {
        if (GetTickCount64() >= deadline) { error = ERROR_TIMEOUT; break; }
        if (!ReadFile(f.h, block, sizeof(block), &n, nullptr)) { error = GetLastError(); break; }
        if (!n) { status = BCryptFinishHash(hash, digest, sizeof(digest), 0); if (status >= 0) result = bytes(digest, 32); break; }
        status = BCryptHashData(hash, block, n, 0);
    }
    if (status < 0) error = (DWORD)status;
    if (hash) BCryptDestroyHash(hash); if (alg) BCryptCloseAlgorithmProvider(alg, 0);
    return result;
}
struct Module { U64 base = 0; DWORD size = 0; std::wstring path; };
static Module module_at(HANDLE process, U64 base, HANDLE file) {
    Module m; m.base = base; m.path = path_of(file);
    if (m.path.empty()) { wchar_t p[32768]; DWORD n = K32GetModuleFileNameExW(process, (HMODULE)(uintptr_t)base, p, 32768); if (n) m.path.assign(p, n); }
    IMAGE_DOS_HEADER dos{}; SIZE_T n = 0; DWORD size = 0;
    if (ReadProcessMemory(process, (LPCVOID)(uintptr_t)base, &dos, sizeof(dos), &n) && n == sizeof(dos) &&
        dos.e_magic == IMAGE_DOS_SIGNATURE && dos.e_lfanew > 0 && dos.e_lfanew < 0x100000 &&
        ReadProcessMemory(process, (LPCVOID)(uintptr_t)(base + (DWORD)dos.e_lfanew + 24 + 56), &size, 4, &n) && n == 4)
        m.size = size;
    return m;
}
static Module find_module(const std::map<U64, Module>& modules, U64 ip) {
    for (const auto& row : modules) if (ip >= row.second.base && ip - row.second.base < row.second.size) return row.second;
    return {};
}
static std::string native_context(const CONTEXT& c, bool ok, DWORD error) {
    Obj r;
#define REG64(n) r.add(#n, hq(c.n))
    REG64(Rax); REG64(Rbx); REG64(Rcx); REG64(Rdx); REG64(Rsi); REG64(Rdi); REG64(Rbp); REG64(Rsp); REG64(Rip);
    REG64(R8); REG64(R9); REG64(R10); REG64(R11); REG64(R12); REG64(R13); REG64(R14); REG64(R15);
    REG64(EFlags); REG64(SegCs); REG64(SegDs); REG64(SegEs); REG64(SegFs); REG64(SegGs); REG64(SegSs);
    REG64(Dr0); REG64(Dr1); REG64(Dr2); REG64(Dr3); REG64(Dr6); REG64(Dr7); REG64(MxCsr);
#undef REG64
    return Obj().add("api", quote("GetThreadContext while stopped at the debug event; not a kernel trap frame"))
        .add("ok", boolean(ok)).add("error", std::to_string(error)).add("context_flags", hq(c.ContextFlags))
        .add("byte_count", std::to_string(sizeof(c))).add("raw_hex", quote(bytes(&c, sizeof(c)))).add("registers", r.str()).str();
}
static std::string wow_context(const WOW64_CONTEXT& c, bool ok, DWORD error) {
    Obj r;
#define REG32(n) r.add(#n, hq(c.n))
    REG32(Eax); REG32(Ebx); REG32(Ecx); REG32(Edx); REG32(Esi); REG32(Edi); REG32(Ebp); REG32(Esp); REG32(Eip);
    REG32(EFlags); REG32(SegCs); REG32(SegDs); REG32(SegEs); REG32(SegFs); REG32(SegGs); REG32(SegSs);
    REG32(Dr0); REG32(Dr1); REG32(Dr2); REG32(Dr3); REG32(Dr6); REG32(Dr7);
#undef REG32
    return Obj().add("api", quote("Wow64GetThreadContext while stopped at the debug event"))
        .add("ok", boolean(ok)).add("error", std::to_string(error)).add("context_flags", hq(c.ContextFlags))
        .add("byte_count", std::to_string(sizeof(c))).add("raw_hex", quote(bytes(&c, sizeof(c)))).add("registers", r.str()).str();
}
static std::string selectors(HANDLE thread, const WOW64_CONTEXT& c, DWORD& ds_base) {
    const DWORD sels[] = {c.SegCs, c.SegDs, c.SegEs, c.SegFs, c.SegGs, c.SegSs};
    const char* names[] = {"cs", "ds", "es", "fs", "gs", "ss"}; Obj result;
    for (size_t i = 0; i < 6; ++i) {
        WOW64_LDT_ENTRY e{}; bool ok = Wow64GetThreadSelectorEntry(thread, sels[i], &e) != FALSE;
        DWORD error = ok ? 0 : GetLastError();
        DWORD base = e.BaseLow | (DWORD(e.HighWord.Bytes.BaseMid) << 16) | (DWORD(e.HighWord.Bytes.BaseHi) << 24);
        DWORD limit = e.LimitLow | (DWORD(e.HighWord.Bits.LimitHi) << 16);
        if (e.HighWord.Bits.Granularity) limit = (limit << 12) | 0xfff;
        if (i == 1 && ok) ds_base = base;
        result.add(names[i], Obj().add("selector", hq(sels[i])).add("ok", boolean(ok)).add("error", std::to_string(error))
            .add("raw_hex", quote(bytes(&e, sizeof(e)))).add("base", hq(base)).add("limit", hq(limit))
            .add("present", boolean(e.HighWord.Bits.Pres != 0)).add("type", hq(e.HighWord.Bits.Type)).str());
    }
    result.add("limitation", quote("Descriptor-table entries, not the processor's hidden segment caches. A null selector may have no valid descriptor."));
    return result.str();
}
static std::string processor(HANDLE thread) {
    PROCESSOR_NUMBER ideal{}; GROUP_AFFINITY affinity{};
    bool iok = GetThreadIdealProcessorEx(thread, &ideal) != FALSE; DWORD ie = iok ? 0 : GetLastError();
    bool aok = GetThreadGroupAffinity(thread, &affinity) != FALSE; DWORD ae = aok ? 0 : GetLastError();
    return Obj().add("available", "false").add("number", "null")
        .add("reason", quote("Win32 user-mode inspection of a stopped remote thread does not expose its original fault CPU. Collector CPU and ideal CPU are not the fault CPU."))
        .add("ideal_processor", Obj().add("ok", boolean(iok)).add("error", std::to_string(ie))
             .add("group", std::to_string(ideal.Group)).add("number", std::to_string(ideal.Number)).str())
        .add("group_affinity", Obj().add("ok", boolean(aok)).add("error", std::to_string(ae))
             .add("group", std::to_string(affinity.Group)).add("mask", hq(affinity.Mask)).str()).str();
}
static std::string mitigations(HANDLE process) {
    static const char* names[] = {"DEP", "ASLR", "DynamicCode", "StrictHandleCheck", "SystemCallDisable", "OptionsMask",
        "ExtensionPointDisable", "ControlFlowGuard", "Signature", "FontDisable", "ImageLoad", "SystemCallFilter",
        "PayloadRestriction", "ChildProcess", "SideChannelIsolation", "UserShadowStack", "RedirectionTrust", "UserPointerAuth", "SEHOP"};
    static_assert(MaxProcessMitigationPolicy == sizeof(names) / sizeof(names[0]), "update policy inventory for the SDK");
    Obj result;
    for (int i = 0; i < MaxProcessMitigationPolicy; ++i) {
        unsigned char b[16]{}; SIZE_T n = i == ProcessDEPPolicy ? sizeof(PROCESS_MITIGATION_DEP_POLICY) :
            i == ProcessMitigationOptionsMask ? 2 * sizeof(ULONGLONG) : 4;
        bool ok = GetProcessMitigationPolicy(process, (PROCESS_MITIGATION_POLICY)i, b, n) != FALSE;
        DWORD error = ok ? 0 : GetLastError();
        result.add(names[i], Obj().add("policy", std::to_string(i)).add("ok", boolean(ok)).add("error", std::to_string(error))
            .add("byte_count", std::to_string(n)).add("raw_hex", quote(bytes(b, n))).str());
    }
    return result.str();
}
static BOOL CALLBACK dump_callback(PVOID parameter, const PMINIDUMP_CALLBACK_INPUT in, PMINIDUMP_CALLBACK_OUTPUT out) {
    if (in->CallbackType == CancelCallback) {
        out->CheckCancel = TRUE; out->Cancel = GetTickCount64() >= *(ULONGLONG*)parameter;
    }
    return TRUE;
}
struct Capture { bool complete = false; std::string json; };
static Capture capture(HANDLE process, DWORD pid, const DEBUG_EVENT& ev, const Module& module,
                       const std::wstring& out, ULONGLONG deadline) {
    Capture result; Obj root;
    Handle thread(OpenThread(THREAD_GET_CONTEXT | THREAD_QUERY_INFORMATION, FALSE, ev.dwThreadId));
    DWORD thread_error = thread.h ? 0 : GetLastError();
    CONTEXT native{}; native.ContextFlags = CONTEXT_ALL;
    bool nok = thread.h && GetThreadContext(thread.h, &native); DWORD ne = nok ? 0 : thread.h ? GetLastError() : thread_error;
    WOW64_CONTEXT wow{}; wow.ContextFlags = WOW64_CONTEXT_ALL;
    bool wok = thread.h && Wow64GetThreadContext(thread.h, &wow); DWORD we = wok ? 0 : thread.h ? GetLastError() : thread_error;
    DWORD ds_base = 0; std::string sel = selectors(thread.h, wow, ds_base);
    const auto& e = ev.u.Exception.ExceptionRecord;
    U64 ip = wok ? wow.Eip : (U64)e.ExceptionAddress;
    Read code(process, ip); bool indirect = code.count >= 7 && code.data[0] == 0xff && code.data[1] == 0x24 && code.data[2] == 0xbd;
    DWORD table_base = 0, slot_offset = 0, table_va = 0, target_va = 0;
    if (indirect && wok) {
        memcpy(&table_base, code.data + 3, 4); slot_offset = table_base + wow.Edi * 4u; table_va = ds_base + slot_offset;
    }
    Read slot(process, table_va);
    if (slot.count >= 4) memcpy(&target_va, slot.data, 4);
    Read target(process, target_va);
    DWORD hash_error = 0; std::string sha = sha_file(module.path, deadline, hash_error);
    std::vector<std::string> params;
    for (DWORD i = 0; i < e.NumberParameters && i < EXCEPTION_MAXIMUM_PARAMETERS; ++i) params.push_back(hq(e.ExceptionInformation[i]));
    root.add("schema", "1").add("utc", quote(utc())).add("pid", std::to_string(pid)).add("tid", std::to_string(ev.dwThreadId))
        .add("raw_debug_event", Obj().add("byte_count", std::to_string(sizeof(ev))).add("raw_hex", quote(bytes(&ev, sizeof(ev))))
             .add("event_code", std::to_string(ev.dwDebugEventCode)).add("first_chance", boolean(ev.u.Exception.dwFirstChance != 0))
             .add("exception_code", hq(e.ExceptionCode)).add("flags", hq(e.ExceptionFlags)).add("address", hq((U64)e.ExceptionAddress))
             .add("chained_record", hq((U64)e.ExceptionRecord)).add("number_parameters", std::to_string(e.NumberParameters))
             .add("parameters", array(params)).str())
        .add("native_context", native_context(native, nok, ne)).add("wow64_context", wow_context(wow, wok, we))
        .add("selectors", sel).add("thread_processor_number", processor(thread.h))
        .add("module", Obj().add("base", hq(module.base)).add("size", hq(module.size)).add("path", quote(utf8(module.path)))
             .add("sha256", quote(sha)).add("hash_error", std::to_string(hash_error))
             .add("hash_scope", quote("File at the debug event path, not a hash of relocated process memory")).str())
        .add("instruction", code.json())
        .add("jump_table", Obj().add("applicable", boolean(indirect)).add("base_offset", hq(table_base))
             .add("slot_offset", hq(slot_offset)).add("descriptor_ds_base", hq(ds_base))
             .add("address_basis", quote("32-bit effective offset plus queried DS descriptor base; hidden segment cache unavailable"))
             .add("slot", slot.json()).add("target", target.json()).str())
        .add("regions", Obj().add("ip", region(process, ip)).add("table", region(process, table_va))
             .add("stack", region(process, wow.Esp)).str())
        .add("process_mitigations", mitigations(process))
        .add("continuation", quote("DBG_EXCEPTION_NOT_HANDLED; the debuggee's own handler runs after capture"))
        .add("limitations", quote("No hardware trap vector/error code, CR2/CR3, CPU PTE/PFN/TLB, hidden segment cache or original fault CPU. Native context is the Win32 stopped-thread context, not asserted to be the original trapping RIP."));
    // Persist the pre-dump record first. A dump failure never discards the primary record.
    std::string before = root.str() + '\n'; write_new(out + L"\\first-exception.json", before);
    Handle dump(CreateFileW((out + L"\\first-exception.dmp").c_str(), GENERIC_WRITE, FILE_SHARE_READ, nullptr, CREATE_NEW, 0, nullptr));
    DWORD de = dump.h == INVALID_HANDLE_VALUE ? GetLastError() : 0; bool dok = false;
    EXCEPTION_RECORD record = e; EXCEPTION_POINTERS pointers{&record, &native};
    MINIDUMP_EXCEPTION_INFORMATION ex{ev.dwThreadId, &pointers, FALSE};
    MINIDUMP_USER_STREAM streams[] = {{NATIVE_STREAM, sizeof(native), &native}, {WOW_STREAM, sizeof(wow), &wow},
        {EVENT_STREAM, sizeof(ev), const_cast<DEBUG_EVENT*>(&ev)}};
    MINIDUMP_USER_STREAM_INFORMATION user{3, streams};
    MINIDUMP_CALLBACK_INFORMATION callback{dump_callback, &deadline};
    const auto flags = (MINIDUMP_TYPE)(MiniDumpWithFullMemory | MiniDumpWithFullMemoryInfo | MiniDumpWithThreadInfo);
    if (!de) {
        dok = MiniDumpWriteDump(process, pid, dump.h, flags, nok ? &ex : nullptr, &user, &callback) != FALSE;
        de = dok ? 0 : GetLastError();
    }
    LARGE_INTEGER size{}; if (dump.h != INVALID_HANDLE_VALUE) GetFileSizeEx(dump.h, &size);
    result.complete = nok && wok && !sha.empty() && dok;
    result.json = Obj().add("complete", boolean(result.complete)).add("dump_ok", boolean(dok)).add("dump_error", std::to_string(de))
        .add("dump_bytes", std::to_string(size.QuadPart)).add("dump_flags", hq(flags))
        .add("native_user_stream", hq(NATIVE_STREAM)).add("wow64_user_stream", hq(WOW_STREAM)).add("event_user_stream", hq(EVENT_STREAM)).str();
    write_new(out + L"\\capture-result.json", result.json + '\n');
    return result;
}
struct Options { DWORD pid = 0, seconds = 60; std::wstring out, module, command, ready; };
struct Watchdog { HANDLE finish = nullptr, target = nullptr; DWORD seconds = 0, pid = 0; bool owned = false; std::wstring out; };
static DWORD WINAPI watchdog_main(void* opaque) {
    const auto& w = *(Watchdog*)opaque;
    if (WaitForSingleObject(w.finish, w.seconds * 1000) == WAIT_OBJECT_0) return 0;
    // Hard bound also covers a stalled DbgHelp call. DebugSetProcessKillOnExit(FALSE)
    // makes abrupt collector exit detach rather than terminate an attached debuggee.
    if (w.owned) TerminateProcess(w.target, ERROR_TIMEOUT);
    try { write_new(w.out + L"\\watchdog-timeout.json", Obj().add("collector_exit", "20").add("pid", std::to_string(w.pid))
        .add("started_by_collector", boolean(w.owned)).add("seconds", std::to_string(w.seconds))
        .add("reason", quote("hard watchdog expired; any partial dump must not be accepted")).str() + '\n'); } catch (...) { }
    TerminateProcess(GetCurrentProcess(), FAILED);
    return 0;
}
static Options options(int argc, wchar_t** argv) {
    Options o;
    for (int i = 1; i < argc; ++i) {
        std::wstring s = argv[i];
        if (s == L"--") { for (++i; i < argc; ++i) { if (!o.command.empty()) o.command += L' '; o.command += win_arg(argv[i]); } break; }
        if (i + 1 >= argc) throw std::runtime_error("missing option value");
        std::wstring value = argv[++i];
        if (s == L"--out") o.out = value;
        else if (s == L"--module") o.module = lower(value);
        else if (s == L"--ready-file") o.ready = value;
        else if (s == L"--command-line") o.command = value;
        else if (s == L"--pid" || s == L"--seconds") {
            size_t n = 0; unsigned long v = std::stoul(value, &n);
            if (n != value.size() || !v || (s == L"--seconds" && v > 180)) throw std::runtime_error("invalid numeric argument");
            if (s == L"--pid") o.pid = v; else o.seconds = v;
        } else throw std::runtime_error("unknown option: " + utf8(s));
    }
    if (o.out.empty() || (!!o.pid == !o.command.empty())) throw std::runtime_error("use --out DIR --seconds 1..180 and either --pid PID or -- COMMAND [ARGS]");
    return o;
}
int wmain(int argc, wchar_t** argv) {
    if (argc == 2 && std::wstring(argv[1]) == L"--help") {
        puts("bd215collector --out DIR --seconds 60 [--module case-insensitive-substring] [--ready-file FILE] (--pid PID | -- EXE ARGS...)\nExit: 10 no match, 11 complete record+dump, 20 collector error. One target, first matching first-chance AV only."); return 0;
    }
    DWORD pid = 0; bool owned = false, attached = false, record_written = false, complete = false, exited = false;
    bool timed_out = false; DWORD child_exit = STILL_ACTIVE; ULONGLONG deadline = 0; std::wstring out; std::string failure;
    Handle process;
    Handle finish(CreateEventW(nullptr, TRUE, FALSE, nullptr)), watchdog;
    Watchdog watch;
    try {
        Options o = options(argc, argv); out = o.out; owned = !o.command.empty();
        if (!CreateDirectoryW(out.c_str(), nullptr)) check(GetLastError() == ERROR_ALREADY_EXISTS, "CreateDirectory(out)");
        check(GetFileAttributesW((out + L"\\first-exception.json").c_str()) == INVALID_FILE_ATTRIBUTES &&
              GetFileAttributesW((out + L"\\run-result.json").c_str()) == INVALID_FILE_ATTRIBUTES, "output directory already used");
        deadline = GetTickCount64() + U64(o.seconds) * 1000;
        if (owned) {
            STARTUPINFOW si{}; si.cb = sizeof(si); PROCESS_INFORMATION pi{};
            si.dwFlags = STARTF_USESTDHANDLES; si.hStdInput = GetStdHandle(STD_INPUT_HANDLE);
            si.hStdOutput = GetStdHandle(STD_OUTPUT_HANDLE); si.hStdError = GetStdHandle(STD_ERROR_HANDLE);
            check(CreateProcessW(nullptr, o.command.data(), nullptr, nullptr, TRUE, DEBUG_ONLY_THIS_PROCESS | CREATE_NO_WINDOW,
                                 nullptr, nullptr, &si, &pi) != FALSE, "CreateProcess(DEBUG_ONLY_THIS_PROCESS)");
            pid = pi.dwProcessId; process.h = pi.hProcess; CloseHandle(pi.hThread); attached = true;
        } else {
            pid = o.pid;
            process.h = OpenProcess(PROCESS_QUERY_INFORMATION | PROCESS_VM_READ | PROCESS_DUP_HANDLE | SYNCHRONIZE, FALSE, pid);
            check(process.h != nullptr, "OpenProcess");
            check(DebugActiveProcess(pid) != FALSE, "DebugActiveProcess"); attached = true;
        }
        check(DebugSetProcessKillOnExit(FALSE) != FALSE, "DebugSetProcessKillOnExit(FALSE)");
        check(finish.h != nullptr, "CreateEvent(watchdog)");
        watch.finish = finish.h; watch.target = process.h; watch.seconds = o.seconds + 1;
        watch.pid = pid; watch.owned = owned; watch.out = out;
        watchdog.h = CreateThread(nullptr, 0, watchdog_main, &watch, 0, nullptr);
        check(watchdog.h != nullptr, "CreateThread(watchdog)");
        BOOL wow = FALSE; check(IsWow64Process(process.h, &wow) && wow, "target must be x86 under WoW64");
        if (!o.ready.empty()) write_new(o.ready, std::to_string(pid) + '\n');
        std::map<U64, Module> modules; bool initial_break = false, initial_wow_break = false;
        while (GetTickCount64() < deadline) {
            DEBUG_EVENT ev{};
            if (!WaitForDebugEventEx(&ev, 100)) { check(GetLastError() == ERROR_SEM_TIMEOUT, "WaitForDebugEventEx"); continue; }
            DWORD continuation = DBG_CONTINUE;
            try {
                if (ev.dwDebugEventCode == CREATE_PROCESS_DEBUG_EVENT || ev.dwDebugEventCode == LOAD_DLL_DEBUG_EVENT) {
                    bool cp = ev.dwDebugEventCode == CREATE_PROCESS_DEBUG_EVENT;
                    HANDLE file = cp ? ev.u.CreateProcessInfo.hFile : ev.u.LoadDll.hFile;
                    U64 base = (U64)(cp ? ev.u.CreateProcessInfo.lpBaseOfImage : ev.u.LoadDll.lpBaseOfDll);
                    modules[base] = module_at(process.h, base, file); if (file) CloseHandle(file);
                } else if (ev.dwDebugEventCode == UNLOAD_DLL_DEBUG_EVENT) modules.erase((U64)ev.u.UnloadDll.lpBaseOfDll);
                else if (ev.dwDebugEventCode == EXIT_PROCESS_DEBUG_EVENT) { exited = true; child_exit = ev.u.ExitProcess.dwExitCode; }
                else if (ev.dwDebugEventCode == EXCEPTION_DEBUG_EVENT) {
                    continuation = DBG_EXCEPTION_NOT_HANDLED;
                    const auto& ex = ev.u.Exception.ExceptionRecord;
                    if (ex.ExceptionCode == EXCEPTION_BREAKPOINT && !initial_break) { initial_break = true; continuation = DBG_CONTINUE; }
                    else if (ex.ExceptionCode == 0x4000001f && !initial_wow_break) { initial_wow_break = true; continuation = DBG_CONTINUE; }
                    Module m = find_module(modules, (U64)ex.ExceptionAddress);
                    bool sentinel = ex.NumberParameters >= 2 && (ex.ExceptionInformation[1] == 0xffffffffull || ex.ExceptionInformation[1] == ~ULONG_PTR(0));
                    bool module_match = !o.module.empty() && lower(leaf(m.path)).find(o.module) != std::wstring::npos;
                    if (!record_written && ev.u.Exception.dwFirstChance && ex.ExceptionCode == EXCEPTION_ACCESS_VIOLATION && (sentinel || module_match)) {
                        record_written = true; Capture c = capture(process.h, pid, ev, m, out, deadline); complete = c.complete;
                        if (!complete) failure = "one or more required capture APIs/dump/hash failed; see capture-result.json";
                    }
                }
            } catch (...) { ContinueDebugEvent(ev.dwProcessId, ev.dwThreadId, ev.dwDebugEventCode == EXCEPTION_DEBUG_EVENT ? DBG_EXCEPTION_NOT_HANDLED : DBG_CONTINUE); throw; }
            check(ContinueDebugEvent(ev.dwProcessId, ev.dwThreadId, continuation) != FALSE, "ContinueDebugEvent");
            if (exited) { attached = false; break; }
            // Detach an externally owned process immediately after the single capture and continue it unhandled.
            if (record_written && !owned) { check(DebugActiveProcessStop(pid) != FALSE, "DebugActiveProcessStop"); attached = false; break; }
        }
        if (attached) {
            timed_out = GetTickCount64() >= deadline;
            if (owned && timed_out) { check(TerminateProcess(process.h, ERROR_TIMEOUT) != FALSE, "terminate owned timed-out child"); }
            check(DebugActiveProcessStop(pid) != FALSE, "detach at bound"); attached = false;
            if (owned && timed_out) { WaitForSingleObject(process.h, 3000); GetExitCodeProcess(process.h, &child_exit); }
        }
    } catch (const std::exception& e) { failure = e.what(); }
    if (attached) {
        // Never terminate an attached process, even on an error. Kill-on-exit has been disabled.
        if (owned) TerminateProcess(process.h, ERROR_CANCELLED);
        DebugActiveProcessStop(pid);
    }
    SetEvent(finish.h); if (watchdog.h) WaitForSingleObject(watchdog.h, INFINITE);
    int code = !failure.empty() ? FAILED : record_written && complete ? RECORDED : NO_MATCH;
    if (!out.empty()) {
        try { write_new(out + L"\\run-result.json", Obj().add("collector_exit", std::to_string(code))
            .add("pid", std::to_string(pid)).add("started_by_collector", boolean(owned))
            .add("matched", boolean(record_written)).add("complete", boolean(complete)).add("timed_out", boolean(timed_out))
            .add("debuggee_exited", boolean(exited)).add("debuggee_exit_code", std::to_string(child_exit))
            .add("error", quote(failure)).str() + '\n'); } catch (const std::exception& e) { fprintf(stderr, "bd215collector: %s\n", e.what()); return FAILED; }
    }
    fprintf(stderr, "bd215collector: result=%d pid=%lu matched=%u complete=%u timeout=%u %s\n", code, pid,
            record_written ? 1u : 0u, complete ? 1u : 0u, timed_out ? 1u : 0u, failure.c_str());
    return code;
}
