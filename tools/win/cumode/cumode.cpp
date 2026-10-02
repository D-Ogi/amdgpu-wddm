// cumode - switch the BC-250 GPU between 24 compute units (the firmware's harvest, stock) and 40.
//
// The driver does the work at device start (driver/kmd/cumode.c, docs/design/cu-mode.md). This tool only
//   - writes the setting (HKLM\SYSTEM\CurrentControlSet\Services\bc250kmd\Parameters: CuMode, CuDisableWgp),
//   - asks for the reboot that applies it, or, opt-in and for lab validation, restarts the display device,
//   - confirms a 40 CU start once it has been healthy (BC250_ESCAPE_RUN_CU_MODE, OP_CONFIRM),
//   - reports what the driver applied, the register values it read back, and the GPU temperature.
// It never touches clocks, voltage, the SMU, BIOS or firmware. Nothing is permanent beyond two registry values.
//
//   cumode                          the window
//   cumode status                   setting, this start, registers, temperature
//   cumode set 24|40 [--disable M]  the setting (administrator); applies at the next start
//   cumode apply                    what the next start will do, and how to get there
//   cumode reboot                   restart Windows now (administrator)
//   cumode restart-device           restart the display device instead (administrator; lab validation only)
//   cumode confirm [--wait S]       confirm a pending 40 CU start (administrator); waits up to S seconds
//
// Exit codes: 0 done, 1 failed, 2 bad usage or no BC-250 with our driver, 3 refused by the driver.
#include <windows.h>
#include <winternl.h>
#include <setupapi.h>
#include <initguid.h>
#include <ntddvdeo.h>
#include <d3dkmthk.h>
#include <cstdio>
#include <cstdarg>
#include <cwchar>
#include <string>

extern "C" {
#include "../../../driver/kmd/bc250kmd_escape.h"
#include "../../../driver/shim/include/bc250_cu_mode.h"
}

static const wchar_t kParameters[] = L"SYSTEM\\CurrentControlSet\\Services\\bc250kmd\\Parameters";
static const wchar_t kHardwareId[] = L"PCI\\VEN_1002&DEV_13FE";
static const wchar_t kWarning[] =
    L"40 CUs draw more power and heat than 24: the unlock's reference measured about +30 W at 1500 MHz. "
    L"This tool changes no clock and no voltage (the lab runs 1000 MHz / 820 mV). Watch the temperature "
    L"and stop above 87 C. An unconfirmed 40 CU start falls back to 24 at the next start.";

static const wchar_t* const kReason[BC250_CU_REASON_COUNT] = {
    L"none",
    L"CuMode is neither 24 nor 40",
    L"CuDisableWgp names bits outside the part",
    L"an earlier 40 CU start was never confirmed: fell back to 24",
    L"the pending mark could not be written",
    L"not a 1002:13FE",
    L"RLC power gating is on",
    L"stock registers not as measured",
    L"a written value did not read back: stock restored",
    L"stock did not read back either",
    L"GFX bring-up did not reach the constants stage",
    L"topology outside what this was written for",
};

// ---- output: the console, or the window's text box -------------------------------------------------------------

static std::wstring g_out;

static void Say(const wchar_t* format, ...)
{
    wchar_t line[1024];
    va_list args;
    va_start(args, format);
    _vsnwprintf_s(line, _countof(line), _TRUNCATE, format, args);
    va_end(args);
    g_out += line;
    g_out += L"\r\n";
}

static const wchar_t* ReasonName(unsigned long reason)
{
    return reason < BC250_CU_REASON_COUNT ? kReason[reason] : L"(unknown reason)";
}

// ---- registry ---------------------------------------------------------------------------------------------------

static bool ReadDword(const wchar_t* name, DWORD* value)
{
    DWORD size = sizeof(*value);
    *value = 0;
    return RegGetValueW(HKEY_LOCAL_MACHINE, kParameters, name, RRF_RT_REG_DWORD, nullptr, value, &size) == ERROR_SUCCESS;
}

static LSTATUS WriteDword(HKEY key, const wchar_t* name, DWORD value)
{
    return RegSetValueExW(key, name, 0, REG_DWORD, reinterpret_cast<const BYTE*>(&value), sizeof(value));
}

