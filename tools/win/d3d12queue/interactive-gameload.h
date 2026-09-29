// SPDX-License-Identifier: MIT
// Game load variant of the interactive client (build.ps1 -GameLoad). Included by interactive.h inside namespace
// interactive, after Session. The "copy" verb then runs the load, so commands and receipts keep their form.
//
// A game stopped the whole lab machine within a second of loading the UMD, during a burst of kernel paging
// submissions. This load grows in steps so that the step at which a machine stops is known. A step has a fixed
// total of bytes (256 MB, 512 MB, 1 GB, 2 GB) and two arms with that total: SMALL, committed buffers of 64 KB
// created back to back, and LARGE, committed resources of 64 MB, buffers and 2D textures with mips in turn. Each
// resource is written by the GPU from an UPLOAD staging buffer with a pattern of the run's seed and the resource's
// id, in command lists of at most 1024 copies; after the fence a sample is read back and compared: every LARGE
// resource, every 64th SMALL one and the last. Allocation, submission, fence and content are counted apart, and
// bytes touched apart from the number of objects. An arm's resources live until its verification is done and are
// released before the next arm starts, followed by a fence round trip. A last phase runs four threads, each with
// its own allocator, list, fence, staging and seed, on the SMALL arm of the 256 MB step, all on the one queue.
//
// While the load runs, a render thread shows it on the screen as a game would: a borderless window over the
// primary output, a flip-model swap chain on the same queue (the configuration the -Present variant proved:
// FLIP_DISCARD, two buffers, the admitted 8-bit format, Present with sync interval 1) and one frame per Present,
// drawn with clears only so that nothing in it depends on the back buffer format. The frame holds a band in the
// colour of the current step and arm, a bar that moves with every frame (a frozen picture keeps it still), and
// one tile per written batch: yellow when its fence completed, green when its arm verified clean, red for a
// mismatch or a failed allocation. The render thread has its own allocators, list and fence. Under
// --interactive-warp no window is made: the same frames go to an offscreen render target of the same size.
//
// milestones.log in the session directory gets one line per milestone, written through to the disk before the
// next step, so that a machine-wide stop leaves the last one readable; each line carries the Present count and
// the longest interval between two Presents so far. trace.jsonl keeps its form; records of a worker thread carry
// a "thread" field (0 to 3 the load threads, 4 the render thread).
namespace gameload {
constexpr UINT64 MiB=1ull<<20;
constexpr UINT64 steps[]={256*MiB,512*MiB,1024*MiB,2048*MiB};
constexpr unsigned step_count=sizeof(steps)/sizeof(steps[0]);
constexpr UINT64 large_size=64*MiB,small_size=64*1024;
// One LARGE texture footprint (64 MB with row and placement padding) or 1024 SMALL buffers per batch.
constexpr UINT64 staging_size=65*MiB;constexpr UINT batch_limit=1024;
constexpr unsigned threads=4;constexpr UINT64 thread_staging=16*MiB;constexpr UINT thread_batch_limit=256;
// RGBA8 4096x3072 with its full chain: 48 MB at mip 0, 64 MB in all.
constexpr UINT texture_width=4096,texture_height=3072;constexpr UINT16 texture_mips=13;
constexpr UINT64 small_sample=64;
constexpr UINT64 placement=D3D12_TEXTURE_DATA_PLACEMENT_ALIGNMENT;
// Single-thread steps may start until 40 s after the verb began, the threads until 50 s. The software adapter gets
// 10 s and 15 s: its control waits 20 s for a receipt. Nothing new starts in the last 10 s before the client's
// deadline, and work already created is written and verified until 5 s before it.
constexpr ULONGLONG single_ms=40000,thread_ms=50000,warp_single_ms=10000,warp_thread_ms=15000,margin_ms=5000;
enum : unsigned {arm_small=1,arm_large=2};
#ifndef INTERACTIVE_GAMELOAD_ARMS
#define INTERACTIVE_GAMELOAD_ARMS 3
#endif
constexpr unsigned arms=INTERACTIVE_GAMELOAD_ARMS;
static_assert(arms>=1 && arms<=3,"INTERACTIVE_GAMELOAD_ARMS: 1 SMALL, 2 LARGE, 3 both");
// The screen. The back buffer format is named here only; the frame is made of clears and holds no format
// assumption, so a 10-bit or FP16 format is this one line.
constexpr DXGI_FORMAT back_buffer_format=DXGI_FORMAT_B8G8R8A8_UNORM;
constexpr UINT back_buffers=2;
constexpr int render_thread=4;
constexpr unsigned tile_columns=64,tile_rows=16,tile_limit=tile_columns*tile_rows;
constexpr ULONGLONG render_ready_ms=10000,render_join_ms=10000,final_frames_ms=1000;
constexpr LONGLONG minimum_frame_us=8000;   // an unpaced Present never spins faster than this
enum : unsigned char {tile_empty,tile_written,tile_verified,tile_bad};

// What the screen shows, written by the load threads and read by the render thread.
struct Board {
    SRWLOCK lock=SRWLOCK_INIT;
    unsigned char tiles[tile_limit]{};unsigned tile_count{};
    unsigned step{},arm{};bool threads{};int outcome{};   // outcome 0 running, 1 clean, -1 failed
    std::atomic<unsigned long long> presents{},longest_us{};
    unsigned open(unsigned char state){
        AcquireSRWLockExclusive(&lock);const unsigned index=tile_count<tile_limit?tile_count++:tile_limit;
        if(index<tile_limit)tiles[index]=state;ReleaseSRWLockExclusive(&lock);return index;
    }
    // A red tile stays red.
    void set(unsigned index,unsigned char state){
        AcquireSRWLockExclusive(&lock);if(index<tile_count && tiles[index]!=tile_bad)tiles[index]=state;ReleaseSRWLockExclusive(&lock);
    }
    void phase(unsigned new_step,unsigned new_arm,bool thread_phase){
        AcquireSRWLockExclusive(&lock);step=new_step;arm=new_arm;threads=thread_phase;ReleaseSRWLockExclusive(&lock);
    }
    void finish(bool clean){AcquireSRWLockExclusive(&lock);outcome=clean?1:-1;ReleaseSRWLockExclusive(&lock);}
};

// splitmix64 of seed, resource id and word index: every 8 bytes differ, and so does every resource and run.
inline UINT64 word(UINT64 seed,UINT64 id,UINT64 index){
    UINT64 z=seed^(id*0x9E3779B97F4A7C15ull)^(index*0xD1B54A32D192ED03ull);
    z=(z^(z>>30))*0xBF58476D1CE4E5B9ull;z=(z^(z>>27))*0x94D049BB133111EBull;return z^(z>>31);
}
// data is 8-byte aligned: placements are 512-byte aligned in a mapped buffer.
inline void fill(unsigned char* data,UINT64 size,UINT64 seed,UINT64 id){
    const UINT64 words=size/8;UINT64* out=reinterpret_cast<UINT64*>(data);
    for(UINT64 i=0;i<words;++i)out[i]=word(seed,id,i);
    if(size%8){const UINT64 last=word(seed,id,words);std::memcpy(data+words*8,&last,static_cast<size_t>(size%8));}
}
// size bytes that sit at byte offset 'offset' of the resource's layout; offset is a multiple of 8.
inline bool same(const unsigned char* data,UINT64 offset,UINT64 size,UINT64 seed,UINT64 id){
    const UINT64 first=offset/8,words=size/8;
    for(UINT64 i=0;i<words;++i){UINT64 value;std::memcpy(&value,data+i*8,8);if(value!=word(seed,id,first+i))return false;}
    if(size%8){const UINT64 last=word(seed,id,first+words);if(std::memcmp(data+words*8,&last,static_cast<size_t>(size%8)))return false;}
    return true;
}
inline D3D12_RESOURCE_DESC buffer(UINT64 size){
    D3D12_RESOURCE_DESC desc{};desc.Dimension=D3D12_RESOURCE_DIMENSION_BUFFER;desc.Width=size;desc.Height=1;
    desc.DepthOrArraySize=1;desc.MipLevels=1;desc.SampleDesc.Count=1;desc.Layout=D3D12_TEXTURE_LAYOUT_ROW_MAJOR;return desc;
}
inline D3D12_HEAP_PROPERTIES heap(D3D12_HEAP_TYPE type){
    D3D12_HEAP_PROPERTIES properties{};properties.Type=type;properties.CreationNodeMask=properties.VisibleNodeMask=1;return properties;
}
inline UINT64 aligned(UINT64 value){return (value+placement-1)/placement*placement;}
struct Shape {
    D3D12_RESOURCE_DESC small_buffer=buffer(small_size),large_buffer=buffer(large_size),texture{};
    D3D12_PLACED_SUBRESOURCE_FOOTPRINT footprint[texture_mips]{};UINT rows[texture_mips]{};UINT64 row_bytes[texture_mips]{};
    UINT64 texture_total{},texture_tight{};
};
inline Shape shape(ID3D12Device* device){
    Shape s;s.texture.Dimension=D3D12_RESOURCE_DIMENSION_TEXTURE2D;s.texture.Width=texture_width;s.texture.Height=texture_height;
    s.texture.DepthOrArraySize=1;s.texture.MipLevels=texture_mips;s.texture.Format=DXGI_FORMAT_R8G8B8A8_UNORM;s.texture.SampleDesc.Count=1;
    device->GetCopyableFootprints(&s.texture,0,texture_mips,0,s.footprint,s.rows,s.row_bytes,&s.texture_total);
    for(UINT i=0;i<texture_mips;++i)s.texture_tight+=s.rows[i]*s.row_bytes[i];
    return s;
}
struct Counts {
    UINT64 created{},written{},fenced{},sampled{},verified{},mismatched{},bytes{},verified_bytes{},submissions{},allocation_failures{};
    void add(const Counts& o){created+=o.created;written+=o.written;fenced+=o.fenced;sampled+=o.sampled;verified+=o.verified;
        mismatched+=o.mismatched;bytes+=o.bytes;verified_bytes+=o.verified_bytes;submissions+=o.submissions;allocation_failures+=o.allocation_failures;}
};
// One line per milestone, through the file system cache to the disk before the caller goes on.
class Milestones {
public:
    explicit Milestones(const std::filesystem::path& path){
        file=CreateFileW(path.c_str(),FILE_APPEND_DATA,FILE_SHARE_READ,nullptr,OPEN_ALWAYS,FILE_ATTRIBUTE_NORMAL|FILE_FLAG_WRITE_THROUGH,nullptr);
        if(file==INVALID_HANDLE_VALUE)error=GetLastError();
        QueryPerformanceFrequency(&frequency);QueryPerformanceCounter(&origin);
    }
    Milestones(const Milestones&)=delete;Milestones& operator=(const Milestones&)=delete;
    ~Milestones(){if(file!=INVALID_HANDLE_VALUE)CloseHandle(file);}
    DWORD open_error() const{return error;}
    bool failed() const{return lost.load();}
    const Board* board{};   // Present count and longest interval, when a screen runs
    void line(const char* phase,unsigned step,const char* arm,int thread,const char* what,const Counts& c,UINT64 fence){
        LARGE_INTEGER now{};QueryPerformanceCounter(&now);
        const long long us=(now.QuadPart-origin.QuadPart)*1000000/frequency.QuadPart;
        char who[12]{};if(thread<0)strcpy_s(who,"main");else if(thread==render_thread)strcpy_s(who,"render");else sprintf_s(who,"%d",thread);
        const unsigned long long presents=board?board->presents.load():0ull,longest=board?board->longest_us.load()/1000:0ull;
        char text[440]{};
        const int length=sprintf_s(text,"qpc=%lld us=%lld phase=%s step=%u size_mb=%llu arm=%s thread=%s event=%s created=%llu written=%llu "
            "fenced=%llu verified=%llu mismatched=%llu bytes=%llu fence=%llu presents=%llu present_gap_ms=%llu\n",now.QuadPart,us,phase,step,
            step && step<=step_count?steps[step-1]/MiB:0ull,arm,who,what,c.created,c.written,c.fenced,c.verified,c.mismatched,c.bytes,fence,
            presents,longest);
        AcquireSRWLockExclusive(&lock);
        DWORD written=0;
        if(file==INVALID_HANDLE_VALUE || length<0 || !WriteFile(file,text,static_cast<DWORD>(length),&written,nullptr) ||
           written!=static_cast<DWORD>(length) || !FlushFileBuffers(file))lost=true;
        ReleaseSRWLockExclusive(&lock);
    }
private:
    HANDLE file{INVALID_HANDLE_VALUE};DWORD error{};std::atomic<bool> lost{};  // read without the lock by failed()
    SRWLOCK lock=SRWLOCK_INIT;LARGE_INTEGER frequency{},origin{};
};
struct Item {ComPtr<ID3D12Resource> resource;UINT64 id{};unsigned tile{tile_limit};bool texture{},sampled{};};
// A submitter: one allocator, list, fence and staging pair. The single-thread phase has one, every worker its own.
// While a submission's completion is unproven, 'pending' (and the flag it mirrors) stays set, and the destructor
// then leaves every object unreleased for process teardown.
struct Context {
    Session& s;Milestones& log;Board& board;const Shape& shape;
    int thread;UINT64 seed;const char* phase;UINT64 staging_size;UINT batch_limit;bool& mirror;
    ComPtr<ID3D12CommandAllocator> allocator;ComPtr<ID3D12GraphicsCommandList> list;ComPtr<ID3D12Fence> fence;
    ComPtr<ID3D12Resource> staging,readback;unsigned char* staging_data{};
    HANDLE event{};UINT64 value{},next_id{};bool pending{};
    std::vector<Item> items;
    unsigned step{};const char* arm="-";Counts counts;
    Context(Session& session,Milestones& milestones,Board& screen,const Shape& layout,int index,UINT64 pattern,const char* name,UINT64 bytes,UINT limit,bool& flag)
        :s(session),log(milestones),board(screen),shape(layout),thread(index),seed(pattern),phase(name),staging_size(bytes),batch_limit(limit),mirror(flag){}
    Context(const Context&)=delete;Context& operator=(const Context&)=delete;
    ~Context(){
        if(pending){for(auto& item:items)item.resource.Detach();list.Detach();allocator.Detach();fence.Detach();staging.Detach();readback.Detach();}
        if(event)CloseHandle(event);
    }
    void trace(const char* label,HRESULT hr=S_OK){s.event("after",label,hr,thread);}
    template<class F> HRESULT api(const char* label,F&& function){s.event("before",label,S_OK,thread);HRESULT hr=function();s.event("after",label,hr,thread);return hr;}
    void milestone(const char* what){log.line(phase,step,arm,thread,what,counts,value);}
    void set_pending(bool state){pending=state;mirror=state;}
    HRESULT setup(){
        event=CreateEventW(nullptr,FALSE,FALSE,nullptr);if(!event)return HRESULT_FROM_WIN32(GetLastError());
        ID3D12Device* device=s.device.Get();
        HRESULT hr=api("Game load CreateCommandAllocator",[&]{return device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT,IID_PPV_ARGS(&allocator));});if(FAILED(hr))return hr;
        hr=api("Game load CreateCommandList",[&]{return device->CreateCommandList(0,D3D12_COMMAND_LIST_TYPE_DIRECT,allocator.Get(),nullptr,IID_PPV_ARGS(&list));});if(FAILED(hr))return hr;
        hr=api("Game load CreateFence",[&]{return device->CreateFence(0,D3D12_FENCE_FLAG_NONE,IID_PPV_ARGS(&fence));});if(FAILED(hr))return hr;
        const D3D12_RESOURCE_DESC desc=buffer(staging_size);
        D3D12_HEAP_PROPERTIES properties=heap(D3D12_HEAP_TYPE_UPLOAD);
        hr=api("Game load CreateCommittedResource UPLOAD staging",[&]{return device->CreateCommittedResource(&properties,D3D12_HEAP_FLAG_NONE,&desc,D3D12_RESOURCE_STATE_GENERIC_READ,nullptr,IID_PPV_ARGS(&staging));});if(FAILED(hr))return hr;
        properties=heap(D3D12_HEAP_TYPE_READBACK);
        hr=api("Game load CreateCommittedResource READBACK",[&]{return device->CreateCommittedResource(&properties,D3D12_HEAP_FLAG_NONE,&desc,D3D12_RESOURCE_STATE_COPY_DEST,nullptr,IID_PPV_ARGS(&readback));});if(FAILED(hr))return hr;
        // The staging buffer stays mapped: the CPU only writes it, and only while no submission reads it.
        void* data=nullptr;const D3D12_RANGE none{0,0};
        hr=api("Game load Map UPLOAD staging",[&]{return staging->Map(0,&none,&data);});if(FAILED(hr))return hr;if(!data)return E_POINTER;
        staging_data=static_cast<unsigned char*>(data);return S_OK;
    }
    HRESULT wait(){
        HRESULT hr=fence->SetEventOnCompletion(value,event);if(FAILED(hr))return hr;
        const ULONGLONG end=(std::min)(s.deadline,GetTickCount64()+5000);DWORD result=WAIT_TIMEOUT;
        while(GetTickCount64()<end && !s.abort_requested() && result==WAIT_TIMEOUT)result=WaitForSingleObject(event,20);
        const UINT64 completed=fence->GetCompletedValue();
        if(completed==UINT64_MAX)return DXGI_ERROR_DEVICE_REMOVED;
        if(completed<value)return s.abort_requested()?HRESULT_FROM_WIN32(ERROR_CANCELLED):HRESULT_FROM_WIN32(WAIT_TIMEOUT);
        set_pending(false);return S_OK;
    }
    // Close, execute, signal and wait for the recorded batch; the milestone before the execution reaches the disk first.
    HRESULT submit(bool write,UINT count){
        char label[160]{};
        HRESULT hr=list->Close();
        if(FAILED(hr)){trace("Game load Close CommandList",hr);return hr;}
        ID3D12CommandList* lists[]={list.Get()};
        if(write)counts.written+=count;
        ++counts.submissions;log.line(phase,step,arm,thread,write?"submit_write":"submit_read",counts,value+1);
        set_pending(true);
        s.queue->ExecuteCommandLists(1,lists);
        hr=s.queue->Signal(fence.Get(),++value);
        if(SUCCEEDED(hr))hr=wait();
        sprintf_s(label,"Game load %s step %u %s %s batch of %u, fence %llu",phase,step,arm,write?"write":"read",count,value);trace(label,hr);
        if(FAILED(hr))return hr;
        milestone(write?"fence_write":"fence_read");
        hr=allocator->Reset();if(SUCCEEDED(hr))hr=list->Reset(allocator.Get(),nullptr);
        if(FAILED(hr))trace("Game load Reset allocator and list",hr);
        return hr;
    }
    HRESULT round_trip(){set_pending(true);HRESULT hr=s.queue->Signal(fence.Get(),++value);return SUCCEEDED(hr)?wait():hr;}
    UINT64 tight(const Item& item) const{return item.texture?shape.texture_tight:item.resource->GetDesc().Width;}
    UINT64 footprint(const Item& item) const{return item.texture?shape.texture_total:item.resource->GetDesc().Width;}
    void record_write(const Item& item,UINT64 offset){
        if(!item.texture){list->CopyBufferRegion(item.resource.Get(),0,staging.Get(),offset,item.resource->GetDesc().Width);return;}
        for(UINT i=0;i<texture_mips;++i){
            D3D12_TEXTURE_COPY_LOCATION dst{},src{};dst.pResource=item.resource.Get();dst.Type=D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;dst.SubresourceIndex=i;
            src.pResource=staging.Get();src.Type=D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;src.PlacedFootprint=shape.footprint[i];src.PlacedFootprint.Offset+=offset;
            list->CopyTextureRegion(&dst,0,0,0,&src,nullptr);
        }
    }
    // Buffers were created COMMON and decayed to it after the write; a texture is still COPY_DEST.
    void record_read(const Item& item,UINT64 offset){
        if(!item.texture){list->CopyBufferRegion(readback.Get(),offset,item.resource.Get(),0,item.resource->GetDesc().Width);return;}
        D3D12_RESOURCE_BARRIER barrier{};barrier.Type=D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;barrier.Transition.pResource=item.resource.Get();
        barrier.Transition.Subresource=D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
        barrier.Transition.StateBefore=D3D12_RESOURCE_STATE_COPY_DEST;barrier.Transition.StateAfter=D3D12_RESOURCE_STATE_COPY_SOURCE;
        list->ResourceBarrier(1,&barrier);
        for(UINT i=0;i<texture_mips;++i){
            D3D12_TEXTURE_COPY_LOCATION dst{},src{};src.pResource=item.resource.Get();src.Type=D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;src.SubresourceIndex=i;
            dst.pResource=readback.Get();dst.Type=D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;dst.PlacedFootprint=shape.footprint[i];dst.PlacedFootprint.Offset+=offset;
            list->CopyTextureRegion(&dst,0,0,0,&src,nullptr);
        }
    }
    bool compare(const Item& item,const unsigned char* data) const{
        if(!item.texture)return same(data,0,item.resource->GetDesc().Width,seed,item.id);
        for(UINT i=0;i<texture_mips;++i)for(UINT row=0;row<shape.rows[i];++row){
            const UINT64 at=shape.footprint[i].Offset+static_cast<UINT64>(row)*shape.footprint[i].Footprint.RowPitch;
            if(!same(data+at,at,shape.row_bytes[i],seed,item.id))return false;
        }
        return true;
    }
};

