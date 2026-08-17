//--------------------------------------------------------------
// File name: loader.c - Unified Resident ELF Loader
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

#define USER_MEM_START_ADDR 0x00100000

#define MAX_LOADER_ARGS 16
#define MAX_LOADER_ARG_LEN 256

#define PT_LOAD 1
#define SHT_PROGBITS 1
#define SHT_SYMTAB 2
#define SHT_STRTAB 3
#define SHT_NOBITS 8
#define ELFMAG "\177ELF"
#define SELFMAG 4

typedef struct {
	u8 e_ident[16];
	u16 e_type;
	u16 e_machine;
	u32 e_version;
	u32 e_entry;
	u32 e_phoff;
	u32 e_shoff;
	u32 e_flags;
	u16 e_ehsize;
	u16 e_phentsize;
	u16 e_phnum;
	u16 e_shentsize;
	u16 e_shnum;
	u16 e_shstrndx;
} Elf32_Ehdr;

typedef struct {
	u32 p_type;
	u32 p_offset;
	u32 p_vaddr;
	u32 p_paddr;
	u32 p_filesz;
	u32 p_memsz;
	u32 p_flags;
	u32 p_align;
} Elf32_Phdr;

typedef struct {
	u32 sh_name;
	u32 sh_type;
	u32 sh_flags;
	u32 sh_addr;
	u32 sh_offset;
	u32 sh_size;
	u32 sh_link;
	u32 sh_info;
	u32 sh_addralign;
	u32 sh_entsize;
} Elf32_Shdr;

static char saved_target[MAX_LOADER_ARG_LEN];
static char saved_path[MAX_LOADER_ARG_LEN];
static char saved_extra_args[MAX_LOADER_ARGS][MAX_LOADER_ARG_LEN];
static char *exec_argv[MAX_LOADER_ARGS];
static int exec_argc;

static void wipeUserMem(void)
{
	int i;
	int memsize = GetMemorySize();
	for (i = USER_MEM_START_ADDR; i < memsize; i += 64) {
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

static inline u32 phdr_bss_len(Elf32_Phdr ph)
{
	return ph.p_memsz - ph.p_filesz;
}

static int loadStagedElf(u8 *elf_buf, u32 elf_size, u32 *entry_out, void **gp_out)
{
	Elf32_Ehdr *eh;
	Elf32_Phdr *ph;
	int i;

	if (elf_buf == NULL || elf_size < sizeof(Elf32_Ehdr))
		return -EINVAL;

	eh = (Elf32_Ehdr *)elf_buf;
	if (memcmp(eh->e_ident, ELFMAG, SELFMAG) != 0)
		return -EINVAL;

	if (eh->e_entry == 0 || (eh->e_entry & 0x3) != 0)
		return -EINVAL;

	if (eh->e_phoff == 0 || eh->e_phnum == 0)
		return -EINVAL;

	ph = (Elf32_Phdr *)(elf_buf + eh->e_phoff);

	// Relocate each PT_LOAD segment to its target virtual address
	for (i = 0; i < eh->e_phnum; i++) {
		if (ph[i].p_type != PT_LOAD)
			continue;

		if (ph[i].p_filesz > 0) {
			if (ph[i].p_offset + ph[i].p_filesz > elf_size)
				return -EINVAL;
			memcpy((void *)ph[i].p_vaddr, (void *)(elf_buf + ph[i].p_offset), ph[i].p_filesz);
		}

		// Zero out uninitialized BSS section
		if (ph[i].p_memsz > ph[i].p_filesz) {
			memset((void *)(ph[i].p_vaddr + ph[i].p_filesz), 0, phdr_bss_len(ph[i]));
		}
	}

	*entry_out = eh->e_entry;
	*gp_out = NULL;

	// Clear the staging window in RAM so user memory is completely pristine
	memset((void *)elf_buf, 0, elf_size);

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

	// Copy incoming arguments safely into resident loader BSS
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

	// Check for reboot IOP flags: "-r" or "-la=...R..."
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

	// Prepare outbound argv for the child ELF:
	// args[0] = bootpath (e.g. "hdd0:__system:pfs:path/to/elf", "mc0:...", "mass0:...")
	exec_argc = 0;
	exec_argv[exec_argc++] = saved_path;

	// Copy any additional sidecar arguments
	for (i = 2; i < argc && exec_argc < MAX_LOADER_ARGS; i++) {
		if (argv[i] != NULL && strcmp(argv[i], "-r") != 0 && strcmp(argv[i], "-nr") != 0 && strncmp(argv[i], "-la=", 4) != 0) {
			strncpy(saved_extra_args[exec_argc], argv[i], MAX_LOADER_ARG_LEN - 1);
			saved_extra_args[exec_argc][MAX_LOADER_ARG_LEN - 1] = '\0';
			exec_argv[exec_argc] = saved_extra_args[exec_argc];
			exec_argc++;
		}
	}

	// Route 1: Memory Staged Payload (mem:ADDR:SIZE) - Works for HDD PFS, USB exFAT, VMC, DVR, XFROM
	if (!strncmp(saved_target, "mem:", 4)) {
		u32 elf_addr = 0, elf_size = 0;
		u32 entry_point = 0;
		void *gp_ptr = NULL;

		if (parseMemPath(saved_target, &elf_addr, &elf_size) < 0 || elf_addr < USER_MEM_START_ADDR || elf_size == 0) {
			SifExitRpc();
			return -EINVAL;
		}

		// Wipe user memory around the staged ELF
		wipeUserMemPreserving(elf_addr, elf_size);

		if (rebootiop) {
			while (!SifIopReset("", 0));
			while (!SifIopSync());
		}

		ret = loadStagedElf((u8 *)elf_addr, elf_size, &entry_point, &gp_ptr);
		if (ret < 0 || entry_point == 0) {
			SifExitRpc();
			return -EINVAL;
		}

		SifExitRpc();
		FlushCache(0);
		FlushCache(2);
		ExecPS2((void *)entry_point, gp_ptr, exec_argc, exec_argv);
		return 0;
	}

	// Route 2: Direct SifLoadElf Device Execution (mc0:, cdrom0:, rom0:)
	wipeUserMem();
	FlushCache(0);

	memset(&elfdata, 0, sizeof(elfdata));
	SifLoadFileInit();
	ret = SifLoadElf(saved_target, &elfdata);
	if (ret != 0 || elfdata.epc == 0)
		ret = SifLoadElfEncrypted(saved_target, &elfdata);
	SifLoadFileExit();

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