static bool IsAdmin()
{
    SID_IDENTIFIER_AUTHORITY nt = SECURITY_NT_AUTHORITY;
    PSID admins = nullptr;
    BOOL member = FALSE;
    if (!AllocateAndInitializeSid(&nt, 2, SECURITY_BUILTIN_DOMAIN_RID, DOMAIN_ALIAS_RID_ADMINS, 0, 0, 0, 0, 0, 0, &admins))
        return false;
    if (!CheckTokenMembership(nullptr, admins, &member)) member = FALSE;
    FreeSid(admins);
    return member != FALSE;
}

// ---- the adapter ------------------------------------------------------------------------------------------------

// The GUID_DISPLAY_DEVICE_ARRIVAL interface of the first present device with the BC-250's hardware id
// (the same lookup as tools/win/bc250kmd_cli). With Restart, that device is restarted instead.
static bool FindAdapter(std::wstring* path, bool restart, DWORD* error)
{
    HDEVINFO set = SetupDiGetClassDevsW(&GUID_DISPLAY_DEVICE_ARRIVAL, nullptr, nullptr, DIGCF_PRESENT | DIGCF_DEVICEINTERFACE);
    SP_DEVICE_INTERFACE_DATA iface = { sizeof(iface) };
    union {
        SP_DEVICE_INTERFACE_DETAIL_DATA_W detail;
        BYTE space[sizeof(SP_DEVICE_INTERFACE_DETAIL_DATA_W) + 512 * sizeof(wchar_t)];
    } buffer;
    bool found = false;
    *error = ERROR_NOT_FOUND;
    if (set == INVALID_HANDLE_VALUE) { *error = GetLastError(); return false; }
    for (DWORD index = 0; !found && SetupDiEnumDeviceInterfaces(set, nullptr, &GUID_DISPLAY_DEVICE_ARRIVAL, index, &iface); index++) {
        SP_DEVINFO_DATA info = { sizeof(info) };
        wchar_t hardwareId[512] = {};
        buffer.detail.cbSize = sizeof(SP_DEVICE_INTERFACE_DETAIL_DATA_W);
        if (!SetupDiGetDeviceInterfaceDetailW(set, &iface, &buffer.detail, sizeof(buffer), nullptr, &info)) continue;
        if (!SetupDiGetDeviceRegistryPropertyW(set, &info, SPDRP_HARDWAREID, nullptr, reinterpret_cast<BYTE*>(hardwareId),
                                               sizeof(hardwareId) - sizeof(wchar_t), nullptr)) continue;
        if (_wcsnicmp(hardwareId, kHardwareId, wcslen(kHardwareId)) != 0) continue;
        found = true;
        *error = ERROR_SUCCESS;
        if (path) *path = buffer.detail.DevicePath;
        if (restart) {
            // DICS_PROPCHANGE: PnP stops and starts the device, which runs StartDevice and so the CU mode.
            SP_PROPCHANGE_PARAMS change = {};
            change.ClassInstallHeader.cbSize = sizeof(SP_CLASSINSTALL_HEADER);
            change.ClassInstallHeader.InstallFunction = DIF_PROPERTYCHANGE;
            change.StateChange = DICS_PROPCHANGE;
            change.Scope = DICS_FLAG_GLOBAL;
            if (!SetupDiSetClassInstallParamsW(set, &info, &change.ClassInstallHeader, sizeof(change)) ||
                !SetupDiCallClassInstaller(DIF_PROPERTYCHANGE, set, &info))
                *error = GetLastError();
        }
    }
    SetupDiDestroyDeviceInfoList(set);
    return found;
}

static NTSTATUS Escape(const std::wstring& path, void* data, UINT size, bool noAdapterSynchronization)
{
    D3DKMT_OPENADAPTERFROMDEVICENAME open = {};
    D3DKMT_ESCAPE escape = {};
    NTSTATUS status;
    open.pDeviceName = path.c_str();
    status = D3DKMTOpenAdapterFromDeviceName(&open);
    if (!NT_SUCCESS(status)) return status;
    escape.hAdapter = open.hAdapter;
    escape.Type = D3DKMT_ESCAPE_DRIVERPRIVATE;
    escape.Flags.Value = 0;
    escape.Flags.NoAdapterSynchronization = noAdapterSynchronization ? 1 : 0;
    escape.pPrivateDriverData = data;
    escape.PrivateDriverDataSize = size;
    status = D3DKMTEscape(&escape);
    D3DKMT_CLOSEADAPTER close = { open.hAdapter };
    D3DKMTCloseAdapter(&close);
    return status;
}

