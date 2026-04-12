#include "ui.h"
#include "debug.h"
#include "gettext.h"
#include <deadbeef/deadbeef.h>

#include <vector>
#include <regex>
#include <gtkmm.h>
#include <gio/gio.h>
#include <time.h>
#include <string>
#include <future>
#include <chrono>
#include <atomic>
#include <algorithm>
#include <cctype>
#include <cmath>

using namespace std;
using namespace Gtk;
using namespace Glib;

struct linessizes{
	int titlesize;
	int artistsize;
	int newlinesize;
};

const DB_playItem_t *last;

struct timespec tss = {0, 100000000};

// TODO: eliminate all the global objects, as their initialization is not well defined

// Widget destruction flag - set to true when widgets are being destroyed
static atomic<bool> widgets_destroyed{false};

// Simplified stateless smooth scroll animation
static sigc::connection animation_connection;
static const int SCROLL_ANIMATION_INTERVAL = 16;     // ~60 FPS
static const double SCROLL_DAMPING_FACTOR = 0.15;    // Move 15% of distance per frame
static const double SCROLL_THRESHOLD = 0.5;          // Stop when within 0.5px

// NEW: Two-component layout
// Container
static VBox *mainVBox;

// Header (fixed, non-scrolling)
static TextView *headerView;
static RefPtr<TextBuffer> headerBuffer;

// Lyrics (scrollable)
static TextView *lyricsView;
static ScrolledWindow *lyricsScrolled;
static RefPtr<TextBuffer> lyricsBuffer;

// OLD: Keep for backward compatibility during transition
static TextView *lyricView;  // Will point to lyricsView
static ScrolledWindow *lyricbar;  // Will point to mainVBox
static RefPtr<TextBuffer> refBuffer;  // Will point to lyricsBuffer

static RefPtr<TextTag> tagItalic, tagBoldHeader, tagBoldLyrics, tagLarge, tagCenter, tagSmall, tagForegroundColorHighlight, tagForegroundColorRegular, tagLeftmargin, tagRightmargin, tagRegular;
static vector<RefPtr<TextTag>> tagsTitle, tagsArtist, tagsSyncline, tagsNosyncline, tagPadding;
static RefPtr<Gtk::CssProvider> cssProvider;

static GSettings *interface_settings = nullptr;
static gulong interface_settings_handler = 0;
static gulong gtk_theme_name_handler = 0;

// Lyrics buffer state for stable highlight/scroll
static DB_playItem_t *lyrics_buffer_track = nullptr;
static int lyrics_padding_lines = 0;
static int last_highlight_line = -1;
static int cached_lyrics_size = -1;
static bool lyrics_buffer_ready = false;

static const char *default_light_color(ThemePaletteRole role) {
	switch (role) {
		case ThemePaletteRole::Background:
			return "#F6F6F6";
		case ThemePaletteRole::Highlight:
			return "#571c1c";
		case ThemePaletteRole::Regular:
			return "#000000";
	}

	return "#000000";
}

static const char *legacy_color_key(ThemePaletteRole role) {
	switch (role) {
		case ThemePaletteRole::Background:
			return "lyricbar.backgroundcolor";
		case ThemePaletteRole::Highlight:
			return "lyricbar.highlightcolor";
		case ThemePaletteRole::Regular:
			return "lyricbar.regularcolor";
	}

	return "lyricbar.regularcolor";
}

static const char *palette_color_key(bool dark_mode, ThemePaletteRole role) {
	switch (role) {
		case ThemePaletteRole::Background:
			return dark_mode ? "lyricbar.dark.backgroundcolor" : "lyricbar.light.backgroundcolor";
		case ThemePaletteRole::Highlight:
			return dark_mode ? "lyricbar.dark.highlightcolor" : "lyricbar.light.highlightcolor";
		case ThemePaletteRole::Regular:
			return dark_mode ? "lyricbar.dark.regularcolor" : "lyricbar.light.regularcolor";
	}

	return "lyricbar.light.regularcolor";
}

static double clamp_double(double value, double min_value, double max_value) {
	return max(min_value, min(value, max_value));
}

static bool parse_hex_color(const string& hex_color, double& red, double& green, double& blue) {
	if (!isValidHexaCode(hex_color)) {
		return false;
	}

	int red_int = 0;
	int green_int = 0;
	int blue_int = 0;
	sscanf(hex_color.c_str(), "#%02x%02x%02x", &red_int, &green_int, &blue_int);
	red = red_int / 255.0;
	green = green_int / 255.0;
	blue = blue_int / 255.0;
	return true;
}