// One arm of one step: create, write, verify, release. Creation stops at soft_end (budget_hit); what exists is
// still written and verified until hard_end. An allocation failure ends creation the same way and is returned.
inline HRESULT run_arm(Context& c,unsigned step,UINT64 total,unsigned arm,ULONGLONG soft_end,ULONGLONG hard_end,bool& budget_hit){
    Session& s=c.s;const bool large=arm==arm_large;
    c.step=step;c.arm=large?"LARGE":"SMALL";c.counts=Counts{};c.board.phase(step,arm,c.thread>=0);
    std::vector<unsigned> tiles;
    const UINT64 count=total/(large?large_size:small_size);
    const D3D12_HEAP_PROPERTIES properties=heap(D3D12_HEAP_TYPE_DEFAULT);
    char label[360]{};
    sprintf_s(label,"Game load %s step %u %s %llu MB begin: %llu resources",c.phase,step,c.arm,total/MiB,count);c.trace(label);
    c.milestone("arm_begin");
    ULONGLONG mark=GetTickCount64();
    c.items.clear();c.items.reserve(static_cast<size_t>(count));
    HRESULT failure=S_OK;
    for(UINT64 k=0;k<count;++k){
        if(k%256==0 && s.abort_requested()){failure=HRESULT_FROM_WIN32(ERROR_CANCELLED);break;}
        if(GetTickCount64()>=soft_end){budget_hit=true;break;}
        Item item;item.id=c.next_id++;item.texture=large && k%2!=0;
        const D3D12_RESOURCE_DESC* desc=item.texture?&c.shape.texture:large?&c.shape.large_buffer:&c.shape.small_buffer;
        const HRESULT hr=s.device->CreateCommittedResource(&properties,D3D12_HEAP_FLAG_NONE,desc,
            item.texture?D3D12_RESOURCE_STATE_COPY_DEST:D3D12_RESOURCE_STATE_COMMON,nullptr,IID_PPV_ARGS(&item.resource));
        if(FAILED(hr)){
            ++c.counts.allocation_failures;
            sprintf_s(label,"Game load %s step %u %s CreateCommittedResource %s %llu of %llu",c.phase,step,c.arm,item.texture?"texture":"buffer",k+1,count);
            c.trace(label,hr);c.milestone("allocation_failed");c.board.open(tile_bad);failure=hr;break;
        }
        c.items.push_back(std::move(item));++c.counts.created;
        if(large || c.counts.created%256==0)c.milestone("create");
    }
    const size_t n=c.items.size();
    for(size_t i=0;i<n;++i){c.items[i].sampled=large || i%small_sample==0 || i+1==n;if(c.items[i].sampled)++c.counts.sampled;}
    const ULONGLONG create_ms=GetTickCount64()-mark;mark=GetTickCount64();
    sprintf_s(label,"Game load %s step %u %s created %llu of %llu in %llu ms%s",c.phase,step,c.arm,c.counts.created,count,create_ms,
        budget_hit?", time budget reached":"");
    c.trace(label,failure);
    if(failure==HRESULT_FROM_WIN32(ERROR_CANCELLED))return failure;

    // Write: every created resource, packed into batches of the staging buffer.
    HRESULT hr=S_OK;UINT64 cursor=0;UINT batch=0;size_t first=0;
    auto flush=[&](size_t end)->HRESULT{
        if(!batch)return S_OK;
        const HRESULT result=c.submit(true,batch);
        if(SUCCEEDED(result)){const unsigned tile=c.board.open(tile_written);tiles.push_back(tile);
            for(size_t j=first;j<end;++j){++c.counts.fenced;c.counts.bytes+=c.tight(c.items[j]);c.items[j].tile=tile;}}
        cursor=0;batch=0;first=end;return result;
    };
    for(size_t j=0;j<n && SUCCEEDED(hr);++j){
        const UINT64 need=c.footprint(c.items[j]);
        if(batch==c.batch_limit || cursor+need>c.staging_size){hr=flush(j);if(FAILED(hr))break;}
        if(GetTickCount64()>=hard_end || s.abort_requested()){hr=s.abort_requested()?HRESULT_FROM_WIN32(ERROR_CANCELLED):HRESULT_FROM_WIN32(WAIT_TIMEOUT);
            c.trace("Game load write stopped: deadline margin or abort",hr);break;}
        fill(c.staging_data+cursor,need,c.seed,c.items[j].id);
        c.record_write(c.items[j],cursor);cursor=aligned(cursor+need);++batch;
    }
    if(SUCCEEDED(hr))hr=flush(n);
    if(FAILED(hr))return hr;
    const ULONGLONG write_ms=GetTickCount64()-mark;mark=GetTickCount64();

    // Verify: the sampled resources, read back in batches of the readback buffer.
    std::vector<std::pair<size_t,UINT64>> placed;placed.reserve(c.batch_limit);
    unsigned reported=0;
    auto check=[&]()->HRESULT{
        if(placed.empty())return S_OK;
        const HRESULT result=c.submit(false,static_cast<UINT>(placed.size()));if(FAILED(result))return result;
        void* data=nullptr;const D3D12_RANGE range{0,static_cast<SIZE_T>(cursor)},none{0,0};
        HRESULT mapped=c.readback->Map(0,&range,&data);if(SUCCEEDED(mapped) && !data)mapped=E_POINTER;
        if(FAILED(mapped)){c.trace("Game load Map READBACK",mapped);return mapped;}
        for(const auto& [index,offset]:placed){
            const Item& item=c.items[index];
            if(c.compare(item,static_cast<const unsigned char*>(data)+offset)){++c.counts.verified;c.counts.verified_bytes+=c.tight(item);}
            else{++c.counts.mismatched;c.board.set(item.tile,tile_bad);
                if(reported++<4){sprintf_s(label,"Game load %s step %u %s resource %zu (id %llx) differs",c.phase,step,c.arm,index,item.id);c.trace(label,E_FAIL);}}
        }
        c.readback->Unmap(0,&none);c.milestone("verify");
        placed.clear();cursor=0;return S_OK;
    };
    cursor=0;
    for(size_t j=0;j<n && SUCCEEDED(hr);++j){
        if(!c.items[j].sampled)continue;
        const UINT64 need=c.footprint(c.items[j]);
        if(placed.size()==c.batch_limit || cursor+need>c.staging_size){hr=check();if(FAILED(hr))break;}
        if(GetTickCount64()>=hard_end || s.abort_requested()){hr=s.abort_requested()?HRESULT_FROM_WIN32(ERROR_CANCELLED):HRESULT_FROM_WIN32(WAIT_TIMEOUT);
            c.trace("Game load verification stopped: deadline margin or abort",hr);break;}
        c.record_read(c.items[j],cursor);placed.emplace_back(j,cursor);cursor=aligned(cursor+need);
    }
    if(SUCCEEDED(hr))hr=check();
    if(FAILED(hr))return hr;
    const ULONGLONG verify_ms=GetTickCount64()-mark;mark=GetTickCount64();

    // Release: every use of these resources has completed; then one fence round trip on the queue.
    c.items.clear();
    hr=c.round_trip();
    const ULONGLONG release_ms=GetTickCount64()-mark;
    sprintf_s(label,"Game load %s step %u %s released %zu resources, fence %llu",c.phase,step,c.arm,n,c.value);c.trace(label,hr);
    if(FAILED(hr))return hr;
    c.milestone("arm_end");
    for(const unsigned tile:tiles)c.board.set(tile,tile_verified);
    sprintf_s(label,"Game load %s step %u %s: created %llu, written %llu, fenced %llu, verified %llu of %llu, mismatched %llu, "
        "bytes %llu, verified bytes %llu, submissions %llu, ms %llu/%llu/%llu/%llu",c.phase,step,c.arm,c.counts.created,c.counts.written,
        c.counts.fenced,c.counts.verified,c.counts.sampled,c.counts.mismatched,c.counts.bytes,c.counts.verified_bytes,c.counts.submissions,
        create_ms,write_ms,verify_ms,release_ms);
    const bool clean=c.counts.mismatched==0 && c.counts.verified==c.counts.sampled && c.counts.fenced==c.counts.created;
    c.trace(label,SUCCEEDED(failure) && !clean?E_FAIL:failure);
    if(FAILED(failure))return failure;
    return clean?S_OK:E_FAIL;
}

