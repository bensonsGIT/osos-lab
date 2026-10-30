/* SPDX-License-Identifier: GPL-3.0-only */

#include "play_similar.h"

#include "album_artist.h"
#include "cfw.h"
#include "play_next.h"
#include "up_next.h"

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

/* Field-wise so the payload, linked without libc, needs no memset. */
static void pool_init(struct pool *pool, unsigned capacity)
{
    pool->count = pool->seen = 0;
    pool->capacity = capacity;
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

static int separator(char c)
{
    return c == ';' || c == '/' || c == ',';
}

/* Reads the next separator-delimited genre, trimmed of spaces, and advances *text. */
static unsigned next_genre(const char **text, const char **start)
{
    const char *s = *text;
    while (*s == ' ')
        s++;
    *start = s;
    while (*s && !separator(*s))
        s++;
    const char *end = s;
    while (end > *start && end[-1] == ' ')
        end--;
    *text = *s ? s + 1 : s;
    return end - *start;
}

static char lower(char c)
{
    return c >= 'A' && c <= 'Z' ? c + ('a' - 'A') : c;
}

/* True when the separator-joined list holds genre x of n chars, ignoring ASCII case. */
static int has_genre(const char *list, const char *x, unsigned n)
{
    while (n && *list) {
        const char *y;
        unsigned i = 0, length = next_genre(&list, &y);
        while (i < n && i < length && lower(x[i]) == lower(y[i]))
            i++;
        if (i == n && i == length)
            return 1;
    }
    return 0;
}

/* Every genre tagged on the seed artist's songs, joined with ';'. Tagging often
   varies across an artist's songs, so this matches more than the seed's own tag. */
struct profile {
    char text[192];
    unsigned length;
};

static void profile_add(struct profile *profile, const char *tags)
{
    while (*tags) {
        const char *x;
        unsigned n = next_genre(&tags, &x);
        if (!n || has_genre(profile->text, x, n) ||
            profile->length + n + 2 > sizeof(profile->text))
            continue;
        if (profile->length)
            profile->text[profile->length++] = ';';
        for (unsigned i = 0; i < n; i++)
            profile->text[profile->length++] = x[i];
        profile->text[profile->length] = 0;
    }
}

static int genres_overlap(const char *tags, const char *profile)
{
    while (*tags) {
        const char *x;
        unsigned n = next_genre(&tags, &x);
        if (has_genre(profile, x, n))
            return 1;
    }
    return 0;
}

/* Adds the item's genres to the profile, or reports whether it shares one with it. */
static int genre_step(struct osos_media_item *item, struct profile *profile, int add)
{
    struct osos_native_string string;
    osos_native_string_init(&string);
    osos_media_item_text(item, OSOS_MEDIA_GENRE, &string);
    const char *text = osos_native_string_data(&string);
    int match = 0;
    if (text && add)
        profile_add(profile, text);
    else if (text)
        match = genres_overlap(text, profile->text);
    osos_native_string_destroy(&string);
    return match;
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

static struct osos_media_record *track(void *browser, struct osos_media_list *list,
                                       uint32_t index)
{
    struct osos_media_entry *entry = osos_list_order_entry(list, 6, 0, index);
    struct osos_media_record *record = entry ? entry->song.record : NULL;
    return record && osos_browser_filter(browser, entry, NULL) ? record : NULL;
}

enum { ARTIST, NEAR, GENRE, ERA, POOLS };

/* Walks every music track through a temporary library browser, as Album Artists does:
   first to build the artist's genre profile, then to sort candidates into pools. */
static void scan(struct osos_media_item *seed, struct pool *pools)
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
    struct profile profile;
    profile.length = 0;
    profile.text[0] = 0;
    genre_step(seed, &profile, 1);
    for (uint32_t i = 0; i < count; i++) {
        struct osos_media_record *record = track(browser, list, i);
        if (record && record != seed->record && same_artist(seed->record, record)) {
            struct osos_media_item item;
            osos_media_item_init_record(&item, record);
            if (osos_media_item_valid(&item))
                genre_step(&item, &profile, 1);
        }
    }

    uint16_t year = osos_media_item_year(seed);
    for (uint32_t i = 0; i < count; i++) {
        struct osos_media_record *record = track(browser, list, i);
        if (!record || record == seed->record)
            continue;
        if (same_artist(seed->record, record)) {
            offer(&pools[ARTIST], record);
            continue;
        }
        struct osos_media_item item;
        osos_media_item_init_record(&item, record);
        if (!osos_media_item_valid(&item))
            continue;
        uint16_t other = osos_media_item_year(&item);
        int near_year = year && other && (other > year ? other - year : year - other) <=
                                             NEAR_YEARS;
        if (profile.length && genre_step(&item, &profile, 0))
            offer(&pools[near_year ? NEAR : GENRE], record);
        else if (near_year)
            offer(&pools[ERA], record);
    }
    osos_model_ref_release(&reference);
}

void cfw_play_similar(struct osos_media_item *seed)
{
    if (!seed || !seed->record || !osos_media_item_valid(seed))
        return;
    if (!random_state)
        random_state = ((uint32_t(*)(void))OSOS_CRYPTO_RANDOM_NEXT_WORD)() | 1;

    struct pool pools[POOLS];
    for (unsigned p = 0; p < POOLS; p++)
        pool_init(&pools[p], p == ARTIST ? ARTIST_PICKS : PICKS);
    scan(seed, pools);

    /* Artist matches first, then shared genre and era, the rest of the shared genres,
       and finally any genre from the same era to fill niche-genre mixes. */
    struct osos_media_record *picks[PICKS];
    unsigned n = 0;
    for (unsigned p = 0; p < POOLS; p++)
        for (unsigned i = 0; i < pools[p].count && n < PICKS; i++)
            picks[n++] = pools[p].records[i];
    for (unsigned i = n; i > 1; i--) {
        unsigned j = next_random() % i;
        struct osos_media_record *swap = picks[i - 1];
        picks[i - 1] = picks[j];
        picks[j] = swap;
    }

    if (!n)
        return;
    /* The queue becomes the current song followed only by the picks. */
    cfw_up_next_clear();
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
