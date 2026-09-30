// SPDX-License-Identifier: MIT
// Ray pipeline variant of the interactive client (build.ps1 -RayPipeline). Included by interactive.h inside namespace
// interactive, after Session. The "copy" verb then performs this scene, so commands and receipts keep their form.
//
// The scene of the ray query variant (interactive-rayquery.h) traced through a raytracing pipeline instead of an
// inline query, public API only (ID3D12Device5, ID3D12GraphicsCommandList4): a bottom level of one triangle and a
// top level of one instance, then a state object from the DXIL library of raypipeline.hlsl (raygen, miss, closest),
// one triangle hit group, shader and pipeline configs (recursion depth 1) and a global root signature, with no local
// root signature or collection. A shader table in UPLOAD memory holds the three identifiers; DispatchRays 8x8
// writes 1 for a hit and 2 for a miss. Builds and dispatch are recorded on one DIRECT list for the session's DIRECT
// queue. The READBACK buffer starts at a value that is neither 1 nor 2, and every word is compared with the pattern
// computed on the CPU from the triangle's edges, which keep a margin from every ray.
//
// Other builds of this file:
// - build.ps1 -RayState makes raystate() below the "copy" verb instead: the same state object created and released
//   twice, with no acceleration structure, command list or submission.
// - build.ps1 -RayCollection traces the same scene through a pipeline made of a COLLECTION state object (library, hit
//   group, configs, global root signature) and nothing else of code (ray_collection_state).
// - build.ps1 -RayGrow traces it through a pipeline grown by AddToStateObject (ray_grow_state): the parent holds the
//   library of raygrow.hlsl, whose hit group reads a constant from its local root signature, and the addition brings
//   a second miss shader. Hits carry that constant, misses are 2 in even columns and 3 in odd ones.
#include "raypipeline-program.h"
#include "raygrow-program.h"

enum class RayMode {Pipeline,Grow,Collection};

// Global root signature: the top level as a root SRV t0 and the output as a root UAV u0, both by address. With local,
// the local root signature of the grow variant's hit group instead: one 32-bit constant at b0 in space 1.
inline HRESULT ray_root_signature(Session& s,ComPtr<ID3D12RootSignature>& root,bool local=false){
    auto proc=GetProcAddress(s.runtime,"D3D12SerializeRootSignature");
    decltype(&D3D12SerializeRootSignature) serialize=nullptr;
    static_assert(sizeof(serialize)==sizeof(proc));std::memcpy(&serialize,&proc,sizeof(serialize));
    if(!serialize)return E_NOINTERFACE;
    D3D12_ROOT_PARAMETER parameters[2]{};D3D12_ROOT_SIGNATURE_DESC signature{};signature.pParameters=parameters;
    if(local){
        parameters[0].ParameterType=D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;parameters[0].Constants={0,1,1};
        parameters[0].ShaderVisibility=D3D12_SHADER_VISIBILITY_ALL;
        signature.NumParameters=1;signature.Flags=D3D12_ROOT_SIGNATURE_FLAG_LOCAL_ROOT_SIGNATURE;
    }else{
        parameters[0].ParameterType=D3D12_ROOT_PARAMETER_TYPE_SRV;parameters[0].ShaderVisibility=D3D12_SHADER_VISIBILITY_ALL;
        parameters[1].ParameterType=D3D12_ROOT_PARAMETER_TYPE_UAV;parameters[1].ShaderVisibility=D3D12_SHADER_VISIBILITY_ALL;
        signature.NumParameters=2;
    }
    ComPtr<ID3DBlob> blob,errors;
    HRESULT hr=s.api(local?"D3D12SerializeRootSignature local":"D3D12SerializeRootSignature",[&]{return serialize(&signature,D3D_ROOT_SIGNATURE_VERSION_1_0,&blob,&errors);});if(FAILED(hr))return hr;
    return s.api(local?"CreateRootSignature local":"CreateRootSignature",[&]{return s.device->CreateRootSignature(0,blob->GetBufferPointer(),blob->GetBufferSize(),IID_PPV_ARGS(&root));});
}
// One state object: the library's three exports, the hit group, both configs and the global root signature.
// Without associations the configs and the root signature apply to every export. A collection of the same
// subobjects is self-contained, so the driver can compile it on creation.
inline HRESULT ray_state_object(Session& s,ID3D12Device5* device5,ID3D12RootSignature* root,ComPtr<ID3D12StateObject>& state,
                                D3D12_STATE_OBJECT_TYPE type=D3D12_STATE_OBJECT_TYPE_RAYTRACING_PIPELINE){
    D3D12_EXPORT_DESC exports[3]{{L"raygen",nullptr,D3D12_EXPORT_FLAG_NONE},{L"miss",nullptr,D3D12_EXPORT_FLAG_NONE},
        {L"closest",nullptr,D3D12_EXPORT_FLAG_NONE}};
    D3D12_DXIL_LIBRARY_DESC library{{g_raypipeline_lib,sizeof(g_raypipeline_lib)},3,exports};
    D3D12_HIT_GROUP_DESC group{L"group",D3D12_HIT_GROUP_TYPE_TRIANGLES,nullptr,L"closest",nullptr};
    D3D12_RAYTRACING_SHADER_CONFIG shader_config{4,8};  // payload one uint, attributes two floats (barycentrics)
    D3D12_RAYTRACING_PIPELINE_CONFIG pipeline_config{1};
    D3D12_GLOBAL_ROOT_SIGNATURE global{root};
    const D3D12_STATE_SUBOBJECT subobjects[5]{
        {D3D12_STATE_SUBOBJECT_TYPE_DXIL_LIBRARY,&library},{D3D12_STATE_SUBOBJECT_TYPE_HIT_GROUP,&group},
        {D3D12_STATE_SUBOBJECT_TYPE_RAYTRACING_SHADER_CONFIG,&shader_config},
        {D3D12_STATE_SUBOBJECT_TYPE_RAYTRACING_PIPELINE_CONFIG,&pipeline_config},
        {D3D12_STATE_SUBOBJECT_TYPE_GLOBAL_ROOT_SIGNATURE,&global}};
    const D3D12_STATE_OBJECT_DESC pipeline{type,5,subobjects};
    return s.api(type==D3D12_STATE_OBJECT_TYPE_COLLECTION?"CreateStateObject COLLECTION lib_6_3":"CreateStateObject RAYTRACING_PIPELINE lib_6_3",
        [&]{return device5->CreateStateObject(&pipeline,IID_PPV_ARGS(&state));});
}
// Identifiers of raygen, miss and the hit group, in that order; they point into the state object.
inline HRESULT ray_identifiers(Session& s,ID3D12StateObjectProperties* properties,const void* (&identifiers)[3]){
    const wchar_t* const names[3]{L"raygen",L"miss",L"group"};
    const char* const labels[3]{"GetShaderIdentifier raygen","GetShaderIdentifier miss","GetShaderIdentifier group"};
    for(int i=0;i<3;++i){
        s.event("before",labels[i]);identifiers[i]=properties->GetShaderIdentifier(names[i]);s.event("after",labels[i],identifiers[i]?S_OK:E_FAIL);
        if(!identifiers[i])return E_FAIL;
    }
    return S_OK;
}
// Releases the caller's last reference: "Release <what>" around the call, then "<what> released", then the count the
// runtime returns, which is an observation and decides nothing.
template<class T> void ray_release(Session& s,ComPtr<T>& object,const char* what){
    char label[96]{};sprintf_s(label,"Release %s",what);
    s.event("before",label);const ULONG left=object.Detach()->Release();s.event("after",label);
    sprintf_s(label,"%s released",what);s.event("after",label);
    sprintf_s(label,"%s references left %lu",what,static_cast<unsigned long>(left));s.event("after",label);
}