static string rgb_to_hex(double red, double green, double blue) {
	char hex_color[8];
	std::snprintf(
		hex_color,
		sizeof hex_color,
		"#%02x%02x%02x",
		(int)round(clamp_double(red, 0.0, 1.0) * 255.0),
		(int)round(clamp_double(green, 0.0, 1.0) * 255.0),
		(int)round(clamp_double(blue, 0.0, 1.0) * 255.0)
	);
	return hex_color;
}

static void rgb_to_hsl(double red, double green, double blue, double& hue, double& saturation, double& lightness) {
	double max_channel = max(red, max(green, blue));
	double min_channel = min(red, min(green, blue));
	double delta = max_channel - min_channel;

	lightness = (max_channel + min_channel) / 2.0;

	if (delta == 0.0) {
		hue = 0.0;
		saturation = 0.0;
		return;
	}

	saturation = delta / (1.0 - abs(2.0 * lightness - 1.0));

	if (max_channel == red) {
		hue = fmod((green - blue) / delta, 6.0);
	}
	else if (max_channel == green) {
		hue = ((blue - red) / delta) + 2.0;
	}
	else {
		hue = ((red - green) / delta) + 4.0;
	}

	hue *= 60.0;
	if (hue < 0.0) {
		hue += 360.0;
	}
}

static double hue_to_rgb(double p, double q, double t) {
	if (t < 0.0) {
		t += 1.0;
	}
	if (t > 1.0) {
		t -= 1.0;
	}
	if (t < 1.0 / 6.0) {
		return p + (q - p) * 6.0 * t;
	}
	if (t < 1.0 / 2.0) {
		return q;
	}
	if (t < 2.0 / 3.0) {
		return p + (q - p) * (2.0 / 3.0 - t) * 6.0;
	}
	return p;
}

static void hsl_to_rgb(double hue, double saturation, double lightness, double& red, double& green, double& blue) {
	if (saturation == 0.0) {
		red = lightness;
		green = lightness;
		blue = lightness;
		return;
	}

	double hue_normalized = hue / 360.0;
	double q = lightness < 0.5 ? lightness * (1.0 + saturation) : lightness + saturation - lightness * saturation;
	double p = 2.0 * lightness - q;

	red = hue_to_rgb(p, q, hue_normalized + 1.0 / 3.0);
	green = hue_to_rgb(p, q, hue_normalized);
	blue = hue_to_rgb(p, q, hue_normalized - 1.0 / 3.0);
}

string derive_dark_mode_color(const string& light_hex) {
	double red = 0.0;
	double green = 0.0;
	double blue = 0.0;
	if (!parse_hex_color(light_hex, red, green, blue)) {
		return light_hex;
	}

	double hue = 0.0;
	double saturation = 0.0;
	double lightness = 0.0;
	rgb_to_hsl(red, green, blue, hue, saturation, lightness);

	double dark_saturation = clamp_double(0.85 * saturation, 0.0, 0.8);
	double dark_lightness = clamp_double(0.72 - 0.60 * lightness, 0.30, 0.78);

	hsl_to_rgb(hue, dark_saturation, dark_lightness, red, green, blue);
	return rgb_to_hex(red, green, blue);
}

static string get_light_palette_color(ThemePaletteRole role) {
	const char *configured = deadbeef->conf_get_str_fast(palette_color_key(false, role), "");
	if (isValidHexaCode(configured)) {
		return configured;
	}

	const char *legacy = deadbeef->conf_get_str_fast(legacy_color_key(role), default_light_color(role));
	if (isValidHexaCode(legacy)) {
		return legacy;
	}

	return default_light_color(role);
}

string get_palette_color_for_mode(bool dark_mode, ThemePaletteRole role) {
	if (!dark_mode) {
		return get_light_palette_color(role);
	}

	const char *configured = deadbeef->conf_get_str_fast(palette_color_key(true, role), "");
	if (isValidHexaCode(configured)) {
		return configured;
	}

	return derive_dark_mode_color(get_light_palette_color(role));
}

static bool theme_name_requests_dark_mode() {
	GtkSettings *gtk_settings = gtk_settings_get_default();
	if (!gtk_settings) {
		return false;
	}

	gchar *theme_name = nullptr;
	g_object_get(gtk_settings, "gtk-theme-name", &theme_name, NULL);
	if (!theme_name) {
		return false;
	}

	string lowered_theme_name(theme_name);
	g_free(theme_name);
	transform(lowered_theme_name.begin(), lowered_theme_name.end(), lowered_theme_name.begin(), [](unsigned char ch) {
		return (char)tolower(ch);
	});
	return lowered_theme_name.find("-dark") != string::npos;
}

