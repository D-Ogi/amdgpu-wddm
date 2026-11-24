#!/usr/bin/env python3
# SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
"""Makes a signature database file (db.esl, dbx.esl) for the Secure Boot module of the BC-250 stage.

The file is one or more EFI_SIGNATURE_LIST structures, the format of the UEFI db and dbx variables. An X.509
certificate goes in as EFI_CERT_X509_GUID. A PE image goes in as its Authenticode SHA-256 digest
(EFI_CERT_SHA256_GUID), which allows or forbids exactly these bytes.

    python mkesl.py --out db.esl --cert windows-pca.cer --image another.efi
    python mkesl.py --out dbx.esl --sha256 <64 hex digits> --revoke-cert old-ca.cer
    python mkesl.py --out dbx.esl --empty          # a database that forbids nothing
    python mkesl.py --digest bootmgfw.efi          # prints the Authenticode SHA-256 digest and writes nothing

--empty writes one list with one image digest of 32 zero bytes. No image has that Authenticode digest, so the list
forbids nothing. It is not a list without an entry: the Windows Boot Manager of 26300 answers EFI_INVALID_PARAMETER
at once when the stage serves such a list as dbx with sb.windows=1 (BD-386).

The tool needs only the Python standard library. It reads and writes files. It writes no UEFI variable.
"""
import argparse
import base64
import hashlib
import struct
import sys
import uuid

# The type GUIDs of the UEFI Specification (edk2 MdePkg/Include/Guid/ImageAuthentication.h).
EFI_CERT_X509_GUID = uuid.UUID("a5c059a1-94e4-4aa7-87b5-ab155c2bf072")
EFI_CERT_SHA256_GUID = uuid.UUID("c1c41626-504c-4092-aca9-41f936934328")
EFI_CERT_X509_SHA256_GUID = uuid.UUID("3bd2a492-96c0-4079-b420-fcf98ef103ed")
# The owner GUID of the entries that this project makes. An owner GUID only names who added an entry.
BC250_OWNER_GUID = uuid.UUID("2e9685e0-ca1d-4d95-807f-6cc9d9865143")


class FormatError(ValueError):
    pass


def authenticode_sha256(image: bytes) -> bytes:
    """The SHA-256 Authenticode digest of a PE image ("Windows Authenticode Portable Executable Signature Format",
    section "Calculating the PE Image Hash")."""
    if len(image) < 0x40 or image[:2] != b"MZ":
        raise FormatError("not a PE image: no MZ header")
    pe = struct.unpack_from("<I", image, 0x3C)[0]
    if image[pe:pe + 4] != b"PE\0\0":
        raise FormatError("not a PE image: no PE signature")
    sections, = struct.unpack_from("<H", image, pe + 6)
    optional_bytes, = struct.unpack_from("<H", image, pe + 20)
    optional = pe + 24
    magic, = struct.unpack_from("<H", image, optional)
    if magic == 0x10B:
        count_at, directories = optional + 92, optional + 96
    elif magic == 0x20B:
        count_at, directories = optional + 108, optional + 112
    else:
        raise FormatError("not a PE32 or PE32+ image")
    headers, = struct.unpack_from("<I", image, optional + 60)
    checksum = optional + 64
    count, = struct.unpack_from("<I", image, count_at)
    end = len(image)
    digest = hashlib.sha256()
    digest.update(image[:checksum])
    if count > 4:
        security = directories + 4 * 8
        table_at, table_bytes = struct.unpack_from("<II", image, security)
        if table_bytes:
            if table_at + table_bytes != len(image):
                raise FormatError("the certificate table is not at the end of the file")
            end = table_at
        digest.update(image[checksum + 4:security])
        digest.update(image[security + 8:headers])
    else:
        digest.update(image[checksum + 4:headers])
    table = optional + optional_bytes
    ranges = []
    for index in range(sections):
        size, pointer = struct.unpack_from("<II", image, table + 40 * index + 16)
        if size:
            ranges.append((pointer, size))
    position = headers
    for pointer, size in sorted(ranges):
        if pointer + size > end:
            raise FormatError("a section reaches past the end of the image data")
        digest.update(image[pointer:pointer + size])
        position += size
    if position > end:
        raise FormatError("the sections are larger than the file")
    digest.update(image[position:end])
    return digest.digest()


def read_certificate(path: str) -> bytes:
    """A certificate file as DER. PEM is accepted."""
    with open(path, "rb") as handle:
        data = handle.read()
    if b"-----BEGIN CERTIFICATE-----" in data:
        body = data.split(b"-----BEGIN CERTIFICATE-----", 1)[1].split(b"-----END CERTIFICATE-----", 1)[0]
        data = base64.b64decode(b"".join(body.split()))
    if not data or data[0] != 0x30:
        raise FormatError(f"{path}: not a DER or PEM certificate")
    return data


