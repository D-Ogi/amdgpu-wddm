// Launch only for the already logged-on active console user, from LocalSystem.
// No credentials, token duplication into the child, desktop ACL edits or inherited handles.
#include <wtsapi32.h>
#include <userenv.h>
static bool enablePrivilege(HANDLE token,const wchar_t *name) {
    TOKEN_PRIVILEGES privilege{};privilege.PrivilegeCount=1;
    if(!LookupPrivilegeValueW(nullptr,name,&privilege.Privileges[0].Luid))return false;
    privilege.Privileges[0].Attributes=SE_PRIVILEGE_ENABLED;
    SetLastError(ERROR_SUCCESS);
    return AdjustTokenPrivileges(token,FALSE,&privilege,0,nullptr,nullptr) && GetLastError()==ERROR_SUCCESS;
}
static BOOL createActiveConsoleChild(const wchar_t *exe,wchar_t *command,PROCESS_INFORMATION *pi,DWORD &session,const char *&stage) {
    stage="open_caller_token";
    Handle caller;
    if(!OpenProcessToken(GetCurrentProcess(),TOKEN_QUERY|TOKEN_ADJUST_PRIVILEGES,&caller.h))return FALSE;
    stage="query_caller_token";
    DWORD needed=0;GetTokenInformation(caller.h,TokenUser,nullptr,0,&needed);
    std::vector<unsigned char> data(needed);
    if(!needed || !GetTokenInformation(caller.h,TokenUser,data.data(),needed,&needed))return FALSE;
    stage="require_system";
    if(!IsWellKnownSid(reinterpret_cast<TOKEN_USER *>(data.data())->User.Sid,WinLocalSystemSid)){
        SetLastError(ERROR_ACCESS_DENIED);return FALSE;
    }
    stage="enable_tcb";
    if(!enablePrivilege(caller.h,L"SeTcbPrivilege"))return FALSE;
    stage="active_console";
    session=WTSGetActiveConsoleSessionId();
    if(session==0 || session==0xffffffff){SetLastError(ERROR_NO_SUCH_LOGON_SESSION);return FALSE;}
    stage="console_state";
    LPWSTR state=nullptr;DWORD stateBytes=0;
    if(!WTSQuerySessionInformationW(WTS_CURRENT_SERVER_HANDLE,session,WTSConnectState,&state,&stateBytes))return FALSE;
    const bool active=stateBytes==sizeof(WTS_CONNECTSTATE_CLASS) &&
        *reinterpret_cast<WTS_CONNECTSTATE_CLASS *>(state)==WTSActive;
    WTSFreeMemory(state);
    if(!active){SetLastError(ERROR_NO_SUCH_LOGON_SESSION);return FALSE;}
    stage="query_user_token";
    Handle user;
    if(!WTSQueryUserToken(session,&user.h))return FALSE;
    stage="user_token_session";
    DWORD tokenSession=0,bytes=0;
    if(!GetTokenInformation(user.h,TokenSessionId,&tokenSession,sizeof(tokenSession),&bytes))return FALSE;
    if(tokenSession!=session){SetLastError(ERROR_INVALID_DATA);return FALSE;}
    stage="user_environment";
    LPVOID environment=nullptr;
    if(!CreateEnvironmentBlock(&environment,user.h,FALSE))return FALSE;
    STARTUPINFOW startup{};startup.cb=sizeof(startup);
    wchar_t desktop[]=L"winsta0\\default";startup.lpDesktop=desktop;
    startup.dwFlags=STARTF_USESHOWWINDOW;startup.wShowWindow=SW_HIDE;
    // Windows prohibits inheriting handles across sessions. Child-side scripts
    // write their own logs; the parent still owns the process and Job handles.
    stage="create_process_as_user";
    const BOOL created=CreateProcessAsUserW(user.h,exe,command,nullptr,nullptr,FALSE,
        CREATE_SUSPENDED|CREATE_NO_WINDOW|CREATE_UNICODE_ENVIRONMENT,
        environment,nullptr,&startup,pi);
    const DWORD error=GetLastError();DestroyEnvironmentBlock(environment);SetLastError(error);
    return created;
}