static NTSTATUS CuModeEscape(const std::wstring& path, BC250_ESCAPE_CU_MODE* data, unsigned long op, unsigned long long generation)
{
    memset(data, 0, sizeof(*data));
    data->Magic = BC250_ESCAPE_MAGIC;
    data->Command = BC250_ESCAPE_RUN_CU_MODE;
    data->AbiVersion = BC250_CU_MODE_ABI;
    data->Op = op;
    data->ExpectedGeneration = generation;
    return Escape(path, data, sizeof(*data), true);
}

// Temperature through the SMU owner's READ: administrator only, no hardware access flag, no clock change.
static bool ReadTemperature(const std::wstring& path, long* milliC)
{
    BC250_ESCAPE_CLOCK clock = {};
    clock.Magic = BC250_ESCAPE_MAGIC;
    clock.Command = BC250_ESCAPE_RUN_CLOCK;
    clock.AbiVersion = BC250_CLOCK_ABI;
    clock.Op = BC250_CLOCK_OP_READ;
    if (!NT_SUCCESS(Escape(path, &clock, sizeof(clock), true)) || clock.Status != BC250_ESCAPE_STATUS_DONE || !clock.Ready)
        return false;
    *milliC = clock.TemperatureMc;
    return true;
}

// ---- commands ---------------------------------------------------------------------------------------------------

static void Flags(unsigned long flags, wchar_t* text, size_t size)
{
    static const struct { unsigned long bit; const wchar_t* name; } names[] = {
        { BC250_CU_MODE_FLAG_VALID, L"valid" }, { BC250_CU_MODE_FLAG_PENDING, L"PENDING" },
        { BC250_CU_MODE_FLAG_CONFIRMED, L"confirmed" }, { BC250_CU_MODE_FLAG_STOCK_RECORD, L"stock-record" },
        { BC250_CU_MODE_FLAG_CONSISTENT, L"consistent" }, { BC250_CU_MODE_FLAG_WROTE, L"wrote" } };
    text[0] = 0;
    for (const auto& n : names)
        if (flags & n.bit) {
            if (text[0]) wcscat_s(text, size, L" ");
            wcscat_s(text, size, n.name);
        }
    if (!text[0]) wcscpy_s(text, size, L"-");
}

