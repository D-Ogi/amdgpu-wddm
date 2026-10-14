// SPDX-License-Identifier: MIT
// Run-from-DriverStore names. SourceDisksFiles requires unique basenames across
// both architectures; CopyFiles must not rename a DIRID 13 file at installation.
#pragma once

#ifdef _WIN64
#define AMDGPU_CPU_UMD_FILE L"bc250d3d.dll"
#define AMDGPU_HOSTED_UMD_FILE L"bc250d3d_zink.dll"
#define AMDGPU_APP_UMD_RELATIVE L"..\\d3d11\\amdgpu_wddm_d3d11.dll"
#define AMDGPU_D3D11_CONFIG_FILE L"amdgpu_wddm_d3d11.config"
#define AMDGPU_DXVK_FILE L"amdgpu_wddm_dxvk.dll"
#define AMDGPU_D3D11_ICD_FILE L"amdgpu_wddm_radv_d3d11.dll"
#define AMDGPU_VKD3D_FILE L"amdgpu_wddm_vkd3d.dll"
#define AMDGPU_D3D12_ICD_FILE L"amdgpu_wddm_radv_d3d12.dll"
#else
#define AMDGPU_CPU_UMD_FILE L"bc250d3d32.dll"
#define AMDGPU_HOSTED_UMD_FILE L"bc250d3d_zink32.dll"
#define AMDGPU_APP_UMD_RELATIVE L"..\\d3d11\\amdgpu_wddm_d3d1132.dll"
#define AMDGPU_D3D11_CONFIG_FILE L"amdgpu_wddm_d3d1132.config"
#define AMDGPU_DXVK_FILE L"amdgpu_wddm_dxvk32.dll"
#define AMDGPU_D3D11_ICD_FILE L"amdgpu_wddm_radv_d3d1132.dll"
#define AMDGPU_VKD3D_FILE L"amdgpu_wddm_vkd3d32.dll"
#define AMDGPU_D3D12_ICD_FILE L"amdgpu_wddm_radv_d3d1232.dll"
#endif
