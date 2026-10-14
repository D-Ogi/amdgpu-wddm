using System;
using System.IO;
using System.Linq;
using System.Runtime.InteropServices;
using System.Security.Cryptography;
using System.Security.Cryptography.Pkcs;
using System.Security.Cryptography.X509Certificates;

namespace AmdgpuWddm.Release
{
    // Test-certificate verification without importing a root into the host.
    // Both the signed content and the PE digest must verify before chain policy.
    public static class DriverSignature
    {
        [DefaultDllImportSearchPaths(DllImportSearchPath.System32)]
        [DllImport("wintrust.dll", CharSet = CharSet.Unicode, SetLastError = true)]
        static extern bool CryptCATAdminAcquireContext2(out IntPtr context, IntPtr subsystem,
            string algorithm, IntPtr policy, uint flags);
        [DefaultDllImportSearchPaths(DllImportSearchPath.System32)]
        [DllImport("wintrust.dll", SetLastError = true)]
        static extern bool CryptCATAdminCalcHashFromFileHandle2(IntPtr context, IntPtr file,
            ref uint size, byte[] digest, uint flags);
        [DefaultDllImportSearchPaths(DllImportSearchPath.System32)]
        [DllImport("wintrust.dll")]
        static extern bool CryptCATAdminReleaseContext(IntPtr context, uint flags);

        sealed class Der
        {
            readonly byte[] bytes;
            int at;
            public Der(byte[] value) { bytes = value; }
            public bool End { get { return at == bytes.Length; } }
            public byte[] Read(int tag)
            {
                if (at + 2 > bytes.Length || bytes[at++] != tag) throw new InvalidDataException("DER tag");
                int count = bytes[at++];
                if ((count & 128) != 0)
                {
                    int n = count & 127;
                    if (n == 0 || n > 4 || at + n > bytes.Length || bytes[at] == 0)
                        throw new InvalidDataException("DER length");
                    count = 0;
                    for (int i = 0; i < n; ++i) count = checked(count * 256 + bytes[at++]);
                    if (count < 128) throw new InvalidDataException("noncanonical DER length");
                }
                if (count > bytes.Length - at) throw new InvalidDataException("truncated DER");
                byte[] value = new byte[count]; Array.Copy(bytes, at, value, 0, count); at += count;
                return value;
            }
            public void Done() { if (!End) throw new InvalidDataException("extra DER data"); }
        }

        static byte[] SignedDigest(byte[] content)
        {
            var outer = new Der(content); var body = new Der(outer.Read(0x30)); outer.Done();
            body.Read(0x30); // SpcAttributeTypeAndOptionalValue
            var digest = new Der(body.Read(0x30)); body.Done();
            var algorithm = new Der(digest.Read(0x30));
            byte[] sha256 = { 0x60,0x86,0x48,0x01,0x65,0x03,0x04,0x02,0x01 };
            if (!algorithm.Read(6).SequenceEqual(sha256)) throw new InvalidDataException("not SHA-256");
            if (!algorithm.End && algorithm.Read(5).Length != 0) throw new InvalidDataException("algorithm parameters");
            algorithm.Done(); byte[] result = digest.Read(4); digest.Done();
            if (result.Length != 32) throw new InvalidDataException("digest length");
            return result;
        }

