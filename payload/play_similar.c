/* SPDX-License-Identifier: GPL-3.0-only */

#include "play_similar.h"

#include "album_artist.h"
#include "cfw.h"
#include "play_next.h"

#define FIELD(type, object, offset) (*(type *)((uint8_t *)(object) + (offset)))

#define PICKS 25
#define ARTIST_PICKS 8
#define NEAR_YEARS 5

/* Reservoir sample: every candidate seen has an equal chance of being kept. */
struct pool {
    struct osos_media_record *records[PICKS];
    unsigned count, capacity, seen;
};

static uint32_t random_state;

static uint32_t next_random(void)
{
    /* xorshift32, seeded once from the hardware generator. */
    uint32_t x = random_state;
    x ^= x << 13;
    x ^= x >> 17;
    x ^= x << 5;
    return random_state = x;
}

static void offer(struct pool *pool, struct osos_media_record *record)
{
    pool->seen++;
    if (pool->count < pool->capacity) {
        pool->records[pool->count++] = record;
    } else {
        uint32_t slot = next_random() % pool->seen;
        if (slot < pool->capacity)
            pool->records[slot] = record;
    }
}

static int genre_equal(struct osos_media_item *item, const char *genre)
{
    struct osos_native_string string;
    osos_native_string_init(&string);
    osos_media_item_text(item, OSOS_MEDIA_GENRE, &string);
    const char *text = osos_native_string_data(&string);
    int equal = text && *text && cfw_string_equal(text, genre);
    osos_native_string_destroy(&string);
    return equal;
}

static uint32_t artist_token(struct osos_media_record *record)
{
    return FIELD(uint32_t, record, 0x2c);
}

static int same_artist(struct osos_media_record *a, struct osos_media_record *b)
{
    uint32_t artist = artist_token(a), album_artist = cfw_album_artist_token(a);
    return (artist && artist == artist_token(b)) ||
           (album_artist && album_artist == cfw_album_artist_token(b));
}

/* Walks every music track through a temporary library browser, as Album Artists does. */
static void scan(struct osos_media_record *seed, const char *genre, uint16_t year,
                 struct pool *artist, struct pool *near, struct pool *genre_pool)
{
    void **registry = (void **)OSOS_BROWSER_REGISTRY;
    unsigned available = 0;
    for (unsigned i = 0; i < 4; i++)
        available += registry[i] == NULL;
    /* Leave a slot for native auxiliary browsing. */
    if (available < 2)
        return;
    void *model = osos_model_create(0);
    void *browser = NULL;
    if (osos_browser_create(FIELD(void *, model, 0x40), &browser)) {
        osos_model_destroy(model);
        return;
    }
    FIELD(void *, model, 0x40) = browser;
    FIELD(uint8_t, model, 0x45) = 2;
    void *reference = NULL;
    osos_model_ref_init(&reference, model, 0);
    osos_library_model_prepare_artists(model);

    struct osos_media_list *list = osos_browser_current_list(browser);
    uint32_t count = list && osos_list_root(list) ? osos_list_root(list)->tracks : 0;
    for (uint32_t i = 0; i < count; i++) {
        struct osos_media_entry *entry = osos_list_order_entry(list, 6, 0, i);
        struct osos_media_record *record = entry ? entry->song.record : NULL;
        if (!record || record == seed || !osos_browser_filter(browser, entry, NULL))
            continue;
        if (same_artist(seed, record)) {
            offer(artist, record);
            continue;
        }
        if (!*genre)
            continue;
        struct osos_media_item item;
        osos_media_item_init_record(&item, record);
        if (!osos_media_item_valid(&item) || !genre_equal(&item, genre))
            continue;
        uint16_t other = osos_media_item_year(&item);
        int near_year = year && other && (other > year ? other - year : year - other) <=
                                             NEAR_YEARS;
        offer(near_year ? near : genre_pool, record);
    }
    osos_model_ref_release(&reference);
}

void cfw_play_similar(struct osos_media_item *seed)
{
    if (!seed || !seed->record || !osos_media_item_valid(seed))
        return;
    if (!random_state)
        random_state = ((uint32_t(*)(void))OSOS_CRYPTO_RANDOM_NEXT_WORD)() | 1;

    char genre[64];
    unsigned length = 0;
    struct osos_native_string string;
    osos_native_string_init(&string);
    osos_media_item_text(seed, OSOS_MEDIA_GENRE, &string);
    const char *text = osos_native_string_data(&string);
    while (text && text[length] && length < sizeof(genre) - 1) {
        genre[length] = text[length];
        length++;
    }
    genre[length] = 0;
    osos_native_string_destroy(&string);

    struct pool artist = {.capacity = ARTIST_PICKS};
    struct pool near = {.capacity = PICKS}, genre_pool = {.capacity = PICKS};
    scan(seed->record, genre, osos_media_item_year(seed), &artist, &near, &genre_pool);

    /* Artist matches first, then same genre and era, then the rest of the genre. */
    struct osos_media_record *picks[PICKS];
    unsigned n = 0;
    struct pool *pools[] = {&artist, &near, &genre_pool};
    for (unsigned p = 0; p < 3; p++)
        for (unsigned i = 0; i < pools[p]->count && n < PICKS; i++)
            picks[n++] = pools[p]->records[i];
    for (unsigned i = n; i > 1; i--) {
        unsigned j = next_random() % i;
        struct osos_media_record *swap = picks[i - 1];
        picks[i - 1] = picks[j];
        picks[j] = swap;
    }

    struct osos_player *player = osos_player_get();
    osos_player_lock(player);
    /* Play Next inserts directly after the current song, so add in reverse. */
    while (n--) {
        struct osos_media_item item;
        osos_media_item_init_record(&item, picks[n]);
        cfw_play_next_add(player, &item, 0);
    }
    osos_player_unlock(player);
}