static int Status()
{
    DWORD mode = 0, disable = 0, pending = 0, confirmed = 0, lastApplied = 0, lastReason = 0, error = 0;
    bool modeSet = ReadDword(L"CuMode", &mode);
    bool disableSet = ReadDword(L"CuDisableWgp", &disable);
    bool pendingSet = ReadDword(L"CuModePending", &pending);
    bool confirmedSet = ReadDword(L"CuModeConfirmed", &confirmed);
    bool lastSet = ReadDword(L"CuModeLastApplied", &lastApplied);
    std::wstring path;
    BC250_ESCAPE_CU_MODE data;
    wchar_t flags[160];
    long milliC = 0;

    ReadDword(L"CuModeLastReason", &lastReason);
    Say(L"setting      CuMode %s%lu, CuDisableWgp %s0x%05lX, pending %s0x%lX, confirmed %s0x%lX",
        modeSet ? L"" : L"(absent) ", modeSet ? mode : 24ul, disableSet ? L"" : L"(absent) ", disable,
        pendingSet ? L"" : L"(absent) ", pending, confirmedSet ? L"" : L"(absent) ", confirmed);
    if (lastSet) Say(L"last start   applied %lu, reason %lu (%s)", lastApplied, lastReason, ReasonName(lastReason));
    if (!FindAdapter(&path, false, &error)) {
        Say(L"adapter      no present %s display device (error %lu)", kHardwareId, error);
        return 2;
    }
    NTSTATUS status = CuModeEscape(path, &data, BC250_CU_MODE_OP_READ, 0);
    if (!NT_SUCCESS(status) || data.Status != BC250_ESCAPE_STATUS_DONE) {
        Say(L"driver       the CU mode escape failed: 0x%08lX, status %lu (driver older than 0.7.174?)", status, data.Status);
        return 3;
    }
    Flags(data.Flags, flags, _countof(flags));
    Say(L"driver       version 0x%08lX, PCI id 0x%08lX, start generation %llu", data.Version, data.PciId, data.Generation);
    Say(L"this start   requested %lu, applied %lu CUs, counted %lu, reason %lu (%s)", data.Requested, data.Applied,
        data.ActiveCus, data.Reason, ReasonName(data.Reason));
    Say(L"flags        %s", flags);
    for (unsigned i = 0; i < BC250_CU_MODE_SA_COUNT; i++)
        Say(L"SE%u SA%u      CC 0x%08lX USER 0x%08lX SPI 0x%02lX  active WGPs 0x%02lX   stock CC 0x%08lX SPI 0x%02lX",
            i / 2, i % 2, data.Cc[i], data.User[i], data.Spi[i], data.ActiveWgps[i], data.StockCc[i], data.StockSpi[i]);
    Say(L"RLC          PG_CNTL 0x%08lX, PG_ALWAYS_ON_WGP_MASK 0x%08lX", data.RlcPgCntl, data.RlcAonWgpMask);
    if (ReadTemperature(path, &milliC)) Say(L"temperature  %.1f C%s", milliC / 1000.0, milliC >= 87000 ? L"  STOP AND COOL DOWN" : L"");
    else Say(L"temperature  unavailable%s", IsAdmin() ? L"" : L" (needs an administrator)");
    if (data.Flags & BC250_CU_MODE_FLAG_PENDING)
        Say(L"note         40 CUs are pending: run `cumode confirm` once the desktop has run healthy for a minute.");
    if ((modeSet ? mode : 24ul) != data.Applied)
        Say(L"note         the setting differs from this start: it applies at the next start (reboot).");
    return 0;
}

static int Set(unsigned long mode, bool haveDisable, unsigned long disable)
{
    HKEY key;
    LSTATUS result;
    if (mode != BC250_CU_MODE_STOCK && mode != BC250_CU_MODE_FULL) { Say(L"only 24 or 40"); return 2; }
    if (haveDisable && (mode != BC250_CU_MODE_FULL || disable >= (1ul << (BC250_CU_SA_MAX * BC250_CU_WGP_MAX)))) {
        Say(L"--disable is a 20-bit mask (bit sa * 5 + wgp) and only goes with 40");
        return 2;
    }
    if (!IsAdmin()) { Say(L"writing the setting needs an administrator"); return 1; }
    result = RegOpenKeyExW(HKEY_LOCAL_MACHINE, kParameters, 0, KEY_SET_VALUE, &key);
    if (result != ERROR_SUCCESS) { Say(L"cannot open the driver's Parameters key (error %ld): is bc250kmd installed?", result); return 2; }
    result = WriteDword(key, L"CuMode", mode);
    if (result == ERROR_SUCCESS)
        result = haveDisable ? WriteDword(key, L"CuDisableWgp", disable) : RegDeleteValueW(key, L"CuDisableWgp");
    if (result == ERROR_FILE_NOT_FOUND) result = ERROR_SUCCESS;
    // A new request is confirmed on its own start, never by an older one's confirmation.
    if (result == ERROR_SUCCESS) {
        result = RegDeleteValueW(key, L"CuModeConfirmed");
        if (result == ERROR_FILE_NOT_FOUND) result = ERROR_SUCCESS;
    }
    if (result == ERROR_SUCCESS) result = RegFlushKey(key);
    RegCloseKey(key);
    if (result != ERROR_SUCCESS) { Say(L"writing the setting failed (error %ld)", result); return 1; }
    Say(L"CuMode = %lu%s. It applies at the next start of the driver: reboot (`cumode reboot`).", mode,
        haveDisable ? L" with CuDisableWgp" : L"");
    if (mode == BC250_CU_MODE_FULL) Say(L"%s", kWarning);
    return 0;
}

