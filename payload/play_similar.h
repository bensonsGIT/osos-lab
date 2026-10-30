/* SPDX-License-Identifier: GPL-3.0-only */

#ifndef CFW_PLAY_SIMILAR_H
#define CFW_PLAY_SIMILAR_H

#include "osos.h"

/* Queues songs related to seed after the current one. Takes the player lock itself;
   the library scan runs before it is taken. */
void cfw_play_similar(struct osos_media_item *seed);

#endif
