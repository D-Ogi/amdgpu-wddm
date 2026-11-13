#!/usr/bin/env python3
# SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
"""Makes the test images and databases for the Secure Boot host test (sb_test.c).

    python make_fixtures.py <output directory>

Each run makes new RSA keys in memory, signs small synthetic PE images with them, and writes only public data:
images, certificates inside signature lists, digests. No private key is written to a file, and none is in the
repository.

The keys and certificates come from the Python package "cryptography". The Authenticode SignedData is encoded
here, byte by byte, so the test does not depend on a signing tool. Exit code 3 says that the package is not
installed. The build script then skips the tests that need the fixtures and says so.
"""
import datetime
import hashlib
import os
import struct
import sys

sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "tools"))
import mkesl  # noqa: E402

try:
    from cryptography import x509
    from cryptography.hazmat.primitives import hashes, serialization
    from cryptography.hazmat.primitives.asymmetric import padding, rsa
    from cryptography.x509.oid import NameOID
except ImportError:
    print("make_fixtures: the Python package 'cryptography' is not installed", file=sys.stderr)
    sys.exit(3)

OID_SIGNED_DATA = bytes.fromhex("2A864886F70D010702")
OID_CONTENT_TYPE = bytes.fromhex("2A864886F70D010903")
OID_MESSAGE_DIGEST = bytes.fromhex("2A864886F70D010904")
OID_RSA = bytes.fromhex("2A864886F70D010101")
OID_SHA256 = bytes.fromhex("608648016503040201")
OID_SPC_INDIRECT_DATA = bytes.fromhex("2B060104018237020104")
OID_SPC_PE_IMAGE_DATA = bytes.fromhex("2B06010401823702010F")


