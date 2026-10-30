/* SPDX-License-Identifier: GPL-3.0-only */
#include "osos.h"
#include "cfw.h"
#include "patch.h"
#include "play_next.h"
#include "play_similar.h"
#include "play_similar_ui.h"
#include "song_info.h"
#include "song_info_ui.h"
#include "up_next.h"
#include "up_next_ui.h"

int cfw_song_action_original(void *, const char *, uint32_t);
int cfw_playlist_action_original(void *, const char *, uint32_t);
int cfw_genius_action_original(void *, const char *, uint32_t);

enum song_source { SONG_LIST, PLAYLIST, GENIUS, NOW_PLAYING };

static int selected_item(void *controller, enum song_source source,
                         struct osos_media_item *selected)
{
    if (source == NOW_PLAYING) {
        osos_media_item_init(selected);
        osos_player_current_item(osos_player_get(), selected);
        return osos_media_item_valid(selected);
    }
    int row = *(int *)((uint8_t *)controller + (source == PLAYLIST ? 0xe8 : 0xd4));
    if (row < 0)
        return 0;

    if (source == GENIUS) {
        void *genius = osos_genius_get();
        row = osos_genius_map_row(genius, row);
        if (row < 0)
            return 0;
        osos_media_item_init(selected);
        osos_genius_get_item(genius, row, selected);
        return osos_media_item_valid(selected);
    }

    if (source == PLAYLIST) {
        row = osos_song_controller_map_row(controller, row);
        if (row < 0)
            return 0;
    }
    void *model = osos_song_controller_model(controller);
    if (!model)
        return 0;
    osos_song_model_get_item(model, row, selected);
    return osos_media_item_valid(selected);
}

static int song_action(void *controller, enum song_source source, const char *action,
                       uint32_t argument)
{
    int info = cfw_string_equal(action, "CFW_SongInfo");
    int similar = cfw_string_equal(action, "CFW_PlaySimilar");
    int last = cfw_string_equal(action, "CFW_PlayLast");
    int queue = !info && !similar;
    if (queue &&
        (source == NOW_PLAYING || (!last && !cfw_string_equal(action, "CFW_PlayNext"))))
        return 0;
    if (argument != OSOS_ACTION_SUPPORT_QUERY) {
        struct osos_player *player = osos_player_get();
        /* Play Similar scans the library first and takes the lock itself. */
        if (queue)
            osos_player_lock(player);
        struct osos_media_item item;
        struct osos_media_item *selected =
            selected_item(controller, source, &item) ? &item : NULL;
        if (info)
            cfw_song_info_show(controller, selected);
        else if (similar)
            cfw_play_similar(selected);
        else
            cfw_play_next_add(player, selected, last);
        if (queue)
            osos_player_unlock(player);
    }
    return 1;
}

PATCH_ARM void cfw_song_menu_cancel(struct osos_context_menu *menu, uint32_t label,
                                    const char *action, uint32_t flags)
{
    cfw_play_next_menu(menu);
    if (cfw_play_next_available())
        osos_context_menu_append(menu, CFW_PLAY_SIMILAR_LABEL, "CFW_PlaySimilar", 1);
    osos_context_menu_append(menu, CFW_SONG_INFO_LABEL, "CFW_SongInfo", 1);
    osos_context_menu_append(menu, label, action, flags);
}

PATCH_ARM int cfw_song_action(void *controller, const char *action, uint32_t argument)
{
    if (song_action(controller, SONG_LIST, action, argument))
        return 1;
    return cfw_song_action_original(controller, action, argument);
}

PATCH_ARM int cfw_playlist_action(void *controller, const char *action,
                                  uint32_t argument)
{
    if (song_action(controller, PLAYLIST, action, argument))
        return 1;
    return cfw_playlist_action_original(controller, action, argument);
}

PATCH_ARM int cfw_genius_action(void *controller, const char *action, uint32_t argument)
{
    if (song_action(controller, GENIUS, action, argument))
        return 1;
    return cfw_genius_action_original(controller, action, argument);
}

PATCH_ARM void cfw_now_playing_menu_cancel(struct osos_context_menu *menu,
                                           uint32_t label, const char *action,
                                           uint32_t flags)
{
    osos_context_menu_append(menu, CFW_UP_NEXT_LABEL, "CFW_UpNext", 1);
    if (cfw_up_next_has_songs())
        osos_context_menu_append(menu, CFW_CLEAR_QUEUE_LABEL, "CFW_ClearQueue", 1);
    if (cfw_play_next_available())
        osos_context_menu_append(menu, CFW_PLAY_SIMILAR_LABEL, "CFW_PlaySimilar", 1);
    osos_context_menu_append(menu, CFW_SONG_INFO_LABEL, "CFW_SongInfo", 1);
    osos_context_menu_append(menu, label, action, flags);
}

PATCH_ARM int cfw_now_playing_action(void *controller, const char *action,
                                     uint32_t argument)
{
    if (cfw_string_equal(action, "CFW_UpNext")) {
        if (argument != OSOS_ACTION_SUPPORT_QUERY)
            cfw_up_next_show(controller);
        return 1;
    }
    if (cfw_string_equal(action, "CFW_ClearQueue")) {
        if (argument != OSOS_ACTION_SUPPORT_QUERY)
            cfw_up_next_clear();
        return 1;
    }
    if (song_action(controller, NOW_PLAYING, action, argument))
        return 1;
    return osos_now_playing_action(controller, action, argument);
}