static int Apply()
{
    DWORD mode = 24;
    std::wstring path;
    DWORD error;
    BC250_ESCAPE_CU_MODE data;
    ReadDword(L"CuMode", &mode);
    if (FindAdapter(&path, false, &error) && NT_SUCCESS(CuModeEscape(path, &data, BC250_CU_MODE_OP_READ, 0)) &&
        data.Status == BC250_ESCAPE_STATUS_DONE && data.Applied == mode) {
        Say(L"this start already runs %lu CUs; nothing to apply.", mode);
        return 0;
    }
    Say(L"CuMode %lu applies at the next start. Reboot with `cumode reboot`; `cumode restart-device` restarts only the"
        L" display device (lab validation, see docs/design/cu-mode.md).", mode);
    return 0;
}

static int Reboot()
{
    HANDLE token;
    TOKEN_PRIVILEGES privileges = {};
    if (!IsAdmin()) { Say(L"a reboot from here needs an administrator"); return 1; }
    if (OpenProcessToken(GetCurrentProcess(), TOKEN_ADJUST_PRIVILEGES | TOKEN_QUERY, &token)) {
        privileges.PrivilegeCount = 1;
        privileges.Privileges[0].Attributes = SE_PRIVILEGE_ENABLED;
        if (LookupPrivilegeValueW(nullptr, SE_SHUTDOWN_NAME, &privileges.Privileges[0].Luid))
            AdjustTokenPrivileges(token, FALSE, &privileges, 0, nullptr, nullptr);
        CloseHandle(token);
    }
    wchar_t message[] = L"cumode: restarting to apply the GPU compute unit setting";
    if (!InitiateSystemShutdownExW(nullptr, message, 10, FALSE, TRUE,
                                   SHTDN_REASON_MAJOR_HARDWARE | SHTDN_REASON_MINOR_RECONFIG | SHTDN_REASON_FLAG_PLANNED)) {
        Say(L"InitiateSystemShutdownEx failed (error %lu)", GetLastError());
        return 1;
    }
    Say(L"Windows restarts in 10 seconds.");
    return 0;
}

static int RestartDevice()
{
    DWORD error = 0;
    if (!IsAdmin()) { Say(L"restarting the device needs an administrator"); return 1; }
    Say(L"restarting the display device (lab validation only: a warm restart is not the default path)");
    if (!FindAdapter(nullptr, true, &error)) { Say(L"no present %s display device", kHardwareId); return 2; }
    if (error != ERROR_SUCCESS) { Say(L"the device restart failed (error %lu); reboot instead", error); return 1; }
    Say(L"restart requested; run `cumode status` when the desktop is back.");
    return 0;
}

static int Confirm(unsigned long waitSeconds)
{
    std::wstring path;
    DWORD error;
    BC250_ESCAPE_CU_MODE data;
    NTSTATUS status;
    if (!IsAdmin()) { Say(L"confirming needs an administrator"); return 1; }
    if (!FindAdapter(&path, false, &error)) { Say(L"no present %s display device", kHardwareId); return 2; }
    for (ULONGLONG deadline = GetTickCount64() + waitSeconds * 1000ull;;) {
        status = CuModeEscape(path, &data, BC250_CU_MODE_OP_READ, 0);
        if (!NT_SUCCESS(status) || data.Status != BC250_ESCAPE_STATUS_DONE) {
            Say(L"the CU mode escape failed: 0x%08lX", status);
            return 3;
        }
        if (!(data.Flags & BC250_CU_MODE_FLAG_PENDING)) {
            Say(data.Applied == BC250_CU_MODE_FULL ? L"40 CUs are confirmed; nothing pending." :
                                                    L"this start runs %lu CUs; nothing to confirm.", data.Applied);
            return 0;
        }
        status = CuModeEscape(path, &data, BC250_CU_MODE_OP_CONFIRM, data.Generation);
        if (NT_SUCCESS(status) && data.Status == BC250_ESCAPE_STATUS_DONE && NT_SUCCESS((NTSTATUS)data.NtStatus)) {
            Say(L"40 CUs confirmed for this request; later starts keep them.");
            return 0;
        }
        if ((NTSTATUS)data.NtStatus != (NTSTATUS)0xC00000A3L /* STATUS_DEVICE_NOT_READY */ || GetTickCount64() >= deadline) {
            Say(L"the driver refused the confirmation: NtStatus 0x%08lX (not READY: the desktop needs a minute of"
                L" healthy presents first)", data.NtStatus);
            return 3;
        }
        Sleep(5000);
    }
}

