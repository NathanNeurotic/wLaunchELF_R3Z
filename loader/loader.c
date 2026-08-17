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

#define MAX_LOADER_ARGS 16
#define MAX_LOADER_ARG_LEN 256

static char saved_target[MAX_LOADER_ARG_LEN];
static char saved_path[MAX_LOADER_ARG_LEN];
static char saved_extra_args[MAX_LOADER_ARGS][MAX_LOADER_ARG_LEN];
static char *exec_argv[MAX_LOADER_ARGS];
static int exec_argc;

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

int main(int argc, char *argv[])
{
	static t_ExecData elfdata;
	int ret, rebootiop = 0;
	int i;

	// Initialize SIF RPC
	SifInitRpc(0);
	wipeUserMem();

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

	if (argc > 2 && argv[2] != NULL) {
		rebootiop = (!strcmp("-r", argv[2]));
	}

	// Prepare outbound argv for child ELF:
	// exec_argv[0] = saved_path (e.g. "hdd0:__system:pfs:path/to/BOOT.ELF" or "mc0:...")
	exec_argc = 0;
	exec_argv[exec_argc++] = saved_path;

	// Copy any additional sidecar arguments
	for (i = 3; i < argc && exec_argc < MAX_LOADER_ARGS; i++) {
		if (argv[i] != NULL) {
			strncpy(saved_extra_args[exec_argc], argv[i], MAX_LOADER_ARG_LEN - 1);
			saved_extra_args[exec_argc][MAX_LOADER_ARG_LEN - 1] = '\0';
			exec_argv[exec_argc] = saved_extra_args[exec_argc];
			exec_argc++;
		}
	}

	// Writeback data cache before loading ELF
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