        public static void Verify(string path, string rootFile)
        {
            using (var file = new FileStream(path, FileMode.Open, FileAccess.Read, FileShare.Read))
            using (var reader = new BinaryReader(file))
            using (var root = new X509Certificate2(rootFile))
            {
                if (string.Equals(Path.GetExtension(path), ".cat", StringComparison.OrdinalIgnoreCase))
                {
                    if (file.Length == 0 || file.Length > 16 * 1024 * 1024) throw new InvalidDataException("catalog extent");
                    var catalog = new SignedCms(); catalog.Decode(reader.ReadBytes((int)file.Length));
                    // SDK wincrypt.h: szOID_CTL, the signed catalog content.
                    if (catalog.ContentInfo.ContentType.Value != "1.3.6.1.4.1.311.10.1")
                        throw new InvalidDataException("catalog content type");
                    VerifySigner(catalog, root);
                    return;
                }
                if (file.Length < 64 || reader.ReadUInt16() != 0x5a4d) throw new InvalidDataException("not PE");
                file.Position = 0x3c; uint pe = reader.ReadUInt32();
                if (pe > file.Length - 24) throw new InvalidDataException("PE offset");
                file.Position = pe;
                if (reader.ReadUInt32() != 0x4550) throw new InvalidDataException("PE signature");
                file.Position = pe + 20; int optionalSize = reader.ReadUInt16();
                file.Position = pe + 24; int magic = reader.ReadUInt16();
                int directory = magic == 0x20b ? 112 : magic == 0x10b ? 96 : 0;
                if (directory == 0 || optionalSize < directory + 40) throw new InvalidDataException("PE optional header");
                file.Position = pe + 24 + directory + 32;
                uint offset = reader.ReadUInt32(), size = reader.ReadUInt32();
                if (size < 8 || size > 16 * 1024 * 1024 || offset < pe + 24 + optionalSize ||
                    (ulong)offset + size != (ulong)file.Length || (offset & 7) != 0)
                    throw new InvalidDataException("certificate table extent");
                file.Position = offset;
                uint length = reader.ReadUInt32();
                if (length < 8 || ((ulong)length + 7 & ~7UL) != size || reader.ReadUInt16() != 0x200 || reader.ReadUInt16() != 2)
                    throw new InvalidDataException("WIN_CERTIFICATE");
                var cms = new SignedCms(); cms.Decode(reader.ReadBytes(checked((int)length - 8)));
                while (file.Position < file.Length) if (reader.ReadByte() != 0) throw new InvalidDataException("certificate padding");
                if (cms.ContentInfo.ContentType.Value != "1.3.6.1.4.1.311.2.1.4" || cms.SignerInfos.Count != 1)
                    throw new InvalidDataException("Authenticode signed content");
                cms.CheckSignature(true);
                byte[] expected = SignedDigest(cms.ContentInfo.Content);
                IntPtr context;
                if (!CryptCATAdminAcquireContext2(out context, IntPtr.Zero, "SHA256", IntPtr.Zero, 0))
                    throw new CryptographicException(Marshal.GetLastWin32Error());
                try
                {
                    uint count = 32; byte[] actual = new byte[32]; file.Position = 0;
                    if (!CryptCATAdminCalcHashFromFileHandle2(context, file.SafeFileHandle.DangerousGetHandle(), ref count, actual, 0))
                        throw new CryptographicException(Marshal.GetLastWin32Error());
                    if (count != 32 || !actual.SequenceEqual(expected)) throw new CryptographicException("PE digest mismatch");
                }
                finally { CryptCATAdminReleaseContext(context, 0); }
                VerifySigner(cms, root);
            }
        }

        static void VerifySigner(SignedCms cms, X509Certificate2 root)
        {
                if (cms.SignerInfos.Count != 1 || cms.SignerInfos[0].DigestAlgorithm.Value != "2.16.840.1.101.3.4.2.1")
                    throw new CryptographicException("one SHA-256 signer required");
                cms.CheckSignature(true);
                X509Certificate2 signer = cms.SignerInfos[0].Certificate;
                if (signer == null || !signer.Extensions.OfType<X509EnhancedKeyUsageExtension>().Any(
                    e => e.EnhancedKeyUsages.Cast<Oid>().Any(o => o.Value == "1.3.6.1.5.5.7.3.3")))
                    throw new CryptographicException("missing code-signing EKU");
                using (var chain = new X509Chain())
                {
                    chain.ChainPolicy.ExtraStore.AddRange(cms.Certificates);
                    chain.ChainPolicy.ExtraStore.Add(root);
                    chain.ChainPolicy.ApplicationPolicy.Add(new Oid("1.3.6.1.5.5.7.3.3"));
                    chain.ChainPolicy.RevocationMode = X509RevocationMode.NoCheck;
                    chain.ChainPolicy.VerificationFlags = X509VerificationFlags.AllowUnknownCertificateAuthority;
                    if (!chain.Build(signer) || chain.ChainElements.Count == 0 ||
                        chain.ChainElements[chain.ChainElements.Count - 1].Certificate.Thumbprint != root.Thumbprint ||
                        chain.ChainStatus.Any(s => s.Status != X509ChainStatusFlags.NoError && s.Status != X509ChainStatusFlags.UntrustedRoot))
                        throw new CryptographicException("signature chain does not satisfy the pinned test-root policy");
                }
        }
    }
}
