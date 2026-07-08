#ifndef MMCE_CARD_H
#define MMCE_CARD_H

#include "launchelf.h"

int mmceCardGetMountSlot(const char *path, const FILEINFO *file);
int mmceCardSwitchToFile(const char *path, const FILEINFO *file, int *mc_slot, char *message, size_t message_size);

#endif
