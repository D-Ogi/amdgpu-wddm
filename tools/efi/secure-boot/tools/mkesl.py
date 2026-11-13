#!/usr/bin/env python3
# SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
"""Makes a signature database file (db.esl, dbx.esl) for the Secure Boot module of the BC-250 stage.

The file is one or more EFI_SIGNATURE_LIST structures, the format of the UEFI db and dbx variables. An X.509
certificate goes in as EFI_CERT_X509_GUID. A PE image goes in as its Authenticode SHA-256 digest
(EFI_CERT_SHA256_GUID), which allows or forbids exactly these bytes.

    python mkesl.py --out db.esl --cert windows-pca.cer --image another.efi
    python mkesl.py --out dbx.esl --sha256 <64 hex digits>
    python mkesl.py --digest bootmgfw.efi          # prints the Authenticode SHA-256 digest and writes nothing

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


def signature_list(type_guid: uuid.UUID, owner: uuid.UUID, entries: list) -> bytes:
    """One EFI_SIGNATURE_LIST. All entries of a list have the same size."""
    sizes = {len(entry) for entry in entries}
    if len(sizes) != 1:
        raise FormatError("the entries of one list must have the same size")
    entry_bytes = 16 + sizes.pop()
    body = b"".join(owner.bytes_le + entry for entry in entries)
    return type_guid.bytes_le + struct.pack("<III", 28 + len(body), 0, entry_bytes) + body


def database(certificates: list, digests: list, owner: uuid.UUID = BC250_OWNER_GUID) -> bytes:
    """A database: one list for each certificate (their sizes differ) and one list for all digests."""
    out = b"".join(signature_list(EFI_CERT_X509_GUID, owner, [certificate]) for certificate in certificates)
    if digests:
        out += signature_list(EFI_CERT_SHA256_GUID, owner, digests)
    return out


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--out", help="the database file to write")
    parser.add_argument("--cert", action="append", default=[], help="an X.509 certificate, DER or PEM")
    parser.add_argument("--image", action="append", default=[], help="a PE image: its Authenticode digest goes in")
    parser.add_argument("--sha256", action="append", default=[], help="an Authenticode SHA-256 digest, 64 hex digits")
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
        if not certificates and not digests:
            parser.error("give one or more of --cert, --image, --sha256")
        data = database(certificates, digests, uuid.UUID(args.owner))
    except (FormatError, OSError, ValueError) as error:
        print(f"mkesl: {error}", file=sys.stderr)
        return 1
    with open(args.out, "wb") as handle:
        handle.write(data)
    print(f"{args.out}: {len(certificates)} certificates, {len(digests)} digests, {len(data)} bytes")
    return 0


if __name__ == "__main__":
    sys.exit(main())
