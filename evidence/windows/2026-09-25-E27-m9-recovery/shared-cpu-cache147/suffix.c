static void surface_case(ULONG magic,ULONG version,ULONG shared,ULONG access,UINT bytes,UINT reserved)
{
 BC250_DEVICE device={0};ULONG resource[5]={magic,version,shared,access,0};
 BC250_WDDM_ALLOCATION_PRIVATE data={0x4137424Cul,1,64,32,256,D3DDDIFMT_A8R8G8B8,8192};
 DXGK_ALLOCATIONINFO info={0},before;DXGKARG_CREATEALLOCATION create={0};NTSTATUS status;
 BOOLEAN recognized=(BOOLEAN)(bytes>=4 && magic==0x52363245ul);
 BOOLEAN valid=(BOOLEAN)(!recognized || (shared<=1 && ((version==1 && bytes==12) || (version==2 && bytes==16 && access<4))));
 BOOLEAN aperture=(BOOLEAN)(recognized && valid && shared==1);
 BOOLEAN cached=(BOOLEAN)(aperture && version==2 && (access&2) && !(access&1));
 info.FlagsWddm2.Value=reserved; info.pPrivateDriverData=&data;info.PrivateDriverDataSize=sizeof(data);before=info;
 create.pPrivateDriverData=resource;create.PrivateDriverDataSize=bytes;create.NumAllocations=1;create.pAllocationInfo=&info;
 created=0;status=Bc250WddmCreateAllocation(&device,&create);
 CHECK(status==(valid?STATUS_SUCCESS:STATUS_INVALID_PARAMETER));
 if(valid){
  CHECK(created==1);CHECK(info.FlagsWddm2.Cached==cached);CHECK(info.FlagsWddm2.CpuVisible==1);
  CHECK((info.FlagsWddm2.Value&reserved)==reserved);CHECK(info.FlagsWddm2.AccessedPhysically==(UINT)!aperture);
  CHECK(info.PreferredSegment.SegmentId0==(aperture?2u:1u));CHECK(info.SupportedReadSegmentSet==(aperture?2u:1u));
  CHECK(info.SupportedWriteSegmentSet==info.SupportedReadSegmentSet);CHECK(info.Size==8192);
  WddmFreeObject(info.hAllocation);
 }else{CHECK(created==0);CHECK(memcmp(&before,&info,sizeof(info))==0);CHECK(create.hResource==NULL);}
}
static void bc2a(void)
{
 ULONG version,heap,flags;
 for(version=1;version<=2;version++)for(heap=2;heap<=4;heap+=2)for(flags=0;flags<8;flags++){
  BC250_DEVICE device={0};ULONGLONG raw[24]={0};ULONG *d=(ULONG*)raw;
  DXGK_ALLOCATIONINFO info={0};DXGKARG_CREATEALLOCATION create={0};ULONG otherResource[4]={0xdeadbeef,2,1,2};
  d[0]=UMD_BLOB_ALLOC_MAGIC;d[1]=version;d[2]=sizeof(raw);raw[2]=8192;raw[3]=4096;d[8]=heap;raw[5]=flags;
  info.pPrivateDriverData=raw;info.PrivateDriverDataSize=sizeof(raw);info.FlagsWddm2.Reserved0=1;
  create.NumAllocations=1;create.pAllocationInfo=&info;create.pPrivateDriverData=otherResource;create.PrivateDriverDataSize=sizeof(otherResource);
  CHECK(Bc250WddmCreateAllocation(&device,&create)==STATUS_SUCCESS);
  CHECK(info.FlagsWddm2.Cached==(UINT)(version==2 && heap==2 && !(flags&6)));
  CHECK(info.FlagsWddm2.Reserved0==1);CHECK(info.PreferredSegment.SegmentId0==(heap==2?2u:1u));
  WddmFreeObject(info.hAllocation);
 }
}
int main(void)
{
 UINT version,shared,access,bytes,r;DXGK_ALLOCATIONINFOFLAGS_WDDM2_0 flag={0};UINT reserved[3];
 reserved[0]=0;flag.Reserved0=1;reserved[1]=flag.Value;flag.DXGK_ALLOC_RESERVED16=1;flag.DXGK_ALLOC_RESERVED17=1;reserved[2]=flag.Value;
 C_ASSERT(sizeof(BC250_WDDM_ALLOCATION_PRIVATE)==32);
 for(r=0;r<3;r++)for(version=0;version<4;version++)for(shared=0;shared<3;shared++)for(access=0;access<8;access++)for(bytes=0;bytes<=20;bytes++)
  surface_case(0x52363245,version,shared,access,bytes,reserved[r]);
 surface_case(0xdeadbeef,2,1,2,16,reserved[2]);surface_case(0,0,0,0,0,reserved[2]);
 bc2a();printf("Actual CreateAllocation: %u checks, %u failures\n",checks,failures);return failures?1:0;
}
