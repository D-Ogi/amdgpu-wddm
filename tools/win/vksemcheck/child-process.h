/* Child diagnostics must reach the same destinations as the parent's streams,
 * including when the parent was started without a console. Do not change the
 * parent's handle flags: use temporary inheritable copies instead.
 */
static BOOL StartChildWithStandardHandles(const wchar_t *application, wchar_t *command,
                                         PROCESS_INFORMATION *process)
{
    const DWORD ids[] = { STD_INPUT_HANDLE, STD_OUTPUT_HANDLE, STD_ERROR_HANDLE };
    HANDLE copies[3] = { NULL, NULL, NULL };
    SECURITY_ATTRIBUTES attributes = { sizeof attributes, NULL, TRUE };
    STARTUPINFOW startup = { sizeof startup };
    BOOL started = FALSE;
    DWORD error = ERROR_SUCCESS;
    memset(process, 0, sizeof *process);
    for (unsigned i = 0; i < 3; ++i) {
        HANDLE original = GetStdHandle(ids[i]);
        if (!original || original == INVALID_HANDLE_VALUE) {
            copies[i] = CreateFileW(L"NUL", i == 0 ? GENERIC_READ : GENERIC_WRITE,
                                    FILE_SHARE_READ | FILE_SHARE_WRITE, &attributes,
                                    OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
            if (copies[i] == INVALID_HANDLE_VALUE) {
                copies[i] = NULL;
                error = GetLastError();
                goto done;
            }
        } else if (!DuplicateHandle(GetCurrentProcess(), original, GetCurrentProcess(),
                                    &copies[i], 0, TRUE, DUPLICATE_SAME_ACCESS)) {
            error = GetLastError();
            goto done;
        }
    }
    startup.dwFlags = STARTF_USESTDHANDLES;
    startup.hStdInput = copies[0];
    startup.hStdOutput = copies[1];
    startup.hStdError = copies[2];
    started = CreateProcessW(application, command, NULL, NULL, TRUE, CREATE_NO_WINDOW,
                             NULL, NULL, &startup, process);
    if (!started) error = GetLastError();
done:
    for (unsigned i = 0; i < 3; ++i)
        if (copies[i]) CloseHandle(copies[i]);
    SetLastError(error);
    return started;
}
