/* SPDX-License-Identifier: GPL-3.0-only */

#include "up_next.h"

#include "up_next_ui.h"

/* Rows are rendered on demand, so one line is formatted at a time. */
static struct {
    char data[256];
    unsigned size;
} line;

static void append(const char *text)
{
    while (*text) {
        unsigned bytes = 1;
        unsigned char first = (unsigned char)*text;
        if (first >= 0xc0 && first < 0xe0)
            bytes = 2;
        else if (first >= 0xe0 && first < 0xf0)
            bytes = 3;
        else if (first >= 0xf0)
            bytes = 4;
        if (line.size + bytes >= sizeof(line.data))
            break;
        for (unsigned i = 0; i < bytes && *text; i++)
            line.data[line.size++] = *text++;
    }
    line.data[line.size] = 0;
}

static void decimal(uint32_t value)
{
    char digits[11];
    unsigned start = sizeof(digits) - 1;
    digits[start] = 0;
    do {
        digits[--start] = '0' + value % 10;
        value /= 10;
    } while (value);
    append(digits + start);
}

static void text_field(struct osos_media_item *item, enum osos_media_text field)
{
    struct osos_native_string string;
    osos_native_string_init(&string);
    osos_media_item_text(item, field, &string);
    const char *text = osos_native_string_data(&string);
    append(text && *text ? text : "Unknown");
    osos_native_string_destroy(&string);
}

/* Caller holds the player lock. Songs after the current one, in play order. */
static unsigned upcoming(struct osos_player *player)
{
    if (!player->current || !osos_playback_list_valid(&player->playback))
        return 0;
    struct osos_media_list *list = player->playback.list;
    if (osos_list_root(list)->groups)
        return 0;
    unsigned count = osos_list_root(list)->tracks;
    unsigned next = (unsigned)player->current_index + 1;
    return next < count ? count - next : 0;
}

static unsigned row_count(void)
{
    struct osos_player *player = osos_player_get();
    osos_player_lock(player);
    unsigned count = upcoming(player);
    osos_player_unlock(player);
    /* An empty queue still shows one explanatory row. */
    return count ? count : 1;
}

static const char *row_text(uint32_t index)
{
    struct osos_player *player = osos_player_get();
    line.size = 0;
    line.data[0] = 0;
    osos_player_lock(player);
    if (index < upcoming(player)) {
        /* Playback indexes are in play order, so shuffle and Play Next are reflected. */
        struct osos_media_entry *entry =
            osos_playback_entry(&player->playback, player->current_index + 1 + index);
        struct osos_media_item item;
        if (entry && entry->song.record) {
            osos_media_item_init_record(&item, entry->song.record);
            if (osos_media_item_valid(&item)) {
                decimal(index + 1);
                append(". ");
                text_field(&item, OSOS_MEDIA_TITLE);
                append(" - ");
                text_field(&item, OSOS_MEDIA_ARTIST);
            }
        }
    } else if (index == 0) {
        append("Nothing queued");
    }
    osos_player_unlock(player);
    return line.data;
}

int cfw_up_next_property(uint32_t type, uint32_t property, uintptr_t *out)
{
    if (property != CFW_UpNext_Property)
        return 0;
    if (type == OSOS_PROPERTY_COUNT)
        *out = row_count();
    else if (type == OSOS_PROPERTY_STRING)
        *out = (uintptr_t)row_text(0);
    else
        return 0;
    return 1;
}

int cfw_up_next_indexed_property(uint32_t type, uint32_t property, uint32_t index,
                                 uintptr_t *out)
{
    if (property != CFW_UpNext_Property)
        return 0;
    if (type == OSOS_PROPERTY_STRING)
        *out = (uintptr_t)row_text(index);
    else if (type == OSOS_RESOURCE_BMAP)
        *out = 0; /* No checkmark. */
    else
        return 0;
    return 1;
}

void cfw_up_next_show(void *controller)
{
    void *screen =
        osos_create_screen_controller(osos_controller_registry(), CFW_UpNext_Screen,
                                      CFW_UpNext_Layout, 2); /* foregroundpush */
    osos_controller_push(controller, screen);
}
