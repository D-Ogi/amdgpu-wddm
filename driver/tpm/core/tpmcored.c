/* SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0 */
/*
 * tpmcored.exe: the user-mode half of the BC-250 TPM servicer, a LocalSystem service (Bc250TpmCore).
 *
 * It opens the control device of tpmserv.sys, takes each command that tpm.sys put in the CRB page, runs it in the
 * TPM core (tpmcore.c) and gives the answer back. The TPM state is in %ProgramData%\BC250\tpm, readable only by
 * SYSTEM and Administrators.
 *
 * tpm.sys starts before any service, so its first commands find no core and the device can fail its start. After
 * the core is up and connected, the service restarts an ACPI\MSFT0101 device node that has a problem.
 *
 *   tpmcored.exe            run as the service (the SCM starts it)
 *   tpmcored.exe --console  run in this console until Ctrl+C (lab diagnosis)
 */
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <winioctl.h>
#include <cfgmgr32.h>
#include <sddl.h>
#include <stdio.h>
#include <stdarg.h>
#include <string.h>
#include <wchar.h>
#include "tpmcore.h"
#include "../tpmserv_ioctl.h"

#define SERVICE_NAME	L"Bc250TpmCore"
#define TPM_RC_FAILURE	0x101u

static SERVICE_STATUS_HANDLE g_status_handle;
static SERVICE_STATUS g_status;
static HANDLE g_stop;
static int g_console;
static FILE *g_log;
static wchar_t g_dir[MAX_PATH];

static void say(const char *fmt, ...)
{
	char line[512];
	SYSTEMTIME t;
	va_list ap;

	va_start(ap, fmt);
	vsnprintf(line, sizeof(line), fmt, ap);
	va_end(ap);
	GetSystemTime(&t);
	if (g_console) printf("%02u:%02u:%02u.%03uZ %s\n", t.wHour, t.wMinute, t.wSecond, t.wMilliseconds, line);
	if (g_log != NULL) {
		fprintf(g_log, "%04u-%02u-%02uT%02u:%02u:%02u.%03uZ %s\n", t.wYear, t.wMonth, t.wDay, t.wHour, t.wMinute,
			t.wSecond, t.wMilliseconds, line);
		fflush(g_log);
	}
}

/* %ProgramData%\BC250\tpm with a protected DACL: full access for SYSTEM and Administrators, nobody else. */
static int make_state_dir(void)
{
	static const wchar_t sddl[] = L"D:P(A;OICI;FA;;;SY)(A;OICI;FA;;;BA)";
	wchar_t base[MAX_PATH], parent[MAX_PATH];
	SECURITY_ATTRIBUTES sa = {sizeof(sa), NULL, FALSE};
	DWORD n = GetEnvironmentVariableW(L"ProgramData", base, MAX_PATH);
	int ok;

	if (n == 0 || n >= MAX_PATH - 16) return 0;
	if (!ConvertStringSecurityDescriptorToSecurityDescriptorW(sddl, SDDL_REVISION_1, &sa.lpSecurityDescriptor, NULL))
		return 0;
	_snwprintf(parent, MAX_PATH, L"%ls\\BC250", base);
	_snwprintf(g_dir, MAX_PATH, L"%ls\\BC250\\tpm", base);
	CreateDirectoryW(parent, NULL);
	ok = CreateDirectoryW(g_dir, &sa) || GetLastError() == ERROR_ALREADY_EXISTS;
	LocalFree(sa.lpSecurityDescriptor);
	return ok;
}

