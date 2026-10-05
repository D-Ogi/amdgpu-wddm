using System;using System.Diagnostics;using System.Threading;
class CpuMemory146 {
 static ulong sink;
 static void Main(){
  Console.WriteLine("utc="+DateTime.UtcNow.ToString("o"));
  const int n=8*1024*1024;var a=new ulong[n];for(int i=0;i<n;i++)a[i]=(ulong)i;
  ulong expected=(ulong)n*(n-1)/2;
  for(int k=0;k<4;k++){var sw=Stopwatch.StartNew();ulong sum=0;for(int i=0;i<n;i++)sum+=a[i];sw.Stop();Console.WriteLine("read pass="+k+" bytes="+(n*8)+" elapsed_ms="+sw.Elapsed.TotalMilliseconds.ToString("F3",System.Globalization.CultureInfo.InvariantCulture)+" correct="+(sum==expected));if(sum!=expected)Environment.Exit(2);sink=sum;}
  long end=Stopwatch.GetTimestamp()+5*Stopwatch.Frequency;int count=Math.Min(Environment.ProcessorCount,12);var ts=new Thread[count];var counts=new long[count];
  for(int i=0;i<count;i++){int index=i;ts[i]=new Thread(()=>{ulong v=(ulong)index+1;long chunks=0;while(Stopwatch.GetTimestamp()<end){for(int j=0;j<100000;j++){v^=v<<13;v^=v>>7;v^=v<<17;}chunks++;}counts[index]=chunks;Interlocked.Exchange(ref checksum,unchecked((long)v));});ts[i].IsBackground=true;ts[i].Start();}
  foreach(var t in ts)t.Join();long total=0;foreach(long c in counts)total+=c;
  Console.WriteLine("alu duration_s=5 threads="+count+" iterations="+(total*100000)+" checksum="+checksum+" sink="+sink);Console.WriteLine("done="+DateTime.UtcNow.ToString("o"));
 }
 static long checksum;
}
