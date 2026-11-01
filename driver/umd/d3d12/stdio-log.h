// SPDX-License-Identifier: MIT
// Where the D3D12 UMD's diagnostic lines go: the shell, engine-ddi and the trace in ddi-trace.h all print here.
// A UMD runs inside the application, whose stdio is the application's. 3DMark's adapter_info.exe pipes stderr,
// never reads it, and blocked in WriteFile once ~12 KB of these lines had filled the pipe; its parent gave up
// after 15 s and Time Spy found no rendering device (session 283). So nothing is printed unless the process asks
// through AMDGPU_WDDM_LOG, read once per process (the vkd3d-proton engine reads this same sink through
// engine-ddi, and so does the DXVK shell):
//   unset or empty: the timed switch below decides. Windows keeps no empty variable of its own, but a process
//     started with an environment block that holds one reads it as unset, and so does this sink.
//   "0" or anything else: no output.
//   "stderr": the process's stderr, the behaviour before this switch.
//   "file:<path>": appended to <path>, opened for append only and shared, so several modules and processes may
//     write to one file; a path that cannot be opened means no output.
// That list is the lab flavour's (AMDGPU_WDDM_LAB_FLAVOUR, BD-167). The release flavour reads AMDGPU_WDDM_LOG only
// as the carrier of the timed switch below: beside a marker that names this process, and only as "file:" and a
// plain file name in the switch's folder, a file that already exists. Without such a marker the switch decides, as
// with the variable unset; "stderr", every other value and every other path give no output of their own.
//
// The timed switch (the control app's "Record game logs"): a tester cannot set an environment variable for a game
// that Steam or Epic starts, so the control app turns logging on for a few minutes instead. Its elevated helper
// writes HKLM\SOFTWARE\amdgpu-wddm\Diagnostics GameLogUntil, a REG_QWORD holding the UTC FILETIME at which
// logging stops, and prepares %ProgramData%\amdgpu-wddm\logs\games with an access list that lets any user create
// files there. With no marker and no AMDGPU_WDDM_LOG, and that instant still ahead, this sink opens a new file of
// its own in that folder and marks the process with two variables:
//   AMDGPU_WDDM_LOG=file:<path>, so that a module loaded later (DXVK, vkd3d-proton, RADV's ICD) writes its lines
//     into the same file, and
//   AMDGPU_WDDM_GAMELOG=<pid>;<the instant as 16 hex digits>, so that it takes the bounds with it.
// Every reader of the switch answers the marker the same way, this header and RADV's u_amdgpu_wddm_stdio.c alike:
//   the marker names this process: append to that file, bounded by the instant the marker carries and by the size
//     cap. The first module to open the file sets the marker and no other module opens a second file.
//   the marker names another process: both variables were inherited from the parent that started this process,
//     and they say nothing about it. Both are ignored, the switch is read again here, and the recording that
//     follows is a file of this process, with its own marker.
//   no marker and AMDGPU_WDDM_LOG set: in the lab flavour the process asked for the lines itself, and nothing
//     bounds them; in the release flavour the switch decides, as with the variable unset.
// A marker that cannot be read, or one longer than the buffer, counts as absent.
// A module whose first reading of the switch found no recording looks again at most once a second: it joins the file
// of a marker of this process that another module set later, or, without such a marker, opens a file of its own when
// the switch is on now (BD-283, detail::late_join).
//
// Three bounds hold a switch somebody forgets: every print checks the instant (one read of the shared user-data
// page), the file stops at kFileCapBytes with one last line saying so, and the file is created once per process,
// so a game that restarts in a loop leaves many small files and no unbounded one. The cap is measured on the file
// itself, so the lines of every module writing into it count, and whichever of them prints next stops.
//
// AMDGPU_WDDM_DDI_TRACE and the experiment switches choose what is logged; this switch only chooses where.
// Debugger output (OutputDebugString) is not stdio and stays as it is.
#pragma once
#pragma comment(lib,"advapi32.lib") // RegGetValueA: the timed switch below. Every module that logs reads it.
#include <windows.h>
#include <fcntl.h>
#include <io.h>
#include <atomic>
#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <cstring>

