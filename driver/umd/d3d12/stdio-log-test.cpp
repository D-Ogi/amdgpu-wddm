// SPDX-License-Identifier: MIT
// Host tests of stdio-log.h: the choice of sink (AMDGPU_WDDM_LOG against the control app's timed switch), the
// bounds of a switched-on recording, and the file the switch opens.
//
// No machine state is touched. HKEY_LOCAL_MACHINE is replaced by a private application hive for this process only
// (RegLoadAppKey and RegOverridePredefKey, as the per-application settings gate does it), ProgramData points at a
// directory below the working directory, and every environment variable is set in this process alone.
//
// The sink is read once per process (a function-local static), so the cases that need another reading call
// detail::open_sink() themselves and close what they get. The one end-to-end case, which goes through the
// memoized sink, runs last.
//
// Both flavours build this file (BD-167): with AMDGPU_WDDM_LAB_FLAVOUR the lab's sink, which honours every value of
// AMDGPU_WDDM_LOG, and without it the release sink, which reads the variable only beside a marker of this process
// and joins only a file of the switch's folder. The cases that differ say which flavour they check.
#include "stdio-log.h"

#include <initializer_list>
#include <string>
#include <thread>
#include <vector>

namespace detail=amdgpu_wddm_log::detail;

namespace {
int failures=0,checks=0;
#define CHECK(x) do{++checks;if(!(x)){std::fprintf(stderr,"FAIL %s:%d: %s\n",__FILE__,__LINE__,#x);++failures;}}while(0)

const unsigned long long kSecond=10000000ull;   // one second in FILETIME units

std::string cwd() {
    char path[MAX_PATH]{};
    GetCurrentDirectoryA(MAX_PATH,path);
    return path;
}

// Everything this gate writes lives below the working directory. program_data() is where the sink looks now, so a
// case that wants an empty folder of its own points ProgramData at another tree.
std::string root() {return cwd()+"\\stdio-log-test";}

std::string program_data() {
    char path[MAX_PATH]{};
    const DWORD n=GetEnvironmentVariableA("ProgramData",path,sizeof(path));
    return (n&&n<sizeof(path))?path:"";
}

std::string games_folder() {return program_data()+"\\"+amdgpu_wddm_log::game_log::kFolder;}

void make_tree(const std::string& path) {
    std::string so_far;
    for(size_t i=0;i<=path.size();++i){
        if(i==path.size()||path[i]=='\\'){
            if(so_far.size()>3)CreateDirectoryA(so_far.c_str(),nullptr);
            if(i==path.size())break;
        }
        so_far.push_back(path[i]);
    }
}

void remove_tree(const std::string& path) {
    WIN32_FIND_DATAA found{};
    const HANDLE search=FindFirstFileA((path+"\\*").c_str(),&found);
    if(search!=INVALID_HANDLE_VALUE){
        do{
            const std::string name=found.cFileName;
            if(name=="."||name=="..")continue;
            const std::string child=path+"\\"+name;
            if(found.dwFileAttributes&FILE_ATTRIBUTE_DIRECTORY)remove_tree(child);
            else DeleteFileA(child.c_str());
        }while(FindNextFileA(search,&found));
        FindClose(search);
    }
    RemoveDirectoryA(path.c_str());
}

std::vector<std::string> files_in(const std::string& path) {
    std::vector<std::string> list;
    WIN32_FIND_DATAA found{};
    const HANDLE search=FindFirstFileA((path+"\\*.log").c_str(),&found);
    if(search==INVALID_HANDLE_VALUE)return list;
    do{
        if(!(found.dwFileAttributes&FILE_ATTRIBUTE_DIRECTORY))list.push_back(found.cFileName);
    }while(FindNextFileA(search,&found));
    FindClose(search);
    return list;
}

std::string read_file(const std::string& path) {
    const HANDLE file=CreateFileA(path.c_str(),GENERIC_READ,FILE_SHARE_READ|FILE_SHARE_WRITE|FILE_SHARE_DELETE,
        nullptr,OPEN_EXISTING,FILE_ATTRIBUTE_NORMAL,nullptr);
    if(file==INVALID_HANDLE_VALUE)return "";
    std::string text;
    char buffer[4096];
    DWORD read=0;
    while(ReadFile(file,buffer,sizeof(buffer),&read,nullptr)&&read)text.append(buffer,read);
    CloseHandle(file);
    return text;
}

bool has(const std::string& text,const char* part) {return text.find(part)!=std::string::npos;}

void set_env(const char* name,const char* value) {SetEnvironmentVariableA(name,value);}

// Both variables of the sink: the file and the marker that says whose recording it is. A case that leaves one of
// them behind would decide the next one.
void clear_sink_env() {
    SetEnvironmentVariableA(amdgpu_wddm_log::game_log::kVariable,nullptr);
    SetEnvironmentVariableA(amdgpu_wddm_log::game_log::kMarkerVariable,nullptr);
}

// The switch in the private hive: absent, or an instant given as seconds from now (negative = in the past).
void set_switch_absent() {
    HKEY key=nullptr;
    if(RegOpenKeyExA(HKEY_LOCAL_MACHINE,amdgpu_wddm_log::game_log::kKeyPath,0,KEY_SET_VALUE,&key)==ERROR_SUCCESS){
        RegDeleteValueA(key,amdgpu_wddm_log::game_log::kValueName);
        RegCloseKey(key);
    }
}

void set_switch_seconds(long long seconds) {
    HKEY key=nullptr;
    const LSTATUS st=RegCreateKeyExA(HKEY_LOCAL_MACHINE,amdgpu_wddm_log::game_log::kKeyPath,0,nullptr,0,KEY_SET_VALUE,nullptr,
        &key,nullptr);
    CHECK(st==ERROR_SUCCESS);
    if(st!=ERROR_SUCCESS)return;
    const unsigned long long until=detail::now_filetime()+static_cast<unsigned long long>(seconds*
        static_cast<long long>(kSecond));
    RegSetValueExA(key,amdgpu_wddm_log::game_log::kValueName,0,REG_QWORD,reinterpret_cast<const BYTE*>(&until),sizeof(until));
    RegCloseKey(key);
}

void set_switch_wrong_type() {
    HKEY key=nullptr;
    if(RegCreateKeyExA(HKEY_LOCAL_MACHINE,amdgpu_wddm_log::game_log::kKeyPath,0,nullptr,0,KEY_SET_VALUE,nullptr,&key,nullptr)
        !=ERROR_SUCCESS)return;
    const DWORD seconds=600;
    RegSetValueExA(key,amdgpu_wddm_log::game_log::kValueName,0,REG_DWORD,reinterpret_cast<const BYTE*>(&seconds),sizeof(seconds));
    RegCloseKey(key);
}

void close_sink(const detail::Sink& s) {if(s.file&&s.file!=stderr)std::fclose(s.file);}

// ---- the pure parts ------------------------------------------------------------------------------------------

void step_tests() {
    const detail::Sink off{nullptr,0,0};
    const detail::Sink plain{stderr,0,0};
    const detail::Sink timed{stderr,1000*kSecond,amdgpu_wddm_log::game_log::kFileCapBytes};
    CHECK(detail::step(off,0,0,false)==detail::Step::Stop);
    // A sink the process asked for has no bounds: no instant and no size ever stops it.
    CHECK(detail::step(plain,~0ull,~0ull,false)==detail::Step::Write);
    CHECK(detail::step(plain,0,0,true)==detail::Step::Stop);          // closed is closed, whatever the bounds
    CHECK(detail::step(timed,999*kSecond,0,false)==detail::Step::Write);
    CHECK(detail::step(timed,1000*kSecond,0,false)==detail::Step::Last);
    CHECK(detail::step(timed,1001*kSecond,0,false)==detail::Step::Last);
    CHECK(detail::step(timed,0,amdgpu_wddm_log::game_log::kFileCapBytes-1,false)==detail::Step::Write);
    CHECK(detail::step(timed,0,amdgpu_wddm_log::game_log::kFileCapBytes,false)==detail::Step::Last);
    CHECK(detail::step(timed,0,amdgpu_wddm_log::game_log::kFileCapBytes,true)==detail::Step::Stop);   // the last line is written once
    CHECK(has(detail::stop_reason(timed,1000*kSecond,0),"recording time is over"));
    CHECK(has(detail::stop_reason(timed,0,amdgpu_wddm_log::game_log::kFileCapBytes),"file is full"));
}

void name_tests() {
    char name[96]{};
    detail::safe_image_name(name,sizeof(name),"C:\\Program Files\\Game\\witcher3.exe");
    CHECK(!std::strcmp(name,"witcher3"));
    detail::safe_image_name(name,sizeof(name),"D:/steamapps/common/Quake II RTX/q2rtx.exe");
    CHECK(!std::strcmp(name,"q2rtx"));
    // A name of its own with characters a file name may not carry, and no extension to cut.
    detail::safe_image_name(name,sizeof(name),"my game*v2");
    CHECK(!std::strcmp(name,"my_game_v2"));
    detail::safe_image_name(name,sizeof(name),"");
    CHECK(!std::strcmp(name,"process"));
    detail::safe_image_name(name,sizeof(name),"C:\\x\\");
    CHECK(!std::strcmp(name,"process"));
    // A dotfile keeps its name: the cut is an extension, not a first dot.
    detail::safe_image_name(name,sizeof(name),".hidden");
    CHECK(!std::strcmp(name,".hidden"));
    const std::string very_long(200,'a');
    detail::safe_image_name(name,sizeof(name),very_long.c_str());
    CHECK(std::strlen(name)==64);

    SYSTEMTIME utc{};
    utc.wYear=2026;utc.wMonth=10;utc.wDay=13;utc.wHour=7;utc.wMinute=5;utc.wSecond=9;
    char file[128]{};
    CHECK(detail::log_file_name(file,sizeof(file),"witcher3",4242,utc,0));
    CHECK(!std::strcmp(file,"witcher3-4242-20261013T070509Z.log"));
    CHECK(detail::log_file_name(file,sizeof(file),"witcher3",4242,utc,3));
    CHECK(!std::strcmp(file,"witcher3-4242-20261013T070509Z-3.log"));
    char tiny[8]{};
    CHECK(!detail::log_file_name(tiny,sizeof(tiny),"witcher3",4242,utc,0));   // a name that does not fit is refused
}

// The marker AMDGPU_WDDM_GAMELOG, "<pid>;<16 hex digits>": every reader of the switch writes and reads this shape,
// this header and RADV's u_amdgpu_wddm_stdio.c alike, so a value of any other shape has to be refused.
void marker_tests() {
    unsigned long pid=0;
    unsigned long long until=0;
    char mark[48]{};
    CHECK(detail::marker_value(mark,sizeof(mark),4242,0x01D4C0FFEE123456ull));
    CHECK(!std::strcmp(mark,"4242;01D4C0FFEE123456"));
    CHECK(detail::parse_marker(mark,&pid,&until));
    CHECK(pid==4242 && until==0x01D4C0FFEE123456ull);
    // Lower case is read too: the other readers of the marker format it as they like.
    CHECK(detail::parse_marker("7;01d4c0ffee123456",&pid,&until));
    CHECK(pid==7 && until==0x01D4C0FFEE123456ull);
    CHECK(detail::parse_marker("4294967295;0000000000000000",&pid,&until) && pid==4294967295ul && until==0);
    for(const char* bad:{"","4242","4242;","4242;0","4242;01D4C0FFEE12345","4242;01D4C0FFEE1234567",
        "4242;01D4C0FFEE12345G","4242;01D4C0FFEE123456 ","-1;01D4C0FFEE123456",";01D4C0FFEE123456",
        "42 42;01D4C0FFEE123456","42949672950;01D4C0FFEE123456"})
        CHECK(!detail::parse_marker(bad,&pid,&until));
    char tiny_mark[8]{};
    CHECK(!detail::marker_value(tiny_mark,sizeof(tiny_mark),4242,0x01D4C0FFEE123456ull));
}

// Which joined path is a file of the switch's folder: the folder, one backslash and a plain name. The release
// flavour joins nothing else, whatever the marker says.
void member_tests() {
    const std::string folder="C:\\ProgramData\\amdgpu-wddm\\logs\\games";
    CHECK(detail::game_log_member((folder+"\\witcher3-4242-20261013T070509Z.log").c_str(),folder.c_str()));
    // Windows compares the names of a path without case, and so does the check.
    CHECK(detail::game_log_member("c:\\programdata\\AMDGPU-WDDM\\Logs\\games\\a.log",folder.c_str()));
    for(const std::string& bad:{std::string(),folder,folder+"\\",folder+"\\.",folder+"\\..",folder+"\\sub\\a.log",
        folder+"\\..\\a.log",folder+"\\a.log:stream",folder+"\\a/b.log",folder+"X\\a.log",folder+"/a.log",
        "D"+folder.substr(1)+"\\a.log","\\\\?\\"+folder+"\\a.log",std::string("C:\\Users\\x\\a.log")})
        CHECK(!detail::game_log_member(bad.c_str(),folder.c_str()));
    CHECK(!detail::game_log_member(nullptr,folder.c_str()));
    CHECK(!detail::game_log_member("C:\\a.log",""));                // no folder: nothing is a member
}

// Two writers of one file, which is what the marker asks for: the shell, an engine and the Vulkan ICD of one
// process all append to the switch's file. Every line has to arrive whole, so each one is written once. A
// formatted print to an unbuffered stream wrote a character at a time and tore 7975 of 8000 lines.
void tear_tests() {
    const std::string path=root()+"\\tear.log";
    DeleteFileA(path.c_str());
    const int lines=2000;
    const int width=64;                 // 63 characters of one writer and the newline
    const auto writer=[&path,lines,width](char mark){
        FILE* const file=detail::open_append(path.c_str());
        if(!file)return;
        char text[65];
        for(int i=0;i<width-1;++i)text[i]=mark;
        text[width-1]='\n';
        text[width]='\0';
        for(int i=0;i<lines;++i)detail::write_text(file,text,width);
        std::fclose(file);
    };
    std::thread one(writer,'a');
    std::thread two(writer,'b');
    one.join();
    two.join();
    const std::string text=read_file(path);
    CHECK(text.size()==static_cast<size_t>(2*lines*width));
    size_t whole=0,torn=0;
    for(size_t i=0;i+static_cast<size_t>(width)<=text.size();i+=static_cast<size_t>(width)){
        const std::string line=text.substr(i,static_cast<size_t>(width));
        if(line[width-1]=='\n'&&line.find_first_not_of(line[0])==static_cast<size_t>(width-1))++whole;
        else ++torn;
    }
    CHECK(torn==0);
    std::printf("stdio-log-test: %zu whole lines, %zu torn, of %d from two writers\n",whole,torn,2*lines);

    // What a line costs, written whole against printed into the stream, so that the cost of the fix is on record.
    LARGE_INTEGER hz{},a{},b{},c{};
    QueryPerformanceFrequency(&hz);
    const std::string whole_path=root()+"\\cost-write.log";
    const std::string print_path=root()+"\\cost-print.log";
    DeleteFileA(whole_path.c_str());
    DeleteFileA(print_path.c_str());
    char sample[65];
    for(int i=0;i<width-1;++i)sample[i]='c';
    sample[width-1]='\n';
    sample[width]='\0';
    if(FILE* const file=detail::open_append(whole_path.c_str())){
        QueryPerformanceCounter(&a);
        for(int i=0;i<lines;++i)detail::write_text(file,sample,width);
        QueryPerformanceCounter(&b);
        std::fclose(file);
    }
    if(FILE* const file=detail::open_append(print_path.c_str())){
        for(int i=0;i<lines;++i)std::fprintf(file,"%s",sample);
        QueryPerformanceCounter(&c);
        std::fclose(file);
    }
    if(hz.QuadPart)
        std::printf("stdio-log-test: one line of %d bytes: %.2f us written whole, %.2f us printed into the "
            "stream\n",width,1e6*static_cast<double>(b.QuadPart-a.QuadPart)/
            (static_cast<double>(hz.QuadPart)*lines),1e6*static_cast<double>(c.QuadPart-b.QuadPart)/
            (static_cast<double>(hz.QuadPart)*lines));
    DeleteFileA(whole_path.c_str());
    DeleteFileA(print_path.c_str());
}

int emit_line(FILE* file,_Printf_format_string_ const char* format,...) {
    va_list args;
    va_start(args,format);
    const int count=detail::emit(file,format,args);
    va_end(args);
    return count;
}

// A line that does not fit the buffer: cut, marked and still one write, because a second write for its tail is
// what tearing is made of.
void cut_tests() {
    const std::string path=root()+"\\cut.log";
    DeleteFileA(path.c_str());
    FILE* const file=detail::open_append(path.c_str());
    CHECK(file!=nullptr);
    if(!file)return;
    CHECK(emit_line(file,"a short line %d\n",7)==15);
    const std::string very_long(5000,'x');
    const int count=emit_line(file,"%s\n",very_long.c_str());
    CHECK(count==2047);
    std::fclose(file);
    const std::string text=read_file(path);
    CHECK(has(text,"a short line 7\n"));
    CHECK(text.size()==static_cast<size_t>(15+2047));
    CHECK(text.compare(text.size()-std::strlen("...[line cut]\n"),std::strlen("...[line cut]\n"),
        "...[line cut]\n")==0);
    CHECK(text.find('\n',15)==text.size()-1);       // the cut line is one line: no newline inside it
}

// ---- the choice of sink ------------------------------------------------------------------------------------

#ifdef AMDGPU_WDDM_LAB_FLAVOUR
// AMDGPU_WDDM_LOG decides alone whenever the process carries it, whatever the switch says.
void environment_wins_tests() {
    set_switch_seconds(600);
    set_env("AMDGPU_WDDM_LOG","stderr");
    detail::Sink s=detail::open_sink();
    CHECK(s.file==stderr && s.until==0 && s.cap==0);

    // Windows keeps no empty variable (a variable set to "" is gone), so "off" is every other value there is.
    for(const char* value:{"0","1","yes","file:","FILE:x","stderr2"}){
        set_env("AMDGPU_WDDM_LOG",value);
        s=detail::open_sink();
        CHECK(s.file==nullptr);
        close_sink(s);
    }

    const std::string path=root()+"\\explicit.log";
    DeleteFileA(path.c_str());
    set_env("AMDGPU_WDDM_LOG",("file:"+path).c_str());
    s=detail::open_sink();
    CHECK(s.file!=nullptr && s.until==0 && s.cap==0);
    if(s.file){
        std::fprintf(s.file,"explicit line\n");
        close_sink(s);
        CHECK(has(read_file(path),"explicit line"));
        // An explicit file is appended to, never truncated: the shells and the engines share one.
        set_env("AMDGPU_WDDM_LOG",("file:"+path).c_str());
        s=detail::open_sink();
        if(s.file){std::fprintf(s.file,"second line\n");close_sink(s);}
        const std::string text=read_file(path);
        CHECK(has(text,"explicit line") && has(text,"second line"));
    }
    // A file the process asked for is nobody's timed recording: the explicit route marks nothing, and so a module
    // loaded later reads the same variable and keeps the same freedom from bounds.
    CHECK(GetEnvironmentVariableA(amdgpu_wddm_log::game_log::kMarkerVariable,nullptr,0)==0);
    clear_sink_env();
}
#else
// The release flavour: without a marker of this process, AMDGPU_WDDM_LOG is as if it were unset, whatever it holds.
// The switch off gives no output and makes no file; the switch on gives the switch's own file, with its bounds.
void environment_wins_tests() {
    // A folder of its own: switch_tests below starts from an empty one.
    const std::string kept=program_data();
    set_env("ProgramData",(root()+"\\programdata-environment").c_str());
    make_tree(games_folder());
    const std::string path=root()+"\\explicit.log";
    DeleteFileA(path.c_str());
    const std::vector<std::string> values{"stderr","0","1","file:","FILE:x","stderr2","file:"+path};

    set_switch_absent();
    for(const std::string& value:values){
        set_env(amdgpu_wddm_log::game_log::kVariable,value.c_str());
        const detail::Sink s=detail::open_sink();
        CHECK(s.file==nullptr);
        close_sink(s);
    }
    CHECK(GetFileAttributesA(path.c_str())==INVALID_FILE_ATTRIBUTES);  // the file the variable names: never made
    CHECK(files_in(games_folder()).empty());

    set_switch_seconds(600);
    for(const std::string& value:values){
        clear_sink_env();
        set_env(amdgpu_wddm_log::game_log::kVariable,value.c_str());
        const size_t before=files_in(games_folder()).size();
        const detail::Sink s=detail::open_sink();
        CHECK(s.file!=nullptr && s.file!=stderr);
        CHECK(s.until>detail::now_filetime() && s.cap==amdgpu_wddm_log::game_log::kFileCapBytes);
        CHECK(files_in(games_folder()).size()==before+1);
        close_sink(s);
    }
    CHECK(GetFileAttributesA(path.c_str())==INVALID_FILE_ATTRIBUTES);
    set_env("ProgramData",kept.c_str());
    clear_sink_env();
}
#endif

// The switch alone, with no AMDGPU_WDDM_LOG: absent, another type, over, and running.
void switch_tests() {
    clear_sink_env();

    set_switch_absent();
    detail::Sink s=detail::open_sink();
    CHECK(s.file==nullptr);
    CHECK(files_in(games_folder()).empty());

    set_switch_wrong_type();
    s=detail::open_sink();
    CHECK(s.file==nullptr);

    set_switch_seconds(-60);        // the recording time is over
    s=detail::open_sink();
    CHECK(s.file==nullptr);
    CHECK(files_in(games_folder()).empty());
    CHECK(GetEnvironmentVariableA("AMDGPU_WDDM_LOG",nullptr,0)==0);   // nothing was pointed anywhere

    set_switch_seconds(600);
    s=detail::open_sink();
    CHECK(s.file!=nullptr);
    CHECK(s.until>detail::now_filetime());
    CHECK(s.cap==amdgpu_wddm_log::game_log::kFileCapBytes);
    std::vector<std::string> files=files_in(games_folder());
    CHECK(files.size()==1);
    if(s.file&&files.size()==1){
        std::fprintf(s.file,"a line of the switched-on sink\n");
        close_sink(s);
        const std::string text=read_file(games_folder()+"\\"+files[0]);
        CHECK(has(text,"amdgpu-wddm game log:"));
        CHECK(has(text,"stdio-log-test.exe"));                      // the process
        CHECK(has(text,"pid "));
        CHECK(has(text,"module "));
        CHECK(has(text,"Z, recording until"));                      // the UTC instant of the first line
        CHECK(has(text,"a line of the switched-on sink"));
        // The engines this process loads later read the switch themselves, so the sink points them at this file.
        char value[MAX_PATH+8]{};
        const DWORD n=GetEnvironmentVariableA("AMDGPU_WDDM_LOG",value,sizeof(value));
        CHECK(n>5 && n<sizeof(value) && !std::strncmp(value,"file:",5) && has(value,files[0].c_str()));
        // And it marks the file with this process and this instant, so that they take the bounds with it and a
        // process the game starts later does not take the file at all.
        char mark[48]{};
        const DWORD k=GetEnvironmentVariableA(amdgpu_wddm_log::game_log::kMarkerVariable,mark,sizeof(mark));
        unsigned long pid=0;
        unsigned long long instant=0;
        CHECK(k && k<sizeof(mark) && detail::parse_marker(mark,&pid,&instant));
        CHECK(pid==GetCurrentProcessId() && instant==s.until);
    } else {
        close_sink(s);
    }
    // A second reading of the same process gets its own file: CREATE_NEW never writes over one that is there.
    clear_sink_env();
    s=detail::open_sink();
    CHECK(s.file!=nullptr);
    close_sink(s);
    CHECK(files_in(games_folder()).size()==2);
    clear_sink_env();
}

// A marker another process left: a game starts this process, so the parent's file and the parent's bounds are not
// this process's. Both variables are ignored and the switch decides again, into a file of this process.
void inherited_marker_tests() {
    clear_sink_env();
    const std::string parent=root()+"\\parent.log";
    DeleteFileA(parent.c_str());
    FILE* const planted=detail::open_append(parent.c_str());
    CHECK(planted!=nullptr);
    if(planted){std::fprintf(planted,"the parent's line\n");std::fclose(planted);}
    const unsigned long long stale=detail::now_filetime()+3600*kSecond;
    char mark[48]{};
    CHECK(detail::marker_value(mark,sizeof(mark),GetCurrentProcessId()+1,stale));   // any id that is not ours
    set_env(amdgpu_wddm_log::game_log::kVariable,("file:"+parent).c_str());
    set_env(amdgpu_wddm_log::game_log::kMarkerVariable,mark);

    // With the recording over, the inherited pair gives nothing at all: the parent's file stays the parent's.
    set_switch_seconds(-60);
    detail::Sink s=detail::open_sink();
    CHECK(s.file==nullptr);
    close_sink(s);

    set_switch_seconds(600);
    const size_t before=files_in(games_folder()).size();
    s=detail::open_sink();
    CHECK(s.file!=nullptr);
    CHECK(s.until!=stale);                  // the instant is the switch's, never the one the parent carried
    CHECK(s.until>detail::now_filetime());
    CHECK(s.cap==amdgpu_wddm_log::game_log::kFileCapBytes);
    if(s.file)std::fprintf(s.file,"the child's line\n");
    close_sink(s);
    CHECK(files_in(games_folder()).size()==before+1);
    CHECK(!has(read_file(parent),"the child's line"));          // and not one line reached the parent's file
    // The marker now names this process, so the modules this process loads join this file and not the parent's.
    char value[48]{};
    const DWORD n=GetEnvironmentVariableA(amdgpu_wddm_log::game_log::kMarkerVariable,value,sizeof(value));
    unsigned long pid=0;
    unsigned long long until=0;
    CHECK(n && n<sizeof(value) && detail::parse_marker(value,&pid,&until));
    CHECK(pid==GetCurrentProcessId() && until==s.until);
    clear_sink_env();
}

// A marker of this process: the module joins the file the first module opened, under the instant the marker
// carries and a cap measured on the file, and it opens no file of its own.
void joined_marker_tests() {
    clear_sink_env();
    set_switch_seconds(600);                // on, and not read at all in these cases: the marker decides
    // Where the first module made it: in the switch's folder, the only place the release flavour joins.
    const std::string shared=games_folder()+"\\shared.log";
    DeleteFileA(shared.c_str());
    FILE* const first=detail::open_append(shared.c_str());
    CHECK(first!=nullptr);
    if(first){std::fprintf(first,"the first module's line\n");std::fclose(first);}
    const unsigned long long until=detail::now_filetime()+300*kSecond;
    char mark[48]{};
    CHECK(detail::marker_value(mark,sizeof(mark),GetCurrentProcessId(),until));
    set_env(amdgpu_wddm_log::game_log::kVariable,("file:"+shared).c_str());
    set_env(amdgpu_wddm_log::game_log::kMarkerVariable,mark);
    const size_t before=files_in(games_folder()).size();
    detail::Sink s=detail::open_sink();
    CHECK(s.file!=nullptr);
    CHECK(s.until==until);                                      // the bounds of the recording, from the marker
    CHECK(s.cap==amdgpu_wddm_log::game_log::kFileCapBytes);
    CHECK(files_in(games_folder()).size()==before);             // no second file of the same process
    if(s.file){
        std::fprintf(s.file,"the joining module's line\n");
        std::fflush(s.file);
        // The cap counts what the other module wrote as well, because it is the size of the file.
        const unsigned long long size=detail::file_size(s.file);
        CHECK(size>=std::strlen("the first module's line\nthe joining module's line\n"));
        CHECK(detail::step(s,detail::now_filetime(),size,false)==detail::Step::Write);
        close_sink(s);
        const std::string text=read_file(shared);
        CHECK(has(text,"the first module's line") && has(text,"the joining module's line"));
    } else {
        close_sink(s);
    }

    const std::string outside=root()+"\\outside.log";
    DeleteFileA(outside.c_str());
    if(FILE* const planted=detail::open_append(outside.c_str()))std::fclose(planted);
#ifdef AMDGPU_WDDM_LAB_FLAVOUR
    // The lab flavour joins the file the variable names, wherever it is, under the marker's bounds.
    set_env(amdgpu_wddm_log::game_log::kVariable,("file:"+outside).c_str());
    s=detail::open_sink();
    CHECK(s.file!=nullptr && s.until==until && s.cap==amdgpu_wddm_log::game_log::kFileCapBytes);
    close_sink(s);
#else
    // The release flavour joins only a file that is already there, directly in the switch's folder: a file
    // elsewhere, a name below a directory of the folder, a way out of it, a directory and a name that is not there
    // are refused, and nothing is made.
    const std::string sub=games_folder()+"\\sub";
    CreateDirectoryA(sub.c_str(),nullptr);
    const std::string below=sub+"\\below.log";
    if(FILE* const deeper=detail::open_append(below.c_str()))std::fclose(deeper);
    const std::string absent=games_folder()+"\\absent.log";
    const std::string escape=games_folder()+"\\..\\escape.log";
    DeleteFileA(absent.c_str());
    DeleteFileA(escape.c_str());
    for(const std::string& target:{outside,below,escape,sub,absent}){
        set_env(amdgpu_wddm_log::game_log::kVariable,("file:"+target).c_str());
        s=detail::open_sink();
        CHECK(s.file==nullptr);
        close_sink(s);
    }
    CHECK(read_file(outside).empty() && read_file(below).empty());
    CHECK(GetFileAttributesA(absent.c_str())==INVALID_FILE_ATTRIBUTES);
    CHECK(GetFileAttributesA(escape.c_str())==INVALID_FILE_ATTRIBUTES);
    DeleteFileA(below.c_str());
    RemoveDirectoryA(sub.c_str());
#endif
    set_env(amdgpu_wddm_log::game_log::kVariable,("file:"+shared).c_str());

    // The same marker with an instant that has passed: the file is joined, and the first print ends the recording.
    char over[48]{};
    CHECK(detail::marker_value(over,sizeof(over),GetCurrentProcessId(),detail::now_filetime()-kSecond));
    set_env(amdgpu_wddm_log::game_log::kMarkerVariable,over);
    s=detail::open_sink();
    CHECK(s.file!=nullptr);
    CHECK(detail::step(s,detail::now_filetime(),0,false)==detail::Step::Last);
    close_sink(s);

    // A marker of this process with nothing to join: no file, and the switch is not read either, because a module
    // of this process has already chosen where the lines of this process go.
    set_env(amdgpu_wddm_log::game_log::kVariable,nullptr);
    set_env(amdgpu_wddm_log::game_log::kMarkerVariable,mark);
    const size_t none=files_in(games_folder()).size();
    s=detail::open_sink();
    CHECK(s.file==nullptr);
    CHECK(files_in(games_folder()).size()==none);
    close_sink(s);

    // "stderr" beside a marker of this process is not a file to join either.
    set_env(amdgpu_wddm_log::game_log::kVariable,"stderr");
    s=detail::open_sink();
    CHECK(s.file==nullptr);
    close_sink(s);

    const std::string asked_for=root()+"\\explicit-marker.log";
    DeleteFileA(asked_for.c_str());
#ifdef AMDGPU_WDDM_LAB_FLAVOUR
    // A marker that cannot be read counts as absent, so an explicit variable decides, and with no bounds.
    set_env(amdgpu_wddm_log::game_log::kVariable,("file:"+asked_for).c_str());
    for(const char* bad:{"nonsense","4242","4242;zz",""}){
        set_env(amdgpu_wddm_log::game_log::kMarkerVariable,bad);
        s=detail::open_sink();
        CHECK(s.file!=nullptr && s.until==0 && s.cap==0);
        close_sink(s);
    }
    CHECK(files_in(games_folder()).size()==none);               // and the switch opened nothing in those cases
#else
    // A marker that cannot be read counts as absent, and in the release flavour so does the variable beside it:
    // the switch decides, into a file of this process with its bounds, and the file the variable names is not made.
    size_t made=0;
    for(const char* bad:{"nonsense","4242","4242;zz",""}){
        set_env(amdgpu_wddm_log::game_log::kVariable,("file:"+asked_for).c_str());
        set_env(amdgpu_wddm_log::game_log::kMarkerVariable,bad);
        s=detail::open_sink();
        CHECK(s.file!=nullptr && s.until>detail::now_filetime() && s.cap==amdgpu_wddm_log::game_log::kFileCapBytes);
        if(s.file)++made;
        close_sink(s);
    }
    CHECK(GetFileAttributesA(asked_for.c_str())==INVALID_FILE_ATTRIBUTES);
    CHECK(files_in(games_folder()).size()==none+made);
#endif
    clear_sink_env();
}

// A name in the folder that is not a plain new file: a reparse point if this PC lets the gate make one, else a
// directory. Either way the sink refuses the name and the recording stays off, which is the point of CREATE_NEW
// with FILE_FLAG_OPEN_REPARSE_POINT: a planted name never redirects the lines.
void reparse_tests() {
    clear_sink_env();
    set_switch_seconds(600);
    // An empty folder of its own: the files the cases above left carry this process's id and could hold the very
    // names this one wants to plant.
    const std::string kept=program_data();
    const std::string mine=root()+"\\programdata-planted";
    set_env("ProgramData",mine.c_str());
    make_tree(games_folder());
    SYSTEMTIME utc{};
    GetSystemTime(&utc);
    char image[MAX_PATH]{};
    const DWORD n=GetModuleFileNameA(nullptr,image,sizeof(image));
    char name[96]{};
    detail::safe_image_name(name,sizeof(name),(n&&n<sizeof(image))?image:"");
    // Every name open_sink would try this second, so that it has nothing left to fall back on.
    std::vector<std::string> taken;
    const char* kind="nothing";
    for(unsigned attempt=0;attempt<8;++attempt){
        char file[128]{};
        if(!detail::log_file_name(file,sizeof(file),name,GetCurrentProcessId(),utc,attempt))break;
        const std::string path=games_folder()+"\\"+file;
        if(CreateSymbolicLinkA(path.c_str(),(root()+"\\stolen.log").c_str(),0x2 /* a file, unprivileged */)){
            kind="reparse point";
        } else if(CreateDirectoryA(path.c_str(),nullptr)){
            if(std::strcmp(kind,"reparse point"))kind="directory";
        } else {
            break;
        }
        taken.push_back(path);
    }
    std::printf("stdio-log-test: 8 names planted, each one a %s: %s\n",kind,taken.size()==8?"yes":"no");
    if(taken.size()==8){
        const size_t before=files_in(games_folder()).size();
        detail::Sink s=detail::open_sink();
        CHECK(s.file==nullptr);                                 // refused: no line went anywhere
        close_sink(s);
        CHECK(read_file(root()+"\\stolen.log").empty());        // and nothing followed the planted name
        CHECK(files_in(games_folder()).size()==before);
    } else {
        // Nothing can be planted here, so the case says nothing either way; the refusal itself is the code path
        // CREATE_NEW takes, and open_new checks the attributes of what it opened on top of that.
        std::fprintf(stderr,"stdio-log-test: the planted-name case was skipped (%zu of 8 names)\n",taken.size());
    }
#ifndef AMDGPU_WDDM_LAB_FLAVOUR
    // A join in the release flavour: a marker of this process that names a planted name, or a second link of a file
    // elsewhere, is refused too, and no line follows it.
    char mark[48]{};
    CHECK(detail::marker_value(mark,sizeof(mark),GetCurrentProcessId(),detail::now_filetime()+300*kSecond));
    if(!taken.empty()){
        set_env(amdgpu_wddm_log::game_log::kVariable,("file:"+taken[0]).c_str());
        set_env(amdgpu_wddm_log::game_log::kMarkerVariable,mark);
        const detail::Sink s=detail::open_sink();
        CHECK(s.file==nullptr);
        close_sink(s);
        CHECK(read_file(root()+"\\stolen.log").empty());
    }
    const std::string target=root()+"\\link-target.log";
    DeleteFileA(target.c_str());
    if(FILE* const made=detail::open_append(target.c_str()))std::fclose(made);
    const std::string linked=games_folder()+"\\linked.log";
    if(CreateHardLinkA(linked.c_str(),target.c_str(),nullptr)){
        set_env(amdgpu_wddm_log::game_log::kVariable,("file:"+linked).c_str());
        set_env(amdgpu_wddm_log::game_log::kMarkerVariable,mark);
        const detail::Sink s=detail::open_sink();
        CHECK(s.file==nullptr);
        close_sink(s);
        CHECK(read_file(target).empty());
        DeleteFileA(linked.c_str());
    } else {
        std::fprintf(stderr,"stdio-log-test: the second-link case was skipped (error %lu)\n",GetLastError());
    }
    clear_sink_env();
#endif
    for(const std::string& path:taken){DeleteFileA(path.c_str());RemoveDirectoryA(path.c_str());}
    set_env("ProgramData",kept.c_str());
    clear_sink_env();
}

// The one case through the memoized sink of this process: print opens the switch's file, writes the line and
// counts it, and flush reaches the file. It runs last, because the sink is read once.
void print_tests() {
    clear_sink_env();
    set_switch_seconds(600);
    const size_t before=files_in(games_folder()).size();
    amdgpu_wddm_log::print("print reached the sink: %d %s\n",7,"ok");
    amdgpu_wddm_log::flush();
    CHECK(amdgpu_wddm_log::enabled());
    std::vector<std::string> files=files_in(games_folder());
    CHECK(files.size()==before+1);
    bool found=false;
    for(const std::string& f:files)
        if(has(read_file(games_folder()+"\\"+f),"print reached the sink: 7 ok"))found=true;
    CHECK(found);
    CHECK(detail::written().load(std::memory_order_relaxed)>0);   // the bytes of a bounded file are counted
}
} // namespace