/* Restarts every ACPI\MSFT0101 device node that has a problem. Returns the number of restarts asked for. */
static int restart_failed_tpm(void)
{
	static const wchar_t prefix[] = L"ACPI\\MSFT0101\\";
	ULONG chars = 0;
	wchar_t *list, *id;
	int restarted = 0;

	if (CM_Get_Device_ID_List_SizeW(&chars, L"ACPI", CM_GETIDLIST_FILTER_ENUMERATOR | CM_GETIDLIST_FILTER_PRESENT) !=
	    CR_SUCCESS || chars == 0)
		return 0;
	list = (wchar_t *)calloc(chars, sizeof(wchar_t));
	if (list == NULL) return 0;
	if (CM_Get_Device_ID_ListW(L"ACPI", list, chars, CM_GETIDLIST_FILTER_ENUMERATOR | CM_GETIDLIST_FILTER_PRESENT) ==
	    CR_SUCCESS) {
		for (id = list; *id; id += wcslen(id) + 1) {
			DEVINST dn;
			ULONG status = 0, problem = 0;
			if (_wcsnicmp(id, prefix, wcslen(prefix)) != 0) continue;
			if (CM_Locate_DevNodeW(&dn, id, CM_LOCATE_DEVNODE_NORMAL) != CR_SUCCESS) continue;
			if (CM_Get_DevNode_Status(&status, &problem, dn, 0) != CR_SUCCESS) continue;
			say("tpm device %ls status=0x%lx problem=%lu", id, status, problem);
			if (!(status & DN_HAS_PROBLEM)) continue;
			say("tpm device restart cr=0x%lx", (unsigned long)CM_Setup_DevNode(dn, CM_SETUP_DEVNODE_READY));
			restarted++;
		}
	}
	free(list);
	return restarted;
}

static DWORD WINAPI restart_thread(LPVOID unused)
{
	(void)unused;
	say("tpm restarts asked: %d", restart_failed_tpm());
	return 0;
}

/* One overlapped DeviceIoControl that the stop event can interrupt. Returns 1 with *bytes set, 0 on failure or
 * stop. */
static int ioctl_wait(HANDLE dev, DWORD code, const void *in, DWORD in_bytes, void *out, DWORD out_bytes, DWORD *bytes)
{
	OVERLAPPED ov;
	HANDLE waits[2];
	DWORD w;

	memset(&ov, 0, sizeof(ov));
	ov.hEvent = CreateEventW(NULL, TRUE, FALSE, NULL);
	if (ov.hEvent == NULL) return 0;
	*bytes = 0;
	if (!DeviceIoControl(dev, code, (void *)in, in_bytes, out, out_bytes, NULL, &ov) &&
	    GetLastError() != ERROR_IO_PENDING) {
		CloseHandle(ov.hEvent);
		return 0;
	}
	waits[0] = ov.hEvent;
	waits[1] = g_stop;
	w = WaitForMultipleObjects(2, waits, FALSE, INFINITE);
	if (w != WAIT_OBJECT_0) CancelIoEx(dev, &ov);
	w = GetOverlappedResult(dev, &ov, bytes, TRUE) && w == WAIT_OBJECT_0;
	CloseHandle(ov.hEvent);
	return (int)w;
}

static void serve(HANDLE dev)
{
	static unsigned char command[TPMSERV_MAX_COMMAND], response[TPMSERV_MAX_COMMAND];
	DWORD got, unused;
	size_t n;

	while (WaitForSingleObject(g_stop, 0) != WAIT_OBJECT_0) {
		if (!ioctl_wait(dev, IOCTL_TPMSERV_GET_COMMAND, NULL, 0, command, sizeof(command), &got)) {
			say("get_command failed (%lu)", GetLastError());
			return;
		}
		if (got < 10) continue;
		n = tpmcore_process(command, got, response, sizeof(response));
		if (n == 0) {
			/* The core gave no answer: the requester still gets one, TPM_RC_FAILURE with the command's tag. */
			static const unsigned char fail[10] = {0x80, 0x01, 0x00, 0x00, 0x00, 0x0A, 0x00, 0x00, 0x01, 0x01};
			memcpy(response, fail, sizeof(fail));
			n = sizeof(fail);
		}
		say("cmd 0x%08x %lu -> rc 0x%08x %u", ((unsigned)command[6] << 24) | ((unsigned)command[7] << 16) |
		    ((unsigned)command[8] << 8) | command[9], got, ((unsigned)response[6] << 24) |
		    ((unsigned)response[7] << 16) | ((unsigned)response[8] << 8) | response[9], (unsigned)n);
		if (!ioctl_wait(dev, IOCTL_TPMSERV_PUT_RESPONSE, response, (DWORD)n, NULL, 0, &unused)) {
			say("put_response failed (%lu)", GetLastError());
			return;
		}
	}
}