// The collection variant's pipeline: a COLLECTION with the subobjects of ray_state_object, then an executable
// pipeline made of that collection (all of its exports), the global root signature and the pipeline config, with no
// library of its own. The application's reference to the collection is released once the pipeline exists.
inline HRESULT ray_collection_state(Session& s,ID3D12Device5* device5,ID3D12RootSignature* root,ComPtr<ID3D12StateObject>& executable){
    ComPtr<ID3D12StateObject> collection;
    HRESULT hr=ray_state_object(s,device5,root,collection,D3D12_STATE_OBJECT_TYPE_COLLECTION);if(FAILED(hr))return hr;
    D3D12_EXISTING_COLLECTION_DESC existing{collection.Get(),0,nullptr};
    D3D12_GLOBAL_ROOT_SIGNATURE global{root};
    D3D12_RAYTRACING_PIPELINE_CONFIG pipeline_config{1};
    const D3D12_STATE_SUBOBJECT subobjects[3]{
        {D3D12_STATE_SUBOBJECT_TYPE_EXISTING_COLLECTION,&existing},{D3D12_STATE_SUBOBJECT_TYPE_GLOBAL_ROOT_SIGNATURE,&global},
        {D3D12_STATE_SUBOBJECT_TYPE_RAYTRACING_PIPELINE_CONFIG,&pipeline_config}};
    const D3D12_STATE_OBJECT_DESC pipeline{D3D12_STATE_OBJECT_TYPE_RAYTRACING_PIPELINE,3,subobjects};
    hr=s.api("CreateStateObject RAYTRACING_PIPELINE from collection",[&]{return device5->CreateStateObject(&pipeline,IID_PPV_ARGS(&executable));});if(FAILED(hr))return hr;
    ray_release(s,collection,"Collection");
    return S_OK;
}

