/* SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0 */
/*
 * The flash job of the BC-250 stage (docs/design/p6-flash.md). The load option flash=<job> names the job. Without
 * the option the module does nothing.
 *
 * Job read: the module reads the firmware image of the board through the flash protocol of the firmware. It writes
 * the image and a result file beside the stage image. It writes nothing to the flash.
 *
 * The module is the first in the order of the modules. A job must not depend on the work of another module, and the
 * read comes before the reset of the core unlock.
 *
 * Co nagle, to po diable - what is done in a hurry is done badly: the image is read and kept before anything is
 * written.
 */
#include <stdarg.h>
#include <stddef.h>
#include <string.h>
#include "efi.h"
#include "efl_text.h"
#include "stage.h"
#include "flash.h"

#define FLASH_NAME		"flash"
#define FLASH_CHUNK_BYTES	(64u * 1024u)
#define FLASH_PAGE_BYTES	4096u
#define FLASH_DUMP_NAME		"bc250-flash.dump"
#define FLASH_RESULT_NAME	"bc250-flash.result"
#define FLASH_LINE_BYTES	160u
#define FLASH_RESULT_BYTES	1024u
#define FLASH_TEXT_BYTES	(FLASH_DIGEST_BYTES * 2u + 1u)

/* The flash protocol of the firmware: the FlashDriver of the stock image. Its functions take the address of the
 * flash in the ROM window of the processor (docs/design/p6-flash.md). The write functions have no type here: no job
 * of this module calls them. */
static const EFI_GUID flash_protocol_guid = { 0x755B6596, 0x6896, 0x4BA3, { 0xB3, 0xDD, 0x1C, 0x62, 0x9F, 0xD1, 0xEA, 0x88 } };
typedef EFI_STATUS (EFIAPI *FLASH_READ)(UINTN address, UINTN bytes, VOID *data);
struct firmware_flash {
	FLASH_READ Read;
	VOID *Erase, *Write, *EraseWrite, *Open, *Close;
};

/* The text of the result file of the last job. */
static char result[FLASH_RESULT_BYTES];
static size_t result_used;

/* Adds text to the result file. Text that does not fit is cut. The text always ends with a null character. */
static void result_text(const char *text)
{
	size_t n = strlen(text), i;
	if (n > sizeof(result) - result_used - 1u) n = sizeof(result) - result_used - 1u;
	for (i = 0; i < n; i++) result[result_used++] = text[i];
	result[result_used] = 0;
}

/* One line of the result file: the text, then one line feed, as in the log file of the stage. */
static void result_line(const char *format, ...)
{
	char line[FLASH_LINE_BYTES];
	va_list args;
	va_start(args, format);
	(void)efl_vformat(line, sizeof(line), format, args);
	va_end(args);
	result_text(line);
	result_text("\n");
}

/* Writes a new file beside the stage image. A file of that name is removed first, so that the new file has these
 * bytes and no byte of an earlier file stays behind them. 0 on failure. */
static int write_sibling(struct stage *stage, const char *name, const unsigned char *data, size_t bytes)
{
	unsigned short path[STAGE_PATH_CHARS];
	EFI_FILE_PROTOCOL *file = NULL;
	EFI_STATUS status;
	const UINT64 mode = EFI_FILE_MODE_READ | EFI_FILE_MODE_WRITE | EFI_FILE_MODE_CREATE;
	size_t at = 0;
	int ok = 0;

	if (stage->root == NULL || !stage_sibling(stage, name, path, STAGE_PATH_CHARS)) return 0;
	status = stage->root->Open(stage->root, &file, path, mode, 0);
	if (EFI_ERROR(status)) return 0;
	/* Delete closes the handle. */
	(void)file->Delete(file);
	file = NULL;
	status = stage->root->Open(stage->root, &file, path, mode, 0);
	if (EFI_ERROR(status) || file == NULL) return 0;
	while (at < bytes) {
		UINTN count = bytes - at > FLASH_CHUNK_BYTES ? FLASH_CHUNK_BYTES : bytes - at;
		status = file->Write(file, &count, (VOID *)(data + at));
		/* A volume that is full takes fewer bytes and can still answer with success. */
		if (EFI_ERROR(status) || count == 0 || count > bytes - at) break;
		at += count;
	}
	if (at == bytes && !EFI_ERROR(file->Flush(file))) ok = 1;
	file->Close(file);
	return ok;
}

