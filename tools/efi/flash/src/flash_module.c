/* SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0 */
/*
 * The flash job of the BC-250 stage (docs/design/p6-flash.md). The load option flash=<job> names the job. Without
 * the option the module does nothing.
 *
 * Job read: the module reads the firmware image of the board through the flash protocol of the firmware. It writes
 * the image and a result file beside the stage image. It writes nothing to the flash. With flash.reset=1 the module
 * restarts the board after the job.
 *
 * Job test (flash=test flash.sector=<image offset>): the write test of one free sector of the outer volume, for the
 * measurement of the write functions of the protocol (flash_writetest.h). The sector is erased at the end. The
 * module reads the image before the test and after it: the result file says which bytes changed, and the image
 * after the test goes into the image file. The module writes no other sector, and no sector at all when the one
 * that the option names is not free.
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
#include <cpuid.h>
#include "efi.h"
#include "efl_text.h"
#include "stage.h"
#include "flash.h"
#include "flash_writetest.h"

#define FLASH_NAME		"flash"
#define FLASH_CHUNK_BYTES	(64u * 1024u)
#define FLASH_PAGE_BYTES	4096u
#define FLASH_DUMP_NAME		"bc250-flash.dump"
#define FLASH_RESULT_NAME	"bc250-flash.result"
#define FLASH_LINE_BYTES	160u
#define FLASH_RESULT_BYTES	2048u
#define FLASH_RESET_PAUSE_US	(2u * 1000u * 1000u)
#define FLASH_TEXT_BYTES	(FLASH_DIGEST_BYTES * 2u + 1u)
#define FLASH_MS_UNKNOWN	(~0ull)

/* The flash protocol of the firmware: the FlashDriver of the stock image. Its functions take the address of the
 * flash in the ROM window of the processor (docs/design/p6-flash.md). */
