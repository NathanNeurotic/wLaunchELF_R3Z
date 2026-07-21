#include "launchelf.h"
#include "hdd_header_injector.h"

#include <hdd-ioctl.h>

#define APA_HEADER_SECTORS 4096
#define APA_HEADER_SIZE (APA_HEADER_SECTORS * 512)
#define APA_HEADER_MAGIC "PS2ICON3D"
#define HDD_HEADER_SOURCE_WAIT_MS 3000
#define HDD_HEADER_SOURCE_POLL_MS 500

enum HddHeaderFileKind {
	HDD_HEADER_SYSTEM_CNF = 0,
	HDD_HEADER_ICON_SYS,
	HDD_HEADER_LIST_ICO,
	HDD_HEADER_BOOT_KELF,

	HDD_HEADER_FILE_COUNT
};

typedef struct
{
	const char *name;
	u32 data_offset;
	u32 offset_offset;
	u32 size_offset;
	u32 max_size;
	int required;
} HddHeaderFileSpec;

static const HddHeaderFileSpec hdd_header_files[HDD_HEADER_FILE_COUNT] = {
    {"system.cnf", 0x1200, 0x1010, 0x1014, 0x1400 - 0x1200, 1},
    {"icon.sys", 0x1400, 0x1018, 0x101C, 0x1800 - 0x1400, 1},
    {"list.ico", 0x1800, 0x1020, 0x1024, 0x111000 - 0x1800, 1},
    {"boot.kelf", 0x111000, 0x1030, 0x1034, APA_HEADER_SIZE - 0x111000, 0},
};

static unsigned char hdd_header_io_buffer[512 + sizeof(hddAtaTransfer_t)] __attribute__((aligned(64)));
static unsigned char hdd_header_buffer[APA_HEADER_SIZE] __attribute__((aligned(64)));

static void hddHeaderDelay(int ms)
{
	u64 end_time;

	end_time = Timer() + ms;
	while (Timer() < end_time) {
	}
}

static int hddHeaderJoinPath(char *out, size_t out_size, const char *dir, const char *file)
{
	const char *separator;
	size_t dir_len;
	int written;

	if (out == NULL || out_size == 0 || dir == NULL || file == NULL)
		return -EINVAL;

	dir_len = strlen(dir);
	separator = (dir_len > 0 && dir[dir_len - 1] == '/') ? "" : "/";
	written = snprintf(out, out_size, "%s%s%s", dir, separator, file);

	return (written >= 0 && written < (int)out_size) ? 0 : -ENAMETOOLONG;
}

static int hddHeaderGetFileSize(const char *path, u32 *size)
{
	char fixed_path[MAX_PATH];
	s64 file_size;
	int fd;
	int ret;

	ret = genFixPath(path, fixed_path);
	if (ret < 0)
		return ret;

	fd = genOpen(fixed_path, FIO_O_RDONLY);
	if (fd < 0)
		return fd;

	file_size = genLseek(fd, 0, SEEK_END);
	genClose(fd);
	if (file_size < 0)
		return (int)file_size;
	if (file_size > 0xFFFFFFFFLL)
		return -EFBIG;

	if (size != NULL)
		*size = (u32)file_size;
	return 0;
}

static int hddHeaderSourceHasFile(const char *dir, const HddHeaderFileSpec *spec)
{
	char path[MAX_PATH];
	u32 size;
	int ret;

	ret = hddHeaderJoinPath(path, sizeof(path), dir, spec->name);
	if (ret < 0)
		return ret;

	ret = hddHeaderGetFileSize(path, &size);
	if (ret < 0 && !strcmp(spec->name, "boot.kelf")) {
		ret = hddHeaderJoinPath(path, sizeof(path), dir, "BOOT.KELF");
		if (ret < 0)
			return ret;
		ret = hddHeaderGetFileSize(path, &size);
	}
	if (ret < 0)
		return ret;
	if (size > spec->max_size)
		return -EFBIG;

	return 0;
}

static int hddHeaderSourceReady(const char *dir)
{
	int i;
	int ret;

	for (i = 0; i < HDD_HEADER_FILE_COUNT; i++) {
		if (!hdd_header_files[i].required)
			continue;

		ret = hddHeaderSourceHasFile(dir, &hdd_header_files[i]);
		if (ret < 0)
			return ret;
	}

	return 0;
}