int main() {
    clear_sink_env();
    SetEnvironmentVariableA("AMDGPU_WDDM_DDI_TRACE",nullptr);
    remove_tree(root());
    // Before anything looks at a folder: ProgramData names a tree of this gate, never the real one.
    SetEnvironmentVariableA("ProgramData",(root()+"\\programdata").c_str());
    make_tree(games_folder());

    step_tests();
    name_tests();
    marker_tests();
    member_tests();
    tear_tests();
    cut_tests();

    const std::string hive=root()+"\\stdio-log-test.hive";
    DeleteFileA(hive.c_str());
    HKEY key=nullptr;
    const std::wstring wide(hive.begin(),hive.end());
    const LSTATUS loaded=RegLoadAppKeyW(wide.c_str(),&key,KEY_ALL_ACCESS,0,0);
    CHECK(loaded==ERROR_SUCCESS);
    if(loaded==ERROR_SUCCESS){
        CHECK(RegOverridePredefKey(HKEY_LOCAL_MACHINE,key)==ERROR_SUCCESS);
        environment_wins_tests();
        switch_tests();
        inherited_marker_tests();
        joined_marker_tests();
        reparse_tests();
        print_tests();
        RegOverridePredefKey(HKEY_LOCAL_MACHINE,nullptr);
        RegCloseKey(key);
    }
    DeleteFileA(hive.c_str());
#ifdef AMDGPU_WDDM_LAB_FLAVOUR
    std::printf("stdio-log-test (lab flavour): %d checks, %d failures\n",checks,failures);
#else
    std::printf("stdio-log-test (release flavour): %d checks, %d failures\n",checks,failures);
#endif
    return failures ? 1 : 0;
}