bool is_dark_mode_active() {
	if (interface_settings) {
		gchar *color_scheme = g_settings_get_string(interface_settings, "color-scheme");
		if (color_scheme) {
			string color_scheme_value(color_scheme);
			g_free(color_scheme);
			if (color_scheme_value == "prefer-dark") {
				return true;
			}
			if (color_scheme_value == "prefer-light") {
				return false;
			}
		}
	}

	return theme_name_requests_dark_mode();
}

static string get_active_palette_color(ThemePaletteRole role) {
	return get_palette_color_for_mode(is_dark_mode_active(), role);
}

static void apply_background_css() {
	if (!headerView || !lyricsView) {
		return;
	}

	string background_color = get_active_palette_color(ThemePaletteRole::Background);
	if (!isValidHexaCode(background_color)) {
		background_color = default_light_color(ThemePaletteRole::Background);
	}

	string css_config("\
		#headerView text {\
		background-color: ");
	css_config.append(background_color);
	css_config.append(";\
		}\
		#lyricsView text {\
		background-color: ");
	css_config.append(background_color);
	css_config.append(";\
		}\
	");

	if (!cssProvider) {
		cssProvider = Gtk::CssProvider::create();
		RefPtr<Gdk::Screen> screen = Gdk::Screen::get_default();
		if (screen) {
			Gtk::StyleContext::add_provider_for_screen(screen, cssProvider, GTK_STYLE_PROVIDER_PRIORITY_APPLICATION);
		}
	}

	cssProvider->load_from_data(css_config);

	headerView->queue_draw();
	lyricsView->queue_draw();
}

void refresh_theme_colors() {
	if (!lyricsBuffer) {
		return;
	}

	get_tags();
	apply_background_css();
	if (headerView) {
		headerView->queue_draw();
	}
	if (lyricsView) {
		lyricsView->queue_draw();
	}
}

static void on_system_theme_changed() {
	signal_idle().connect_once([] {
		refresh_theme_colors();
	});
}

static void on_interface_color_scheme_changed(GSettings *, gchar *, gpointer) {
	on_system_theme_changed();
}

static void on_gtk_theme_name_changed(GObject *, GParamSpec *, gpointer) {
	on_system_theme_changed();
}

static void initialize_theme_observers() {
	if (!interface_settings) {
		GSettingsSchemaSource *schema_source = g_settings_schema_source_get_default();
		if (schema_source) {
			GSettingsSchema *schema = g_settings_schema_source_lookup(schema_source, "org.gnome.desktop.interface", TRUE);
			if (schema) {
				interface_settings = g_settings_new_full(schema, NULL, NULL);
				g_settings_schema_unref(schema);
				interface_settings_handler = g_signal_connect(interface_settings, "changed::color-scheme", G_CALLBACK(on_interface_color_scheme_changed), NULL);
			}
		}
	}

	GtkSettings *gtk_settings = gtk_settings_get_default();
	if (gtk_settings && gtk_theme_name_handler == 0) {
		gtk_theme_name_handler = g_signal_connect(gtk_settings, "notify::gtk-theme-name", G_CALLBACK(on_gtk_theme_name_changed), NULL);
	}
}

bool isValidHexaCode(string str) {
    regex hexaCode("^#([a-fA-F0-9]{6}|[a-fA-F0-9]{3})$");
    return regex_match(str, hexaCode);
}

// Stateless smooth scroll animation tick - target is bound to the callback
static bool smooth_scroll_tick(double target) {
	// Check if widgets are valid
	if (widgets_destroyed.load() || !lyricsScrolled) {
		return false; // Stop animation
	}

	auto vadj = lyricsScrolled->get_vadjustment();
	if (!vadj) {
		return false;
	}

	// Get current position
	double current = vadj->get_value();
	double distance = target - current;

	// Check if we're close enough to stop
	if (abs(distance) < SCROLL_THRESHOLD) {
		vadj->set_value(target);
		return false; // Stop animation
	}

	// Apply damping: move a fraction of the distance
	double new_position = current + distance * SCROLL_DAMPING_FACTOR;

	// Clamp to valid range
	double page = vadj->get_page_size();
	double lower = vadj->get_lower();
	double upper = vadj->get_upper();
	double maxv = upper - page;
	if (maxv < lower) maxv = lower;
	if (new_position < lower) new_position = lower;
	if (new_position > maxv) new_position = maxv;

	vadj->set_value(new_position);
	return true; // Continue animation
}

