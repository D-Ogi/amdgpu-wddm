using System.Reflection;
using System.Runtime.Versioning;

[assembly: AssemblyTitle("Nagi Setup")]
[assembly: AssemblyProduct("Nagi")]
[assembly: AssemblyDescription("Setup window of Nagi (amdgpu-wddm), the graphics driver for the ASRock BC-250")]
[assembly: AssemblyVersion("0.1.0.0")]
[assembly: AssemblyFileVersion("0.1.0.0")]
// csc does not add this attribute by itself (see the control app's AssemblyInfo.cs); build.ps1 checks the image for it.
[assembly: TargetFramework(".NETFramework,Version=v4.8", FrameworkDisplayName = ".NET Framework 4.8")]
