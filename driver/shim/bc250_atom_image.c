/* PROVENANCE: AMD ATOM/VFCT layouts (MIT), Linux v6.18 7d0a66e4.
 * Unlike amdgpu_acpi_vfct_bios/check_atom_bios, check all lengths before dereference,
 * reject duplicate matching images and check identity in both VFCT and the PCI ROM. */
#include "bc250_atom_image.h"
#include <string.h>
#include "atom_layouts.inc"

/* PCI Firmware Specification ROM/PCIR fields; ATOM header offsets from AMD atom.h. */
enum {
    PciRomPcir = 0x18, PciDataMinimum = 0x18, PciDataVendor = 4, PciDataDevice = 6,
    PciDataLength = 0x0a, PciDataImageLength = 0x10,
    AtomRomHeader = 0x48, AtomRomMagic = 4, AtomRomCommand = 0x1e, AtomRomData = 0x20,
    AtomHeaderMinimum = 0x22, AtomTableHeader = 4, AtomCommandHeader = 6
};

typedef char VfctLayoutSize[(sizeof(struct uefi_acpi_vfct) == 76) ? 1 : -1];
typedef char VfctImageSize[(sizeof(struct vfct_image_header) == 28) ? 1 : -1];
typedef char AtomTableSize[(sizeof(struct atom_common_table_header) == 4) ? 1 : -1];

static uint16_t Read16(const uint8_t *b, size_t p)
{ return (uint16_t)((uint16_t)b[p] | (uint16_t)((uint16_t)b[p+1] << 8)); }
static uint32_t Read32(const uint8_t *b, size_t p)
{ return (uint32_t)Read16(b,p) | ((uint32_t)Read16(b,p+2) << 16); }
static int Fits(size_t total, size_t p, size_t n)
{ return p <= total && n <= total-p; }
static int Checksum(const uint8_t *b, size_t n)
{
    unsigned sum = 0;
    size_t i;
    for (i=0; i<n; ++i) sum = (sum + b[i]) & 255u;
    return sum == 0;
}

static BC250_ATOM_STATUS Master(const uint8_t *b, size_t n, uint16_t p, uint16_t *count)
{
    unsigned size;
    if (!p || !Fits(n,p,AtomTableHeader)) return BC250_ATOM_BOUNDS;
    size = Read16(b,p);
    if (size < AtomTableHeader || (size-AtomTableHeader)%2 || !Fits(n,p,size))
        return BC250_ATOM_BOUNDS;
    /* This parser uses the v2.1 command/data layouts, not a guessed index map. */
    if (b[p+2] != 2 || b[p+3] != 1) return BC250_ATOM_VERSION;
    *count = (uint16_t)((size-AtomTableHeader)/2);
    return BC250_ATOM_OK;
}

BC250_ATOM_STATUS Bc250AtomImageOpen(const void *bytes, size_t length,
    uint16_t vendor, uint16_t device, BC250_ATOM_IMAGE *out)
{
    const uint8_t *b = bytes;
    BC250_ATOM_IMAGE image = {0};
    BC250_ATOM_STATUS status;
    unsigned declared, pcir, pcirLength, header, headerLength;
    if (!out) return BC250_ATOM_ARGUMENT;
    memset(out,0,sizeof(*out));
    if (!b || !vendor || !device) return BC250_ATOM_ARGUMENT;
    if (length < AtomRomHeader+2 || length > BC250_ATOM_MAX_IMAGE) return BC250_ATOM_BOUNDS;
    if (Read16(b,0) != 0xaa55) return BC250_ATOM_SIGNATURE;
    declared = (unsigned)b[2]*512u;
    if (declared < AtomRomHeader+2 || declared > length) return BC250_ATOM_BOUNDS;
    if (!Checksum(b,declared)) return BC250_ATOM_CHECKSUM;
    pcir = Read16(b,PciRomPcir);
    if (!pcir || !Fits(declared,pcir,PciDataMinimum)) return BC250_ATOM_BOUNDS;
    if (memcmp(b+pcir,"PCIR",4)) return BC250_ATOM_SIGNATURE;
    pcirLength = Read16(b,pcir+PciDataLength);
    if (pcirLength < PciDataMinimum || !Fits(declared,pcir,pcirLength)) return BC250_ATOM_BOUNDS;
    if ((unsigned)Read16(b,pcir+PciDataImageLength)*512u != declared) return BC250_ATOM_BOUNDS;
    if (Read16(b,pcir+PciDataVendor) != vendor || Read16(b,pcir+PciDataDevice) != device)
        return BC250_ATOM_IDENTITY_MISMATCH;
    header = Read16(b,AtomRomHeader);
    if (!header || !Fits(declared,header,AtomHeaderMinimum)) return BC250_ATOM_BOUNDS;
    headerLength = Read16(b,header);
    if (headerLength < AtomHeaderMinimum || !Fits(declared,header,headerLength)) return BC250_ATOM_BOUNDS;
    if (memcmp(b+header+AtomRomMagic,"ATOM",4)) return BC250_ATOM_SIGNATURE;
    image.CommandMaster = Read16(b,header+AtomRomCommand);
    image.DataMaster = Read16(b,header+AtomRomData);
    status = Master(b,declared,image.CommandMaster,&image.CommandCount);
    if (status != BC250_ATOM_OK) return status;
    status = Master(b,declared,image.DataMaster,&image.DataCount);
    if (status != BC250_ATOM_OK) return status;
    image.Bytes = b;
    image.Length = declared;
    *out = image;
    return BC250_ATOM_OK;
}

