//--------------------------------------------------------------
//File name:   mmce_card.c
//--------------------------------------------------------------
#include "mmce_card.h"
#include "init.h"

#ifdef MMCE

#define MMCE_CMD_GET_STATUS 0x02
#define MMCE_CMD_SET_CARD_CHANNEL 0x0A

#define MMCE_CARD_TYPE_REGULAR 0x00
#define MMCE_CARD_TYPE_BOOT 0x01
#define MMCE_STATUS_BUSY 0x0001
#define MMCE_STATUS_ERRNO(_status) (((_status) >> 8) & 0xFF)
#define MMCE_STATUS_FLAGS(_status) ((_status) & 0xFF)

#define MMCE_SWITCH_POLL_COUNT 15
#define MMCE_SWITCH_POLL_DELAY_US 500000

typedef struct
{
	int unit;
	int mc_slot;
	u8 type;
	u16 card;
	u16 channel;
	char card_name[MAX_NAME];
	char fullpath[MAX_PATH];
} MmceCardSelection;

static int componentEquals(const char *component, size_t component_len, const char *literal)
{
	size_t i;

	if (component_len != strlen(literal))
		return FALSE;

	for (i = 0; i < component_len; i++) {
		if (wle_ascii_tolower((unsigned char)component[i]) != wle_ascii_tolower((unsigned char)literal[i]))
			return FALSE;
	}

	return TRUE;
}

static int matchCardFolder(const char *relative_path, const char *prefix, const char **folder, size_t *folder_len)
{
	const char *candidate;
	const char *slash;
	size_t prefix_len;

	prefix_len = strlen(prefix);
	if (strncmp(relative_path, prefix, prefix_len))
		return FALSE;

	candidate = relative_path + prefix_len;
	slash = strchr(candidate, '/');
	if (slash == NULL || slash == candidate || slash[1] != '\0')
		return FALSE;

	*folder = candidate;
	*folder_len = (size_t)(slash - candidate);
	return TRUE;
}

static int parseDecimalU16(const char *text, size_t text_len, u16 min_value, u16 max_value, u16 *value_out)
{
	u32 value;
	size_t i;

	if (text_len == 0)
		return FALSE;

	value = 0;
	for (i = 0; i < text_len; i++) {
		if (text[i] < '0' || text[i] > '9')
			return FALSE;

		value = value * 10 + (u32)(text[i] - '0');
		if (value > max_value)
			return FALSE;
	}

	if (value < min_value)
		return FALSE;

	*value_out = (u16)value;
	return TRUE;
}

static u16 hashCardName(const char *text, size_t text_len)
{
	u32 hash;
	size_t i;

	hash = 2166136261u;
	for (i = 0; i < text_len; i++) {
		hash ^= (unsigned char)text[i];
		hash *= 16777619u;
	}

	return (u16)((hash >> 16) ^ (hash & 0xFFFF));
}

static int parseCardId(const char *text, size_t text_len, u16 *card)
{
	if (parseDecimalU16(text, text_len, 0, 0xFFFF, card))
		return TRUE;

	/* The MMCE command carries a 16-bit card value, so named files need a stable local mapping. */
	*card = hashCardName(text, text_len);
	return TRUE;
}

static int parseMmceCardSelection(const char *path, const FILEINFO *file, MmceCardSelection *selection)
{
	const char *relative_path;
	const char *folder;
	const char *dot;
	const char *dash;
	size_t folder_len;
	size_t card_name_len;
	size_t channel_len;

	if (path == NULL || file == NULL || selection == NULL)
		return FALSE;
	if (file->stats.AttrFile & sceMcFileAttrSubdir)
		return FALSE;
	if (strncmp(path, "mmce", 4) || path[4] < '0' || path[4] > '1' || path[5] != ':' || path[6] != '/')
		return FALSE;

	selection->unit = path[4] - '0';
	selection->mc_slot = selection->unit;
	relative_path = path + 7;

	if (matchCardFolder(relative_path, "MemoryCards/PS2/", &folder, &folder_len) ||
	    matchCardFolder(relative_path, "MemoryCard/PS2/", &folder, &folder_len)) {
		if (!genCmpFileExt(file->name, "MCD"))
			return FALSE;
	} else if (matchCardFolder(relative_path, "MemoryCards/", &folder, &folder_len) ||
	           matchCardFolder(relative_path, "MemoryCard/", &folder, &folder_len)) {
		if (!genCmpFileExt(file->name, "MCD") || componentEquals(folder, folder_len, "PS2"))
			return FALSE;
	} else if (matchCardFolder(relative_path, "PS2/", &folder, &folder_len)) {
		if (!genCmpFileExt(file->name, "MC2"))
			return FALSE;
	} else
		return FALSE;

	dot = strrchr(file->name, '.');
	if (dot == NULL || dot <= file->name)
		return FALSE;

	dash = dot;
	while (dash > file->name && *dash != '-')
		dash--;
	if (dash <= file->name)
		return FALSE;

	channel_len = (size_t)(dot - dash - 1);
	if (!parseDecimalU16(dash + 1, channel_len, 1, 1024, &selection->channel))
		return FALSE;

	card_name_len = (size_t)(dash - file->name);
	if (card_name_len == 0 || card_name_len >= sizeof(selection->card_name))
		return FALSE;

	memcpy(selection->card_name, file->name, card_name_len);
	selection->card_name[card_name_len] = '\0';
	if (!parseCardId(selection->card_name, card_name_len, &selection->card))
		return FALSE;

	selection->type = componentEquals(folder, folder_len, "BOOT") ? MMCE_CARD_TYPE_BOOT : MMCE_CARD_TYPE_REGULAR;
	snprintf(selection->fullpath, sizeof(selection->fullpath), "%s%s", path, file->name);

	return TRUE;
}

