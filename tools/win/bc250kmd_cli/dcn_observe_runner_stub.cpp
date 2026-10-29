// Host-only runner control. Does not open an adapter or use driver escapes.
#include <cstring>
#include <cstdio>
int main(int argc, char** argv)
{
    for (int i = 1; i + 1 < argc; ++i) {
        if (!std::strcmp(argv[i], "--samples")) {
            const int result = !std::strcmp(argv[i + 1], "1") ? 0 : 1;
            std::printf("{\"type\":\"end\",\"exit_code\":%d}\n", result);
            return result;
        }
    }
    return 2;
}
