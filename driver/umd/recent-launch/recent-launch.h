// SPDX-License-Identifier: MIT
// The recent-launch record of the D3D12 shell and the D3D11 (DXVK) shell: after the first successful outer
// CreateDevice of a process, one worker thread notes the application in the user's own list, which the control
// application shows as "recently launched". Format, switch and lock: docs/design/recent-launches.md (version 1).
//
// The record is written only by the shells' adapter CreateDevice, after it succeeded: never by the hosted Vulkan
// ICD below them, never in DllMain, on a submit or Present path, or on the thread that creates the device. The
// calling thread pays for one atomic exchange, a reference on the module and starting one thread; everything else
// (switch, paths, lock, file) runs on the worker. Every failure only loses this entry: the device never waits for
// it or sees it.
#pragma once
#include <windows.h>
#include <algorithm>
#include <atomic>
#include <charconv>
#include <cstdint>
#include <cstdio>
#include <cwchar>
#include <string>
#include <string_view>
#include <vector>
#pragma comment(lib,"advapi32.lib") // RegGetValueW, GetTokenInformation

namespace amdgpu_wddm::recent_launch {
// Bits of an entry's Apis field. 4 is reserved for a Vulkan writer in the ICD, which does not exist.
enum : uint32_t {ApiD3D12=1,ApiD3D11=2};

enum class Outcome : unsigned {
    Recorded,          // a new launch of the path: Starts counted, Apis = this API
    Merged,            // the same process again (the other shell): its API added, Starts unchanged
    SwitchOff,         // the user turned the list off (switch value 0)
    SwitchUnreadable,  // the switch exists but could not be read or has an unknown value: nothing written
    Excluded,          // under the Windows directory, one of our own tools, a service account, or a path that is not
                       // valid UTF-16
    AppContainer,      // an AppContainer process: nothing written
    NoStore,           // no usable %LOCALAPPDATA% or module path
    Busy,              // another writer or a Clear held the lock longer than the wait
    Failed,            // an I/O error, a list of a newer format version, or no memory
};
inline const char* outcome_name(Outcome o) noexcept {
    switch(o){
    case Outcome::Recorded:return "recorded";
    case Outcome::Merged:return "merged";
    case Outcome::SwitchOff:return "switch-off";
    case Outcome::SwitchUnreadable:return "switch-unreadable";
    case Outcome::Excluded:return "excluded";
    case Outcome::AppContainer:return "appcontainer";
    case Outcome::NoStore:return "no-store";
    case Outcome::Busy:return "busy";
    case Outcome::Failed:return "failed";
    }
    return "?";
}

inline constexpr wchar_t kSwitchKey[]=L"Software\\amdgpu-wddm\\Control"; // under HKEY_CURRENT_USER
inline constexpr wchar_t kSwitchValue[]=L"RecordRecentLaunches";        // REG_DWORD: absent or 1 = on, 0 = off
inline constexpr wchar_t kStoreDirName[]=L"amdgpu-wddm";                // under %LOCALAPPDATA%
inline constexpr wchar_t kStoreName[]=L"recent-launches.txt";
inline constexpr wchar_t kLockName[]=L"recent-launches.lock";
inline constexpr wchar_t kTempName[]=L"recent-launches.tmp";
inline constexpr std::string_view kHeaderPrefix="amdgpu-wddm recent-launches ";
inline constexpr std::string_view kHeader="amdgpu-wddm recent-launches 1\n";
inline constexpr size_t kMaxEntries=64;
// The worker's wait for the lock. No device or game thread waits for it; it bounds how long a worker may queue
// behind other writers (a few milliseconds each) or a Clear before it drops its entry as Busy.
inline constexpr DWORD kLockWaitMs=1000;
inline constexpr DWORD kMaxStoreBytes=8u<<20;

struct Entry {
    uint64_t start{};   // creation time of the process of the last launch, FILETIME UTC
    uint32_t pid{};     // its process id; (start, pid) identifies the process
    uint32_t starts{};  // launches counted, saturating
    uint32_t apis{};    // Api bits of the last launch
    std::wstring path;  // normalized full path of the executable
};

// What a commit reads from the process, filled by gather(); host tests fill it themselves.
struct Inputs {
    std::wstring exe;          // normalized full path of the process's executable
    std::wstring store_dir;    // %LOCALAPPDATA%\amdgpu-wddm
    std::wstring windows_dir;  // normalized system Windows directory, nothing is recorded below it
    uint64_t start{};
    uint32_t pid{};
    HKEY switch_root{HKEY_CURRENT_USER};
    const wchar_t* switch_key{kSwitchKey};
    DWORD lock_wait_ms{kLockWaitMs};
    // Host tests only, null in the driver: called after the first switch read, with the lock held before the
    // second switch read, and with the temporary file open before it is written (to throw an allocation failure).
    void (*after_precheck)(void*){};
    void (*inside_lock)(void*){};
    void (*after_temp_open)(void*){};
    void* hook_context{};
};

namespace detail {
inline bool to_utf8(std::wstring_view w,std::string& out) {
    out.clear();
    if(w.empty())return true;
    const int n=WideCharToMultiByte(CP_UTF8,WC_ERR_INVALID_CHARS,w.data(),int(w.size()),nullptr,0,nullptr,nullptr);
    if(n<=0)return false;
    out.resize(size_t(n));
    return WideCharToMultiByte(CP_UTF8,WC_ERR_INVALID_CHARS,w.data(),int(w.size()),out.data(),n,nullptr,nullptr)==n;
}
inline bool to_wide(std::string_view s,std::wstring& out) {
    out.clear();
    if(s.empty())return true;
    const int n=MultiByteToWideChar(CP_UTF8,MB_ERR_INVALID_CHARS,s.data(),int(s.size()),nullptr,0);
    if(n<=0)return false;
    out.resize(size_t(n));
    return MultiByteToWideChar(CP_UTF8,MB_ERR_INVALID_CHARS,s.data(),int(s.size()),out.data(),n)==n;
}
// Digits only: no sign, no space, no empty field; a hexadecimal field has exactly 16 digits.
template<typename T> bool number(std::string_view s,int base,T& value) {
    if(s.empty())return false;
    const auto r=std::from_chars(s.data(),s.data()+s.size(),value,base);
    return r.ec==std::errc{} && r.ptr==s.data()+s.size() && (base!=16 || s.size()==16);
}
inline bool control_free(std::wstring_view path) noexcept {
    return std::none_of(path.begin(),path.end(),[](wchar_t c){return c<0x20;});
}
inline bool has_prefix(std::wstring_view s,std::wstring_view prefix) noexcept {
    return s.size()>=prefix.size() && s.compare(0,prefix.size(),prefix)==0;
}
// "\\?\C:\x" -> "C:\x", "\\?\UNC\srv\share\x" -> "\\srv\share\x".
inline std::wstring strip_prefix(std::wstring path) {
    if(has_prefix(path,L"\\\\?\\UNC\\"))return L"\\"+path.substr(7);
    if(has_prefix(path,L"\\\\?\\"))return path.substr(4);
    return path;
}
// A path the file functions accept past MAX_PATH.
inline std::wstring extended(const std::wstring& path) {
    if(path.size()<MAX_PATH-12 || has_prefix(path,L"\\\\?\\"))return path;
    if(has_prefix(path,L"\\\\"))return L"\\\\?\\UNC\\"+path.substr(2);
    return L"\\\\?\\"+path;
}
// Owns one handle (a file or an event; INVALID_HANDLE_VALUE counts as none) and closes it on every way out,
// an exception included: a handle left open on the worker would stay open for the life of the game.
class Handle {
public:
    explicit Handle(HANDLE h=nullptr) noexcept:h_(h==INVALID_HANDLE_VALUE?nullptr:h){}
    Handle(const Handle&)=delete;
    Handle& operator=(const Handle&)=delete;
    ~Handle(){reset();}
    explicit operator bool() const noexcept {return h_!=nullptr;}
    HANDLE get() const noexcept {return h_;}
    void reset() noexcept {if(h_){CloseHandle(h_);h_=nullptr;}}
    HANDLE release() noexcept {const HANDLE h=h_;h_=nullptr;return h;}
private:
    HANDLE h_;
};
template<typename F> std::wstring grow(F&& fill) {
    std::wstring buffer(MAX_PATH,L'\0');
    for(;;){
        const DWORD n=fill(buffer.data(),DWORD(buffer.size()));
        if(!n)return {};
        if(n<buffer.size()){buffer.resize(n);return buffer;}
        if(buffer.size()>=32768)return {};
        buffer.resize(std::max<size_t>(n+1,buffer.size()*2));
    }
}
} // namespace detail

// Ordinal comparison without case, as NTFS compares names.
inline bool same_path(std::wstring_view a,std::wstring_view b) noexcept {
    return CompareStringOrdinal(a.data(),int(a.size()),b.data(),int(b.size()),TRUE)==CSTR_EQUAL;
}
// True when path lies strictly inside dir: "C:\Windows\x" is inside "C:\Windows", "C:\Windows2\x" and
// "C:\Windows.old\x" are not.
inline bool under_directory(std::wstring_view path,std::wstring_view dir) noexcept {
    while(!dir.empty() && dir.back()==L'\\')dir.remove_suffix(1);
    return !dir.empty() && path.size()>dir.size()+1 && path[dir.size()]==L'\\' && same_path(path.substr(0,dir.size()),dir);
}
// This project's own programs (capability dumps, test clients, the KMD CLI, vulkaninfo of the bug report) create
// devices for diagnosis; they are not the user's applications. Matched on the file name, without case.
inline bool own_tool(std::wstring_view path) noexcept {
    const size_t slash=path.find_last_of(L"\\/");
    const std::wstring_view name=slash==std::wstring_view::npos?path:path.substr(slash+1);
    auto starts=[&](std::wstring_view prefix){return name.size()>prefix.size() && same_path(name.substr(0,prefix.size()),prefix);};
    const bool exe=name.size()>4 && same_path(name.substr(name.size()-4),L".exe");
    return exe && (starts(L"amdgpu_wddm_") || starts(L"bc250") || starts(L"vulkaninfo"));
}
// The final path of an existing file or directory: symbolic links, junctions, short names and letter case resolved
// as the file system stores them, without the \\?\ prefix. A path that cannot be opened stays as GetFullPathNameW
// writes it.
inline std::wstring normalize(const std::wstring& path) {
    if(path.empty())return {};
    const std::wstring ext=detail::extended(path);
    detail::Handle file(CreateFileW(ext.c_str(),FILE_READ_ATTRIBUTES,FILE_SHARE_READ|FILE_SHARE_WRITE|FILE_SHARE_DELETE,
        nullptr,OPEN_EXISTING,FILE_FLAG_BACKUP_SEMANTICS,nullptr));
    if(file){
        std::wstring final=detail::grow([&](wchar_t* p,DWORD n){
            return GetFinalPathNameByHandleW(file.get(),p,n,FILE_NAME_NORMALIZED|VOLUME_NAME_DOS);});
        file.reset();
        if(!final.empty())return detail::strip_prefix(std::move(final));
    }
    return detail::strip_prefix(detail::grow([&](wchar_t* p,DWORD n){return GetFullPathNameW(ext.c_str(),n,p,nullptr);}));
}

enum class Switch {On,Off,Unreadable};
// A missing key or value is the owner-approved default, on. Anything else that is not a REG_DWORD 0 or 1 skips.
inline Switch read_switch(HKEY root,const wchar_t* key) noexcept {
    DWORD type=0,value=0,size=sizeof(value);
    const LSTATUS s=RegGetValueW(root,key,kSwitchValue,RRF_RT_ANY|RRF_NOEXPAND,&type,&value,&size);
    if(s==ERROR_FILE_NOT_FOUND || s==ERROR_PATH_NOT_FOUND)return Switch::On;
    if(s!=ERROR_SUCCESS || type!=REG_DWORD || size!=sizeof(value))return Switch::Unreadable;
    return value==0?Switch::Off:value==1?Switch::On:Switch::Unreadable;
}

// Strict: the header of version 1, then lines "<start, 16 hex>\t<pid>\t<starts>\t<apis>\t<path>", then
// "end <count>" as the last line. A torn, truncated or edited list is no list at all.
inline bool parse(std::string_view text,std::vector<Entry>& out) {
    out.clear();
    if(text.substr(0,kHeader.size())!=kHeader)return false;
    size_t pos=kHeader.size();
    for(;;){
        const size_t eol=text.find('\n',pos);
        if(eol==std::string_view::npos)return false;
        const std::string_view line=text.substr(pos,eol-pos);
        pos=eol+1;
        if(line.substr(0,4)=="end "){
            size_t count=0;
            return detail::number(line.substr(4),10,count) && count==out.size() && pos==text.size();
        }
        std::string_view field[5];
        size_t from=0;
        for(int i=0;i<4;++i){
            const size_t tab=line.find('\t',from);
            if(tab==std::string_view::npos)return false;
            field[i]=line.substr(from,tab-from);from=tab+1;
        }
        field[4]=line.substr(from);
        Entry e;
        if(!detail::number(field[0],16,e.start) || !detail::number(field[1],10,e.pid) ||
           !detail::number(field[2],10,e.starts) || !detail::number(field[3],10,e.apis) ||
           !detail::to_wide(field[4],e.path) || e.path.empty() || !detail::control_free(e.path))return false;
        out.push_back(std::move(e));
        if(out.size()>kMaxEntries)return false;
    }
}
inline bool serialize(const std::vector<Entry>& entries,std::string& out) {
    out.assign(kHeader);
    std::string path;char head[96];
    for(const Entry& e:entries){
        if(!detail::to_utf8(e.path,path))return false;
        std::snprintf(head,sizeof(head),"%016llX\t%lu\t%lu\t%lu\t",static_cast<unsigned long long>(e.start),
            static_cast<unsigned long>(e.pid),static_cast<unsigned long>(e.starts),static_cast<unsigned long>(e.apis));
        out+=head;out+=path;out+='\n';
    }
    std::snprintf(head,sizeof(head),"end %zu\n",entries.size());
    out+=head;
    return true;
}
// The list after one launch note, most recently launched first, at most kMaxEntries: a new launch moves its path to
// the front and counts a start, the same process again (the other shell) only adds its API. The order is the order
// of the notes, not of the clock, so a clock set back cannot prune the launch just noted.
inline Outcome apply(std::vector<Entry>& entries,const std::wstring& exe,uint64_t start,uint32_t pid,uint32_t api) {
    auto it=std::find_if(entries.begin(),entries.end(),[&](const Entry& e){return same_path(e.path,exe);});
    if(it!=entries.end() && it->start==start && it->pid==pid){it->apis|=api;return Outcome::Merged;}
    if(it==entries.end()){
        entries.insert(entries.begin(),Entry{start,pid,1,api,exe});
    } else {
        it->start=start;it->pid=pid;it->apis=api;
        if(it->starts!=UINT32_MAX)++it->starts;
        std::rotate(entries.begin(),it,it+1);
    }
    if(entries.size()>kMaxEntries)entries.resize(kMaxEntries);
    return Outcome::Recorded;
}

// An exclusive byte-range lock on byte 0 of the lock file, waited for at most wait_ms. Released by the destructor,
// or by the system when the process ends first. The lock file is never deleted: a writer that opened a deleted
// lock file would lock a different file than the next one.
class StoreLock {
public:
    StoreLock()=default;
    StoreLock(const StoreLock&)=delete;
    StoreLock& operator=(const StoreLock&)=delete;
    ~StoreLock(){
        if(held_){OVERLAPPED ov{};UnlockFileEx(file_,0,1,0,&ov);}
        if(file_!=INVALID_HANDLE_VALUE)CloseHandle(file_);
    }
    // False with Busy when the wait ran out, Failed on any other error.
    bool acquire(const std::wstring& dir,DWORD wait_ms,Outcome& failure) noexcept {
        failure=Outcome::Failed;
        try {
            file_=CreateFileW(detail::extended(dir+L'\\'+kLockName).c_str(),GENERIC_READ|GENERIC_WRITE,
                FILE_SHARE_READ|FILE_SHARE_WRITE|FILE_SHARE_DELETE,nullptr,OPEN_ALWAYS,
                FILE_ATTRIBUTE_NORMAL|FILE_FLAG_OVERLAPPED,nullptr);
        } catch(...) {return false;}
        if(file_==INVALID_HANDLE_VALUE)return false;
        OVERLAPPED ov{};
        ov.hEvent=CreateEventW(nullptr,TRUE,FALSE,nullptr);
        if(!ov.hEvent)return false;
        if(LockFileEx(file_,LOCKFILE_EXCLUSIVE_LOCK,0,1,0,&ov)){
            held_=true;
        } else if(GetLastError()==ERROR_IO_PENDING){
            if(WaitForSingleObject(ov.hEvent,wait_ms)!=WAIT_OBJECT_0)CancelIoEx(file_,&ov);
            DWORD n=0;
            held_=GetOverlappedResult(file_,&ov,&n,TRUE)!=FALSE; // a grant that raced the cancel still counts
            if(!held_ && GetLastError()==ERROR_OPERATION_ABORTED)failure=Outcome::Busy;
        }
        CloseHandle(ov.hEvent);
        return held_;
    }
private:
    HANDLE file_{INVALID_HANDLE_VALUE};
    bool held_{};
};

// The whole list, or missing = true when there is none. A file larger than kMaxStoreBytes is no list of this
// version (64 entries of the longest paths stay below it); only its first bytes are read, with oversize = true, so
// that the commit can still tell a later format version, which it keeps, from a damaged list. False: the file
// exists but could not be read.
inline bool read_store(const std::wstring& dir,std::string& text,bool& missing,bool* oversize=nullptr) {
    text.clear();missing=false;
    if(oversize)*oversize=false;
    const std::wstring path=detail::extended(dir+L'\\'+kStoreName);
    detail::Handle file(CreateFileW(path.c_str(),GENERIC_READ,FILE_SHARE_READ|FILE_SHARE_WRITE|FILE_SHARE_DELETE,
        nullptr,OPEN_EXISTING,FILE_FLAG_SEQUENTIAL_SCAN,nullptr));
    if(!file){
        const DWORD e=GetLastError();
        missing=e==ERROR_FILE_NOT_FOUND || e==ERROR_PATH_NOT_FOUND;
        return missing;
    }
    LARGE_INTEGER size{};
    if(!GetFileSizeEx(file.get(),&size))return false;
    const bool large=size.QuadPart>kMaxStoreBytes;
    if(oversize)*oversize=large;
    text.resize(large?64:size_t(size.QuadPart));
    DWORD done=0;
    return text.empty() || (ReadFile(file.get(),text.data(),DWORD(text.size()),&done,nullptr) && done==text.size());
}
// Writes the temporary file and renames it over the list in one step, so a reader sees the old or the new list,
// never a part. POSIX semantics replace a list that a reader holds open (with FILE_SHARE_DELETE); MoveFileExW is
// the fallback where the file system does not offer them.
// The temporary file is opened without write sharing, so its handle is owned at once: one left open by an exception
// would make every later writer, in any process, fail on it until this process ends.
inline bool write_store(const std::wstring& dir,const std::string& text,void (*after_open)(void*)=nullptr,
    void* hook_context=nullptr) {
    const std::wstring temp=detail::extended(dir+L'\\'+kTempName),store=detail::extended(dir+L'\\'+kStoreName);
    // Shared for reading: once renamed, readers open the new list while this handle is still being closed.
    detail::Handle file(CreateFileW(temp.c_str(),GENERIC_WRITE|DELETE,FILE_SHARE_READ|FILE_SHARE_DELETE,nullptr,
        CREATE_ALWAYS,FILE_ATTRIBUTE_NORMAL,nullptr));
    if(!file)return false;
    if(after_open)after_open(hook_context);
    DWORD done=0;
    bool ok=WriteFile(file.get(),text.data(),DWORD(text.size()),&done,nullptr) && done==text.size();
    bool renamed=false;
    if(ok){
        // The full path: kernelbase resolves a bare name against the working directory, not the file's own.
        const std::wstring_view name=store;
        std::vector<unsigned char> info(sizeof(FILE_RENAME_INFO)+name.size()*sizeof(wchar_t));
        auto* rename=reinterpret_cast<FILE_RENAME_INFO*>(info.data());
        rename->Flags=FILE_RENAME_FLAG_REPLACE_IF_EXISTS|FILE_RENAME_FLAG_POSIX_SEMANTICS;
        rename->FileNameLength=DWORD(name.size()*sizeof(wchar_t));
        std::copy(name.begin(),name.end(),rename->FileName);
        renamed=SetFileInformationByHandle(file.get(),FileRenameInfoEx,rename,DWORD(info.size()))!=FALSE;
    }
    file.reset();
    if(ok && !renamed)ok=MoveFileExW(temp.c_str(),store.c_str(),MOVEFILE_REPLACE_EXISTING)!=FALSE;
    if(!ok)DeleteFileW(temp.c_str());
    return ok;
}

// The process's launch mark for one list: a named event "Local\amdgpu-wddm-recent-launch-<start>-<pid>-<list>" that
// the shell which counted the launch keeps open until the process ends. The other shell of the same process finds
// it, even when a later process of the same path has taken the entry meanwhile, so one process is one launch
// whatever the order of the notes and whatever the clock. counted = true: the mark existed. Null when the event
// cannot be created; the entry's (start, pid) is then the only check.
inline HANDLE launch_mark(const Inputs& in,bool& counted) {
    counted=false;
    std::wstring dir=in.store_dir;
    LCMapStringEx(LOCALE_NAME_INVARIANT,LCMAP_UPPERCASE,dir.data(),int(dir.size()),dir.data(),int(dir.size()),
        nullptr,nullptr,0); // in place, kernel32 only (no user32 in the shells)
    uint64_t list=0xCBF29CE484222325ull; // FNV-1a of the list's directory, case folded
    for(const wchar_t c:dir){list^=uint16_t(c);list*=0x100000001B3ull;}
    wchar_t name[96];
    std::swprintf(name,std::size(name),L"Local\\amdgpu-wddm-recent-launch-%016llX-%lu-%016llX",
        static_cast<unsigned long long>(in.start),static_cast<unsigned long>(in.pid),static_cast<unsigned long long>(list));
    SetLastError(ERROR_SUCCESS);
    const HANDLE mark=CreateEventW(nullptr,TRUE,FALSE,name);
    counted=mark && GetLastError()==ERROR_ALREADY_EXISTS;
    return mark;
}

// One launch note: the bounded commit protocol of docs/design/recent-launches.md.
inline Outcome commit(const Inputs& in,uint32_t api) {
    if(in.exe.empty() || in.store_dir.empty())return Outcome::NoStore;
    if(!in.windows_dir.empty() && under_directory(in.exe,in.windows_dir))return Outcome::Excluded;
    if(own_tool(in.exe))return Outcome::Excluded;
    std::string utf8;
    if(!detail::to_utf8(in.exe,utf8) || !detail::control_free(in.exe))return Outcome::Excluded;
    Switch s=read_switch(in.switch_root,in.switch_key);
    if(s!=Switch::On)return s==Switch::Off?Outcome::SwitchOff:Outcome::SwitchUnreadable;
    if(in.after_precheck)in.after_precheck(in.hook_context);
    if(!CreateDirectoryW(detail::extended(in.store_dir).c_str(),nullptr) && GetLastError()!=ERROR_ALREADY_EXISTS)
        return Outcome::Failed;
    StoreLock lock;Outcome failure{};
    if(!lock.acquire(in.store_dir,in.lock_wait_ms,failure))return failure;
    if(in.inside_lock)in.inside_lock(in.hook_context);
    // Rechecked under the lock: a Clear that turned the list off first finds no writer that saw it on.
    s=read_switch(in.switch_root,in.switch_key);
    if(s!=Switch::On)return s==Switch::Off?Outcome::SwitchOff:Outcome::SwitchUnreadable;
    std::string text;bool missing=false,oversize=false;
    std::vector<Entry> entries;
    if(!read_store(in.store_dir,text,missing,&oversize))return Outcome::Failed; // never replace a list it could not read
    if(!missing && (oversize || !parse(text,entries))){
        entries.clear();
        // A list of a later format version belongs to a newer writer, whatever its size: leave it. A torn one is
        // replaced.
        if(text.substr(0,kHeaderPrefix.size())==kHeaderPrefix && text.substr(0,kHeader.size())!=kHeader)
            return Outcome::Failed;
    }
    bool counted=false;
    detail::Handle mark(launch_mark(in,counted));
    const auto old=std::find_if(entries.begin(),entries.end(),[&](const Entry& e){return same_path(e.path,in.exe);});
    const bool own=old!=entries.end() && old->start==in.start && old->pid==in.pid;
    // Counted already, and the entry now shows a later process of the path or the list was cleared: nothing to add.
    if(counted && !own)return Outcome::Merged;
    if(own && (old->apis&api)==api)return Outcome::Merged;
    const Outcome o=apply(entries,in.exe,in.start,in.pid,api);
    if(!serialize(entries,text) || !write_store(in.store_dir,text,in.after_temp_open,in.hook_context))return Outcome::Failed;
    if(!counted)mark.release(); // open until the process ends: its launch is in the list
    return o;
}

inline bool app_container() noexcept {
    DWORD value=0,size=0;
    return !GetTokenInformation(GetCurrentProcessToken(),TokenIsAppContainer,&value,sizeof(value),&size) || value!=0;
}
inline bool service_account() noexcept {
    alignas(8) unsigned char buffer[SECURITY_MAX_SID_SIZE+sizeof(TOKEN_USER)];
    DWORD size=0;
    if(!GetTokenInformation(GetCurrentProcessToken(),TokenUser,buffer,sizeof(buffer),&size))return true;
    const PSID sid=reinterpret_cast<TOKEN_USER*>(buffer)->User.Sid;
    return IsWellKnownSid(sid,WinLocalSystemSid) || IsWellKnownSid(sid,WinLocalServiceSid) ||
        IsWellKnownSid(sid,WinNetworkServiceSid);
}
// The production inputs of this process; false with the reason when this process records nothing.
inline bool gather(Inputs& in,Outcome& skip) {
    skip=Outcome::NoStore;
    if(app_container()){skip=Outcome::AppContainer;return false;}
    if(service_account()){skip=Outcome::Excluded;return false;}
    std::wstring local=detail::grow([](wchar_t* p,DWORD n){return GetEnvironmentVariableW(L"LOCALAPPDATA",p,n);});
    while(!local.empty() && local.back()==L'\\')local.pop_back();
    const bool absolute=(local.size()>=3 && local[1]==L':' && local[2]==L'\\') || detail::has_prefix(local,L"\\\\");
    if(!absolute)return false;
    in.store_dir=local+L'\\'+kStoreDirName;
    in.exe=normalize(detail::grow([](wchar_t* p,DWORD n){return GetModuleFileNameW(nullptr,p,n);}));
    in.windows_dir=normalize(detail::grow([](wchar_t* p,DWORD n){return GetSystemWindowsDirectoryW(p,n);}));
    if(in.exe.empty() || in.windows_dir.empty())return false;
    FILETIME created{},exited{},kernel{},user{};
    if(!GetProcessTimes(GetCurrentProcess(),&created,&exited,&kernel,&user))return false;
    in.start=(uint64_t(created.dwHighDateTime)<<32)|created.dwLowDateTime;
    in.pid=GetCurrentProcessId();
    return true;
}

// One per module: the header is compiled into each shell, so each notes its process at most once.
inline std::atomic<bool> noted{false};

inline DWORD WINAPI worker(void* parameter) noexcept {
    const uint32_t api=static_cast<uint32_t>(reinterpret_cast<uintptr_t>(parameter));
    SetThreadDescription(GetCurrentThread(),L"amdgpu-wddm recent launch");
    LARGE_INTEGER t0{},t1{},f{};
    QueryPerformanceCounter(&t0);
    Outcome o=Outcome::Failed;
    try {
        Inputs in;
        if(gather(in,o))o=commit(in,api);
    } catch(...) {o=Outcome::Failed;}
    QueryPerformanceCounter(&t1);QueryPerformanceFrequency(&f);
    char line[96];
    std::snprintf(line,sizeof(line),"amdgpu-wddm recent-launch api=%lu outcome=%s us=%lld\n",
        static_cast<unsigned long>(api),outcome_name(o),static_cast<long long>((t1.QuadPart-t0.QuadPart)*1000000/f.QuadPart));
    OutputDebugStringA(line);
    HMODULE self=nullptr;
    if(GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS|GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
        reinterpret_cast<LPCWSTR>(&worker),&self))FreeLibraryAndExitThread(self,0); // the reference note_outer_device took
    return 0;
}
// Called by a shell's adapter CreateDevice after it succeeded. The worker holds a reference on the shell, so the
// runtime may unload it meanwhile.
inline void note_outer_device(uint32_t api) noexcept {
    if(noted.exchange(true,std::memory_order_acq_rel))return;
    HMODULE self=nullptr;
    if(!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS,reinterpret_cast<LPCWSTR>(&worker),&self))return;
    const HANDLE thread=CreateThread(nullptr,64*1024,worker,reinterpret_cast<void*>(uintptr_t(api)),
        CREATE_SUSPENDED|STACK_SIZE_PARAM_IS_A_RESERVATION,nullptr);
    if(!thread){FreeLibrary(self);return;}
    SetThreadPriority(thread,THREAD_PRIORITY_BELOW_NORMAL);
    ResumeThread(thread);
    CloseHandle(thread);
}
} // namespace amdgpu_wddm::recent_launch
