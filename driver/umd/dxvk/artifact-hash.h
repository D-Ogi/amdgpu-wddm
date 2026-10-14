// SPDX-License-Identifier: MIT
#pragma once
#include <windows.h>
#include <bcrypt.h>
#include <wincrypt.h>
#include <mscat.h>
#include <cstring>
#pragma comment(lib,"bcrypt.lib")
#pragma comment(lib,"wintrust.lib")
namespace bc250::umd {
// Retain the file handle through LoadLibraryEx: no write/delete sharing means
// the verified bytes cannot change between verification and image loading.
class VerifiedArtifact final {
public:
    ~VerifiedArtifact(){if(file_!=INVALID_HANDLE_VALUE)CloseHandle(file_);}
    VerifiedArtifact()=default;
    VerifiedArtifact(const VerifiedArtifact &)=delete;
    VerifiedArtifact &operator=(const VerifiedArtifact &)=delete;
    HRESULT open(const wchar_t *path,const unsigned char *expected,bool authenticode=false) {
        if(file_!=INVALID_HANDLE_VALUE || !path || !expected)return E_INVALIDARG;
        file_=CreateFileW(path,GENERIC_READ,FILE_SHARE_READ,nullptr,OPEN_EXISTING,FILE_ATTRIBUTE_NORMAL,nullptr);
        if(file_==INVALID_HANDLE_VALUE)return HRESULT_FROM_WIN32(GetLastError());
        if(authenticode) {
            // WDK/SDK mscat.h: SHA-256 of the PE image excludes its certificate
            // table, checksum and security-directory entry. No trust decision
            // is implied; this compares the pinned code identity. Windows and
            // the installer separately check the package signature/catalog.
            HCATADMIN context=nullptr;
            if(!CryptCATAdminAcquireContext2(&context,nullptr,BCRYPT_SHA256_ALGORITHM,nullptr,0))
                return HRESULT_FROM_WIN32(GetLastError());
            struct CatCleanup { HCATADMIN h; ~CatCleanup(){CryptCATAdminReleaseContext(h,0);} } cat{context};
            unsigned char digest[32]{};DWORD bytes=sizeof(digest);
            if(!CryptCATAdminCalcHashFromFileHandle2(context,file_,&bytes,digest,0))
                return HRESULT_FROM_WIN32(GetLastError());
            if(bytes!=sizeof(digest))return E_FAIL;
            return std::memcmp(digest,expected,sizeof(digest)) ? HRESULT_FROM_WIN32(ERROR_CRC) : S_OK;
        }
        BCRYPT_ALG_HANDLE algorithm=nullptr;BCRYPT_HASH_HANDLE hash=nullptr;
        struct Cleanup {
            BCRYPT_ALG_HANDLE &a;BCRYPT_HASH_HANDLE &h;
            ~Cleanup(){if(h)BCryptDestroyHash(h);if(a)BCryptCloseAlgorithmProvider(a,0);}
        } cleanup{algorithm,hash};
        if(BCryptOpenAlgorithmProvider(&algorithm,BCRYPT_SHA256_ALGORITHM,nullptr,0)<0 ||
           BCryptCreateHash(algorithm,&hash,nullptr,0,nullptr,0,0)<0)return E_FAIL;
        unsigned char buffer[16384];DWORD count=0;
        for(;;) {
            if(!ReadFile(file_,buffer,sizeof(buffer),&count,nullptr))return HRESULT_FROM_WIN32(GetLastError());
            if(!count)break;
            if(BCryptHashData(hash,buffer,count,0)<0)return E_FAIL;
        }
        unsigned char digest[32]{};
        if(BCryptFinishHash(hash,digest,sizeof(digest),0)<0)return E_FAIL;
        return std::memcmp(digest,expected,sizeof(digest)) ? HRESULT_FROM_WIN32(ERROR_CRC) : S_OK;
    }
private:
    HANDLE file_=INVALID_HANDLE_VALUE;
};
}
