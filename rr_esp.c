                                                                    
  
                                                                         
                                                                          
                                                                            
                                                               
  
                                                
                                                                             
                                                  
   
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <dbghelp.h>
#include <mmsystem.h>
#include <tlhelp32.h>                                                       
#include <stdio.h>
#include <stddef.h>
#include <stdarg.h>
#include <string.h>
#include <math.h>
#include <stdlib.h>
#include <wchar.h>

#include "menu_bridge.h"
#include "gpu.h"
#include "rr_mem.h"
#include "rr_spawn.h"                                                                      

                                                                         
                                                                     
   
static volatile LONG g_rescan_now = 0;
#include "rr_unity.h"
#include "rr_chain.h"
#include "rr_game.h"
#include "rr_draw.h"
#include "rr_aim.h"
#include "rr_features.h"
#include "rr_camera.h"
#include "rr_friends.h"
#include "rr_shot.h"
#include "rr_visibility.h"
#include "item_icon_map.h"
#include "rr_los.h"
#include "rr_find.h"
#include "invoke_offsets.h"

#pragma comment(lib, "winmm.lib")

#ifndef MOD_NOREPEAT
#define MOD_NOREPEAT 0x4000
#endif

#define GAME_EXE      "ROB RUN.exe"

static HWND  g_hwnd   = NULL;
static int   g_sw = 0, g_sh = 0;
static volatile LONG g_quit = 0;

               

static CRITICAL_SECTION g_cs;
static RR_PLAYER g_buf[3][RR_MAX_ENTITIES];
static RR_CAMERA g_cam[3];
static int       g_nbuf[3];
static int g_buffer_pins[3];
static DWORD g_buffer_sample_at[3];
static HANDLE g_game_frame_event,g_scene_event;
static unsigned g_painted_seq;
static uint64_t g_painted_camera_model;
static uint32_t g_painted_camera_frame;
static DWORD g_camera_frame_at;
static HANDLE g_camera_thread;
static int       g_front = 0;
static unsigned  g_snapshot_seq;
static DWORD     g_snapshot_at;
static volatile LONG g_repaint_pending;
static volatile LONG g_present_retry;
static int       g_nrender = 0;
static RR_PLAYER g_render[RR_MAX_ENTITIES];
static RR_CAMERA g_rcam;
static SRWLOCK g_camera_lock=SRWLOCK_INIT;
static RR_CAMERA g_fast_camera;
static DWORD g_fast_camera_at;

static volatile LONG g_game_ok = 0;
static int g_show_footer = 1;                                                              

                                                                       
                                                                         
                                                                             
                                        
   
static int g_hk_menu = 0, g_hk_profile = 0, g_hk_quit = 0;
static HHOOK g_kb_hook = NULL;
static DWORD g_hk_last[4] = { 0, 0, 0, 0 };
                                                                             
                                                                                
static volatile LONG g_hk_down[4] = { 0, 0, 0, 0 };
                                                                          
static volatile LONG g_hk_armed[4] = { 1, 1, 1, 1 };
static long g_cashfind_bad;

                                                                             
                                                                              
                                                  
static CRITICAL_SECTION g_hk_log_cs;
static volatile LONG g_hk_log_ready;

// Handle hklogf.
static void hklogf(const char* fmt, ...) {
    char path[MAX_PATH];
    GetModuleFileNameA(NULL, path, MAX_PATH);
    char* slash = strrchr(path, '\\');
    if (slash) *(slash + 1) = 0; else path[0] = 0;
    strncat(path, "hotkeys.log", MAX_PATH - strlen(path) - 1);
    if (!g_hk_log_ready) {
        if (InterlockedCompareExchange(&g_hk_log_ready, 1, 0) == 0)
            InitializeCriticalSection(&g_hk_log_cs);
    }
    if (g_hk_log_ready) EnterCriticalSection(&g_hk_log_cs);
    FILE* f = fopen(path, "a");
    if (f) {
        fprintf(f, "%lu ", (unsigned long)GetTickCount());
        va_list ap;
        va_start(ap, fmt);
        vfprintf(f, fmt, ap);
        va_end(ap);
        fputc('\n', f);
        fclose(f);
    }
    if (g_hk_log_ready) LeaveCriticalSection(&g_hk_log_cs);
}


static DWORD g_menu_open_at;

// Handle hk action.
static void hk_action(int id) {
    BR_Config* b = br_get();
    DWORD now = GetTickCount();
    if (id == 1) {
                                                                               
                                                                             
                                                                              
                                                  
        int mk = b->menu_key > 0 ? b->menu_key : VK_INSERT;
        if (b->menu_open && !(GetAsyncKeyState(mk) & 0x8000)) {
            hklogf("Insert close ignored (key up)");
            return;
        }
        if (b->menu_open && (DWORD)(now - g_menu_open_at) < 600) {
            hklogf("Insert close ignored (just opened)");
            return;
        }
    }
    if (id == 1) {
        br_request_toggle();
        if (b->menu_open) g_menu_open_at = now;
        hklogf("Insert menu=%s", b->menu_open ? "open" : "closed");
        if (b->menu_open) g_show_footer = 1;
    } else if(id==2) {
        int slot=b->profile_slot;
        for(int attempt=0;attempt<5;++attempt){slot=slot%5+1;if(br_profile_load(slot))break;}
    } else if (id == 3) {
        hklogf("END quit");
        InterlockedExchange(&g_quit, 1);
        if (g_hwnd) PostMessageA(g_hwnd, WM_CLOSE, 0, 0);
    }
}

// Handle vk to id.
static int vk_to_id(int vk) {
    int mk = br_get()->menu_key > 0 ? br_get()->menu_key : VK_INSERT;
    if (vk == mk) return 1;
    if (vk == VK_F6) return 2;
    if (vk == VK_END) return 3;
    return 0;
}

// Handle hk vk.
static int hk_vk(int id) {
    int mk = br_get()->menu_key > 0 ? br_get()->menu_key : VK_INSERT;
    return id == 1 ? mk : id == 2 ? VK_F6 : id == 3 ? VK_END : 0;
}

// Handle hk release.
static void hk_release(int id) {
    if (id < 1 || id > 3) return;
                                                                           
                                                                          
                                                                          
    if (GetAsyncKeyState(hk_vk(id)) & 0x8000) return;
    InterlockedExchange(&g_hk_down[id], 0);
    InterlockedExchange(&g_hk_armed[id], 1);
}

// Handle hk deliver.
static void hk_deliver(int id, const char* src) {
    DWORD now, prev, age;
    if (id < 1 || id > 3) return;
    int vk = hk_vk(id);
    int down = (GetAsyncKeyState(vk) & 0x8000) != 0;
    now = GetTickCount();

                                                                            
                                                                              
                                                               
    if (!down) {
        InterlockedExchange(&g_hk_armed[id], 1);
        InterlockedExchange(&g_hk_down[id], 0);
        return;
    }
    if (InterlockedCompareExchange(&g_hk_armed[id], 1, 1) == 0) {
        hklogf("key id=%d src=%s held, ignored", id, src);
        return;
    }
    prev = (DWORD)InterlockedExchange((volatile LONG*)&g_hk_last[id], (LONG)now);
    age = now - prev;
    if (age < 300) {
        hklogf("key id=%d src=%s debounced (age=%lu)", id, src, (unsigned long)age);
        return;
    }
    if (InterlockedCompareExchange(&g_hk_armed[id], 0, 1) != 1) {
        hklogf("key id=%d src=%s held, ignored", id, src);
        return;
    }
    InterlockedExchange(&g_hk_down[id], 1);
    hklogf("key id=%d src=%s act (age=%lu)", id, src, (unsigned long)age);
    hk_action(id);
}

                                                                            
                                                              

// Handle kb hook proc.
static LRESULT CALLBACK kb_hook_proc(int code, WPARAM w, LPARAM l) {
    if (code == HC_ACTION) {
        const KBDLLHOOKSTRUCT* k = (const KBDLLHOOKSTRUCT*)l;
        if (w == WM_KEYDOWN || w == WM_SYSKEYDOWN) {
            int id = vk_to_id((int)k->vkCode);
            if (id) PostMessageA(g_hwnd, WM_APP + 2, (WPARAM)id, 0);
        } else if (w == WM_KEYUP || w == WM_SYSKEYUP) {
                                                                         
                                                                          
                                                                            
            int id = vk_to_id((int)k->vkCode);
            if (id) PostMessageA(g_hwnd, WM_APP + 3, (WPARAM)id, 0);
        }
    }
    return CallNextHookEx(NULL, code, w, l);
}

// Handle game focused.
static int game_focused(void) {
    DWORD pid = 0;
    HWND fg = GetForegroundWindow();
    if (fg) GetWindowThreadProcessId(fg, &pid);
    return g_rr_pid && pid == g_rr_pid;
}

typedef struct { DWORD pid; HWND window; long area; } GAME_WINDOW_SEARCH;
// Find game window.
static BOOL CALLBACK find_game_window(HWND window, LPARAM param) {
    GAME_WINDOW_SEARCH* s = (GAME_WINDOW_SEARCH*)param;
    DWORD pid = 0; RECT rc;
    GetWindowThreadProcessId(window, &pid);
    if (pid != s->pid || !IsWindowVisible(window) || IsIconic(window) ||
        GetWindow(window, GW_OWNER) || !GetClientRect(window, &rc)) return TRUE;
    long width = rc.right - rc.left, height = rc.bottom - rc.top;
    if (width > 100 && height > 100 && width * height > s->area) {
        s->area = width * height; s->window = window;
    }
    return TRUE;
}

// Handle sync game window.
static void sync_game_window(void) {
    if (!g_rr_pid || !g_hwnd) return;
    GAME_WINDOW_SEARCH s = { g_rr_pid, NULL, 0 };
    EnumWindows(find_game_window, (LPARAM)&s);
    if (!s.window) return;
    RECT client, overlay; POINT origin = {0, 0};
    if (!GetClientRect(s.window, &client) || !ClientToScreen(s.window, &origin) ||
        !GetWindowRect(g_hwnd, &overlay)) return;
    int width = client.right - client.left, height = client.bottom - client.top;
    if (overlay.left != origin.x || overlay.top != origin.y ||
        overlay.right - overlay.left != width || overlay.bottom - overlay.top != height) {
        SetWindowPos(g_hwnd, HWND_TOPMOST, origin.x, origin.y, width, height,
                     SWP_NOACTIVATE | SWP_NOOWNERZORDER);
        InvalidateRect(g_hwnd, NULL, FALSE);
    }
}

                                                                              
                                                                
// Handle raw input on.
static int raw_input_on(void) {
    RAWINPUTDEVICE rid;
    rid.usUsagePage = 0x01;                            
    rid.usUsage = 0x06;                                
    rid.dwFlags = RIDEV_INPUTSINK;
    rid.hwndTarget = g_hwnd;
    return RegisterRawInputDevices(&rid, 1, sizeof(rid)) ? 1 : 0;
}
static char g_exe[64] = GAME_EXE;                                               
static int  g_demo = 0;                                                           
static char g_status[192] = "starting";

          

typedef struct {
    const char* key;
    size_t offset;
    int minimum, maximum;
} CFG_INT;

#define CFG_FIELD(field, initial, minimum, maximum) { #field, offsetof(BR_Config, field), minimum, maximum },
static const CFG_INT g_cfg_ints[] = { BR_SETTINGS(CFG_FIELD) };
#undef CFG_FIELD

// Handle config set.
int br_config_set(BR_Config* b, const char* key, int value) {
    for (size_t i = 0; i < sizeof(g_cfg_ints) / sizeof(g_cfg_ints[0]); ++i) {
        const CFG_INT* field = &g_cfg_ints[i];
        if (strcmp(key, field->key) != 0) continue;
        if (value < field->minimum) value = field->minimum;
        if (value > field->maximum) value = field->maximum;
        *(int*)((char*)b + field->offset) = value;
        if(!strcmp(key,"cham_color_mode"))b->cham_visibility=value==2;
        if(!strcmp(key,"cham_visibility"))b->cham_color_mode=value?2:0;
        return 1;
    }
    return 0;
}

// Handle config text.
int br_config_text(BR_Config* b,const char* key,const char* value) {
    char* dest=NULL;size_t size=0;
    if(!strcmp(key,"loot_filter")){dest=b->loot_filter;size=sizeof(b->loot_filter);}
    if(!strcmp(key,"loot_values")){dest=b->loot_values;size=sizeof(b->loot_values);}
    if(!dest || strlen(value)>=size || strpbrk(value,"\r\n"))return 0;
    memcpy(dest,value,strlen(value)+1);return 1;
}

// Handle cfg path.
static int cfg_path(wchar_t* path, size_t capacity) {
    DWORD length = GetModuleFileNameW(NULL, path, (DWORD)capacity);
    if (!length || length >= capacity) return 0;
    wchar_t* slash = wcsrchr(path, L'\\');
    if (!slash) return 0;
    size_t prefix = (size_t)(slash + 1 - path);
    return wcscpy_s(path + prefix, capacity - prefix, L"larpd1ck.cfg") == 0;
}

