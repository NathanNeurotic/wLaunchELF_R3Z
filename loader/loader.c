//--------------------------------------------------------------
// File name: loader.c
//--------------------------------------------------------------
#include "tamtypes.h"
#include "debug.h"
#include "kernel.h"
#include "iopcontrol.h"
#include "sifrpc.h"
#include "loadfile.h"
#include "string.h"
#include "iopheap.h"
#include "errno.h"
#include <elf.h>

#define USER_MEM_START_ADDR 0x100000

#define MAX_LOADER_ARGS 16
#define MAX_LOADER_ARG_LEN 256

static char saved_target[MAX_LOADER_ARG_LEN];
static char saved_path[MAX_LOADER_ARG_LEN];
static char saved_extra_args[MAX_LOADER_ARGS][MAX_LOADER_ARG_LEN];
static char *exec_argv[MAX_LOADER_ARGS];
static int exec_argc;

void _libcglue_init(void)
{
}

void _libcglue_deinit(void)
{
}

void _libcglue_args_parse(int argc, char **argv)
{
	(void)argc;
	(void)argv;
}

static void wipeUserMem(void)
{
	int i;
	for (i = 0x100000; i < GetMemorySize(); i += 64) {
		asm volatile(
		    "\tsq $0, 0(%0) \n"
		    "\tsq $0, 16(%0) \n"
		    "\tsq $0, 32(%0) \n"
		    "\tsq $0, 48(%0) \n" ::"r"(i));
	}
}

static void wipeUserMemPreserving(u32 preserve_addr, u32 preserve_size)
{
	u32 end, preserve_end;

	end = GetMemorySize();
	if (preserve_addr < USER_MEM_START_ADDR || preserve_addr >= end || preserve_size == 0) {
		wipeUserMem();
		return;
	}

	preserve_end = preserve_addr + preserve_size;
	if (preserve_end < preserve_addr || preserve_end > end)
		preserve_end = end;

	if (preserve_addr > USER_MEM_START_ADDR)
		memset((void *)USER_MEM_START_ADDR, 0, preserve_addr - USER_MEM_START_ADDR);
	if (preserve_end < end)
		memset((void *)preserve_end, 0, end - preserve_end);
}

static int parseHex8(const char *text, u32 *value)
{
	u32 out;
	int i;

	if (text == NULL || value == NULL)
		return -EINVAL;

	out = 0;
	for (i = 0; i < 8; i++) {
		char c = text[i];
		u32 digit;

		if (c >= '0' && c <= '9')
			digit = c - '0';
		else if (c >= 'A' && c <= 'F')
			digit = c - 'A' + 10;
		else if (c >= 'a' && c <= 'f')
			digit = c - 'a' + 10;
		else
			return -EINVAL;

		out = (out << 4) | digit;
	}

	*value = out;
	return 0;
}

static int parseMemPath(const char *path, u32 *addr, u32 *size)
{
	if (path == NULL || strncmp(path, "mem:", 4) != 0)
		return -EINVAL;
	if (parseHex8(path + 4, addr) < 0)
		return -EINVAL;
	if (path[12] != ':')
		return -EINVAL;
	if (parseHex8(path + 13, size) < 0)
		return -EINVAL;
	return 0;
}

static int loadELFFromMemory(u32 elf_addr, u32 *entry)
{
	Elf32_Ehdr *eh;
	Elf32_Phdr *ph;
	void *src;
	int i;

	if (entry == NULL)
		return -EINVAL;

	eh = (Elf32_Ehdr *)elf_addr;
	if (memcmp(eh->e_ident, ELFMAG, SELFMAG) != 0)
		return -ENOEXEC;
	if ((eh->e_type != ET_EXEC) && (eh->e_type != ET_DYN))
		return -ENOEXEC;
	if (eh->e_machine != EM_MIPS)
		return -ENOEXEC;
	if (eh->e_entry == 0 || (eh->e_entry & 0x3) != 0)
		return -ENOEXEC;

	ph = (Elf32_Phdr *)(elf_addr + eh->e_phoff);
	for (i = 0; i < eh->e_phnum; i++) {
		if (ph[i].p_type != PT_LOAD)
			continue;

		src = (void *)(elf_addr + ph[i].p_offset);
		memcpy((void *)ph[i].p_vaddr, src, ph[i].p_filesz);
		if (ph[i].p_memsz > ph[i].p_filesz)
			memset((void *)(ph[i].p_vaddr + ph[i].p_filesz), 0, ph[i].p_memsz - ph[i].p_filesz);
	}

	*entry = eh->e_entry;
	FlushCache(0);
	FlushCache(2);
	return 0;
}

