from pathlib import Path
import sys
p=Path(sys.argv[1]);s=p.read_text()
guard='        if(!Build->DmaSize || !Build->pDmaBufferPrivateData ||\n           !PagingPrivateQueuedDirectCapacity(Build->DmaBufferPrivateDataSize))\n            return STATUS_GRAPHICS_INSUFFICIENT_DMA_BUFFER;\n'
assert s.count(guard)==1
p.write_text(s.replace(guard,""))
