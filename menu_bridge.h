                                                                              
                                                                            
   
#ifndef MENU_BRIDGE_H
#define MENU_BRIDGE_H

#ifdef __cplusplus
extern "C" {
#endif

#define BR_MAGIC 0x57444D31u
#define BR_VERSION 22u
#define BR_COLOR_COUNT 63

enum {
    BR_C_FRIEND_ESP=43,BR_C_FRIEND_CHAMS=44,
    BR_C_BOX_VISIBLE=45,BR_C_SKELETON_VISIBLE=48,BR_C_HEAD_VISIBLE=51,BR_C_TEXT_VISIBLE=54,
    BR_C_FRIEND_ESP_VISIBLE=57,BR_C_FRIEND_CHAMS_VISIBLE=60
};

#define BR_LABEL_SETTINGS(X, N) \
    X(esp_##N##_free, 0, 0, 1) \
    X(esp_##N##_x, 0, -3000, 3000) X(esp_##N##_y, 0, -3000, 4000)

#define BR_WEAPON_SETTINGS(X, N) \
    X(w##N##_bone, 0, 0, 9) X(w##N##_fov, 150, 30, 600) \
    X(w##N##_smooth, 5, 1, 20) X(w##N##_cal, 100, 10, 2000) \
    X(w##N##_speed, 0, 0, 3000) X(w##N##_gravity, 981, 0, 3000) \
    X(w##N##_recoil_x, 0, -500, 500) X(w##N##_recoil_y, 0, -500, 500)

                                                                            
#define BR_HUD_SETTINGS(X, N) \
    X(h##N##_radar_x, 30, 0, 1840) X(h##N##_radar_y, 40, 0, 1010) \
    X(h##N##_radar_w, 210, 80, 800) X(h##N##_radar_h, 180, 70, 800) \
    X(h##N##_status_x, 12, 0, 1700) X(h##N##_status_y, 1040, 0, 1050) \
    X(h##N##_target_x, 1550, 0, 1700) X(h##N##_target_y, 40, 0, 1000) \
    X(h##N##_loot_x, 800, 0, 1700) X(h##N##_loot_y, 1000, 0, 1040)

#define BR_EXTRA_SETTINGS(X) \
    X(always_head_on, 0, 0, 1) \
    X(friend_bind_on, 1, 0, 1) X(friend_key, 0x76, 1, 254) X(friend_radius, 12, 0, 50) \
    X(show_friends, 1, 0, 1) X(ignore_friends, 1, 0, 1) X(friend_visibility, 0, 0, 1) \
    X(box_visibility, 0, 0, 1) X(skeleton_visibility, 0, 0, 1) X(head_visibility, 0, 0, 1) X(text_visibility, 0, 0, 1) \
    X(fov_on, 0, 0, 1) X(camera_fov, 0, 0, 140) \
    X(aspect_on, 0, 0, 1) X(aspect_preset, 0, 0, 5) X(aspect_value, 1333, 500, 4000) X(aspect_pct, 0, 0, 200) \
    X(zoom_on, 0, 0, 1) X(zoom_key, 0x43, 1, 254) X(zoom_power, 200, 125, 800) X(zoom_smooth, 0, 0, 250) \
    X(freelook_on, 0, 0, 1) X(freelook_key, 0x72, 1, 254) X(freecam_speed, 1200, 100, 20000) \
    X(esp_opacity, 100, 5, 100) X(text_size, 12, 8, 24) X(font_face, 0, 0, 2) \
    X(text_background, 35, 0, 100) X(text_outline, 1, 0, 1) \
    X(box_style, 0, 0, 3) X(box_civilian, 0, 0, 4) X(box_police, 0, 0, 4) X(box_robber, 0, 0, 4) \
    X(box_width, 10, 5, 60) X(skeleton_width, 10, 5, 60) \
    X(box_custom, 0, 0, 1) X(skeleton_custom, 0, 0, 1) X(head_custom, 0, 0, 1) \
    X(box_alpha, 100, 5, 100) X(skeleton_alpha, 100, 5, 100) X(head_alpha, 100, 5, 100) \
    X(head_style, 0, 0, 2) X(head_size, 5, 2, 12) \
    X(label_side, 0, 0, 3) X(label_order, 0, 0, 2) X(label_gap, 2, 0, 12) \
    X(name_side, 0, 0, 4) X(role_side, 0, 0, 4) X(distance_side, 0, 0, 4) X(weapon_side, 0, 0, 4) \
    X(declutter, 1, 0, 1) X(declutter_hide, 0, 0, 1) X(label_limit, 24, 1, 100) \
    X(lod_on, 1, 0, 1) X(lod_marker_only, 0, 0, 1) X(lod_detail, 80, 5, 1000) X(lod_dot, 200, 10, 2000) \
    X(fade_ms, 120, 0, 600) X(cham_style, 0, 0, 2) X(cham_shader, 0, 0, 11) X(cham_material, 0, 0, 5) X(cham_opacity, 40, 1, 100) \
    X(skybox_on, 0, 0, 1) X(skybox_shader, 0, 0, 5) X(skybox_opacity, 65, 5, 100) \
    X(cham_model, 0, 0, 5) X(cham_anim, 0, 0, 6) \
    X(cham_parts, 7, 1, 7) X(cham_visibility, 0, 0, 1) X(cham_line, 1, 1, 4) \
    X(cham_color_mode, 0, 0, 2) X(vehicle_cham_opacity, 32, 1, 100) \
    X(loot_color_mode, 0, 0, 2) X(item_icon_name, 0, 0, 1) X(vehicle_icons, 1, 0, 1) \
    X(radar_shape, 0, 0, 1) X(radar_north, 0, 0, 1) X(radar_background, 0, 0, 100) \
    X(radar_grid, 0, 0, 1) X(radar_icons, 1, 0, 1) \
    X(radar_self_arrow, 0, 0, 2) X(radar_view_cone, 0, 0, 1) \
    X(offscreen_on, 0, 0, 1) X(offscreen_count, 16, 1, 64) X(offscreen_size, 10, 5, 24) \
    X(layout_auto, 1, 0, 1) X(layout_bank, 0, 0, 3) \
    X(status_panel, 1, 0, 1) X(target_panel, 0, 0, 1) X(preview_on, 1, 0, 1) \
    X(aa_priority, 0, 0, 3) X(aa_lock, 0, 0, 1) X(aa_switch_delay, 180, 0, 2000) \
    X(aa_next_key, 0x58, 1, 254) X(aa_skip_downed, 1, 0, 1) \
    X(weapon_profiles, 0, 0, 1) X(weapon_override, 0, 0, 5) \
    X(ballistic_on, 0, 0, 1) X(bullet_speed, 0, 0, 3000) X(bullet_gravity, 981, 0, 3000) \
    X(trigger_on, 0, 0, 1) X(trigger_key, 0x06, 1, 254) X(trigger_radius, 6, 1, 30) \
    X(trigger_delay, 120, 0, 1000) X(trigger_interval, 150, 50, 2000) \
    X(trigger_distance, 100, 1, 1000) X(trigger_los, 1, 0, 1) X(trigger_ads, 1, 0, 1) \
    X(recoil_on, 0, 0, 1) X(recoil_x, 0, -500, 500) X(recoil_y, 150, -500, 500) \
    X(recoil_delay, 0, 0, 1000) X(recoil_strength, 100, 10, 150) \
    X(loot_guide, 0, 0, 1) X(loot_category, 0, 0, 4) X(loot_min_value, 0, 0, 1000000) \
    X(loot_priority, 0, 0, 1) \
    X(item_icons, 1, 0, 1) X(item_icon_size, 40, 16, 96) X(item_icon_distance, 1, 0, 1) \
    X(menu_key, 0x2D, 1, 254) X(menu_theme, 0, 0, 4) \
    X(menu_particles, 1, 0, 1) X(watermark_on, 1, 0, 1) X(streamproof, 0, 0, 1) \
    X(silent_aim_on, 0, 0, 1) X(silent_aim_fov, 140, 20, 600) X(silent_show_fov, 1, 0, 1) \
    X(silent_key, 0x06, 1, 254) X(silent_key_mode, 0, 0, 1) X(silent_bone, 0, 0, 3) \
    X(silent_hitchance, 100, 1, 100) X(silent_dist, 300, 0, 1000) \
    X(silent_los_on, 0, 0, 1) X(silent_skip_downed, 1, 0, 1) \
    X(target_tracer, 0, 0, 1) \
    X(offscreen_fov_ring, 1, 0, 1) X(offscreen_radius, 0, 0, 350) X(offscreen_color_mode, 0, 0, 2) \
    X(aa_sticky, 0, 0, 1) X(dead_check, 1, 0, 1) \
    BR_LABEL_SETTINGS(X, role) BR_LABEL_SETTINGS(X, name) \
    BR_LABEL_SETTINGS(X, distance) BR_LABEL_SETTINGS(X, weapon) \
    BR_LABEL_SETTINGS(X, health) BR_LABEL_SETTINGS(X, armor) BR_LABEL_SETTINGS(X, crouch) \
    BR_LABEL_SETTINGS(X, hiding) BR_LABEL_SETTINGS(X, vehicle) BR_LABEL_SETTINGS(X, down) \
    BR_WEAPON_SETTINGS(X, 1) BR_WEAPON_SETTINGS(X, 2) BR_WEAPON_SETTINGS(X, 3) \
    BR_WEAPON_SETTINGS(X, 4) BR_WEAPON_SETTINGS(X, 5) \
    BR_HUD_SETTINGS(X, 0) BR_HUD_SETTINGS(X, 1) BR_HUD_SETTINGS(X, 2) BR_HUD_SETTINGS(X, 3)

                                                                
                                                          
#define BR_SETTINGS(X) \
    X(enabled, 1, 1, 1) \
    X(show_box, 1, 0, 1) \
    X(show_skeleton, 1, 0, 1) \
    X(show_name, 1, 0, 1) \
    X(show_armor, 1, 0, 1) \
    X(show_distance, 1, 0, 1) \
    X(show_weapon, 1, 0, 1) \
    X(show_head, 1, 0, 1) \
    X(show_self, 0, 0, 1) \
    X(show_clan, 1, 0, 1) \
    X(show_police, 1, 0, 1) \
    X(cham_on, 0, 0, 1) \
    X(max_distance, 30000, 0, 200000) \
    X(skel_distance, 15000, 0, 200000) \
    X(line_w, 1, 1, 4) \
    X(radar_on, 1, 0, 1) \
    X(radar_range, 150, 25, 500) \
    X(aa_on, 0, 0, 1) \
    X(aa_show_fov, 1, 0, 1) \
    X(aa_fov_px, 150, 30, 600) \
    X(aa_key, 0x12, 1, 254) \
    X(aa_smooth, 5, 1, 20) \
    X(aa_aim_cal, 100, 10, 2000) \
    X(aa_dist, 0, 0, 1000) \
    X(aa_target_clan, 0, 0, 1) \
    X(aa_target_police, 0, 0, 1) \
    X(aa_bone, 0, 0, 9) \
    X(aa_los_on, 0, 0, 1) \
    X(aa_closest_first, 0, 0, 1) \
    X(show_loot, 0, 0, 1) \
    X(show_loot_nodes, 0, 0, 1) \
    X(loot_tier_none, 1, 0, 1) \
    X(loot_tier_common, 1, 0, 1) \
    X(loot_tier_uncommon, 1, 0, 1) \
    X(loot_tier_rare, 1, 0, 1) \
    X(loot_tier_epic, 1, 0, 1) \
    X(loot_tier_legendary, 1, 0, 1) \
    X(loot_tier_exotic, 1, 0, 1) \
    X(vehicle_chams, 0, 0, 1) \
    X(show_containers, 0, 0, 1) \
    X(show_doors, 0, 0, 1) \
    X(show_dropped_loot, 0, 0, 1) \
    X(show_vehicle_loot, 0, 0, 1) \
    X(radar_world, 0, 0, 1) \
    X(show_crouch, 0, 0, 1) \
    X(show_health, 0, 0, 1) \
    X(show_vehicle_markers, 0, 0, 1) \
    X(show_vehicle_occupants, 0, 0, 1) \
    BR_EXTRA_SETTINGS(X)

typedef struct BR_Config {
    unsigned int magic, version;
#define BR_DECLARE(field, initial, minimum, maximum) int field;
    BR_SETTINGS(BR_DECLARE)
#undef BR_DECLARE
    struct { unsigned char rgb[3]; } color[BR_COLOR_COUNT];
    char loot_filter[64];
    char loot_values[512];                                                       

    int menu_open;
    int telem_targets;
    int telem_game_pid;
    int telem_cham_n;
    int telem_los_state;                                      
    int profile_slot, telem_layout, telem_weapon, telem_target;
    int telem_view_w, telem_view_h;
    int telem_render_hz, telem_read_hz, telem_sample_ms, telem_draw_ms, telem_age_ms;
    int telem_friend_count,telem_friends_saved;
    int telem_always_head,telem_always_head_shots;
    int telem_camera_mod,telem_camera_fov,telem_camera_aspect,telem_zoom,telem_freelook,telem_freelook_ready;
    int telem_camera;

#ifdef RR_PORT_DIAGNOSTICS
    int show_vehicles;
#endif
} BR_Config;

                                                                             
// Handle loot rarity visible.
static int br_loot_rarity_visible(const BR_Config* config, int rarity) {
    switch (rarity) {
        case 1: return config->loot_tier_common;
        case 2: return config->loot_tier_uncommon;
        case 3: return config->loot_tier_rare;
        case 4: return config->loot_tier_epic;
        case 5: return config->loot_tier_legendary;
        case 6: return config->loot_tier_exotic;
        default: return config->loot_tier_none;
    }
}

BR_Config* br_get(void);
int br_config_set(BR_Config* config, const char* key, int value);
int br_config_save(void);
int br_config_load(void);
int br_profile_save(int slot);
int br_profile_load(int slot);
int br_config_save_named(const wchar_t* name);
int br_config_load_named(const wchar_t* name);
void br_visual_preset(int preset);
int br_config_text(BR_Config* config, const char* key, const char* value);

int br_start_menu(void);
void br_stop_menu(void);
void br_set_menu_visible(int visible);
void br_request_toggle(void);
void* br_menu_hwnd(void);

#ifdef __cplusplus
}
#endif
#endif
