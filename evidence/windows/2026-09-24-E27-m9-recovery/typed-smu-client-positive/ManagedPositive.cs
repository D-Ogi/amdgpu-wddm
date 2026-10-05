using System;
using Bc250Mon;
class Positive {
 static int Main() {
  using(var driver=new Driver()) {
   var read=driver.ReadClock();var set=driver.SetClock(1000,820);
   if(read.ObservedMHz!=1000 || read.ObservedVid!=116 || read.TemperatureMc!=67500 || set.Ready!=1 || driver.ReadTemperature()!=67.5)return 1;
   Console.WriteLine("Actual managed Driver READ/SET and temperature passed with fixture DLL; no hardware access");
  }
  return 0;
 }
}