// The grow variant's pipeline. The parent allows additions and holds the library of raygrow.hlsl, the hit group with
// its local root signature (associated by name), the configs and the global root signature; its identifiers of
// raygen, miss and group are copied to ids[0..2] and its stack size is set to its default plus 1024 bytes. The
// addition allows additions too and is valid on its own: the library of raygrow-miss.hlsl with the same configs and
// global root signature. The child starts with the parent's stack size (DXR specification, AddToStateObject); a
// different answer fails on the BC-250 and is only recorded on WARP. The identifier of miss_new goes to ids[3], and
// the parent is released before the child is used.
inline HRESULT ray_grow_state(Session& s,ID3D12Device5* device5,ID3D12RootSignature* root,ID3D12RootSignature* local,
                              ComPtr<ID3D12StateObject>& child,unsigned char (&ids)[4][D3D12_SHADER_IDENTIFIER_SIZE_IN_BYTES]){
    constexpr UINT64 added_stack=1024;
    char label[128]{};
    const D3D12_STATE_OBJECT_CONFIG growable{D3D12_STATE_OBJECT_FLAG_ALLOW_STATE_OBJECT_ADDITIONS};
    D3D12_RAYTRACING_SHADER_CONFIG shader_config{4,8};
    D3D12_RAYTRACING_PIPELINE_CONFIG pipeline_config{1};
    D3D12_GLOBAL_ROOT_SIGNATURE global{root};
    D3D12_EXPORT_DESC exports[3]{{L"raygen",nullptr,D3D12_EXPORT_FLAG_NONE},{L"miss",nullptr,D3D12_EXPORT_FLAG_NONE},
        {L"closest",nullptr,D3D12_EXPORT_FLAG_NONE}};
    D3D12_DXIL_LIBRARY_DESC library{{g_raygrow_lib,sizeof(g_raygrow_lib)},3,exports};
    D3D12_HIT_GROUP_DESC group{L"group",D3D12_HIT_GROUP_TYPE_TRIANGLES,nullptr,L"closest",nullptr};
    D3D12_LOCAL_ROOT_SIGNATURE local_root{local};
    D3D12_STATE_SUBOBJECT subobjects[8]{
        {D3D12_STATE_SUBOBJECT_TYPE_STATE_OBJECT_CONFIG,&growable},{D3D12_STATE_SUBOBJECT_TYPE_DXIL_LIBRARY,&library},
        {D3D12_STATE_SUBOBJECT_TYPE_HIT_GROUP,&group},{D3D12_STATE_SUBOBJECT_TYPE_LOCAL_ROOT_SIGNATURE,&local_root},
        {D3D12_STATE_SUBOBJECT_TYPE_SUBOBJECT_TO_EXPORTS_ASSOCIATION,nullptr},
        {D3D12_STATE_SUBOBJECT_TYPE_RAYTRACING_SHADER_CONFIG,&shader_config},
        {D3D12_STATE_SUBOBJECT_TYPE_RAYTRACING_PIPELINE_CONFIG,&pipeline_config},
        {D3D12_STATE_SUBOBJECT_TYPE_GLOBAL_ROOT_SIGNATURE,&global}};
    LPCWSTR grouped[1]{L"group"};
    const D3D12_SUBOBJECT_TO_EXPORTS_ASSOCIATION association{&subobjects[3],1,grouped};
    subobjects[4].pDesc=&association;
    const D3D12_STATE_OBJECT_DESC parent_desc{D3D12_STATE_OBJECT_TYPE_RAYTRACING_PIPELINE,8,subobjects};
    ComPtr<ID3D12StateObject> parent;
    HRESULT hr=s.api("CreateStateObject RAYTRACING_PIPELINE parent, additions allowed",[&]{return device5->CreateStateObject(&parent_desc,IID_PPV_ARGS(&parent));});if(FAILED(hr))return hr;
    ComPtr<ID3D12StateObjectProperties> parent_properties;
    hr=s.api("QueryInterface ID3D12StateObjectProperties parent",[&]{return parent.As(&parent_properties);});if(FAILED(hr))return hr;
    const void* identifiers[3]{};
    hr=ray_identifiers(s,parent_properties.Get(),identifiers);if(FAILED(hr))return hr;
    // Only these copies are used from here; the returned pointers belong to the parent and die with it.
    for(int i=0;i<3;++i)std::memcpy(ids[i],identifiers[i],sizeof(ids[i]));
    s.event("after","Parent identifiers copied");
    s.event("before","GetShaderStackSize parent raygen");const UINT64 parent_raygen=parent_properties->GetShaderStackSize(L"raygen");s.event("after","GetShaderStackSize parent raygen");
    s.event("before","GetPipelineStackSize parent default");const UINT64 initial=parent_properties->GetPipelineStackSize();s.event("after","GetPipelineStackSize parent default");
    const UINT64 chosen=initial+added_stack;
    s.event("before","SetPipelineStackSize parent");parent_properties->SetPipelineStackSize(chosen);s.event("after","SetPipelineStackSize parent");
    s.event("before","GetPipelineStackSize parent");const UINT64 parent_stack=parent_properties->GetPipelineStackSize();s.event("after","GetPipelineStackSize parent");
    sprintf_s(label,"Parent stack raygen %llu, pipeline default %llu, set %llu, read back %llu",static_cast<unsigned long long>(parent_raygen),
        static_cast<unsigned long long>(initial),static_cast<unsigned long long>(chosen),static_cast<unsigned long long>(parent_stack));
    s.event("after",label,parent_stack==chosen?S_OK:E_FAIL);if(parent_stack!=chosen)return E_FAIL;

    D3D12_EXPORT_DESC added[1]{{L"miss_new",nullptr,D3D12_EXPORT_FLAG_NONE}};
    D3D12_DXIL_LIBRARY_DESC addition_library{{g_raygrow_miss_lib,sizeof(g_raygrow_miss_lib)},1,added};
    const D3D12_STATE_SUBOBJECT addition_subobjects[5]{
        {D3D12_STATE_SUBOBJECT_TYPE_STATE_OBJECT_CONFIG,&growable},{D3D12_STATE_SUBOBJECT_TYPE_DXIL_LIBRARY,&addition_library},
        {D3D12_STATE_SUBOBJECT_TYPE_RAYTRACING_SHADER_CONFIG,&shader_config},
        {D3D12_STATE_SUBOBJECT_TYPE_RAYTRACING_PIPELINE_CONFIG,&pipeline_config},
        {D3D12_STATE_SUBOBJECT_TYPE_GLOBAL_ROOT_SIGNATURE,&global}};
    const D3D12_STATE_OBJECT_DESC addition{D3D12_STATE_OBJECT_TYPE_RAYTRACING_PIPELINE,5,addition_subobjects};
    ComPtr<ID3D12Device7> device7;
    hr=s.api("QueryInterface ID3D12Device7",[&]{return device5->QueryInterface(IID_PPV_ARGS(&device7));});if(FAILED(hr))return hr;
    hr=s.api("AddToStateObject miss_new",[&]{return device7->AddToStateObject(&addition,parent.Get(),IID_PPV_ARGS(&child));});if(FAILED(hr))return hr;
    ComPtr<ID3D12StateObjectProperties> child_properties;
    hr=s.api("QueryInterface ID3D12StateObjectProperties child",[&]{return child.As(&child_properties);});if(FAILED(hr))return hr;
    s.event("before","GetPipelineStackSize child");const UINT64 child_stack=child_properties->GetPipelineStackSize();s.event("after","GetPipelineStackSize child");
    // WARP starts the child at 0 against that rule (README); there a mismatch is recorded and the operation goes on.
    // On the BC-250 it fails.
    const bool inherited=child_stack==parent_stack,software=s.mode==AdapterMode::Warp;
    sprintf_s(label,"Pipeline stack size parent %llu child %llu%s",static_cast<unsigned long long>(parent_stack),
        static_cast<unsigned long long>(child_stack),!inherited && software?", software adapter: not decisive":"");
    s.event("after",label,inherited || software?S_OK:E_FAIL);if(!inherited && !software)return E_FAIL;
    s.event("before","GetShaderStackSize child raygen");const UINT64 child_raygen=child_properties->GetShaderStackSize(L"raygen");s.event("after","GetShaderStackSize child raygen");
    sprintf_s(label,"Shader stack size raygen parent %llu child %llu",static_cast<unsigned long long>(parent_raygen),static_cast<unsigned long long>(child_raygen));
    s.event("after",label);
    s.event("before","GetShaderIdentifier miss_new");const void* fresh=child_properties->GetShaderIdentifier(L"miss_new");
    s.event("after","GetShaderIdentifier miss_new",fresh?S_OK:E_FAIL);if(!fresh)return E_FAIL;
    std::memcpy(ids[3],fresh,sizeof(ids[3]));
    bool distinct=true;UINT zero=0;
    for(int i=0;i<4;++i){
        zero+=std::all_of(ids[i],ids[i]+sizeof(ids[i]),[](unsigned char b){return b==0;})?1u:0u;
        for(int j=0;j<i;++j)if(!std::memcmp(ids[i],ids[j],sizeof(ids[i])))distinct=false;
    }
    sprintf_s(label,"Identifiers %u of 4 nonzero, %s",4-zero,distinct?"all different":"two equal");
    s.event("after",label,!zero && distinct?S_OK:E_FAIL);if(zero || !distinct)return E_FAIL;
    // The parent loses every application reference here, properties first; no session slot ever holds it. From here
    // only the child is used, and the copied parent identifiers stay valid for it (DXR specification).
    ray_release(s,parent_properties,"Parent properties");
    ray_release(s,parent,"Parent state object");
    return S_OK;
}