static void result_write(struct stage *stage)
{
	if (!write_sibling(stage, FLASH_RESULT_NAME, (const unsigned char *)result, result_used))
		stage_say(stage, FLASH_NAME, "the result file did not write beside this application");
}

/* Removes a file beside the stage image, when it is there. A file that is not there is no failure. 0 when the file
 * stays. */
static int remove_sibling(struct stage *stage, const char *name)
{
	unsigned short path[STAGE_PATH_CHARS];
	EFI_FILE_PROTOCOL *file = NULL;
	EFI_STATUS status;

	if (stage->root == NULL) return 0;
	if (!stage_sibling(stage, name, path, STAGE_PATH_CHARS)) return 1;
	status = stage->root->Open(stage->root, &file, path, EFI_FILE_MODE_READ, 0);
	if (status == EFI_NOT_FOUND) return 1;
	if (EFI_ERROR(status) || file == NULL) return 0;
	/* Delete closes the handle. */
	status = file->Delete(file);
	return !EFI_ERROR(status);
}

/* One volume on the console and in the result file. */
static void say_volume(struct stage *stage, const char *name, const struct flash_volume *volume)
{
	if (!volume->present) {
		stage_sayf(stage, FLASH_NAME, "volume %s: no firmware volume header with a sound length", name);
		result_line("volume %s absent", name);
		return;
	}
	stage_sayf(stage, FLASH_NAME, "volume %s: %llu bytes, used to 0x%llx", name, volume->bytes, (unsigned long long)volume->used_end);
	result_line("volume %s bytes %llu used 0x%llx", name, volume->bytes, (unsigned long long)volume->used_end);
	if (!volume->stream.present) {
		result_line("stream %s absent", name);
		return;
	}
	stage_sayf(stage, FLASH_NAME, "volume %s: stream at 0x%llx, %llu compressed bytes, %llu plain bytes", name,
		   (unsigned long long)volume->stream.first, (unsigned long long)volume->stream.compressed, volume->stream.plain);
	result_line("stream %s first 0x%llx compressed %llu plain %llu", name, (unsigned long long)volume->stream.first,
		    (unsigned long long)volume->stream.compressed, volume->stream.plain);
}

/* The flash protocol of the firmware, or NULL when the firmware does not have it. */
static struct firmware_flash *find_flash(struct stage *stage)
{
	struct firmware_flash *flash = NULL;
	if (EFI_ERROR(stage->bs->LocateProtocol((EFI_GUID *)&flash_protocol_guid, NULL, (VOID **)&flash)) || flash == NULL ||
	    flash->Read == NULL)
		return NULL;
	return flash;
}

