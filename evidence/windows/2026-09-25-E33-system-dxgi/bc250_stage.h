#pragma once
#include <windows.h>
inline void bc250Stage(const char* s) { DWORD n; WriteFile(GetStdHandle(STD_ERROR_HANDLE),s,(DWORD)lstrlenA(s),&n,nullptr); }