// Start smooth scroll animation to target position
static void start_smooth_scroll(double target) {
	if (widgets_destroyed.load() || !lyricsScrolled) {
		return;
	}

	auto vadj = lyricsScrolled->get_vadjustment();
	if (!vadj) {
		return;
	}

	// Cancel any existing animation
	if (animation_connection.connected()) {
		animation_connection.disconnect();
	}

	// Start new animation with target bound to callback
	animation_connection = Glib::signal_timeout().connect(
		sigc::bind(sigc::ptr_fun(&smooth_scroll_tick), target),
		SCROLL_ANIMATION_INTERVAL
	);
}

static void update_highlight_line(int buffer_line) {
	if (!lyricsBuffer) {
		return;
	}

	int line_count = lyricsBuffer->get_line_count();
	if (buffer_line < 0 || buffer_line >= line_count) {
		return;
	}

	auto start_iter = lyricsBuffer->get_iter_at_line(buffer_line);
	TextIter end_iter;
	if (buffer_line + 1 < line_count) {
		end_iter = lyricsBuffer->get_iter_at_line(buffer_line + 1);
	} else {
		end_iter = lyricsBuffer->end();
	}

	lyricsBuffer->remove_tag(tagForegroundColorRegular, start_iter, end_iter);
	if (deadbeef->conf_get_int("lyricbar.bold", 1) == 1) {
		lyricsBuffer->apply_tag(tagBoldLyrics, start_iter, end_iter);
	}
	lyricsBuffer->apply_tag(tagForegroundColorHighlight, start_iter, end_iter);
}

static void clear_highlight_line(int buffer_line) {
	if (!lyricsBuffer) {
		return;
	}

	int line_count = lyricsBuffer->get_line_count();
	if (buffer_line < 0 || buffer_line >= line_count) {
		return;
	}

	auto start_iter = lyricsBuffer->get_iter_at_line(buffer_line);
	TextIter end_iter;
	if (buffer_line + 1 < line_count) {
		end_iter = lyricsBuffer->get_iter_at_line(buffer_line + 1);
	} else {
		end_iter = lyricsBuffer->end();
	}

	if (deadbeef->conf_get_int("lyricbar.bold", 1) == 1) {
		lyricsBuffer->remove_tag(tagBoldLyrics, start_iter, end_iter);
	}
	lyricsBuffer->remove_tag(tagForegroundColorHighlight, start_iter, end_iter);
	lyricsBuffer->apply_tag(tagForegroundColorRegular, start_iter, end_iter);
}

static void schedule_scroll_to_line(int buffer_line) {
	signal_idle().connect_once([buffer_line]() {
		if (widgets_destroyed.load() || !lyricsView || !lyricsScrolled || !lyricsBuffer) {
			return;
		}

		if (buffer_line < 0 || buffer_line >= lyricsBuffer->get_line_count()) {
			return;
		}

		auto iter = lyricsBuffer->get_iter_at_line(buffer_line);
		Gdk::Rectangle rect;
		lyricsView->get_iter_location(iter, rect);

		double line_center = (double)rect.get_y() + rect.get_height() / 2.0;

		auto vadj = lyricsScrolled->get_vadjustment();
		double page = vadj->get_page_size();
		double lower = vadj->get_lower();
		double upper = vadj->get_upper();
		double target = line_center - page / 2.0;

		double maxv = upper - page;
		if (maxv < lower) maxv = lower;
		if (target < lower) target = lower;
		if (target > maxv) target = maxv;

		start_smooth_scroll(target);
	});
}