/* Job read: the whole image through the flash protocol, then the two files. */
static void job_read(struct stage *stage, struct firmware_flash *flash)
{
	EFI_PHYSICAL_ADDRESS memory = 0;
	struct flash_image info;
	unsigned char *image;
	char sha[FLASH_TEXT_BYTES], base[FLASH_TEXT_BYTES], store[FLASH_TEXT_BYTES];
	EFI_STATUS status;
	unsigned long done = 0;		/* the bytes that are read */
	int kept;

	/* An image of an earlier job must not stay beside a result that says that this job did not read the image. */
	if (!remove_sibling(stage, FLASH_DUMP_NAME))
		stage_say(stage, FLASH_NAME, "the image file of an earlier job did not remove");
	status = ((EFI_ALLOCATE_PAGES)stage->bs->AllocatePages)(AllocateAnyPages, EfiLoaderData, FLASH_IMAGE_BYTES / FLASH_PAGE_BYTES,
								 &memory);
	if (EFI_ERROR(status)) {
		stage_sayf(stage, FLASH_NAME, "no memory for the image, status 0x%llx", (unsigned long long)status);
		result_line("error no memory");
		result_line("status failed");
		result_write(stage);
		return;
	}
	image = (unsigned char *)(UINTN)memory;
	while (done < FLASH_IMAGE_BYTES) {
		UINTN count = FLASH_CHUNK_BYTES;
		status = flash->Read((UINTN)(FLASH_ROM_BASE + done), count, image + done);
		if (EFI_ERROR(status)) {
			stage_sayf(stage, FLASH_NAME, "the read failed at 0x%llx, status 0x%llx", (unsigned long long)done, (unsigned long long)status);
			break;
		}
		done += FLASH_CHUNK_BYTES;
	}
	flash_image_describe(image, done, &info);
	flash_digest_text(info.sha256, sha);
	flash_digest_text(info.base, base);
	flash_digest_text(info.store, store);
	stage_sayf(stage, FLASH_NAME, "read %llu of %llu bytes", (unsigned long long)done, (unsigned long long)FLASH_IMAGE_BYTES);
	stage_sayf(stage, FLASH_NAME, "sha256 %s", sha);
	stage_sayf(stage, FLASH_NAME, "base %s", base);
	stage_sayf(stage, FLASH_NAME, "store %s", store);
	say_volume(stage, "store", &info.store_volume);
	say_volume(stage, "outer", &info.outer_volume);
	say_volume(stage, "pei", &info.pei_volume);
	result_line("sha256 %s", sha);
	result_line("base %s", base);
	result_line("store %s", store);
	result_line("image-bytes %llu", (unsigned long long)done);
	if (done == FLASH_IMAGE_BYTES) {
		kept = write_sibling(stage, FLASH_DUMP_NAME, image, FLASH_IMAGE_BYTES);
		if (kept) stage_say(stage, FLASH_NAME, "the image is in the file " FLASH_DUMP_NAME " beside this application");
		else stage_say(stage, FLASH_NAME, "the image file did not write: the image is not kept");
		result_line("dump %s", kept ? "yes" : "no");
		result_line("status ok");
	} else {
		result_line("read-failed-at 0x%llx", (unsigned long long)done);
		result_line("dump no");
		result_line("status failed");
	}
	result_write(stage);
	((EFI_FREE_PAGES)stage->bs->FreePages)(memory, FLASH_IMAGE_BYTES / FLASH_PAGE_BYTES);
}

static void flash_run(struct stage *stage)
{
	const char *job = stage_option_text(&stage->options, FLASH_NAME);
	struct firmware_flash *flash;

	if (job == NULL) return;
	/* The result file: the format, the job, what the job found, and the status line at the end. */
	result_used = 0;
	result[0] = 0;
	result_line("format 1");
	result_line("job %s", job);
	if (!stage_board_bc250(stage)) {
		stage_say(stage, FLASH_NAME, "not a BC-250: no flash job");
		result_line("error not a BC-250");
		result_line("status failed");
		result_write(stage);
		return;
	}
	flash = find_flash(stage);
	if (flash == NULL) {
		stage_say(stage, FLASH_NAME, "the firmware has no flash protocol: no flash job");
		result_line("error no flash protocol");
		result_line("status failed");
		result_write(stage);
		return;
	}
	stage_sayf(stage, FLASH_NAME, "job %s, flash protocol at 0x%llx", job, (unsigned long long)(UINTN)flash);
	if (!strcmp(job, "read")) job_read(stage, flash);
	else {
		stage_sayf(stage, FLASH_NAME, "unknown job '%s': nothing done", job);
		result_line("error unknown job");
		result_line("status failed");
		result_write(stage);
	}
}

static void flash_probe(struct stage *stage)
{
	stage_sayf(stage, FLASH_NAME, "probe: flash protocol %s", find_flash(stage) ? "present" : "absent");
}

const struct stage_module flash_module = {
	.name       = FLASH_NAME,
	.off_switch = "flash",
	.default_on = 1,
	.run        = flash_run,
	.probe      = flash_probe,
	.gate       = NULL,
	.loaded     = NULL,
	.unwind     = NULL,
};
