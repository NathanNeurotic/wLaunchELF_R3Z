//--------------------------------------------------------------
// File name: loader.c
//--------------------------------------------------------------
#include "tamtypes.h"
#include "debug.h"
#include "kernel.h"
#include "iopcontrol.h"
#include "sifrpc.h"
#include "loadfile.h"
#include "fileXio_rpc.h"
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include "iopheap.h"
#include "errno.h"

#ifndef SEEK_SET
#define SEEK_SET 0
#endif
#ifndef SEEK_END
#define SEEK_END 2
#endif

#ifndef FIO_O_RDONLY
#define FIO_O_RDONLY 0x0001
#endif
#ifndef FIO_FILE_MODE
#define FIO_FILE_MODE 0777
#endif

#define MAX_LOADER_ARGS 16
#define MAX_LOADER_ARG_LEN 256

#define ELF_MAGIC 0x464C457F

typedef struct {
	u8 ident[16];
	u16 type;
	u16 machine;
	u32 version;
	u32 entry;
	u32 phoff;
	u32 shoff;
	u32 flags;
	u16 ehsize;
	u16 phentsize;
	u16 phnum;
	u16 shentsize;
	u16 shnum;
	u16 shstrndx;
} elf_header_t;

typedef struct {
	u32 type;
	u32 offset;
	void *vaddr;
	u32 paddr;
	u32 filesz;
	u32 memsz;
	u32 flags;
	u32 align;
} elf_pheader_t;

#define ELF_PT_LOAD 1

static char saved_target[MAX_LOADER_ARG_LEN];
static char saved_hostpath[MAX_LOADER_ARG_LEN];
static char saved_bootpath[MAX_LOADER_ARG_LEN];
static char saved_extra_args[MAX_LOADER_ARGS][MAX_LOADER_ARG_LEN];
static char *exec_argv[MAX_LOADER_ARGS];
static int exec_argc;

static t_ExecData elfdata;

static void wipeUserMem(void)
{
	int i;
	int memsize = GetMemorySize();
	for (i = 0x100000; i < memsize; i += 64) {
		asm volatile(
		    "\tsq $0, 0(%0) \n"
		    "\tsq $0, 16(%0) \n"
		    "\tsq $0, 32(%0) \n"
		    "\tsq $0, 48(%0) \n" ::"r"(i));
	}
}

static int tLoadElf(const char *filename)
{
	u8 *boot_elf = (u8 *)0x01800000;
	elf_header_t *eh = (elf_header_t *)boot_elf;
	elf_pheader_t *eph;
	int fd, size, i;

	fileXioInit();

	fd = fileXioOpen(filename, FIO_O_RDONLY, FIO_FILE_MODE);
	if (fd < 0)
		return -1;

	size = fileXioLseek(fd, 0, SEEK_END);
	if (size <= 0) {
		fileXioClose(fd);
		return -1;
	}

	fileXioLseek(fd, 0, SEEK_SET);
	if (fileXioRead(fd, boot_elf, sizeof(elf_header_t)) != sizeof(elf_header_t)) {
		fileXioClose(fd);
		return -1;
	}

	if (*(u32 *)&eh->ident != ELF_MAGIC) {
		fileXioClose(fd);
		return -1;
	}

	fileXioLseek(fd, eh->phoff, SEEK_SET);
	eph = (elf_pheader_t *)(boot_elf + sizeof(elf_header_t));
	size = eh->phnum * eh->phentsize;
	if (fileXioRead(fd, eph, size) != size) {
		fileXioClose(fd);
		return -1;
	}

	for (i = 0; i < eh->phnum; i++) {
		if (eph[i].type != ELF_PT_LOAD)
			continue;

		fileXioLseek(fd, eph[i].offset, SEEK_SET);
		size = eph[i].filesz;
		if (size > 0)
			fileXioRead(fd, eph[i].vaddr, size);

		if (eph[i].memsz > eph[i].filesz)
			memset((u8 *)eph[i].vaddr + eph[i].filesz, 0, eph[i].memsz - eph[i].filesz);
	}

	fileXioClose(fd);

	elfdata.epc = eh->entry;
	elfdata.gp = 0;
	return 0;
}