def der(tag: int, content: bytes) -> bytes:
    n = len(content)
    if n < 0x80:
        return bytes([tag, n]) + content
    length = n.to_bytes((n.bit_length() + 7) // 8, "big")
    return bytes([tag, 0x80 | len(length)]) + length + content


def der_integer(value: int) -> bytes:
    return der(0x02, value.to_bytes(value.bit_length() // 8 + 1, "big"))


def algorithm(oid: bytes) -> bytes:
    return der(0x30, der(0x06, oid) + der(0x05, b""))


# ---- keys and certificates ---------------------------------------------------------------------------------------

class Party:
    """A key and its certificate."""

    def __init__(self, name: str, bits: int, issuer: "Party" = None, ca: bool = None):
        self.key = rsa.generate_private_key(public_exponent=65537, key_size=bits)
        subject = x509.Name([x509.NameAttribute(NameOID.COMMON_NAME, "BC-250 test " + name)])
        signer = issuer if issuer else self
        start = datetime.datetime(2026, 1, 1, tzinfo=datetime.timezone.utc)
        builder = (x509.CertificateBuilder().subject_name(subject).issuer_name(signer.certificate.subject if issuer else subject)
                   .public_key(self.key.public_key()).serial_number(x509.random_serial_number())
                   .not_valid_before(start).not_valid_after(start + datetime.timedelta(days=30)))
        if ca is not None:
            builder = builder.add_extension(x509.BasicConstraints(ca=ca, path_length=None), critical=True)
        self.certificate = builder.sign(signer.key, hashes.SHA256())
        self.der = self.certificate.public_bytes(serialization.Encoding.DER)


# ---- PE images ---------------------------------------------------------------------------------------------------

def pe_image(seed: bytes, plus: bool = True, reverse_sections: bool = False, trailing: bytes = b"") -> bytes:
    """A small PE image with two sections. It is never executed: the sections hold a byte pattern."""
    optional_bytes = 240 if plus else 224
    lfanew = 0x80
    dos = b"MZ" + bytes(0x3A) + struct.pack("<I", lfanew)
    dos += bytes(lfanew - len(dos))
    coff = struct.pack("<HHIIIHH", 0x8664 if plus else 0x14C, 2, 0, 0, 0, optional_bytes, 0x22)
    optional = bytearray(optional_bytes)
    struct.pack_into("<H", optional, 0, 0x20B if plus else 0x10B)
    struct.pack_into("<I", optional, 16, 0x1000)                   # AddressOfEntryPoint
    struct.pack_into("<II", optional, 32, 0x1000, 0x200)           # SectionAlignment, FileAlignment
    struct.pack_into("<I", optional, 56, 0x3000)                   # SizeOfImage
    struct.pack_into("<I", optional, 60, 0x200)                    # SizeOfHeaders
    struct.pack_into("<H", optional, 68, 10)                       # Subsystem: EFI application
    struct.pack_into("<I", optional, 108 if plus else 92, 16)      # NumberOfRvaAndSizes
    text = (hashlib.sha256(seed + b"text").digest() * 16)[:0x200]
    data = (hashlib.sha256(seed + b"data").digest() * 16)[:0x200]
    rows = [(b".text\0\0\0", 0x200), (b".data\0\0\0", 0x400)]
    if reverse_sections:
        rows.reverse()
    table = b""
    for name, pointer in rows:
        table += name + struct.pack("<IIIIIIHHI", 0x200, pointer * 8, 0x200, pointer, 0, 0, 0, 0, 0x60000020)
    headers = dos + b"PE\0\0" + coff + bytes(optional) + table
    headers += bytes(0x200 - len(headers))
    return headers + text + data + trailing


def security_entry(image: bytes) -> int:
    pe = struct.unpack_from("<I", image, 0x3C)[0]
    optional = pe + 24
    plus = struct.unpack_from("<H", image, optional)[0] == 0x20B
    return optional + (112 if plus else 96) + 4 * 8


def signed_data(digest: bytes, signer: Party, bag: list) -> bytes:
    """An Authenticode SignedData for an image with this digest."""
    pe_data = der(0x30, der(0x06, OID_SPC_PE_IMAGE_DATA) +
                  der(0x30, der(0x03, b"\0") + der(0xA0, der(0xA2, der(0x80, b"")))))
    content = pe_data + der(0x30, algorithm(OID_SHA256) + der(0x04, digest))
    attributes = (der(0x30, der(0x06, OID_CONTENT_TYPE) + der(0x31, der(0x06, OID_SPC_INDIRECT_DATA))) +
                  der(0x30, der(0x06, OID_MESSAGE_DIGEST) + der(0x31, der(0x04, hashlib.sha256(content).digest()))))
    signature = signer.key.sign(der(0x31, attributes), padding.PKCS1v15(), hashes.SHA256())
    issuer_and_serial = der(0x30, signer.certificate.issuer.public_bytes() + der_integer(signer.certificate.serial_number))
    signer_info = der(0x30, der_integer(1) + issuer_and_serial + algorithm(OID_SHA256) + der(0xA0, attributes) +
                      algorithm(OID_RSA) + der(0x04, signature))
    body = (der_integer(1) + der(0x31, algorithm(OID_SHA256)) +
            der(0x30, der(0x06, OID_SPC_INDIRECT_DATA) + der(0xA0, der(0x30, content))) +
            der(0xA0, b"".join(party.der for party in bag)) + der(0x31, signer_info))
    return der(0x30, der(0x06, OID_SIGNED_DATA) + der(0xA0, der(0x30, body)))


def sign(image: bytes, signatures: list) -> bytes:
    """Adds an attribute certificate table with one WIN_CERTIFICATE for each (signer, bag)."""
    image = image + bytes(-len(image) % 8)
    digest = mkesl.authenticode_sha256(image)
    table = b""
    for signer, bag in signatures:
        blob = signed_data(digest, signer, bag)
        entry = struct.pack("<IHH", 8 + len(blob), 0x0200, 0x0002) + blob
        table += entry + bytes(-len(entry) % 8)
    out = bytearray(image + table)
    struct.pack_into("<II", out, security_entry(image), len(image), len(table))
    return bytes(out)


def flip(image: bytes, offset: int) -> bytes:
    out = bytearray(image)
    out[offset] ^= 0x01
    return bytes(out)


def main() -> int:
    if len(sys.argv) != 2:
        print(__doc__, file=sys.stderr)
        return 2
    out = sys.argv[1]
    os.makedirs(out, exist_ok=True)

    def write(name: str, data: bytes) -> None:
        with open(os.path.join(out, name), "wb") as handle:
            handle.write(data)

    root = Party("root", 4096, ca=True)
    inter = Party("intermediate", 3072, issuer=root, ca=True)
    leaf = Party("signer", 2048, issuer=inter, ca=False)
    other = Party("other", 3072, ca=True)
    plain = Party("intermediate without the cA flag", 2048, issuer=root)
    leaf2 = Party("signer under it", 2048, issuer=plain, ca=False)

    unsigned = pe_image(b"one")
    unsigned32 = pe_image(b"two", plus=False, reverse_sections=True, trailing=b"tail!")
    signed = sign(unsigned, [(leaf, [leaf, inter])])
    write("unsigned.efi", unsigned)
    write("unsigned32.efi", unsigned32)
    write("signed.efi", signed)
    write("signed32.efi", sign(unsigned32, [(leaf, [inter, leaf])]))
    write("signed_tampered.efi", flip(signed, 0x210))                   # a byte of a section
    write("signed_badsig.efi", flip(signed, len(signed) - 12))          # a byte of the RSA signature
    write("signed_other.efi", sign(unsigned, [(other, [other])]))
    write("signed_noca.efi", sign(unsigned, [(leaf2, [leaf2, plain])]))
    write("signed_two.efi", sign(unsigned, [(other, [other]), (leaf, [leaf, inter])]))
    write("signed_nobag.efi", sign(unsigned, [(leaf, [])]))             # the signer's certificate is not there
    broken = bytearray(signed)
    struct.pack_into("<I", broken, security_entry(signed) + 4, 64)      # the table does not end the file
    write("signed_badtable.efi", bytes(broken))
    write("not_pe.bin", b"This is not a PE image." * 8)

    for name, image in (("unsigned.efi", unsigned), ("unsigned32.efi", unsigned32), ("signed.efi", signed)):
        write(name + ".digest", mkesl.authenticode_sha256(image))

    digest = mkesl.authenticode_sha256(unsigned)
    write("db_root.esl", mkesl.database([root.der], []))
    write("db_inter.esl", mkesl.database([inter.der], []))
    write("db_leaf.esl", mkesl.database([leaf.der], []))
    write("db_digest.esl", mkesl.database([], [digest]))
    write("db_mixed.esl", mkesl.signature_list(mkesl.uuid.UUID(int=7), mkesl.BC250_OWNER_GUID, [b"unknown type"]) +
          mkesl.database([other.der, root.der], [hashlib.sha256(b"x").digest(), digest]))
    write("dbx_leaf.esl", mkesl.database([leaf.der], []))
    write("dbx_inter.esl", mkesl.database([inter.der], []))
    write("dbx_root.esl", mkesl.database([root.der], []))
    write("dbx_digest.esl", mkesl.database([], [digest]))
    write("bad.esl", mkesl.database([root.der], [])[:-3])
    print(f"fixtures written to {out}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