// Public budget of the adapter against what a step needs; unknown is traced and not taken as a reason to skip.
inline bool fits(Session& s,Milestones& log,IDXGIAdapter3* adapter,unsigned step,const char* phase,UINT64 need){
    char label[240]{};
    if(!adapter){sprintf_s(label,"Game load %s step %u budget unknown (no IDXGIAdapter3), not checked",phase,step);s.event("after",label);return true;}
    DXGI_QUERY_VIDEO_MEMORY_INFO local{},other{};
    HRESULT hr=adapter->QueryVideoMemoryInfo(0,DXGI_MEMORY_SEGMENT_GROUP_LOCAL,&local);
    if(FAILED(hr)){sprintf_s(label,"Game load %s step %u budget query failed, not checked",phase,step);s.event("after",label,hr);return true;}
    const HRESULT other_hr=adapter->QueryVideoMemoryInfo(0,DXGI_MEMORY_SEGMENT_GROUP_NON_LOCAL,&other);
    sprintf_s(label,"Game load %s step %u needs %llu MB: local budget %llu MB, usage %llu MB; non-local budget %llu MB, usage %llu MB",
        phase,step,need/MiB,local.Budget/MiB,local.CurrentUsage/MiB,other.Budget/MiB,other.CurrentUsage/MiB);
    s.event("after",label,other_hr);
    if(local.CurrentUsage<=local.Budget && need<=local.Budget-local.CurrentUsage)return true;
    sprintf_s(label,"Game load %s step %u skipped: %llu MB does not fit the local budget",phase,step,need/MiB);s.event("after",label);
    log.line(phase,step,"-",-1,"step_skipped",Counts{},0);
    return false;
}