inline HRESULT ray_dispatch(Session& s,RayMode mode){
    if(!s.queue || s.pending || s.copy_success)return E_UNEXPECTED;
    const bool grow=mode==RayMode::Grow;
    constexpr UINT grid=8,words=grid*grid;
    constexpr UINT32 hit=1,miss=2,new_miss=3,grow_hit=0x00C0FFEEu,prefill=0xA5A5A5A5u;
    constexpr UINT64 upload_bytes=1024,instance_offset=256,output_bytes=words*4;
    // Shader table: raygen record at 0, miss at 64, hit group at 128, each one identifier with no local arguments.
    // The grow variant has two miss records (64 and 96) and a hit group record of 64 bytes: identifier, then the
    // local root constant.
    constexpr UINT64 record=D3D12_SHADER_IDENTIFIER_SIZE_IN_BYTES,table_step=D3D12_RAYTRACING_SHADER_TABLE_BYTE_ALIGNMENT,table_bytes=256;
    constexpr UINT64 grow_hit_stride=64;
    static_assert(record%D3D12_RAYTRACING_SHADER_RECORD_BYTE_ALIGNMENT==0 && 3*table_step<=table_bytes);
    static_assert(2*record<=table_step && record+4<=grow_hit_stride && grow_hit_stride%D3D12_RAYTRACING_SHADER_RECORD_BYTE_ALIGNMENT==0 &&
                  2*table_step+grow_hit_stride<=table_bytes);
    // The triangle at z = 0.5 in the rays' xy plane; ray (x, y) starts at ((x + 0.5) / 4 - 1, (y + 0.5) / 4 - 1).
    constexpr float triangle[3][3]{{-0.8f,-0.7f,0.5f},{0.1f,-0.7f,0.5f},{-0.7f,0.9f,0.5f}};
    ID3D12Device* device=s.device.Get();
    char label[128]{};
    D3D12_FEATURE_DATA_D3D12_OPTIONS5 options5{};
    HRESULT hr=s.api("CheckFeatureSupport OPTIONS5",[&]{return device->CheckFeatureSupport(D3D12_FEATURE_D3D12_OPTIONS5,&options5,sizeof(options5));});
    sprintf_s(label,"Reported RaytracingTier %u",SUCCEEDED(hr)?static_cast<unsigned>(options5.RaytracingTier):0u);
    s.event("after",label,hr);if(FAILED(hr))return hr;
    // AddToStateObject needs tier 1.1.
    if(options5.RaytracingTier<(grow?D3D12_RAYTRACING_TIER_1_1:D3D12_RAYTRACING_TIER_1_0)){
        s.event("after",grow?"RaytracingTier below 1.1, no ray pipeline attempted":"RaytracingTier below 1.0, no ray pipeline attempted",DXGI_ERROR_UNSUPPORTED);
        return DXGI_ERROR_UNSUPPORTED;}
    {   // Observation only: a state object refusal below reads differently without it.
        D3D12_FEATURE_DATA_SHADER_MODEL model{D3D_SHADER_MODEL_6_5};
        const HRESULT query=s.api("CheckFeatureSupport SHADER_MODEL",[&]{return device->CheckFeatureSupport(D3D12_FEATURE_SHADER_MODEL,&model,sizeof(model));});
        sprintf_s(label,"Reported HighestShaderModel %x",SUCCEEDED(query)?static_cast<unsigned>(model.HighestShaderModel):0u);s.event("after",label,query);
    }

    // Expected word of each ray from the triangle's edge functions; no ray may lie within 0.05 of an edge line.
    const UINT32 hit_word=grow?grow_hit:hit;
    UINT32 expected[words]{};UINT hits=0,old_misses=0,new_misses=0;bool margin=true;
    for(UINT y=0;y<grid;++y)for(UINT x=0;x<grid;++x){
        const double px=(x+0.5)/4.0-1.0,py=(y+0.5)/4.0-1.0;int positive=0;
        for(int e=0;e<3;++e){
            const float* a=triangle[e];const float* b=triangle[(e+1)%3];
            const double dx=double{b[0]}-a[0],dy=double{b[1]}-a[1],side=dx*(py-a[1])-dy*(px-a[0]);
            if(side*side<0.05*0.05*(dx*dx+dy*dy))margin=false;
            positive+=side>0?1:0;
        }
        UINT32& word=expected[y*grid+x];
        word=(positive==0 || positive==3)?hit_word:(grow && (x&1))?new_miss:miss;
        hits+=word==hit_word?1u:0u;old_misses+=word==miss?1u:0u;new_misses+=word==new_miss?1u:0u;
    }
    sprintf_s(label,"Expected pattern %u hits of %u, margin %s",hits,words,margin?"kept":"violated");
    s.event("after",label,margin && hits && hits<words?S_OK:E_UNEXPECTED);if(!margin || !hits || hits==words)return E_UNEXPECTED;
    if(grow){
        sprintf_s(label,"Expected misses %u old, %u new, hit word %08x",old_misses,new_misses,hit_word);
        s.event("after",label,old_misses && new_misses?S_OK:E_UNEXPECTED);if(!old_misses || !new_misses)return E_UNEXPECTED;
    }

    ComPtr<ID3D12Device5> device5;
    hr=s.api("QueryInterface ID3D12Device5",[&]{return s.device.As(&device5);});if(FAILED(hr))return hr;
    s.extra[0]=device5;

    D3D12_RESOURCE_DESC buffer{};buffer.Dimension=D3D12_RESOURCE_DIMENSION_BUFFER;buffer.Width=upload_bytes;
    buffer.Height=1;buffer.DepthOrArraySize=1;buffer.MipLevels=1;buffer.SampleDesc.Count=1;buffer.Layout=D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    D3D12_HEAP_PROPERTIES heap{};heap.Type=D3D12_HEAP_TYPE_UPLOAD;heap.CreationNodeMask=heap.VisibleNodeMask=1;
    hr=s.api("CreateCommittedResource UPLOAD",[&]{return device->CreateCommittedResource(&heap,D3D12_HEAP_FLAG_NONE,&buffer,D3D12_RESOURCE_STATE_GENERIC_READ,nullptr,IID_PPV_ARGS(&s.upload));});if(FAILED(hr))return hr;
    s.event("before","GetGPUVirtualAddress UPLOAD");const D3D12_GPU_VIRTUAL_ADDRESS upload_va=s.upload->GetGPUVirtualAddress();s.event("after","GetGPUVirtualAddress UPLOAD",upload_va?S_OK:E_FAIL);
    if(!upload_va)return E_FAIL;
    ComPtr<ID3D12Resource> table;buffer.Width=table_bytes;
    hr=s.api("CreateCommittedResource shader table UPLOAD",[&]{return device->CreateCommittedResource(&heap,D3D12_HEAP_FLAG_NONE,&buffer,D3D12_RESOURCE_STATE_GENERIC_READ,nullptr,IID_PPV_ARGS(&table));});if(FAILED(hr))return hr;
    s.extra[7]=table;
    s.event("before","GetGPUVirtualAddress shader table");const D3D12_GPU_VIRTUAL_ADDRESS table_va=table->GetGPUVirtualAddress();
    s.event("after","GetGPUVirtualAddress shader table",table_va && !(table_va%table_step)?S_OK:E_FAIL);if(!table_va || table_va%table_step)return E_FAIL;

    // Vertices at 0 and the instance description at 256 of the UPLOAD buffer.
    D3D12_RAYTRACING_GEOMETRY_DESC geometry{};geometry.Type=D3D12_RAYTRACING_GEOMETRY_TYPE_TRIANGLES;
    geometry.Flags=D3D12_RAYTRACING_GEOMETRY_FLAG_OPAQUE;geometry.Triangles.IndexFormat=DXGI_FORMAT_UNKNOWN;
    geometry.Triangles.VertexFormat=DXGI_FORMAT_R32G32B32_FLOAT;geometry.Triangles.VertexCount=3;
    geometry.Triangles.VertexBuffer={upload_va,sizeof(triangle[0])};
    D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_INPUTS bottom{};bottom.Type=D3D12_RAYTRACING_ACCELERATION_STRUCTURE_TYPE_BOTTOM_LEVEL;
    bottom.Flags=D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BUILD_FLAG_PREFER_FAST_TRACE;bottom.NumDescs=1;
    bottom.DescsLayout=D3D12_ELEMENTS_LAYOUT_ARRAY;bottom.pGeometryDescs=&geometry;
    D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_INPUTS top{};top.Type=D3D12_RAYTRACING_ACCELERATION_STRUCTURE_TYPE_TOP_LEVEL;
    top.Flags=D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BUILD_FLAG_PREFER_FAST_TRACE;top.NumDescs=1;
    top.DescsLayout=D3D12_ELEMENTS_LAYOUT_ARRAY;top.InstanceDescs=upload_va+instance_offset;
    D3D12_RAYTRACING_ACCELERATION_STRUCTURE_PREBUILD_INFO bottom_info{},top_info{};
    s.event("before","GetRaytracingAccelerationStructurePrebuildInfo bottom level");
    device5->GetRaytracingAccelerationStructurePrebuildInfo(&bottom,&bottom_info);
    s.event("after","GetRaytracingAccelerationStructurePrebuildInfo bottom level");
    s.event("before","GetRaytracingAccelerationStructurePrebuildInfo top level");
    device5->GetRaytracingAccelerationStructurePrebuildInfo(&top,&top_info);
    s.event("after","GetRaytracingAccelerationStructurePrebuildInfo top level");
    // A zero or implausibly large answer is refused before any buffer is sized from it.
    const auto sane=[](UINT64 bytes){return bytes && bytes<=(64ull<<20);};
    const bool sized=sane(bottom_info.ResultDataMaxSizeInBytes) && sane(bottom_info.ScratchDataSizeInBytes) &&
                     sane(top_info.ResultDataMaxSizeInBytes) && sane(top_info.ScratchDataSizeInBytes);
    sprintf_s(label,"Prebuild bottom level %llu bytes scratch %llu, top level %llu bytes scratch %llu",
        static_cast<unsigned long long>(bottom_info.ResultDataMaxSizeInBytes),static_cast<unsigned long long>(bottom_info.ScratchDataSizeInBytes),
        static_cast<unsigned long long>(top_info.ResultDataMaxSizeInBytes),static_cast<unsigned long long>(top_info.ScratchDataSizeInBytes));
    s.event("after",label,sized?S_OK:E_FAIL);if(!sized)return E_FAIL;

    // Structures and scratch: DEFAULT with unordered access; one scratch serves both builds in turn.
    heap.Type=D3D12_HEAP_TYPE_DEFAULT;buffer.Flags=D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
    ComPtr<ID3D12Resource> blas,tlas,scratch;
    buffer.Width=bottom_info.ResultDataMaxSizeInBytes;
    hr=s.api("CreateCommittedResource bottom level",[&]{return device->CreateCommittedResource(&heap,D3D12_HEAP_FLAG_NONE,&buffer,D3D12_RESOURCE_STATE_RAYTRACING_ACCELERATION_STRUCTURE,nullptr,IID_PPV_ARGS(&blas));});if(FAILED(hr))return hr;
    s.extra[1]=blas;
    buffer.Width=top_info.ResultDataMaxSizeInBytes;
    hr=s.api("CreateCommittedResource top level",[&]{return device->CreateCommittedResource(&heap,D3D12_HEAP_FLAG_NONE,&buffer,D3D12_RESOURCE_STATE_RAYTRACING_ACCELERATION_STRUCTURE,nullptr,IID_PPV_ARGS(&tlas));});if(FAILED(hr))return hr;
    s.extra[2]=tlas;
    buffer.Width=(std::max)(bottom_info.ScratchDataSizeInBytes,top_info.ScratchDataSizeInBytes);
    hr=s.api("CreateCommittedResource scratch",[&]{return device->CreateCommittedResource(&heap,D3D12_HEAP_FLAG_NONE,&buffer,D3D12_RESOURCE_STATE_UNORDERED_ACCESS,nullptr,IID_PPV_ARGS(&scratch));});if(FAILED(hr))return hr;
    s.extra[3]=scratch;
    buffer.Width=output_bytes;
    hr=s.api("CreateCommittedResource OUTPUT",[&]{return device->CreateCommittedResource(&heap,D3D12_HEAP_FLAG_NONE,&buffer,D3D12_RESOURCE_STATE_UNORDERED_ACCESS,nullptr,IID_PPV_ARGS(&s.middle));});if(FAILED(hr))return hr;
    heap.Type=D3D12_HEAP_TYPE_READBACK;buffer.Flags=D3D12_RESOURCE_FLAG_NONE;
    hr=s.api("CreateCommittedResource READBACK",[&]{return device->CreateCommittedResource(&heap,D3D12_HEAP_FLAG_NONE,&buffer,D3D12_RESOURCE_STATE_COPY_DEST,nullptr,IID_PPV_ARGS(&s.readback));});if(FAILED(hr))return hr;

    s.event("before","GetGPUVirtualAddress bottom level");const D3D12_GPU_VIRTUAL_ADDRESS blas_va=blas->GetGPUVirtualAddress();s.event("after","GetGPUVirtualAddress bottom level");
    constexpr UINT64 alignment=D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BYTE_ALIGNMENT;
    sprintf_s(label,"Bottom level GPU address 0x%016llx",static_cast<unsigned long long>(blas_va));
    s.event("after",label,blas_va && !(blas_va%alignment)?S_OK:E_FAIL);if(!blas_va || blas_va%alignment)return E_FAIL;
    s.event("before","GetGPUVirtualAddress top level, scratch, OUTPUT");
    const D3D12_GPU_VIRTUAL_ADDRESS tlas_va=tlas->GetGPUVirtualAddress(),scratch_va=scratch->GetGPUVirtualAddress(),output_va=s.middle->GetGPUVirtualAddress();
    const bool aligned=tlas_va && scratch_va && output_va && !(tlas_va%alignment) && !(scratch_va%alignment);
    s.event("after","GetGPUVirtualAddress top level, scratch, OUTPUT",aligned?S_OK:E_FAIL);if(!aligned)return E_FAIL;

    void* data=nullptr;const D3D12_RANGE empty{0,0};const D3D12_RANGE whole{0,static_cast<SIZE_T>(output_bytes)};
    hr=s.api("Map UPLOAD",[&]{return s.upload->Map(0,&empty,&data);});if(FAILED(hr))return hr;if(!data)return E_POINTER;
    D3D12_RAYTRACING_INSTANCE_DESC instance{};
    instance.Transform[0][0]=instance.Transform[1][1]=instance.Transform[2][2]=1.0f;
    instance.InstanceMask=0xFF;instance.AccelerationStructure=blas_va;
    std::memcpy(data,triangle,sizeof(triangle));
    std::memcpy(static_cast<unsigned char*>(data)+instance_offset,&instance,sizeof(instance));
    {const D3D12_RANGE written{0,static_cast<SIZE_T>(upload_bytes)};s.event("before","Unmap UPLOAD");s.upload->Unmap(0,&written);s.event("after","Unmap UPLOAD");}
    data=nullptr;hr=s.api("Map READBACK prefill",[&]{return s.readback->Map(0,&empty,&data);});if(FAILED(hr))return hr;if(!data)return E_POINTER;
    for(UINT i=0;i<words;++i)std::memcpy(static_cast<unsigned char*>(data)+i*4,&prefill,4);
    s.event("before","Unmap READBACK prefill");s.readback->Unmap(0,&whole);s.event("after","Unmap READBACK prefill");
    data=nullptr;hr=s.api("Map READBACK before submit",[&]{return s.readback->Map(0,&whole,&data);});if(FAILED(hr))return hr;if(!data)return E_POINTER;
    bool untouched=true;
    for(UINT i=0;i<words;++i){UINT32 word=0;std::memcpy(&word,static_cast<const unsigned char*>(data)+i*4,4);if(word!=prefill){untouched=false;break;}}
    s.event("before","Unmap READBACK before submit");s.readback->Unmap(0,&empty);s.event("after","Unmap READBACK before submit");
    s.event("after","Destination holds prefill before submit",untouched?S_OK:E_FAIL);if(!untouched)return E_FAIL;

    ComPtr<ID3D12RootSignature> root;
    hr=ray_root_signature(s,root);if(FAILED(hr))return hr;
    s.extra[4]=root;
    ComPtr<ID3D12StateObject> state;
    if(grow){
        ComPtr<ID3D12RootSignature> local;
        hr=ray_root_signature(s,local,true);if(FAILED(hr))return hr;
        s.extra[8]=local;
        unsigned char ids[4][record]{};
        hr=ray_grow_state(s,device5.Get(),root.Get(),local.Get(),state,ids);if(FAILED(hr))return hr;
        s.extra[5]=state;
        data=nullptr;hr=s.api("Map shader table",[&]{return table->Map(0,&empty,&data);});if(FAILED(hr))return hr;if(!data)return E_POINTER;
        auto* bytes=static_cast<unsigned char*>(data);
        std::memset(bytes,0,table_bytes);
        std::memcpy(bytes,ids[0],record);                          // raygen, from the parent
        std::memcpy(bytes+table_step,ids[1],record);               // miss index 0, from the parent
        std::memcpy(bytes+table_step+record,ids[3],record);        // miss index 1, from the addition
        std::memcpy(bytes+2*table_step,ids[2],record);             // hit group, from the parent
        std::memcpy(bytes+2*table_step+record,&grow_hit,4);        // its local root constant
    }else{
        if(mode==RayMode::Collection){hr=ray_collection_state(s,device5.Get(),root.Get(),state);if(FAILED(hr))return hr;}
        else{hr=ray_state_object(s,device5.Get(),root.Get(),state);if(FAILED(hr))return hr;}
        s.extra[5]=state;
        ComPtr<ID3D12StateObjectProperties> properties;
        hr=s.api("QueryInterface ID3D12StateObjectProperties",[&]{return state.As(&properties);});if(FAILED(hr))return hr;
        const void* identifiers[3]{};
        hr=ray_identifiers(s,properties.Get(),identifiers);if(FAILED(hr))return hr;
        data=nullptr;hr=s.api("Map shader table",[&]{return table->Map(0,&empty,&data);});if(FAILED(hr))return hr;if(!data)return E_POINTER;
        std::memset(data,0,table_bytes);
        for(int i=0;i<3;++i)std::memcpy(static_cast<unsigned char*>(data)+i*table_step,identifiers[i],record);
    }
    {const D3D12_RANGE written{0,static_cast<SIZE_T>(table_bytes)};s.event("before","Unmap shader table");table->Unmap(0,&written);s.event("after","Unmap shader table");}

    hr=s.api("CreateCommandAllocator",[&]{return device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT,IID_PPV_ARGS(&s.allocator));});if(FAILED(hr))return hr;
    hr=s.api("CreateCommandList",[&]{return device->CreateCommandList(0,D3D12_COMMAND_LIST_TYPE_DIRECT,s.allocator.Get(),nullptr,IID_PPV_ARGS(&s.list));});if(FAILED(hr))return hr;
    ComPtr<ID3D12GraphicsCommandList4> list4;
    hr=s.api("QueryInterface ID3D12GraphicsCommandList4",[&]{return s.list.As(&list4);});if(FAILED(hr))return hr;
    s.extra[6]=list4;
    hr=s.api("CreateFence",[&]{return device->CreateFence(0,D3D12_FENCE_FLAG_NONE,IID_PPV_ARGS(&s.fence));});if(FAILED(hr))return hr;
    ID3D12GraphicsCommandList4* list=list4.Get();
    const auto call=[&s](const char* what,auto&& function){s.event("before",what);function();s.event("after",what);};

    D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_DESC build{};
    build.DestAccelerationStructureData=blas_va;build.Inputs=bottom;build.ScratchAccelerationStructureData=scratch_va;
    call("BuildRaytracingAccelerationStructure bottom level",[&]{list->BuildRaytracingAccelerationStructure(&build,0,nullptr);});
    // No resource: every unordered access, the scratch's reuse and the bottom level before its reader.
    D3D12_RESOURCE_BARRIER uav{};uav.Type=D3D12_RESOURCE_BARRIER_TYPE_UAV;
    call("ResourceBarrier UAV after bottom level",[&]{list->ResourceBarrier(1,&uav);});
    build.DestAccelerationStructureData=tlas_va;build.Inputs=top;
    call("BuildRaytracingAccelerationStructure top level",[&]{list->BuildRaytracingAccelerationStructure(&build,0,nullptr);});
    call("ResourceBarrier UAV after top level",[&]{list->ResourceBarrier(1,&uav);});
    call("SetComputeRootSignature",[&]{list->SetComputeRootSignature(root.Get());});
    // The grow variant binds only the child: this list never saw the parent.
    call(grow?"SetPipelineState1 child":"SetPipelineState1",[&]{list->SetPipelineState1(state.Get());});
    call("SetComputeRootShaderResourceView top level",[&]{list->SetComputeRootShaderResourceView(0,tlas_va);});
    call("SetComputeRootUnorderedAccessView OUTPUT",[&]{list->SetComputeRootUnorderedAccessView(1,output_va);});
    D3D12_DISPATCH_RAYS_DESC rays{};
    rays.RayGenerationShaderRecord={table_va,record};
    if(grow){
        rays.MissShaderTable={table_va+table_step,2*record,record};
        rays.HitGroupTable={table_va+2*table_step,grow_hit_stride,grow_hit_stride};
    }else{
        rays.MissShaderTable={table_va+table_step,record,record};
        rays.HitGroupTable={table_va+2*table_step,record,record};
    }
    rays.Width=grid;rays.Height=grid;rays.Depth=1;
    call("DispatchRays 8 8 1",[&]{list->DispatchRays(&rays);});
    D3D12_RESOURCE_BARRIER barrier{};barrier.Type=D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;barrier.Transition.pResource=s.middle.Get();
    barrier.Transition.Subresource=D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    barrier.Transition.StateBefore=D3D12_RESOURCE_STATE_UNORDERED_ACCESS;barrier.Transition.StateAfter=D3D12_RESOURCE_STATE_COPY_SOURCE;
    call("ResourceBarrier UNORDERED_ACCESS to COPY_SOURCE",[&]{list->ResourceBarrier(1,&barrier);});
    call("CopyBufferRegion OUTPUT to READBACK",[&]{list->CopyBufferRegion(s.readback.Get(),0,s.middle.Get(),0,output_bytes);});
    hr=s.api("Close CommandList",[&]{return list->Close();});if(FAILED(hr))return hr;

    ID3D12CommandList* commands[]={s.list.Get()};s.pending=true;
    s.event("before","ExecuteCommandLists");s.queue->ExecuteCommandLists(1,commands);s.event("after","ExecuteCommandLists");
    hr=s.api("Queue Signal 1",[&]{return s.queue->Signal(s.fence.Get(),1);});if(FAILED(hr))return hr;
    HANDLE completed_event=CreateEventW(nullptr,FALSE,FALSE,nullptr);if(!completed_event)return HRESULT_FROM_WIN32(GetLastError());
    hr=s.api("SetEventOnCompletion 1",[&]{return s.fence->SetEventOnCompletion(1,completed_event);});
    if(SUCCEEDED(hr)){
        ULONGLONG end=(std::min)(s.deadline,GetTickCount64()+5000);DWORD wait=WAIT_TIMEOUT;
        s.event("before","WaitForFence bounded");
        while(GetTickCount64()<end && !s.abort_requested() && wait==WAIT_TIMEOUT)wait=WaitForSingleObject(completed_event,20);
        s.event("after","WaitForFence bounded",wait==WAIT_OBJECT_0?S_OK:HRESULT_FROM_WIN32(WAIT_TIMEOUT));
        s.event("before","GetCompletedValue");UINT64 completed=s.fence->GetCompletedValue();s.event("after","GetCompletedValue",completed>=1 && completed!=UINT64_MAX?S_OK:E_FAIL);
        if(completed==UINT64_MAX)hr=DXGI_ERROR_DEVICE_REMOVED;
        else if(completed<1)hr=HRESULT_FROM_WIN32(WAIT_TIMEOUT);
        else s.pending=false;
    }
    CloseHandle(completed_event);if(FAILED(hr))return hr;

    data=nullptr;hr=s.api("Map READBACK",[&]{return s.readback->Map(0,&whole,&data);});if(FAILED(hr))return hr;if(!data)return E_POINTER;
    UINT equal=0,got=0,got_old=0,got_new=0,first=words;
    for(UINT i=0;i<words;++i){
        UINT32 word=0;std::memcpy(&word,static_cast<const unsigned char*>(data)+i*4,4);
        got+=word==hit_word?1u:0u;got_old+=word==miss?1u:0u;got_new+=word==new_miss?1u:0u;
        if(word==expected[i])++equal;else if(first==words)first=i;
    }
    s.event("before","Unmap READBACK");s.readback->Unmap(0,&empty);s.event("after","Unmap READBACK");
    const int difference=first==words?-1:static_cast<int>(first);
    if(grow)sprintf_s(label,"Ray grow %u of %u words equal, %u hits, %u old misses, %u new misses, first difference %d",equal,words,got,got_old,got_new,difference);
    else sprintf_s(label,"%s %u of %u words equal, %u hits, first difference %d",mode==RayMode::Collection?"Ray collection":"Ray pipeline",equal,words,got,difference);
    const bool exact=equal==words && got && got<words && (!grow || (got_old && got_new));
    s.event("after",label,exact?S_OK:E_FAIL);
    hr=s.api("GetDeviceRemovedReason",[&]{return device->GetDeviceRemovedReason();});if(FAILED(hr))return hr;
    hr=exact?S_OK:E_FAIL;s.copy_success=exact;return hr;
}
inline HRESULT raypipeline(Session& s){return ray_dispatch(s,RayMode::Pipeline);}
inline HRESULT raygrow(Session& s){return ray_dispatch(s,RayMode::Grow);}
inline HRESULT raycollection(Session& s){return ray_dispatch(s,RayMode::Collection);}