BC250_ATOM_STATUS Bc250AtomGetTable(const BC250_ATOM_IMAGE *image, unsigned index,
    int command, BC250_ATOM_TABLE *out)
{
    unsigned offset, size, master, count;
    const uint8_t *b;
    if (!out) return BC250_ATOM_ARGUMENT;
    memset(out,0,sizeof(*out));
    if (!image || !image->Bytes || image->Length > BC250_ATOM_MAX_IMAGE) return BC250_ATOM_ARGUMENT;
    b = image->Bytes;
    master = command ? image->CommandMaster : image->DataMaster;
    count = command ? image->CommandCount : image->DataCount;
    if (index >= count || !Fits(image->Length,master,AtomTableHeader+2u*count)) return BC250_ATOM_BOUNDS;
    offset = Read16(b,master+AtomTableHeader+2u*index);
    if (!offset) return BC250_ATOM_NOT_FOUND;
    if (!Fits(image->Length,offset,AtomTableHeader)) return BC250_ATOM_BOUNDS;
    size = Read16(b,offset);
    if (size < (unsigned)(command ? AtomCommandHeader : AtomTableHeader) || !Fits(image->Length,offset,size))
        return BC250_ATOM_BOUNDS;
    out->Offset = offset;
    out->Length = size;
    out->Format = b[offset+2];
    out->Content = b[offset+3];
    if (command) {
        out->WorkspaceDwords = b[offset+4];
        out->ParameterBytes = b[offset+5] & 127u;
        if (out->ParameterBytes % 4u) {
            memset(out,0,sizeof(*out));
            return BC250_ATOM_PARAMETER;
        }
    }
    return BC250_ATOM_OK;
}

BC250_ATOM_STATUS Bc250AtomVfctSelect(const void *bytes, size_t length,
    const BC250_ATOM_IDENTITY *identity, BC250_ATOM_IMAGE *out)
{
    const uint8_t *b = bytes;
    BC250_ATOM_IMAGE found = {0};
    unsigned total, pos, lib, matches = 0;
    if (!out) return BC250_ATOM_ARGUMENT;
    memset(out,0,sizeof(*out));
    if (!b || !identity || identity->Bus > 255 || identity->Device > 31 || identity->Function > 7 ||
        !identity->VendorId || !identity->DeviceId) return BC250_ATOM_ARGUMENT;
    if (length < sizeof(struct uefi_acpi_vfct) || length > BC250_ATOM_MAX_VFCT) return BC250_ATOM_BOUNDS;
    if (memcmp(b,"VFCT",4)) return BC250_ATOM_SIGNATURE;
    total = Read32(b,offsetof(struct uefi_acpi_vfct,sheader.tableLength));
    if (total < sizeof(struct uefi_acpi_vfct) || total != length) return BC250_ATOM_BOUNDS;
    if (!Checksum(b,total)) return BC250_ATOM_CHECKSUM;
    pos = Read32(b,offsetof(struct uefi_acpi_vfct,vbiosimageoffset));
    lib = Read32(b,offsetof(struct uefi_acpi_vfct,lib1Imageoffset));
    if (!pos) return BC250_ATOM_NOT_FOUND;
    if (pos < sizeof(struct uefi_acpi_vfct) || pos >= total) return BC250_ATOM_BOUNDS;
    /* Do not interpret an optional LIB1 payload as a VBIOS. Refuse an overlapping/unsupported section order. */
    if (lib) {
        if (lib <= pos || lib >= total) return BC250_ATOM_BOUNDS;
        total = lib;
    }
    while (pos < total) {
        const uint8_t *h;
        unsigned n;
        BC250_ATOM_STATUS status;
        if (!Fits(total,pos,sizeof(struct vfct_image_header))) return BC250_ATOM_BOUNDS;
        h = b+pos;
        n = Read32(h,offsetof(struct vfct_image_header,imagelength));
        pos += sizeof(struct vfct_image_header);
        if (!Fits(total,pos,n)) return BC250_ATOM_BOUNDS;
        if (Read32(h,offsetof(struct vfct_image_header,pcibus)) == identity->Bus &&
            Read32(h,offsetof(struct vfct_image_header,pcidevice)) == identity->Device &&
            Read32(h,offsetof(struct vfct_image_header,pcifunction)) == identity->Function &&
            Read16(h,offsetof(struct vfct_image_header,vendorid)) == identity->VendorId &&
            Read16(h,offsetof(struct vfct_image_header,deviceid)) == identity->DeviceId) {
            if (++matches > 1) return BC250_ATOM_AMBIGUOUS;
            status = Bc250AtomImageOpen(b+pos,n,identity->VendorId,identity->DeviceId,&found);
            if (status != BC250_ATOM_OK) return status;
        }
        pos += n;
    }
    if (!matches) return BC250_ATOM_NOT_FOUND;
    *out = found;
    return BC250_ATOM_OK;
}