struct Outcome {Counts counts;HRESULT hr{S_OK};bool pending{},budget{};};
inline void worker(Session& s,Milestones& log,Board& board,const Shape& layout,unsigned index,UINT64 seed,ULONGLONG soft_end,ULONGLONG hard_end,Outcome& out) noexcept {
    try{
        Context c(s,log,board,layout,static_cast<int>(index),seed,"threads",thread_staging,thread_batch_limit,out.pending);
        c.next_id=(static_cast<UINT64>(index)+1)<<40;c.step=1;c.arm="SMALL";
        char label[120]{};sprintf_s(label,"Game load thread %u begin, seed %016llx",index,seed);c.trace(label);c.milestone("thread_begin");
        HRESULT hr=c.setup();bool hit=false;
        if(SUCCEEDED(hr))hr=run_arm(c,1,steps[0]/threads,arm_small,soft_end,hard_end,hit);
        out.counts=c.counts;out.hr=hr;out.budget=hit;
        sprintf_s(label,"Game load thread %u end: created %llu, verified %llu, mismatched %llu",index,c.counts.created,c.counts.verified,c.counts.mismatched);
        c.trace(label,hr);c.milestone("thread_end");
    }catch(...){out.hr=E_OUTOFMEMORY;}
}

inline LRESULT CALLBACK screen_window_proc(HWND window,UINT message,WPARAM w,LPARAM l){return DefWindowProcW(window,message,w,l);}
// The render thread: a window and swap chain (or, on the software adapter, an offscreen target), then one frame
// per Present until the load ends, and one more second showing the outcome. Its results are read after the join.
struct Screen {
    Session& s;Milestones& log;Board& board;const bool offscreen;
    std::atomic<bool> stop{false};
    HANDLE ready{};   // set after the first frame, or when the setup failed
    HRESULT hr{S_OK};bool pending{};unsigned long long frames{},occluded{},failures{};UINT width{},height{};
    D3D12_RECT rects[tile_limit]{};
    Screen(Session& session,Milestones& milestones,Board& shown)
        :s(session),log(milestones),board(shown),offscreen(session.mode==AdapterMode::Warp){ready=CreateEventW(nullptr,TRUE,FALSE,nullptr);}
    Screen(const Screen&)=delete;Screen& operator=(const Screen&)=delete;
    ~Screen(){if(ready)CloseHandle(ready);}
    void trace(const char* label,HRESULT code=S_OK){s.event("after",label,code,render_thread);}
    void note(const char* what,UINT64 fence){log.line("render",0,"-",render_thread,what,Counts{},fence);}

