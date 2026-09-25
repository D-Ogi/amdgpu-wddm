from pathlib import Path
p=Path('scratch/m12/mesa-current-src/src/gallium/drivers/zink/zink_resource.c');s=p.read_text();needle='   ici->extent.width = templ->width0;';assert s.count(needle)==1;s=s.replace(needle,'''   if (!templ->width0 || !templ->height0 || !templ->depth0) {
      fprintf(stderr, "BC250 zink zero image: target=%u format=%u size=%ux%ux%u bind=%x\\n",
              templ->target, templ->format, templ->width0, templ->height0, templ->depth0, bind);
      fflush(stderr);
   }
'''+needle);p.write_text(s)