namespace amdgpu_wddm_log {

// The control app's timed switch, in one place: the app writes these, this sink reads them.
namespace game_log {
inline constexpr const char* kKeyPath="SOFTWARE\\amdgpu-wddm\\Diagnostics";
inline constexpr const char* kValueName="GameLogUntil";
// Below %ProgramData%. The control app creates it and owns its access list.
inline constexpr const char* kFolder="amdgpu-wddm\\logs\\games";
// One file of one process. 64 MiB is far past any session a person records on purpose and still small enough to
// attach to a report.
inline constexpr unsigned long long kFileCapBytes=64ull<<20;
// Where the lines go, and, beside it, which process a timed recording belongs to: "<pid>;<16 hex digits of the
// UTC FILETIME at which it stops>". Without the marker a module could not tell the switch's file, which has
// bounds, from a file the user asked for, which has none.
inline constexpr const char* kVariable="AMDGPU_WDDM_LOG";
inline constexpr const char* kMarkerVariable="AMDGPU_WDDM_GAMELOG";
} // namespace game_log

namespace detail {

// What one sink is: the file (null = nothing is logged) and the bounds that belong to it. An explicit
// AMDGPU_WDDM_LOG has no bounds, because the process asked for the lines itself.
struct Sink {
    FILE* file;
    unsigned long long until;       // the UTC FILETIME after which nothing is written; 0: no time bound
    unsigned long long cap;         // bytes this file may hold; 0: no size bound
    // A sink without a file that the timed switch decided (BD-283): why it found no recording, the UTC FILETIME of
    // that decision and the Windows error of a file that could not be made. Null: no later look may change the
    // decision (a file, an explicit AMDGPU_WDDM_LOG, or a marker of this process that was already the answer).
    const char* why=nullptr;
    unsigned long long decided=0;
    unsigned long error=0;
};

// What a print must do. Pure, so the host test drives every case without a clock.
enum class Step { Write, Last, Stop };

inline Step step(const Sink& s,unsigned long long now,unsigned long long written,bool closed) noexcept {
    if(!s.file)return Step::Stop;
    if(closed)return Step::Stop;
    if(s.until && now>=s.until)return Step::Last;
    if(s.cap && written>=s.cap)return Step::Last;
    return Step::Write;
}

inline const char* stop_reason(const Sink& s,unsigned long long now,unsigned long long written) noexcept {
    if(s.until && now>=s.until)return "the recording time is over";
    if(s.cap && written>=s.cap)return "this file is full";
    return "the recording stopped";
}

inline unsigned long long now_filetime() noexcept {
    FILETIME t;
    GetSystemTimeAsFileTime(&t);
    return (static_cast<unsigned long long>(t.dwHighDateTime)<<32)|t.dwLowDateTime;
}

// How much the file holds, with the lines of every other module and process that writes into it: the switch's
// file is shared, so a counter of this module's own bytes would not bound it. 0 when the handle cannot be asked,
// and then the writer's own count is all that bounds the file.
inline unsigned long long file_size(FILE* f) noexcept {
    if(!f)return 0;
    const intptr_t raw=_get_osfhandle(_fileno(f));
    if(raw==-1||raw==-2)return 0;
    LARGE_INTEGER size{};
    if(!GetFileSizeEx(reinterpret_cast<HANDLE>(raw),&size))return 0;
    return static_cast<unsigned long long>(size.QuadPart);
}

// The marker, "<pid>;<16 hex digits>", read and written in one place. Pure, so the host test drives the inherited
// and the joined case without starting a process. A value of any other shape is refused and counts as no marker.
inline bool parse_marker(const char* value,unsigned long* pid,unsigned long long* until) noexcept {
    if(!value||!*value)return false;
    const char* p=value;
    unsigned long long id=0;
    size_t digits=0;
    for(;*p>='0'&&*p<='9';++p,++digits){
        if(digits>9)return false;                       // no process id needs an eleventh digit
        id=id*10+static_cast<unsigned long long>(*p-'0');
    }
    if(!digits||*p!=';')return false;
    ++p;
    unsigned long long instant=0;
    size_t hex=0;
    for(;*p;++p,++hex){
        unsigned nibble;
        const char c=*p;
        if(c>='0'&&c<='9')nibble=static_cast<unsigned>(c-'0');
        else if(c>='a'&&c<='f')nibble=static_cast<unsigned>(c-'a'+10);
        else if(c>='A'&&c<='F')nibble=static_cast<unsigned>(c-'A'+10);
        else return false;
        if(hex>=16)return false;
        instant=(instant<<4)|nibble;
    }
    if(hex!=16)return false;
    *pid=static_cast<unsigned long>(id);
    *until=instant;
    return true;
}

inline bool marker_value(char* out,size_t size,unsigned long pid,unsigned long long until) noexcept {
    return _snprintf_s(out,size,_TRUNCATE,"%lu;%016llX",pid,until)>0;
}

// Whether path names a file directly in folder: the folder, one backslash and a plain file name, with no directory
// part, no stream and neither "." nor "..". The folder is compared without regard to case, as Windows compares it.
// Pure, so the host test drives it. The release flavour joins no other file (BD-167).
inline bool game_log_member(const char* path,const char* folder) noexcept {
    if(!path||!folder||!*folder)return false;
    const size_t length=std::strlen(folder);
    if(_strnicmp(path,folder,length)||path[length]!='\\')return false;
    const char* const name=path+length+1;
    if(!*name||!std::strcmp(name,".")||!std::strcmp(name,".."))return false;
    for(const char* p=name;*p;++p)if(*p=='\\'||*p=='/'||*p==':')return false;
    return true;
}

inline FILE* from_handle(HANDLE file) noexcept {
    const int fd=_open_osfhandle(reinterpret_cast<intptr_t>(file),_O_APPEND|_O_WRONLY);
    if(fd<0){CloseHandle(file);return nullptr;}
    FILE* const out=_fdopen(fd,"a");
    if(!out){_close(fd);return nullptr;}
    std::setvbuf(out,nullptr,_IONBF,0); // nothing of ours waits in a buffer: write_text below writes the line
    return out;
}

// One line, one write. A formatted print to an unbuffered stream reaches the file one character at a time, which
// a file of several writers holds as torn lines: 7975 of 8000 in a two-thread probe, and that was before the
// marker made the modules of one process share a file on purpose. So the line is built first and written whole,
// with one WriteFile on the handle the stream carries, which an append handle puts at the end of the file. The
// stream is the fallback, for a handle that cannot be fetched.
inline int write_text(FILE* file,const char* text,int length) noexcept {
    if(length<=0)return 0;
    const intptr_t raw=_get_osfhandle(_fileno(file));
    if(raw!=-1&&raw!=-2){
        DWORD written=0;
        if(WriteFile(reinterpret_cast<HANDLE>(raw),text,static_cast<DWORD>(length),&written,nullptr))
            return static_cast<int>(written);
    }
    return static_cast<int>(std::fwrite(text,1,static_cast<size_t>(length),file));
}

// One formatted line, written whole. A line longer than the buffer is cut and says so, because a second write
// for its tail would tear against another writer of the same file. No line this driver prints is that long.
inline constexpr const char kCutMark[]="...[line cut]\n";

inline int emit(FILE* file,_Printf_format_string_ const char* format,va_list args) noexcept {
    char line[2048];
    va_list copy;
    va_copy(copy,args);
    const int count=_vsnprintf_s(line,sizeof(line),_TRUNCATE,format,copy);
    va_end(copy);
    if(count>=0)return write_text(file,line,count);
    std::memcpy(line+sizeof(line)-sizeof(kCutMark),kCutMark,sizeof(kCutMark));
    return write_text(file,line,static_cast<int>(sizeof(line)-1));
}

inline FILE* open_append(const char* path) noexcept {
    const HANDLE file=CreateFileA(path,FILE_APPEND_DATA,FILE_SHARE_READ|FILE_SHARE_WRITE|FILE_SHARE_DELETE,nullptr,
        OPEN_ALWAYS,FILE_ATTRIBUTE_NORMAL,nullptr);
    if(file==INVALID_HANDLE_VALUE)return nullptr;
    return from_handle(file);
}

// One new file of the timed switch. CREATE_NEW, so a name that exists is never written to and never truncated;
// FILE_FLAG_OPEN_REPARSE_POINT with a check of what was really opened, so a name planted in the folder cannot
// send the lines anywhere else.
inline FILE* open_new(const char* path) noexcept {
    const HANDLE file=CreateFileA(path,FILE_APPEND_DATA,FILE_SHARE_READ|FILE_SHARE_WRITE|FILE_SHARE_DELETE,nullptr,
        CREATE_NEW,FILE_ATTRIBUTE_NORMAL|FILE_FLAG_OPEN_REPARSE_POINT,nullptr);
    if(file==INVALID_HANDLE_VALUE)return nullptr;
    BY_HANDLE_FILE_INFORMATION info{};
    if(!GetFileInformationByHandle(file,&info)||(info.dwFileAttributes&FILE_ATTRIBUTE_REPARSE_POINT)!=0){
        CloseHandle(file);
        return nullptr;
    }
    return from_handle(file);
}

#ifndef AMDGPU_WDDM_LAB_FLAVOUR
// The switch's file that another module of this process made, as the release flavour joins it: OPEN_EXISTING, so a
// join never creates a name, and the check of open_new, so a planted name cannot send the lines anywhere else. A
// file the switch made is a plain file on a disk with one name, so a device name, a reparse point or a second link
// is refused.
inline FILE* open_joined(const char* path) noexcept {
    const HANDLE file=CreateFileA(path,FILE_APPEND_DATA|FILE_READ_ATTRIBUTES,
        FILE_SHARE_READ|FILE_SHARE_WRITE|FILE_SHARE_DELETE,nullptr,OPEN_EXISTING,
        FILE_ATTRIBUTE_NORMAL|FILE_FLAG_OPEN_REPARSE_POINT,nullptr);
    if(file==INVALID_HANDLE_VALUE)return nullptr;
    BY_HANDLE_FILE_INFORMATION info{};
    if(GetFileType(file)!=FILE_TYPE_DISK||!GetFileInformationByHandle(file,&info)||
        (info.dwFileAttributes&(FILE_ATTRIBUTE_REPARSE_POINT|FILE_ATTRIBUTE_DIRECTORY))!=0||info.nNumberOfLinks!=1){
        CloseHandle(file);
        return nullptr;
    }
    return from_handle(file);
}
#endif

// GameLogUntil, read only, from the 64-bit view of the registry (RRF_SUBKEY_WOW6464KEY, as the per-application
// settings reader takes its values), so a 32-bit game reads the value the control app wrote. RRF_RT_QWORD refuses
// a value of another type by itself. 0: no switch, another type, or a key that cannot be read.
inline unsigned long long game_log_until() noexcept {
    unsigned long long until=0;
    DWORD bytes=sizeof(until);
    const LSTATUS st=RegGetValueA(HKEY_LOCAL_MACHINE,game_log::kKeyPath,game_log::kValueName,
        RRF_RT_QWORD|RRF_SUBKEY_WOW6464KEY,nullptr,&until,&bytes);
    return st==ERROR_SUCCESS && bytes==sizeof(until) ? until : 0;
}

// %ProgramData%\amdgpu-wddm\logs\games. Windows sets ProgramData for every process; a process without it has no
// folder this sink may write to, and the switch stays off there.
inline bool game_log_folder(char* out,size_t size) noexcept {
    char root[MAX_PATH]{};
    const DWORD n=GetEnvironmentVariableA("ProgramData",root,sizeof(root));
    if(!n||n>=sizeof(root))return false;
    return _snprintf_s(out,size,_TRUNCATE,"%s\\%s",root,game_log::kFolder)>0;
}

// The base name of a process image for a file name: no directory, no extension, at most 64 characters, and only
// letters, digits, '.', '-' and '_' (anything else becomes '_'). An empty or unreadable path gives "process".
inline void safe_image_name(char* out,size_t size,const char* path) noexcept {
    const char* base=path?path:"";
    for(const char* p=base;*p;++p)if(*p=='\\'||*p=='/'||*p==':')base=p+1;
    size_t length=std::strlen(base);
    const char* dot=nullptr;
    for(const char* p=base;*p;++p)if(*p=='.')dot=p;
    if(dot&&dot!=base)length=static_cast<size_t>(dot-base);
    if(length>64)length=64;
    if(size<2)return;
    if(length==0){_snprintf_s(out,size,_TRUNCATE,"%s","process");return;}
    size_t written=0;
    for(size_t i=0;i<length&&written+1<size;++i){
        const unsigned char c=static_cast<unsigned char>(base[i]);
        const bool plain=(c>='0'&&c<='9')||(c>='a'&&c<='z')||(c>='A'&&c<='Z')||c=='.'||c=='-'||c=='_';
        out[written++]=plain?static_cast<char>(c):'_';
    }
    out[written]='\0';
}

// "<image>-<pid>-<yyyymmddThhmmssZ>.log", and with attempt above 0 a "-<attempt>" before the extension: two
// processes of the same image and the same second would otherwise ask for one name.
inline bool log_file_name(char* out,size_t size,const char* image,unsigned long pid,const SYSTEMTIME& utc,
    unsigned attempt) noexcept {
    char suffix[16]{};
    if(attempt)_snprintf_s(suffix,sizeof(suffix),_TRUNCATE,"-%u",attempt);
    return _snprintf_s(out,size,_TRUNCATE,"%s-%lu-%04u%02u%02uT%02u%02u%02uZ%s.log",image,pid,utc.wYear,utc.wMonth,
        utc.wDay,utc.wHour,utc.wMinute,utc.wSecond,suffix)>0;
}

// The file of the module this header was compiled into, for the first line of the log: a report then says which
// of the shells opened the file.
inline void this_module_path(char* out,size_t size) noexcept {
    if(!size)return;
    out[0]='\0';
    HMODULE module=nullptr;
    if(!GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS|GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
        reinterpret_cast<LPCSTR>(&now_filetime),&module))
        return;
    const DWORD n=GetModuleFileNameA(module,out,static_cast<DWORD>(size));
    if(!n||n>=size)out[0]='\0';
}

// The timed switch: the instant, the folder, one new file, the first line, and AMDGPU_WDDM_LOG for the engines
// this process loads later.
// A sink without a file, with the reason, for a later look (late_join below).
inline Sink no_recording(const char* why,unsigned long error=0) noexcept {
    Sink s{nullptr,0,0};
    s.why=why;
    s.decided=now_filetime();
    s.error=error;
    return s;
}

inline Sink open_game_log() noexcept {
    const unsigned long long until=game_log_until();
    if(!until)return no_recording("the switch was off");
    if(now_filetime()>=until)return no_recording("the recording time was over");
    char folder[MAX_PATH]{};
    if(!game_log_folder(folder,sizeof(folder)))return no_recording("no ProgramData folder");
    char image[MAX_PATH]{};
    const DWORD n=GetModuleFileNameA(nullptr,image,sizeof(image));
    char name[96]{};
    safe_image_name(name,sizeof(name),(n&&n<sizeof(image))?image:"");
    SYSTEMTIME utc{};
    GetSystemTime(&utc);
    const unsigned long pid=GetCurrentProcessId();
    char path[MAX_PATH]{};
    FILE* out=nullptr;
    unsigned long error=0;
    for(unsigned attempt=0;attempt<8&&!out;++attempt){
        char file[128]{};
        if(!log_file_name(file,sizeof(file),name,pid,utc,attempt))return no_recording("no file name");
        if(_snprintf_s(path,sizeof(path),_TRUNCATE,"%s\\%s",folder,file)<=0)return no_recording("no file name");
        out=open_new(path);
        if(!out)error=GetLastError();
    }
    if(!out)return no_recording("no file could be made",error);
    // The other modules of this process read the switch themselves; point them at the file this sink just made,
    // and mark it with this process and this instant, so that they take the same bounds and a process the game
    // starts later does not take the file at all.
    char value[MAX_PATH+8]{};
    if(_snprintf_s(value,sizeof(value),_TRUNCATE,"file:%s",path)>0)SetEnvironmentVariableA(game_log::kVariable,value);
    char mark[48]{};
    if(marker_value(mark,sizeof(mark),pid,until))SetEnvironmentVariableA(game_log::kMarkerVariable,mark);
    char module[MAX_PATH]{};
    this_module_path(module,sizeof(module));
    char first[1024]{};
    const int length=_snprintf_s(first,sizeof(first),_TRUNCATE,
        "amdgpu-wddm game log: %s pid %lu, module %s, opened %04u-%02u-%02uT%02u:%02u:%02uZ, "
        "recording until the time the control app set, at most %llu MiB\n",(n&&n<sizeof(image))?image:"(unknown)",pid,
        module[0]?module:"(unknown)",utc.wYear,utc.wMonth,utc.wDay,utc.wHour,utc.wMinute,utc.wSecond,
        game_log::kFileCapBytes>>20);
    write_text(out,first,length);
    std::fflush(out);
    return {out,until,game_log::kFileCapBytes};
}

// Another module of this process opened the switch's file: append to it and keep its bounds. The marker carries
// the instant, so this module needs no second reading of the registry, and the cap is measured on the file, which
// counts what that other module has written as well.
// The release flavour joins only a file that already exists directly in the switch's folder (BD-167).
inline Sink join_game_log(const char* value,unsigned long long until) noexcept {
    if(std::strncmp(value,"file:",5)||!value[5])return {nullptr,0,0};
#ifdef AMDGPU_WDDM_LAB_FLAVOUR
    FILE* const out=open_append(value+5);
#else
    char folder[MAX_PATH]{};
    if(!game_log_folder(folder,sizeof(folder))||!game_log_member(value+5,folder))return {nullptr,0,0};
    FILE* const out=open_joined(value+5);
#endif
    if(!out)return {nullptr,0,0};
    return {out,until,game_log::kFileCapBytes};
}

// The sink of this process: the marker first, then (lab flavour) an AMDGPU_WDDM_LOG the process asked for itself,
// else the control app's timed switch.
inline Sink open_sink() noexcept {
    char value[MAX_PATH+8]{};
    const DWORD n=GetEnvironmentVariableA(game_log::kVariable,value,sizeof(value));
#ifdef AMDGPU_WDDM_LAB_FLAVOUR
    if(n>=sizeof(value))return {nullptr,0,0};   // a value too long for the buffer is still a value: off
#endif
    char mark[48]{};
    const DWORD m=GetEnvironmentVariableA(game_log::kMarkerVariable,mark,sizeof(mark));
    unsigned long marked_pid=0;
    unsigned long long marked_until=0;
    if(m&&m<sizeof(mark)&&parse_marker(mark,&marked_pid,&marked_until)){
        // This process marked it: join that file under its bounds, and nothing else, because the marker is the
        // file. Another process marked it: the variables were inherited and neither applies here.
        if(marked_pid==GetCurrentProcessId())
            return n&&n<sizeof(value)?join_game_log(value,marked_until):Sink{nullptr,0,0};
        return open_game_log();
    }
#ifdef AMDGPU_WDDM_LAB_FLAVOUR
    if(n){
        if(!std::strcmp(value,"stderr"))return {stderr,0,0};
        if(std::strncmp(value,"file:",5)||!value[5])return {nullptr,0,0};
        return {open_append(value+5),0,0};
    }
#endif
    // The release flavour: without a marker, AMDGPU_WDDM_LOG is not read, as if it were unset (BD-167).
    return open_game_log();
}

inline Sink& sink() noexcept {
    static Sink s=open_sink();
    return s;
}

inline std::atomic<unsigned long long>& written() noexcept {
    static std::atomic<unsigned long long> bytes{0};
    return bytes;
}

inline std::atomic<int>& closed() noexcept {
    static std::atomic<int> flag{0};
    return flag;
}

// BD-283: the late join. The sink is decided at the first print of this module, once per process. A process that
// reaches this module before the switch is on (a shell host that Windows starts ahead of time and keeps for hours)
// then keeps no file for its whole life, while a module that decides later, RADV's ICD at the first device, opens
// the file and records alone; the lines of this module, the DDI statuses among them, are lost. So a sink that the
// switch decided without a file looks again, at most once a second: when another module has marked this process
// since then, this module joins that file, as open_sink does; without such a marker and with the switch on now, it
// opens a file of its own (late_join below). Its first line there says why the first decision found no recording.
inline constexpr unsigned long long kLateJoinPeriod=10000000ull;   // one second in FILETIME units
inline constexpr unsigned long long kNoMoreLooks=~0ull;

struct LateJoin {
    std::atomic<FILE*> file{nullptr};           // published after joined is written, and then never changed
    Sink joined{nullptr,0,0};
    std::atomic<unsigned long long> next{0};    // the FILETIME of the next look; kNoMoreLooks: none
};

inline LateJoin& late() noexcept {
    static LateJoin j;
    return j;
}

inline void filetime_text(char* out,size_t size,unsigned long long t) noexcept {
    FILETIME f{static_cast<DWORD>(t),static_cast<DWORD>(t>>32)};
    SYSTEMTIME u{};
    if(!FileTimeToSystemTime(&f,&u)){_snprintf_s(out,size,_TRUNCATE,"%s","(no time)");return;}
    _snprintf_s(out,size,_TRUNCATE,"%04u-%02u-%02uT%02u:%02u:%02u.%03uZ",u.wYear,u.wMonth,u.wDay,u.wHour,u.wMinute,
        u.wSecond,u.wMilliseconds);
}

// The late file of this module: its first line names the first decision, then the sink is published.
inline const Sink* publish_late(const Sink& decided,LateJoin& j,const Sink& late,const char* how) noexcept {
    char module[MAX_PATH]{};
    this_module_path(module,sizeof(module));
    char when[40]{};
    filetime_text(when,sizeof(when),decided.decided);
    char first[640]{};
    const int length=_snprintf_s(first,sizeof(first),_TRUNCATE,
        "amdgpu-wddm game log: module %s %s; its first print, at %s, found no recording: %s (error %lu)\n",
        module[0]?module:"(unknown)",how,when,decided.why,decided.error);
    write_text(late.file,first,length);
    j.joined=late;
    j.file.store(late.file,std::memory_order_release);
    return &j.joined;
}

// The sink to write to now: decided's own file, else the file of a late join, else null. Pure apart from the
// environment, the switch and the file itself, so the host test drives it with a LateJoin of its own and any clock.
// A look finds one of three things:
//   a marker of this process: join that file, as open_sink does, and look no more, whatever the join gives.
//   no marker of this process, and the switch on now: open a file of this process, as open_sink would now, and
//     mark it for the other modules (BD-283 r7: in 20261101T225230Z the shell of a host started at 22:14 found the
//     switch off, the switch came on at 22:52:30, and the host's first Win+N at 22:52:32 removed a device 49 ms
//     later; RADV's ICD opened the file only at its next device, so the shell's site line had no file to go to).
//     A file that cannot be made ends the looks; a switch that is still off or over leaves them on.
//   no marker of this process, the switch off: nothing, and the next look in a second.
// Two modules of one process that look at the same instant could each make a file; the looks are a second apart
// and each module looks only until its first file, so the worst case is two files of one process.
inline const Sink* late_join(const Sink& decided,LateJoin& j,unsigned long long now) noexcept {
    if(decided.file)return &decided;
    if(j.file.load(std::memory_order_acquire))return &j.joined;
    if(!decided.why)return nullptr;
    unsigned long long next=j.next.load(std::memory_order_relaxed);
    if(now<next)return nullptr;
    // One thread looks; the others go on without a sink until the next second.
    if(!j.next.compare_exchange_strong(next,now+kLateJoinPeriod,std::memory_order_relaxed))return nullptr;
    char mark[48]{};
    const DWORD m=GetEnvironmentVariableA(game_log::kMarkerVariable,mark,sizeof(mark));
    unsigned long pid=0;
    unsigned long long until=0;
    if(!m||m>=sizeof(mark)||!parse_marker(mark,&pid,&until)||pid!=GetCurrentProcessId()){
        const Sink own=open_game_log();
        if(!own.file){
            if(own.why&&!std::strcmp(own.why,"no file could be made"))j.next.store(kNoMoreLooks,std::memory_order_relaxed);
            return nullptr;
        }
        j.next.store(kNoMoreLooks,std::memory_order_relaxed);
        return publish_late(decided,j,own,"opens this file late");
    }
    j.next.store(kNoMoreLooks,std::memory_order_relaxed);
    char value[MAX_PATH+8]{};
    const DWORD n=GetEnvironmentVariableA(game_log::kVariable,value,sizeof(value));
    if(!n||n>=sizeof(value))return nullptr;
    const Sink joined=join_game_log(value,until);
    if(!joined.file)return nullptr;
    return publish_late(decided,j,joined,"joins this file late");
}

inline const Sink* active() noexcept {
    const Sink& s=sink();
    if(s.file)return &s;
    if(!s.why)return nullptr;
    return late_join(s,late(),now_filetime());
}
} // namespace detail