// Create/destroy control (build.ps1 -RayState). The state object of raypipeline() twice in turn: create it, query its
// properties, take the three identifiers (32 bytes each, none all zero, no two equal), then release the properties
// and the state object before the next. Nothing reaches the queue, so every object is released inside the verb and
// gpu_pending stays false.
inline HRESULT raystate(Session& s){
    if(!s.queue || s.pending || s.copy_success)return E_UNEXPECTED;
    constexpr UINT passes=2;
    constexpr size_t bytes=D3D12_SHADER_IDENTIFIER_SIZE_IN_BYTES;
    ID3D12Device* device=s.device.Get();
    char label[128]{};
    D3D12_FEATURE_DATA_D3D12_OPTIONS5 options5{};
    HRESULT hr=s.api("CheckFeatureSupport OPTIONS5",[&]{return device->CheckFeatureSupport(D3D12_FEATURE_D3D12_OPTIONS5,&options5,sizeof(options5));});
    sprintf_s(label,"Reported RaytracingTier %u",SUCCEEDED(hr)?static_cast<unsigned>(options5.RaytracingTier):0u);
    s.event("after",label,hr);if(FAILED(hr))return hr;
    if(options5.RaytracingTier<D3D12_RAYTRACING_TIER_1_0){
        s.event("after","RaytracingTier below 1.0, no state object attempted",DXGI_ERROR_UNSUPPORTED);return DXGI_ERROR_UNSUPPORTED;}
    ComPtr<ID3D12Device5> device5;
    hr=s.api("QueryInterface ID3D12Device5",[&]{return s.device.As(&device5);});if(FAILED(hr))return hr;
    ComPtr<ID3D12RootSignature> root;
    hr=ray_root_signature(s,root);if(FAILED(hr))return hr;
    UINT done=0;
    for(UINT pass=1;pass<=passes;++pass){
        ComPtr<ID3D12StateObject> state;
        hr=ray_state_object(s,device5.Get(),root.Get(),state);if(FAILED(hr))return hr;
        ComPtr<ID3D12StateObjectProperties> properties;
        hr=s.api("QueryInterface ID3D12StateObjectProperties",[&]{return state.As(&properties);});if(FAILED(hr))return hr;
        const void* identifiers[3]{};
        hr=ray_identifiers(s,properties.Get(),identifiers);if(FAILED(hr))return hr;
        bool distinct=true;UINT zero=0;
        for(int i=0;i<3;++i){
            const auto* id=static_cast<const unsigned char*>(identifiers[i]);
            zero+=std::all_of(id,id+bytes,[](unsigned char b){return b==0;})?1u:0u;
            for(int j=0;j<i;++j)if(!std::memcmp(identifiers[i],identifiers[j],bytes))distinct=false;
        }
        sprintf_s(label,"Pass %u identifiers %u of 3 nonzero, %s",pass,3-zero,distinct?"all different":"two equal");
        s.event("after",label,!zero && distinct?S_OK:E_FAIL);if(zero || !distinct)return E_FAIL;
        // The remaining reference counts are observations; the runtime owns them.
        s.event("before","Release ID3D12StateObjectProperties");const ULONG kept=properties.Detach()->Release();s.event("after","Release ID3D12StateObjectProperties");
        sprintf_s(label,"Pass %u properties released, %lu references left",pass,static_cast<unsigned long>(kept));s.event("after",label);
        s.event("before","Release ID3D12StateObject");const ULONG left=state.Detach()->Release();s.event("after","Release ID3D12StateObject");
        sprintf_s(label,"Pass %u state object released, %lu references left",pass,static_cast<unsigned long>(left));s.event("after",label);
        ++done;
    }
    hr=s.api("GetDeviceRemovedReason",[&]{return device->GetDeviceRemovedReason();});if(FAILED(hr))return hr;
    sprintf_s(label,"Ray state %u of %u state objects created and released",done,passes);
    s.event("after",label,done==passes?S_OK:E_FAIL);
    s.copy_success=done==passes;return done==passes?S_OK:E_FAIL;
}
