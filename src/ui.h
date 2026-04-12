#pragma once
#ifndef LYRICBAR_UI_H
#define LYRICBAR_UI_H

#include <gtk/gtk.h>
#include <deadbeef/deadbeef.h>

#include "utils.h"

#ifdef __cplusplus

#include <string>
#include <vector>


using namespace std;

enum class ThemePaletteRole {
	Background,
	Highlight,
	Regular,
};

bool isValidHexaCode(string str);

bool is_dark_mode_active();

string get_palette_color_for_mode(bool dark_mode, ThemePaletteRole role);

string derive_dark_mode_color(const string& light_hex);

void set_lyrics(DB_playItem_t *track, string past, string present, string future, string padding);

void set_lyrics_with_scroll(DB_playItem_t *track, const vector<string>& all_lyrics, int current_line_index);

void sync_or_unsync(bool syncedlyrics);

vector<int> sizelines(DB_playItem_t *track, string lyrics);

void get_tags();

void refresh_theme_colors();

extern "C" {
#endif

GtkWidget *construct_lyricbar();

int message_handler(struct ddb_gtkui_widget_s *, uint32_t id, uintptr_t ctx, uint32_t, uint32_t);

void lyricbar_destroy();

#ifdef __cplusplus
}
#endif

#endif // LYRICBAR_UI_H
