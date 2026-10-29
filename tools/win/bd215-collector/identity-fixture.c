#define WIN32_LEAN_AND_MEAN
#include <windows.h>
__declspec(dllexport) int bd215_fixture(void) { return 215; }
BOOL WINAPI DllMain(HINSTANCE instance, DWORD reason, LPVOID reserved)
{
   (void)instance; (void)reason; (void)reserved;
   return TRUE;
}