int main(int argc, char *argv[])
{
	static t_ExecData elfdata;
	int ret, rebootiop = 0;
	int i;

	// Initialize SIF RPC
	SifInitRpc(0);

	if (argc < 1) {
		SifExitRpc();
		return -EINVAL;
	}

	// Copy incoming arguments safely into resident loader BSS before wiping user memory
	saved_target[0] = '\0';
	saved_path[0] = '\0';

	if (argv[0] != NULL) {
		strncpy(saved_target, argv[0], sizeof(saved_target) - 1);
		saved_target[sizeof(saved_target) - 1] = '\0';
	}
	if (argc > 1 && argv[1] != NULL) {
		strncpy(saved_path, argv[1], sizeof(saved_path) - 1);
		saved_path[sizeof(saved_path) - 1] = '\0';
	} else {
		strncpy(saved_path, saved_target, sizeof(saved_path) - 1);
		saved_path[sizeof(saved_path) - 1] = '\0';
	}

	// Check for reboot IOP flags: either "-r" (ISR standard) or "-la=...R..." (R3Z)
	for (i = 2; i < argc; i++) {
		if (argv[i] != NULL) {
			if (!strcmp("-r", argv[i])) {
				rebootiop = 1;
			} else if (!strncmp(argv[i], "-la=", 4)) {
				if (strchr(argv[i] + 4, 'R') != NULL)
					rebootiop = 1;
			}
		}
	}

	// Prepare outbound argv for the child ELF
	exec_argc = 0;
	exec_argv[exec_argc++] = saved_path;

	// Copy any additional arguments (ignoring internal loader control flags)
	for (i = 2; i < argc && exec_argc < MAX_LOADER_ARGS; i++) {
		if (argv[i] != NULL && strcmp(argv[i], "-r") != 0 && strcmp(argv[i], "-nr") != 0 && strncmp(argv[i], "-la=", 4) != 0) {
			strncpy(saved_extra_args[exec_argc], argv[i], MAX_LOADER_ARG_LEN - 1);
			saved_extra_args[exec_argc][MAX_LOADER_ARG_LEN - 1] = '\0';
			exec_argv[exec_argc] = saved_extra_args[exec_argc];
			exec_argc++;
		}
	}

	// Branch 1: Embedded Memory Payload (MBR / memory boot)
	if (!strncmp(saved_target, "mem:", 4)) {
		u32 elf_addr = 0, elf_size = 0, entry = 0;
		if (parseMemPath(saved_target, &elf_addr, &elf_size) < 0 || elf_addr < USER_MEM_START_ADDR || elf_size == 0) {
			SifExitRpc();
			return -EINVAL;
		}

		wipeUserMemPreserving(elf_addr, elf_size);

		if (rebootiop) {
			while (!SifIopReset("", 0));
			while (!SifIopSync());
		}

		ret = loadELFFromMemory(elf_addr, &entry);
		if (ret < 0) {
			entry = USER_MEM_START_ADDR;
			memcpy((void *)entry, (void *)elf_addr, elf_size);
		}

		SifExitRpc();
		FlushCache(0);
		FlushCache(2);
		ExecPS2((void *)entry, NULL, exec_argc, exec_argv);
		return 0;
	}

	// Branch 2: Standard File Payload (from PFS / MC / USB / host / CDVD)
	wipeUserMem();
	FlushCache(0);

	ret = SifLoadElf(saved_target, &elfdata);
	if (ret != 0 || elfdata.epc == 0)
		ret = SifLoadElfEncrypted(saved_target, &elfdata);

	if (ret == 0 && elfdata.epc != 0 && (elfdata.epc & 0x3) == 0) {
		if (rebootiop) {
			while (!SifIopReset("", 0));
			while (!SifIopSync());
		}

		SifExitRpc();
		FlushCache(0);
		FlushCache(2);

		ExecPS2((void *)elfdata.epc, (void *)elfdata.gp, exec_argc, exec_argv);
		return 0;
	} else {
		SifExitRpc();
		return -ENOENT;
	}
}
