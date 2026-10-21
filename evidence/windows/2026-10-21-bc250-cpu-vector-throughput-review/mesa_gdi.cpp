/* mesa_gdi.cpp - the per-row RGBA8 -> BGRA8 loop of the GDI present in Mesa's wsi_common_win32.cpp
 * (mesa 042227dc, src/vulkan/wsi/wsi_common_win32.cpp:2299-2313), with the same types: byte pointers, the width,
 * height and format read through the chain pointer on every use, as the shipped object does (vulkan_radeon.dll
 * 4B307F93 reloads extent.width from [rbx+0x39c] per pixel). Compiled as C++ like the original file.
 */
#include <stdint.h>
#include <string.h>

struct fake_chain {
   char pad[0x39c];
   unsigned width, height;
   char pad2[0x3b0 - 0x3a4];
   int format;
};

extern "C" __declspec(noinline) void mesa_gdi_copy(fake_chain *chain, uint8_t *dptr, const uint8_t *ptr, int dpitch,
                                                   unsigned spitch)
{
   for (unsigned h = 0; h < chain->height; h++) {
      if (chain->format == 37 /* VK_FORMAT_R8G8B8A8_UNORM */) {
         for (unsigned x = 0; x < chain->width; x++) {
            dptr[x * 4 + 0] = ptr[x * 4 + 2];
            dptr[x * 4 + 1] = ptr[x * 4 + 1];
            dptr[x * 4 + 2] = ptr[x * 4 + 0];
            dptr[x * 4 + 3] = ptr[x * 4 + 3];
         }
      } else {
         memcpy(dptr, ptr, chain->width * 4);
      }
      dptr += dpitch;
      ptr += spitch;
   }
}

/* The same loop with the width read once per row into a local: the byte stores through dptr may alias *chain
 * (uint8_t is a character type), so the original must reload chain->width after every store and cannot be
 * vectorized; with a local bound the compiler is free to vectorize. */
extern "C" __declspec(noinline) void mesa_gdi_copy_hoisted(fake_chain *chain, uint8_t *dptr, const uint8_t *ptr,
                                                           int dpitch, unsigned spitch)
{
   for (unsigned h = 0; h < chain->height; h++) {
      const unsigned width = chain->width;
      if (chain->format == 37 /* VK_FORMAT_R8G8B8A8_UNORM */) {
         for (unsigned x = 0; x < width; x++) {
            dptr[x * 4 + 0] = ptr[x * 4 + 2];
            dptr[x * 4 + 1] = ptr[x * 4 + 1];
            dptr[x * 4 + 2] = ptr[x * 4 + 0];
            dptr[x * 4 + 3] = ptr[x * 4 + 3];
         }
      } else {
         memcpy(dptr, ptr, width * 4);
      }
      dptr += dpitch;
      ptr += spitch;
   }
}