// "Size" lyrics to be able to make lines "dissapear" on top.
// INTERNAL: This function MUST only be called from the main GTK thread!
static vector<int> sizelines_internal(DB_playItem_t * track, string lyrics) {
	// Check if widgets are being destroyed
	if (widgets_destroyed.load()) {
		return vector<int>();
	}

	// Validate widget pointers
	if (!lyricView || !lyricbar || !refBuffer) {
		return vector<int>();
	}

	if (!is_playing(track)) {
		return vector<int>();
	}

	string artist, title;
	deadbeef->pl_lock();
	artist = deadbeef->pl_find_meta(track, "artist") ?: _("Unknown Artist");
	title  = deadbeef->pl_find_meta(track, "title") ?: _("Unknown Title");
	deadbeef->pl_unlock();

	refBuffer->erase(refBuffer->begin(), refBuffer->end());
	refBuffer->insert_with_tags(refBuffer->begin(), title, tagsTitle);
	if (g_utf8_validate(lyrics.c_str(),-1,NULL)){
		refBuffer->insert_with_tags(refBuffer->end(), string{"\n"} + artist + "\n\n", tagsArtist);
		refBuffer->insert_with_tags(refBuffer->end(), lyrics, tagsNosyncline);
	}

//	std::cout << "Sizelines" << "\n";
	death_signal = 1;
	int sumatory = 0;
	int temporaly = 0;
	vector<int>  values;

//	I didn't found another way to be sure lyrics are displayed than wait millisenconds with nanosleep.
	nanosleep(&tss, NULL);

	// Re-check after sleep
	if (widgets_destroyed.load() || !lyricView || !lyricbar || !refBuffer) {
		death_signal = 0;
		return vector<int>();
	}

	deadbeef->pl_lock();
	values.push_back(lyricbar->get_allocation().get_height()*(deadbeef->conf_get_int("lyricbar.vpostion", 50))/100);
	deadbeef->pl_unlock();
	values.push_back(0);
	Gdk::Rectangle rectangle;
	for (int i = 2; i < refBuffer->get_line_count()-1; i++) {
		lyricView->get_iter_location(refBuffer->get_iter_at_line(i-2), rectangle);
		values.push_back(rectangle.get_y() - temporaly);
		temporaly = rectangle.get_y();
	}

	values[1] = values.size()-3;

	for (unsigned i = 2; i < values.size()-2; i++) {
		sumatory += values[i];
		if (sumatory > (values[0] - values[3] - values[4])) {
			values[1] = i-2;
			break;
		}
	}
//	std::cout << "Sizelines finished" << "\n";
	death_signal = 0;

	return values;
}

// Thread-safe wrapper for sizelines - can be called from any thread
vector<int> sizelines(DB_playItem_t * track, string lyrics) {
	// Check if widgets are destroyed before even trying
	if (widgets_destroyed.load()) {
		return vector<int>();
	}

	// Create promise/future for result
	auto promise_ptr = make_shared<promise<vector<int>>>();
	future<vector<int>> result_future = promise_ptr->get_future();

	// Marshal to main GTK thread
	signal_idle().connect_once([track, lyrics, promise_ptr]() {
		try {
			vector<int> result = sizelines_internal(track, lyrics);
			promise_ptr->set_value(result);
		} catch (...) {
			// If any exception occurs, return empty vector
			promise_ptr->set_value(vector<int>());
		}
	});

	// Wait for result with timeout (5 seconds)
	if (result_future.wait_for(chrono::seconds(5)) == future_status::timeout) {
		// Timeout - return empty vector
		return vector<int>();
	}

	return result_future.get();
}

// TWO-COMPONENT VERSION: Updates header and lyrics separately
void set_lyrics(DB_playItem_t *track, string past, string present, string future, string padding) {
	signal_idle().connect_once([track, past, present, future, padding ] {

		// Check if widgets are destroyed
		if (widgets_destroyed.load() || !headerBuffer || !lyricsBuffer) {
			return;
		}

		if (!is_playing(track)) {
			return;
		}
		string artist, title;
		deadbeef->pl_lock();
		artist = deadbeef->pl_find_meta(track, "artist") ?: _("Unknown Artist");
		title  = deadbeef->pl_find_meta(track, "title") ?: _("Unknown Title");
		deadbeef->pl_unlock();

		// Update HEADER buffer (fixed, non-scrolling)
		headerBuffer->erase(headerBuffer->begin(), headerBuffer->end());
		headerBuffer->insert_with_tags(headerBuffer->begin(), title, tagsTitle);
		headerBuffer->insert_with_tags(headerBuffer->end(), "\n" + artist, tagsArtist);

		// Update LYRICS buffer (scrollable)
		lyricsBuffer->erase(lyricsBuffer->begin(), lyricsBuffer->end());

		if (g_utf8_validate(future.c_str(),-1,NULL)){
			lyricsBuffer->insert_with_tags(lyricsBuffer->end(), padding, tagPadding);
			lyricsBuffer->insert_with_tags(lyricsBuffer->end(),past, tagsNosyncline);
			lyricsBuffer->insert_with_tags(lyricsBuffer->end(),present, tagsSyncline);
			lyricsBuffer->insert_with_tags(lyricsBuffer->end(),future, tagsNosyncline);
		}
		else{
			death_signal = 1;
			string error = "Wrong character encoding";
			lyricsBuffer->insert_with_tags(lyricsBuffer->end(), padding, tagPadding);
			lyricsBuffer->insert_with_tags(lyricsBuffer->end(),error, tagsSyncline);
		}




	});
}

