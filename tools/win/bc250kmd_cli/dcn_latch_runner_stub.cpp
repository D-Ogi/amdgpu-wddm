// Runner exit-code control only. No adapter, display or driver access.
#include <cstdio>
#include <cstring>
int main(int argc,char** argv)
{
    for (int i=1;i+1<argc;++i) {
        if (!std::strcmp(argv[i],"--arm")) {
            int result=!std::strcmp(argv[i+1],"0")?0:1;
            std::printf("{\"type\":\"end\",\"exit_code\":%d}\n",result);
            return result;
        }
    }
    return 2;
}