// ---- the window -------------------------------------------------------------------------------------------------

enum { IdStatus = 100, IdMode24, IdMode40, IdSave, IdReboot, IdConfirm, IdRefresh, IdLab, IdRestart };

static HWND g_text, g_mode24, g_mode40, g_restart;

static void Show(HWND window)
{
    SetWindowTextW(g_text, g_out.c_str());
    g_out.clear();
    (void)window;
}

static void Refresh(HWND window)
{
    DWORD mode = 24;
    ReadDword(L"CuMode", &mode);
    SendMessageW(g_mode24, BM_SETCHECK, mode == 40 ? BST_UNCHECKED : BST_CHECKED, 0);
    SendMessageW(g_mode40, BM_SETCHECK, mode == 40 ? BST_CHECKED : BST_UNCHECKED, 0);
    Status();
    Show(window);
}

static LRESULT CALLBACK WindowProc(HWND window, UINT message, WPARAM wParam, LPARAM lParam)
{
    switch (message) {
    case WM_CREATE: {
        HFONT font = (HFONT)GetStockObject(DEFAULT_GUI_FONT);
        HFONT mono = CreateFontW(-13, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE, DEFAULT_CHARSET, 0, 0, 0,
                                 FIXED_PITCH | FF_MODERN, L"Consolas");
        HWND warning = CreateWindowW(L"STATIC", kWarning, WS_CHILD | WS_VISIBLE, 12, 10, 760, 48, window, nullptr, nullptr, nullptr);
        g_text = CreateWindowW(L"EDIT", L"", WS_CHILD | WS_VISIBLE | WS_BORDER | WS_VSCROLL | WS_HSCROLL | ES_MULTILINE |
                               ES_READONLY | ES_AUTOVSCROLL | ES_AUTOHSCROLL, 12, 64, 760, 260, window, (HMENU)IdStatus, nullptr, nullptr);
        g_mode24 = CreateWindowW(L"BUTTON", L"24 CUs (stock)", WS_CHILD | WS_VISIBLE | BS_AUTORADIOBUTTON | WS_GROUP,
                                 12, 336, 140, 22, window, (HMENU)IdMode24, nullptr, nullptr);
        g_mode40 = CreateWindowW(L"BUTTON", L"40 CUs", WS_CHILD | WS_VISIBLE | BS_AUTORADIOBUTTON, 160, 336, 100, 22,
                                 window, (HMENU)IdMode40, nullptr, nullptr);
        HWND buttons[] = {
            CreateWindowW(L"BUTTON", L"Save (applies at next reboot)", WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON, 12, 368, 220, 28, window, (HMENU)IdSave, nullptr, nullptr),
            CreateWindowW(L"BUTTON", L"Reboot now", WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON, 240, 368, 120, 28, window, (HMENU)IdReboot, nullptr, nullptr),
            CreateWindowW(L"BUTTON", L"Confirm 40", WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON, 368, 368, 120, 28, window, (HMENU)IdConfirm, nullptr, nullptr),
            CreateWindowW(L"BUTTON", L"Refresh", WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON, 496, 368, 100, 28, window, (HMENU)IdRefresh, nullptr, nullptr),
            CreateWindowW(L"BUTTON", L"Lab: restart the device instead of rebooting", WS_CHILD | WS_VISIBLE | BS_AUTOCHECKBOX,
                          12, 408, 320, 22, window, (HMENU)IdLab, nullptr, nullptr),
        };
        g_restart = CreateWindowW(L"BUTTON", L"Restart device", WS_CHILD | WS_VISIBLE | WS_DISABLED | BS_PUSHBUTTON,
                                  340, 404, 140, 28, window, (HMENU)IdRestart, nullptr, nullptr);
        for (HWND control : { warning, g_mode24, g_mode40, g_restart }) SendMessageW(control, WM_SETFONT, (WPARAM)font, TRUE);
        for (HWND control : buttons) SendMessageW(control, WM_SETFONT, (WPARAM)font, TRUE);
        SendMessageW(g_text, WM_SETFONT, (WPARAM)(mono ? mono : font), TRUE);
        Refresh(window);
        return 0;
    }
    case WM_COMMAND:
        switch (LOWORD(wParam)) {
        case IdSave:
            Set(SendMessageW(g_mode40, BM_GETCHECK, 0, 0) == BST_CHECKED ? 40 : 24, false, 0);
            Show(window);
            return 0;
        case IdReboot:
            if (MessageBoxW(window, L"Restart Windows now to apply the setting?", L"cumode", MB_OKCANCEL | MB_ICONQUESTION) == IDOK) {
                Reboot();
                Show(window);
            }
            return 0;
        case IdConfirm:
            Confirm(0);
            Show(window);
            return 0;
        case IdRefresh:
            Refresh(window);
            return 0;
        case IdLab:
            EnableWindow(g_restart, SendMessageW((HWND)lParam, BM_GETCHECK, 0, 0) == BST_CHECKED);
            return 0;
        case IdRestart:
            if (MessageBoxW(window, L"Restart the display device now? The screen goes dark for a few seconds. "
                            L"This path is for lab validation; the default is a reboot.", L"cumode",
                            MB_OKCANCEL | MB_ICONWARNING) == IDOK) {
                RestartDevice();
                Show(window);
            }
            return 0;
        }
        break;
    case WM_DESTROY:
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcW(window, message, wParam, lParam);
}

static int Window()
{
    WNDCLASSW type = {};
    MSG message;
    type.lpfnWndProc = WindowProc;
    type.hInstance = GetModuleHandleW(nullptr);
    type.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    type.hbrBackground = (HBRUSH)(COLOR_BTNFACE + 1);
    type.lpszClassName = L"cumode";
    if (!RegisterClassW(&type)) return 1;
    HWND window = CreateWindowW(L"cumode", L"BC-250 compute units", WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX,
                                CW_USEDEFAULT, CW_USEDEFAULT, 800, 480, nullptr, nullptr, type.hInstance, nullptr);
    if (!window) return 1;
    ShowWindow(window, SW_SHOWNORMAL);
    while (GetMessageW(&message, nullptr, 0, 0) > 0) {
        TranslateMessage(&message);
        DispatchMessageW(&message);
    }
    return 0;
}

// ---- entry --------------------------------------------------------------------------------------------------------

static int Usage()
{
    Say(L"cumode [status | set 24|40 [--disable MASK] | apply | reboot | restart-device | confirm [--wait SECONDS]]");
    Say(L"without arguments: the window");
    return 2;
}

int wmain(int argc, wchar_t** argv)
{
    int code;
    if (argc < 2) {
        // Started from Explorer: the console is ours alone, so let it go and show only the window.
        DWORD processes[2];
        if (GetConsoleProcessList(processes, 2) == 1) FreeConsole();
        return Window();
    }
    const wchar_t* command = argv[1];
    if (!_wcsicmp(command, L"status")) code = Status();
    else if (!_wcsicmp(command, L"set") && argc >= 3) {
        bool haveDisable = false;
        unsigned long disable = 0;
        if (argc == 5 && !_wcsicmp(argv[3], L"--disable")) { haveDisable = true; disable = wcstoul(argv[4], nullptr, 0); }
        else if (argc != 3) return Usage(), fputws(g_out.c_str(), stdout), 2;
        code = Set(wcstoul(argv[2], nullptr, 10), haveDisable, disable);
    }
    else if (!_wcsicmp(command, L"apply")) code = Apply();
    else if (!_wcsicmp(command, L"reboot")) code = Reboot();
    else if (!_wcsicmp(command, L"restart-device")) code = RestartDevice();
    else if (!_wcsicmp(command, L"confirm")) {
        unsigned long wait = 0;
        if (argc == 4 && !_wcsicmp(argv[2], L"--wait")) wait = wcstoul(argv[3], nullptr, 10);
        else if (argc != 2) return Usage(), fputws(g_out.c_str(), stdout), 2;
        code = Confirm(wait);
    }
    else code = Usage();
    fputws(g_out.c_str(), stdout);
    return code;
}