    // Clears only: a background, the band of the step (left) and arm (right), a bar that moves one step per frame,
    // and the tiles of the written batches.
    void draw(ID3D12GraphicsCommandList* list,D3D12_CPU_DESCRIPTOR_HANDLE view,unsigned long long frame){
        unsigned char tiles[tile_limit];unsigned count=0,step=0,arm=0;bool threads_phase=false;int outcome=0;
        AcquireSRWLockShared(&board.lock);
        count=board.tile_count;std::memcpy(tiles,board.tiles,sizeof(tiles));step=board.step;arm=board.arm;threads_phase=board.threads;outcome=board.outcome;
        ReleaseSRWLockShared(&board.lock);
        static constexpr FLOAT background[4]{0.06f,0.06f,0.08f,1.0f},strip[4]{0.02f,0.02f,0.03f,1.0f},bar[4]{1.0f,1.0f,1.0f,1.0f};
        static constexpr FLOAT step_colour[step_count+1][4]{{0.3f,0.3f,0.3f,1.0f},{0.15f,0.35f,0.95f,1.0f},{0.0f,0.75f,0.75f,1.0f},
            {1.0f,0.55f,0.1f,1.0f},{0.65f,0.25f,0.9f,1.0f}};
        static constexpr FLOAT thread_colour[4]{0.95f,0.95f,0.95f,1.0f},small_colour[4]{0.55f,0.8f,1.0f,1.0f},large_colour[4]{0.55f,0.3f,0.1f,1.0f};
        static constexpr FLOAT tile_colour[4][4]{{0.14f,0.14f,0.16f,1.0f},{1.0f,0.85f,0.0f,1.0f},{0.1f,0.8f,0.25f,1.0f},{0.95f,0.1f,0.1f,1.0f}};
        const LONG w=static_cast<LONG>(width),h=static_cast<LONG>(height);
        const LONG band=h/10,ticker=band+h/20,margin=(std::max)(4L,h/60);
        list->ClearRenderTargetView(view,background,0,nullptr);
        if(outcome){const D3D12_RECT whole{0,0,w,band};list->ClearRenderTargetView(view,tile_colour[outcome>0?tile_verified:tile_bad],1,&whole);}
        else{
            const D3D12_RECT left{0,0,w*7/10,band},right{w*7/10,0,w,band};
            list->ClearRenderTargetView(view,threads_phase?thread_colour:step_colour[step<=step_count?step:0],1,&left);
            list->ClearRenderTargetView(view,arm==arm_large?large_colour:arm==arm_small?small_colour:step_colour[0],1,&right);
        }
        const D3D12_RECT lane{0,band,w,ticker};list->ClearRenderTargetView(view,strip,1,&lane);
        const LONG bar_width=(std::max)(4L,w/48),travel=(std::max)(1L,w-bar_width);
        const LONG x=static_cast<LONG>((frame*static_cast<unsigned long long>((std::max)(1L,travel/120)))%static_cast<unsigned long long>(travel));
        const D3D12_RECT moving{x,band,x+bar_width,ticker};list->ClearRenderTargetView(view,bar,1,&moving);
        const LONG top=ticker+margin,cell_w=(std::max)(1L,(w-2*margin)/static_cast<LONG>(tile_columns)),
            cell_h=(std::max)(1L,(h-top-margin)/static_cast<LONG>(tile_rows)),gap=(std::max)(1L,cell_w/8);
        for(unsigned char state=tile_empty;state<=tile_bad;++state){
            UINT used=0;
            for(unsigned i=0;i<tile_limit;++i){
                if((i<count?tiles[i]:tile_empty)!=state)continue;
                const LONG cx=margin+static_cast<LONG>(i%tile_columns)*cell_w,cy=top+static_cast<LONG>(i/tile_columns)*cell_h;
                rects[used++]={cx,cy,cx+cell_w-gap,cy+cell_h-gap};
            }
            if(used)list->ClearRenderTargetView(view,tile_colour[state],used,rects);
        }
    }

