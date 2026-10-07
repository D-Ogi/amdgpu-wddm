// SPDX-License-Identifier: MIT
// The replay-log experiment: with deferred-replay, engine-ddi's replay lines (the policy, rings, a summary at close
// 64 and then every 10 s, long waits, stalls, the teardown) go to a file of their own as well, each with the UTC time,
// the QPC and the thread. A game started by Steam inherits no environment from a trial and nobody reads its stderr;
// this file is how its replay figures leave the process.
//   Directory: the REG_SZ value "LogDirectory" of the application profile (ddi-trace.h: the key that holds
//     "Experiment"), else the process's temporary directory (GetTempPath).
//   Name: amdgpu_wddm-replay-<image file name>-<process id>-<device number>.log, the device number counting the
//     devices of the process that opened the file (from 1).
// The file is opened for append only and shared; each line is one WriteFile. A file that cannot be opened means no
// file: the lines still go where engine-ddi's lines go.
#pragma once
#include "ddi-trace.h"
#include <windows.h>
#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <cwchar>

namespace native12 {
namespace replay_log_detail {
// The file name for an image path, a process and a device, after directory (which ends with a separator or not).
inline bool path(const wchar_t* directory,const wchar_t* image,DWORD pid,uint32_t device,wchar_t* out,size_t capacity) noexcept {
    if(!directory || !directory[0] || !image || !out || !capacity)return false;
    const wchar_t* name=image;
    for(const wchar_t* p=image;*p;++p)if(*p==L'\\' || *p==L'/')name=p+1;
    if(!name[0])return false;
    const size_t length=std::wcslen(directory);
    const bool separated=directory[length-1]==L'\\' || directory[length-1]==L'/';
    const int n=std::swprintf(out,capacity,L"%ls%lsamdgpu_wddm-replay-%ls-%lu-%u.log",directory,separated?L"":L"\\",name,
        static_cast<unsigned long>(pid),device);
    return n>0 && static_cast<size_t>(n)<capacity;
}
// The profile's LogDirectory for this process, else the temporary directory.
inline bool directory(wchar_t* out,DWORD capacity) noexcept {
    wchar_t image[MAX_PATH]{};
    const DWORD n=GetModuleFileNameW(nullptr,image,MAX_PATH);
    wchar_t key[96+MAX_PATH]{};
    if(n && n<MAX_PATH && ddi_detail::application_profile_key(image,key,sizeof(key)/sizeof(wchar_t))){
        DWORD bytes=capacity*sizeof(wchar_t);
        if(RegGetValueW(HKEY_LOCAL_MACHINE,key,L"LogDirectory",RRF_RT_REG_SZ|ddi_detail::registry_view,nullptr,out,&bytes)==
           ERROR_SUCCESS && out[0])
            return true;
    }
    const DWORD length=GetTempPathW(capacity,out);
    return length && length<capacity;
}
inline std::atomic<uint32_t> devices{0};
} // namespace replay_log_detail

class ReplayLog {
public:
    ReplayLog() noexcept=default;
    ~ReplayLog() {close();}
    ReplayLog(const ReplayLog&)=delete;
    ReplayLog& operator=(const ReplayLog&)=delete;
    // The file at path; false (and no file) if it cannot be opened.
    bool open(const wchar_t* path) noexcept {
        close();
        file_=CreateFileW(path,FILE_APPEND_DATA,FILE_SHARE_READ|FILE_SHARE_WRITE|FILE_SHARE_DELETE,nullptr,OPEN_ALWAYS,
            FILE_ATTRIBUTE_NORMAL,nullptr);
        return file_!=INVALID_HANDLE_VALUE;
    }
    // The file this process's next device gets (replay_log_detail::path); its path in path when given.
    bool open_for_process(wchar_t* path=nullptr,size_t capacity=0) noexcept {
        wchar_t directory[MAX_PATH]{},image[MAX_PATH]{},file[2*MAX_PATH]{};
        const DWORD n=GetModuleFileNameW(nullptr,image,MAX_PATH);
        const uint32_t device=replay_log_detail::devices.fetch_add(1,std::memory_order_relaxed)+1;
        if(!n || n>=MAX_PATH || !replay_log_detail::directory(directory,MAX_PATH) ||
           !replay_log_detail::path(directory,image,GetCurrentProcessId(),device,file,sizeof(file)/sizeof(wchar_t)))return false;
        if(path && capacity){
            const size_t length=std::wcslen(file);
            if(length>=capacity)return false;
            std::wmemcpy(path,file,length+1);
        }
        return open(file);
    }
    bool is_open() const noexcept {return file_!=INVALID_HANDLE_VALUE;}
    // One line: "<UTC> qpc=<n> thread=<id> <line>\n" in a single write.
    void write(const char* line) noexcept {
        if(file_==INVALID_HANDLE_VALUE || !line)return;
        SYSTEMTIME t{};GetSystemTime(&t);
        LARGE_INTEGER now{};QueryPerformanceCounter(&now);
        char text[640];
        const int n=std::snprintf(text,sizeof(text),"%04u-%02u-%02uT%02u:%02u:%02u.%03uZ qpc=%lld thread=%lu %s\n",
            t.wYear,t.wMonth,t.wDay,t.wHour,t.wMinute,t.wSecond,t.wMilliseconds,static_cast<long long>(now.QuadPart),
            static_cast<unsigned long>(GetCurrentThreadId()),line);
        if(n<=0)return;
        const DWORD bytes=static_cast<size_t>(n)<sizeof(text)?static_cast<DWORD>(n):static_cast<DWORD>(sizeof(text)-1);
        if(static_cast<size_t>(n)>=sizeof(text))text[sizeof(text)-2]='\n';
        DWORD written=0;
        WriteFile(file_,text,bytes,&written,nullptr);
    }
    void close() noexcept {
        if(file_!=INVALID_HANDLE_VALUE)CloseHandle(file_);
        file_=INVALID_HANDLE_VALUE;
    }

private:
    HANDLE file_=INVALID_HANDLE_VALUE;
};
} // namespace native12
