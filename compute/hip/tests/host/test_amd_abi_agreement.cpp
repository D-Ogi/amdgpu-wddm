// test_amd_abi_agreement.cpp - our hip/hip_runtime.h against AMD's hip_runtime_api.h.
//
// amdhip64_7.def exports most entry points of this runtime unchanged under AMD's names. This test
// prints, under each header, the type of each of those that a header declares and the number of each value
// a caller passes through them (amd_abi_probe.inc), and fails on the first line that differs.
// The two lists are written to the directory named by argv[1] as evidence.

#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

void bc250_abi_dump_ours_entry(std::FILE* out);
void bc250_abi_dump_amd_entry(std::FILE* out);

static std::vector<std::string> dump(void (*fn)(std::FILE*), const std::string& path) {
    std::FILE* file = std::fopen(path.c_str(), "w+");
    if (file == nullptr) {
        std::fprintf(stderr, "cannot write %s\n", path.c_str());
        return {};
    }
    fn(file);
    std::fflush(file);
    std::rewind(file);
    std::vector<std::string> lines;
    char line[1024];
    while (std::fgets(line, sizeof(line), file) != nullptr) {
        lines.emplace_back(line);
    }
    std::fclose(file);
    return lines;
}

int main(int argc, char** argv) {
    const std::string dir = argc > 1 ? argv[1] : ".";
    const std::vector<std::string> ours = dump(bc250_abi_dump_ours_entry, dir + "\\abi-ours.txt");
    const std::vector<std::string> amd = dump(bc250_abi_dump_amd_entry, dir + "\\abi-amd.txt");
    if (ours.empty() || ours.size() != amd.size()) {
        std::printf("FAIL: %zu lines under our header, %zu under AMD's\n", ours.size(), amd.size());
        return 1;
    }
    int differ = 0;
    size_t functions = 0;
    size_t values = 0;
    for (size_t i = 0; i < ours.size(); ++i) {
        functions += ours[i].rfind("fn ", 0) == 0 ? 1u : 0u;
        values += ours[i].rfind("value ", 0) == 0 ? 1u : 0u;
        if (ours[i] != amd[i]) {
            std::printf("DIFFER ours: %s       AMD:  %s", ours[i].c_str(), amd[i].c_str());
            ++differ;
        }
    }
    if (differ != 0) {
        std::printf("FAIL: %d of %zu lines differ\n", differ, ours.size());
        return 1;
    }
    std::printf("PASS: %zu entry point types and %zu values agree between the two headers\n",
                functions, values);
    return 0;
}
