#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include "bd201scratch_modules.h"
int main(int argc, char **argv)
{
   HMODULE fixture = NULL;
   if (argc == 2) {
      fixture = LoadLibraryA(argv[1]);
      if (!fixture) return 1;
   }
   bd201_record_identity("before-device");
   bd201_record_first_submit();
   bd201_record_first_submit(); /* Only the first accepted submission emits a record. */
   bd201_print_modules();
   if (fixture) FreeLibrary(fixture);
   return 0;
}
