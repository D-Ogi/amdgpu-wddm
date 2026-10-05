#include <stdlib.h>
#define main bc250_upstream_main
#include "../../drivers/llvmpipe/lp_test_main.c"
#undef main
int main(int argc, char **argv) {
   _set_error_mode(_OUT_TO_STDERR);
   return bc250_upstream_main(argc, argv);
}
