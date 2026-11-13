# Tiled shared textures

Status: design. Nothing in this document is implemented. It is step 6 of
BD-373, the shared-open work.

## Problem

The ENGINE LAYOUT form (`driver/contract/bc250_shared_resource.h`, record S26R
version 2) has LINEAR textures only. A LINEAR image is slow to render to and to
sample. The form cannot carry an OPTIMAL image, because the opener's engine can
initialize the DCC metadata again and destroy the content.

A Vulkan image with a DRM format modifier has a layout that a second device can
rebuild from the modifier, the extent and the subresource counts. This document
uses that to share tiled textures, mip chains and arrays between the D3D11 and
D3D12 shells.

## What the sources show

The revisions are Mesa `fix/t38-bd226-timing-gate` (RADV with the WDDM2
winsys), vkd3d-proton fork 940b4bc4 (engine V15) and the DXVK fork 4cf4e386f.

1. RADV always exposes `VK_EXT_image_drm_format_modifier`
   (`radv_physical_device.c`, `EXT_image_drm_format_modifier = true`). The
   modifier list comes from `ac_get_supported_modifiers` with DCC and DCC retile
   on (`radv_formats.c`, `radv_modifier_options`).
2. A modifier with DCC limits the image to one mip level and one array layer
   (`radv_formats.c`, `ac_modifier_has_dcc(modifier) || video`). A modifier
   without DCC keeps the mip and array limits of the format.
3. RADV allocates no CMASK, FMASK or fast-clear predicate for an image with a
   modifier (`radv_image_alloc_values`: "images with modifiers can be
   potentially imported"). An image without DCC thus has no metadata that an
   opener can initialize again.
4. `vkGetImageSubresourceLayout` of an image with a modifier gives the layout of
   one memory plane, for mip 0 and layer 0 only (`radv_image.c`,
   `assert(level == 0)`). The layout of every other subresource follows from
   the modifier and the create info.
5. An image with external handle types and without a modifier gets no DCC
   (`radv_image.c`, `external_handle_types && tiling != ...MODIFIER_EXT`). An
   exportable OPTIMAL image thus loses DCC already.
6. The WDDM2 winsys has no `buffer_set_metadata`. Its NT handle import on the
   BC-250 reads only the LB7A record of a LINEAR surface. The ENGINE LAYOUT
   open does not use this import: the shell adopts the allocation and the
   engine creates the image. The modifier can thus go in the S26R record, and
   the winsys needs no change.
7. Neither engine creates an image with a modifier today. The DXVK fork uses
   the tiling only in a refusal test (`src/ddi/engine_test.cpp`).

## Design

### Record

S26R version 3 adds three fields at the end of the record: `Modifier`,
`PlaneOffset` and `PlaneRowPitch`, each 64 bits. The record becomes 264 bytes.
A reader accepts versions 1, 2 and 3. `Tiling` is
`VK_IMAGE_TILING_DRM_FORMAT_MODIFIER_EXT` (1000158000) or LINEAR. For the
modifier tiling, `LayoutHash` is the hash of the modifier, the plane offset,
the row pitch and the plane size. The match rule compares the modifier and the
plane layout in addition to the version 2 fields.

### Modifier choice

Both shells use one ordered list of modifiers without DCC. The first is the
64 KiB `R_X` swizzle of GFX10 for the format, and the last is
`DRM_FORMAT_MOD_LINEAR`. A shell takes the first modifier that
`vkGetPhysicalDeviceImageFormatProperties2` admits for the description, with
`VkPhysicalDeviceImageDrmFormatModifierInfoEXT`. A description that no modifier
admits keeps the version 2 LINEAR form. Modifiers with DCC are a later version.
They need the queue-family ownership rules of RADV for foreign queues, and an
opener that never transitions the image from `VK_IMAGE_LAYOUT_UNDEFINED`.

### Creator and opener

The creator's engine creates the image with
`VkImageDrmFormatModifierListCreateInfoEXT` that holds the chosen modifier. It
reads the modifier back with `vkGetImageDrmFormatModifierPropertiesEXT` and the
plane layout with `vkGetImageSubresourceLayout`, and the shell writes them in
the record. The opener's engine creates its image over the adopted memory with
`VkImageDrmFormatModifierExplicitCreateInfoEXT`, from the record. Then the
opener makes its own record and the match rule decides. Both engines need a new
ABI version for this: engine ABI 1.12 for vkd3d-proton, and the same function
in the DXVK engine.

### Which creates get the form

The D3D11 shell knows a shared create (`D3D11_RESOURCE_MISC_SHARED` and
`D3D11_RESOURCE_MISC_SHARED_NTHANDLE`). It gives the form only to shared
textures, so other textures do not change.

The D3D12 DDI does not say that a create is shared (engine-ddi
`INTEGRATION.md`, "The shared surface"). The D3D12 shell must thus select the
form from the description, as BD-217 does for the LINEAR surface. There are
two choices:

- (a) Every texture that `Bc250SharedResourceDescWhy` admits. Such a texture
  loses DCC and fast clears, also when the application never shares it.
- (b) Only the textures that take the LINEAR surface today, and the mip chains
  and arrays inside the same rule. This replaces LINEAR with a tiled layout,
  which is faster, and adds mips and arrays. Other textures keep OPTIMAL.

Choice (b) is the recommendation. It is the smallest change to what renders
today. A lab measurement must decide whether (b) costs frame time: Quake II RTX
and Rise of the Tomb Raider, with the form on and off.

### Out of scope

The present and scan-out path keeps its LINEAR LB7A record. The kernel driver
does not change: the BC2A blob gives the placement, and `MemorySize` gives the
size. Buffers do not change.

## Tests

- Contract host tests: the version 3 record, its decode, the match rule for
  each new field, and a version 2 record that a version 3 reader accepts.
- Engine tests on RADV: a texture with mips and layers created with the chosen
  modifier, opened with the explicit modifier on a second device, and every
  subresource copied in both directions.
- Lab: `tools/win/shared-interop` gets a tiled texture with 7 mips that D3D11
  creates and D3D12 opens, and the reverse. Each case compares every texel of
  every subresource.

## Open questions

1. Does the D3D12 runtime keep a record of 264 bytes for every texture in
   choice (b)? The buffer probe of step 5 answers the same question for 240
   bytes.
2. What does choice (b) cost in frame time on the two release games?
