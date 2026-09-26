/* Windows GFX pipeline control; E14 allocation and CPU-visible readback helpers. */
#define main e14_reference_main
#include "../E14-vulkan-compute-reference/vkcompute.c"
#undef main
#define JOBS 16u
#define WORDS (4u*1024u*1024u)
int main(int argc,char**argv)
{
    struct ctx c={0}; struct gbuf buffers[JOBS]; VkCommandBuffer commands[JOBS]; VkFence fences[JOBS];
    uint32_t repetitions=argc>1?(uint32_t)atoi(argv[1]):16u, pending_before=0, mismatches=0;
    if(repetitions<1 || repetitions>32)die("repetitions must be 1..32");
    setvbuf(stdout,NULL,_IONBF,0);
    ctx_init(&c);
    printf("stage device-created\n");
    if(c.props.vendorID!=0x1002 || c.props.deviceID!=0x13fe)die("unexpected GPU");
    VkCommandBufferAllocateInfo alloc={VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
    alloc.commandPool=c.pool; alloc.level=VK_COMMAND_BUFFER_LEVEL_PRIMARY;alloc.commandBufferCount=JOBS;
    VK_CHECK(vkAllocateCommandBuffers(c.dev,&alloc,commands));
    for(uint32_t i=0;i<JOBS;i++){
        buf_create(&c,&buffers[i],(VkDeviceSize)WORDS*4u);
        VkFenceCreateInfo fi={VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
        VK_CHECK(vkCreateFence(c.dev,&fi,NULL,&fences[i]));
        VkCommandBufferBeginInfo begin={VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
        VK_CHECK(vkBeginCommandBuffer(commands[i],&begin));
        for(uint32_t pass=0;pass<repetitions;pass++){
            vkCmdFillBuffer(commands[i],buffers[i].buf,0,VK_WHOLE_SIZE,0x12340000u+i);
            VkMemoryBarrier barrier={VK_STRUCTURE_TYPE_MEMORY_BARRIER};
            barrier.srcAccessMask=VK_ACCESS_TRANSFER_WRITE_BIT;
            barrier.dstAccessMask=VK_ACCESS_TRANSFER_WRITE_BIT|VK_ACCESS_HOST_READ_BIT;
            vkCmdPipelineBarrier(commands[i],VK_PIPELINE_STAGE_TRANSFER_BIT,
                VK_PIPELINE_STAGE_TRANSFER_BIT|VK_PIPELINE_STAGE_HOST_BIT,0,1,&barrier,0,NULL,0,NULL);
        }
        VK_CHECK(vkEndCommandBuffer(commands[i]));
    }
    printf("stage buffers-recorded\n");
    for(uint32_t i=0;i<JOBS;i++){
        if(i && vkGetFenceStatus(c.dev,fences[i-1])==VK_NOT_READY)++pending_before;
        VkSubmitInfo submit={VK_STRUCTURE_TYPE_SUBMIT_INFO};
        submit.commandBufferCount=1;submit.pCommandBuffers=&commands[i];
        VK_CHECK(vkQueueSubmit(c.queue,1,&submit,fences[i]));
    }
    printf("stage all-submitted\n");
    VkResult wait=vkWaitForFences(c.dev,JOBS,fences,VK_TRUE,10ull*1000000000ull);
    if(wait!=VK_SUCCESS)die("pipeline fence wait result %d",wait);
    printf("stage fences-complete\n");
    for(uint32_t i=0;i<JOBS;i++){
        printf("stage readback job%u\n",i);
        uint32_t* data=(uint32_t*)buffers[i].map;
        for(uint32_t k=0;k<WORDS;k++)if(data[k]!=(0x12340000u+i))++mismatches;
        printf("job%u pattern=%08x hash=%016llx\n",i,0x12340000u+i,(unsigned long long)fnv1a64(data,(size_t)WORDS*4));
        vkDestroyFence(c.dev,fences[i],NULL);buf_destroy(&c,&buffers[i]);
    }
    printf("RESULT %s jobs=%u repeats=%u verified_words=%u mismatches=%u prior_api_fence_pending=%u\n",
        mismatches?"FAIL":"PASS",JOBS,repetitions,JOBS*WORDS,mismatches,pending_before);
    vkFreeCommandBuffers(c.dev,c.pool,JOBS,commands);ctx_fini(&c);return mismatches?1:0;
}