// Handle cfg save at.
static int cfg_save_at(const wchar_t* path, const BR_Config* b) {
    wchar_t temp[MAX_PATH + 20];
    if (swprintf_s(temp, _countof(temp), L"%ls.%lu.tmp", path, GetCurrentProcessId()) < 0) return 0;
    FILE* f = _wfopen(temp, L"w");
    if (!f) return 0;
    fprintf(f, "# larpd1ck settings\n");
    fprintf(f,"loot_filter=%s\nloot_values=%s\n",b->loot_filter,b->loot_values);
    for (size_t i = 0; i < sizeof(g_cfg_ints) / sizeof(g_cfg_ints[0]); ++i) {
        const CFG_INT* field = &g_cfg_ints[i];
        int value = *(const int*)((const char*)b + field->offset);
        fprintf(f, "%s=%d\n", field->key, value);
    }
    for (int i = 0; i < BR_COLOR_COUNT; ++i)
        fprintf(f, "color%d=%u,%u,%u,0\n", i, (unsigned)b->color[i].rgb[0],
                (unsigned)b->color[i].rgb[1], (unsigned)b->color[i].rgb[2]);
    int ok = !ferror(f);
    if (fclose(f) != 0) ok = 0;
    if (ok && MoveFileExW(temp, path, MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) return 1;
    DeleteFileW(temp);
    return 0;
}

// Handle cfg load at.
static int cfg_load_at(const wchar_t* path, BR_Config* b) {
    FILE* f = _wfopen(path, L"r");
    if (!f) return 0;
    BR_Config loaded = *b;
    int legacy_visible = -1, legacy_head = -1;
    int have_los = 0, have_bone = 0;
    char line[1024];
    while (fgets(line, sizeof(line), f)) {
        char* nl = strpbrk(line, "\r\n");
        if (nl) *nl = 0;
        if (!line[0] || line[0] == '#') continue;
        char* value = strchr(line, '=');
        if (!value) continue;
        *value++ = 0;
        if(br_config_text(&loaded,line,value))continue;
        if (!strncmp(line, "color", 5)) {
            char* end;
            long index = strtol(line + 5, &end, 10);
            int red, green, blue;
            if (end != line + 5 && !*end && index >= 0 && index < BR_COLOR_COUNT &&
                sscanf(value, "%d,%d,%d", &red, &green, &blue) == 3 &&
                (unsigned)red <= 255 && (unsigned)green <= 255 && (unsigned)blue <= 255) {
                loaded.color[index].rgb[0] = (unsigned char)red;
                loaded.color[index].rgb[1] = (unsigned char)green;
                loaded.color[index].rgb[2] = (unsigned char)blue;
            }
            continue;
        }
        char* end;
        long number = strtol(value, &end, 10);
        if (end == value || *end) continue;
        if (!strcmp(line, "aa_visible") && (number == 0 || number == 1))
            legacy_visible = (int)number;
        else if (!strcmp(line, "aa_head") && (number == 0 || number == 1))
            legacy_head = (int)number;
        else if (br_config_set(&loaded, line, (int)number)) {
            if (!strcmp(line, "aa_los_on")) have_los = 1;
            if (!strcmp(line, "aa_bone")) have_bone = 1;
        }
    }
    int ok = !ferror(f);
    fclose(f);
    if (ok) {
        if (!have_los && legacy_visible >= 0) loaded.aa_los_on = legacy_visible;
        if (!have_bone && legacy_head >= 0) loaded.aa_bone = legacy_head ? 0 : 1;
#define BR_APPLY(field, initial, minimum, maximum) b->field = loaded.field;
        BR_SETTINGS(BR_APPLY)
#undef BR_APPLY
        memcpy(b->color, loaded.color, sizeof(b->color));
        memcpy(b->loot_filter,loaded.loot_filter,sizeof(b->loot_filter));
        memcpy(b->loot_values,loaded.loot_values,sizeof(b->loot_values));
    }
    return ok;
}

// Handle config save.
int br_config_save(void) {
    wchar_t path[MAX_PATH];
    return cfg_path(path, MAX_PATH) && cfg_save_at(path, br_get());
}

// Handle config load.
int br_config_load(void) {
    wchar_t path[MAX_PATH];
    return cfg_path(path, MAX_PATH) && cfg_load_at(path, br_get());
}

// Handle profile path.
static int profile_path(wchar_t* path,int slot) {
    if(slot<1 || slot>5 || !cfg_path(path,MAX_PATH))return 0;
    wchar_t* name=wcsrchr(path,L'\\');if(!name)return 0;
    return swprintf_s(name+1,MAX_PATH-(size_t)(name+1-path),L"larpd1ck.profile%d.cfg",slot)>0;
}
// Handle profile save.
int br_profile_save(int slot) {
    wchar_t path[MAX_PATH];
    int ok=profile_path(path,slot)&&cfg_save_at(path,br_get());
    if(ok)br_get()->profile_slot=slot;return ok;
}
// Handle profile load.
int br_profile_load(int slot) {
    wchar_t path[MAX_PATH];
    int ok=profile_path(path,slot)&&cfg_load_at(path,br_get());
    if(ok)br_get()->profile_slot=slot;return ok;
}
// Build named config path under configs\<name>.cfg next to exe.
static int named_cfg_path(wchar_t* path, size_t cap, const wchar_t* name, int create_dir) {
    if (!name || !name[0] || !cfg_path(path, cap)) return 0;
    wchar_t* slash = wcsrchr(path, L'\\');
    if (!slash) return 0;
    size_t prefix = (size_t)(slash + 1 - path);
    if (swprintf_s(path + prefix, cap - prefix, L"configs") < 0) return 0;
    if (create_dir) CreateDirectoryW(path, NULL);
    return swprintf_s(path + prefix, cap - prefix, L"configs\\%ls.cfg", name) > 0;
}
int br_config_save_named(const wchar_t* name) {
    wchar_t path[MAX_PATH];
    return named_cfg_path(path, MAX_PATH, name, 1) && cfg_save_at(path, br_get());
}
int br_config_load_named(const wchar_t* name) {
    wchar_t path[MAX_PATH];
    return named_cfg_path(path, MAX_PATH, name, 0) && cfg_load_at(path, br_get());
}
// Handle visual preset.
void br_visual_preset(int preset) {
    BR_Config* b=br_get();
    b->show_box=1;b->show_name=1;b->show_distance=1;b->text_outline=1;b->declutter=1;
    b->show_skeleton=preset==1;b->show_weapon=preset==1;b->show_head=preset==1;
    b->box_style=preset==1?1:0;b->text_size=preset==2?14:12;
    b->esp_opacity=preset==2?75:100;b->text_background=preset==2?65:25;
    b->lod_on=preset!=1;b->fade_ms=120;
}

                 

// Handle aa move mouse.
static void aa_move_mouse(int dx, int dy) {
    if (!dx && !dy) return;
    INPUT in;
    memset(&in, 0, sizeof(in));
    in.type = INPUT_MOUSE;
    in.mi.dx = dx;
    in.mi.dy = dy;
    in.mi.dwFlags = MOUSEEVENTF_MOVE;
    SendInput(1, &in, sizeof(in));
}

// Handle aa clear path.
static int aa_clear_path(RR_V3 start, RR_V3 end) {
    return rr_los_check(start,end)==RR_LOS_CLEAR;
}

// Handle extra input tick.
static void extra_input_tick(unsigned snapshot_at) {
    static RR_TRIGGER_CLOCK trigger;
    static RR_RECOIL_CLOCK recoil;
    static DWORD last_tick;
    static float carry_x,carry_y;
    static DWORD rcs_last_ms;
    static float rcs_prev_pitch;
    static int rcs_was_firing,rcs_has_prev;
    static int tb_holding;
    static DWORD tb_release_at;
    DWORD now=timeGetTime();
    if(tb_holding && now>=tb_release_at){
        INPUT up={0};up.type=INPUT_MOUSE;up.mi.dwFlags=MOUSEEVENTF_LEFTUP;
        SendInput(1,&up,sizeof(INPUT));
        tb_holding=0;
    }
    BR_Config* shared=br_get();
    if(now-last_tick<(shared->recoil_on?2u:8u))return;
    last_tick=now;
    const RR_PLAYER* local=NULL;
    for(int i=0;i<g_nrender;++i)if(g_render[i].valid && g_render[i].local==RR_LOCAL_CONFIRMED){local=&g_render[i];break;}
    BR_Config config;rr_effective_config(shared,local?rr_weapon_class(local->weapon):0,&config);
    int firing=(GetAsyncKeyState(VK_LBUTTON)&0x8000)!=0;
    int rcs_ok=!config.menu_open && !config.telem_freelook && game_focused() &&
               (!local || (!local->dead && !local->inveh));
    if(config.recoil_on && rcs_ok && firing){
        float calib=(config.aa_aim_cal>0?(float)config.aa_aim_cal/100.0f:1.0f);
        if(!rcs_was_firing){
            rcs_was_firing=1;
            rcs_last_ms=now;
            carry_x=carry_y=0;
            rcs_has_prev=0;
        }else{
            unsigned dt=(unsigned)(now-rcs_last_ms);
            if(dt>50u)dt=50u;
            rcs_last_ms=now;
            if(dt>0u){
                carry_x+=(float)config.recoil_x*2.5f*calib*(float)dt/1000.0f;
                carry_y+=(float)config.recoil_y*2.5f*calib*(float)dt/1000.0f;
            }
        }
        if(g_rcam.valid){
            float fy=g_rcam.fwd.y;if(fy>1.0f)fy=1.0f;if(fy<-1.0f)fy=-1.0f;
            float pitch=asinf(fy);
            if(rcs_has_prev){
                float dp=pitch-rcs_prev_pitch;
                if(dp>0.0012f && dp<0.25f)carry_y+=dp*935.3074f*calib;
            }
            rcs_prev_pitch=pitch;rcs_has_prev=1;
        }else{
            rcs_has_prev=0;
        }
        int mx=(int)roundf(carry_x),my=(int)roundf(carry_y);
        carry_x-=(float)mx;carry_y-=(float)my;
        if(mx || my)aa_move_mouse(mx,my);
    }else{
        rcs_was_firing=0;
        rcs_has_prev=0;
        carry_x=carry_y=0;
    }
    int usable=!config.menu_open && !config.telem_freelook && game_focused() && g_rcam.valid && now-snapshot_at<=200 &&
               local && !local->dead && !local->inveh && local->weapon[0];
    if(!usable){
        if(tb_holding){INPUT up={0};up.type=INPUT_MOUSE;up.mi.dwFlags=MOUSEEVENTF_LEFTUP;SendInput(1,&up,sizeof(INPUT));tb_holding=0;}
        trigger.active=0;memset(&recoil,0,sizeof(recoil));
        return;
    }
    RECT rc;GetClientRect(g_hwnd,&rc);int w=rc.right,h=rc.bottom;
    int held=config.trigger_on && (GetAsyncKeyState(config.trigger_key)&0x8000) && (!firing || tb_holding) &&
             (!config.trigger_ads || (GetAsyncKeyState(VK_RBUTTON)&0x8000));
    if(!held || tb_holding){if(!held)trigger.active=0;return;}
    config.aa_fov_px=config.trigger_radius;config.aa_dist=config.trigger_distance;
    config.aa_priority=0;config.aa_closest_first=0;config.aa_los_on=config.trigger_los;
    config.aa_bone=RR_AIM_CLOSEST;
    RR_AIM_PICK pick=rr_aim_pick(g_render,g_nrender,&g_rcam,w,h,&config,0,0,0,
                                config.trigger_los?aa_clear_path:NULL);
    int jitter_interval = config.trigger_interval + (rand() % 17);
    if(rr_trigger_due(&trigger,now,pick.player?pick.player->addr:0,pick.player?pick.player->id:0,
                      pick.player!=NULL,config.trigger_delay,jitter_interval)){
        INPUT down={0};down.type=INPUT_MOUSE;down.mi.dwFlags=MOUSEEVENTF_LEFTDOWN;
        SendInput(1,&down,sizeof(INPUT));
        tb_holding=1;
        tb_release_at=now + 28u + (unsigned)(rand() % 34);
    }
}

// Handle aa tick.
static void aa_tick(unsigned snapshot_seq, DWORD snapshot_at) {
    static unsigned last_snapshot;
    static DWORD last_update;
    static uint64_t locked_addr;
    static int32_t locked_id;
    static int last_group;
    static RR_V3 last_position;
    static DWORD last_position_at;
    static RR_AIM_STEER steer;
    static DWORD last_diag;
    static DWORD last_good;
    static int next_was,previous_weapon;
    static uint64_t pending_excluded;
    BR_Config* shared = br_get();
    int weapon=0;
    for(int i=0;i<g_nrender;++i)if(g_render[i].valid && g_render[i].local==RR_LOCAL_CONFIRMED){weapon=rr_weapon_class(g_render[i].weapon);break;}
    BR_Config effective;rr_effective_config(shared,weapon,&effective);
    BR_Config* b=&effective;
    shared->telem_weapon=b->weapon_override?b->weapon_override:weapon;
    DWORD now = timeGetTime();
    int held = b->aa_on && (GetAsyncKeyState(b->aa_key) & 0x8000);
    int next_down=(GetAsyncKeyState(b->aa_next_key)&0x8000)!=0;
    int next_pressed=next_down && !next_was;next_was=next_down;
    if(next_pressed && held)pending_excluded=locked_addr;
    uint64_t excluded=pending_excluded;
    if (!held || !b->aa_lock || next_pressed || weapon!=previous_weapon) locked_addr = 0;
    previous_weapon=weapon;

                                                                           
                                                                 
       
    if (b->aa_on && now - last_diag > 500) {
        last_diag = now;
        hklogf("aim: held=%d key=0x%02X menu=%d focused=%d cam=%d n=%d age=%lu keydown=%d",
               held, b->aa_key, b->menu_open, game_focused(),
               g_rcam.valid, g_nrender, (unsigned long)(now - snapshot_at),
               (GetAsyncKeyState(b->aa_key) & 0x8000) ? 1 : 0);
    }

    if (!held || b->menu_open ||
        b->telem_freelook || !game_focused() || !g_rcam.valid || !g_nrender || now - snapshot_at > 200) {
        steer = (RR_AIM_STEER){0};
        last_position_at = 0;
        locked_addr=0;pending_excluded=0;shared->telem_target=0;
        return;
    }
                                                                                
    if(b->ignore_friends && locked_addr){
        for(int i=0;i<g_nrender;++i)if(g_render[i].addr==locked_addr && g_render[i].id==locked_id && rr_friends_contains(g_render[i].public_id)){
            locked_addr=0;locked_id=0;last_position_at=0;last_snapshot=~0u;steer=(RR_AIM_STEER){0};break;
        }
    }
    static float last_fx = 0.0f, last_fy = 0.0f, last_fz = 0.0f;
    DWORD elapsed = now - last_update;
    int new_snap = (snapshot_seq != last_snapshot);
    int cam_moved = (fabsf(g_rcam.fwd.x - last_fx) > 1e-5f ||
                     fabsf(g_rcam.fwd.y - last_fy) > 1e-5f ||
                     fabsf(g_rcam.fwd.z - last_fz) > 1e-5f);
    if (b->aa_smooth <= 1) {
        if ((!new_snap && !cam_moved && elapsed < 4) || elapsed < 1) return;
    } else if (!new_snap || elapsed < 6) {
        return;
    }
    last_snapshot = snapshot_seq;
    last_fx = g_rcam.fwd.x;
    last_fy = g_rcam.fwd.y;
    last_fz = g_rcam.fwd.z;
    last_update = now;
    int group = b->aa_bone;
    RECT rc;
    int vw = g_sw, vh = g_sh;
    if (g_hwnd && GetClientRect(g_hwnd, &rc)) { vw = rc.right; vh = rc.bottom; }
    float cx = vw * 0.5f, cy = vh * 0.5f;
    RR_AIM_PICK pick = rr_aim_pick(g_render, g_nrender, &g_rcam, vw, vh, b,
                                  b->aa_sticky ? locked_addr : 0, b->aa_sticky ? locked_id : 0, excluded,
                                  (b->aa_los_on || b->aa_priority==3) ? aa_clear_path : NULL);
    if(!pick.player && b->aa_sticky && locked_addr && now-last_good>=(DWORD)b->aa_switch_delay){
        locked_addr=0;
        pick=rr_aim_pick(g_render,g_nrender,&g_rcam,vw,vh,b,0,0,excluded,
                         (b->aa_los_on || b->aa_priority==3)?aa_clear_path:NULL);
    }
    pending_excluded=0;
    if (!pick.player) {
        steer = (RR_AIM_STEER){0};
        last_position_at = 0;
        shared->telem_target=0;
        return;
    }
    const RR_PLAYER* p = pick.player;
    last_good=now;shared->telem_target=p->id;
    if (!locked_addr || last_group != group) {
        steer = (RR_AIM_STEER){0};
    } else {
        RR_V3 predicted;
        float sx, sy;
        if (last_position_at && p->addr == locked_addr && !p->inveh &&
            rr_aim_motion_point(pick.point, p->pos, last_position,
                                            snapshot_at - last_position_at, now - snapshot_at,
                                            b->aa_smooth, &predicted) &&
            game_w2s(&g_rcam, &predicted, vw, vh, &sx, &sy)) {
            float dx = sx - cx, dy = sy - cy;
            if (isfinite(dx) && isfinite(dy) &&
                (!b->aa_los_on || aa_clear_path(g_rcam.pos, predicted))) {
                pick.dx = dx;
                pick.dy = dy;
                pick.point=predicted;
            }
        }
    }
    if (new_snap || p->addr != locked_addr) {
        last_position = p->pos;
        last_position_at = snapshot_at;
    }
    locked_addr = p->addr;
    locked_id = p->id;
    last_group = group;
    if(b->ballistic_on && b->bullet_speed>0){
        RR_V3 predicted;float x,y;
        if(rr_ballistic_point(g_rcam.pos,pick.point,p->velocity_valid?p->velocity:(RR_V3){0},
                              (float)b->bullet_speed,(float)b->bullet_gravity/100,&predicted) &&
           game_w2s(&g_rcam,&predicted,vw,vh,&x,&y)) {pick.dx=x-cx;pick.dy=y-cy;pick.point=predicted;}
    }
    rr_target_tracer_set_id(p->addr, p->id, group);
    int mx, my;
    RR_VIEWPORT viewport=game_viewport(&g_rcam,vw,vh);
    rr_aim_step(&steer, pick.dx, pick.dy, g_rcam.fov, (int)viewport.height, b->aa_smooth, b->aa_aim_cal,
                elapsed, (GetAsyncKeyState(VK_LBUTTON) & 0x8000) != 0, &mx, &my);
    if (mx || my)
        hklogf("aim: move mx=%d my=%d dist=%.1f err=(%.1f,%.1f)", mx, my, p->dist,
               pick.dx, pick.dy);
    aa_move_mouse(mx, my);
}

              

           

                                                                              
                                                                            
                                                                         
// Handle demo snapshot.
static int demo_snapshot(RR_PLAYER* out, int max, RR_CAMERA* cam) {
    static const char* names[] = { "PLAYER ONE", "PLAYER TWO", "PLAYER THREE",
                                   "PLAYER FOUR", "PLAYER FIVE", "PLAYER SIX" };
    if (!out || max <= 0) return 0;

    memset(cam, 0, sizeof(*cam));
    cam->pos.x = 0.0f; cam->pos.y = 1.7f; cam->pos.z = 0.0f;
    cam->fwd.x = 0.0f; cam->fwd.y = -0.02f; cam->fwd.z = 1.0f;
    cam->rot.w = 1.0f;
    cam->fov = 70.0f;
    cam->valid = 1;

    static DWORD t0 = 0;
    if (!t0) t0 = GetTickCount();
    float t = (float)(GetTickCount() - t0) * 0.0004f;

    int n = max < 10 ? max : 10;
    for (int i = 0; i < n; i++) {
        RR_PLAYER* p = &out[i];
        memset(p, 0, sizeof(*p));
        float a = t + (float)i * 1.0472f;
        float dist = 9.0f + (float)i * 6.0f;
        p->addr = 0x1000 + (uint64_t)i;
        p->id = 100 + i;
        p->pos.x = sinf(a) * dist * 0.55f;
        p->pos.y = 0.0f;
        p->pos.z = cosf(a) * dist + 6.0f;
        p->head = p->pos;
        p->head.y += 1.8f;
        float dx = p->pos.x - cam->pos.x, dy = p->pos.y - cam->pos.y, dz = p->pos.z - cam->pos.z;
        p->dist = sqrtf(dx * dx + dy * dy + dz * dz);
        p->dead = (i == 4);
        if (i == 6) { p->local = RR_LOCAL_CONFIRMED; p->pos = (RR_V3){0}; }
        if (i == 1 || i == 3) p->relationships |= RR_REL_CLAN;
        if (i == 2 || i == 3) p->relationships |= RR_REL_POLICE;
        p->valid = 1;
        snprintf(p->name, sizeof(p->name), "%s", names[i % 6]);
        snprintf(p->role,sizeof(p->role),"%s",i%3==0?"cop":i%3==1?"robber":"hubber");
        snprintf(p->weapon,sizeof(p->weapon),"dp9_m1");
        if(i>=7){
            static const char* loot_names[]={"pistol_ammo_1","cash_1","afak_1"};
            p->is_loot=1;p->loot_rarity=i-6;p->local=0;p->dead=0;
            p->pos=(RR_V3){(i-8)*3.0f,0,12};p->head=p->pos;
            p->dist=sqrtf(p->pos.x*p->pos.x+144.0f);
            snprintf(p->name,sizeof(p->name),"%s",loot_names[i-7]);
        }
    }
    return n;
}
                                                                          
                                                                           
                                                                           
                                                                          
   
// Handle overlay exit.
static void overlay_exit(int code) {
    InterlockedExchange(&g_quit,1);
    if(g_camera_thread && GetThreadId(g_camera_thread)!=GetCurrentThreadId())WaitForSingleObject(g_camera_thread,5000);
    br_config_save();
    rr_shot_stop();
    rr_friends_stop();
    ExitProcess((UINT)code);
}

                                                                                 
// Handle publish snapshot.
static void publish_snapshot(const RR_PLAYER* players, int count, const RR_CAMERA* camera) {
    if(count<0)count=0;if(count>RR_MAX_ENTITIES)count=RR_MAX_ENTITIES;
    EnterCriticalSection(&g_cs);
    int back=-1;
    for(int i=0;i<3;++i)if(i!=g_front && !g_buffer_pins[i]){back=i;break;}
    LeaveCriticalSection(&g_cs);
    if(back<0)return;
    memcpy(g_buf[back],players,sizeof(RR_PLAYER)*(size_t)count);
    g_nbuf[back]=count;g_cam[back]=*camera;
    DWORD now=timeGetTime(),sample_at=now,max_age=0;
    for(int i=0;i<count;++i)if(players[i].sampled_at && !players[i].is_loot && !players[i].is_vehicle){
        DWORD age=now-players[i].sampled_at;
        if(age>max_age){max_age=age;sample_at=players[i].sampled_at;}
    }
    g_buffer_sample_at[back]=sample_at;
    rr_visibility_submit(players,count,camera);
    EnterCriticalSection(&g_cs);
    g_front=back;++g_snapshot_seq;g_snapshot_at=sample_at;
    LeaveCriticalSection(&g_cs);
    if(g_scene_event)SetEvent(g_scene_event);
}

// Handle record render stats.
static void record_render_stats(void) {
    static DWORD last;
    DWORD now=timeGetTime();
    if(now-last<2000)return;
    last=now;
    BR_Config* perf=br_get();
    FILE* stats=fopen("render_stats.log","a");
    if(stats){fprintf(stats,"%lu fps=%d read_hz=%d sample_ms=%d draw_ms=%d age_ms=%d\n",
        (unsigned long)now,perf->telem_render_hz,perf->telem_read_hz,
        perf->telem_sample_ms,perf->telem_draw_ms,perf->telem_age_ms);fclose(stats);}
}

// Sample game data.
static DWORD WINAPI ReadThread(LPVOID p) {
    (void)p;
    timeBeginPeriod(1);
    int tick = 0;
    int dead_ticks = 0;
    int game_gone = 0;
                                                                            
                                                                                
                                                     
       
    DWORD snap_started = 0, rate_t0 = 0, last_snap_ms = 0;
    int rate_n = 0, last_hz = 0;
                                                                              
                                                                                
    static RR_PLAYER tmp[RR_MAX_ENTITIES];
    for (;;) {
        record_render_stats();
                                                                             
                                                                       
                                         
        if (!g_demo && g_rr_pid == 0) {
            if (rr_drv_open()) rr_attach(GAME_EXE);
            if (g_rr_pid) { game_init(); }
        }
        if (InterlockedCompareExchange(&g_quit, 0, 0)) break;

                                                                          
                                                                              
                                                                
        if (g_demo) {
            RR_CAMERA cam;
            int n = demo_snapshot(tmp, RR_MAX_ENTITIES, &cam);
            publish_snapshot(tmp, n, &cam);
            DWORD finished=timeGetTime();
            if(!rate_t0)rate_t0=finished;
            ++rate_n;
            if(finished-rate_t0>=1000){last_hz=(int)(rate_n*1000u/(finished-rate_t0));rate_n=0;rate_t0=finished;}
            br_get()->telem_read_hz=last_hz;
            InterlockedExchange(&g_game_ok, 1);
            Sleep(16);
            continue;
        }
        RR_CAMERA cam;
        memset(&cam, 0, sizeof(cam));
        int n = 0;
        snap_started = timeGetTime();
        __try {
            n = game_snapshot(tmp, RR_MAX_ENTITIES, &cam);
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            hklogf("exception in game_snapshot");
            n = 0;
        }
        {
            DWORD finished = timeGetTime();
            last_snap_ms = finished - snap_started;
            if (!rate_t0) rate_t0 = finished;
            ++rate_n;
            if (finished - rate_t0 >= 1000) {
                last_hz = (int)((rate_n * 1000u) / (finished - rate_t0));
                rate_n = 0;
                rate_t0 = finished;
            }
        }

                                                                                   
                                                                                   
           
                                                                                 
                                     
           
                                                                          
        if (InterlockedExchange(&g_rescan_now, 0) && !game_discovery_active()) {
            game_init();
            n = game_snapshot(tmp, RR_MAX_ENTITIES, &cam);
        }
                                                                                 
                                                                      
           
        if ((tick % 20) == 0) {
            uint8_t mz[2];
            uint64_t img = rr_image_base();
            if (img && rr_read(img, mz, sizeof(mz))) {
                dead_ticks = 0;
            } else if (++dead_ticks >= 3) {
                dead_ticks = 0;
                                                                                
                                                                             
                                                                              
                hklogf("target process lost - clearing anchors and re-attaching");
                game_reattach_reset();
                if (rr_attach(g_exe)) {
                    hklogf("re-attached to pid %u", g_rr_pid);
                    game_init();
                    n = game_snapshot(tmp, RR_MAX_ENTITIES, &cam);
                } else {
                    g_rr_pid = 0;                                                 
                }
            }
        }
                                                                                  
                                                                                 
           
        if ((tick % 240) == 0 && g_rr_pid) {
            if (rr_process_visible(g_rr_pid) == 0) {
                if (++game_gone >= 2) {
                    hklogf("target pid %u exited - shutting down", (unsigned)g_rr_pid);
                    overlay_exit(0);
                }
            } else {
                game_gone = 0;
            }
        }
        if (!game_discovery_active() && (tick % 40) == 0 &&
            (n == 0 || game_world_addr() == 0)) {
            game_init();
            n = game_snapshot(tmp, RR_MAX_ENTITIES, &cam);
        }

        publish_snapshot(tmp, n, &cam);
        br_get()->telem_read_hz = last_hz;
        br_get()->telem_sample_ms = (int)last_snap_ms;

        InterlockedExchange(&g_game_ok, n > 0);
        if ((tick % 3000) == 0) {
            char dpath[MAX_PATH];
            GetModuleFileNameA(NULL, dpath, MAX_PATH);
            char* dslash = strrchr(dpath, 0x5C);
            if (dslash) *(dslash + 1) = 0;
            strncat(dpath, "status.log", MAX_PATH - strlen(dpath) - 1);
            FILE* df = fopen(dpath, "a");
            if (df) {
                char stx[192];
                game_status(stx, sizeof(stx));
                fprintf(df, "%lu pid=%lu n=%d inst=%d world=%llX snap=%lums hz=%d status=[%s]\n",
                        (unsigned long)GetTickCount(), (unsigned long)g_rr_pid, n, game_instance_mode(),
                        (unsigned long long)game_world_addr(), (unsigned long)last_snap_ms, last_hz, stx);
                fclose(df);
            }
        }
        if (n > 0 && !br_get()->menu_open) g_show_footer = 0;                                
        if ((tick % 250) == 0) {
            char st[192];
            game_status(st, sizeof(st));
            EnterCriticalSection(&g_cs);
            snprintf(g_status, sizeof(g_status), "%s players=%d", st, n);
            LeaveCriticalSection(&g_cs);
            BR_Config* b = br_get();
            b->telem_game_pid = (int)g_rr_pid;
        }
        tick++;

                                                                          
                                                                           
        if(g_game_frame_event && cam.render_state)WaitForSingleObject(g_game_frame_event,16);
        else Sleep(1);
    }
    return 0;
}

// Handle key pressed.
static int KeyPressed(int vk, int* was) {
    int down = (GetAsyncKeyState(vk) & 0x8000) != 0;
    int hit = down && !*was;
    *was = down;
    return hit;
}

// Schedule rendering.
static DWORD WINAPI RenderThread(LPVOID p) {
    (void)p;
    timeBeginPeriod(1);
    int k_ins = 0, k_end = 0;
    int k_profile=0;
    unsigned cached_seq = ~0u;
    for (;;) {
        if (InterlockedCompareExchange(&g_quit, 0, 0)) break;

                                                                             
                                                                     
        int mk = br_get()->menu_key > 0 ? br_get()->menu_key : VK_INSERT;
        if (!(GetAsyncKeyState(mk) & 0x8000)) InterlockedExchange(&g_hk_armed[1], 1);
        if (!(GetAsyncKeyState(VK_F6) & 0x8000)) InterlockedExchange(&g_hk_armed[2], 1);
        if (!(GetAsyncKeyState(VK_END) & 0x8000)) InterlockedExchange(&g_hk_armed[3], 1);

        if (KeyPressed(mk, &k_ins)) hk_deliver(1, "poll");
        if (KeyPressed(VK_END, &k_end)) { hk_deliver(3, "poll"); break; }
        if(KeyPressed(VK_F6,&k_profile))hk_deliver(2,"poll");

                                       
        EnterCriticalSection(&g_cs);
        unsigned snapshot_seq = g_snapshot_seq;
        DWORD snapshot_at = g_snapshot_at;
        if(cached_seq != snapshot_seq) {
            g_nrender = g_nbuf[g_front];
            memcpy(g_render, g_buf[g_front], sizeof(RR_PLAYER) * (size_t)g_nrender);
            g_rcam = g_cam[g_front];
            cached_seq = snapshot_seq;
        }
        g_status[sizeof(g_status) - 1] = 0;
        LeaveCriticalSection(&g_cs);
        AcquireSRWLockShared(&g_camera_lock);
        if(g_rcam.valid && g_fast_camera.valid && g_rcam.model_source==g_fast_camera.model_source &&
            timeGetTime()-g_fast_camera_at<100 && g_fast_camera.sampled_qpc>=g_rcam.sampled_qpc)g_rcam=g_fast_camera;
        ReleaseSRWLockShared(&g_camera_lock);

                                                                             
        RECT friend_view;int friend_w=g_sw,friend_h=g_sh;
        if(g_hwnd && GetClientRect(g_hwnd,&friend_view)){friend_w=friend_view.right;friend_h=friend_view.bottom;}
        rr_friends_key(g_render,g_nrender,&g_rcam,friend_w,friend_h,
            !br_get()->menu_open && game_focused() && timeGetTime()-snapshot_at<=100);
        rr_friends_apply(g_render,g_nrender);
        if (!g_demo) { __try { aa_tick(snapshot_seq, snapshot_at);extra_input_tick(snapshot_at);rr_shot_tick(g_render,g_nrender,&g_rcam,friend_w,friend_h,snapshot_at,game_focused()); } __except (EXCEPTION_EXECUTE_HANDLER) { hklogf("exception in input tick"); } }

                                                                              
                                                                                
                                                                                
        Sleep(g_nrender > 0 ? 1 : 48);
    }
    return 0;
}

             

// Sample the latest camera independently.
static DWORD WINAPI CameraThread(LPVOID unused){
    (void)unused;
    HANDLE timer=CreateWaitableTimerExW(NULL,NULL,0x00000002,TIMER_ALL_ACCESS);
    if(!timer)timer=CreateWaitableTimerW(NULL,FALSE,NULL);
    uint64_t last_model=0;uint32_t last_frame=0;
    while(!InterlockedCompareExchange(&g_quit,0,0)){
        RR_CAMERA camera;
        EnterCriticalSection(&g_cs);camera=g_cam[g_front];LeaveCriticalSection(&g_cs);
        AcquireSRWLockShared(&g_camera_lock);
        if(camera.valid && g_fast_camera.valid && camera.model_source==g_fast_camera.model_source &&
            timeGetTime()-g_fast_camera_at<100 && g_fast_camera.sampled_qpc>=camera.sampled_qpc)camera=g_fast_camera;
        ReleaseSRWLockShared(&g_camera_lock);
        int refreshed=0;
        if(camera.valid && !g_demo){
            __try{refreshed=game_refresh_render_camera(&camera);}
            __except(EXCEPTION_EXECUTE_HANDLER){refreshed=0;}
        }
        __try{rr_camera_tick(&camera,game_focused());}
        __except(EXCEPTION_EXECUTE_HANDLER){br_get()->telem_camera_mod=4;}
        if(refreshed || camera.valid){
            AcquireSRWLockExclusive(&g_camera_lock);
            g_fast_camera=camera;g_fast_camera_at=timeGetTime();
            int changed=refreshed && camera.render_state && (last_model!=camera.model_source || last_frame!=camera.frame_number);
            if(changed)g_camera_frame_at=g_fast_camera_at;
            ReleaseSRWLockExclusive(&g_camera_lock);
            if(changed){
                last_model=camera.model_source;last_frame=camera.frame_number;
                if(g_game_frame_event)SetEvent(g_game_frame_event);
                if(g_scene_event)SetEvent(g_scene_event);
            }
        }
        if(timer){LARGE_INTEGER due;due.QuadPart=camera.valid?-5000:-100000;
            if(SetWaitableTimer(timer,&due,0,NULL,NULL,FALSE))WaitForSingleObject(timer,20);else Sleep(1);
        }else Sleep(camera.valid?1:10);
    }
    rr_camera_shutdown();
    if(timer)CloseHandle(timer);
    return 0;
}

typedef struct {
    float x, y, speed, len, bright;
} TechParticle;

static void render_diagonal_rain(int vw, int vh) {
    static TechParticle particles[80];
    static int inited = 0;
    static ULONGLONG last_qpc = 0;
    if (vw <= 0 || vh <= 0) return;
    if (!inited) {
        inited = 1;
        for (int i = 0; i < 80; ++i) {
            particles[i].x = (float)(rand() % vw);
            particles[i].y = (float)(rand() % vh);
            particles[i].speed = 0.5f + ((rand() % 100) / 40.0f);
            particles[i].len = 8.0f + (float)(rand() % 16);
            particles[i].bright = 0.5f + ((rand() % 50) / 100.0f);
        }
    }
    ULONGLONG now = GetTickCount64();
    float dt = (last_qpc && (now - last_qpc) < 250) ? (float)(now - last_qpc) * 0.001f : 0.016f;
    last_qpc = now;
    if (dt < 0.0f) dt = 0.0f;
    if (dt > 0.1f) dt = 0.1f;

    float t = (float)(now % 6283185ULL) * 0.001f;

    /* Smooth animated dim backdrop (no horizontal bands) */
    float bg_g = 0.5f + 0.5f * sinf(t * 0.7f);
    int br = (int)(8.0f + bg_g * 10.0f);
    int bg = (int)(12.0f - bg_g * 3.0f);
    int bb = (int)(32.0f - bg_g * 8.0f);
    r_fill(0.0f, 0.0f, (float)vw, (float)vh, RARGB(115, br, bg, bb));

    for (int i = 0; i < 80; ++i) {
        TechParticle* p = &particles[i];
        float moveAmount = p->speed * 60.0f * dt;
        p->x += moveAmount;
        p->y += moveAmount;
        if (p->x > vw || p->y > vh) {
            if (rand() % 2 == 0) {
                p->x = (float)(rand() % vw);
                p->y = -10.0f;
            } else {
                p->x = -10.0f;
                p->y = (float)(rand() % vh);
            }
            p->speed = 0.5f + ((rand() % 100) / 40.0f);
            p->bright = 0.5f + ((rand() % 50) / 100.0f);
        }
        if (p->x > -20.0f && p->x < vw + 20.0f && p->y > -20.0f && p->y < vh + 20.0f) {
            float hx = p->x, hy = p->y;
            float tx = p->x - p->len, ty = p->y - p->len;
            /* Animated blend between dark blue and current purple */
            float phase = 0.5f + 0.5f * sinf(t * 2.2f + (float)i * 0.37f + (hx + hy) * 0.004f);
            int tail_r = (int)(14.0f + phase * (140.0f - 14.0f));
            int tail_g = (int)(48.0f + phase * (60.0f - 48.0f));
            int tail_b = (int)(165.0f + phase * (220.0f - 165.0f));
            int head_r = (int)(35.0f + phase * (180.0f - 35.0f));
            int head_g = (int)(95.0f + phase * (100.0f - 95.0f));
            int head_b = (int)(225.0f + phase * (255.0f - 225.0f));

            const int segs = 4;
            for (int s = 0; s < segs; ++s) {
                float u0 = (float)s / (float)segs;
                float u1 = (float)(s + 1) / (float)segs;
                float um = 0.5f * (u0 + u1);
                float sx0 = tx + (hx - tx) * u0, sy0 = ty + (hy - ty) * u0;
                float sx1 = tx + (hx - tx) * u1, sy1 = ty + (hy - ty) * u1;
                int sr = (int)((float)tail_r + ((float)head_r - (float)tail_r) * um);
                int sg = (int)((float)tail_g + ((float)head_g - (float)tail_g) * um);
                int sb = (int)((float)tail_b + ((float)head_b - (float)tail_b) * um);
                int sa = (int)((45.0f + 175.0f * um) * p->bright);
                if (sa > 255) sa = 255;
                r_line_c(sx0, sy0, sx1, sy1, RARGB(sa, sr, sg, sb), 1.6f);
            }
            int ha = (int)(255.0f * p->bright);
            if (ha > 255) ha = 255;
            r_fill_ellipse(hx, hy, 1.7f, 1.7f, RARGB(ha, head_r, head_g, head_b));
        }
    }
}

static void render_watermark(const BR_Config* b) {
    SYSTEMTIME st;
    GetLocalTime(&st);
    char wm[96];
    snprintf(wm, sizeof(wm), "larpd1ck  |  FPS: %d  |  %02d:%02d",
             b->telem_render_hz, (int)st.wHour, (int)st.wMinute);
    gpu_font(12.0f);
    gpu_text_style(0, 0, 0);
    float tw = r_text_width(wm);
    float th = r_text_height();
    float bx = 14.0f, by = 14.0f, bw = tw + 20.0f, bh = th + 12.0f;
    r_fill(bx, by, bw, bh, RARGB(220, 17, 18, 22));
    r_fill(bx, by, bw, 2.0f, RRGB(0, 102, 255));
    r_rect(bx, by, bw, bh, RARGB(220, 37, 39, 48), 1.0f);
    r_text(bx + 10.0f, by + 6.0f, wm, RRGB(236, 238, 242), RT_LEFT);
}

static void radar_drag_tick(BR_Config* b, int vw, int vh, const RR_CAMERA* cam) {
    static int dragging = 0;
    static int start_cx = 0, start_cy = 0, start_rx = 0, start_ry = 0;
    if (!b->radar_on || vw <= 0 || vh <= 0) { dragging = 0; return; }
    int down = (GetAsyncKeyState(VK_LBUTTON) & 0x8000) != 0;
    if (!down) { dragging = 0; return; }
    POINT pt;
    if (!GetCursorPos(&pt)) { dragging = 0; return; }
    HWND menu_hwnd = (HWND)br_menu_hwnd();
    if (!dragging && menu_hwnd && IsWindowVisible(menu_hwnd)) {
        RECT mr;
        if (GetWindowRect(menu_hwnd, &mr) && PtInRect(&mr, pt)) return;
    }
    ScreenToClient(g_hwnd, &pt);
    RR_VIEWPORT view = game_viewport(cam, vw, vh);
    if (view.width <= 0 || view.height <= 0) return;
    int bank = rr_layout_bank(b, vw, vh);
    RR_HUD_RECT r = rr_hud_rect(b, bank, 0, view);
    if (!dragging) {
        if (pt.x >= r.x && pt.x <= r.x + r.w && pt.y >= r.y && pt.y <= r.y + r.h) {
            dragging = 1;
            start_cx = pt.x; start_cy = pt.y;
            if (bank == 0) { start_rx = b->h0_radar_x; start_ry = b->h0_radar_y; }
            else if (bank == 1) { start_rx = b->h1_radar_x; start_ry = b->h1_radar_y; }
            else if (bank == 2) { start_rx = b->h2_radar_x; start_ry = b->h2_radar_y; }
            else { start_rx = b->h3_radar_x; start_ry = b->h3_radar_y; }
        }
    } else {
        int dx = (pt.x - start_cx) * 1920 / view.width;
        int dy = (pt.y - start_cy) * 1080 / view.height;
        char kx[32], ky[32];
        snprintf(kx, sizeof(kx), "h%d_radar_x", bank);
        snprintf(ky, sizeof(ky), "h%d_radar_y", bank);
        br_config_set(b, kx, start_rx + dx);
        br_config_set(b, ky, start_ry + dy);
    }
}

// Handle window messages.
static LRESULT CALLBACK OverlayProc(HWND h, UINT m, WPARAM w, LPARAM l) {
    (void)l;
    switch (m) {
    case WM_TIMER:
        if (w == 18 && !g_demo) sync_game_window();
        return 0;
    case WM_DESTROY:
        PostQuitMessage(0);
        return 0;
    case WM_ERASEBKGND:
        return 1;
    case WM_ACTIVATE:
    case WM_SETFOCUS:
                                                                                
                                
           
        InterlockedExchange(&g_rescan_now, 1);
        return 0;
    case WM_MOUSEACTIVATE:
        return MA_NOACTIVATE;
    case WM_NCHITTEST:
        return HTTRANSPARENT;
    case WM_APP + 1:
        InvalidateRect(h, NULL, FALSE);
                                                                            
                                                                                      
        UpdateWindow(h);
        return 0;
    case WM_HOTKEY:
        hk_deliver((int)w, "hotkey");
        return 0;
    case WM_APP + 2:                                                   
        hk_deliver((int)w, "hook");
        return 0;
    case WM_APP + 3:                                                         
        hk_release((int)w);
        return 0;
    case WM_INPUT: {                                                 
        static BYTE buf[128];
        UINT size = sizeof(buf);
        if (GetRawInputData((HRAWINPUT)l, RID_INPUT, buf, &size, sizeof(RAWINPUTHEADER)) != (UINT)-1) {
            RAWINPUT* ri = (RAWINPUT*)buf;
            if (ri->header.dwType == RIM_TYPEKEYBOARD) {
                const RAWKEYBOARD* kb = &ri->data.keyboard;
                int id = vk_to_id((int)kb->VKey);
                if (id && (kb->Flags & RI_KEY_BREAK)) hk_release(id);
                else if (id) hk_deliver(id, "raw");
            }
        }
        return DefWindowProc(h, m, w, l);
    }
    case WM_PAINT: {
        DWORD draw_started=timeGetTime();
        PAINTSTRUCT ps;
        int pinned=-1;
        HDC hdc = BeginPaint(h, &ps);
        RECT rc;
        GetClientRect(h, &rc);
        int vw = rc.right - rc.left, vh = rc.bottom - rc.top;
        if (vw > 0 && vh > 0) {
            if (!gpu_ready() && !gpu_init(h, vw, vh)) {
                MessageBoxA(NULL, "GPU backend unavailable (D3D11/DComp)", "larpd1ck", 0);
                EndPaint(h, &ps);
                InterlockedExchange(&g_repaint_pending, 0);
                return 0;
            }
            gpu_size(vw, vh);
            if(!gpu_frame_ready()){
                EndPaint(h,&ps);InterlockedExchange(&g_repaint_pending,0);return 0;
            }
            EnterCriticalSection(&g_cs);
            pinned=g_front;++g_buffer_pins[pinned];
            const RR_PLAYER* paint_players=g_buf[pinned];
            int paint_count=g_nbuf[pinned];RR_CAMERA paint_camera=g_cam[pinned];
            DWORD published_at=g_buffer_sample_at[pinned];unsigned source_seq=g_snapshot_seq;
            LeaveCriticalSection(&g_cs);
            AcquireSRWLockShared(&g_camera_lock);
            if(paint_camera.valid && g_fast_camera.valid && paint_camera.model_source==g_fast_camera.model_source &&
                timeGetTime()-g_fast_camera_at<100 && g_fast_camera.sampled_qpc>=paint_camera.sampled_qpc)paint_camera=g_fast_camera;
            ReleaseSRWLockShared(&g_camera_lock);
            __try {
                gpu_begin();
                if (br_get()->skybox_on) {
                    float fx = 0.0f, fy = 0.0f, fz = 1.0f;
                    float rx = 1.0f, ry = 0.0f, rz = 0.0f;
                    float ux = 0.0f, uy = 1.0f, uz = 0.0f;
                    float fov = 60.0f, asp = (float)vw / (float)(vh > 0 ? vh : 1);
                    if (paint_camera.valid) {
                        fx = paint_camera.fwd.x; fy = paint_camera.fwd.y; fz = paint_camera.fwd.z;
                        rx = paint_camera.right.x; ry = paint_camera.right.y; rz = paint_camera.right.z;
                        ux = paint_camera.up.x; uy = paint_camera.up.y; uz = paint_camera.up.z;
                        if (isfinite(paint_camera.fov) && paint_camera.fov > 1.0f && paint_camera.fov < 179.0f)
                            fov = paint_camera.fov;
                        if (isfinite(paint_camera.aspect) && paint_camera.aspect > 0.25f && paint_camera.aspect < 8.0f)
                            asp = paint_camera.aspect;
                    }
                    gpu_draw_skybox(br_get()->skybox_shader, br_get()->skybox_opacity,
                                    fx, fy, fz, rx, ry, rz, ux, uy, uz, fov, asp);
                }
                if (br_get()->menu_open && br_get()->menu_particles) {
                    render_diagonal_rain(vw, vh);
                }
                radar_drag_tick(br_get(), vw, vh, &paint_camera);
                __try { draw_esp(paint_players, paint_count, &paint_camera, vw, vh); }
                __except (EXCEPTION_EXECUTE_HANDLER) { hklogf("exception in draw_esp"); }
                if (br_get()->watermark_on) {
                    render_watermark(br_get());
                }

                                                                              
                if (g_show_footer && br_get()->status_panel) {
                    RR_VIEWPORT view=game_viewport(&paint_camera,vw,vh);
                    RR_HUD_RECT hud=rr_hud_rect(br_get(),rr_layout_bank(br_get(),vw,vh),1,view);
                    gpu_font(11.0f);
                    char line[256];
                    snprintf(line, sizeof(line), "ROB RUN  %s", g_status);
                    r_text(hud.x, hud.y, line, RARGB(170, 170, 170, 170),
                           RT_LEFT | RT_OUTLINE);
                }
                int presented=gpu_present();
                InterlockedExchange(&g_present_retry, !presented);
                static DWORD rate_started;
                static unsigned rendered;
                DWORD finished=timeGetTime();
                BR_Config* perf=br_get();
                perf->telem_camera=paint_camera.render_state;
                perf->telem_draw_ms=(int)(finished-draw_started);
                perf->telem_age_ms=published_at?(int)(finished-published_at):0;
                if(!rate_started)rate_started=finished;
                if(presented){
                    ++rendered;g_painted_seq=source_seq;
                    g_painted_camera_model=paint_camera.model_source;g_painted_camera_frame=paint_camera.frame_number;
                }
                if(finished-rate_started>=1000) {
                    perf->telem_render_hz=(int)(rendered*1000u/(finished-rate_started));
                    rendered=0;rate_started=finished;

                }
            } __except (EXCEPTION_EXECUTE_HANDLER) {
                hklogf("exception in paint");
                InterlockedExchange(&g_present_retry, 1);
            }
        }
        if(pinned>=0){EnterCriticalSection(&g_cs);--g_buffer_pins[pinned];LeaveCriticalSection(&g_cs);}
        EndPaint(h, &ps);
        InterlockedExchange(&g_repaint_pending, 0);
        return 0;
    }
    default:
        break;
    }
    return DefWindowProc(h, m, w, l);
}

                    

                                                                            
                                                                          
                                                                        
static FILE* g_out = NULL;

// Handle out open.
static void out_open(const char* path) {
    if (!AttachConsole(ATTACH_PARENT_PROCESS)) AllocConsole();
    freopen("CONOUT$", "w", stdout);
    g_out = fopen(path, "w");
}

// Handle out close.
static void out_close(void) {
    if (g_out) { fclose(g_out); g_out = NULL; }
    FreeConsole();
}

// Handle out.
static void out(const char* fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    vprintf(fmt, ap);
    va_end(ap);
    if (g_out) {
        va_start(ap, fmt);
        vfprintf(g_out, fmt, ap);
        va_end(ap);
        fflush(g_out);
    }
}

static int g_fail = 0;
// Check.
static void check(int cond, const char* what) {
    out("  [%s] %s\n", cond ? "PASS" : "FAIL", what);
    if (!cond) g_fail++;
}


// Run selftest.
static int run_selftest(void) {
    wchar_t temp_dir[MAX_PATH], config_path[MAX_PATH];
                                                                       
    DWORD temp_len = GetCurrentDirectoryW(_countof(temp_dir), temp_dir);
    int have_temp = temp_len && temp_len < _countof(temp_dir) &&
        GetTempFileNameW(temp_dir, L"rrc", 0, config_path);
    BR_Config written = {0}, loaded = {0};
    written.radar_range = 275;
    written.radar_on = 1;
    written.radar_world = 1;
    written.show_health = written.show_crouch = written.show_vehicle_markers = written.show_vehicle_occupants = 1;
    written.show_containers = written.show_doors = written.show_dropped_loot = written.show_vehicle_loot = 1;
    written.aa_los_on = 1;
    written.aa_closest_first = 1;
    written.show_loot = written.show_loot_nodes = written.vehicle_chams = 1;
    written.show_self = 1;
    written.loot_tier_none = written.loot_tier_uncommon = written.loot_tier_epic = written.loot_tier_exotic = 1;
    written.aa_key = VK_RBUTTON;
    written.aa_bone = RR_AIM_FEET;
    written.color[7].rgb[0] = 117;
    check(have_temp && cfg_save_at(config_path, &written) && cfg_load_at(config_path, &loaded) &&
          loaded.radar_range == 275 && loaded.radar_on == 1 && loaded.radar_world == 1 &&
          loaded.show_containers == 1 && loaded.show_doors == 1 && loaded.show_dropped_loot == 1 && loaded.show_vehicle_loot == 1 &&
          loaded.show_health == 1 && loaded.show_crouch == 1 && loaded.show_vehicle_markers == 1 && loaded.show_vehicle_occupants == 1 && loaded.aa_los_on == 1 &&
          loaded.aa_closest_first == 1 && loaded.aa_key == VK_RBUTTON && loaded.aa_bone == RR_AIM_FEET && loaded.color[7].rgb[0] == 117,
          "config round trip saves radar, aim and colors");
    check(loaded.show_loot && loaded.show_loot_nodes && loaded.vehicle_chams,
          "config round trip saves loot and vehicle chams switches");
    check(loaded.show_self == 1, "config round trip saves self ESP switch");
    check(br_loot_rarity_visible(&loaded, 0) && !br_loot_rarity_visible(&loaded, 1) &&
          br_loot_rarity_visible(&loaded, 2) && !br_loot_rarity_visible(&loaded, 3) &&
          br_loot_rarity_visible(&loaded, 4) && !br_loot_rarity_visible(&loaded, 5) &&
          br_loot_rarity_visible(&loaded, 6) && br_loot_rarity_visible(&loaded, -1),
          "loot tier filters round trip and map each rarity");
    if (have_temp) {
        written.radar_range = 125;
        check(cfg_save_at(config_path, &written) && cfg_load_at(config_path, &loaded) &&
              loaded.radar_range == 125, "config save replaces the previous profile");
        FILE* legacy = _wfopen(config_path, L"w");
        int legacy_written = 0;
        if (legacy) {
            fputs("aa_visible=1\naa_head=0\n", legacy);
            legacy_written = fclose(legacy) == 0;
        }
        loaded.aa_los_on = 0;
        check(legacy_written && cfg_load_at(config_path, &loaded) &&
              loaded.aa_los_on == 1 && loaded.aa_bone == 1 && loaded.radar_range == 125,
              "config imports legacy aim settings and retains unspecified values");
        legacy = _wfopen(config_path, L"a");
        legacy_written = 0;
        if (legacy) {
            fputs("aa_los_on=0\naa_bone=2\naa_visible=1\naa_head=0\n", legacy);
            legacy_written = fclose(legacy) == 0;
        }
        check(legacy_written && cfg_load_at(config_path, &loaded) &&
              loaded.aa_los_on == 0 && loaded.aa_bone == 2,
              "config prefers current keys over legacy aliases");
        DeleteFileW(config_path);
    }

                                                                   
    check(U_OBJ_HEADER == 0x10, "IL2CPP object header is 16 bytes");
    check(U_CLASS_STATIC == 0xB8, "Il2CppClass.static_fields at 0xB8 (metadata 39 layout)");
    check(U_STR_LEN == 0x10 && U_STR_CHARS == 0x14, "Il2CppString length/char layout");
    check(U_DICT_ENTRIES == 0x18 && U_DICT_COUNT == 0x20, "Dictionary _entries/_count layout");

                                                     
    check(RR_OFF_CHAR_TRANSFORM == 0x58, "BattleCharacterModel.Transform from dump (0x58)");
    check(RR_OFF_WEM_BATTLE_CHARS == 0x18, "WorldEntitiesModel._battleCharacters from dump (0x18)");
    check(RR_OFF_WORLD_ENTITIES == 0x38, "WorldModel.WorldEntitiesModel from dump (0x38)");
    check(RR_OFF_CAM_POS == 0x10 && RR_OFF_CAM_ROT == 0x1C, "BattleCameraModel Position/Orientation");

                                                                              
    RR_CAMERA cam;
    memset(&cam, 0, sizeof(cam));
    cam.pos.x = 0; cam.pos.y = 0; cam.pos.z = 0;
    cam.fwd.x = 0; cam.fwd.y = 0; cam.fwd.z = 1;
    cam.rot.w = 1.0f;
    cam.fov = 90.0f;
    cam.valid = 1;

    const int W = 1920, H = 1080;
    float sx = 0, sy = 0;
    RR_V3 p = { 0, 0, 10 };
    check(game_w2s(&cam, &p, W, H, &sx, &sy) == 1 &&
          fabsf(sx - 960.0f) < 0.5f && fabsf(sy - 540.0f) < 0.5f,
          "world point on the axis projects to the screen centre");

    p.x = 10; p.y = 0; p.z = 10;                              
    int inside = game_w2s(&cam, &p, W, H, &sx, &sy);
                                                                  
                                                                              
                                                        
    check(inside == 1 && fabsf(sx - 1500.0f) < 1.0f && fabsf(sy - 540.0f) < 0.5f,
          "45 deg right ray lands where the aspect ratio puts it");

                                                                      
    p.x = 1.7777778f * 10.0f; p.y = 0; p.z = 10;
    game_w2s(&cam, &p, W, H, &sx, &sy);
    check(sx > 1918.0f && sx < 1922.0f, "ray on the horizontal edge lands on the right border");

    p.x = 0; p.y = 10; p.z = 10;                           
    game_w2s(&cam, &p, W, H, &sx, &sy);
    check(fabsf(sx - 960.0f) < 0.5f && sy < 5.0f, "45 deg up ray lands on the top edge");

    p.x = 0; p.y = 0; p.z = -5;
    check(game_w2s(&cam, &p, W, H, &sx, &sy) == 0, "point behind the camera is rejected");
                                                                         
                                                                             
    cam.aspect=16.0f/9.0f;
    RR_VIEWPORT view=game_viewport(&cam,1920,1200);
    check(fabsf(view.y-60.0f)<0.01f && fabsf(view.height-1080.0f)<0.01f,
          "16:10 client fits the 16:9 camera between 60px bars");
    p=(RR_V3){10,0,10};
    game_w2s(&cam,&p,1920,1200,&sx,&sy);
    check(fabsf(sx-1500.0f)<0.5f && fabsf(sy-600.0f)<0.5f,
          "off-centre ESP uses the rendered height, not the full client height");
    p=(RR_V3){0,10,10};
    game_w2s(&cam,&p,1920,1200,&sx,&sy);
    check(fabsf(sy-60.0f)<0.5f,"upper image edge excludes the letterbox bar");
    view=game_viewport(&cam,2560,1080);
    check(fabsf(view.x-320.0f)<0.01f && fabsf(view.width-1920.0f)<0.01f,
          "ultrawide client fits a 16:9 camera between side bars");
    {
        RR_V3 points[]={{0,0,10},{10,0,10},{-10,3,10},{0,10,10},{0,0,-5}};
        float xy[10];
        int parity=1;
        const int sizes[][2]={{1920,1080},{1920,1200},{2560,1080},{1280,1024}};
        for(int size=0;size<4;++size) {
            game_w2s_many(&cam,points,5,sizes[size][0],sizes[size][1],xy);
            for(int point=0;point<4;++point) {
                game_w2s(&cam,&points[point],sizes[size][0],sizes[size][1],&sx,&sy);
                if(fabsf(sx-xy[point*2])>0.01f || fabsf(sy-xy[point*2+1])>0.01f) parity=0;
            }
            if(isfinite(xy[8]) || isfinite(xy[9])) parity=0;
        }
        check(parity,"skeleton and mesh projections agree across viewport shapes");
    }
    cam.aspect=NAN;
    view=game_viewport(&cam,1920,1200);
    check(view.x==0 && view.y==0 && view.height==1200,
          "unavailable lens aspect falls back to the full client");
    cam.aspect=0;
    cam.fov=NAN;
    p=(RR_V3){0,0,10};
    check(!game_w2s(&cam,&p,W,H,&sx,&sy) && !isfinite(sx) && !isfinite(sy),
          "invalid lens cannot publish screen coordinates");
    cam.fov=90;
    (void)inside;

                                                                         
    {
        BR_Config cfg=*br_get(),effective;
        cfg.weapon_profiles=1;cfg.w3_fov=77;cfg.w3_speed=900;
        rr_effective_config(&cfg,3,&effective);
        check(effective.aa_fov_px==77 && effective.bullet_speed==900 && cfg.aa_fov_px!=77,
              "weapon profile overrides a copy without changing general settings");
        rr_effective_config(&cfg,0,&effective);
        check(effective.aa_fov_px==cfg.aa_fov_px,"unknown weapons retain the general profile");
        cfg.weapon_override=3;rr_effective_config(&cfg,0,&effective);
        check(effective.aa_fov_px==77,"manual weapon category overrides unknown names");
        check(rr_weapon_class("DP9_m1")==1 && rr_weapon_class("unknown")==0,
              "weapon name matching is bounded and preserves an unknown state");
        check(rr_item_icon("pistol_ammo_1",1)==314 && rr_item_icon("DP9_m1",0)==311 &&
              rr_item_icon("afak_1",1)==316 && rr_item_icon("unknown",1)==0,
              "ItemIcons selects original game IDs and preserves unknown names");
        int icon_resources=1;
        for(int id=INVOKE_ICON_FIRST;id<=INVOKE_ICON_LAST;++id){
            HRSRC icon=FindResourceW(NULL,MAKEINTRESOURCEW(id),MAKEINTRESOURCEW(10));
            const unsigned char* data=icon?(const unsigned char*)LockResource(LoadResource(NULL,icon)):NULL;
            if(!icon || SizeofResource(NULL,icon)<8 || !data || memcmp(data,"\x89PNG\r\n\x1a\n",8))icon_resources=0;
        }
        check(icon_resources,"all original ROB RUN item sprites are embedded as PNG resources");
        RR_V3 predicted;
        check(rr_ballistic_point((RR_V3){0},(RR_V3){0,0,100},(RR_V3){10,0,0},100,0,&predicted) &&
              predicted.x>10 && predicted.x<10.2f,"projectile prediction leads a moving target");
        check(rr_ballistic_point((RR_V3){0},(RR_V3){0,0,100},(RR_V3){0},100,9.81f,&predicted) &&
              predicted.y>4.9f && predicted.y<5.1f,"projectile prediction compensates gravity in metres");
        check(!rr_ballistic_point((RR_V3){0},(RR_V3){0,0,100},(RR_V3){0},0,9.81f,&predicted) &&
              !rr_ballistic_point((RR_V3){0},(RR_V3){0,0,100},(RR_V3){0,0,200},100,0,&predicted),
              "disabled or unreachable projectile solutions are rejected");
        RR_TRIGGER_CLOCK trigger={0};
        int timing=!rr_trigger_due(&trigger,1000,1,1,1,100,150) &&
                   !rr_trigger_due(&trigger,1099,1,1,1,100,150) &&
                   rr_trigger_due(&trigger,1100,1,1,1,100,150) &&
                   !rr_trigger_due(&trigger,1200,1,1,1,100,150) &&
                   rr_trigger_due(&trigger,1250,1,1,1,100,150);
        check(timing,"trigger clock enforces dwell and the global firing interval");
        rr_trigger_due(&trigger,1300,0,0,0,100,150);
        check(!rr_trigger_due(&trigger,1400,1,1,1,100,150),"trigger eligibility loss resets target dwell");
        RR_PLAYER loot={0};snprintf(loot.name,sizeof(loot.name),"Ammo_Box");
        strcpy_s(cfg.loot_values,sizeof(cfg.loot_values),"ammo=25;cash=100");cfg.loot_min_value=20;cfg.loot_category=2;
        check(rr_loot_value(&loot,&cfg)==25 && rr_loot_allowed(&loot,&cfg),"loot filters use explicit case-insensitive custom value rules");
        strcpy_s(loot.name,sizeof(loot.name),"unknown");
        check(rr_loot_value(&loot,&cfg)==-1 && !rr_loot_allowed(&loot,&cfg),"unknown loot is not assigned an invented price");
        cfg.layout_auto=1;
        check(rr_layout_bank(&cfg,1920,1080)==0 && rr_layout_bank(&cfg,1920,1200)==1 &&
              rr_layout_bank(&cfg,3440,1440)==2,"HUD layout follows the client aspect group");
        cfg.h1_radar_x=1840;cfg.h1_radar_y=1010;cfg.h1_radar_w=800;cfg.h1_radar_h=800;
        RR_HUD_RECT hud=rr_hud_rect(&cfg,1,0,(RR_VIEWPORT){0,60,1920,1080});
        check(hud.x+hud.w<=1920 && hud.y+hud.h<=1140,"HUD dragging cannot place a widget outside the image");
        RR_CAMERA camera={0};camera.valid=1;camera.fov=90;camera.fwd.z=1;
        RR_PLAYER targets[2]={0};
        for(int i=0;i<2;++i){targets[i].valid=1;targets[i].addr=0x100+i;targets[i].id=i+1;targets[i].pos=(RR_V3){(float)(i+1),-1.65f,10};}
        targets[0].dist=100;targets[1].dist=10;
        cfg.aa_bone=0;cfg.aa_fov_px=200;cfg.aa_dist=0;cfg.aa_closest_first=0;cfg.aa_priority=0;cfg.aa_los_on=0;cfg.aa_skip_downed=1;
        RR_AIM_PICK pick=rr_aim_pick(targets,2,&camera,1920,1080,&cfg,0,0,0,NULL);
        check(pick.player==&targets[0],"crosshair priority selects the nearest projected target");
        cfg.aa_priority=1;pick=rr_aim_pick(targets,2,&camera,1920,1080,&cfg,0,0,0,NULL);
        check(pick.player==&targets[1],"distance priority selects the nearest world target");
        pick=rr_aim_pick(targets,2,&camera,1920,1080,&cfg,0,0,targets[1].addr,NULL);
        check(pick.player==&targets[0],"next-target selection excludes the previous identity");
        targets[0].dead=targets[1].dead=1;pick=rr_aim_pick(targets,2,&camera,1920,1080,&cfg,0,0,0,NULL);
        check(!pick.player,"downed-target exclusion applies to acquisition");
        check(vk_to_id(VK_INSERT)==1 && vk_to_id(VK_F1)==0 && vk_to_id(VK_END)==3,
              "Insert toggles only the menu and F1 has no overlay action");
        br_config_set(&cfg,"enabled",0);
        check(cfg.enabled==1,"legacy master-off settings cannot disable the overlay");
        if(have_temp){
            cfg=*br_get();cfg.h2_radar_x=640;cfg.w5_recoil_y=123;cfg.box_style=3;
            strcpy_s(cfg.loot_filter,sizeof(cfg.loot_filter),"ammo");
            strcpy_s(cfg.loot_values,sizeof(cfg.loot_values),"ammo=25;cash=100");
            BR_Config roundtrip=*br_get();int saved=cfg_save_at(config_path,&cfg)&&cfg_load_at(config_path,&roundtrip);
            int same=saved;
            for(size_t i=0;i<sizeof(g_cfg_ints)/sizeof(g_cfg_ints[0]);++i){size_t offset=g_cfg_ints[i].offset;
                if(*(int*)((char*)&cfg+offset)!=*(int*)((char*)&roundtrip+offset))same=0;}
            check(same && !strcmp(roundtrip.loot_values,cfg.loot_values) && !strcmp(roundtrip.loot_filter,"ammo"),
                  "all visual, HUD, weapon and text settings survive a configuration round trip");
            DeleteFileW(config_path);
        }
        wchar_t profile[MAX_PATH];
        check(!profile_path(profile,0) && !profile_path(profile,6) && profile_path(profile,5),
              "profile destinations are limited to the five configured slots");
    }

                                                                        
    {
        RR_RECOIL_CLOCK recoil={0};
        rr_recoil_elapsed(&recoil,1000,1,0,1,0);
        unsigned dry=0;
        for(unsigned t=1002;t<=1040;t+=2)dry+=rr_recoil_elapsed(&recoil,t,1,0,1,0);
        check(dry==0,"clicks without a confirmed ShotEvent never move recoil control");
        rr_recoil_elapsed(&recoil,1042,1,1,1,0);
        unsigned pulse=0;
        for(unsigned t=1044;t<=1080;t+=2)pulse+=rr_recoil_elapsed(&recoil,t,1,1,1,0);
        check(pulse==24,"one shot earns one finite pulse; a held event cannot repeat it");
        rr_recoil_elapsed(&recoil,1082,1,0,1,20);
        rr_recoil_elapsed(&recoil,1084,1,1,1,20);
        check(rr_recoil_elapsed(&recoil,1090,1,0,1,20)==0 &&
              rr_recoil_elapsed(&recoil,1110,1,0,1,20)==6,
              "recoil delay starts from a confirmed shot, not the mouse button");
        check(rr_recoil_elapsed(&recoil,1112,1,-1,1,20)==0 && recoil.count==0,
              "unavailable shot data cancels pending correction");
        rr_recoil_elapsed(&recoil,1120,1,0,1,20);
        rr_recoil_elapsed(&recoil,1122,1,1,1,20);
        check(rr_recoil_elapsed(&recoil,1124,2,0,1,20)==0 && recoil.count==0,
              "changing weapon context cancels the previous shot pulse");
        rr_recoil_elapsed(&recoil,1126,2,1,1,20);
        check(rr_recoil_elapsed(&recoil,1128,2,0,0,20)==0 && !recoil.initialized,
              "disabling recoil cancels queued correction immediately");
        rr_recoil_elapsed(&recoil,0xfffffff0u,1,0,1,0);
        rr_recoil_elapsed(&recoil,0xfffffff2u,1,1,1,0);
        check(rr_recoil_elapsed(&recoil,2,1,0,1,0)==16,
              "confirmed-shot timing remains correct across clock wrap");
    }

                                                               
    {
        const char* tmp = "selftest_chain.txt";
        FILE* f = fopen(tmp, "w");
        if (f) {
            fprintf(f, "# test\nworld = GameAssembly.dll+0x81B7148\ncamera = world+0x38 +0x18\n");
            fclose(f);
        }
        int n = chain_load(tmp);
        check(n == 2, "chain file parsed two symbols");
        check(chain_defined("world") && chain_defined("camera"), "chain symbols are registered");
        DeleteFileA(tmp);
    }

    out("\n%s (%d failure%s)\n", g_fail ? "SELFTEST FAILED" : "SELFTEST OK",
           g_fail, g_fail == 1 ? "" : "s");
    return g_fail;
}

            

// Handle cls of.
static void cls_of(uint64_t obj, char* out, size_t n) {
    out[0] = 0;
    uint64_t k = u_klass(obj);
    if (!k) { snprintf(out, n, "<no class>"); return; }
    char nm[120];
    if (!u_class_name(k, nm, sizeof(nm))) { snprintf(out, n, "<bad name>"); return; }
    snprintf(out, n, "%s", nm);
}

// Run probe.
static int run_probe(void) {
    if (!rr_attach(g_exe)) {
        out("[probe] '%s' is not running\n", g_exe);
        return 2;
    }
    out("[probe] pid %u\n", g_rr_pid);
    if (!rr_drv_open()) {
        out("[probe] kernel backend unavailable (map kernel-mode.sys first)\n");
        return 3;
    }
    uint64_t mod = 0;
    uint32_t size = 0;
    if (!rr_module("GameAssembly.dll", &mod, &size)) {
        out("[probe] GameAssembly.dll not in the module list of '%s'\n", g_exe);
                                                                              
                                                                       
           
        {
            static char names[96][64];
            uint64_t bases[96];
            uint32_t sizes[96];
            int n = rr_modules(&names[0][0], bases, sizes, 96);
            out("[probe] target modules: %d\n", n);
            for (int i = 0; i < n && i < 16; i++)
                out("    %-40s base=%llX size=%X\n", names[i],
                    (unsigned long long)bases[i], sizes[i]);
        }
        return 4;
    }
    out("[probe] GameAssembly.dll base=%llX size=%X\n",
           (unsigned long long)mod, size);
    out("[probe] image base=%llX\n", (unsigned long long)rr_image_base());
    {
        uint64_t world = 0, battle = 0, camera = 0;
        int found = game_entry_probe(&world, &battle, &camera);
        out("[probe] UnityEntry: %s world=%llX battle=%llX camera=%llX\n",
            found ? "validated" : "unavailable; scan fallback",
            (unsigned long long)world, (unsigned long long)battle,
            (unsigned long long)camera);
    }

    chain_load("invoke_chain.cfg");
    out("[probe] chain symbols: %d\n", chain_count());
    for (int i = 0; i < chain_count(); i++) {
        uint64_t v = chain_value(i);
        char cn[120];
        cls_of(v, cn, sizeof(cn));
        out("  %-12s = %llX  (%s)\n", chain_name(i), (unsigned long long)v, v ? cn : "null");
    }

                                                                    
    const struct { const char* n; uint32_t rva; } statics[] = {
        { "WorldEntitiesModel", RR_SF_WORLDENTITIESMODEL },
        { "WorldModel",         RR_SF_WORLDMODEL },
        { "BattleCameraModel",  RR_SF_BATTLECAMERAMODEL },
        { "BattleCharacterModel", RR_SF_BATTLECHARACTERMODEL },
    };
    for (int i = 0; i < (int)(sizeof(statics) / sizeof(statics[0])); i++) {
        if (!statics[i].rva) continue;
        uint64_t p = rr_u64(mod + statics[i].rva);
        char cn[120];
        cls_of(p, cn, sizeof(cn));
        out("  static %-22s rva=%07X -> %llX (%s)\n", statics[i].n,
               statics[i].rva, (unsigned long long)p, p ? cn : "null");
    }

    int ok = game_init();
    out("[probe] game_init: %s\n", ok ? "world found" : "no world");
    char st[192];
    game_status(st, sizeof(st));
    out("[probe] status: %s\n", st);
    out("[probe] prim offset=0x%X dict offset=0x%X\n",
           game_prim_offset(), game_dict_offset());

    static RR_PLAYER players[RR_MAX_PLAYERS];
    RR_CAMERA cam;
    int n = game_snapshot(players, RR_MAX_PLAYERS, &cam);
    out("[probe] players=%d camera=%s\n", n, cam.valid ? "valid" : "missing");
    for (int i = 0; i < n && i < 16; i++) {
        out("  #%-3d id=%-5d pos=(%.2f, %.2f, %.2f) dist=%.1f dead=%d\n",
               i, players[i].id, players[i].pos.x, players[i].pos.y, players[i].pos.z,
               players[i].dist, players[i].dead);
    }
    if (n > 0) {
        uint64_t obj = players[0].addr;
        char cn[120];
        cls_of(obj, cn, sizeof(cn));
        out("[probe] player[0] class=%s\n", cn);
        uint64_t hits[8];
        int h = chain_sweep_ptr(obj, 0x10, 0x200, "BattleCameraModel", hits, 8);
        out("[probe] player[0] -> BattleCameraModel hits: %d\n", h);
        for (int i = 0; i < h; i++) out("    %llX\n", (unsigned long long)hits[i]);
    }
    if (PL(chain_addr("world"))) {
        uint64_t w = chain_addr("world");
        uint64_t hits[8];
        int h = chain_sweep_ptr(w, 0x10, 0x200, "WorldEntitiesModel", hits, 8);
        out("[probe] world -> WorldEntitiesModel hits (offset 0x38 expected): %d\n", h);
        for (int i = 0; i < h; i++) {
            uint64_t e = hits[i];
            for (uint32_t off = 0x10; off <= 0x200; off += 8) {
                if (rr_u64(w + off) == e) out("    at +0x%X\n", off);
            }
        }
    }
    return n >= 0 ? 0 : 1;
}

                 

                                                                        
                                                                         
                                           
   
// Handle inputtest proc.
static LRESULT CALLBACK inputtest_proc(int code, WPARAM w, LPARAM l) {
    if (code == HC_ACTION && (w == WM_KEYDOWN || w == WM_SYSKEYDOWN)) {
        const KBDLLHOOKSTRUCT* k = (const KBDLLHOOKSTRUCT*)l;
        out("key vk=0x%02X %s ctrl=%d\n", (unsigned)k->vkCode,
            vk_to_id((int)k->vkCode) ? "<hotkey>" : "",
            (GetAsyncKeyState(VK_CONTROL) & 0x8000) ? 1 : 0);
    }
    return CallNextHookEx(NULL, code, w, l);
}

                                                                                
                                                                             
                                                                          
// Run objlist.
static int run_objlist(void) {
    if (!rr_drv_open()) { out("[objlist] kernel backend unavailable\n"); return 3; }
    if (!rr_attach(g_exe)) { out("[objlist] '%s' is not running\n", g_exe); return 2; }
    game_objlist_diag(g_out);
    out("[objlist] done\n");
    return 0;
}

// Run gomlist.
static int run_gomlist(void) {
    if (!rr_drv_open()) { out("[gomlist] kernel backend unavailable\n"); return 3; }
    if (!rr_attach(g_exe)) { out("[gomlist] '%s' is not running\n", g_exe); return 2; }
    game_gom_list_diag(g_out);
    out("[gomlist] done\n");
    return 0;
}

// Run chaindiag.
static int run_chaindiag(void) {
    if (!rr_drv_open()) { out("[chain] kernel backend unavailable\n"); return 3; }
    if (!rr_attach(g_exe)) { out("[chain] '%s' is not running\n", g_exe); return 2; }
    game_chain_diag(g_out);
    out("[chain] done\n");
    return 0;
}

// Run gomdiag.
static int run_gomdiag(void) {
    if (!rr_drv_open()) { out("[gomdiag] kernel backend unavailable\n"); return 3; }
    if (!rr_attach(g_exe)) { out("[gomdiag] '%s' is not running\n", g_exe); return 2; }
    game_gom_diag(g_out);
    out("[gomdiag] done\n");
    return 0;
}

// Run offsets.
static int run_offsets(void) {
    if (!rr_drv_open()) {
        out("[offsets] kernel backend unavailable (map kernel-mode.sys first)\n");
        return 3;
    }
    if (!rr_attach(g_exe)) {
        out("[offsets] '%s' is not running\n", g_exe);
        return 2;
    }
    out("[offsets] pid %u\n", g_rr_pid);
    game_init();
    {
        char st[192];
        game_status(st, sizeof(st));
        out("[offsets] world=0x%llX status: %s\n",
            (unsigned long long)game_world_addr(), st);
    }

    static const char* classes[] = {
        "WorldModel", "WorldEntitiesModel", "BattleCameraModel", "BattleCharacterModel",
        "CharacterTransformModel", "TransformModel", "CharacterReplicatedTransformModel",
        "BattleCharacterData", "BattleCharacterCommonData", "BattleCharacterPrivateData",
        "BattleCharacterPublicData", "WeaponModel", "CrouchModel", "BattleStanceModel",
        "PlayerBattleModel", "HealthModel"
    };
    for (int i = 0; i < (int)(sizeof(classes) / sizeof(classes[0])); ++i) {
        out("[offsets] resolving %s ...\n", classes[i]);
        game_dump_class(classes[i], g_out);
    }

    static const char* statics[] = {
        "WorldModel", "WorldEntitiesModel", "BattleCharacterModel", "BattleCameraModel"
    };
    for (int i = 0; i < (int)(sizeof(statics) / sizeof(statics[0])); ++i) {
        out("[offsets] static %s ...\n", statics[i]);
        game_dump_static(statics[i], g_out);
    }

    out("[offsets] done\n");
    return 0;
}

// Run inputtest.
static int run_inputtest(int seconds) {
    if (seconds <= 0) seconds = 30;
    out("input test: %d s, press INSERT / END ...\n", seconds);
    HHOOK h = SetWindowsHookExW(WH_KEYBOARD_LL, inputtest_proc, GetModuleHandleW(NULL), 0);
    out("low-level hook: %s\n", h ? "installed" : "FAILED");
    if (!h) return 1;
    DWORD end = GetTickCount() + (DWORD)seconds * 1000;
    while (GetTickCount() < end) {
        MSG m;
        while (PeekMessage(&m, NULL, 0, 0, PM_REMOVE)) {
            TranslateMessage(&m);
            DispatchMessage(&m);
        }
        Sleep(10);
    }
    UnhookWindowsHookEx(h);
    out("input test done\n");
    return 0;
}

                

                                                                           
                                                                               
                                         
// Handle log stack.
static void rr_log_stack(EXCEPTION_POINTERS* ep, FILE* f) {
    HMODULE dbg = LoadLibraryA("dbghelp.dll");
    if (!dbg) return;
    typedef BOOL (WINAPI *PFN_SW64)(DWORD,HANDLE,HANDLE,LPSTACKFRAME64,PVOID,
        PREAD_PROCESS_MEMORY_ROUTINE64,PFUNCTION_TABLE_ACCESS_ROUTINE64,
        PGET_MODULE_BASE_ROUTINE64,PTRANSLATE_ADDRESS_ROUTINE64);
    typedef BOOL (WINAPI *PFN_SYM)(HANDLE,PCSTR,BOOL);
    typedef BOOL (WINAPI *PFN_SF)(HANDLE,DWORD64,PSYMBOL_INFO,PDWORD64);
    PFN_SW64 pSW = (PFN_SW64)(void*)GetProcAddress(dbg, "StackWalk64");
    PFN_SYM pSI = (PFN_SYM)(void*)GetProcAddress(dbg, "SymInitialize");
    PFN_SF pSF = (PFN_SF)(void*)GetProcAddress(dbg, "SymFromAddr");
    if (!pSW || !pSI) return;
    static int inited = 0;
    if (!inited) { pSI(GetCurrentProcess(), NULL, TRUE); inited = 1; }

    CONTEXT ctx = *ep->ContextRecord;
    STACKFRAME64 sf;
    memset(&sf, 0, sizeof(sf));
    sf.AddrPC.Offset = ctx.Rip;    sf.AddrPC.Mode = AddrModeFlat;
    sf.AddrFrame.Offset = ctx.Rbp; sf.AddrFrame.Mode = AddrModeFlat;
    sf.AddrStack.Offset = ctx.Rsp; sf.AddrStack.Mode = AddrModeFlat;

    for (int k = 0; k < 12; k++) {
        if (!pSW(IMAGE_FILE_MACHINE_AMD64, GetCurrentProcess(), GetCurrentThread(),
                 &sf, &ctx, NULL, NULL, NULL, NULL)) break;
        HMODULE hm = NULL;
        char m[MAX_PATH] = "?";
        if (GetModuleHandleExA(0x4 | 0x2, (LPCSTR)sf.AddrPC.Offset, &hm) && hm)
            GetModuleFileNameA(hm, m, MAX_PATH);
        fprintf(f, "  frame[%d] %s+0x%llX\n", k, m,
                (unsigned long long)(sf.AddrPC.Offset - (uint64_t)hm));
    }
}

// Handle crash filter.
static LONG WINAPI rr_crash_filter(EXCEPTION_POINTERS* ep) {
    FILE* f = fopen("crash.log", "a");
    if (f) {
        {
                                                                                     
            char mod[MAX_PATH] = "?";
            HMODULE hm = NULL;
            if (GetModuleHandleExA(0x00000004                  | 0x00000002                       ,
                                  (LPCSTR)ep->ExceptionRecord->ExceptionAddress, &hm) && hm)
                GetModuleFileNameA(hm, mod, MAX_PATH);
            fprintf(f, "%lu exception=%08lX address=%p module=%s rva=0x%llX\n",
                    (unsigned long)GetTickCount(),
                    (unsigned long)ep->ExceptionRecord->ExceptionCode,
                    ep->ExceptionRecord->ExceptionAddress, mod,
                    (unsigned long long)((uintptr_t)ep->ExceptionRecord->ExceptionAddress - (uintptr_t)hm));
        }
        rr_log_stack(ep, f);
        fclose(f);
    }
    return EXCEPTION_EXECUTE_HANDLER;
}

                       

                                                                    
                                                                           
                                                                     
   
// Handle loot hexdump.
static void loot_hexdump(uint64_t base, const uint8_t* p, unsigned len) {
    unsigned i, j;
    for (i = 0; i < len; i += 16) {
        unsigned n = len - i;
        if (n > 16) n = 16;
        out("    %016llX  ", (unsigned long long)(base + i));
        for (j = 0; j < 16; j++) {
            if (j < n) out("%02X ", p[i + j]);
            else out("   ");
        }
        out(" |");
        for (j = 0; j < n; j++) {
            unsigned char c = p[i + j];
            out("%c", (c >= 32 && c < 127) ? c : '.');
        }
        out("|\n");
    }
}

// Handle loot dump obj.
static void loot_dump_obj(uint64_t addr, const char* label) {
    static uint8_t buf[0x800];
    char cn[96];
    uint64_t klass;
    uint32_t isz_e8 = 0, isz_f0 = 0;
    int ok_e8, ok_f0;
    unsigned isize, want, o;

    if (!PL(addr)) {
        out("  %s ---\n", label);
        return;
    }
    klass = u_klass(addr);
    if (!u_class_name(klass, cn, sizeof cn)) strcpy(cn, "?");
    ok_e8 = PL(klass) ? rr_read(klass + U_CLASS_GENCONT, &isz_e8, 4) : 0;
    ok_f0 = PL(klass) ? rr_read(klass + U_CLASS_INST_SIZE, &isz_f0, 4) : 0;

                                                                            
                                                                           
       
    if (ok_f0 && isz_f0 >= 0x18 && isz_f0 <= 0x800) isize = isz_f0;
    else if (ok_e8 && isz_e8 >= 0x18 && isz_e8 <= 0x800) isize = isz_e8;
    else if (strstr(cn, "LootNodeModel") || strstr(cn, "RandomLootObjectModel")) isize = 0xC0;
    else isize = 0x100;

    out("  %s @ %016llX  class=%s  size=0x%X  [0xF0=%u/%s 0xE8=%u/%s]\n",
        label, (unsigned long long)addr, cn, isize,
        isz_f0, ok_f0 ? "ok" : "unreadable", isz_e8, ok_e8 ? "ok" : "unreadable");

    want = isize;
    while (want > 0x18 && !rr_read(addr, buf, want)) want -= 8;
    if (!rr_read(addr, buf, want)) { out("    (unreadable)\n"); return; }

    for (o = 0x10; o + 8 <= want; o += 8) {
        uint64_t v, k2;
        char c2[96];
        memcpy(&v, buf + o, 8);
        if (!v) continue;
        if (!PL(v)) {
            uint32_t lo;
            float fv;
            memcpy(&lo, &v, 4);
            memcpy(&fv, &lo, 4);
            out("    +0x%02X = %d, %d   (0x%016llX)  lo32 f=%g\n",
                o, (int)lo, (int)(uint32_t)(v >> 32),
                (unsigned long long)v, (double)fv);
            continue;
        }
        k2 = u_klass(v);
        if (!u_class_name(k2, c2, sizeof c2)) {
                                                                                    
                                                                                           
               
            uint32_t lo;
            float fv;
            memcpy(&lo, &v, 4);
            memcpy(&fv, &lo, 4);
            out("    +0x%02X = %d, %d   (0x%016llX)  lo32 f=%g\n",
                o, (int)lo, (int)(uint32_t)(v >> 32),
                (unsigned long long)v, (double)fv);
            continue;
        }
        out("    +0x%02X -> %s @ %016llX", o, c2, (unsigned long long)v);
        if (strcmp(c2, "String") == 0) {
            char s[128];
            if (u_string(v, s, sizeof s)) out("  \"%s\"", s);
        } else if (strstr(c2, "IntData")) {
            out("  value=%d", rr_i32(v + 0x18));
        } else if (strstr(c2, "BoolData")) {
            out("  value=%u", (unsigned)rr_u32(v + 0x18));
        } else if (strstr(c2, "FloatData")) {
            out("  value=%.3f", rr_f32(v + 0x18));
        }
        out("\n");

                                                                       
                                                                                
                                                                            
                                                                      
                                                                         
        if (strstr(c2, "PrimitiveModel") || strstr(c2, "ReferenceData")) {
            uint64_t inner = rr_u64(v + 0x10);
            char c3[96];
            if (PL(inner) && u_class_name(u_klass(inner), c3, sizeof c3)) {
                out("        inner %s @ %016llX", c3, (unsigned long long)inner);
                if (strstr(c3, "IntData")) out(" value=%d", rr_i32(inner + 0x18));
                else if (strstr(c3, "FloatData")) out(" value=%.3f", rr_f32(inner + 0x18));
                else if (strstr(c3, "BoolData")) out(" value=%u", (unsigned)rr_u32(inner + 0x18));
                else if (strstr(c3, "StringData")) {
                    uint64_t sp = rr_u64(inner + 0x18);
                    char s[96];
                    if (PL(sp) && u_string(sp, s, sizeof s)) out(" \"%s\"", s);
                } else if (strcmp(c3, "String") == 0) {
                    char s[96];
                    if (u_string(inner, s, sizeof s)) out(" \"%s\"", s);
                }
                out("\n");
            }
        }
    }
    loot_hexdump(addr, buf, want);
}

                                                                             
                                                                     
// Handle ci contains.
static int ci_contains(const char* hay, const char* needle) {
    size_t nl = strlen(needle);
    if (!nl) return 1;
    for (; *hay; ++hay) {
        size_t k = 0;
        while (k < nl && hay[k]) {
            char a = hay[k], b = needle[k];
            if (a >= 'A' && a <= 'Z') a = (char)(a + 32);
            if (b >= 'A' && b <= 'Z') b = (char)(b + 32);
            if (a != b) break;
            ++k;
        }
        if (k == nl) return 1;
    }
    return 0;
}

// Handle loot dump entry.
static void loot_dump_entry(const RR_LOOT_ENT* e, int idx, const char* tag) {
    out("\n%s[%d] id=%d kind=%d rarity=%d name=\"%s\" pos=(%.1f, %.1f, %.1f)\n",
        tag, idx, e->id, e->kind, e->rarity, e->name, e->pos.x, e->pos.y, e->pos.z);
    loot_dump_obj(e->obj, "node");
    loot_dump_obj(e->desc, "desc");
    loot_dump_obj(e->loot, "loot");
    loot_dump_obj(e->inv, "item");
}

// Handle loot report.
static void loot_report(const RR_LOOT_DEBUG* d, int got) {
    out("[lootdump] world=%016llX nodes dict=%016llX count=%d loose dict=%016llX count=%d cached=%d\n",
        (unsigned long long)d->world,
        (unsigned long long)d->dict_nodes, d->entries_nodes,
        (unsigned long long)d->dict_loose, d->entries_loose, got);
    out("[lootdump] nodes: seen=%d empty=%d unresolved=%d   loose: seen=%d empty=%d unresolved=%d\n",
        d->nodes_seen, d->nodes_empty, d->nodes_bad,
        d->loose_seen, d->loose_empty, d->loose_bad);
}

// Run lootdump.
static int run_lootdump(const char* filter) {
    static RR_LOOT_ENT ents[256];
    RR_LOOT_DEBUG dbg;
    int loops, n = 0, i, shown = 0, got;
    enum { MAX_SHOW = 3 };

    if (!rr_attach(g_exe)) { out("[lootdump] '%s' is not running\n", g_exe); return 2; }
    if (!rr_drv_open()) { out("[lootdump] kernel backend unavailable\n"); return 3; }
    if (!game_init()) { out("[lootdump] game_init: no world\n"); return 4; }
    out("[lootdump] pid %u filter=\"%s\"\n", g_rr_pid, filter ? filter : "");

                                                                                 
                                                                            
                                 
    memset(&dbg, 0, sizeof dbg);
    got = rr_loot_refresh(&dbg);
    loot_report(&dbg, got);
    for (loops = 0; got <= 0 && loops < 40; ++loops) {
        Sleep(200);
        got = rr_loot_refresh(&dbg);
    }
    if (got > 0) n = rr_loot_snapshot(ents, 256);
    out("[lootdump] %d loot entries after %d poll(s)\n", n, loops);
    if (!n) {
        loot_report(&dbg, got);
        out("[lootdump] nothing published.\n"
            "           world=0 means the scan found no WorldEntitiesModel;\n"
            "           count=0 means the dictionary is empty (area not streamed in);\n"
            "           count>0 with cached=0 means the entries failed validation.\n");
        return 5;
    }

                                                                                
                                                                                 
    out("\n[lootdump] all entries:\n");
    for (i = 0; i < n; ++i)
        out("  [%02d] kind=%d id=%-5d rarity=%d name=\"%s\"\n",
            i, ents[i].kind, ents[i].id, ents[i].rarity, ents[i].name);

                                                                              
                                                                               
                                          
    {
        int want_idx = (filter && filter[0] == '#') ? atoi(filter + 1) : -1;
        if (want_idx >= 0) {
            if (want_idx < n) {
                loot_dump_entry(&ents[want_idx], want_idx, "");
                shown = 1;
            } else {
                out("\n[lootdump] index %d out of range (0..%d)\n", want_idx, n - 1);
            }
        } else {
            int pass, k;
            for (pass = 0; pass < 2 && shown < MAX_SHOW; ++pass) {
                int want_kind = pass == 0 ? 2 : 1;
                for (k = 0; k < n && shown < MAX_SHOW; ++k) {
                    if (ents[k].kind != want_kind) continue;
                    if (!ci_contains(ents[k].name, filter)) continue;
                    loot_dump_entry(&ents[k], k, "");
                    ++shown;
                }
            }
        }
    }

                                                                                 
                                                                           
    if (!shown) {
        int fallback = 0;
        out("\n[lootdump] no name matched \"%s\" - dumping nodes regardless\n", filter);
        for (i = 0; i < n && fallback < 2; ++i) {
            if (ents[i].kind != 2) continue;
            loot_dump_entry(&ents[i], i, "  ");
            ++fallback;
        }
        shown = fallback;
    }

    out("\n[lootdump] %d of %d entries dumped\n", shown, n);
    return shown ? 0 : 6;
}

                                                                        
                                                                         
                                                             
   
// Run lootset.
static int run_lootset(const char* filter, int vmin, int vmax) {
    static RR_LOOT_ENT ents[256];
    static uint64_t done[32];
    RR_LOOT_DEBUG dbg;
    int n, i, k, ndone = 0, written = 0;

    if (!rr_attach(g_exe)) { out("[lootset] '%s' is not running\n", g_exe); return 2; }
    if (!rr_drv_open()) { out("[lootset] kernel backend unavailable\n"); return 3; }
    if (!game_init()) { out("[lootset] game_init: no world\n"); return 4; }
    (void)rr_loot_refresh(&dbg);
    n = rr_loot_snapshot(ents, 256);
    out("[lootset] %d entries, filter=\"%s\", target %d..%d\n",
        n, filter ? filter : "", vmin, vmax);
    if (!n) return 5;

    for (i = 0; i < n; ++i) {
        RR_LOOT_ENT* e = &ents[i];
        int old_min, old_max, new_min = -1, new_max = -1, dup = 0;

        if (!ci_contains(e->name, filter)) continue;
        if (!u_is(e->loot, "LootNodeLootDescription")) continue;
        for (k = 0; k < ndone; ++k) if (done[k] == e->loot) { dup = 1; break; }
        if (dup) continue;
        if (ndone < (int)(sizeof(done) / sizeof(done[0]))) done[ndone++] = e->loot;

        old_min = rr_i32(e->loot + 0x28);
        old_max = rr_i32(e->loot + 0x2C);
        if (rr_write(e->loot + 0x28, &vmin, 4) && rr_write(e->loot + 0x2C, &vmax, 4)) {
            new_min = rr_i32(e->loot + 0x28);
            new_max = rr_i32(e->loot + 0x2C);
        }
        out("  %-22s table %016llX  %d..%d -> %d..%d  %s\n",
            e->name, (unsigned long long)e->loot, old_min, old_max,
            new_min, new_max,
            (new_min == vmin && new_max == vmax) ? "VERIFIED" : "NOT APPLIED");
        if (new_min == vmin && new_max == vmax) written++;
    }
    out("[lootset] %d distinct table(s), %d written and verified\n", ndone, written);
    return written ? 0 : 6;
}

                                                                    
                                                                         
                                                                        
   
// Run itemset.
static int run_itemset(const char* filter, int value) {
    static RR_LOOT_ENT ents[256];
    static uint64_t done[32];
    RR_LOOT_DEBUG dbg;
    int n, i, k, ndone = 0, written = 0;

    if (!rr_attach(g_exe)) { out("[itemset] '%s' is not running\n", g_exe); return 2; }
    if (!rr_drv_open()) { out("[itemset] kernel backend unavailable\n"); return 3; }
    if (!game_init()) { out("[itemset] game_init: no world\n"); return 4; }
    (void)rr_loot_refresh(&dbg);
    n = rr_loot_snapshot(ents, 256);
    out("[itemset] %d entries, filter=\"%s\", value=%d\n", n, filter ? filter : "", value);
    if (!n) return 5;

    for (i = 0; i < n; ++i) {
        RR_LOOT_ENT* e = &ents[i];
        int old_v, new_v = -1, dup = 0;

        if (!ci_contains(e->name, filter)) continue;
        if (!u_is(e->inv, "InventoryItemDescription")) continue;
        for (k = 0; k < ndone; ++k) if (done[k] == e->inv) { dup = 1; break; }
        if (dup) continue;
        if (ndone < (int)(sizeof(done) / sizeof(done[0]))) done[ndone++] = e->inv;

        old_v = rr_i32(e->inv + 0x2C);
        if (rr_write(e->inv + 0x2C, &value, 4)) new_v = rr_i32(e->inv + 0x2C);
        out("  %-22s item %016llX  value %d -> %d  %s\n",
            e->name, (unsigned long long)e->inv, old_v, new_v,
            (new_v == value) ? "VERIFIED" : "NOT APPLIED");
        if (new_v == value) written++;

                                                                              
                                                                      
           
        if (u_is(e->loot, "LootObjectItemDescription")) {
            int lo = rr_i32(e->loot + 0x28), ln = -1;
            if (rr_write(e->loot + 0x28, &value, 4)) ln = rr_i32(e->loot + 0x28);
            out("  %-22s loot %016llX  count %d -> %d  %s\n",
                e->name, (unsigned long long)e->loot, lo, ln,
                (ln == value) ? "VERIFIED" : "NOT APPLIED");
        }
    }
    out("[itemset] %d distinct item(s), %d written and verified\n", ndone, written);
    return written ? 0 : 6;
}

                                                                      
                                                                        
                                                                            
                                                                           
   
typedef struct { uint64_t addr, parent; uint32_t poff; int depth; } CASHNODE;

// Handle cash visited.
static int cash_visited(const CASHNODE* q, int n, uint64_t a) {
    int i;
    for (i = 0; i < n; ++i) if (q[i].addr == a) return 1;
    return 0;
}

// Run cashscan.
static int run_cashscan(long long amount, int maxdepth) {
    static CASHNODE q[3000];
    char cn[96];
    uint64_t start;
    int qn = 0, head = 0, hits = 0;

    if (!rr_attach(g_exe)) { out("[cashscan] '%s' is not running\n", g_exe); return 2; }
    if (!rr_drv_open()) { out("[cashscan] kernel backend unavailable\n"); return 3; }
    if (!game_init()) { out("[cashscan] game_init: no world\n"); return 4; }

    start = rr_local_player();
    if (!start) {
        RR_CAMERA cam;
        static RR_PLAYER players[RR_MAX_PLAYERS];
        int n = game_snapshot(players, RR_MAX_PLAYERS, &cam);
        out("[cashscan] no local player flagged yet (%d entities) - try again in game\n", n);
        return 5;
    }
    if (!u_class_name(u_klass(start), cn, sizeof cn)) strcpy(cn, "?");
    out("[cashscan] local player %016llX (%s), looking for %lld, depth %d\n",
        (unsigned long long)start, cn, amount, maxdepth);

    q[qn].addr = start; q[qn].parent = 0; q[qn].poff = 0; q[qn].depth = 0; qn++;

    while (head < qn) {
        CASHNODE n = q[head++];
        uint64_t klass = u_klass(n.addr);
        uint32_t isz = 0;
        unsigned want = 0x120, o;
        static uint8_t buf[0x400];

        if (!u_class_name(klass, cn, sizeof cn)) strcpy(cn, "?");
        if (PL(klass)) {
            isz = rr_u32(klass + U_CLASS_INST_SIZE);
            if (isz >= 0x18 && isz < 0x401) want = isz;
        }
        while (want > 0x18 && !rr_read(n.addr, buf, want)) want -= 8;
        if (!rr_read(n.addr, buf, want)) continue;

                                                                               
        for (o = 0x10; o + 4 <= want; o += 4) {
            int32_t v32;
            memcpy(&v32, buf + o, 4);
            if ((long long)v32 == amount) {
                out("[cashscan] HIT int32 %lld at %016llX  [%s]%s\n", amount,
                    (unsigned long long)(n.addr + o), cn,
                    n.parent ? "" : " (root object)");
                if (n.parent)
                    out("[cashscan]    reached from %016llX +0x%X\n",
                        (unsigned long long)n.parent, n.poff);
                hits++;
            }
            if (o + 8 <= want) {
                int64_t v64;
                memcpy(&v64, buf + o, 8);
                if (v64 == amount) {
                    out("[cashscan] HIT int64 %lld at %016llX  [%s]\n", amount,
                        (unsigned long long)(n.addr + o), cn);
                    hits++;
                }
            }
        }

                                                                                 
                                                                                 
        if (strstr(cn, "IntData") || strstr(cn, "Int32Data")) {
            out("[cashscan]   intdata %016llX %s value=%d\n",
                (unsigned long long)n.addr, cn, rr_i32(n.addr + 0x18));
        }

        if (n.depth >= maxdepth) continue;
        for (o = 0x10; o + 8 <= want; o += 8) {
            uint64_t v, k2;
            char c2[96];
            memcpy(&v, buf + o, 8);
            if (!PL(v)) continue;
            k2 = u_klass(v);
            if (!u_class_name(k2, c2, sizeof c2)) continue;
            if (qn >= (int)(sizeof(q) / sizeof(q[0]))) break;
            if (cash_visited(q, qn, v)) continue;
            q[qn].addr = v; q[qn].parent = n.addr; q[qn].poff = o; q[qn].depth = n.depth + 1; qn++;
        }
    }
    out("[cashscan] %d object(s) walked, %d hit(s)\n", qn, hits);
    return hits ? 0 : 6;
}

                                                                      
                                                                             
                                                                         
   
// Handle klass name at.
static int klass_name_at(uint64_t v, char* name, size_t n, int* strong) {
    uint64_t image, self;
    int i, any = 0;

    *strong = 0;
    if (!PL(v) || (v & 7)) return 0;
    image = rr_u64(v);
    if (!PL(image)) return 0;
    if (!u_class_name(v, name, n)) return 0;
    for (i = 0; name[i] && i < (int)n; i++) {
        unsigned char c = (unsigned char)name[i];
        if (c < 32 || c > 126) return 0;
        any = 1;
    }
    if (!any) return 0;
    if (rr_read(v + 0x20, &self, sizeof self) && self == v) *strong = 1;
    else if (rr_read(v + 0x18, &self, sizeof self) && self == v) *strong = 1;
    return 1;
}

                                                                              
                                                                       
typedef struct { char name[64]; int count, strong; } CASHCLS;
static CASHCLS g_cashcls[48];
static int g_ncashcls;

// Handle cashcls note.
static void cashcls_note(const char* n, int strong) {
    int i;
    for (i = 0; i < g_ncashcls; ++i) {
        if (strcmp(g_cashcls[i].name, n) == 0) { g_cashcls[i].count++; g_cashcls[i].strong |= strong; return; }
    }
    if (g_ncashcls < (int)(sizeof(g_cashcls) / sizeof(g_cashcls[0]))) {
        memset(&g_cashcls[g_ncashcls], 0, sizeof(g_cashcls[g_ncashcls]));
        strncpy(g_cashcls[g_ncashcls].name, n, sizeof(g_cashcls[g_ncashcls].name) - 1);
        g_cashcls[g_ncashcls].count = 1;
        g_cashcls[g_ncashcls].strong = strong;
        g_ncashcls++;
    }
}

                                                                         
                                                                             
                                                                    
// Handle cashfind report.
static int cashfind_report(uint64_t hit, long long amount, int verbose) {
    char cn[96];
    unsigned off;
    for (off = 0x10; off <= 0x400; off += 8) {
        uint64_t base = hit - off;
        int strong = 0;
        if (base < 0x10000) break;
        if (klass_name_at(base, cn, sizeof cn, &strong)) {
            cashcls_note(cn, strong);
            if (verbose)
                out("[cashfind] %016llX = %lld   in %s +0x%X (%s)\n",
                    (unsigned long long)hit, amount, cn, off, strong ? "strong" : "weak");
            return 1;
        }
    }
    return 0;
}

                                                                      
                                                                           
                                                                         
   
// Run cashfind.
static int run_cashfind(long long amount) {
    enum { BATCH = 64, CHUNK = 0x1000, MAXSHOW = 80 };
    uint64_t addr = 0x10000;
    const uint64_t limit = 0x7FFFFFFEFFFFull;
    uint8_t* pool;
    long chunks = 0;
    int hits = 0, raw = 0;
    long long last_mb = -1;

    if (!rr_attach(g_exe)) { out("[cashfind] '%s' is not running\n", g_exe); return 2; }
    if (!rr_drv_open()) { out("[cashfind] kernel backend unavailable\n"); return 3; }
    pool = (uint8_t*)malloc((size_t)BATCH * CHUNK);
    if (!pool) return 6;
    out("[cashfind] searching all private memory for the int32 %lld\n", amount);

                                                                          
                                                                             
                                                               
       
    {
        uint64_t lp = rr_local_player();
        if (!lp) {
            out("[cashfind] self-check skipped: no local player flagged yet\n");
        } else {
            uint64_t k = u_klass(lp);
            char n[96];
            int strong = 0;
            if (klass_name_at(k, n, sizeof n, &strong))
                out("[cashfind] self-check: player klass %016llX -> %s (%s)\n",
                    (unsigned long long)k, n, strong ? "strong" : "weak");
            else {
                free(pool);
                out("[cashfind] self-check FAILED: could not name the local player's own class\n"
                    "[cashfind] the identification logic is broken, so a scan would mean nothing - stop\n");
                return 7;
            }
        }
    }

    while (addr < limit) {
        RR_REGION rg;
        if (!rr_query_region(addr, &rg) || !rg.size) { addr += 0x10000; continue; }
        {
            uint64_t start = rg.base < 0x10000 ? 0x10000 : rg.base;
            uint64_t end = rg.base + rg.size;
            uint32_t pr = rg.protect & 0xFF;
            int usable = rg.state == 0x1000 && rg.type == 0x20000 &&
                         pr != 0 && !(pr & 0x01) && !(pr & 0x100);
            uint64_t q;
            for (q = start; usable && q < end; q += (uint64_t)BATCH * CHUNK) {
                RR_RD_REQ req[BATCH];
                int n = 0, i;
                for (i = 0; i < BATCH && q + (uint64_t)i * CHUNK < end; i++) {
                    uint64_t a = q + (uint64_t)i * CHUNK;
                    unsigned sz = (unsigned)((end - a) < CHUNK ? (end - a) : CHUNK);
                    req[n].addr = a; req[n].size = sz; req[n].dst = pool + (size_t)n * CHUNK;
                    memset(req[n].dst, 0, sz);
                    n++;
                }
                if (!n) break;
                if (rr_read_batch(req, n) <= 0) g_cashfind_bad++;
                for (i = 0; i < n; i++) {
                    uint8_t* p = (uint8_t*)(pool + (size_t)i * CHUNK);
                    unsigned o;
                    for (o = 0; o + 4 <= req[i].size; o += 4) {
                        int32_t v;
                        memcpy(&v, p + o, 4);
                        if ((long long)v != amount) continue;
                        raw++;
                        if (cashfind_report(req[i].addr + (uint64_t)o, amount, hits < MAXSHOW))
                            hits++;
                    }
                }
                chunks += n;
                {
                    long long mb = ((long long)chunks * CHUNK) >> 20;
                    if (mb != last_mb) {
                        last_mb = mb;
                        out("[cashfind] %lld MB read, %d raw, %d identified\n", mb, raw, hits);
                    }
                }
            }
            addr = (end > addr) ? end : addr + 0x10000;
        }
    }
    free(pool);
    out("[cashfind] %ld chunks, %d value(s) matched, %d inside an identifiable object%s\n",
        chunks, raw, hits, hits >= MAXSHOW ? " (display limit reached)" : "");
                                                                          
                                                                                  
       
    if (g_ncashcls) {
        int i, k;
        out("[cashfind] classes behind the matches:\n");
        for (i = 0; i < g_ncashcls; ++i) {
            int best = i;
            for (k = i + 1; k < g_ncashcls; ++k)
                if (g_cashcls[k].count > g_cashcls[best].count) best = k;
            if (best != i) { CASHCLS t = g_cashcls[i]; g_cashcls[i] = g_cashcls[best]; g_cashcls[best] = t; }
            out("  %-40s %3d  %s\n", g_cashcls[i].name, g_cashcls[i].count,
                g_cashcls[i].strong ? "strong" : "weak");
        }
    }
    if (g_cashfind_bad)
        out("[cashfind] WARNING: %ld batches returned no data\n", g_cashfind_bad);
    return hits ? 0 : 6;
}

                                                                        
                                                                          
                                                                            
   
// Run xfprobe.
static int run_xfprobe(int ms) {
    static RR_PLAYER players[RR_MAX_PLAYERS];
    RR_CAMERA cam;
    uint64_t addr = 0, t58 = 0, t60 = 0;
    float pa[3], pb[3];
    int have = 0, ca = 0, cb = 0, differ = 0, samples = 0;
    DWORD last_a = 0, last_b = 0;
    long sum_a = 0, sum_b = 0;

    if (!rr_attach(g_exe)) { out("[xfprobe] '%s' is not running\n", g_exe); return 2; }
    if (!rr_drv_open()) { out("[xfprobe] kernel backend unavailable\n"); return 3; }
    if (!game_init()) { out("[xfprobe] no world - get into a match first\n"); return 4; }

    int n = game_snapshot(players, RR_MAX_PLAYERS, &cam);
    float best = 1e30f;
    int best_i = -1;
    for (int i = 0; i < n; ++i) {
        if (!players[i].valid || players[i].local || players[i].is_loot ||
            players[i].is_vehicle || players[i].dead) continue;
        if (players[i].dist > 0.5f && players[i].dist < best) {
            best = players[i].dist;
            best_i = i;
        }
    }
    if (best_i < 0) { out("[xfprobe] no remote player in view (%d entries)\n", n); return 5; }

    addr = players[best_i].addr;
    t58 = rr_u64(addr + RR_OFF_CHAR_TRANSFORM);
    t60 = rr_u64(addr + RR_OFF_CHAR_REPL_TRANSFORM);
    out("[xfprobe] %d entries, target %016llX at %.1f m\n",
        n, (unsigned long long)addr, (double)best);
    out("[xfprobe] Transform          0x58 -> %016llX\n", (unsigned long long)t58);
    out("[xfprobe] ReplicatedTransform 0x60 -> %016llX\n", (unsigned long long)t60);
    if (!PL(t58) && !PL(t60)) {
        out("[xfprobe] neither offset resolved to a model - offsets are stale\n");
        return 6;
    }
    if (PL(t58) && rr_read(t58 + 0x10, pa, sizeof(pa)) == 0) memset(pa, 0, sizeof(pa));
    if (PL(t60) && rr_read(t60 + 0x10, pb, sizeof(pb)) == 0) memset(pb, 0, sizeof(pb));
    have = 1;

    if (ms < 500) ms = 500;
    if (ms > 5000) ms = 5000;
    out("[xfprobe] sampling for %d ms - keep the target moving\n", ms);
    DWORD t0 = timeGetTime();
    for (;;) {
        DWORD now = timeGetTime();
        if ((int)(now - t0) >= ms) break;
        if (PL(t58)) {
            float v[3];
            if (rr_read(t58 + 0x10, v, sizeof(v)) && memcmp(v, pa, sizeof(v)) != 0) {
                if (have && last_a) sum_a += (long)(now - last_a);
                ++ca;
                last_a = now;
                memcpy(pa, v, sizeof(pa));
            }
        }
        if (PL(t60)) {
            float v[3];
            if (rr_read(t60 + 0x10, v, sizeof(v)) && memcmp(v, pb, sizeof(v)) != 0) {
                if (have && last_b) sum_b += (long)(now - last_b);
                ++cb;
                last_b = now;
                memcpy(pb, v, sizeof(pb));
            }
        }
        if (PL(t58) && PL(t60) && memcmp(pa, pb, sizeof(pa)) != 0) ++differ;
        ++samples;
        Sleep(1);
    }

    out("[xfprobe] %d samples over %d ms\n", samples, ms);
    out("[xfprobe] 0x58 Transform          : %d changes, mean interval %.1f ms\n",
        ca, ca > 1 ? (double)sum_a / (double)(ca - 1) : 0.0);
    out("[xfprobe] 0x60 ReplicatedTransform: %d changes, mean interval %.1f ms\n",
        cb, cb > 1 ? (double)sum_b / (double)(cb - 1) : 0.0);
    out("[xfprobe] samples where the two disagree: %d of %d\n", differ, samples);
    out("[xfprobe] --- read this ---\n");
    out("[xfprobe] an interval near the simulated tick is the network transform;\n"
        "[xfprobe] one changing far more often is interpolated and is what the\n"
        "[xfprobe] overlay should be reading. If they never disagree, 0x60 is not\n"
        "[xfprobe] a separate transform on this build and the fix has to be\n"
        "[xfprobe] extrapolation by time-since-last-change instead.\n");
    return 0;
}

                                                                    
                                                                            
                                                                             
   
// Handle aimtest yaw.
static float aimtest_yaw(const RR_CAMERA* c) {
    return atan2f(c->fwd.x, c->fwd.z) * 57.29577951f;
}

// Run aimtest.
static int run_aimtest(void) {
    static RR_PLAYER players[RR_MAX_PLAYERS];
    RR_CAMERA cam;
    float before, after;
    int n;

    if (!rr_attach(g_exe)) {
        out("[aimtest] '%s' is not running\n", g_exe);
        return 2;
    }
    if (!rr_drv_open()) {
        out("[aimtest] kernel backend unavailable\n");
        return 3;
    }
    if (!game_init()) {
        out("[aimtest] no world - get into a match first\n");
        return 4;
    }

    n = game_snapshot(players, RR_MAX_PLAYERS, &cam);
    if (!cam.valid) {
        out("[aimtest] camera invalid (players=%d) - cannot measure\n", n);
        return 5;
    }
    before = aimtest_yaw(&cam);
    out("[aimtest] players=%d  yaw before = %.3f deg\n", n, before);
    out("[aimtest] click into the game now - injecting in 5 s\n");
    Sleep(5000);

    out("[aimtest] sending 20 x +8 px horizontal\n");
    for (int i = 0; i < 20; i++) {
        aa_move_mouse(8, 0);
        Sleep(15);
    }
    Sleep(250);

    n = game_snapshot(players, RR_MAX_PLAYERS, &cam);
    after = cam.valid ? aimtest_yaw(&cam) : before;
    out("[aimtest] players=%d  yaw after  = %.3f deg  (delta %.3f)\n", n, after,
        after - before);

                                                                          
                                                                                  
       
    float counts_sent = 20.0f * 8.0f;
    if (fabsf(after - before) > 0.05f) {
        float delta = fabsf(after - before);
        float deg_per_count = delta / counts_sent;
                                                                              
                                                           
           
        int vh = g_sh > 16 ? g_sh : 1080;
        float deg_per_px = cam.fov / (float)vh;
        float counts_per_px = deg_per_px / deg_per_count;
        out("[aimtest] PASS: injected input moves the view (delta %.3f deg for %.0f counts)\n",
            after - before, (double)counts_sent);
        out("[aimtest] CALIBRATION %.2f mouse counts per degree   (%.4f deg per count)\n",
            (double)(counts_sent / delta), (double)deg_per_count);
        out("[aimtest] at %.0f deg FOV over %d px: %.3f counts per screen pixel\n",
            (double)cam.fov, vh, (double)counts_per_px);
        out("[aimtest] aim maths assumed 1.000 -> set aa_aim_cal=%d\n",
            (int)(counts_per_px * 100.0f + 0.5f));
        out("[aimtest] (larpd1ck.cfg: aa_aim_cal=%d, then restart the overlay)\n",
            (int)(counts_per_px * 100.0f + 0.5f));
    } else
        out("[aimtest] FAIL: the view did not move. The game is not accepting\n"
            "           injected mouse input, so no aim tuning can help.\n");
    return 0;
}

                                                                    
                                                                            
                                                                          
   
static int find_game_threads(uint32_t pid, uint32_t* out, int max);

// Run ctxread.
static int run_ctxread(void) {
    uint32_t tids[64];
    int n, got = 0, denied = 0;
    long first_fail = 0;

    if (!rr_attach(g_exe)) {
        out("[ctx] '%s' is not running\n", g_exe);
        return 2;
    }
    if (!rr_drv_open()) {
        out("[ctx] kernel backend unavailable (map the new kernel-mode.sys)\n");
        return 3;
    }

    n = find_game_threads(g_rr_pid, tids, 64);
    out("[ctx] pid %u, %d thread(s)\n", g_rr_pid, n);
    if (!n) return 4;

    for (int i = 0; i < n && i < 16; i++) {
        RR_THREAD_CTX ctx;
        long st = rr_drv_get_context(tids[i], &ctx);

        if (st == 0 && ctx.ok) {
            got++;
            out("[ctx] tid %-6u rip=%016llX rsp=%016llX rbp=%016llX flags=%08llX\n",
                tids[i], (unsigned long long)ctx.rip, (unsigned long long)ctx.rsp,
                (unsigned long long)ctx.rbp, (unsigned long long)ctx.rflags);
        } else {
            denied++;
            if (!first_fail) first_fail = st;
        }
    }

    out("[ctx] %d context(s) readable, %d not", got, denied);
    if (denied && first_fail)
        out(" (first status %08lX)\n", (unsigned long)first_fail);
    else
        out("\n");

    if (!got) {
        out("[ctx] no context obtainable - a context hijack is not available\n"
            "      on this system, and the write-only feature set stands.\n");
        return 5;
    }
    out("[ctx] contexts are readable; the next step is a stub that restores\n"
        "      the saved registers before resuming.\n");
    return 0;
}

                                                                            
                                                                       
   
// Find game threads.
static int find_game_threads(uint32_t pid, uint32_t* out, int max) {
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
    THREADENTRY32 te;
    int n = 0;

    if (snap == INVALID_HANDLE_VALUE) return 0;
    memset(&te, 0, sizeof(te));
    te.dwSize = sizeof(te);
    if (Thread32First(snap, &te)) {
        do {
            if (te.th32OwnerProcessID != pid) continue;
            if (n < max) out[n++] = te.th32ThreadID;
        } while (Thread32Next(snap, &te));
    }
    CloseHandle(snap);
    return n;
}

                             

                                                                       
                                                                          
                                                                    
   
// Run writetest.
static int run_writetest(void) {
    const size_t N = 64;
    uint8_t pattern[64], back[64];
    uint8_t* buf;
    int pass = 0, fail = 0;

    out("driver write test\n");

    if (!rr_drv_open()) {
        out("FAIL: driver not available (mapped? elevated? session key?)\n");
        return 1;
    }

    buf = (uint8_t*)VirtualAlloc(NULL, N, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    if (!buf) {
        out("FAIL: could not allocate the test buffer\n");
        rr_drv_close();
        return 1;
    }

                                                                               
    g_rr_pid = GetCurrentProcessId();
    out("target pid %u (this process)\n", (unsigned)g_rr_pid);
    out("buffer %p, %u bytes\n", (void*)buf, (unsigned)N);

    for (size_t i = 0; i < N; i++) pattern[i] = (uint8_t)(0xA5u ^ (unsigned)(i * 7u));
    memset(buf, 0, N);

    if (rr_write((uint64_t)(uintptr_t)buf, pattern, N)) {
        pass++;
        out("write: accepted\n");
    } else {
        fail++;
        out("FAIL: rr_write rejected\n");
    }

    memset(back, 0, sizeof(back));
    if (!rr_read((uint64_t)(uintptr_t)buf, back, N)) {
        fail++;
        out("FAIL: read back rejected\n");
    } else if (memcmp(back, pattern, N) != 0) {
        fail++;
        out("FAIL: memory does not hold what was written\n");
    } else {
        pass++;
        out("read back: %u bytes match\n", (unsigned)N);
    }

                                                                            
                                                                             
                                                             
    if (rr_write(0x10, pattern, 8)) {
        fail++;
        out("FAIL: write to 0x10 reported success\n");
    } else {
        pass++;
        out("write to 0x10 correctly refused\n");
    }

                                                                          
                                                                                 
       
    {
        enum { PAGES = 64, PSIZE = 0x1000 };
        const size_t BIG = (size_t)PAGES * PSIZE;
        static uint8_t back2[PAGES * PSIZE];
        uint8_t* big = (uint8_t*)VirtualAlloc(NULL, BIG, MEM_COMMIT | MEM_RESERVE,
                                              PAGE_READWRITE);
        if (!big) {
            fail++;
            out("FAIL: could not allocate the batch buffer\n");
        } else {
            RR_RD_REQ req[PAGES];
            int i, got, match = 1;
            size_t k;
            for (k = 0; k < BIG; k++) big[k] = (uint8_t)(k * 13u + 7u);
            memset(back2, 0, sizeof(back2));
            for (i = 0; i < PAGES; i++) {
                req[i].addr = (uint64_t)(uintptr_t)(big + (size_t)i * PSIZE);
                req[i].size = PSIZE;
                req[i].dst = back2 + (size_t)i * PSIZE;
            }
            got = rr_read_batch(req, PAGES);
            for (k = 0; k < BIG && match; k++) if (back2[k] != big[k]) match = 0;
            if (got == PAGES && match) {
                pass++;
                out("batch read: %d x 4 KB copied and matched (%u KB)\n",
                    PAGES, (unsigned)(BIG >> 10));
            } else {
                fail++;
                out("FAIL: batch read returned %d of %d want, content %s\n",
                    got, PAGES, match ? "ok" : "MISMATCH");
            }
            VirtualFree(big, 0, MEM_RELEASE);
        }
    }

    VirtualFree(buf, 0, MEM_RELEASE);
    g_rr_pid = 0;
    rr_drv_close();

    out("result: %d passed, %d failed\n", pass, fail);
    return fail ? 1 : 0;
}

           

                                                                           
                                                                            
                                     
int run_nodescan(const char* text);
int run_classscan(const char* text);

                                                                                 
                                                                          
static int g_cli_no_pause;

// Handle cli pause.
static void cli_pause(void) {
    if (g_cli_no_pause) return;
    cli_pause();
}

                                                                              
// Run chamdiag.
static int run_chamdiag(int trace) {
    if (!rr_attach(g_exe) || !rr_drv_open()) { out("attach or driver unavailable\n"); return 2; }
    BR_Config* cfg=br_get();
    cfg->enabled=1; cfg->cham_on=1; cfg->show_self=0;
    cfg->vehicle_chams=0; cfg->show_loot=0; cfg->show_loot_nodes=0; cfg->aa_on=0;
    out("initializing live reader\n");
    uint64_t world_hint=0;
    FILE* status=fopen("..\\1_run\\status.log","r");
    if (status) {
        char line[512];
        while (fgets(line,sizeof(line),status)) {
            char* found=strstr(line,"world=");
            if (found) world_hint=_strtoui64(found+6,NULL,16);
        }
        fclose(status);
    }
    if (!game_pose_diag_init(world_hint)) { out("live roster unavailable\n"); return 3; }
    game_pose_diag_enable(); cham_diag_enable();
    FILE* trace_file=trace?fopen("chamtrace.csv","w"):NULL;
    uint64_t trace_target=0;
    if (trace && !trace_file) { out("could not open trace file\n"); return 4; }
    if (trace_file) {
        fprintf(trace_file,"utc_ms,tick_ms,frame,id,player_x,player_y,player_z");
        const char* labels[4]={"head","neck","left","right"};
        for(int b=0;b<4;++b) for(int k=0;k<12;++k) fprintf(trace_file,",%s_%d",labels[b],k);
        fputc('\n',trace_file);
    }
    static RR_PLAYER players[RR_MAX_PLAYERS]; RR_CAMERA camera;
    DWORD start=timeGetTime(), report=start;
    unsigned ticks=0, players_seen=0;
    while (timeGetTime()-start < (trace?90000u:30000u)) {
        int n=game_snapshot(players,RR_MAX_PLAYERS,&camera);
        if (trace_file && !trace_target) {
            int chosen=-1;
            for (int i=0;i<n;++i) if (players[i].valid && !players[i].local && !players[i].is_loot &&
                !players[i].is_vehicle && players[i].cham_valid) {
                if (!_stricmp(players[i].name,"Ranzar")) { chosen=i; break; }
            }
            if (chosen>=0) { trace_target=players[chosen].addr; out("trace target name=%s id=%d\n",players[chosen].name,players[chosen].id); }
        }
        for (int i=0;i<n;++i) {
            if (players[i].valid && !players[i].local && !players[i].is_vehicle && !players[i].is_loot) ++players_seen;
            cham_diag_sample(&players[i]);
            if (trace_file && trace_target && players[i].addr==trace_target && players[i].cham_valid) {
                FILETIME ft; ULARGE_INTEGER utc;
                GetSystemTimeAsFileTime(&ft); utc.LowPart=ft.dwLowDateTime; utc.HighPart=ft.dwHighDateTime;
                unsigned long long utc_ms=(utc.QuadPart-116444736000000000ULL)/10000ULL;
                RR_PLAYER* x=&players[i];
                fprintf(trace_file,"%llu,%lu,%u,%d,%.6f,%.6f,%.6f",utc_ms,(unsigned long)timeGetTime(),ticks,x->id,x->pos.x,x->pos.y,x->pos.z);
                const int bones[4]={4,3,6,17};
                for(int b=0;b<4;++b) for(int k=0;k<12;++k) fprintf(trace_file,",%.7g",x->cham_skin[bones[b]][k]);
                fputc('\n',trace_file);
            }
        }
        ++ticks;
        DWORD now=timeGetTime();
        if (now-report >= 10000) {
            RR_CHAM_DIAG c=cham_diag_results(); RR_POSE_DIAG p=game_pose_diag_results();
            out("elapsed=%lu ticks=%u observed=%u fresh=%u repeated=%u initial=%u state=%u gap=%u jump=%u max_gap_ms=%u max_jump_m=%.3f attempts=%u success=%u failed=%u mesh_reused=%u cham_reused=%u no_mesh=%u\n",
                (unsigned long)(now-start),ticks,players_seen,c.fresh,c.repeated,c.initial,c.state,c.gap,c.jump,c.max_gap_ms,c.max_jump_m,
                p.attempts,p.success,p.failed,p.reused_mesh,p.reused_cham,p.missing_rig);
            out("motion moving=%u reversals=%u bone_jumps=%u max_bone_delta_m=%.3f head_spikes=%u left_spikes=%u right_spikes=%u\n",c.moving_samples,c.reversals,c.bone_jumps,c.max_bone_delta_m,c.head_spikes,c.left_spikes,c.right_spikes);
            out("local_quaternions samples=%u neck_reversals=%u left_reversals=%u right_reversals=%u paired_reads=%u paired_changes=%u\n",p.quat_samples,p.neck_reversals,p.left_reversals,p.right_reversals,p.pair_samples,p.pair_changes);
            out("asset_rest both_arms=%u rest_then_return=%u\n",p.both_arms_rest,p.rest_reversions);
            out("rotation_return_run_lengths 1=%u 2=%u 3=%u 4=%u 5=%u 6=%u\n",p.rotation_run_return[0],p.rotation_run_return[1],p.rotation_run_return[2],p.rotation_run_return[3],p.rotation_run_return[4],p.rotation_run_return[5]);
            out("coordinated_pose_holds=%u\n",p.held_spikes);
            out("standing_samples=%u seated_samples=%u seated_holds=%u seated_root_bypass=%u\n",p.standing_samples,p.seated_samples,p.seated_holds,p.seated_root_bypass);
            out("rotation_masks none=%u head=%u left=%u head_left=%u right=%u head_right=%u arms=%u all=%u\n",p.change_mask[0],p.change_mask[1],p.change_mask[2],p.change_mask[3],p.change_mask[4],p.change_mask[5],p.change_mask[6],p.change_mask[7]);
            out("rig_mapping_joints=%u close_alternatives=%u\n",p.mapping_joints,p.mapping_ambiguous);
            out("rig_named_joints=%u name_mismatches=%u\n",p.named_joints,p.mismatched_joints);
            for (int b=0;b<RR_CHAM_BONES;++b) if (p.ambiguous_by_bone[b]) out("rig_ambiguous bone=%d count=%u\n",b,p.ambiguous_by_bone[b]);
            for (int b=0;b<RR_CHAM_BONES;++b) if (p.mismatch_by_bone[b]) out("rig_mismatch bone=%d count=%u\n",b,p.mismatch_by_bone[b]);
            report=now;
        }
        Sleep(8);
    }
    if (trace_file) fclose(trace_file);
    return 0;
}
// Run application.
int WINAPI WinMain(HINSTANCE hInst, HINSTANCE hPrev, LPSTR lpCmd, int nShow) {
    (void)hPrev; (void)nShow;
    SetUnhandledExceptionFilter(rr_crash_filter);
    int mode_menu = lpCmd && strstr(lpCmd, "--menu");
    int mode_probe = lpCmd && strstr(lpCmd, "--probe");
    int mode_gomdiag = lpCmd && strstr(lpCmd, "--gomdiag");
    int mode_chain = lpCmd && strstr(lpCmd, "--chain");
    int mode_gomlist = lpCmd && strstr(lpCmd, "--gomlist");
    int mode_objlist = lpCmd && strstr(lpCmd, "--objlist");
    int mode_offsets = lpCmd && strstr(lpCmd, "--offsets");
    int mode_chamdiag = lpCmd && strstr(lpCmd, "--chamdiag");
    int mode_chamtrace = lpCmd && strstr(lpCmd, "--chamtrace");
    int mode_test = lpCmd && strstr(lpCmd, "--selftest");
    int mode_input = lpCmd && strstr(lpCmd, "--inputtest");
    int mode_write = lpCmd && strstr(lpCmd, "--writetest");
    int mode_nodescan = lpCmd && strstr(lpCmd, "--nodescan");
    int mode_classscan = lpCmd && strstr(lpCmd, "--classscan");
    int mode_lootdump = lpCmd && strstr(lpCmd, "--lootdump");
    int mode_lootset = lpCmd && strstr(lpCmd, "--lootset");
    int mode_itemset = lpCmd && strstr(lpCmd, "--itemset");
    int mode_cashscan = lpCmd && strstr(lpCmd, "--cashscan");
    int mode_cashfind = lpCmd && strstr(lpCmd, "--cashfind");
    int mode_ctx = lpCmd && strstr(lpCmd, "--ctxread");
    int mode_aimtest = lpCmd && strstr(lpCmd, "--aimtest");
    int mode_xfprobe = lpCmd && strstr(lpCmd, "--xfprobe");
    int mode_demo = lpCmd && strstr(lpCmd, "--demo");
                                                                             
                                                                                
                                                                  
       
                                                                          
                                                                               
                                                                           
       
    int mode_nohide = lpCmd && strstr(lpCmd, "--nohide");
    int mode_hide = !mode_nohide;
                                                                
    g_cli_no_pause = (lpCmd && strstr(lpCmd, "--no-pause")) ? 1 : 0;
    g_demo = mode_demo;
    if (mode_hide && !mode_nohide && !mode_test && !mode_probe && !mode_input &&
        !mode_write && !mode_aimtest && !mode_ctx && !mode_xfprobe)
        (void)rr_hide_my_process();
    {
        const char* e = lpCmd ? strstr(lpCmd, "--exe ") : NULL;
        if (e) {
            e += 6;
            size_t i = 0;
            while (*e && *e != ' ' && i + 1 < sizeof(g_exe)) g_exe[i++] = *e++;
            g_exe[i] = 0;
        }
    }
    {
        const char* p = lpCmd ? strstr(lpCmd, "--pid ") : NULL;
        if (p) {
            unsigned long v = strtoul(p + 6, NULL, 10);
            if (v) rr_force_pid((uint32_t)v);
        }
    }

    if (mode_test) {
        out_open("selftest.log");
        out("larpd1ckOverlay selftest\n");
        int rc = run_selftest();
        if (getenv("RR_PAUSE")) { out("press enter..."); (void)getchar(); }
        out_close();
        return rc;
    }
    if (mode_chamdiag || mode_chamtrace) {
        out_open("chamdiag.log");
        int rc=run_chamdiag(mode_chamtrace);
        out_close();
        return rc;
    }
    if (mode_input) {
        out_open("inputtest.log");
        int secs = 30;
        const char* s = lpCmd ? strstr(lpCmd, "--inputtest") : NULL;
        if (s) { s += 11; while (*s == ' ') s++; if (*s >= '0' && *s <= '9') secs = atoi(s); }
        int rc = run_inputtest(secs);
        out_close();
        return rc;
    }
    if (mode_offsets) {
        out_open("offsets.log");
        out("RobRun offsets probe\n");
        int rc = run_offsets();
        out_close();
        return rc;
    }
    if (mode_objlist) {
        out_open("objlist.log");
        out("RobRun objlist diagnostic\n");
        int rc = run_objlist();
        out_close();
        return rc;
    }
    if (mode_gomlist) {
        out_open("gomlist.log");
        out("RobRun gomlist diagnostic\n");
        int rc = run_gomlist();
        out_close();
        return rc;
    }
    if (mode_chain) {
        out_open("chain.log");
        out("RobRun chain diagnostic\n");
        int rc = run_chaindiag();
        out_close();
        return rc;
    }
    if (mode_gomdiag) {
        out_open("gomdiag.log");
        out("RobRun GOM diagnostic\n");
        int rc = run_gomdiag();
        out_close();
        return rc;
    }
    if (mode_probe) {
        out_open("probe.log");
        out("larpd1ckOverlay probe\n");
        int rc = run_probe();
        cli_pause();
        out_close();
        return rc;
    }
    if (mode_write) {
        out_open("writetest.log");
        int rc = run_writetest();
        cli_pause();
        out_close();
        return rc;
    }
    if (mode_xfprobe) {
        int ms = 2000;
        const char* a = lpCmd ? strstr(lpCmd, "--xfprobe ") : NULL;
        if (a) { int v = atoi(a + 10); if (v > 0) ms = v; }
        out_open("xfprobe.log");
        out("larpd1ckOverlay transform probe\n");
        int rc = run_xfprobe(ms);
        cli_pause();
        out_close();
        return rc;
    }
    if (mode_aimtest) {
        out_open("aimtest.log");
        out("larpd1ckOverlay injected input test\n");
        int rc = run_aimtest();
        cli_pause();
        out_close();
        return rc;
    }
    if (mode_nodescan) {
        out_open("nodescan.log");
        out("larpd1ckOverlay node scan\n");
        const char* a = strstr(lpCmd, "--nodescan ");
        int rc = run_nodescan(a ? a + 11 : "node_cash_1");
        cli_pause();
        out_close();
        return rc;
    }
    if (mode_cashfind) {
        const char* a = lpCmd ? strstr(lpCmd, "--cashfind ") : NULL;
        long long amount = 0;
        if (a) (void)sscanf(a + 11, "%lld", &amount);
        out_open("cashfind.log");
        out("larpd1ckOverlay whole-heap value search");
        int rc = run_cashfind(amount);
        out_close();
        return rc;
    }
    if (mode_cashscan) {
        const char* a = lpCmd ? strstr(lpCmd, "--cashscan ") : NULL;
        long long amount = 0;
        int depth = 3;
        if (a) (void)sscanf(a + 11, "%lld %d", &amount, &depth);
        if (depth < 1) depth = 1;
        if (depth > 5) depth = 5;
        out_open("cashscan.log");
        out("larpd1ckOverlay cash field scan");
        int rc = run_cashscan(amount, depth);
        out_close();
        return rc;
    }
    if (mode_itemset) {
        const char* a = lpCmd ? strstr(lpCmd, "--itemset ") : NULL;
        int value = 0;
        char filt[64];
        memset(filt, 0, sizeof filt);
        strcpy(filt, "cash");
        if (a) (void)sscanf(a + 10, "%d %63s", &value, filt);
        out_open("itemset.log");
        out("larpd1ckOverlay item value write");
        int rc = run_itemset(filt, value);
        out_close();
        return rc;
    }
    if (mode_lootset) {
        const char* a = lpCmd ? strstr(lpCmd, "--lootset ") : NULL;
        int vmin = 0, vmax = 0;
        char filt[64];
        memset(filt, 0, sizeof filt);
        strcpy(filt, "cash");
        if (a) (void)sscanf(a + 10, "%d %d %63s", &vmin, &vmax, filt);
        out_open("lootset.log");
        out("larpd1ckOverlay loot table write\n");
        int rc = run_lootset(filt, vmin, vmax);
        out_close();
        return rc;
    }
    if (mode_lootdump) {
        const char* a = lpCmd ? strstr(lpCmd, "--lootdump ") : NULL;
        const char* f = a ? a + 11 : "";
        out_open("lootdump.log");
        out("larpd1ckOverlay loot object dump\n");
        int rc = run_lootdump(f);
        out_close();
        return rc;
    }
    if (mode_classscan) {
                                                                            
                                                                           
        const char* a = strstr(lpCmd, "--classscan ");
        int rc = run_classscan(a ? a + 12 : "LootNodeModel,InventoryItemDescription");
        return rc;
    }
    if (mode_ctx) {
        out_open("ctxread.log");
        out("larpd1ckOverlay thread context probe\n");
        int rc = run_ctxread();
        cli_pause();
        out_close();
        return rc;
    }

    g_sw = GetSystemMetrics(SM_CXSCREEN);
    g_sh = GetSystemMetrics(SM_CYSCREEN);

    InitializeCriticalSection(&g_cs);
    br_get();                                                                   
    br_config_load();

    if (!mode_menu && !mode_demo) {
        if (!rr_attach(GAME_EXE)) {
            MessageBoxA(NULL, "ROB RUN is not running", "larpd1ck", 0);
            return 1;
        }
        if (!rr_drv_open()) {
            MessageBoxA(NULL, "Kernel backend unavailable - run START.bat to map the driver",
                        "larpd1ck", 0);
            return 2;
        }
                                                                              
                                                                                         
           
        g_show_footer = 1;
        snprintf(g_status, sizeof(g_status), "starting - discovering world...");
    }
    if (mode_demo) {
        snprintf(g_status, sizeof(g_status), "DEMO - synthetic players");
        g_show_footer = 1;
    }
                                                                                  
    rr_friends_init();
    br_set_menu_visible(mode_menu ? 1 : 0);
    br_start_menu();

    char ov_cls[40], ov_title[40];
    ULONGLONG seed = GetTickCount64() ^ ((ULONGLONG)GetCurrentProcessId() << 16);
    snprintf(ov_cls, sizeof(ov_cls), "DwmSync_%08X", (unsigned)(seed ^ (seed >> 32)));
    snprintf(ov_title, sizeof(ov_title), "Sync_%08X", (unsigned)((seed >> 12) ^ seed));

    WNDCLASSA wc;
    memset(&wc, 0, sizeof(wc));
    wc.lpfnWndProc = OverlayProc;
    wc.hInstance = hInst;
    wc.lpszClassName = ov_cls;
    wc.hCursor = LoadCursor(NULL, IDC_ARROW);
    RegisterClassA(&wc);

    g_hwnd = CreateWindowExA(
        WS_EX_NOREDIRECTIONBITMAP | WS_EX_LAYERED | WS_EX_TRANSPARENT | WS_EX_NOACTIVATE |
        WS_EX_TOPMOST | WS_EX_TOOLWINDOW,
        ov_cls, ov_title, WS_POPUP | WS_VISIBLE, 0, 0, g_sw, g_sh,
        NULL, NULL, hInst, NULL);
    if (!g_hwnd) return 4;

    SetLayeredWindowAttributes(g_hwnd, 0, 255, LWA_ALPHA);
    if (br_get()->streamproof) SetWindowDisplayAffinity(g_hwnd, 0x00000011);

                                                                             
                                                                               
                                                                           
                                                                 
    g_hk_menu = RegisterHotKey(g_hwnd, 1, MOD_NOREPEAT, VK_INSERT) ? 1 : 0;
    g_hk_profile = RegisterHotKey(g_hwnd, 2, MOD_NOREPEAT, VK_F6) ? 1 : 0;
    g_hk_quit = RegisterHotKey(g_hwnd, 3, MOD_CONTROL | MOD_NOREPEAT, VK_END) ? 1 : 0;
    g_kb_hook = SetWindowsHookExW(WH_KEYBOARD_LL, kb_hook_proc, GetModuleHandleW(NULL), 0);
    if (!mode_menu && !mode_demo) { sync_game_window(); SetTimer(g_hwnd, 18, 500, NULL); }
    int raw_ok = raw_input_on();
    hklogf("init menu=%d profile=%d END=%d llhook=%d rawinput=%d",
           g_hk_menu, g_hk_profile, g_hk_quit, g_kb_hook ? 1 : 0, raw_ok);

    if (!mode_menu && !mode_demo){rr_los_start();rr_visibility_start();rr_shot_start();}
    g_game_frame_event=CreateEventW(NULL,FALSE,FALSE,NULL);
    g_scene_event=CreateEventW(NULL,FALSE,FALSE,NULL);
    if (!mode_menu) CreateThread(NULL, 0, ReadThread, NULL, 0, NULL);
    if (!mode_menu) CreateThread(NULL, 0, RenderThread, NULL, 0, NULL);
    if (!mode_menu && !mode_demo) g_camera_thread=CreateThread(NULL, 0, CameraThread, NULL, 0, NULL);

    MSG msg;int done=0;DWORD last_draw=0;
    while(!done){
        int handled=0;
        while(handled++<32 && PeekMessage(&msg,NULL,0,0,PM_REMOVE)){
            if(msg.message==WM_QUIT){done=1;break;}
            TranslateMessage(&msg);DispatchMessage(&msg);
        }
        if(done)break;
        if(mode_menu){MsgWaitForMultipleObjectsEx(0,NULL,INFINITE,QS_ALLINPUT,MWMO_INPUTAVAILABLE);continue;}
        static int last_ov_sp=-1;
        int cur_sp=br_get()->streamproof;
        if(last_ov_sp!=cur_sp){last_ov_sp=cur_sp;SetWindowDisplayAffinity(g_hwnd,cur_sp?0x00000011u:0x00000000u);}
        DWORD now=timeGetTime();unsigned sequence;int entities;
        EnterCriticalSection(&g_cs);sequence=g_snapshot_seq;entities=g_nbuf[g_front];LeaveCriticalSection(&g_cs);
        AcquireSRWLockShared(&g_camera_lock);
        int pending_camera=g_fast_camera.render_state &&
            (g_fast_camera.model_source!=g_painted_camera_model || g_fast_camera.frame_number!=g_painted_camera_frame);
        DWORD camera_age=now-g_camera_frame_at;
        ReleaseSRWLockShared(&g_camera_lock);
        int radar_drag_active=br_get()->radar_on && ((GetAsyncKeyState(VK_LBUTTON) & 0x8000) != 0);
        int dirty=!gpu_ready() || sequence!=g_painted_seq ||
            (pending_camera && camera_age>=1) ||
            ((br_get()->menu_open && br_get()->menu_particles) || radar_drag_active ? (now-last_draw>=16) : 0) ||
            ((br_get()->menu_open || !entities) && now-last_draw>=50);
        if(dirty && gpu_frame_ready()){
            InvalidateRect(g_hwnd,NULL,FALSE);UpdateWindow(g_hwnd);last_draw=timeGetTime();continue;
        }
        HANDLE waits[2];DWORD count=0,frame_index=MAXDWORD;
        HANDLE frame=dirty?gpu_frame_handle():NULL;
        if(frame){frame_index=count;waits[count++]=frame;}
        if(g_scene_event)waits[count++]=g_scene_event;
        DWORD timeout=pending_camera?1:(entities||(br_get()->menu_open&&br_get()->menu_particles)||radar_drag_active)?16:50;
        if(dirty && !frame)timeout=1;
        DWORD wait=MsgWaitForMultipleObjectsEx(count,waits,timeout,QS_ALLINPUT,MWMO_INPUTAVAILABLE);
        if(frame && wait==WAIT_OBJECT_0+frame_index)gpu_grant_frame();
    }

    InterlockedExchange(&g_quit, 1);
    if(g_camera_thread){WaitForSingleObject(g_camera_thread,5000);CloseHandle(g_camera_thread);g_camera_thread=NULL;}
    br_stop_menu();
    br_config_save();
    rr_shot_stop();
    rr_friends_stop();
    rr_visibility_stop();
    rr_los_stop();
                                                                         
                                                                             
       
    rr_drv_close();
    return 0;
}

















































