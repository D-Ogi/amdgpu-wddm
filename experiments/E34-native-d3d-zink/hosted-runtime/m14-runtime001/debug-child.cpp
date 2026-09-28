#define NOMINMAX
#include <windows.h>
#include <dbghelp.h>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

static void FaultContext(HANDLE process,DWORD threadId) {
    HANDLE thread=OpenThread(THREAD_GET_CONTEXT|THREAD_QUERY_INFORMATION,FALSE,threadId);
    if(!thread){std::printf("CONTEXT_OPEN_ERROR %lu\n",GetLastError());return;}
    CONTEXT context{};context.ContextFlags=CONTEXT_CONTROL|CONTEXT_INTEGER;
    if(GetThreadContext(thread,&context)) {
        std::printf("CONTEXT rip=%016llx rsp=%016llx rbp=%016llx rcx=%016llx rdx=%016llx r8=%016llx r9=%016llx\n",
            context.Rip,context.Rsp,context.Rbp,context.Rcx,context.Rdx,context.R8,context.R9);
        // Raw stack words, not a reconstructed call chain. Never fabricate zeros.
        for(unsigned i=0;i<32;++i){
            const ULONG64 address=context.Rsp+8ULL*i;ULONG64 value=0;SIZE_T read=0;
            if(ReadProcessMemory(process,reinterpret_cast<const void *>(address),&value,sizeof(value),&read) && read==sizeof(value))
                std::printf("STACK %016llx %016llx\n",address,value);
            else {std::printf("STACK_UNREADABLE %016llx error=%lu\n",address,GetLastError());break;}
        }
    }else std::printf("CONTEXT_READ_ERROR %lu\n",GetLastError());
    CloseHandle(thread);
}

static void FaultDump(HANDLE process,DWORD processId,const DEBUG_EVENT &event,const wchar_t *exe,unsigned index) {
    HANDLE thread=OpenThread(THREAD_GET_CONTEXT|THREAD_QUERY_INFORMATION,FALSE,event.dwThreadId);
    CONTEXT context{};context.ContextFlags=CONTEXT_ALL;
    if(!thread || !GetThreadContext(thread,&context)) {
        std::printf("DUMP_CONTEXT_ERROR %lu\n",GetLastError());if(thread)CloseHandle(thread);return;
    }
    CloseHandle(thread);
    EXCEPTION_RECORD record=event.u.Exception.ExceptionRecord;
    // Copy only this exception record. Any nested-record pointer belongs to
    // the child; do not accidentally dereference it in the debugger process.
    record.ExceptionRecord=nullptr;
    EXCEPTION_POINTERS pointers{&record,&context};
    MINIDUMP_EXCEPTION_INFORMATION info{event.dwThreadId,&pointers,FALSE};
    std::wstring path=exe;path+=L".fault-"+std::to_wstring(processId)+L"-"+std::to_wstring(index)+L".dmp";
    HANDLE file=CreateFileW(path.c_str(),GENERIC_WRITE,0,nullptr,CREATE_NEW,FILE_ATTRIBUTE_NORMAL,nullptr);
    if(file==INVALID_HANDLE_VALUE){std::printf("DUMP_CREATE_ERROR %lu\n",GetLastError());return;}
    // Normal minidump only: stack/context/module information, not full memory.
    BOOL written=MiniDumpWriteDump(process,processId,file,MiniDumpNormal,&info,nullptr,nullptr);
    DWORD error=written?0:GetLastError();BOOL flushed=FlushFileBuffers(file);CloseHandle(file);
    std::printf("DUMP index=%u written=%u flushed=%u error=%lu path=%ls\n",index,unsigned(written),unsigned(flushed),error,path.c_str());
}

static void Module(HANDLE file,const void *base) {
    wchar_t path[32768]{};
    DWORD count=file?GetFinalPathNameByHandleW(file,path,DWORD(_countof(path)),FILE_NAME_NORMALIZED):0;
    if(count && count<_countof(path))std::printf("MODULE base=%p path=%ls\n",base,path);
    else std::printf("MODULE base=%p path_unavailable=1\n",base);
}