static int hddHeaderBuildCandidateDir(char *out, size_t out_size, const char *source_device, const char *partition_name)
{
	int written;

	if (out == NULL || out_size == 0 || source_device == NULL || partition_name == NULL)
		return -EINVAL;

	written = snprintf(out, out_size, "%s/__Headers/%s/", source_device, partition_name);

	return (written >= 0 && written < (int)out_size) ? 0 : -ENAMETOOLONG;
}

static int hddHeaderPrepareSourceDevice(const char *source_device)
{
	if (source_device == NULL)
		return -EINVAL;

	if (!strncmp(source_device, "usb", 3)) {
		prepareUsbRootBrowse();
		return 0;
	}

#ifdef MMCE
	if (!strncmp(source_device, "mmce", 4))
		return loadMmceModules() ? 0 : -ENODEV;
#else
	if (!strncmp(source_device, "mmce", 4))
		return -ENODEV;
#endif

#ifdef UDPFS
	if (!strncmp(source_device, "udpfs", 5))
		return load_udpfs() ? 0 : -ENODEV;
#else
	if (!strncmp(source_device, "udpfs", 5))
		return -ENODEV;
#endif

	return 0;
}

static int hddHeaderFindSourceDir(const char *source_device, const char *partition_name, char *source_dir, size_t source_dir_size)
{
	char candidate[MAX_PATH];
	int ret;

	ret = hddHeaderBuildCandidateDir(candidate, sizeof(candidate), source_device, partition_name);
	if (ret < 0)
		return ret;

	ret = hddHeaderSourceReady(candidate);
	if (ret >= 0) {
		if (source_dir != NULL && source_dir_size > 0) {
			snprintf(source_dir, source_dir_size, "%s", candidate);
			source_dir[source_dir_size - 1] = '\0';
		}
		return 0;
	}

	return ret;
}

static int hddHeaderWaitForSource(const char *source_device, const char *partition_name, char *source_dir, size_t source_dir_size)
{
	char msg[MAX_TEXT_LINE];
	u64 start_time;
	int ret = -ENOENT;

	start_time = Timer();
	while (Timer() < start_time + HDD_HEADER_SOURCE_WAIT_MS) {
		ret = hddHeaderPrepareSourceDevice(source_device);
		if (ret >= 0)
			ret = hddHeaderFindSourceDir(source_device, partition_name, source_dir, source_dir_size);
		if (ret >= 0)
			return 0;

		snprintf(msg, sizeof(msg), "Waiting for header files on %s %s", source_device, partition_name);
		drawMsg(msg);
		hddHeaderDelay(HDD_HEADER_SOURCE_POLL_MS);
	}

	return ret;
}

static int hddHeaderReadSectors(u32 partition_sector)
{
	hddAtaTransfer_t *transfer = (hddAtaTransfer_t *)hdd_header_io_buffer;
	int i;
	int ret = 0;

	for (i = 0; i < APA_HEADER_SECTORS; i++) {
		transfer->lba = partition_sector + i;
		transfer->size = 1;
		ret = fileXioDevctl("hdd0:", HDIOC_READSECTOR, hdd_header_io_buffer,
		                    sizeof(hddAtaTransfer_t), hdd_header_buffer + 512 * i, 512);
		if (ret < 0)
			return ret;
	}

	return ret;
}

static int hddHeaderWriteSectors(u32 partition_sector)
{
	hddAtaTransfer_t *transfer = (hddAtaTransfer_t *)hdd_header_io_buffer;
	int i;
	int ret = 0;

	for (i = 0; i < APA_HEADER_SECTORS; i++) {
		transfer->lba = partition_sector + i;
		transfer->size = 1;
		memcpy(transfer->data, hdd_header_buffer + 512 * i, 512);
		ret = fileXioDevctl("hdd0:", HDIOC_WRITESECTOR, hdd_header_io_buffer,
		                    512 + sizeof(hddAtaTransfer_t), NULL, 0);
		if (ret < 0)
			return ret;
	}

	return ret;
}