static int loadTargetElf(const char *path)
{
	int ret = -1;

	if (!strncmp(path, "pfs0", 4) || !strncmp(path, "dvr_pfs0", 8) ||
	    !strncmp(path, "vmc", 3) || !strncmp(path, "mass", 4)) {
		ret = tLoadElf(path);
	}

	if (ret != 0) {
		SifLoadFileInit();
		ret = SifLoadElf(path, &elfdata);
		if (ret != 0 || elfdata.epc == 0)
			ret = SifLoadElfEncrypted(path, &elfdata);
		SifLoadFileExit();
	}

	return ret;
}

int main(int argc, char *argv[])
{
	int ret, rebootiop = 0;
	int i;

	// Copy incoming arguments safely into resident loader BSS FIRST before touching user memory!
	saved_target[0] = '\0';
	saved_hostpath[0] = '\0';
	saved_bootpath[0] = '\0';

	if (argc > 0 && argv[0] != NULL) {
		strncpy(saved_target, argv[0], sizeof(saved_target) - 1);
		saved_target[sizeof(saved_target) - 1] = '\0';
	}
	if (argc > 1 && argv[1] != NULL) {
		strncpy(saved_hostpath, argv[1], sizeof(saved_hostpath) - 1);
		saved_hostpath[sizeof(saved_hostpath) - 1] = '\0';
	} else {
		strncpy(saved_hostpath, saved_target, sizeof(saved_hostpath) - 1);
		saved_hostpath[sizeof(saved_hostpath) - 1] = '\0';
	}
	if (argc > 2 && argv[2] != NULL) {
		strncpy(saved_bootpath, argv[2], sizeof(saved_bootpath) - 1);
		saved_bootpath[sizeof(saved_bootpath) - 1] = '\0';
	} else {
		strncpy(saved_bootpath, saved_hostpath, sizeof(saved_bootpath) - 1);
		saved_bootpath[sizeof(saved_bootpath) - 1] = '\0';
	}

	if (argc > 3 && argv[3] != NULL) {
		rebootiop = (!strcmp("-r", argv[3]));
	}

	// Prepare outbound argv for child ELF:
	exec_argc = 0;
	exec_argv[exec_argc++] = saved_hostpath;
	if (strcmp(saved_hostpath, saved_bootpath) != 0) {
		exec_argv[exec_argc++] = saved_bootpath;
	}

	// Copy any additional sidecar arguments
	for (i = 4; i < argc && exec_argc < MAX_LOADER_ARGS; i++) {
		if (argv[i] != NULL) {
			strncpy(saved_extra_args[exec_argc], argv[i], MAX_LOADER_ARG_LEN - 1);
			saved_extra_args[exec_argc][MAX_LOADER_ARG_LEN - 1] = '\0';
			exec_argv[exec_argc] = saved_extra_args[exec_argc];
			exec_argc++;
		}
	}

	// Initialize SIF RPC and wipe user RAM now that arguments are preserved
	SifInitRpc(0);
	wipeUserMem();

	// Writeback data cache before loading ELF
	FlushCache(0);

	memset(&elfdata, 0, sizeof(elfdata));
	ret = loadTargetElf(saved_target);

	if (ret == 0 && elfdata.epc != 0 && (elfdata.epc & 0x3) == 0) {
		if (rebootiop) {
			while (!SifIopReset("", 0));
			while (!SifIopSync());
		}

		SifExitRpc();
		__asm__ __volatile__(
			".set  noreorder\n\t"
			"jal     FlushCache\n\t"
			"li      $a0, 0\n\t"
			"jal     FlushCache\n\t"
			"li      $a0, 2\n\t"
			"lui     $sp, 0x000a\n\t"
			"nop\n\t"
			"addiu   $sp, $sp, 0x8000\n\t"
			"nop\n\t"
			".set  reorder\n\t"
		);

		ExecPS2((void *)elfdata.epc, (void *)elfdata.gp, exec_argc, exec_argv);
		return 0;
	} else {
		SifExitRpc();
		return -ENOENT;
	}
}