static int mmceGetStatus(int unit)
{
	char device[8];

	snprintf(device, sizeof(device), "mmce%d:", unit);
	return fileXioDevctl(device, MMCE_CMD_GET_STATUS, NULL, 0, NULL, 0);
}

static int mmceWaitReady(int unit)
{
	int i;
	int status;

	for (i = 0; i < MMCE_SWITCH_POLL_COUNT; i++) {
		status = mmceGetStatus(unit);
		if (status < 0)
			return status;
		if (MMCE_STATUS_ERRNO(status))
			return -EIO;
		if ((MMCE_STATUS_FLAGS(status) & MMCE_STATUS_BUSY) == 0)
			return 0;
		DelayThread(MMCE_SWITCH_POLL_DELAY_US);
	}

	return -EBUSY;
}

static int mmceSetCardChannel(const MmceCardSelection *selection)
{
	u8 args[5];
	char device[8];

	args[0] = selection->type;
	args[1] = (u8)(selection->card >> 8);
	args[2] = (u8)(selection->card & 0xFF);
	args[3] = (u8)(selection->channel >> 8);
	args[4] = (u8)(selection->channel & 0xFF);

	snprintf(device, sizeof(device), "mmce%d:", selection->unit);
	return fileXioDevctl(device, MMCE_CMD_SET_CARD_CHANNEL, args, sizeof(args), NULL, 0);
}

static void formatResultMessage(char *message, size_t message_size, const MmceCardSelection *selection, int result, const char *detail)
{
	if (message == NULL || message_size == 0)
		return;

	if (detail == NULL)
		detail = "";

	snprintf(message, message_size,
	         "\nMount mc%d: for \"%s\"\nType=%s Card=0x%04X Channel=%u\n%sResult=%d",
	         selection->mc_slot, selection->fullpath,
	         selection->type == MMCE_CARD_TYPE_BOOT ? "boot" : "regular",
	         (unsigned int)selection->card, (unsigned int)selection->channel, detail, result);
}

int mmceCardGetMountSlot(const char *path, const FILEINFO *file)
{
	MmceCardSelection selection;

	if (!parseMmceCardSelection(path, file, &selection))
		return -1;

	return selection.mc_slot;
}

int mmceCardSwitchToFile(const char *path, const FILEINFO *file, int *mc_slot, char *message, size_t message_size)
{
	MmceCardSelection selection;
	int result;

	if (!parseMmceCardSelection(path, file, &selection))
		return -EINVAL;

	if (mc_slot != NULL)
		*mc_slot = selection.mc_slot;

	if (!loadMmceModules()) {
		formatResultMessage(message, message_size, &selection, -ENODEV, "mmceman not registered\n");
		return -ENODEV;
	}

	result = mmceGetStatus(selection.unit);
	if (result < 0) {
		formatResultMessage(message, message_size, &selection, result, "Get status failed\n");
		return result;
	}
	if (MMCE_STATUS_ERRNO(result)) {
		formatResultMessage(message, message_size, &selection, result, "MMCE status error\n");
		return -EIO;
	}
	if (MMCE_STATUS_FLAGS(result) & MMCE_STATUS_BUSY) {
		formatResultMessage(message, message_size, &selection, result, "MMCE is busy\n");
		return -EBUSY;
	}

	result = mmceSetCardChannel(&selection);
	if (result < 0) {
		formatResultMessage(message, message_size, &selection, result, "Set card/channel failed\n");
		return result;
	}

	result = mmceWaitReady(selection.unit);
	if (result < 0) {
		formatResultMessage(message, message_size, &selection, result, "Timed out waiting for MMCE\n");
		return result;
	}

	formatResultMessage(message, message_size, &selection, 0, "");
	return 0;
}

#endif