    void run() noexcept {
        HWND window{};const wchar_t* class_name=L"amdgpu_wddm_d3d12_queue_gameload";bool registered=false;
        ComPtr<IDXGISwapChain3> chain;ComPtr<ID3D12Resource> targets[back_buffers];ComPtr<ID3D12DescriptorHeap> views;
        ComPtr<ID3D12CommandAllocator> allocators[back_buffers];ComPtr<ID3D12GraphicsCommandList> list;ComPtr<ID3D12Fence> fence;
        HANDLE event{};UINT64 value=0,slot[back_buffers]{};
        bool unsignaled=false;   // a list was executed and no Signal after it succeeded yet
        HRESULT result=S_OK;char label[200]{};
        const auto wait=[&](UINT64 target)->HRESULT{
            UINT64 completed=fence->GetCompletedValue();
            if(completed==UINT64_MAX)return DXGI_ERROR_DEVICE_REMOVED;
            if(completed>=target)return S_OK;
            HRESULT code=fence->SetEventOnCompletion(target,event);if(FAILED(code))return code;
            const ULONGLONG end=(std::min)(s.deadline,GetTickCount64()+5000);DWORD status=WAIT_TIMEOUT;
            while(GetTickCount64()<end && !s.abort_requested() && status==WAIT_TIMEOUT)status=WaitForSingleObject(event,20);
            completed=fence->GetCompletedValue();
            if(completed==UINT64_MAX)return DXGI_ERROR_DEVICE_REMOVED;
            return completed>=target?S_OK:HRESULT_FROM_WIN32(WAIT_TIMEOUT);
        };
        try{
            ID3D12Device* device=s.device.Get();
            // Physical pixels for this thread's window, whatever the display scaling.
            SetThreadDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
            RECT area{0,0,GetSystemMetrics(SM_CXSCREEN),GetSystemMetrics(SM_CYSCREEN)};const char* source="system metrics";
            if(!offscreen){ComPtr<IDXGIOutput> output;DXGI_OUTPUT_DESC desc{};
                if(SUCCEEDED(s.adapter->EnumOutputs(0,&output)) && SUCCEEDED(output->GetDesc(&desc))){area=desc.DesktopCoordinates;source="adapter output 0";}}
            width=static_cast<UINT>((std::max)(64L,area.right-area.left));height=static_cast<UINT>((std::max)(64L,area.bottom-area.top));
            sprintf_s(label,"Game load screen %ux%u at %ld,%ld from %s, format %d, %s",width,height,area.left,area.top,source,
                static_cast<int>(back_buffer_format),offscreen?"offscreen target (software adapter: no window)":"borderless window, flip model");
            trace(label);
            event=CreateEventW(nullptr,FALSE,FALSE,nullptr);if(!event)throw HRESULT_FROM_WIN32(GetLastError());
            for(UINT i=0;i<back_buffers && SUCCEEDED(result);++i)result=device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT,IID_PPV_ARGS(&allocators[i]));
            if(SUCCEEDED(result))result=device->CreateCommandList(0,D3D12_COMMAND_LIST_TYPE_DIRECT,allocators[0].Get(),nullptr,IID_PPV_ARGS(&list));
            if(SUCCEEDED(result))result=list->Close();
            if(SUCCEEDED(result))result=device->CreateFence(0,D3D12_FENCE_FLAG_NONE,IID_PPV_ARGS(&fence));
            D3D12_DESCRIPTOR_HEAP_DESC heap_desc{};heap_desc.Type=D3D12_DESCRIPTOR_HEAP_TYPE_RTV;heap_desc.NumDescriptors=back_buffers;
            if(SUCCEEDED(result))result=device->CreateDescriptorHeap(&heap_desc,IID_PPV_ARGS(&views));
            trace("Game load screen allocators, list, fence and RTV heap",result);
            if(FAILED(result))throw result;
            if(!offscreen){
                WNDCLASSEXW window_class{};window_class.cbSize=sizeof(window_class);window_class.lpfnWndProc=screen_window_proc;
                window_class.hInstance=GetModuleHandleW(nullptr);window_class.lpszClassName=class_name;
                window_class.hCursor=LoadCursorW(nullptr,IDC_ARROW);
                if(!RegisterClassExW(&window_class)){result=HRESULT_FROM_WIN32(GetLastError());trace("Game load RegisterClassEx",result);throw result;}
                registered=true;
                note("window_begin",value);
                window=CreateWindowExW(WS_EX_APPWINDOW,class_name,L"amdgpu-wddm game load",WS_POPUP,area.left,area.top,
                    static_cast<int>(width),static_cast<int>(height),nullptr,nullptr,window_class.hInstance,nullptr);
                if(!window){result=HRESULT_FROM_WIN32(GetLastError());trace("Game load CreateWindowEx borderless",result);throw result;}
                ShowWindow(window,SW_SHOW);
                trace("Game load window shown, foreground",SetForegroundWindow(window)?S_OK:S_FALSE);
                note("window",value);
                DXGI_SWAP_CHAIN_DESC1 desc{};desc.Width=width;desc.Height=height;desc.Format=back_buffer_format;desc.SampleDesc.Count=1;
                desc.BufferUsage=DXGI_USAGE_RENDER_TARGET_OUTPUT;desc.BufferCount=back_buffers;desc.Scaling=DXGI_SCALING_STRETCH;
                desc.SwapEffect=DXGI_SWAP_EFFECT_FLIP_DISCARD;desc.AlphaMode=DXGI_ALPHA_MODE_IGNORE;
                ComPtr<IDXGISwapChain1> chain1;
                note("swap_chain_begin",value);
                result=s.factory->CreateSwapChainForHwnd(s.queue.Get(),window,&desc,nullptr,nullptr,&chain1);
                trace("Game load CreateSwapChainForHwnd FLIP_DISCARD 2 buffers",result);if(FAILED(result))throw result;
                note("swap_chain",value);
                result=s.factory->MakeWindowAssociation(window,DXGI_MWA_NO_ALT_ENTER);if(FAILED(result)){trace("Game load MakeWindowAssociation",result);throw result;}
                result=chain1.As(&chain);if(FAILED(result)){trace("Game load IDXGISwapChain3",result);throw result;}
                for(UINT i=0;i<back_buffers;++i){result=chain->GetBuffer(i,IID_PPV_ARGS(&targets[i]));if(FAILED(result)){trace("Game load GetBuffer",result);throw result;}}
            }else{
                D3D12_RESOURCE_DESC desc{};desc.Dimension=D3D12_RESOURCE_DIMENSION_TEXTURE2D;desc.Width=width;desc.Height=height;desc.DepthOrArraySize=1;
                desc.MipLevels=1;desc.Format=back_buffer_format;desc.SampleDesc.Count=1;desc.Flags=D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;
                const D3D12_HEAP_PROPERTIES properties=heap(D3D12_HEAP_TYPE_DEFAULT);
                result=device->CreateCommittedResource(&properties,D3D12_HEAP_FLAG_NONE,&desc,D3D12_RESOURCE_STATE_RENDER_TARGET,nullptr,IID_PPV_ARGS(&targets[0]));
                trace("Game load CreateCommittedResource offscreen render target",result);if(FAILED(result))throw result;
            }
            const UINT increment=device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_RTV);
            for(UINT i=0;i<back_buffers;++i)if(targets[i]){
                D3D12_CPU_DESCRIPTOR_HANDLE view=views->GetCPUDescriptorHandleForHeapStart();view.ptr+=SIZE_T{i}*increment;
                device->CreateRenderTargetView(targets[i].Get(),nullptr,view);
            }

