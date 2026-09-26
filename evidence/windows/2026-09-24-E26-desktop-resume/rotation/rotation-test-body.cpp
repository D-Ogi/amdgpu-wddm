int main(){
 int failures=0;
 for (unsigned n : {2u,3u}) {
  pipe_context pipe={flush,create,release,fb,samplers};
  pipe_resource storage[3]{};Resource resources[3]{};Resource* handles[3];
  RenderTargetView rtv[3]{};ShaderResourceView srv[3]{};
  Device d{};d.pipe=&pipe;d.fb.nr_cbufs=n;
  for(unsigned i=0;i<n;i++) {
   storage[i].id=i;resources[i]={reinterpret_cast<void*>(uintptr_t(100+i)),20+i,200+i,4096,true,reinterpret_cast<void*>(uintptr_t(300+i)),&storage[i]};handles[i]=&resources[i];
   rtv[i]={i+1<n?&rtv[i+1]:nullptr,{&storage[i]}};
   srv[i]={i+1<n?&srv[i+1]:nullptr,new pipe_sampler_view{&storage[i],int(70+i)}};
   d.fb.cbufs[i].texture=&storage[i];d.sampler_views[1][i]=srv[i].handle;
  }
  d.renderTargetViews=rtv;d.shaderResourceViews=srv;
  DXGI_DDI_ARG_ROTATE_RESOURCE_IDENTITIES a{&d,n,handles};
  for(unsigned turn=1;turn<=6;turn++) {
   if(_RotateResourceIdentities(&a)!=S_OK)failures++;
   for(unsigned i=0;i<n;i++) {
    unsigned expected=(i+turn)%n;
    if(resources[i].resource!=&storage[expected] || resources[i].allocation!=20+expected || resources[i].gpuVa!=200+expected || resources[i].cpuMapping!=reinterpret_cast<void*>(uintptr_t(300+expected)) || resources[i].hRTResource!=reinterpret_cast<void*>(uintptr_t(100+i)))failures++;
    if(rtv[i].surface.texture!=&storage[expected] || d.fb.cbufs[i].texture!=&storage[expected] || srv[i].handle->texture!=&storage[expected] || srv[i].handle->descriptor!=70+int(i) || d.sampler_views[1][i]!=srv[i].handle)failures++;
   }
  }
  for(unsigned i=0;i<n;i++)delete srv[i].handle;
 }
 if(flushes!=12||framebuffer_updates!=12||sampler_updates!=36)failures++;
 printf("2/3 buffer identity, stable runtime handles, retained RTV/SRV and bound views: failures=%d\n",failures);
 return failures?1:0;
}