static void run(void)
{
	unsigned r;

	if (!make_state_dir()) { say("state directory failed (%lu)", GetLastError()); return; }
	{
		wchar_t path[MAX_PATH];
		_snwprintf(path, MAX_PATH, L"%ls\\core.log", g_dir);
		g_log = _wfopen(path, L"a");
	}
	r = tpmcore_start(g_dir, TPMSERV_MAX_COMMAND);
	say("core start rc=0x%x", r);
	if (r != 0) return;
	while (WaitForSingleObject(g_stop, 0) != WAIT_OBJECT_0) {
		HANDLE dev = CreateFileW(TPMSERV_USER_PATH, GENERIC_READ | GENERIC_WRITE, 0, NULL, OPEN_EXISTING,
					 FILE_FLAG_OVERLAPPED, NULL);
		if (dev == INVALID_HANDLE_VALUE) {
			/* The filter is not loaded (no BC-250, no stage tables) or another core holds it. */
			if (WaitForSingleObject(g_stop, 2000) == WAIT_OBJECT_0) break;
			continue;
		}
		say("connected to the filter");
		/* The restart waits for the start of tpm.sys, and tpm.sys waits for answers from serve(): it must run on
		 * its own thread, or the two wait for each other. */
		{
			HANDLE t = CreateThread(NULL, 0, restart_thread, NULL, 0, NULL);
			if (t != NULL) CloseHandle(t);
			else say("restart thread failed (%lu)", GetLastError());
		}
		serve(dev);
		CloseHandle(dev);
	}
	tpmcore_stop();
	say("core stopped");
}

static DWORD WINAPI control(DWORD code, DWORD type, LPVOID data, LPVOID context)
{
	(void)type; (void)data; (void)context;
	if (code == SERVICE_CONTROL_STOP || code == SERVICE_CONTROL_PRESHUTDOWN) {
		g_status.dwCurrentState = SERVICE_STOP_PENDING;
		g_status.dwWaitHint = 5000;
		SetServiceStatus(g_status_handle, &g_status);
		SetEvent(g_stop);
		return NO_ERROR;
	}
	return code == SERVICE_CONTROL_INTERROGATE ? NO_ERROR : ERROR_CALL_NOT_IMPLEMENTED;
}

static void WINAPI service_main(DWORD argc, LPWSTR *argv)
{
	(void)argc; (void)argv;
	g_status_handle = RegisterServiceCtrlHandlerExW(SERVICE_NAME, control, NULL);
	if (g_status_handle == NULL) return;
	g_status.dwServiceType = SERVICE_WIN32_OWN_PROCESS;
	g_status.dwCurrentState = SERVICE_RUNNING;
	/* PRESHUTDOWN, not SHUTDOWN: the core stays up as long as the system lets it, for the last commands. */
	g_status.dwControlsAccepted = SERVICE_ACCEPT_STOP | SERVICE_ACCEPT_PRESHUTDOWN;
	SetServiceStatus(g_status_handle, &g_status);
	run();
	g_status.dwCurrentState = SERVICE_STOPPED;
	SetServiceStatus(g_status_handle, &g_status);
}

static BOOL WINAPI console_break(DWORD type)
{
	(void)type;
	SetEvent(g_stop);
	return TRUE;
}

int wmain(int argc, wchar_t **argv)
{
	static SERVICE_TABLE_ENTRYW table[] = {{(LPWSTR)SERVICE_NAME, service_main}, {NULL, NULL}};

	g_stop = CreateEventW(NULL, TRUE, FALSE, NULL);
	if (g_stop == NULL) return 1;
	if (argc > 1 && wcscmp(argv[1], L"--console") == 0) {
		g_console = 1;
		SetConsoleCtrlHandler(console_break, TRUE);
		run();
		return 0;
	}
	return StartServiceCtrlDispatcherW(table) ? 0 : 1;
}
