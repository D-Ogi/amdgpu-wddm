
#define CHECK(c) do{if(!(c)){fprintf(stderr,"FAIL line %d: %s\n",__LINE__,#c);return 1;}}while(0)
int main(void){
 WDDM w={0};DEVICE d={&w,1};device=&d;w.VSyncArmed=1;
 Stop(&d);
 CHECK(priorReaderDone && flushed==1 && d.Wddm==NULL);
 CHECK(lateReader==NULL); /* otherwise it can dereference the object after free */
 CHECK(w.Stopping && !w.VSyncArmed && !d.DcnVsyncArmed && !badOrder);
 CHECK(cancelled==4 && removed==4 && vsyncDisabled==1);
 memset(&w,0,sizeof(w));d.Wddm=&w;d.DcnVsyncArmed=0;flushed=priorReaderDone=cancelled=removed=vsyncDisabled=0;
 Stop(&d);CHECK(lateReader==NULL && priorReaderDone && !badOrder && cancelled==3 && removed==4 && vsyncDisabled==0);
 puts("PASS: actual stop order detaches before DPC flush boundary, joins earlier reader, blocks late pointer capture, cancels timers/reports with vsync on/off");return 0;
}