// New simpler scroll implementation - centers current line
// TWO-COMPONENT VERSION: Updates header and lyrics separately
void set_lyrics_with_scroll(DB_playItem_t *track, const vector<string>& all_lyrics, int current_line_index) {
	signal_idle().connect_once([track, all_lyrics, current_line_index] {

		// Check if widgets are destroyed
		if (widgets_destroyed.load() || !headerBuffer || !lyricsBuffer || !lyricsView) {
			return;
		}

		if (!is_playing(track)) {
			return;
		}

		string artist, title;
		deadbeef->pl_lock();
		artist = deadbeef->pl_find_meta(track, "artist") ?: _("Unknown Artist");
		title  = deadbeef->pl_find_meta(track, "title") ?: _("Unknown Title");
		deadbeef->pl_unlock();

		// Update HEADER buffer (fixed, non-scrolling)
		headerBuffer->erase(headerBuffer->begin(), headerBuffer->end());
		headerBuffer->insert_with_tags(headerBuffer->begin(), title, tagsTitle);
		headerBuffer->insert_with_tags(headerBuffer->end(), "\n" + artist, tagsArtist);

		bool needs_rebuild = false;
		if (!lyrics_buffer_ready || track != lyrics_buffer_track) {
			needs_rebuild = true;
		}
		if (cached_lyrics_size != (int)all_lyrics.size()) {
			needs_rebuild = true;
		}

		if (needs_rebuild) {
			lyricsBuffer->erase(lyricsBuffer->begin(), lyricsBuffer->end());
			lyrics_padding_lines = 0;

			int viewport_height = 0;
			if (lyricsScrolled) {
				viewport_height = lyricsScrolled->get_allocation().get_height();
			}

			int line_height = 0;
			if (!all_lyrics.empty()) {
				lyricsBuffer->insert_with_tags(lyricsBuffer->begin(), "X\n", tagsNosyncline);
				Gdk::Rectangle rect;
				lyricsView->get_iter_location(lyricsBuffer->get_iter_at_line(0), rect);
				line_height = rect.get_height();
				lyricsBuffer->erase(lyricsBuffer->begin(), lyricsBuffer->end());
			}

			if (viewport_height > 0 && line_height > 0) {
				lyrics_padding_lines = (viewport_height / 2) / line_height + 1;
			}
			if (lyrics_padding_lines <= 0) {
				lyrics_padding_lines = 10;
			}

			for (int i = 0; i < lyrics_padding_lines; i++) {
				lyricsBuffer->insert_with_tags(lyricsBuffer->end(), "\n", tagsNosyncline);
			}

			for (const auto &line : all_lyrics) {
				lyricsBuffer->insert_with_tags(lyricsBuffer->end(), line + "\n", tagsNosyncline);
			}

			for (int i = 0; i < lyrics_padding_lines; i++) {
				lyricsBuffer->insert_with_tags(lyricsBuffer->end(), "\n", tagsNosyncline);
			}

			lyrics_buffer_track = track;
			lyrics_buffer_ready = true;
			cached_lyrics_size = static_cast<int>(all_lyrics.size());
			last_highlight_line = -1;
		}

		if (last_highlight_line >= 0) {
			clear_highlight_line(last_highlight_line);
			last_highlight_line = -1;
		}

		if (current_line_index >= 0 && current_line_index < (int)all_lyrics.size()) {
			int buffer_line = lyrics_padding_lines + current_line_index;
			update_highlight_line(buffer_line);
			last_highlight_line = buffer_line;
		schedule_scroll_to_line(buffer_line);
		}
	});
}

// To have scroll bars or not when lyrics are synced or not.
// TWO-COMPONENT VERSION: Only affects lyrics ScrolledWindow
void sync_or_unsync(bool syncedlyrics) {
	if (syncedlyrics == true) {
		lyricsScrolled->set_policy(POLICY_EXTERNAL, POLICY_EXTERNAL);
	}
	else{
		lyricsScrolled->set_policy(POLICY_AUTOMATIC, POLICY_AUTOMATIC);
	}
}

