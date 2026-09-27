from pathlib import Path

p = Path('P:/BC-250/scratch/mesa-fork-2026-09-27/wt-radv/src/vulkan/wsi/wsi_common_win32.cpp')
t = p.read_text(encoding='utf-8')

old = '''                      uint64_t t_blit, uint64_t t_done, VkResult result)
{
   if (!chain->present_log)
      return;
   fprintf(chain->present_log,
           "%p,%s,%" PRIu64 ",%" PRIu64 ",%" PRIu64 ",%" PRIu64 ",%" PRIu64 ",%d\\n",
           (void *)chain, path, present_id, t_enter / 1000,
           (t_copy - t_enter) / 1000, (t_blit - t_copy) / 1000,
           (t_done - t_blit) / 1000, (int)result);'''
new = '''                      uint64_t t_blit, uint64_t t_done, VkResult result,
                      uint64_t copy_cycles)
{
   if (!chain->present_log)
      return;
   /* copy_cycles: CPU cycles this thread actually ran during the copy
    * (QueryThreadCycleTime); against copy_us it tells starvation from work.
    */
   fprintf(chain->present_log,
           "%p,%s,%" PRIu64 ",%" PRIu64 ",%" PRIu64 ",%" PRIu64 ",%" PRIu64 ",%d,%" PRIu64 "\\n",
           (void *)chain, path, present_id, t_enter / 1000,
           (t_copy - t_enter) / 1000, (t_blit - t_copy) / 1000,
           (t_done - t_blit) / 1000, (int)result, copy_cycles);'''
assert old in t
t = t.replace(old, new)

old = '"chain,path,present_id,enter_us,copy_us,blit_us,dwmflush_us,result\\n"'
assert old in t
t = t.replace(old, '"chain,path,present_id,enter_us,copy_us,blit_us,dwmflush_us,result,copy_cycles\\n"')

old = '''         wsi_win32_present_log(chain, "dxgi", present_id, t_enter, t_enter, t_blit,
                               os_time_get_nano(), result);'''
assert old in t
t = t.replace(old, '''         wsi_win32_present_log(chain, "dxgi", present_id, t_enter, t_enter, t_blit,
                               os_time_get_nano(), result, 0);''')

old = '''   const uint8_t *ptr = (const uint8_t *)image->base.cpu_map;
   uint8_t *dptr = (uint8_t *)image->sw.ppvBits;
   for (unsigned h = 0; h < chain->extent.height; h++) {'''
assert t.count(old) == 1
t = t.replace(old, '''   ULONG64 cycles_before = 0, cycles_after = 0;
   if (chain->present_log)
      QueryThreadCycleTime(GetCurrentThread(), &cycles_before);
   const uint8_t *ptr = (const uint8_t *)image->base.cpu_map;
   uint8_t *dptr = (uint8_t *)image->sw.ppvBits;
   for (unsigned h = 0; h < chain->extent.height; h++) {''')

old = '''   const uint64_t t_copy = chain->present_log ? os_time_get_nano() : 0;'''
assert t.count(old) == 1
t = t.replace(old, '''   const uint64_t t_copy = chain->present_log ? os_time_get_nano() : 0;
   if (chain->present_log)
      QueryThreadCycleTime(GetCurrentThread(), &cycles_after);''')

old = '''      wsi_win32_present_log(chain, "gdi", present_id, t_enter, t_copy, t_blit,
                            os_time_get_nano(), result);'''
assert old in t
t = t.replace(old, '''      wsi_win32_present_log(chain, "gdi", present_id, t_enter, t_copy, t_blit,
                            os_time_get_nano(), result, cycles_after - cycles_before);''')
p.write_text(t, encoding='utf-8', newline='\n')
print('patched')
