// SPDX-License-Identifier: MIT
#pragma once
#include <d3d12umddi.h>
// ABI gate for the explicitly supported R8/build0092 diagnostic path, on x64 and on x86 (the WoW64 shell).
// These checks validate layouts, not function implementation or runtime call order.
// A function table is its slot count times the pointer size: 64-bit sizes / 8 on x64, / 4 on x86. The other
// capability records hold no pointer, so one size fits both, except the shader model list (two pointers).
static_assert(sizeof(void*)==8 || sizeof(void*)==4);
static_assert(sizeof(D3D12DDI_ADAPTERFUNCS)==8*sizeof(void*));
static_assert(sizeof(D3D12DDI_DEVICE_FUNCS_CORE_0088)==122*sizeof(void*));
static_assert(sizeof(D3D12DDI_COMMAND_LIST_FUNCS_3D_0092)==70*sizeof(void*));
static_assert(sizeof(D3D12DDI_COMMAND_QUEUE_FUNCS_CORE_0001)==7*sizeof(void*));
static_assert(sizeof(D3D12DDI_EXTENDED_FEATURES_FUNCS_0021)==4*sizeof(void*));
static_assert(sizeof(D3D12DDI_CORELAYER_DEVICECALLBACKS_0062)==18*sizeof(void*));
static_assert(sizeof(D3D12DDI_D3D12_OPTIONS_DATA_0089)==124);
static_assert(sizeof(D3D12DDI_SHADER_CAPS_0084)==64);
static_assert(sizeof(D3D12DDI_OPTIONS_DATA_0090)==4);
static_assert(sizeof(D3D12DDI_OPTIONS_DATA_0091)==16);
static_assert(sizeof(D3D12DDI_D3D12_SHADER_MODELS_DATA_0011)==2*sizeof(void*));
static_assert(sizeof(D3D12DDI_3DPIPELINESUPPORT1_DATA_0081)==8);
static_assert(sizeof(D3D12DDI_MEMORY_ARCHITECTURE_CAPS_0041)==20);
static_assert(sizeof(D3D12DDI_TEXTURE_LAYOUT_CAPS_0026)==20);
static_assert(sizeof(D3D12DDI_GPUVA_CAPS_0004)==4);
