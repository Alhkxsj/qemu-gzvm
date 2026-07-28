#ifndef HW_FLASH_H
#define HW_FLASH_H

/* m25p80.c */

#define TYPE_M25P80 "m25p80-generic"

#include "hw/qdev-core.h"

BlockBackend *m25p80_get_blk(DeviceState *dev);

#endif
