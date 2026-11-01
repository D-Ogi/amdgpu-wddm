// A private directory for one invocation of an AMD face host test. Reserving the directory
// atomically isolates both the missing-file fixture and files read after a child exits.
#pragma once

#include <windows.h>
#include <string>

class AmdFaceTestTemp {
public:
    AmdFaceTestTemp() {
        char root[MAX_PATH] = {};
        const DWORD size = GetTempPathA(MAX_PATH, root);
        if (size == 0 || size >= MAX_PATH) return;
        const std::string prefix = std::string(root) + "bc250-face-" + std::to_string(GetCurrentProcessId()) + "-";
        for (unsigned attempt = 0; attempt < 128; ++attempt) {
            const std::string candidate = prefix + std::to_string(attempt);
            if (CreateDirectoryA(candidate.c_str(), nullptr)) {
                directory_ = candidate;
                return;
            }
            if (GetLastError() != ERROR_ALREADY_EXISTS) return;
        }
    }
    ~AmdFaceTestTemp() {
        // Never delete recursively: each caller removes only the files it created.
        if (!directory_.empty()) RemoveDirectoryA(directory_.c_str());
    }
    AmdFaceTestTemp(const AmdFaceTestTemp&) = delete;
    AmdFaceTestTemp& operator=(const AmdFaceTestTemp&) = delete;
    bool valid() const { return !directory_.empty(); }
    std::string file(const char* leaf) const { return directory_ + "\\" + leaf; }

private:
    std::string directory_;
};
