using System.Reflection;
using System.Runtime.Versioning;

[assembly: AssemblyTitle("amdgpu-wddm Control")]
[assembly: AssemblyProduct("amdgpu-wddm")]
[assembly: AssemblyDescription("Control application for the amdgpu-wddm driver on the ASRock BC-250")]
[assembly: AssemblyVersion("0.7.0.0")]
[assembly: AssemblyFileVersion("0.7.0.0")]
// csc does not add this attribute by itself; without it the runtime treats the exe as an old-framework image and
// SecurityProtocolType.SystemDefault would not mean "the operating system's choice" (review 916 A5). build.ps1 checks
// the built image for the string.
[assembly: TargetFramework(".NETFramework,Version=v4.8", FrameworkDisplayName = ".NET Framework 4.8")]
