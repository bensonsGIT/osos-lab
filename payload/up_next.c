/* SPDX-License-Identifier: GPL-3.0-only */

#include "up_next.h"

#include "up_next_ui.h"

/* Generated row ids are allocated sequentially but are not guaranteed contiguous. */
static const uint32_t rows[] = {
    CFW_UP_NEXT_1,  CFW_UP_NEXT_2,  CFW_UP_NEXT_3,  CFW_UP_NEXT_4,  CFW_UP_NEXT_5,
    CFW_UP_NEXT_6,  CFW_UP_NEXT_7,  CFW_UP_NEXT_8,  CFW_UP_NEXT_9,  CFW_UP_NEXT_10,
    CFW_UP_NEXT_11, CFW_UP_NEXT_12, CFW_UP_NEXT_13, CFW_UP_NEXT_14, CFW_UP_NEXT_15,
    CFW_UP_NEXT_16, CFW_UP_NEXT_17, CFW_UP_NEXT_18, CFW_UP_NEXT_19, CFW_UP_NEXT_20,
};
#define ROWS (sizeof(rows) / sizeof(*rows))

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

static void set_row(unsigned row)
{
    osos_resource_set(osos_resource_bank(), 0x53747220, rows[row], line.data,
                      line.size + 1);
    line.size = 0;
    line.data[0] = 0;
}

/* Caller holds the player lock. Returns the number of rows filled with songs. */
static unsigned fill_rows(struct osos_player *player)
{
    if (!player->current || !osos_playback_list_valid(&player->playback))
        return 0;
    struct osos_media_list *list = player->playback.list;
    if (osos_list_root(list)->groups)
        return 0;
    unsigned count = osos_list_root(list)->tracks;
    unsigned filled = 0;
    /* Playback indexes are in play order, so shuffle and Play Next are reflected. */
    for (unsigned index = (unsigned)player->current_index + 1;
         index < count && filled < ROWS; index++) {
        struct osos_media_entry *entry = osos_playback_entry(&player->playback, index);
        if (!entry || !entry->song.record)
            continue;
        struct osos_media_item item;
        osos_media_item_init_record(&item, entry->song.record);
        if (!osos_media_item_valid(&item))
            continue;
        decimal(filled + 1);
        append(". ");
        text_field(&item, OSOS_MEDIA_TITLE);
        append(" - ");
        text_field(&item, OSOS_MEDIA_ARTIST);
        set_row(filled++);
    }
    return filled;
}

void cfw_up_next_show(void *controller)
{
    struct osos_player *player = osos_player_get();
    line.size = 0;
    line.data[0] = 0;
    osos_player_lock(player);
    unsigned filled = fill_rows(player);
    osos_player_unlock(player);

    if (!filled) {
        append("Nothing queued");
        set_row(filled++);
    }
    /* Rows have a fixed count; blank the ones past the end of the queue. */
    while (filled < ROWS)
        set_row(filled++);

    void *screen =
        osos_create_screen_controller(osos_controller_registry(), CFW_UpNext_Screen,
                                      CFW_UpNext_Layout, 2); /* foregroundpush */
    osos_controller_push(controller, screen);
}
