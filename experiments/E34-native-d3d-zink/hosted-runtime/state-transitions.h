static void state_transitions(ID3D11Device* dev, ID3D11DeviceContext* ctx, bool flushEachDraw) {
   printf("flush_each_draw=%u\n", flushEachDraw ? 1u : 0u);
   const char source[] = R"(
cbuffer Params : register(b0) { float4 transform; float4 tint; };
struct V { float4 p:SV_Position; float4 color:COLOR; };
V VS(float2 p:POSITION, float4 color:COLOR) {
   V o; o.p=float4(p*transform.xy+transform.zw,.5,1); o.color=color*tint; return o;
}
V Instance(float2 p:POSITION,float4 t:TEXCOORD,float4 color:COLOR) {
   V o; o.p=float4(p*t.xy+t.zw,.5,1); o.color=color; return o;
}
float4 PS(V i):SV_Target { return i.color; }
)";
   auto shader=[&](const char* entry,const char* profile) {
      ComPtr<ID3DBlob> b,error;
      HRESULT hr=D3DCompile(source,sizeof(source)-1,"state-transitions",nullptr,nullptr,entry,profile,D3DCOMPILE_ENABLE_STRICTNESS,0,&b,&error);
      if(error)printf("state compile: %s\n",static_cast<const char*>(error->GetBufferPointer()));
      check(hr,"state shader compile");return b;
   };
   auto vc=shader("VS","vs_4_0"),ic=shader("Instance","vs_4_0"),pc=shader("PS","ps_4_0");
   ComPtr<ID3D11VertexShader> vs,ivs;ComPtr<ID3D11PixelShader> ps;
   check(dev->CreateVertexShader(vc->GetBufferPointer(),vc->GetBufferSize(),nullptr,&vs),"state VS");
   check(dev->CreateVertexShader(ic->GetBufferPointer(),ic->GetBufferSize(),nullptr,&ivs),"state instance VS");
   check(dev->CreatePixelShader(pc->GetBufferPointer(),pc->GetBufferSize(),nullptr,&ps),"state PS");
   D3D11_INPUT_ELEMENT_DESC packed[]={{"POSITION",0,DXGI_FORMAT_R32G32_FLOAT,0,0,D3D11_INPUT_PER_VERTEX_DATA,0},{"COLOR",0,DXGI_FORMAT_R32G32B32A32_FLOAT,0,8,D3D11_INPUT_PER_VERTEX_DATA,0}};
   D3D11_INPUT_ELEMENT_DESC split[]={{"POSITION",0,DXGI_FORMAT_R32G32_FLOAT,0,8,D3D11_INPUT_PER_VERTEX_DATA,0},{"COLOR",0,DXGI_FORMAT_R32G32B32A32_FLOAT,3,16,D3D11_INPUT_PER_VERTEX_DATA,0}};
   D3D11_INPUT_ELEMENT_DESC instance[]={{"POSITION",0,DXGI_FORMAT_R32G32_FLOAT,0,0,D3D11_INPUT_PER_VERTEX_DATA,0},{"TEXCOORD",0,DXGI_FORMAT_R32G32B32A32_FLOAT,3,0,D3D11_INPUT_PER_INSTANCE_DATA,1},{"COLOR",0,DXGI_FORMAT_R32G32B32A32_FLOAT,3,16,D3D11_INPUT_PER_INSTANCE_DATA,1}};
   ComPtr<ID3D11InputLayout> layouts[3];
   check(dev->CreateInputLayout(packed,2,vc->GetBufferPointer(),vc->GetBufferSize(),&layouts[0]),"packed layout");
   check(dev->CreateInputLayout(split,2,vc->GetBufferPointer(),vc->GetBufferSize(),&layouts[1]),"split layout");
   check(dev->CreateInputLayout(instance,3,ic->GetBufferPointer(),ic->GetBufferSize(),&layouts[2]),"instance layout");
   auto buffer=[&](UINT size,UINT bind,bool dynamic) {
      D3D11_BUFFER_DESC d={};d.ByteWidth=size;d.BindFlags=bind;d.Usage=dynamic?D3D11_USAGE_DYNAMIC:D3D11_USAGE_DEFAULT;d.CPUAccessFlags=dynamic?D3D11_CPU_ACCESS_WRITE:0;
      ComPtr<ID3D11Buffer> b;check(dev->CreateBuffer(&d,nullptr,&b),"state buffer");return b;
   };
   auto vb=buffer(64*256,D3D11_BIND_VERTEX_BUFFER,true),colorBuffer=buffer(65*64,D3D11_BIND_VERTEX_BUFFER,true),ib=buffer(64*32,D3D11_BIND_INDEX_BUFFER,true);
   auto cb=buffer(32,D3D11_BIND_CONSTANT_BUFFER,false),dynamicCb=buffer(32,D3D11_BIND_CONSTANT_BUFFER,true);
   auto write=[&](ID3D11Buffer* b,D3D11_MAP mode,UINT offset,const void* data,UINT bytes) {
      D3D11_MAPPED_SUBRESOURCE m={};check(ctx->Map(b,0,mode,0,&m),"state buffer Map");memcpy(static_cast<unsigned char*>(m.pData)+offset,data,bytes);ctx->Unmap(b,0);
   };
   D3D11_TEXTURE2D_DESC td={};td.Width=td.Height=64;td.MipLevels=td.ArraySize=1;td.Format=DXGI_FORMAT_R8G8B8A8_UNORM;td.SampleDesc.Count=1;td.BindFlags=D3D11_BIND_RENDER_TARGET;
   ComPtr<ID3D11Texture2D> target,snap[4];ComPtr<ID3D11RenderTargetView> rtv;
   check(dev->CreateTexture2D(&td,nullptr,&target),"state target");check(dev->CreateRenderTargetView(target.Get(),nullptr,&rtv),"state RTV");
   td.BindFlags=0;td.Usage=D3D11_USAGE_STAGING;td.CPUAccessFlags=D3D11_CPU_ACCESS_READ;
   for(auto& t:snap)check(dev->CreateTexture2D(&td,nullptr,&t),"state snapshot");
   D3D11_RASTERIZER_DESC rd={};rd.FillMode=D3D11_FILL_SOLID;rd.CullMode=D3D11_CULL_NONE;rd.DepthClipEnable=TRUE;
   ComPtr<ID3D11RasterizerState> raster;check(dev->CreateRasterizerState(&rd,&raster),"state raster");
   const char* names[]={"viewport-cb-update","streams-layout-stride","vb-nooverwrite","vs-cb-discard","indexed-strip-nooverwrite","instance-stream-base","indexed-list16-nooverwrite","indexed-list16-discard"};
   for(UINT mode=0;mode<8;++mode) {
      printf("state-begin=%s\n",names[mode]);ctx->ClearState();ctx->RSSetState(raster.Get());
      ctx->VSSetShader(mode==5?ivs.Get():vs.Get(),nullptr,0);ctx->PSSetShader(ps.Get(),nullptr,0);ctx->OMSetRenderTargets(1,rtv.GetAddressOf(),nullptr);
      ID3D11Buffer* constants=mode==3?dynamicCb.Get():cb.Get();ctx->VSSetConstantBuffers(0,1,&constants);
      for(UINT pass=0;pass<4;++pass) {
         for(UINT n=0;n<64;++n) {
            const UINT tile=(n*17+pass*11)%64,x=tile%8,y=tile/8;
            float color[4]={float(x*32)/255,float(y*32)/255,float(64+pass*32)/255,1};
            float parameters[8]={.125f,.125f,-.875f+float(x)*.25f,.875f-float(y)*.25f,1,1,1,1};
            if(mode==0||mode==1||mode==5){parameters[0]=parameters[1]=1;parameters[2]=parameters[3]=0;}
            if(mode==3)write(dynamicCb.Get(),D3D11_MAP_WRITE_DISCARD,0,parameters,sizeof(parameters));
            else ctx->UpdateSubresource(cb.Get(),0,nullptr,parameters,0,0);
            D3D11_VIEWPORT vp={0,0,64,64,0,1};if(mode==0){vp.TopLeftX=float(x*8);vp.TopLeftY=float(y*8);vp.Width=vp.Height=8;}
            ctx->RSSetViewports(1,&vp);
            const float positions[6][2]={{-1,1},{1,1},{-1,-1},{-1,-1},{1,1},{1,-1}};
            float data[64]={},colors[64]={};UINT stride=24,offset=0,layout=0;
            const bool splitLayout=mode==1&&(n%2!=0),strip=mode==4||mode==5||mode==6||mode==7;
            const UINT vertices=strip?4:6,first=(mode==4||mode==6||mode==7)?3:0;
            if(splitLayout){stride=32;layout=1;offset=16;}
            if(mode==2||mode==4||mode==6||mode==7)offset=n*256;
            for(UINT v=0;v<vertices;++v) {
               UINT index=strip&&v==3?5:v;float px=positions[index][0],py=positions[index][1];
               if(mode==1){px=px*.125f-.875f+float(x)*.25f;py=py*.125f+.875f-float(y)*.25f;}
               UINT at=(v+first)*(stride/4)+(splitLayout?6:0);
               data[at]=px;data[at+1]=py;
               if(splitLayout)memcpy(colors+v*8+4,color,sizeof(color));else memcpy(data+at+2,color,sizeof(color));
            }
            D3D11_MAP map=(mode==2||mode==4||mode==6||mode==7)&&n?D3D11_MAP_WRITE_NO_OVERWRITE:D3D11_MAP_WRITE_DISCARD;
            write(vb.Get(),map,(mode==2||mode==4||mode==6||mode==7)?offset:0,data,sizeof(data));
            ctx->IASetVertexBuffers(0,1,vb.GetAddressOf(),&stride,&offset);
            if(splitLayout){UINT cs=32,co=0;write(colorBuffer.Get(),D3D11_MAP_WRITE_DISCARD,0,colors,sizeof(colors));ctx->IASetVertexBuffers(3,1,colorBuffer.GetAddressOf(),&cs,&co);}
            if(mode==5) {
               float inst[8]={.125f,.125f,-.875f+float(x)*.25f,.875f-float(y)*.25f,color[0],color[1],color[2],color[3]};
               write(colorBuffer.Get(),n?D3D11_MAP_WRITE_NO_OVERWRITE:D3D11_MAP_WRITE_DISCARD,(n+1)*32,inst,sizeof(inst));
               UINT cs=32,co=0;ctx->IASetVertexBuffers(3,1,colorBuffer.GetAddressOf(),&cs,&co);layout=2;
            }
            ctx->IASetInputLayout(layouts[layout].Get());ctx->IASetPrimitiveTopology(strip&&mode!=6&&mode!=7?D3D11_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP:D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
            if(mode==4) {
               UINT indices[8]={777,888,0,1,2,3,0xffffffff,0xffffffff};
               write(ib.Get(),n?D3D11_MAP_WRITE_NO_OVERWRITE:D3D11_MAP_WRITE_DISCARD,n*32,indices,sizeof(indices));
               ctx->IASetIndexBuffer(ib.Get(),DXGI_FORMAT_R32_UINT,0);ctx->DrawIndexed(4,n*8+2,3);
            } else if(mode==6) {
               unsigned short indices[16]={777,888,0,1,2,2,1,3,0xffff,0xffff,0xffff,0xffff,0xffff,0xffff,0xffff,0xffff};
               write(ib.Get(),n?D3D11_MAP_WRITE_NO_OVERWRITE:D3D11_MAP_WRITE_DISCARD,n*32,indices,sizeof(indices));
               ctx->IASetIndexBuffer(ib.Get(),DXGI_FORMAT_R16_UINT,4);ctx->DrawIndexed(6,n*16,3);
            } else if(mode==7) {
               // Every new backing store is fully initialized. Stale backing
               // contains degenerate indices at this draw's current offset.
               unsigned short indices[1024]={};
               unsigned short current[8]={777,888,0,1,2,2,1,3};
               memcpy(indices+n*16,current,sizeof(current));
               write(ib.Get(),D3D11_MAP_WRITE_DISCARD,0,indices,sizeof(indices));
               ctx->IASetIndexBuffer(ib.Get(),DXGI_FORMAT_R16_UINT,4);ctx->DrawIndexed(6,n*16,3);
            } else if(mode==5)ctx->DrawInstanced(4,1,0,n+1);
            else ctx->Draw(vertices,0);
            if (flushEachDraw) ctx->Flush();
         }
         ctx->CopyResource(snap[pass].Get(),target.Get());
      }
      D3D11_QUERY_DESC qd={D3D11_QUERY_EVENT,0};ComPtr<ID3D11Query> query;check(dev->CreateQuery(&qd,&query),"state query");
      ctx->End(query.Get());ctx->Flush();ULONGLONG deadline=GetTickCount64()+10000;
      for(;;){BOOL ready=FALSE;HRESULT hr=ctx->GetData(query.Get(),&ready,sizeof(ready),0);check(hr,"state GetData");if(hr==S_OK&&ready)break;require(GetTickCount64()<deadline,"state query timeout");Sleep(1);}
      for(UINT pass=0;pass<4;++pass) {
         D3D11_MAPPED_SUBRESOURCE m={};check(ctx->Map(snap[pass].Get(),0,D3D11_MAP_READ,0,&m),"state snapshot Map");UINT bad=0;uint64_t hash=14695981039346656037ull;
         for(UINT y=0;y<64;++y)for(UINT x=0;x<64;++x) {
            unsigned expected[4]={(x/8)*32,(y/8)*32,64+pass*32,255};auto pixel=static_cast<const unsigned char*>(m.pData)+y*m.RowPitch+x*4;bool mismatch=false;
            for(UINT c=0;c<4;++c){mismatch|=pixel[c]!=expected[c];hash=(hash^pixel[c])*1099511628211ull;}
            if(mismatch&&bad++==0)printf("state mismatch %u,%u actual=%u,%u,%u,%u expected=%u,%u,%u,%u\n",x,y,pixel[0],pixel[1],pixel[2],pixel[3],expected[0],expected[1],expected[2],expected[3]);
         }
         ctx->Unmap(snap[pass].Get(),0);printf("state=%s pass=%u mismatches=%u/4096 fnv64=%016llx\n",names[mode],pass,bad,static_cast<unsigned long long>(hash));require(!bad,"state-transition pixels");
      }
   }
   ctx->ClearState();ctx->Flush();printf("PASS state transitions: 2048 draws, 131072 pixels\n");
}
