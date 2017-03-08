/** Copyright 2011-2013 Thorsten Wißmann. All rights reserved.
 *
 * This software is licensed under the "Simplified BSD License".
 * See LICENSE for details */

#ifndef __HERBSTLUFT_SETTINGS_H_
#define __HERBSTLUFT_SETTINGS_H_

#include "glib-backports.h"
#include "x11-types.h"
#include "utils.h"
#include "object.h"
#include "attribute_.h"
#include <string>

enum {
    HS_String = 0,
    HS_Int,
    HS_Compatiblity,
};

typedef struct {
    const char*   name;
    union {
        int         i;
        const char* s_init;
        GString*    str;
        struct {
            const char* read;  // attribute address for reading
            const char* write; // attribute address for writing
        } compat;
    }   value;
    int old_value_i;
    int type;
    void (*on_change)(); // what to call on change
} SettingsPair;

class Settings : public Object {
public:
    using string = std::string;
    Settings();
    Attribute_<int>         frame_gap;
    Attribute_<int>         frame_padding;
    Attribute_<int>         window_gap;
    Attribute_<int>         snap_distance;
    Attribute_<int>         snap_gap;
    Attribute_<int>         mouse_recenter_gap;
    Attribute_<Color>       frame_border_active_color;
    Attribute_<Color>       frame_border_normal_color;
    Attribute_<Color>       frame_border_inner_color;
    Attribute_<Color>       frame_bg_normal_color;
    Attribute_<Color>       frame_bg_active_color;
    Attribute_<int>         frame_bg_transparent;
    Attribute_<int>         frame_transparent_width;
    Attribute_<int>         frame_border_width;
    Attribute_<int>         frame_border_inner_width;
    Attribute_<int>         frame_active_opacity;
    Attribute_<int>         frame_normal_opacity;
    Attribute_<int>         focus_crosses_monitor_boundaries;
    Attribute_<int>         always_show_frame;
    Attribute_<int>         default_direction_external_only;
    Attribute_<int>         default_frame_layout;
    Attribute_<int>         focus_follows_mouse;
    Attribute_<int>         focus_stealing_prevention;
    Attribute_<int>         swap_monitors_to_get_tag;
    Attribute_<int>         raise_on_focus;
    Attribute_<int>         raise_on_focus_temporarily;
    Attribute_<int>         raise_on_click;
    Attribute_<int>         gapless_grid;
    Attribute_<int>         smart_frame_surroundings;
    Attribute_<int>         smart_window_surroundings;
    Attribute_<int>         monitors_locked;
    Attribute_<int>         auto_detect_monitors;
    Attribute_<int>         pseudotile_center_threshold;
    Attribute_<int>         update_dragged_clients;
    Attribute_<string>      tree_style;
    Attribute_<string>      wmname;
    // for compatibility
    Attribute_<int>         window_border_width;
    Attribute_<int>         window_border_inner_width;
    Attribute_<Color>       window_border_inner_color;
    Attribute_<Color>       window_border_active_color;
    Attribute_<Color>       window_border_normal_color;
    Attribute_<Color>       window_border_urgent_color;
private:
    // callbacks for settings
    string relayout();
    string fr_colors();
    string cl_colors();
    string lock_changed();
    string focus_layer();
    string update_wmname();
};

extern int g_initial_monitors_locked;

void settings_init();
void settings_destroy();

SettingsPair* settings_find(const char* name);
SettingsPair* settings_get_by_index(int i);
char* settings_find_string(const char* name);

int settings_set(SettingsPair* pair, const char* value);
int settings_set_command(int argc, const char** argv, Output output);
int settings_toggle(int argc, char** argv, Output output);
int settings_cycle_value(int argc, char** argv, Output output);
int settings_count();
int settings_get(int argc, char** argv, Output output);

#endif