def signed_part_sha256(certificate: bytes) -> bytes:
    """The SHA-256 digest of the TBSCertificate of a DER certificate, with its header (EFI_CERT_X509_SHA256)."""

    def element(data: bytes, at: int):
        """The offset of the content and the offset after the element at this offset."""
        if at + 2 > len(data):
            raise FormatError("the certificate is cut")
        length = data[at + 1]
        content = at + 2
        if length & 0x80:
            count = length & 0x7F
            if count == 0 or count > 4 or content + count > len(data):
                raise FormatError("the certificate has a length that this tool does not read")
            length = int.from_bytes(data[content:content + count], "big")
            content += count
        if content + length > len(data):
            raise FormatError("the certificate is cut")
        return content, content + length

    if not certificate or certificate[0] != 0x30:
        raise FormatError("not a DER certificate")
    content, _ = element(certificate, 0)
    if certificate[content] != 0x30:
        raise FormatError("not a DER certificate")
    _, end = element(certificate, content)
    return hashlib.sha256(certificate[content:end]).digest()


def empty_list(owner: uuid.UUID = None) -> bytes:
    """A well-formed database with no entry: one list of image digests that holds none."""
    return EFI_CERT_SHA256_GUID.bytes_le + struct.pack("<III", 28, 0, 16 + 32)


def signature_list(type_guid: uuid.UUID, owner: uuid.UUID, entries: list) -> bytes:
    """One EFI_SIGNATURE_LIST. All entries of a list have the same size."""
    sizes = {len(entry) for entry in entries}
    if len(sizes) != 1:
        raise FormatError("the entries of one list must have the same size")
    entry_bytes = 16 + sizes.pop()
    body = b"".join(owner.bytes_le + entry for entry in entries)
    return type_guid.bytes_le + struct.pack("<III", 28 + len(body), 0, entry_bytes) + body


def forbids_nothing(owner: uuid.UUID = BC250_OWNER_GUID) -> bytes:
    """A database that forbids nothing and that the Windows Boot Manager takes: one list with the image digest of
    32 zero bytes. The file that the operator puts beside the stage is this one (the --empty of the command line).
    The tests of the reader use the list without an entry (empty_list)."""
    return signature_list(EFI_CERT_SHA256_GUID, owner, [bytes(32)])


def database(certificates: list, digests: list, owner: uuid.UUID = BC250_OWNER_GUID, revoked: list = ()) -> bytes:
    """A database: one list for each certificate (their sizes differ), one list for all image digests, and one
    list for the revoked certificates. A revoked certificate goes in as the digest of its signed part with a
    revocation time of zero, which means "at every time". With no entry at all the database is one empty list."""
    out = b"".join(signature_list(EFI_CERT_X509_GUID, owner, [certificate]) for certificate in certificates)
    if digests:
        out += signature_list(EFI_CERT_SHA256_GUID, owner, digests)
    if revoked:
        out += signature_list(EFI_CERT_X509_SHA256_GUID, owner,
                              [signed_part_sha256(certificate) + bytes(16) for certificate in revoked])
    return out if out else empty_list()


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--out", help="the database file to write")
    parser.add_argument("--cert", action="append", default=[], help="an X.509 certificate, DER or PEM")
    parser.add_argument("--image", action="append", default=[], help="a PE image: its Authenticode digest goes in")
    parser.add_argument("--sha256", action="append", default=[], help="an Authenticode SHA-256 digest, 64 hex digits")
    parser.add_argument("--revoke-cert", action="append", default=[],
                        help="an X.509 certificate that is forbidden: the digest of its signed part goes in")
    parser.add_argument("--empty", action="store_true",
                        help="write a database that forbids nothing: one list with the image digest of 32 zero bytes")
    parser.add_argument("--owner", default=str(BC250_OWNER_GUID), help="the owner GUID of the entries")
    parser.add_argument("--digest", help="print the Authenticode SHA-256 digest of this PE image and stop")
    args = parser.parse_args()
    try:
        if args.digest:
            with open(args.digest, "rb") as handle:
                print(authenticode_sha256(handle.read()).hex())
            return 0
        if not args.out:
            parser.error("--out or --digest is necessary")
        certificates = [read_certificate(path) for path in args.cert]
        digests = []
        for path in args.image:
            with open(path, "rb") as handle:
                digests.append(authenticode_sha256(handle.read()))
        for text in args.sha256:
            value = bytes.fromhex(text)
            if len(value) != 32:
                raise FormatError("--sha256 needs 64 hex digits")
            digests.append(value)
        revoked = [read_certificate(path) for path in args.revoke_cert]
        if not certificates and not digests and not revoked and not args.empty:
            parser.error("give one or more of --cert, --image, --sha256, --revoke-cert, or --empty")
        owner = uuid.UUID(args.owner)
        data = database(certificates, digests, owner, revoked) if (certificates or digests or revoked) \
            else forbids_nothing(owner)
    except (FormatError, OSError, ValueError) as error:
        print(f"mkesl: {error}", file=sys.stderr)
        return 1
    with open(args.out, "wb") as handle:
        handle.write(data)
    print(f"{args.out}: {len(certificates)} certificates, {len(digests)} digests, {len(revoked)} revoked "
          f"certificates, {len(data)} bytes")
    return 0


if __name__ == "__main__":
    sys.exit(main())