Justification get_justification() {

	int align = deadbeef->conf_get_int("lyricbar.lyrics.alignment", 1);
	switch (align) {
		case 0:
			return JUSTIFY_LEFT;
		case 2:
			return JUSTIFY_RIGHT;
		default:
			return JUSTIFY_CENTER;
	}
}

	void get_tags() {
	// Create tags for HEADER buffer
	tagItalic = headerBuffer->create_tag();
	tagItalic->property_style() = Pango::STYLE_ITALIC;
	tagItalic->property_scale() = deadbeef->conf_get_float("lyricbar.fontscale", 1);

	tagBoldHeader = headerBuffer->create_tag();
	tagBoldHeader->property_scale() = deadbeef->conf_get_float("lyricbar.fontscale", 1);
	tagBoldHeader->property_weight() = Pango::WEIGHT_BOLD;

	tagLarge = headerBuffer->create_tag();
	tagLarge->property_scale() = 1.2*deadbeef->conf_get_float("lyricbar.fontscale", 1);

	tagCenter = headerBuffer->create_tag();
	tagCenter->property_justification() = JUSTIFY_CENTER;

	tagsTitle = {tagLarge, tagBoldHeader, tagCenter};
	tagsArtist = {tagItalic, tagCenter};

	// Create tags for LYRICS buffer
	tagRegular = lyricsBuffer->create_tag();
	tagRegular->property_scale() = deadbeef->conf_get_float("lyricbar.fontscale", 1);

	tagLeftmargin = lyricsBuffer->create_tag();
	tagLeftmargin->property_left_margin() = deadbeef->conf_get_float("lyricbar.border", 22)*deadbeef->conf_get_float("lyricbar.fontscale", 1);

	tagRightmargin = lyricsBuffer->create_tag();
	tagRightmargin->property_right_margin() = deadbeef->conf_get_float("lyricbar.border", 22)*deadbeef->conf_get_float("lyricbar.fontscale", 1);

	tagBoldLyrics = lyricsBuffer->create_tag();
	tagBoldLyrics->property_scale() = deadbeef->conf_get_float("lyricbar.fontscale", 1);
	tagBoldLyrics->property_weight() = Pango::WEIGHT_BOLD;

	tagSmall = lyricsBuffer->create_tag();
	tagSmall->property_scale() = 0.0001;

	tagForegroundColorHighlight = lyricsBuffer->create_tag();
	tagForegroundColorRegular = lyricsBuffer->create_tag();
	string highlight_color = get_active_palette_color(ThemePaletteRole::Highlight);
	string regular_color = get_active_palette_color(ThemePaletteRole::Regular);

	if (isValidHexaCode(highlight_color)){
		tagForegroundColorHighlight->property_foreground() = highlight_color;
	}
	else{
		tagForegroundColorHighlight->property_foreground() = "#571c1c";
	}

	if (isValidHexaCode(regular_color)){
		tagForegroundColorRegular->property_foreground() = regular_color;
	}
	else{
		tagForegroundColorRegular->property_foreground() = "#000000";
	}

	tagsSyncline = {tagBoldLyrics, tagForegroundColorHighlight};

	if (deadbeef->conf_get_int("lyricbar.bold", 1) == 1) {
		tagsSyncline = {tagBoldLyrics, tagForegroundColorHighlight};
		tagsNosyncline = {tagRegular, tagLeftmargin, tagRightmargin, tagForegroundColorRegular};
	}
	else{
		tagsSyncline = {tagRegular, tagForegroundColorHighlight};
		tagsNosyncline = {tagRegular, tagForegroundColorRegular};
    	}

    if (get_justification() == JUSTIFY_LEFT) {
		tagRightmargin->property_right_margin() = deadbeef->conf_get_float("lyricbar.border", 22)*deadbeef->conf_get_float("lyricbar.fontscale", 1);
		tagsNosyncline = {tagRegular, tagRightmargin};
    }
	if (get_justification() == JUSTIFY_RIGHT) {
    	tagLeftmargin->property_left_margin() = deadbeef->conf_get_float("lyricbar.border", 22)*deadbeef->conf_get_float("lyricbar.fontscale", 1);
		tagsNosyncline = {tagRegular, tagLeftmargin};
    }

	tagPadding = {tagSmall};
}

extern "C"

GtkWidget *construct_lyricbar() {
	Gtk::Main::init_gtkmm_internals();
	initialize_theme_observers();

	// Create TWO separate buffers
	headerBuffer = TextBuffer::create();
	lyricsBuffer = TextBuffer::create();
	get_tags();

	// Create HEADER view (fixed, non-scrolling)
	headerView = new TextView(headerBuffer);
	headerView->set_editable(false);
	headerView->set_can_focus(false);
	headerView->set_name("headerView");
	headerView->set_justification(JUSTIFY_CENTER);
	headerView->set_wrap_mode(WRAP_WORD_CHAR);
	// Set fixed height for approximately 2 lines
	headerView->set_size_request(-1, 60);  // Fixed height
	headerView->show();

	// Create LYRICS view (scrollable)
	lyricsView = new TextView(lyricsBuffer);
	lyricsView->set_editable(false);
	lyricsView->set_can_focus(false);
	lyricsView->set_name("lyricsView");
	lyricsView->set_left_margin(2);
	lyricsView->set_right_margin(2);
	lyricsView->set_justification(get_justification());
	lyricsView->set_wrap_mode(WRAP_WORD_CHAR);
    if (get_justification() == JUSTIFY_LEFT) {
    	lyricsView->set_left_margin(20);
    }
	lyricsView->show();

	// Wrap lyrics in ScrolledWindow
	lyricsScrolled = new ScrolledWindow();
	lyricsScrolled->add(*lyricsView);
	lyricsScrolled->set_name("lyricsScrolled");
	lyricsScrolled->set_policy(POLICY_EXTERNAL, POLICY_EXTERNAL);
	lyricsScrolled->show();

	// Create VBox container
	mainVBox = new VBox(false, 0);  // not homogeneous, no spacing
	mainVBox->pack_start(*headerView, PACK_SHRINK, 0);  // Fixed size
	mainVBox->pack_start(*lyricsScrolled, PACK_EXPAND_WIDGET, 0);  // Expand to fill
	mainVBox->show();

	// Set backward compatibility pointers
	lyricView = lyricsView;
	refBuffer = lyricsBuffer;

	/**********/

	apply_background_css();


	return GTK_WIDGET(mainVBox->gobj());
}