            LARGE_INTEGER frequency{},last{};QueryPerformanceFrequency(&frequency);
            ULONGLONG stop_at=0;const ULONGLONG hard_end=s.deadline>margin_ms?s.deadline-margin_ms:0;
            for(unsigned long long frame=0;;++frame){
                if(window){MSG message{};while(PeekMessageW(&message,window,0,0,PM_REMOVE)){TranslateMessage(&message);DispatchMessageW(&message);}}
                if(stop.load() && !stop_at)stop_at=GetTickCount64()+final_frames_ms;
                if((stop_at && GetTickCount64()>=stop_at) || GetTickCount64()>=hard_end || s.abort_requested())break;
                // Present with sync interval 1 paces the loop to the display. Offscreen frames are paced to 60 per
                // second here, and an unpaced Present never comes sooner than minimum_frame_us after the last one.
                if(frames){LARGE_INTEGER now{};QueryPerformanceCounter(&now);
                    const LONGLONG elapsed=(now.QuadPart-last.QuadPart)*1000000/frequency.QuadPart,minimum=offscreen?16667:minimum_frame_us;
                    if(elapsed<minimum)Sleep(static_cast<DWORD>((minimum-elapsed+999)/1000));}
                const UINT index=static_cast<UINT>(frame%back_buffers);
                result=wait(slot[index]);if(FAILED(result)){trace("Game load screen frame slot wait",result);break;}
                result=allocators[index]->Reset();if(SUCCEEDED(result))result=list->Reset(allocators[index].Get(),nullptr);
                if(FAILED(result)){trace("Game load screen Reset allocator and list",result);break;}
                const UINT current=chain?chain->GetCurrentBackBufferIndex():0;
                if(current>=back_buffers || !targets[current]){result=E_UNEXPECTED;trace("Game load screen back buffer index",result);break;}
                D3D12_CPU_DESCRIPTOR_HANDLE view=views->GetCPUDescriptorHandleForHeapStart();view.ptr+=SIZE_T{current}*increment;
                D3D12_RESOURCE_BARRIER barrier{};barrier.Type=D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;barrier.Transition.pResource=targets[current].Get();
                barrier.Transition.Subresource=D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
                barrier.Transition.StateBefore=D3D12_RESOURCE_STATE_PRESENT;barrier.Transition.StateAfter=D3D12_RESOURCE_STATE_RENDER_TARGET;
                if(chain)list->ResourceBarrier(1,&barrier);
                draw(list.Get(),view,frame);
                std::swap(barrier.Transition.StateBefore,barrier.Transition.StateAfter);
                if(chain)list->ResourceBarrier(1,&barrier);
                result=list->Close();if(FAILED(result)){trace("Game load screen Close CommandList",result);break;}
                ID3D12CommandList* lists[]={list.Get()};
                pending=true;unsignaled=true;s.queue->ExecuteCommandLists(1,lists);
                if(chain){
                    if(!frame)note("first_present_begin",value);
                    const HRESULT presented=chain->Present(1,0);
                    if(FAILED(presented)){++failures;result=presented;trace("Game load Present",presented);
                        trace("Game load GetDeviceRemovedReason after Present",device->GetDeviceRemovedReason());break;}
                    if(presented!=S_OK && occluded++<4){sprintf_s(label,"Game load Present status at frame %llu",frame);trace(label,presented);}
                }
                result=s.queue->Signal(fence.Get(),++value);if(FAILED(result)){trace("Game load screen Queue Signal",result);break;}
                unsignaled=false;
                slot[index]=value;
                LARGE_INTEGER now{};QueryPerformanceCounter(&now);
                const LONGLONG since=frames?(now.QuadPart-last.QuadPart)*1000000/frequency.QuadPart:0;
                if(frames && static_cast<unsigned long long>(since)>board.longest_us.load())board.longest_us.store(static_cast<unsigned long long>(since));
                last=now;board.presents.store(++frames);
                if(frames==1){note(chain?"first_present":"first_frame",value);
                    trace(chain?"Game load first Present":"Game load first offscreen frame");SetEvent(ready);}
                else if(frames%60==0)note(chain?"present":"frame",value);
            }
#ifdef INTERACTIVE_GAMELOAD_RENDER_HOLD_MS
            // Test build only: outlive the join bound while still tracing, as a render thread stuck in a driver
            // call would, so that the session must end without closing the trace under it.
            for(const ULONGLONG until=GetTickCount64()+INTERACTIVE_GAMELOAD_RENDER_HOLD_MS;GetTickCount64()<until;Sleep(250))
                trace("Game load test hold of the render thread");
#endif
            // Every frame retires before its objects are released. A frame executed without a following Signal
            // (a failed Present, a failed Signal) is not covered by an earlier fence value: it gets a covering
            // Signal now if the queue takes one, and otherwise everything stays for process teardown. The first
            // error stays the result.
            if(unsignaled){
                const HRESULT covered=s.queue->Signal(fence.Get(),value+1);trace("Game load screen covering Signal",covered);
                if(SUCCEEDED(covered)){++value;unsignaled=false;}
            }
            if(pending && !unsignaled){const HRESULT retired=wait(value);if(SUCCEEDED(retired))pending=false;
                else{trace("Game load screen final retire",retired);if(SUCCEEDED(result))result=retired;}}
            if(pending && SUCCEEDED(result))result=E_FAIL;
        }catch(HRESULT code){if(SUCCEEDED(result))result=code;}
        catch(...){result=E_OUTOFMEMORY;}
        hr=result;
        sprintf_s(label,"Game load screen end: %s %llu, longest interval %llu ms, non-S_OK Present statuses %llu, failures %llu",
            offscreen?"offscreen frames":"presents",frames,board.longest_us.load()/1000,occluded,failures);
        trace(label,result);note("render_end",value);
        if(pending){for(auto& target:targets)target.Detach();chain.Detach();views.Detach();for(auto& allocator:allocators)allocator.Detach();list.Detach();fence.Detach();}
        else{for(auto& target:targets)target.Reset();chain.Reset();if(window)DestroyWindow(window);if(registered)UnregisterClassW(class_name,GetModuleHandleW(nullptr));}
        if(event)CloseHandle(event);
        SetEvent(ready);
    }
};
// What the load threads, the render thread and the milestones share. When the render thread does not end in
// time, this stays allocated for the rest of the process.
struct Shared {
    Milestones log;Board board;Screen screen;
    Shared(Session& s,const std::filesystem::path& path):log(path),screen(s,log,board){log.board=&board;}
};
}  // namespace gameload

