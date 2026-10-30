/* SPDX-License-Identifier: GPL-3.0-only */

#ifndef CFW_UP_NEXT_H
#define CFW_UP_NEXT_H

#include "osos.h"

void cfw_up_next_show(void *controller);
int cfw_up_next_action(const char *action, uint32_t argument);
int cfw_up_next_has_songs(void);
void cfw_up_next_clear(void);
int cfw_up_next_property(uint32_t type, uint32_t property, uintptr_t *out);
int cfw_up_next_indexed_property(uint32_t type, uint32_t property, uint32_t index,
                                 uintptr_t *out);

#endif