// The sink, or null when nothing is logged. A bounded sink may already have stopped; print decides that.
inline FILE* stream() noexcept {
    const detail::Sink* s=detail::active();
    return s?s->file:nullptr;
}
inline bool enabled() noexcept {return stream()!=nullptr;}

inline void print(_Printf_format_string_ const char* format,...) noexcept {
    const detail::Sink* const active=detail::active();
    if(!active)return;
    const detail::Sink& s=*active;
    if(!s.until&&!s.cap){                       // the process asked for these lines: no bound, no clock read
        va_list args;
        va_start(args,format);
        detail::emit(s.file,format,args);
        va_end(args);
        return;
    }
    const unsigned long long now=detail::now_filetime();
    // The file holds what every writer put there; this module's own count is the floor, for a handle that cannot
    // be asked its size.
    const unsigned long long own=detail::written().load(std::memory_order_relaxed);
    const unsigned long long shared=detail::file_size(s.file);
    const unsigned long long bytes=shared>own?shared:own;
    switch(detail::step(s,now,bytes,detail::closed().load(std::memory_order_relaxed)!=0)){
    case detail::Step::Stop:
        return;
    case detail::Step::Last:
        if(detail::closed().exchange(1,std::memory_order_relaxed))return;   // one last line, by one thread
        {
            char last[256]{};
            const int length=_snprintf_s(last,sizeof(last),_TRUNCATE,
                "amdgpu-wddm game log: nothing more is written, %s.\n",detail::stop_reason(s,now,bytes));
            detail::write_text(s.file,last,length);
        }
        std::fflush(s.file);
        return;
    case detail::Step::Write:
        break;
    }
    va_list args;
    va_start(args,format);
    const int count=detail::emit(s.file,format,args);
    va_end(args);
    if(count>0)detail::written().fetch_add(static_cast<unsigned long long>(count),std::memory_order_relaxed);
}

inline void flush() noexcept {
    if(FILE* const out=stream())std::fflush(out);
}
} // namespace amdgpu_wddm_log
