from pathlib import Path
p=Path('scratch/m12/mesa-current-src/src/gallium/drivers/zink/zink_kopper.c');s=p.read_text();needle='   error = VKSCR(CreateSwapchainKHR)(screen->dev, &cswap->scci, NULL,';assert s.count(needle)==2;s=s.replace(needle,'''   fprintf(stderr, "BC250 kopper swapchain: type=%u requested=%ux%u caps=%ux%u chosen=%ux%u\\n",
           cdt->type, w, h, cdt->caps.currentExtent.width, cdt->caps.currentExtent.height,
           cswap->scci.imageExtent.width, cswap->scci.imageExtent.height);
   fflush(stderr);
'''+needle,1);p.write_text(s)
