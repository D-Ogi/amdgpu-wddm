// The production router reader with the complete state boundary replaced.
#include "../../../state/client.h"
#include <cstdio>
#include <cstring>
static unsigned checks, failures, reads, closes;
static DWORD open_error;
static unsigned statuses[3], values[3];
#define CHECK(x) do { ++checks; if (!(x)) { ++failures; printf("FAIL %u: %s\n", __LINE__, #x); } } while (0)
DWORD WINAPI AmdgpuClientOpen(AMDGPU_WINDOWS_STATE **out, DWORD writable)
{ CHECK(!writable); *out = open_error ? nullptr : (AMDGPU_WINDOWS_STATE *)1; return open_error; }
void WINAPI AmdgpuClientClose(AMDGPU_WINDOWS_STATE *state) { CHECK(state != nullptr); ++closes; }
DWORD WINAPI AmdgpuClientRead(AMDGPU_WINDOWS_STATE *state, const char *path, const char *name,
    DWORD effective, AMDGPU_STATE_VALUE *v, void *data, DWORD bytes)
{
    unsigned i = reads++;
    CHECK(state != nullptr && !path[0] && bytes == 4 && i < 3);
    CHECK(!strcmp(name, i == 0 ? "InteropLastState" : i == 1 ? "EnableGpuPresentBlit" : "EnableCddDwmInterop"));
    CHECK(effective == (i ? 1u : 0u));
    *v = {}; v->status = statuses[i]; v->type = AMDGPU_STATE_DWORD;
    memcpy(data, &values[i], 4);
    // Deliberately use FILE_NOT_FOUND for an incomplete root too: the status,
    // not this Win32 code, controls whether the reader can use settings.
    return statuses[i] == AMDGPU_STATE_OK ? ERROR_SUCCESS : ERROR_FILE_NOT_FOUND;
}
#include "../router-state.h"
static void Reset() { open_error = reads = closes = 0; for (unsigned i=0;i<3;++i) { statuses[i]=AMDGPU_STATE_OK; values[i]=1; } }
int main()
{
    Reset(); values[0]=3; auto r=ReadRouterKmdState(); CHECK(r.blit_on && r.interop_on && reads==1 && closes==1);
    Reset(); values[0]=1; r=ReadRouterKmdState(); CHECK(r.blit_on && !r.interop_on && reads==1);
    Reset(); statuses[0]=AMDGPU_STATE_ABSENT; r=ReadRouterKmdState(); CHECK(r.blit_on && r.interop_on && reads==3);
    Reset(); statuses[0]=AMDGPU_STATE_INCOMPLETE; r=ReadRouterKmdState(); CHECK(!r.blit_on && !r.interop_on && reads==1);
    Reset(); statuses[0]=AMDGPU_STATE_TYPE; r=ReadRouterKmdState(); CHECK(!r.blit_on && !r.interop_on && reads==1);
    Reset(); statuses[0]=AMDGPU_STATE_ABSENT; statuses[1]=AMDGPU_STATE_ACCESS; r=ReadRouterKmdState(); CHECK(!r.blit_on && !r.interop_on);
    Reset(); statuses[0]=AMDGPU_STATE_ABSENT; values[2]=0; r=ReadRouterKmdState(); CHECK(r.blit_on && !r.interop_on);
    Reset(); open_error=ERROR_SERVICE_DOES_NOT_EXIST; r=ReadRouterKmdState(); CHECK(!r.blit_on && !r.interop_on && !reads && !closes);
    printf("router state: %u checks, %u failures\n",checks,failures); return failures?1:0;
}