static int hddHeaderReadFileIntoHeader(const char *source_dir, const HddHeaderFileSpec *spec)
{
	char path[MAX_PATH];
	char fixed_path[MAX_PATH];
	u32 file_size;
	u32 data_offset;
	int fd;
	int ret;

	ret = hddHeaderJoinPath(path, sizeof(path), source_dir, spec->name);
	if (ret < 0)
		return ret;

	ret = hddHeaderGetFileSize(path, &file_size);
	if (ret < 0 && !strcmp(spec->name, "boot.kelf")) {
		ret = hddHeaderJoinPath(path, sizeof(path), source_dir, "BOOT.KELF");
		if (ret < 0)
			return ret;
		ret = hddHeaderGetFileSize(path, &file_size);
	}
	if (ret < 0)
		return spec->required ? ret : 0;
	if (file_size > spec->max_size)
		return -EFBIG;

	ret = genFixPath(path, fixed_path);
	if (ret < 0)
		return ret;

	fd = genOpen(fixed_path, FIO_O_RDONLY);
	if (fd < 0 && !strcmp(spec->name, "boot.kelf")) {
		hddHeaderJoinPath(path, sizeof(path), source_dir, "BOOT.KELF");
		ret = genFixPath(path, fixed_path);
		if (ret < 0)
			return ret;
		fd = genOpen(fixed_path, FIO_O_RDONLY);
	}
	if (fd < 0)
		return fd;

	if (genRead(fd, hdd_header_buffer + spec->data_offset, file_size) != (int)file_size) {
		genClose(fd);
		return -EIO;
	}
	genClose(fd);

	data_offset = spec->data_offset - 0x1000;
	memcpy(hdd_header_buffer + spec->offset_offset, &data_offset, sizeof(data_offset));
	memcpy(hdd_header_buffer + spec->size_offset, &file_size, sizeof(file_size));

	if (!strcmp(spec->name, "list.ico")) {
		memcpy(hdd_header_buffer + 0x1028, &data_offset, sizeof(data_offset));
		memcpy(hdd_header_buffer + 0x102C, &file_size, sizeof(file_size));
	}

	return 0;
}

static int hddHeaderPatchFromSource(const char *source_dir)
{
	int i;
	int ret;

	memcpy(hdd_header_buffer + 0x1000, APA_HEADER_MAGIC, strlen(APA_HEADER_MAGIC));
	for (i = 0; i < HDD_HEADER_FILE_COUNT; i++) {
		ret = hddHeaderReadFileIntoHeader(source_dir, &hdd_header_files[i]);
		if (ret < 0)
			return ret;
	}

	return 0;
}

int InjectHddPartitionHeaderFromSource(const char *partition_name, const char *source_device, char *source_dir, size_t source_dir_size)
{
	char partition_path[MAX_NAME + 6];
	char resolved_source_dir[MAX_PATH];
	iox_stat_t stat;
	u32 partition_sector;
	int ret;

	if (partition_name == NULL || partition_name[0] == '\0')
		return -EINVAL;
	if (source_device == NULL || source_device[0] == '\0')
		return -EINVAL;

	ret = hddHeaderWaitForSource(source_device, partition_name, resolved_source_dir, sizeof(resolved_source_dir));
	if (ret < 0)
		return ret;
	if (source_dir != NULL && source_dir_size > 0) {
		snprintf(source_dir, source_dir_size, "%s", resolved_source_dir);
		source_dir[source_dir_size - 1] = '\0';
	}

	ret = snprintf(partition_path, sizeof(partition_path), "hdd0:%s", partition_name);
	if (ret < 0 || ret >= (int)sizeof(partition_path))
		return -ENAMETOOLONG;

	ret = fileXioDevctl("hdd0:", HDIOC_STATUS, NULL, 0, NULL, 0);
	if (ret != 0)
		return -EIO;

	ret = fileXioGetStat(partition_path, &stat);
	if (ret < 0)
		return ret;
	partition_sector = stat.private_5;

	ret = hddHeaderReadSectors(partition_sector);
	if (ret < 0)
		return ret;

	ret = hddHeaderPatchFromSource(resolved_source_dir);
	if (ret < 0)
		return ret;

	return hddHeaderWriteSectors(partition_sector);
}
