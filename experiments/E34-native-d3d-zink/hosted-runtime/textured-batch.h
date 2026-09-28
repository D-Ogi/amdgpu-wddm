static void textured_batch(ID3D11Device* dev, ID3D11DeviceContext* ctx, bool separate) {
   const char source[]=R"(
struct V { float4 p:SV_Position; float2 uv:TEXCOORD; };
V VS(float2 p:POSITION,float2 uv:TEXCOORD) { V o;o.p=float4(p,.5,1);o.uv=uv;return o; }
Texture2D<float4> a:register(t0); Texture2D<float4> b:register(t1); SamplerState s:register(s0);
float4 Combined(V i):SV_Target { return a.Sample(s,i.uv); }
float4 Separate(V i):SV_Target { return b.Sample(s,i.uv); }
)";
   auto compileStage=[&](const char* entry,const char* profile) {
      ComPtr<ID3DBlob> code,error;
      HRESULT hr=D3DCompile(source,sizeof(source)-1,"textured-batch",nullptr,nullptr,entry,profile,D3DCOMPILE_ENABLE_STRICTNESS,0,&code,&error);
      if(error)printf("texture compile: %s\n",static_cast<const char*>(error->GetBufferPointer()));
      check(hr,"texture compile");return code;
   };
   auto vc=compileStage("VS","vs_4_0"),pc=compileStage(separate?"Separate":"Combined","ps_4_0");
   ComPtr<ID3D11VertexShader> vs;ComPtr<ID3D11PixelShader> ps;
   check(dev->CreateVertexShader(vc->GetBufferPointer(),vc->GetBufferSize(),nullptr,&vs),"texture VS");
   check(dev->CreatePixelShader(pc->GetBufferPointer(),pc->GetBufferSize(),nullptr,&ps),"texture PS");
   D3D11_INPUT_ELEMENT_DESC elements[]={{"POSITION",0,DXGI_FORMAT_R32G32_FLOAT,0,0,D3D11_INPUT_PER_VERTEX_DATA,0},{"TEXCOORD",0,DXGI_FORMAT_R32G32_FLOAT,0,8,D3D11_INPUT_PER_VERTEX_DATA,0}};
   ComPtr<ID3D11InputLayout> layout;check(dev->CreateInputLayout(elements,2,vc->GetBufferPointer(),vc->GetBufferSize(),&layout),"texture layout");
   D3D11_BUFFER_DESC bd={};bd.ByteWidth=96;bd.Usage=D3D11_USAGE_DYNAMIC;bd.BindFlags=D3D11_BIND_VERTEX_BUFFER;bd.CPUAccessFlags=D3D11_CPU_ACCESS_WRITE;
   ComPtr<ID3D11Buffer> vertices;check(dev->CreateBuffer(&bd,nullptr,&vertices),"texture VB");
   D3D11_TEXTURE2D_DESC td={};td.Width=td.Height=8;td.MipLevels=td.ArraySize=1;td.Format=DXGI_FORMAT_R8G8B8A8_UNORM;td.SampleDesc.Count=1;td.BindFlags=D3D11_BIND_SHADER_RESOURCE;td.Usage=D3D11_USAGE_IMMUTABLE;
   ComPtr<ID3D11Texture2D> texture[2];ComPtr<ID3D11ShaderResourceView> view[2];ComPtr<ID3D11SamplerState> sampler[2];
   for(UINT i=0;i<2;++i){
      unsigned char bytes[256];for(UINT y=0;y<8;++y)for(UINT x=0;x<8;++x){UINT o=(y*8+x)*4;bytes[o]=static_cast<unsigned char>(x*32);bytes[o+1]=static_cast<unsigned char>(y*32);bytes[o+2]=static_cast<unsigned char>(64+i*128);bytes[o+3]=255;}
      D3D11_SUBRESOURCE_DATA init={};init.pSysMem=bytes;init.SysMemPitch=32;
      check(dev->CreateTexture2D(&td,&init,&texture[i]),"sample texture");check(dev->CreateShaderResourceView(texture[i].Get(),nullptr,&view[i]),"sample view");
      D3D11_SAMPLER_DESC sd={};sd.Filter=D3D11_FILTER_MIN_MAG_MIP_POINT;sd.AddressU=sd.AddressV=sd.AddressW=i?D3D11_TEXTURE_ADDRESS_WRAP:D3D11_TEXTURE_ADDRESS_CLAMP;sd.MaxLOD=D3D11_FLOAT32_MAX;sd.ComparisonFunc=D3D11_COMPARISON_NEVER;
      check(dev->CreateSamplerState(&sd,&sampler[i]),"sample sampler");
   }
   td.Width=td.Height=64;td.Usage=D3D11_USAGE_DEFAULT;td.BindFlags=D3D11_BIND_RENDER_TARGET;
   ComPtr<ID3D11Texture2D> target,snapshot[4];ComPtr<ID3D11RenderTargetView> rtv;
   check(dev->CreateTexture2D(&td,nullptr,&target),"texture target");check(dev->CreateRenderTargetView(target.Get(),nullptr,&rtv),"texture RTV");
   td.Usage=D3D11_USAGE_STAGING;td.BindFlags=0;td.CPUAccessFlags=D3D11_CPU_ACCESS_READ;
   for(auto& image:snapshot)check(dev->CreateTexture2D(&td,nullptr,&image),"snapshot");
   D3D11_RASTERIZER_DESC rd={};rd.FillMode=D3D11_FILL_SOLID;rd.CullMode=D3D11_CULL_NONE;rd.DepthClipEnable=TRUE;
   ComPtr<ID3D11RasterizerState> raster;check(dev->CreateRasterizerState(&rd,&raster),"texture raster");
   ctx->ClearState();ctx->RSSetState(raster.Get());D3D11_VIEWPORT vp={0,0,64,64,0,1};ctx->RSSetViewports(1,&vp);
   ctx->IASetInputLayout(layout.Get());ctx->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);UINT stride=16,offset=0;ctx->IASetVertexBuffers(0,1,vertices.GetAddressOf(),&stride,&offset);
   ctx->VSSetShader(vs.Get(),nullptr,0);ctx->PSSetShader(ps.Get(),nullptr,0);ctx->OMSetRenderTargets(1,rtv.GetAddressOf(),nullptr);
   for(UINT pass=0;pass<4;++pass){
      for(UINT n=0;n<64;++n){
         UINT tile=(n*17+pass*11)%64,x=tile%8,y=tile/8,t=(tile+pass)%2,s=(tile/2+pass)%2;
         float l=float(x)/4-1,r=float(x+1)/4-1,top=1-float(y)/4,bottom=1-float(y+1)/4;
         float data[24]={l,top,-.5f,-.5f,r,top,.5f,-.5f,l,bottom,-.5f,.5f,l,bottom,-.5f,.5f,r,top,.5f,-.5f,r,bottom,.5f,.5f};
         D3D11_MAPPED_SUBRESOURCE mapped={};check(ctx->Map(vertices.Get(),0,D3D11_MAP_WRITE_DISCARD,0,&mapped),"texture Map DISCARD");memcpy(mapped.pData,data,sizeof(data));ctx->Unmap(vertices.Get(),0);
         ID3D11ShaderResourceView* slots[2]={view[separate?1-t:t].Get(),view[separate?t:1-t].Get()};
         ctx->PSSetShaderResources(0,2,slots);ctx->PSSetSamplers(0,1,sampler[s].GetAddressOf());ctx->Draw(6,0);
      }
      ctx->CopyResource(snapshot[pass].Get(),target.Get());
   }
   D3D11_QUERY_DESC qd={D3D11_QUERY_EVENT,0};ComPtr<ID3D11Query> query;check(dev->CreateQuery(&qd,&query),"texture query");ctx->End(query.Get());ctx->Flush();ULONGLONG deadline=GetTickCount64()+10000;
   for(;;){BOOL ready=FALSE;HRESULT hr=ctx->GetData(query.Get(),&ready,sizeof(ready),0);check(hr,"texture GetData");if(hr==S_OK&&ready)break;require(GetTickCount64()<deadline,"texture GPU timeout");Sleep(1);}
   for(UINT pass=0;pass<4;++pass){
      D3D11_MAPPED_SUBRESOURCE m={};check(ctx->Map(snapshot[pass].Get(),0,D3D11_MAP_READ,0,&m),"snapshot Map");unsigned bad=0;uint64_t hash=14695981039346656037ull;
      for(UINT y=0;y<64;++y)for(UINT x=0;x<64;++x){UINT tile=(y/8)*8+x/8,t=(tile+pass)%2,s=(tile/2+pass)%2;int u=int(x%8)-4,v=int(y%8)-4;
         u=s?(u+8)%8:(u<0?0:u);v=s?(v+8)%8:(v<0?0:v);unsigned expected[4]={unsigned(u*32),unsigned(v*32),64+t*128,255};
         auto pixel=static_cast<const unsigned char*>(m.pData)+y*m.RowPitch+x*4;bool mismatch=false;for(UINT c=0;c<4;++c){if(pixel[c]!=expected[c])mismatch=true;hash=(hash^pixel[c])*1099511628211ull;}
         if(mismatch&&bad++==0)printf("texture mismatch %u,%u actual=%u,%u,%u,%u expected=%u,%u,%u,%u\n",x,y,pixel[0],pixel[1],pixel[2],pixel[3],expected[0],expected[1],expected[2],expected[3]);
      }
      ctx->Unmap(snapshot[pass].Get(),0);printf("textured=%s pass=%u mismatches=%u/4096 fnv64=%016llx\n",separate?"t1-s0":"t0-s0",pass,bad,static_cast<unsigned long long>(hash));require(bad==0,"textured batch pixels");
   }
   ctx->ClearState();ctx->Flush();
}
