/* SPDX-License-Identifier: GPL-3.0-only */

#include "up_next.h"

#include "cfw.h"
#include "play_next.h"
#include "queue_song_ui.h"
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

static const char *const song_actions[] = {"Play Now", "Move Up", "Move Down", "Remove"};
enum { PLAY_NOW, MOVE_UP, MOVE_DOWN, REMOVE, SONG_ACTIONS };

/* Upcoming-song index of the Queue row that opened the song actions. */
static uint32_t selected;

int cfw_up_next_property(uint32_t type, uint32_t property, uintptr_t *out)
{
    if (property == CFW_QueueSong_Property && type == OSOS_PROPERTY_COUNT) {
        *out = SONG_ACTIONS;
        return 1;
    }
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
    if (property == CFW_QueueSong_Property && index < SONG_ACTIONS) {
        if (type == OSOS_PROPERTY_STRING)
            *out = (uintptr_t)song_actions[index];
        else if (type == OSOS_RESOURCE_BMAP)
            *out = 0;
        else
            return 0;
        return 1;
    }
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

/* Undoes MediaList_LinkEntryAfterCursor for a song directly under the list root.
   The entry stays allocated in the list pool; no native free routine is known. */
static void unlink_entry(struct osos_media_list *list, struct osos_media_entry *entry)
{
    struct osos_media_entry *root = osos_list_root(list);
    struct osos_media_entry *next = entry->next, *previous = entry->previous;
    if (previous)
        previous->next = next;
    if (next)
        next->previous = previous;
    if (root->group.first == entry)
        root->group.first = next != entry ? next : NULL;
    if (root->group.last == entry)
        root->group.last = previous != entry ? previous : NULL;
    root->tracks--;

    /* Record-backed lookups walk record+0x24 then entry+0x28; drop this entry. */
    struct osos_media_record *record = entry->song.record;
    struct osos_media_entry **link = &record->entries;
    while (*link && *link != entry)
        link = &(*link)->song.next_record_entry;
    if (*link)
        *link = entry->song.next_record_entry;

    entry->parent = entry->next = entry->previous = NULL;
    entry->song.next_record_entry = NULL;
}

#define BATCH 64

/* Collects before unlinking so one order rebuild serves the whole batch. */
static unsigned unlink_batch(struct osos_player *player, uint32_t index, unsigned count)
{
    struct osos_media_list *list = player->playback.list;
    struct osos_media_entry *root = osos_list_root(list);
    struct osos_media_entry *current =
        osos_playback_entry(&player->playback, player->current_index);
    struct osos_media_entry *batch[BATCH];
    unsigned n = 0;
    if (!current)
        return 0;
    while (n < count && n < BATCH) {
        struct osos_media_entry *entry = osos_playback_entry(
            &player->playback, player->current_index + 1 + index + n);
        if (!entry || entry == current || entry->parent != root || !entry->song.record)
            break;
        batch[n++] = entry;
    }
    for (unsigned i = 0; i < n && root->tracks > 1; i++)
        unlink_entry(list, batch[i]);
    osos_media_list_invalidate_all_orders(list);
    return n;
}

/* Caller holds the player lock. Only songs after the current one can be removed. */
static unsigned remove_upcoming(struct osos_player *player, uint32_t index,
                                unsigned count)
{
    unsigned available = upcoming(player);
    if (index >= available)
        return 0;
    if (count > available - index)
        count = available - index;
    struct osos_media_list *list = player->playback.list;

    /* Artwork is indexed by queue position; stop its worker before mutation. */
    osos_player_stop_artwork(player);
    osos_database_begin_update(osos_list_database(list));
    unsigned removed = 0;
    while (removed < count) {
        /* Later songs shift down into index after each batch. */
        unsigned n = unlink_batch(player, index, count - removed);
        if (!n)
            break;
        removed += n;
    }
    osos_database_end_update(osos_list_database(list));
    osos_player_rebuild_artwork(player);
    if (removed) {
        /* A removed song may already be prepared as the next track. */
        osos_player_clear_prepared(player);
        osos_audio_set_playback_source(player->playback_source);
    }
    return removed;
}

/* Swaps neighbours a -> b in link order; the root tracks the ends. */
static void swap_adjacent(struct osos_media_entry *root, struct osos_media_entry *a,
                          struct osos_media_entry *b)
{
    struct osos_media_entry *previous = a->previous, *next = b->next;
    if (previous && previous != b)
        previous->next = b;
    if (next && next != a)
        next->previous = a;
    b->previous = previous;
    b->next = a;
    a->previous = b;
    a->next = next;
    if (root->group.first == a)
        root->group.first = b;
    if (root->group.last == b)
        root->group.last = a;
}

/* Caller holds the player lock. Swaps an upcoming song with its play-order neighbour. */
static unsigned move_upcoming(struct osos_player *player, uint32_t index, unsigned up)
{
    unsigned available = upcoming(player);
    if (index >= available || (up ? index == 0 : index + 1 >= available))
        return 0;
    struct osos_media_list *list = player->playback.list;
    struct osos_media_entry *root = osos_list_root(list);
    int base = player->current_index + 1;
    struct osos_media_entry *current =
        osos_playback_entry(&player->playback, player->current_index);
    struct osos_media_entry *x = osos_playback_entry(&player->playback, base + index);
    struct osos_media_entry *y =
        osos_playback_entry(&player->playback, base + index + (up ? -1 : 1));
    if (!current || !x || !y || x == current || y == current || x->parent != root ||
        y->parent != root)
        return 0;

    unsigned moved = 1;
    osos_player_stop_artwork(player);
    osos_database_begin_update(osos_list_database(list));
    if (osos_list_is_shuffled(list)) {
        /* Shuffle order sorts by rank, so trading ranks trades positions. */
        uint32_t rank = x->song.shuffle_rank;
        x->song.shuffle_rank = y->song.shuffle_rank;
        y->song.shuffle_rank = rank;
    } else if (x->next == y) {
        swap_adjacent(root, x, y);
    } else if (y->next == x) {
        swap_adjacent(root, y, x);
    } else {
        /* Play order is not link order here; leave the list untouched. */
        moved = 0;
    }
    if (moved)
        osos_media_list_invalidate_all_orders(list);
    osos_database_end_update(osos_list_database(list));
    osos_player_rebuild_artwork(player);
    if (moved) {
        osos_player_clear_prepared(player);
        osos_audio_set_playback_source(player->playback_source);
    }
    return moved;
}

/* Caller holds the player lock. Makes an upcoming song the next one to play;
   the songs it passes keep their order after it. */
static unsigned bring_next(struct osos_player *player, uint32_t index, unsigned unused)
{
    (void)unused;
    if (index >= upcoming(player))
        return 0;
    if (index == 0)
        return 1;
    struct osos_media_list *list = player->playback.list;
    struct osos_media_entry *root = osos_list_root(list);
    struct osos_media_entry *entry =
        osos_playback_entry(&player->playback, player->current_index + 1 + index);
    if (!entry || entry->parent != root || !entry->song.record)
        return 0;
    struct osos_media_item item;
    osos_media_item_init_record(&item, entry->song.record);
    if (!osos_media_item_valid(&item))
        return 0;

    /* Reuse Play Next's insertion, then drop the original by pointer. */
    unsigned tracks = root->tracks;
    cfw_play_next_add(player, &item, 0);
    if (root->tracks != tracks + 1 || entry->parent != root)
        return 0;
    osos_player_stop_artwork(player);
    osos_database_begin_update(osos_list_database(list));
    unlink_entry(list, entry);
    osos_media_list_invalidate_all_orders(list);
    osos_database_end_update(osos_list_database(list));
    osos_player_rebuild_artwork(player);
    osos_player_clear_prepared(player);
    osos_audio_set_playback_source(player->playback_source);
    return 1;
}

static unsigned edit(unsigned (*change)(struct osos_player *, uint32_t, unsigned),
                     uint32_t index, unsigned argument)
{
    struct osos_player *player = osos_player_get();
    osos_player_lock(player);
    unsigned changed = change(player, index, argument);
    osos_player_unlock(player);
    void *model = osos_ui_service_get(0x8900);
    if (changed && model)
        osos_model_notify(model, OSOS_NOTIFY_ALL, CFW_UpNext_Property);
    return changed;
}

int cfw_up_next_has_songs(void)
{
    struct osos_player *player = osos_player_get();
    osos_player_lock(player);
    int any = upcoming(player) != 0;
    osos_player_unlock(player);
    return any;
}

void cfw_up_next_clear(void)
{
    edit(remove_upcoming, 0, UINT32_MAX);
}

int cfw_up_next_action(const char *action, uint32_t argument)
{
    int select = cfw_string_equal(action, "CFW_UpNextSelect");
    if (!select && !cfw_string_equal(action, "CFW_QueueSongAction"))
        return 0;
    if (argument == OSOS_ACTION_SUPPORT_QUERY)
        return 1;
    uint32_t index = osos_selection_index(osos_ui_selection());
    if (select)
        selected = index;
    else if (index == PLAY_NOW) {
        /* Advance outside the player lock; the native call takes it itself. */
        if (edit(bring_next, selected, 0))
            osos_player_advance_track(osos_player_get(), 0);
    } else if (index == REMOVE)
        edit(remove_upcoming, selected, 1);
    else if (index < SONG_ACTIONS)
        edit(move_upcoming, selected, index == MOVE_UP);
    return 1;
}

void cfw_up_next_show(void *controller)
{
    void *screen =
        osos_create_screen_controller(osos_controller_registry(), CFW_UpNext_Screen,
                                      CFW_UpNext_Layout, 2); /* foregroundpush */
    osos_controller_push(controller, screen);
}
