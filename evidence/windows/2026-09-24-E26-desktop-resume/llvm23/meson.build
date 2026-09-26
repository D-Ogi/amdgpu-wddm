# Copyright Â© 2021 VMware, Inc
# Copyright Â© 2018 Intel Corporation
# SPDX-License-Identifier: MIT

libgallium_d3d10_def = custom_target(
  'd3d10.def',
  input: 'd3d10.def.in',
  output : 'd3d10.def',
  command : gen_vs_module_defs_normal_command,
)

gallium_d3d10_name = get_option('gallium-d3d10-dll-name')
libgallium_d3d10 = shared_library(
  gallium_d3d10_name,
  ['d3d10_gdi.c'],
  vs_module_defs : libgallium_d3d10_def,
  include_directories : [
    inc_include, inc_src, inc_gallium, inc_gallium_aux, inc_d3d10umd, inc_gallium_winsys, inc_gallium_winsys_sw, inc_gallium_drivers, inc_winddk
  ],
  link_whole : [libd3d10umd],
  link_with : [
    libgallium, libwsgdi
  ],
  dependencies : [
    dep_ws2_32, idep_nir, driver_swrast, idep_mesautil
  ],
  name_prefix: '',
  install : true,
)

if with_tests
  test(
    'd3d10',
    executable(
      'test_d3d10',
      files('tests/tri.cpp'),
      cpp_args : [cpp_msvc_compat_args],
      dependencies : [cpp.find_library('d3d11')],
      link_with : [libgallium_d3d10],
    ),
    suite : ['d3d10'],
  )
endif

executable('bc250_ttn_control', 'bc250_ttn_control.c',
 include_directories: [inc_include, inc_src, inc_gallium, inc_gallium_aux],
 link_with: [libgallium],
 dependencies: [dep_ws2_32, idep_nir, driver_swrast, idep_mesautil])

executable('bc250_lp_test_arit',
 ['../../drivers/llvmpipe/lp_test_arit.c', 'bc250_lp_test_main.c', sha1_h],
 c_args: [c_msvc_compat_args],
 dependencies: [dep_llvm, dep_dl, dep_clock, idep_mesautil],
 include_directories: [inc_gallium, inc_gallium_aux, inc_include, inc_src],
 link_with: [libllvmpipe, libgallium])