extern "C"
int message_handler(struct ddb_gtkui_widget_s*, uint32_t id, uintptr_t ctx, uint32_t, uint32_t) {
	auto event = reinterpret_cast<ddb_event_track_t *>(ctx);
	switch (id) {
		case DB_EV_CONFIGCHANGED:
			debug_out << "CONFIG CHANGED\n";
			refresh_theme_colors();
			signal_idle().connect_once([]{ lyricView->set_justification(get_justification()); });
			break;
		case DB_EV_TERMINATE:
			debug_out << "DEADBEEF CLOSED \n";
			death_signal = 1;
			break;
//		case DB_EV_TRACKINFOCHANGED:
//			debug_out << "TRACKINFOCHANGED" << "\n";
//			break;
		case DB_EV_PLUGINSLOADED:
		case DB_EV_SONGSTARTED:
			debug_out << "SONG STARTED\n";
			if (!event->track || event->track == last || deadbeef->pl_get_item_duration(event->track) <= 0.0){
//				std::cout << "if in" << "\n";
				return 0;
			}
			last = event->track;
//			std::cout << "SONG STARTED" << "\n";
			auto tid = deadbeef->thread_start(update_lyrics, event->track);
			deadbeef->thread_detach(tid);
			break;
	}

	return 0;
}

extern "C"
void lyricbar_destroy() {
	// Set destruction flag FIRST to prevent any new GTK operations
	widgets_destroyed.store(true);

	// Stop smooth scroll animation
	if (animation_connection.connected()) {
		animation_connection.disconnect();
	}

	// Give pending operations a moment to check the flag
	struct timespec brief_wait = {0, 10000000}; // 10ms
	nanosleep(&brief_wait, NULL);

	// Delete new widgets (VBox will auto-delete children)
	delete mainVBox;  // This deletes headerView and lyricsScrolled
	// Note: lyricsView is deleted by lyricsScrolled
	// Note: headerView is deleted by mainVBox

	// Clear tag vectors
	tagsArtist.clear();
	tagPadding.clear();
	tagsSyncline.clear();
	tagsNosyncline.clear();
	tagRegular.clear();
	tagsTitle.clear();

	// Reset tag RefPtrs
	tagLarge.reset();
	tagSmall.reset();
	tagBoldHeader.reset();
	tagBoldLyrics.reset();
	tagItalic.reset();
	tagRightmargin.reset();
	tagLeftmargin.reset();
	tagCenter.reset();
	tagForegroundColorHighlight.reset();
	tagForegroundColorRegular.reset();
	cssProvider.reset();

	// Reset buffer RefPtrs
	headerBuffer.reset();
	lyricsBuffer.reset();

	// Reset backward compatibility pointers (don't delete, already deleted)
	lyricView = nullptr;
	lyricbar = nullptr;
	refBuffer.reset();
	lyrics_buffer_track = nullptr;
	lyrics_buffer_ready = false;
	last_highlight_line = -1;
	lyrics_padding_lines = 0;
	cached_lyrics_size = -1;

	if (interface_settings) {
		if (interface_settings_handler != 0) {
			g_signal_handler_disconnect(interface_settings, interface_settings_handler);
			interface_settings_handler = 0;
		}
		g_object_unref(interface_settings);
		interface_settings = nullptr;
	}

	GtkSettings *gtk_settings = gtk_settings_get_default();
	if (gtk_settings && gtk_theme_name_handler != 0) {
		g_signal_handler_disconnect(gtk_settings, gtk_theme_name_handler);
		gtk_theme_name_handler = 0;
	}

	// Reset flag for potential re-initialization
	widgets_destroyed.store(false);
}
