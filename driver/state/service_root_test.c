// Actual Origin/SharedRoot bodies with only the Windows API boundary replaced.
// No registry, SCM, device, GPU or service operation reaches Windows.
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <winsvc.h>
#include <stdio.h>
#include <string.h>
#include "transfer.h"

// GetSharedServiceRegistryStateKey requires this right on the service handle.
// Keep the expected mask in one place, separate from extracted production code.
#define REQUIRED_SERVICE_ACCESS SERVICE_QUERY_CONFIG

static unsigned checks, failures, cases;
static const char *case_name;
#define CHECK(x) do { ++checks; if (!(x)) { ++failures; \
    printf("FAIL %s line %u: %s\n", case_name, (unsigned)__LINE__, #x); } } while (0)
enum { MANAGER, SERVICE, SHARED, ROOT, HANDLE_COUNT };
static unsigned tokens[HANDLE_COUNT], live[HANDLE_COUNT], opened[HANDLE_COUNT], closed[HANDLE_COUNT];
static unsigned manager_calls, service_calls, shared_calls, root_calls, directory_calls;
static DWORD manager_error, service_error, shared_error, root_error, last_error, service_access;
static REGSAM expected_key_access;
static DWORD directory_error, expected_directory_capacity;
static const WCHAR *directory_path;
static BOOL directory_unterminated;
#define FAKE_SC(i) ((SC_HANDLE)(void*)&tokens[i])
#define FAKE_KEY(i) ((HKEY)(void*)&tokens[i])

static void Reset(const char *name, DWORD manager, DWORD service, DWORD shared, DWORD root, REGSAM access)
{
    ++cases; case_name = name;
    memset(live, 0, sizeof(live)); memset(opened, 0, sizeof(opened)); memset(closed, 0, sizeof(closed));
    manager_calls = service_calls = shared_calls = root_calls = 0;
    directory_calls = 0; directory_error = 0; directory_path = L"X:\\ServiceState";
    directory_unterminated = FALSE; expected_directory_capacity = 0;
    manager_error = manager; service_error = service; shared_error = shared; root_error = root;
    last_error = ERROR_SUCCESS; service_access = 0; expected_key_access = access;
}
static void Opened(unsigned index)
{
    CHECK(!live[index]); live[index] = 1; ++opened[index];
}
static DWORD WINAPI FakeGetLastError(void) { return last_error; }
static SC_HANDLE WINAPI FakeOpenSCManagerW(LPCWSTR machine, LPCWSTR database, DWORD access)
{
    ++manager_calls; CHECK(machine == NULL && database == NULL); CHECK(access == SC_MANAGER_CONNECT);
    if (manager_error) { last_error = manager_error; return NULL; }
    Opened(MANAGER); return FAKE_SC(MANAGER);
}
static SC_HANDLE WINAPI FakeOpenServiceW(SC_HANDLE manager, LPCWSTR name, DWORD access)
{
    ++service_calls; CHECK(manager == FAKE_SC(MANAGER) && live[MANAGER]);
    CHECK(name && !wcscmp(name, L"bc250kmd")); CHECK(access == REQUIRED_SERVICE_ACCESS);
    if (service_error) { last_error = service_error; return NULL; }
    service_access = access; Opened(SERVICE); return FAKE_SC(SERVICE);
}
static DWORD WINAPI FakeGetSharedServiceRegistryStateKey(SC_HANDLE service,
    SERVICE_SHARED_REGISTRY_STATE_TYPE type, DWORD access, HKEY *out)
{
    ++shared_calls; CHECK(service == FAKE_SC(SERVICE) && live[SERVICE]);
    CHECK(type == ServiceSharedRegistryPersistentState); CHECK(access == expected_key_access); CHECK(out != NULL);
    // Model the actual boundary: a handle without QUERY_CONFIG fails even if
    // the backing shared root exists. Registry error injection is independent.
    if ((service_access & REQUIRED_SERVICE_ACCESS) != REQUIRED_SERVICE_ACCESS) return ERROR_ACCESS_DENIED;
    if (shared_error) return shared_error;
    Opened(SHARED); *out = FAKE_KEY(SHARED); return ERROR_SUCCESS;
}
static LSTATUS WINAPI FakeRegOpenKeyExW(HKEY parent, LPCWSTR child, DWORD options, REGSAM access, PHKEY out)
{
    ++root_calls; CHECK(parent == FAKE_KEY(SHARED) && live[SHARED]);
    CHECK(child && !wcscmp(child, L"StateV2")); CHECK(options == 0 && access == KEY_READ); CHECK(out != NULL);
    if (root_error) return (LSTATUS)root_error;
    Opened(ROOT); *out = FAKE_KEY(ROOT); return ERROR_SUCCESS;
}
static DWORD WINAPI FakeGetSharedServiceDirectory(SC_HANDLE service, SERVICE_SHARED_DIRECTORY_TYPE type,
    WCHAR *out, DWORD capacity, DWORD *needed)
{
    size_t length; DWORD i;
    ++directory_calls; CHECK(service == FAKE_SC(SERVICE) && live[SERVICE]);
    CHECK(type == ServiceSharedDirectoryPersistentState);
    CHECK(capacity == expected_directory_capacity && out != NULL && needed != NULL);
    if ((service_access & REQUIRED_SERVICE_ACCESS) != REQUIRED_SERVICE_ACCESS) return ERROR_ACCESS_DENIED;
    if (directory_error) {
        // Failed calls can leave partial output; the caller must not expose it.
        out[0] = L'X'; *needed = capacity + 1; return directory_error;
    }
    if (directory_unterminated) {
        for (i = 0; i < capacity; ++i) out[i] = L'Z';
        *needed = capacity; return ERROR_SUCCESS;
    }
    length = wcslen(directory_path); CHECK(length < capacity);
    if (length >= capacity) return ERROR_INSUFFICIENT_BUFFER;
    memcpy(out, directory_path, (length + 1) * sizeof(WCHAR));
    *needed = (DWORD)length + 1; return ERROR_SUCCESS;
}
static BOOL WINAPI FakeCloseServiceHandle(SC_HANDLE handle)
{
    unsigned index;
    if (handle == FAKE_SC(MANAGER)) index = MANAGER;
    else if (handle == FAKE_SC(SERVICE)) index = SERVICE;
    else { CHECK(0); return FALSE; }
    CHECK(live[index] == 1); live[index] = 0; ++closed[index];
    // An error must be saved before cleanup, not recovered after handle close.
    last_error = ERROR_INVALID_HANDLE; return TRUE;
}
static LSTATUS WINAPI FakeRegCloseKey(HKEY handle)
{
    unsigned index;
    if (handle == FAKE_KEY(SHARED)) index = SHARED;
    else if (handle == FAKE_KEY(ROOT)) index = ROOT;
    else { CHECK(0); return ERROR_INVALID_HANDLE; }
    CHECK(live[index] == 1); live[index] = 0; ++closed[index];
    last_error = ERROR_INVALID_HANDLE; return ERROR_SUCCESS;
}
#define GetLastError FakeGetLastError
#define OpenSCManagerW FakeOpenSCManagerW
#define OpenServiceW FakeOpenServiceW
#define GetSharedServiceRegistryStateKey FakeGetSharedServiceRegistryStateKey
#define RegOpenKeyExW FakeRegOpenKeyExW
#define CloseServiceHandle FakeCloseServiceHandle
#define RegCloseKey FakeRegCloseKey
#define GetSharedServiceDirectory FakeGetSharedServiceDirectory
#define API

/* ACTUAL_ORIGIN */
/* ACTUAL_SHARED_ROOT */
/* ACTUAL_LOG_DIRECTORY */

static void CheckClosed(unsigned expected_shared_calls)
{
    unsigned i;
    for (i = 0; i < HANDLE_COUNT; ++i) {
        CHECK(live[i] == 0); CHECK(opened[i] == closed[i]); CHECK(opened[i] <= 1);
    }
    CHECK(manager_calls == 1);
    CHECK(service_calls == (manager_error ? 0u : 1u));
    CHECK(shared_calls == expected_shared_calls);
}
static void CheckOrigin(const char *name, DWORD manager, DWORD service, DWORD shared, DWORD root,
    DWORD expected_error, UINT expected_origin)
{
    UINT origin = 99; DWORD result;
    Reset(name, manager, service, shared, root, KEY_READ);
    result = Origin(&origin);
    CHECK(result == expected_error); CHECK(origin == expected_origin);
    CHECK(root_calls == ((manager || service || shared) ? 0u : 1u));
    CHECK(directory_calls == 0);
    CheckClosed((manager || service) ? 0u : 1u);
}
static void CheckShared(const char *name, DWORD manager, DWORD service, DWORD shared,
    REGSAM access, DWORD expected_error)
{
    HKEY key = NULL; DWORD result;
    Reset(name, manager, service, shared, 0, access);
    result = SharedRoot(access, &key);
    CHECK(result == expected_error); CHECK(root_calls == 0);
    if (!expected_error) {
        CHECK(key == FAKE_KEY(SHARED) && live[SHARED] == 1);
        CHECK(closed[SHARED] == 0); // Ownership of the returned key belongs to the caller.
        if (key == FAKE_KEY(SHARED) && live[SHARED]) (void)FakeRegCloseKey(key);
    } else CHECK(key == NULL);
    CHECK(directory_calls == 0);
    CheckClosed((manager || service) ? 0u : 1u);
}
static void CheckDirectory(const char *name, DWORD manager, DWORD service, DWORD directory,
    const WCHAR *path, BOOL unterminated, DWORD capacity, DWORD expected_error, const WCHAR *expected_path)
{
    WCHAR storage[66], *out = storage + 1; DWORD result, i;
    Reset(name, manager, service, 0, 0, 0);
    directory_error = directory; directory_path = path; directory_unterminated = unterminated;
    expected_directory_capacity = capacity - 5;
    for (i = 0; i < sizeof(storage) / sizeof(storage[0]); ++i) storage[i] = 0x5a5a;
    result = AmdgpuClientLogDirectory(out, capacity);
    CHECK(result == expected_error); CHECK(root_calls == 0);
    CHECK(directory_calls == ((manager || service) ? 0u : 1u));
    CHECK(storage[0] == 0x5a5a);
    for (i = capacity + 1; i < sizeof(storage) / sizeof(storage[0]); ++i) CHECK(storage[i] == 0x5a5a);
    if (expected_error) CHECK(out[0] == 0);
    else { CHECK(expected_path != NULL); CHECK(!wcscmp(out, expected_path)); CHECK(wcslen(out) < capacity); }
    CheckClosed(0);
}
int main(void)
{
    static const DWORD errors[] = { ERROR_ACCESS_DENIED, ERROR_PRIVILEGE_NOT_HELD,
        ERROR_NOT_ENOUGH_MEMORY, ERROR_INVALID_DATA };
    static const REGSAM access[] = { KEY_READ, KEY_WRITE, KEY_READ | KEY_WRITE,
        KEY_READ | KEY_WRITE | DELETE, KEY_QUERY_VALUE, KEY_READ | KEY_WOW64_32KEY };
    unsigned i;
    WCHAR longest[59], longest_expected[64], invalid[8];
    CheckOrigin("fresh service missing", 0, ERROR_SERVICE_DOES_NOT_EXIST, 0, 0, 0, AMDGPU_TRANSFER_FRESH);
    CheckOrigin("legacy shared root absent", 0, 0, ERROR_FILE_NOT_FOUND, 0, 0, AMDGPU_TRANSFER_LEGACY);
    CheckOrigin("legacy shared path absent", 0, 0, ERROR_PATH_NOT_FOUND, 0, 0, AMDGPU_TRANSFER_LEGACY);
    CheckOrigin("legacy StateV2 absent", 0, 0, 0, ERROR_FILE_NOT_FOUND, 0, AMDGPU_TRANSFER_LEGACY);
    CheckOrigin("current StateV2 present", 0, 0, 0, 0, 0, AMDGPU_TRANSFER_CURRENT);
    // Only absence recognized at the specific boundary classifies an origin.
    CheckOrigin("manager absence remains error", ERROR_FILE_NOT_FOUND, 0, 0, 0, ERROR_FILE_NOT_FOUND, 0);
    CheckOrigin("service other absence remains error", 0, ERROR_FILE_NOT_FOUND, 0, 0, ERROR_FILE_NOT_FOUND, 0);
    CheckOrigin("StateV2 path error preserved", 0, 0, 0, ERROR_PATH_NOT_FOUND, ERROR_PATH_NOT_FOUND, 0);
    for (i = 0; i < sizeof(errors) / sizeof(errors[0]); ++i) {
        CheckOrigin("origin manager error", errors[i], 0, 0, 0, errors[i], 0);
        CheckOrigin("origin service error", 0, errors[i], 0, 0, errors[i], 0);
        CheckOrigin("origin shared error", 0, 0, errors[i], 0, errors[i], 0);
        CheckOrigin("origin StateV2 error", 0, 0, 0, errors[i], errors[i], 0);
        CheckShared("shared manager error", errors[i], 0, 0, KEY_READ, errors[i]);
        CheckShared("shared service error", 0, errors[i], 0, KEY_READ | KEY_WRITE, errors[i]);
        CheckShared("shared registry error", 0, 0, errors[i], KEY_READ | KEY_WRITE | DELETE, errors[i]);
    }
    CheckShared("shared service absent", 0, ERROR_SERVICE_DOES_NOT_EXIST, 0, KEY_READ, ERROR_SERVICE_DOES_NOT_EXIST);
    CheckShared("shared root absent", 0, 0, ERROR_FILE_NOT_FOUND, KEY_READ, ERROR_FILE_NOT_FOUND);
    CheckShared("shared path absent", 0, 0, ERROR_PATH_NOT_FOUND, KEY_READ | KEY_WRITE, ERROR_PATH_NOT_FOUND);
    for (i = 0; i < sizeof(access) / sizeof(access[0]); ++i)
        CheckShared("shared exact access and ownership", 0, 0, 0, access[i], ERROR_SUCCESS);
    CheckDirectory("directory suffix", 0, 0, 0, L"X:\\ServiceState", FALSE, 64, 0, L"X:\\ServiceState\\Logs");
    CheckDirectory("directory existing separator", 0, 0, 0, L"X:\\ServiceState\\", FALSE, 64, 0, L"X:\\ServiceState\\Logs");
    CheckDirectory("directory minimum capacity", 0, 0, 0, L"X", FALSE, 7, 0, L"X\\Logs");
    for (i = 0; i < 58; ++i) longest[i] = L'Q';
    longest[58] = 0; memcpy(longest_expected, longest, 58 * sizeof(WCHAR));
    memcpy(longest_expected + 58, L"\\Logs", 6 * sizeof(WCHAR));
    CheckDirectory("directory exact final capacity", 0, 0, 0, longest, FALSE, 64, 0, longest_expected);
    CheckDirectory("directory empty result", 0, 0, 0, L"", FALSE, 64, ERROR_INVALID_DATA, NULL);
    CheckDirectory("directory missing terminator", 0, 0, 0, L"", TRUE, 64, ERROR_INVALID_DATA, NULL);
    for (i = 0; i < sizeof(errors) / sizeof(errors[0]); ++i) {
        CheckDirectory("directory manager error", errors[i], 0, 0, L"", FALSE, 64, errors[i], NULL);
        CheckDirectory("directory service error", 0, errors[i], 0, L"", FALSE, 64, errors[i], NULL);
        CheckDirectory("directory API error", 0, 0, errors[i], L"", FALSE, 64, errors[i], NULL);
    }
    CheckDirectory("directory service absent", 0, ERROR_SERVICE_DOES_NOT_EXIST, 0, L"", FALSE, 64, ERROR_SERVICE_DOES_NOT_EXIST, NULL);
    CheckDirectory("directory absent", 0, 0, ERROR_PATH_NOT_FOUND, L"", FALSE, 64, ERROR_PATH_NOT_FOUND, NULL);
    CheckDirectory("directory buffer too small", 0, 0, ERROR_INSUFFICIENT_BUFFER, L"", FALSE, 7, ERROR_INSUFFICIENT_BUFFER, NULL);
    CheckDirectory("directory more data", 0, 0, ERROR_MORE_DATA, L"", FALSE, 64, ERROR_MORE_DATA, NULL);
    Reset("directory invalid arguments", 0, 0, 0, 0, 0);
    for (i = 0; i < 8; ++i) invalid[i] = 0x5a5a;
    CHECK(AmdgpuClientLogDirectory(NULL, 64) == ERROR_INVALID_PARAMETER);
    CHECK(AmdgpuClientLogDirectory(invalid, 0) == ERROR_INVALID_PARAMETER);
    CHECK(AmdgpuClientLogDirectory(invalid, 6) == ERROR_INVALID_PARAMETER);
    for (i = 0; i < 8; ++i) CHECK(invalid[i] == 0x5a5a);
    CHECK(manager_calls == 0 && service_calls == 0 && shared_calls == 0 && directory_calls == 0 && root_calls == 0);
    for (i = 0; i < HANDLE_COUNT; ++i) CHECK(opened[i] == 0 && closed[i] == 0);
    printf("service root: %u cases, %u checks, %u failures; fake APIs only\n", cases, checks, failures);
    return failures ? 1 : 0;
}