inline HRESULT gameload_run(Session& s){
    using namespace gameload;
    if(!s.queue || s.pending || s.copy_success)return E_UNEXPECTED;
    const ULONGLONG begin=GetTickCount64();const bool warp=s.mode==AdapterMode::Warp;
    const ULONGLONG last_start=s.deadline>begin+2*margin_ms?s.deadline-2*margin_ms:begin;
    const ULONGLONG single_end=(std::min)(begin+(warp?warp_single_ms:single_ms),last_start);
    const ULONGLONG thread_end=(std::min)(begin+(warp?warp_thread_ms:thread_ms),last_start);
    const ULONGLONG hard_end=s.deadline>begin+margin_ms?s.deadline-margin_ms:begin;
    Shared* shared=new Shared(s,s.root/L"milestones.log");
    Milestones& log=shared->log;Board& board=shared->board;Screen& screen=shared->screen;
    if(log.open_error()){const HRESULT failed=HRESULT_FROM_WIN32(log.open_error());s.event("after","Game load milestones.log",failed);delete shared;return failed;}
    UINT64 seed=0;
    if(BCryptGenRandom(nullptr,reinterpret_cast<PUCHAR>(&seed),sizeof(seed),BCRYPT_USE_SYSTEM_PREFERRED_RNG)<0){delete shared;return E_FAIL;}
    char label[480]{};
    sprintf_s(label,"Game load seed %016llx, arms %s, steps 256/512/1024/2048 MB, single-thread starts until %llu ms, threads until %llu ms",
        seed,arms==3?"SMALL then LARGE":arms==1?"SMALL":"LARGE",single_end-begin,thread_end-begin);
    s.event("after",label);
    const Shape layout=shape(s.device.Get());
    sprintf_s(label,"Game load texture %ux%u RGBA8 %u mips: footprint %llu bytes, written %llu bytes",texture_width,texture_height,
        static_cast<unsigned>(texture_mips),layout.texture_total,layout.texture_tight);
    s.event("after",label,layout.texture_total<=staging_size?S_OK:E_FAIL);
    if(layout.texture_total>staging_size){delete shared;return E_FAIL;}
    ComPtr<IDXGIAdapter3> adapter;
    {const HRESULT queried=s.adapter.As(&adapter);if(FAILED(queried))s.event("after","Game load IDXGIAdapter3",queried);}
    log.line("single",0,"-",-1,"load_begin",Counts{},0);

    // The screen comes first, as in a game; the load starts when the first frame is out, or after 10 s without it.
    std::thread render;
    try{render=std::thread([&screen]{screen.run();});}
    catch(...){s.event("after","Game load render thread start",E_OUTOFMEMORY);delete shared;return E_OUTOFMEMORY;}
    s.event("after","Game load screen ready",WaitForSingleObject(screen.ready,static_cast<DWORD>(render_ready_ms))==WAIT_OBJECT_0?S_OK:HRESULT_FROM_WIN32(WAIT_TIMEOUT));

    // A step, an arm or the thread phase counts as exercised only once it created a resource; coverage is
    // complete only when every planned arm and all four threads were, with no step skipped or cut.
    constexpr unsigned arms_per_step=(arms&arm_small?1u:0u)+(arms&arm_large?1u:0u),planned=step_count*arms_per_step+1;
    Counts total;HRESULT result=S_OK;unsigned started=0,ran=0,exercised=0,skipped=0;bool budget_hit=false;
    try{
        {
            Context c(s,log,board,layout,-1,seed,"single",staging_size,batch_limit,s.pending);
            result=c.setup();
            for(unsigned step=1;step<=step_count && SUCCEEDED(result) && !budget_hit;++step){
                if(GetTickCount64()>=single_end){budget_hit=true;sprintf_s(label,"Game load time budget reached at step %u",step);s.event("after",label);
                    log.line("single",step,"-",-1,"time_budget",total,c.value);break;}
                if(!fits(s,log,adapter.Get(),step,"single",steps[step-1])){++skipped;continue;}
                log.line("single",step,"-",-1,"step_begin",total,c.value);
                bool step_exercised=false;
                for(unsigned arm:{arm_small,arm_large}){
                    if(!(arms&arm))continue;
                    bool hit=GetTickCount64()>=single_end;
                    if(!hit){result=run_arm(c,step,steps[step-1],arm,single_end,hard_end,hit);total.add(c.counts);
                        if(c.counts.created){++exercised;step_exercised=true;}}
                    if(FAILED(result))break;
                    if(hit){budget_hit=true;sprintf_s(label,"Game load time budget reached at step %u",step);s.event("after",label);
                        log.line("single",step,"-",-1,"time_budget",total,c.value);break;}
                }
                if(step_exercised)++started;
                log.line("single",step,"-",-1,"step_end",total,c.value);
            }
        }
        if(SUCCEEDED(result) && !s.pending){
            if(GetTickCount64()>=thread_end){s.event("after","Game load time budget reached before the threads");log.line("threads",1,"-",-1,"time_budget",total,0);}
            else if(fits(s,log,adapter.Get(),1,"threads",steps[0]+threads*2*thread_staging)){
                Outcome outcome[threads]{};std::thread workers[threads];unsigned launched=0;
                try{for(;launched<threads;++launched){const unsigned index=launched;
                    workers[index]=std::thread([&s,&log,&board,&layout,&outcome,index,seed,thread_end,hard_end]{
                        worker(s,log,board,layout,index,seed^((index+1)*0xA0761D6478BD642Full),thread_end,hard_end,outcome[index]);});}}
                catch(...){s.event("after","Game load thread start",E_OUTOFMEMORY);result=E_OUTOFMEMORY;}
                for(unsigned i=0;i<launched;++i)workers[i].join();
                for(unsigned i=0;i<launched;++i){
                    total.add(outcome[i].counts);if(outcome[i].pending)s.pending=true;
                    if(outcome[i].counts.created)++ran;
                    if(SUCCEEDED(result) && FAILED(outcome[i].hr))result=outcome[i].hr;
                    if(outcome[i].budget)budget_hit=true;
                }
            }
        }
    }catch(...){if(SUCCEEDED(result))result=E_OUTOFMEMORY;s.event("after","Game load exception",result);}
    if(ran==threads)++exercised;
    // Success needs load that ran: resources created and a verified sample. A run whose every arm the time bound
    // (or the budget) skipped is "none", never a functional copy.
    const bool loaded=exercised && total.created && total.sampled;
    const char* coverage=!loaded?"none":exercised==planned && !budget_hit && !skipped?"complete":"partial";
    const bool load_clean=SUCCEEDED(result) && !s.pending && loaded && total.mismatched==0 && total.allocation_failures==0 &&
        total.verified==total.sampled && total.fenced==total.created;
    if(SUCCEEDED(result) && !loaded){result=HRESULT_FROM_WIN32(ERROR_NO_DATA);s.event("after","Game load incomplete: no load ran",result);}

    // The outcome stays on the screen for a second, then the render thread retires its frames and ends.
    board.finish(load_clean);screen.stop.store(true);
    const bool joined=WaitForSingleObject(static_cast<HANDLE>(render.native_handle()),static_cast<DWORD>(render_join_ms))==WAIT_OBJECT_0;
    if(joined){render.join();if(screen.pending)s.pending=true;}
    else{render.detach();s.pending=true;s.detached=true;s.event("after","Game load render thread did not end; its objects stay for process teardown",HRESULT_FROM_WIN32(WAIT_TIMEOUT));}
    const bool screen_clean=joined && screen.hr==S_OK && !screen.failures && screen.frames;
    // The terminal milestone is written before the outcome is decided, so that its loss fails the run too.
    log.line("single",0,"-",-1,"load_end",total,0);
    const bool clean=load_clean && screen_clean && !log.failed();
    sprintf_s(label,"Game load: steps %u of %u, created %llu, verified %llu, mismatched %llu, bytes %llu, threads %u, %s %llu, "
        "longest present interval %llu ms, coverage %s, arms %u of %u, steps skipped %u, verified bytes %llu, allocation failures %llu, "
        "submissions %llu, time budget %s, %llu ms",
        started,step_count,total.created,total.verified,total.mismatched,total.bytes,ran,screen.offscreen?"offscreen frames":"presents",
        board.presents.load(),board.longest_us.load()/1000,coverage,exercised,planned,skipped,total.verified_bytes,total.allocation_failures,
        total.submissions,budget_hit?"reached":"not reached",GetTickCount64()-begin);
    const HRESULT hr=clean?S_OK:FAILED(result)?result:joined && FAILED(screen.hr)?screen.hr:E_FAIL;
    s.event("after",label,hr);
    if(log.failed())s.event("after","Game load milestones.log write failed",E_FAIL);
    if(joined)delete shared;   // otherwise the render thread may still use it
    s.copy_success=clean;return hr;
}