static const EFI_GUID flash_protocol_guid = { 0x755B6596, 0x6896, 0x4BA3, { 0xB3, 0xDD, 0x1C, 0x62, 0x9F, 0xD1, 0xEA, 0x88 } };
typedef EFI_STATUS (EFIAPI *FLASH_READ)(UINTN address, UINTN bytes, VOID *data);
typedef EFI_STATUS (EFIAPI *FLASH_ERASE)(UINTN address, UINTN bytes);
typedef EFI_STATUS (EFIAPI *FLASH_WRITE)(UINTN address, UINTN bytes, VOID *data);
typedef EFI_STATUS (EFIAPI *FLASH_SESSION)(VOID);
struct firmware_flash {
	FLASH_READ Read;
	FLASH_ERASE Erase;
	FLASH_WRITE Write;		/* programs without an erase */
	FLASH_WRITE EraseWrite;
	FLASH_SESSION Open, Close;
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

/* ---- reading the image ---------------------------------------------------------------------------- */

/* Memory for the image of the board, or NULL. The caller frees it with the count of FLASH_IMAGE_BYTES /
 * FLASH_PAGE_BYTES pages. */
static unsigned char *alloc_image(struct stage *stage)
{
	EFI_PHYSICAL_ADDRESS memory = 0;
	EFI_STATUS status = ((EFI_ALLOCATE_PAGES)stage->bs->AllocatePages)(AllocateAnyPages, EfiLoaderData,
									    FLASH_IMAGE_BYTES / FLASH_PAGE_BYTES, &memory);
	if (EFI_ERROR(status)) {
		stage_sayf(stage, FLASH_NAME, "no memory for the image, status 0x%llx", (unsigned long long)status);
		return NULL;
	}
	return (unsigned char *)(UINTN)memory;
}

static void free_image(struct stage *stage, unsigned char *image)
{
	((EFI_FREE_PAGES)stage->bs->FreePages)((EFI_PHYSICAL_ADDRESS)(UINTN)image, FLASH_IMAGE_BYTES / FLASH_PAGE_BYTES);
}

/* Reads the whole image in pieces of FLASH_CHUNK_BYTES. The count of the bytes that were read: less than the image
 * when a read failed, which the console line then names. */
static unsigned long read_image(struct stage *stage, struct firmware_flash *flash, unsigned char *image)
{
	unsigned long done = 0;
	while (done < FLASH_IMAGE_BYTES) {
		EFI_STATUS status = flash->Read((UINTN)(FLASH_ROM_BASE + done), FLASH_CHUNK_BYTES, image + done);
		if (EFI_ERROR(status)) {
			stage_sayf(stage, FLASH_NAME, "the read failed at 0x%llx, status 0x%llx", (unsigned long long)done,
				   (unsigned long long)status);
			break;
		}
		done += FLASH_CHUNK_BYTES;
	}
	return done;
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

/* The three digests of an image, on the console and in the result file. suffix is empty or names a second image. */
static void say_identity(struct stage *stage, const struct flash_image *info, const char *suffix)
{
	char sha[FLASH_TEXT_BYTES], base[FLASH_TEXT_BYTES], store[FLASH_TEXT_BYTES];
	flash_digest_text(info->sha256, sha);
	flash_digest_text(info->base, base);
	flash_digest_text(info->store, store);
	stage_sayf(stage, FLASH_NAME, "sha256%s %s", suffix, sha);
	stage_sayf(stage, FLASH_NAME, "base%s %s", suffix, base);
	stage_sayf(stage, FLASH_NAME, "store%s %s", suffix, store);
	result_line("sha256%s %s", suffix, sha);
	result_line("base%s %s", suffix, base);
	result_line("store%s %s", suffix, store);
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

/* 1 when the protocol has each function that a write needs. */
static int can_write(const struct firmware_flash *flash)
{
	return flash->Erase != NULL && flash->Write != NULL && flash->EraseWrite != NULL && flash->Open != NULL &&
	       flash->Close != NULL;
}

/* Job read: the whole image through the flash protocol, then the two files. */
static void job_read(struct stage *stage, struct firmware_flash *flash)
{
	struct flash_image info;
	unsigned char *image;
	unsigned long done;
	int kept;

	/* An image of an earlier job must not stay beside a result that says that this job did not read the image. */
	if (!remove_sibling(stage, FLASH_DUMP_NAME))
		stage_say(stage, FLASH_NAME, "the image file of an earlier job did not remove");
	image = alloc_image(stage);
	if (image == NULL) {
		result_line("error no memory");
		result_line("status failed");
		result_write(stage);
		return;
	}
	done = read_image(stage, flash, image);
	flash_image_describe(image, done, &info);
	stage_sayf(stage, FLASH_NAME, "read %llu of %llu bytes", (unsigned long long)done, (unsigned long long)FLASH_IMAGE_BYTES);
	say_identity(stage, &info, "");
	say_volume(stage, "store", &info.store_volume);
	say_volume(stage, "outer", &info.outer_volume);
	say_volume(stage, "pei", &info.pei_volume);
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
	free_image(stage, image);
}

/* ---- the write test ------------------------------------------------------------------------------- */

/* The time of each call of the write test, in milliseconds. The time stamp counter of the processor is the clock:
 * the module has no other one, and the order of the calls is the order of the steps. */
static unsigned long long test_ms[FWT_STEPS];
static unsigned test_calls;
static unsigned long long ticks_per_ms;

static unsigned long long rdtsc(void)
{
	unsigned lo, hi;
	__asm__ volatile("rdtsc" : "=a"(lo), "=d"(hi));
	return ((unsigned long long)hi << 32) | lo;
}

/* The time stamp counter counts at a constant rate (CPUID 80000007H EDX bit 8): its ticks in a stall of 10 ms give
 * the ticks of a millisecond. 0 when the processor has no such counter: the times are then not known. */
static int calibrate(struct stage *stage)
{
	unsigned a, b, c, d;
	unsigned long long start;
	if (!__get_cpuid(0x80000007u, &a, &b, &c, &d) || !(d & (1u << 8))) return 0;
	start = rdtsc();
	stage->bs->Stall(10000);
	ticks_per_ms = (rdtsc() - start) / 10u;
	return ticks_per_ms != 0;
}

/* The start of a timed call. */
static unsigned long long tick(void)
{
	return ticks_per_ms ? rdtsc() : 0;
}

/* The end of a timed call: keeps its time and gives its status back. */
static unsigned long long tock(unsigned long long start, EFI_STATUS status)
{
	test_ms[test_calls < FWT_STEPS ? test_calls : FWT_STEPS - 1u] = ticks_per_ms ? (rdtsc() - start) / ticks_per_ms :
									      FLASH_MS_UNKNOWN;
	test_calls++;
	return (unsigned long long)status;
}

/* The functions of the protocol behind the table of the write test. The arguments are evaluated one after the other,
 * so that the clock starts before the call. */
static unsigned long long op_open(void *context)
{
	unsigned long long start = tick();
	return tock(start, ((struct firmware_flash *)context)->Open());
}

static unsigned long long op_close(void *context)
{
	unsigned long long start = tick();
	return tock(start, ((struct firmware_flash *)context)->Close());
}

static unsigned long long op_read(void *context, unsigned long sector, unsigned char *block)
{
	unsigned long long start = tick();
	return tock(start, ((struct firmware_flash *)context)->Read((UINTN)(FLASH_ROM_BASE + sector), FLASH_SECTOR_BYTES, block));
}

static unsigned long long op_erase(void *context, unsigned long sector)
{
	unsigned long long start = tick();
	return tock(start, ((struct firmware_flash *)context)->Erase((UINTN)(FLASH_ROM_BASE + sector), FLASH_SECTOR_BYTES));
}

static unsigned long long op_write(void *context, unsigned long sector, const unsigned char *block)
{
	unsigned long long start = tick();
	return tock(start, ((struct firmware_flash *)context)->Write((UINTN)(FLASH_ROM_BASE + sector), FLASH_SECTOR_BYTES,
								      (VOID *)block));
}

static unsigned long long op_erase_write(void *context, unsigned long sector, const unsigned char *block)
{
	unsigned long long start = tick();
	return tock(start, ((struct firmware_flash *)context)->EraseWrite((UINTN)(FLASH_ROM_BASE + sector), FLASH_SECTOR_BYTES,
									   (VOID *)block));
}

/* A time in milliseconds as text. "?" when the clock is not there. out takes 24 characters. */
static const char *ms_text(unsigned long long ms, char *out)
{
	if (ms == FLASH_MS_UNKNOWN) return "?";
	(void)efl_format(out, 24, "%llu", ms);
	return out;
}

/* The steps of the write test on the console and in the result file. */
static void say_test(struct stage *stage, const struct flash_write_test_report *report)
{
	char text[24];
	const char *ms;
	unsigned i, calls = 0;
	for (i = 0; i < FWT_STEPS; i++) {
		const char *name = flash_write_test_step_text((enum flash_write_test_step)i);
		if (!report->ran[i]) continue;
		ms = ms_text(test_ms[calls++], text);
		stage_sayf(stage, FLASH_NAME, "test %u %s: status 0x%llx, %s ms, %s", i + 1u, name, report->status[i], ms,
			   report->as_expected[i] ? "as expected" : "NOT as expected");
		result_line("test %u %s status 0x%llx ms %s %s", i + 1u, name, report->status[i], ms,
			    report->as_expected[i] ? "as-expected" : "not-as-expected");
	}
}

/* Job test: the write test on one free sector, with the image before it and after it. */
static void job_test(struct stage *stage, struct firmware_flash *flash)
{
	const char *text = stage_option_text(&stage->options, "flash.sector");
	struct flash_image info, after;
	struct flash_write_test_report report;
	struct flash_write_test_ops ops;
	unsigned char *before = NULL, *image_after = NULL;
	unsigned long sector = 0, read, read_after, in_sector = 0, in_store = 0, elsewhere = 0, i;
	enum flash_sector_place place;
	int ok;

	if (text == NULL || !flash_parse_hex(text, &sector)) {
		stage_say(stage, FLASH_NAME, "the job test needs the image offset of one sector in flash.sector=<hex>: nothing done");
		result_line("error no sector");
		result_line("status failed");
		result_write(stage);
		return;
	}
	result_line("sector 0x%llx", (unsigned long long)sector);
	if (!can_write(flash)) {
		stage_say(stage, FLASH_NAME, "the flash protocol of the firmware has not all the functions of a write: nothing done");
		result_line("error no write functions");
		result_line("status failed");
		result_write(stage);
		return;
	}
	/* An image of an earlier job must not stay beside a result that says that this job did not read the image. */
	if (!remove_sibling(stage, FLASH_DUMP_NAME))
		stage_say(stage, FLASH_NAME, "the image file of an earlier job did not remove");
	before = alloc_image(stage);
	if (before == NULL) {
		result_line("error no memory");
		result_line("status failed");
		result_write(stage);
		return;
	}
	read = read_image(stage, flash, before);
	flash_image_describe(before, read, &info);
	say_identity(stage, &info, "");
	say_volume(stage, "store", &info.store_volume);
	say_volume(stage, "outer", &info.outer_volume);
	say_volume(stage, "pei", &info.pei_volume);
	place = read == FLASH_IMAGE_BYTES ? flash_sector_place(&info, sector) : FLASH_SECTOR_NO_VOLUME;
	stage_sayf(stage, FLASH_NAME, "sector 0x%llx: %s", (unsigned long long)sector, flash_sector_place_text(place));
	result_line("sector-place %s", flash_sector_place_text(place));
	if (place != FLASH_SECTOR_FREE) {
		/* No byte of the flash is written. */
		stage_say(stage, FLASH_NAME, "the sector is not free: no write");
		result_line("error the sector is not free");
		result_line("status failed");
		result_write(stage);
		free_image(stage, before);
		return;
	}
	if (calibrate(stage)) stage_sayf(stage, FLASH_NAME, "time stamp counter: %llu ticks per millisecond", ticks_per_ms);
	else stage_say(stage, FLASH_NAME, "no constant time stamp counter: the times are not known");

	ops.context = flash;
	ops.open = op_open;
	ops.close = op_close;
	ops.read = op_read;
	ops.erase = op_erase;
	ops.write = op_write;
	ops.erase_write = op_erase_write;
	test_calls = 0;
	ok = flash_run_write_test(&ops, sector, &report);
	say_test(stage, &report);
	if (!ok) result_line("error test step %u not as expected", (unsigned)report.failed + 1u);

	/* The image after the test: what changed, and where. */
	image_after = alloc_image(stage);
	if (image_after == NULL) {
		result_line("error no memory for the image after the test");
		ok = 0;
	} else {
		read_after = read_image(stage, flash, image_after);
		flash_image_describe(image_after, read_after, &after);
		say_identity(stage, &after, "-after");
		if (read_after == FLASH_IMAGE_BYTES) {
			for (i = 0; i < FLASH_IMAGE_BYTES; i++) {
				if (before[i] == image_after[i]) continue;
				if (i >= sector && i < sector + FLASH_SECTOR_BYTES) in_sector++;
				else if (i < FLASH_STORE_BYTES) in_store++;
				else elsewhere++;
			}
			stage_sayf(stage, FLASH_NAME, "bytes that changed: %llu in the sector, %llu in the variable store, %llu elsewhere",
				   (unsigned long long)in_sector, (unsigned long long)in_store, (unsigned long long)elsewhere);
			ok = ok && elsewhere == 0 && memcmp(info.base, after.base, FLASH_DIGEST_BYTES) == 0;
			result_line("diff-in-sector %llu", (unsigned long long)in_sector);
			result_line("diff-in-store %llu", (unsigned long long)in_store);
			result_line("diff-elsewhere %llu", (unsigned long long)elsewhere);
			if (write_sibling(stage, FLASH_DUMP_NAME, image_after, FLASH_IMAGE_BYTES)) {
				stage_say(stage, FLASH_NAME, "the image after the test is in the file " FLASH_DUMP_NAME " beside this application");
				result_line("dump yes");
			} else {
				stage_say(stage, FLASH_NAME, "the image file did not write: the image is not kept");
				result_line("dump no");
			}
		} else {
			result_line("error the read after the test failed");
			result_line("dump no");
			ok = 0;
		}
		free_image(stage, image_after);
	}
	result_line("status %s", ok ? "ok" : "failed");
	result_write(stage);
	free_image(stage, before);
}

/* Restarts the board when the load option flash.reset=1 is set. The option is for a start that must not go on to
 * Windows after the job: a measurement, and each job that writes. The log is on the volume before the restart. */
static void restart_if_asked(struct stage *stage)
{
	if (!stage_option_flag(&stage->options, "flash.reset", 0)) return;
	stage_say(stage, FLASH_NAME, "restarting the board");
	stage_log_flush(stage);
	/* The pause gives the file system of the volume time to write its last bytes. */
	stage->bs->Stall(FLASH_RESET_PAUSE_US);
	stage->st->RuntimeServices->ResetSystem(EfiResetCold, EFI_SUCCESS, 0, NULL);
	for (;;) {}
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
	if (!strcmp(job, "read"))
		job_read(stage, flash);
	else if (!strcmp(job, "test"))
		job_test(stage, flash);
	else {
		stage_sayf(stage, FLASH_NAME, "unknown job '%s': nothing done", job);
		result_line("error unknown job");
		result_line("status failed");
		result_write(stage);
		return;
	}
	restart_if_asked(stage);
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