// Short-lived user-mode debugger. No kernel debugger, resident listener or GUI.
static std::wstring Quote(const wchar_t *arg) {
    std::wstring result=L"\"";size_t slashes=0;
    for(;*arg;++arg){
        if(*arg==L'\\'){++slashes;continue;}
        result.append(slashes*( *arg==L'"'?2:1),L'\\');slashes=0;
        if(*arg==L'"')result+=L'\\';result+=*arg;
    }
    result.append(slashes*2,L'\\');result+=L'"';return result;
}
int wmain(int argc,wchar_t **argv) {
    std::setvbuf(stdout,nullptr,_IONBF,0);
    if(argc==3 && !wcscmp(argv[1],L"--fixture")){
        OutputDebugStringA("M14 DEBUG STRING CONTROL\n");
        OutputDebugStringW(L"M14 WIDE STRING CONTROL\n");
        SYSTEM_INFO system{};GetSystemInfo(&system);
        auto pages=static_cast<unsigned char *>(VirtualAlloc(nullptr,2*system.dwPageSize,MEM_RESERVE|MEM_COMMIT,PAGE_READWRITE));
        if(!pages)return 8;DWORD previous=0;
        if(!VirtualProtect(pages+system.dwPageSize,system.dwPageSize,PAGE_NOACCESS,&previous))return 8;
        const wchar_t edge[]=L"M14 PAGE EDGE CONTROL";
        auto edgeString=reinterpret_cast<wchar_t *>(pages+system.dwPageSize-sizeof(edge));
        memcpy(edgeString,edge,sizeof(edge));OutputDebugStringW(edgeString);VirtualFree(pages,0,MEM_RELEASE);

        if(!wcscmp(argv[2],L"tree")){
            wchar_t exe[MAX_PATH]{};if(!GetModuleFileNameW(nullptr,exe,MAX_PATH))return 8;
            std::wstring child=Quote(exe)+L" --fixture sleep";
            STARTUPINFOW si{};si.cb=sizeof(si);PROCESS_INFORMATION pi{};
            if(!CreateProcessW(exe,child.data(),nullptr,nullptr,FALSE,CREATE_NO_WINDOW,nullptr,nullptr,&si,&pi))return 8;
            std::printf("DESCENDANT %lu\n",pi.dwProcessId);CloseHandle(pi.hThread);CloseHandle(pi.hProcess);
        }
        if(!wcscmp(argv[2],L"sleep")){Sleep(10000);return 0;}
        if(!wcscmp(argv[2],L"crash")){RaiseException(EXCEPTION_ACCESS_VIOLATION,EXCEPTION_NONCONTINUABLE,0,nullptr);return 9;}
        return !wcscmp(argv[2],L"fail")?7:0;
    }
    if(argc<3){std::puts("usage: debug-child seconds executable [arguments...]");return 2;}
    wchar_t *end=nullptr;unsigned long seconds=wcstoul(argv[1],&end,10);
    if(!end || *end || seconds<1 || seconds>120)return 2;
    HANDLE job=CreateJobObjectW(nullptr,nullptr);
    JOBOBJECT_EXTENDED_LIMIT_INFORMATION limits{};limits.BasicLimitInformation.LimitFlags=JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
    if(!job || !SetInformationJobObject(job,JobObjectExtendedLimitInformation,&limits,sizeof(limits))){if(job)CloseHandle(job);return 125;}
    std::wstring command=Quote(argv[2]);for(int i=3;i<argc;++i){command+=L' ';command+=Quote(argv[i]);}
    STARTUPINFOW startup{};startup.cb=sizeof(startup);PROCESS_INFORMATION process{};
    if(!CreateProcessW(argv[2],command.data(),nullptr,nullptr,FALSE,DEBUG_ONLY_THIS_PROCESS|CREATE_SUSPENDED|CREATE_NO_WINDOW,
        nullptr,nullptr,&startup,&process)){std::printf("CREATE_ERROR %lu\n",GetLastError());CloseHandle(job);return 125;}
    if(!AssignProcessToJobObject(job,process.hProcess) || ResumeThread(process.hThread)==DWORD(-1)){
        TerminateProcess(process.hProcess,125);CloseHandle(process.hThread);CloseHandle(process.hProcess);CloseHandle(job);return 125;
    }
    wchar_t capture[2]{};
    const bool dumps=GetEnvironmentVariableW(L"BC250_M14_CAPTURE_DUMP",capture,2)==1 && capture[0]==L'1';
    unsigned dumpCount=0;
    const ULONGLONG start=GetTickCount64();bool exited=false,initialBreakpoint=true;DWORD code=125;
    while(GetTickCount64()-start<seconds*1000ULL){
        DEBUG_EVENT event{};
        if(!WaitForDebugEventEx(&event,100)){
            if(GetLastError()==ERROR_SEM_TIMEOUT)continue;
            std::printf("DEBUG_WAIT_ERROR %lu\n",GetLastError());break;
        }
        DWORD status=DBG_CONTINUE;
        switch(event.dwDebugEventCode){
        case CREATE_PROCESS_DEBUG_EVENT:
            Module(event.u.CreateProcessInfo.hFile,event.u.CreateProcessInfo.lpBaseOfImage);
            if(event.u.CreateProcessInfo.hFile)CloseHandle(event.u.CreateProcessInfo.hFile);
            break;
        case CREATE_THREAD_DEBUG_EVENT:break;
        case LOAD_DLL_DEBUG_EVENT:
            Module(event.u.LoadDll.hFile,event.u.LoadDll.lpBaseOfDll);
            if(event.u.LoadDll.hFile)CloseHandle(event.u.LoadDll.hFile);break;
        case UNLOAD_DLL_DEBUG_EVENT:std::printf("MODULE_UNLOAD base=%p\n",event.u.UnloadDll.lpBaseOfDll);break;
        case OUTPUT_DEBUG_STRING_EVENT:{
            const auto &info=event.u.DebugString;
            // OUTPUT_DEBUG_STRING_INFO length is bytes, including Unicode.
            size_t units=info.nDebugStringLength;if(units>4096)units=4096;
            std::vector<wchar_t> buffer(units+1,0);SIZE_T read=0;
            if(ReadProcessMemory(process.hProcess,info.lpDebugStringData,buffer.data(),units,&read)){
                if(info.fUnicode)std::printf("DEBUG %ls\n",buffer.data());
                else std::printf("DEBUG %s\n",reinterpret_cast<const char *>(buffer.data()));
            }else std::printf("DEBUG_READ_ERROR %lu\n",GetLastError());
            break;
        }
        case EXCEPTION_DEBUG_EVENT:{
            const DWORD exception=event.u.Exception.ExceptionRecord.ExceptionCode;
            std::printf("EXCEPTION %08lx first=%lu address=%p\n",exception,event.u.Exception.dwFirstChance,
                event.u.Exception.ExceptionRecord.ExceptionAddress);
            if(initialBreakpoint && exception==EXCEPTION_BREAKPOINT && event.u.Exception.dwFirstChance)initialBreakpoint=false;
            else status=DBG_EXCEPTION_NOT_HANDLED;
            if(!event.u.Exception.dwFirstChance)FaultContext(process.hProcess,event.dwThreadId);
            if(dumps && dumpCount<3 && (!event.u.Exception.dwFirstChance || exception==0xe06d7363))
                FaultDump(process.hProcess,process.dwProcessId,event,argv[2],++dumpCount);
            break;
        }
        case EXIT_PROCESS_DEBUG_EVENT:exited=true;code=event.u.ExitProcess.dwExitCode;break;
        default:break;
        }
        if(!ContinueDebugEvent(event.dwProcessId,event.dwThreadId,status)){std::printf("DEBUG_CONTINUE_ERROR %lu\n",GetLastError());exited=false;break;}
        if(exited)break;
    }
    // Job closure also terminates descendants not covered by DEBUG_ONLY_THIS_PROCESS.
    if(!exited){code=124;std::puts("TIMEOUT_OR_DEBUG_FAILURE");TerminateJobObject(job,code);}
    if(exited)TerminateJobObject(job,125);
    bool closed=false;const ULONGLONG cleanup=GetTickCount64();
    while(GetTickCount64()-cleanup<5000){
        JOBOBJECT_BASIC_ACCOUNTING_INFORMATION accounting{};
        if(!QueryInformationJobObject(job,JobObjectBasicAccountingInformation,&accounting,sizeof(accounting),nullptr))break;
        if(!accounting.ActiveProcesses){closed=true;break;}
        DEBUG_EVENT event{};
        if(WaitForDebugEventEx(&event,50)){
            if(event.dwDebugEventCode==LOAD_DLL_DEBUG_EVENT && event.u.LoadDll.hFile)CloseHandle(event.u.LoadDll.hFile);
            if(event.dwDebugEventCode==CREATE_PROCESS_DEBUG_EVENT){
                if(event.u.CreateProcessInfo.hFile)CloseHandle(event.u.CreateProcessInfo.hFile);
                    }
            if(!ContinueDebugEvent(event.dwProcessId,event.dwThreadId,DBG_CONTINUE))break;
        }else if(GetLastError()!=ERROR_SEM_TIMEOUT)break;
    }
    CloseHandle(job);CloseHandle(process.hThread);CloseHandle(process.hProcess);
    std::printf("CHILD_EXIT %lu observed=%u tree_closed=%u\n",code,unsigned(exited),unsigned(closed));
    return closed?int(code):125;
}
