                                                                           
  
                                                                               
                                                                           
                                                                           
                                                                               
   
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <stdio.h>
#include <string.h>
#include <math.h>
#include <ctype.h>
#include <stdlib.h>

#include "gpu.h"
#include "menu_bridge.h"
#include "rr_game.h"
#include "rr_draw.h"
#include "rr_shot.h"
#include "rr_cham_model.h"
#include "rr_body_joints.h"
#include "rr_vehicle_cham_model.h"
#include "rr_features.h"
#include "rr_los.h"
#include "rr_aim.h"
#include "rr_label_layout.h"
#include "rr_friends.h"
#include "rr_visibility.h"

typedef struct {float x,y,w,h;} LABEL_RECT;
static LABEL_RECT g_labels[RR_MAX_PLAYERS];
static int g_label_count;
static RGBA slot_color(int i);
static RGBA player_color(const RR_PLAYER* p);
static RGBA player_style_color(const RR_PLAYER* p,const BR_Config* b,int visibility,int first,RGBA fallback);

// Handle player alpha.
static float player_alpha(const RR_PLAYER* p,const BR_Config* b){
    return p->visual_alpha*(float)b->esp_opacity/100;
}
// Handle label place.
static int label_place(float* x,float* y,float w,float h,const BR_Config* b,RR_VIEWPORT v){
    *x=fmaxf(v.x+3,fminf(*x,v.x+v.width-w-3));
    *y=fmaxf(v.y+3,fminf(*y,v.y+v.height-h-3));
    if(g_label_count>=b->label_limit)return !b->declutter_hide;
    if(b->declutter){
        int found=0;
        float original=*y;
        for(int attempt=0;attempt<7;++attempt){
            float candidate=original+(attempt?((attempt+1)/2)*(attempt%2?1.0f:-1.0f)*(h+4):0);
            if(candidate<v.y || candidate+h>v.y+v.height)continue;
            int conflict=0;
            for(int j=0;j<g_label_count;++j){const LABEL_RECT* r=&g_labels[j];
                if(*x<r->x+r->w+3 && *x+w+3>r->x && candidate<r->y+r->h+3 && candidate+h+3>r->y){conflict=1;break;}}
            if(!conflict){*y=candidate;found=1;break;}
        }
        if(!found && b->declutter_hide)return 0;
    }
    if(g_label_count<RR_MAX_PLAYERS)g_labels[g_label_count++]=(LABEL_RECT){*x,*y,w,h};
    return 1;
}

static float offscreen_ring_radius(RR_VIEWPORT v, const BR_Config* b) {
    float size = (float)(b->offscreen_size > 0 ? b->offscreen_size : 12);
    float fov_r = 0.0f;
    if (b->aa_on && b->aa_fov_px > 0) fov_r = (float)b->aa_fov_px;
    else if (b->silent_aim_on && b->silent_aim_fov > 0) fov_r = (float)b->silent_aim_fov;
    if (fov_r <= 0.0f) fov_r = 120.0f;
    float extra = (float)(b->offscreen_radius >= 0 ? b->offscreen_radius : 0);
    float ring = fov_r + size * 0.55f + extra;
    float max_r = fminf(v.width, v.height) * 0.5f - (size + 24.0f);
    if (max_r < 30.0f) max_r = 30.0f;
    if (ring > max_r) ring = max_r;
    return ring;
}

// Handle offscreen arrow.
static void offscreen_arrow(const RR_PLAYER* p,const RR_CAMERA* cam,RR_VIEWPORT v,const BR_Config* b,int projected,float sx,float sy){
    float cx=v.x+v.width*.5f, cy=v.y+v.height*.5f;
    float ux, uy;
    if (projected && isfinite(sx) && isfinite(sy) && hypotf(sx - cx, sy - cy) > 1.0f) {
        float dl = hypotf(sx - cx, sy - cy);
        ux = (sx - cx) / dl;
        uy = (sy - cy) / dl;
    } else {
        float dx=p->pos.x-cam->pos.x,dy=p->pos.y-cam->pos.y,dz=p->pos.z-cam->pos.z;
        float rx=cam->right.x,ry=cam->right.y,rz=cam->right.z;
        if(rx*rx+ry*ry+rz*rz<.5f){rx=cam->fwd.z;ry=0;rz=-cam->fwd.x;}
        float lateral=dx*rx+dy*ry+dz*rz;
        float forward=dx*cam->fwd.x+dy*cam->fwd.y+dz*cam->fwd.z;
        float angle=atan2f(lateral,forward);
        ux=sinf(angle);uy=-cosf(angle);
    }
    float x, y;
    if (b->offscreen_fov_ring) {
        float ring = offscreen_ring_radius(v, b);
        x = cx + ux * ring;
        y = cy + uy * ring;
    } else {
        float halfw=fmaxf(20,v.width*.5f-34),halfh=fmaxf(20,v.height*.5f-34);
        float scale=1/fmaxf(fabsf(ux)/halfw,fabsf(uy)/halfh);
        x=cx+ux*scale;y=cy+uy*scale;
    }
    RGBA col = b->offscreen_color_mode == 2 ? slot_color(35) :
               b->offscreen_color_mode == 1 ? player_style_color(p, b, 1, BR_C_BOX_VISIBLE, player_color(p)) :
               player_color(p);
    float size=(float)(b->offscreen_size > 0 ? b->offscreen_size : 12);
    float px = -uy, py = ux;
    /* Outer soft glow chevron */
    float gs = size * 1.28f;
    float gverts[8] = {
        x + ux * gs, y + uy * gs,
        x - ux * gs * 0.55f + px * gs * 0.72f, y - uy * gs * 0.55f + py * gs * 0.72f,
        x - ux * gs * 0.20f, y - uy * gs * 0.20f,
        x - ux * gs * 0.55f - px * gs * 0.72f, y - uy * gs * 0.55f - py * gs * 0.72f
    };
    static const unsigned short cidx[6] = { 0, 1, 2, 0, 2, 3 };
    r_fill_triangles(gverts, 4, cidx, 2, (col & 0x00FFFFFFu) | 0x3A000000u);

    /* Main sleek notched chevron */
    float tipx = x + ux * size, tipy = y + uy * size;
    float lx = x - ux * size * 0.55f + px * size * 0.72f, ly = y - uy * size * 0.55f + py * size * 0.72f;
    float nx = x - ux * size * 0.20f, ny = y - uy * size * 0.20f;
    float rx2 = x - ux * size * 0.55f - px * size * 0.72f, ry2 = y - uy * size * 0.55f - py * size * 0.72f;
    float verts[8] = { tipx, tipy, lx, ly, nx, ny, rx2, ry2 };
    r_fill_triangles(verts, 4, cidx, 2, (col & 0x00FFFFFFu) | 0xCC000000u);
    gpu_mesh_flush();

    /* Crisp dark outer rim + bright inner edge */
    float poly[] = { tipx, tipy, lx, ly, nx, ny, rx2, ry2, tipx, tipy };
    r_polyline(poly, 5, RARGB(200, 10, 12, 18), 2.6f);
    r_polyline(poly, 5, (col & 0x00FFFFFFu) | 0xFF000000u, 1.3f);

    char distance[24]; snprintf(distance, sizeof(distance), "%.0fm", p->dist);
    gpu_font(fmaxf(10.0f, (float)b->text_size - 1.0f));
    float tdir = b->offscreen_fov_ring ? 1.0f : -1.0f;
    r_text(x + ux * tdir * (size + 11.0f), y + uy * tdir * (size + 11.0f), distance, (col & 0x00FFFFFFu) | 0xF0000000u, RT_CENTER | RT_OUTLINE);
}

// Handle slot color.
static RGBA slot_color(int i) {
    BR_Config* b = br_get();
    if (i < 0 || i >= BR_COLOR_COUNT) return RRGB(255, 255, 255);
    const unsigned char* c = b->color[i].rgb;
    return RARGB(255, c[0], c[1], c[2]);
}

                                                                                 
// Handle role color.
static RGBA role_color(const char* role) {
    if (strstr(role, "cop")) return slot_color(0);
    if (strstr(role, "robber")) return slot_color(1);
    if (strstr(role, "hubber") || strstr(role, "civilian")) return slot_color(26);
    if (strstr(role, "citizen")) return slot_color(27);
    return 0;
}

// Distinguish visible, occluded and unavailable states.
static int visibility_palette(const RR_PLAYER* p){
    for(int i=0;i<3;++i)if(p->line_of_sight[i]==RR_LOS_CLEAR)return 0;
    for(int i=0;i<3;++i)if(p->line_of_sight[i]!=RR_LOS_BLOCKED)return 2;
    return 1;
}
// Handle player color.
static RGBA player_color(const RR_PLAYER* p) {
    if(p->is_friend)return slot_color(br_get()->friend_visibility?BR_C_FRIEND_ESP_VISIBLE+visibility_palette(p):BR_C_FRIEND_ESP);
    if (p->relationships & RR_REL_CLAN) return slot_color(28);
    if (p->relationships & RR_REL_POLICE) return slot_color(3);
    if (p->dead) return slot_color(5);
    RGBA color = role_color(p->role);
    return color ? color : slot_color(0);
}

// Select a component palette while keeping friends recognizable.
static RGBA player_style_color(const RR_PLAYER* p,const BR_Config* b,int visibility,int first,RGBA fallback){
    if(p->is_friend)return player_color(p);
    return visibility?slot_color(first+visibility_palette(p)):fallback;
}

// Handle corner box.
static void corner_box(float x, float y, float w, float h, RGBA col, float stroke) {
    float cx = w * 0.28f;
    float cy = h * 0.28f;
    if (cx < 3) cx = 3;
    if (cy < 3) cy = 3;
    float l = x, t = y, r = x + w, b = y + h;
    gpu_pen(col, stroke);
    r_line(l, t, l + cx, t);
    r_line(l, t, l, t + cy);
    r_line(r, t, r - cx, t);
    r_line(r, t, r, t + cy);
    r_line(l, b, l + cx, b);
    r_line(l, b, l, b - cy);
    r_line(r, b, r - cx, b);
    r_line(r, b, r, b - cy);
}

typedef struct {
    uint64_t object, rig;
    int32_t id;
    uint32_t sampled_at;
    uint32_t last_used;
    int crouch, dead, inveh, full_rig;
    RR_V3 root;
    RR_V3 diag_delta, diag_bone;
    int diag_have_delta, diag_have_bone;
    RR_V3 diag_shape[3], diag_shape_older[3];
    int diag_shape_count;
    int vertices_valid;
    RR_V3 vertices[RR_CHAM_VERTICES];
    RR_V3 anchors[RR_MAX_BONES + 2];                                       
    float skin[RR_CHAM_BONES][12];
} CHAM_VISUAL_POSE;
static int g_cham_diag_enabled;
static RR_CHAM_DIAG g_cham_diag;
// Handle cham diag enable.
void cham_diag_enable(void) { memset(&g_cham_diag,0,sizeof(g_cham_diag)); g_cham_diag_enabled=1; }
// Handle cham diag results.
RR_CHAM_DIAG cham_diag_results(void) { return g_cham_diag; }

// Handle cham visual pose.
static CHAM_VISUAL_POSE* cham_visual_pose(const RR_PLAYER* p) {
    static CHAM_VISUAL_POSE poses[RR_MAX_PLAYERS];
    static uint32_t serial;
    ++serial;
    CHAM_VISUAL_POSE* previous = &poses[0];
    for (int i = 0; i < RR_MAX_PLAYERS; ++i) {
        CHAM_VISUAL_POSE* candidate = &poses[i];
        if (candidate->object == p->addr && candidate->id == p->id && candidate->rig == p->cham_rig) {
            previous = candidate;
            break;
        }
        if (serial - candidate->last_used > serial - previous->last_used) previous = candidate;
    }
    previous->last_used = serial;
    int same = previous->object == p->addr && previous->id == p->id && previous->rig == p->cham_rig &&
               previous->crouch == p->crouch && previous->dead == p->dead && previous->inveh == p->inveh &&
               previous->full_rig == p->full_rig;
    uint32_t elapsed = p->cham_sample_at - previous->sampled_at;
    if (same && elapsed == 0) {
        if (g_cham_diag_enabled) ++g_cham_diag.repeated;
        return previous;
    }

    previous->vertices_valid=0;
    const float* first_bone = p->cham_skin[0];
                                                                         
                                                                                 
    RR_V3 root = p->mesh_bones ? p->pos :
        (RR_V3){first_bone[3], first_bone[7], first_bone[11]};
    float dx = root.x - previous->root.x;
    float dy = root.y - previous->root.y;
    float dz = root.z - previous->root.z;
    int reset = !same || elapsed >= 100 || dx*dx + dy*dy + dz*dz > 0.64f;
    if (g_cham_diag_enabled) {
        ++g_cham_diag.fresh;
        int identity = previous->object == p->addr && previous->id == p->id && previous->rig == p->cham_rig;
        if (!identity) ++g_cham_diag.initial;
        else {
            if (!same) ++g_cham_diag.state;
            if (elapsed >= 100) ++g_cham_diag.gap;
            if (dx*dx+dy*dy+dz*dz > 0.64f) ++g_cham_diag.jump;
            if (elapsed > g_cham_diag.max_gap_ms) g_cham_diag.max_gap_ms=elapsed;
            float jump=sqrtf(dx*dx+dy*dy+dz*dz);
            if (jump > g_cham_diag.max_jump_m) g_cham_diag.max_jump_m=jump;
            RR_V3 bone={p->cham_skin[RR_RIG_M_HEAD_1_JNT][3]-root.x,p->cham_skin[RR_RIG_M_HEAD_1_JNT][7]-root.y,p->cham_skin[RR_RIG_M_HEAD_1_JNT][11]-root.z};
            if (same && elapsed>0 && elapsed<100) {
                if (jump>.01f) {
                    ++g_cham_diag.moving_samples;
                    float prev_len=sqrtf(previous->diag_delta.x*previous->diag_delta.x + previous->diag_delta.y*previous->diag_delta.y + previous->diag_delta.z*previous->diag_delta.z);
                    float dot=dx*previous->diag_delta.x+dy*previous->diag_delta.y+dz*previous->diag_delta.z;
                    if (previous->diag_have_delta && prev_len>.01f && dot<-.5f*jump*prev_len) ++g_cham_diag.reversals;
                }
                if (previous->diag_have_bone) {
                    float bx=bone.x-previous->diag_bone.x, by=bone.y-previous->diag_bone.y, bz=bone.z-previous->diag_bone.z;
                    float delta=sqrtf(bx*bx+by*by+bz*bz);
                    if (delta>.15f) ++g_cham_diag.bone_jumps;
                    if (delta>g_cham_diag.max_bone_delta_m) g_cham_diag.max_bone_delta_m=delta;
                }
                const int indices[3]={RR_RIG_M_HEAD_1_JNT,RR_RIG_L_ARM_2_JNT,RR_RIG_R_ARM_2_JNT};
                for (int q=0;q<3;++q) {
                    const float* mat=p->cham_skin[indices[q]];
                    RR_V3 now_shape={mat[3]-root.x,mat[7]-root.y,mat[11]-root.z};
                    RR_V3 last=previous->diag_shape[q], older=previous->diag_shape_older[q];
                    float dx1=now_shape.x-last.x,dy1=now_shape.y-last.y,dz1=now_shape.z-last.z;
                    float dx2=now_shape.x-older.x,dy2=now_shape.y-older.y,dz2=now_shape.z-older.z;
                    if (previous->diag_shape_count>=2 && dx1*dx1+dy1*dy1+dz1*dz1>.0225f && dx2*dx2+dy2*dy2+dz2*dz2<.0064f) {
                        if (q==0) ++g_cham_diag.head_spikes;
                        else if (q==1) ++g_cham_diag.left_spikes;
                        else ++g_cham_diag.right_spikes;
                    }
                    previous->diag_shape_older[q]=last;
                    previous->diag_shape[q]=now_shape;
                }
                if (previous->diag_shape_count<2) ++previous->diag_shape_count;
            } else {
                previous->diag_shape_count=0;
            }
            previous->diag_delta=(RR_V3){dx,dy,dz}; previous->diag_have_delta=same && elapsed>0 && elapsed<100;
            previous->diag_bone=bone; previous->diag_have_bone=1;
        }
        if (!identity) { previous->diag_have_delta=0; previous->diag_have_bone=0; previous->diag_shape_count=0; }
    }
                                                                          
                                                                             
    float blend=1.0f;
    memcpy(previous->skin,p->cham_skin,sizeof(previous->skin));
                                                                          
                                                                            
    for (int i = 0; i < RR_MAX_BONES + 2; ++i) {
        if (p->full_rig && i>=2) {
            int bone=i-2;
            if(!isfinite(p->bone[bone][0]) || !isfinite(p->bone[bone][1]) || !isfinite(p->bone[bone][2])) {
                previous->anchors[i]=(RR_V3){NAN,NAN,NAN};continue;
            }
            const float* m=previous->skin[bone];
            const float* a=rr_cham_bones[bone].anchor;
            previous->anchors[i]=(RR_V3){
                m[0]*a[0]+m[1]*a[1]+m[2]*a[2]+m[3],
                m[4]*a[0]+m[5]*a[1]+m[6]*a[2]+m[7],
                m[8]*a[0]+m[9]*a[1]+m[10]*a[2]+m[11]};
            continue;
        }
        RR_V3 live;
        if (i == 0) live = p->pos;
        else if (i == 1) live = p->head;
        else memcpy(&live, p->bone[i - 2], sizeof(live));
        RR_V3* anchor = &previous->anchors[i];
        if (reset) *anchor = live;
        else {
            anchor->x += dx + (live.x - anchor->x - dx) * blend;
            anchor->y += dy + (live.y - anchor->y - dy) * blend;
            anchor->z += dz + (live.z - anchor->z - dz) * blend;
        }
    }
    previous->object = p->addr;
    previous->id = p->id;
    previous->rig = p->cham_rig;
    previous->sampled_at = p->cham_sample_at;
    previous->crouch = p->crouch;
    previous->dead = p->dead;
    previous->inveh = p->inveh;
    previous->full_rig = p->full_rig;
    previous->root = root;
    return previous;
}
// Handle cham diag sample.
void cham_diag_sample(const RR_PLAYER* player) {
    if (player && player->valid && !player->local && !player->is_vehicle && !player->is_loot &&
        player->cham_valid && player->cham_rig && player->cham_sample_at) (void)cham_visual_pose(player);
}

                                                                             
                                                                              
// Handle styled box.
static void styled_box(const RR_PLAYER* p,const RR_CAMERA* cam,int vw,int vh,
                       float x,float y,float w,float h,RGBA color,const BR_Config* b){
    int override=strstr(p->role,"cop")?b->box_police:strstr(p->role,"robber")?b->box_robber:b->box_civilian;
    int style=override?override-1:b->box_style;float stroke=b->box_width/10.0f;
    if(style==0)corner_box(x,y,w,h,color,stroke);
    else if(style==1)r_rect(x,y,w,h,color,stroke);
    else if(style==2)r_round_rect(x,y,w,h,6,color,stroke);
    else {
        RR_V3 corners[8];float xy[16];float yaw=p->rot_yaw*3.14159265f/180;
        float c=cosf(yaw),s=sinf(yaw),radius=p->inveh?.65f:.36f;
        for(int i=0;i<8;++i){float a=(i&1)?radius:-radius,z=(i&2)?radius:-radius;
            corners[i]=(RR_V3){p->pos.x+a*c+z*s,(i&4)?p->head.y+.15f:p->pos.y,p->pos.z-a*s+z*c};}
        game_w2s_many(cam,corners,8,vw,vh,xy);
        for(int i=0;i<8;++i)for(int axis=1;axis<=4;axis*=2)if(!(i&axis)){
            int j=i|axis;if(isfinite(xy[i*2])&&isfinite(xy[i*2+1])&&isfinite(xy[j*2])&&isfinite(xy[j*2+1]))
                r_line_c(xy[i*2],xy[i*2+1],xy[j*2],xy[j*2+1],color,stroke);
        }
    }
}

// Handle player labels.
static void player_labels(const RR_PLAYER* p,const BR_Config* b,RR_VIEWPORT v,
                           float x,float y,float w,float h,int compact,RGBA col){
    typedef struct{char text[96];RGBA color;int side,icon,kind,free,anchor;float cx,top,width,height;} LINE;
    enum{ROLE,NAME,DISTANCE,WEAPON,HEALTH,ARMOR,CROUCH,HIDING,VEHICLE,DOWN};
    typedef struct{int free,x,y;} POSITION;
#define LABEL_POSITION(N) {b->esp_##N##_free,b->esp_##N##_x,b->esp_##N##_y}
    const POSITION positions[]={LABEL_POSITION(role),LABEL_POSITION(name),LABEL_POSITION(distance),
        LABEL_POSITION(weapon),LABEL_POSITION(health),LABEL_POSITION(armor),LABEL_POSITION(crouch),
        LABEL_POSITION(hiding),LABEL_POSITION(vehicle),LABEL_POSITION(down)};
#undef LABEL_POSITION
    LINE lines[10];int count=0,side=b->label_side;
#define ADD_LABEL(K,C,...) do{if(count<10){snprintf(lines[count].text,sizeof(lines[count].text),__VA_ARGS__);lines[count].kind=K;lines[count].icon=0;lines[count].side=side;lines[count++].color=C;}}while(0)
    const char* role=p->is_friend?"FRIEND":(p->relationships&RR_REL_CLAN)?"CLAN MEMBER":(p->relationships&RR_REL_POLICE)?"POLICE COWORKER":
                     strstr(p->role,"cop")?"COP":strstr(p->role,"robber")?"ROBBER":p->role[0]?"CIVILIAN":"";
    side=b->role_side?b->role_side-1:b->label_side;
    if(!compact && b->show_name && b->label_order!=1 && *role)ADD_LABEL(ROLE,col,"%s",role);
    side=b->name_side?b->name_side-1:b->label_side;
    if(b->show_name)ADD_LABEL(NAME,slot_color(6),"%s",p->local?"YOU":p->name[0]?p->name:"PLAYER");
    side=b->role_side?b->role_side-1:b->label_side;
    if(!compact && b->show_name && b->label_order==1 && *role)ADD_LABEL(ROLE,col,"%s",role);
    side=b->distance_side?b->distance_side-1:b->label_side;
    if(b->show_distance)ADD_LABEL(DISTANCE,slot_color(8),"%.0f m",p->dist);
    if(!compact){
        side=b->weapon_side?b->weapon_side-1:b->label_side;
        if(b->show_weapon && p->weapon[0]){
            ADD_LABEL(WEAPON,slot_color(7),"%s",p->weapon);
            if(b->item_icons && count)lines[count-1].icon=rr_item_icon(p->weapon,0);
        }
        side=b->label_side;
        if(b->show_health && p->health>=0)ADD_LABEL(HEALTH,slot_color(6),"HP: %d",p->health);
        if(b->show_armor && p->armor==1)ADD_LABEL(ARMOR,slot_color(9),"ARMOR");
        if(b->show_crouch && p->crouch==1)ADD_LABEL(CROUCH,slot_color(6),"CROUCHING");
        if(p->hidden_in_object)ADD_LABEL(HIDING,col,"%s",p->hidden_in_object==2?"POSSIBLY HIDING":"HIDING");
        if(p->inveh)ADD_LABEL(VEHICLE,col,"IN %s",p->veh[0]?p->veh:"VEHICLE");
        if(p->dead)ADD_LABEL(DOWN,slot_color(5),"DOWN");
    }
#undef ADD_LABEL
    if(!count)return;
    for(int i=0;i<count;++i)lines[i].color=player_style_color(p,b,b->text_visibility,BR_C_TEXT_VISIBLE,lines[i].color);
    if(b->label_order==2)for(int i=0;i<count/2;++i){LINE temp=lines[i];lines[i]=lines[count-1-i];lines[count-1-i]=temp;}
    gpu_font((float)b->text_size);
    for(int i=0;i<count;++i){LINE* line=&lines[i];
        line->free=positions[line->kind].free;
        line->width=line->icon?(float)b->item_icon_size:r_text_width(line->text)+6;
        line->height=line->icon?(float)b->item_icon_size:b->text_size*1.35f;
    }
    for(int group=0;group<4;++group){
        float width=0,height=0;int members=0;
        for(int i=0;i<count;++i)if(!lines[i].free && lines[i].side==group){
            width=fmaxf(width,lines[i].width);height+=lines[i].height+b->label_gap;++members;}
        if(!members)continue;
        float l=x+w*.5f-width*.5f,t=y-height-4;
        if(group==1)t=y+h+4;
        if(group==2){l=x-width-6;t=y;}
        if(group==3){l=x+w+6;t=y;}
        l=fmaxf(v.x+3,fminf(l,v.x+v.width-width-3));
        t=fmaxf(v.y+3,fminf(t,v.y+v.height-height-3));
        for(int i=0;i<count;++i)if(!lines[i].free && lines[i].side==group){
            lines[i].cx=l+width*.5f;lines[i].top=t;lines[i].anchor=group;
            t+=lines[i].height+b->label_gap;
        }
    }
    int placed[10],used=0,pending[10],remaining=0;
    for(int i=0;i<count;++i){LINE* line=&lines[i];
        if(!line->free){placed[used++]=i;continue;}
        const POSITION* pos=&positions[line->kind];
        line->anchor=rr_label_project(x,y,w,h,pos->x,pos->y,line->width,line->height,&line->cx,&line->top);
        pending[remaining++]=i;
    }
    while(remaining){
        int nearest=0;float best=1e30f;
        for(int j=0;j<remaining;++j){const LINE* line=&lines[pending[j]];
            float gap=line->anchor==0?fabsf(y-line->top-line->height):line->anchor==1?fabsf(line->top-y-h):fabsf(line->top-y);
            if(gap<best){best=gap;nearest=j;}}
        int index=pending[nearest];pending[nearest]=pending[--remaining];LINE* line=&lines[index];
        for(int attempt=0;attempt<10;++attempt){int collision=0;
            for(int j=0;j<used;++j){const LINE* other=&lines[placed[j]];
                if(fabsf(line->cx-other->cx)>=(line->width+other->width)*.5f+2 ||
                   line->top>=other->top+other->height+2 || line->top+line->height+2<=other->top)continue;
                int up=line->anchor==0 || (line->anchor>=2 && positions[line->kind].y<500);
                line->top=up?other->top-line->height-2:other->top+other->height+2;collision=1;
            }
            if(!collision)break;
        }
        placed[used++]=index;
    }
    for(int i=0;i<count;++i){const LINE* line=&lines[i];
        if(!line->icon || !r_icon(line->icon,line->cx-b->item_icon_size*.5f,line->top,(float)b->item_icon_size))
            r_text(line->cx,line->top,line->text,line->color,RT_CENTER|RT_OUTLINE);
    }
}

typedef struct{unsigned short a,b;int face;} CHAM_EDGE;
// Handle edge compare.
static int edge_compare(const void* a,const void* b){
    const CHAM_EDGE* x=a;const CHAM_EDGE* y=b;
    return x->a!=y->a?(int)x->a-y->a:(int)x->b-y->b;
}
// Handle vertex part.
static int vertex_part(int vertex){
    int bone=rr_cham_vertices[vertex].bone[0];
    return rr_aim_bone_in_group(RR_AIM_HEAD,bone)?1:
           rr_aim_bone_in_group(RR_AIM_LEGS,bone)||rr_aim_bone_in_group(RR_AIM_FEET,bone)?4:2;
}
// Apply character model morphing and animation to cham vertices.
static void apply_cham_model_and_anim(const BR_Config* b,const RR_PLAYER* p,const RR_V3* src,RR_V3* dst){
    float t=(float)(GetTickCount64()%3600000ull)*0.001f;
    float sx=1.0f,sy=1.0f,sz=1.0f,dy=0.0f,dx=0.0f,dz=0.0f,rot_y=0.0f;
    if(b->cham_model==1){sx=1.18f;sy=1.03f;sz=1.18f;}       /* Juggernaut */
    else if(b->cham_model==2){sx=0.90f;sy=1.08f;sz=0.90f;dy=0.07f;} /* Cyber Phantom */
    else if(b->cham_model==3){sx=0.91f;sy=0.98f;sz=0.91f;}  /* Slim Tactical */
    else if(b->cham_model==4){sx=1.28f;sy=1.12f;sz=1.28f;}  /* Titan */
    else if(b->cham_model==5){sx=0.86f;sy=0.94f;sz=0.86f;}  /* Shadow Assassin */

    if(b->cham_anim==1){                                    /* Breathing */
        float breath=0.045f*sinf(t*2.6f);
        sx+=breath;sz+=breath;dy+=0.015f*sinf(t*2.6f);
    }else if(b->cham_anim==2){                              /* Hover */
        dy+=0.11f*sinf(t*2.4f);
    }else if(b->cham_anim==3){                              /* Pulse Aura */
        float pulse=0.07f*(0.5f+0.5f*sinf(t*4.5f));
        sx+=pulse;sy+=pulse*0.4f;sz+=pulse;
    }else if(b->cham_anim==5){                              /* Combat Sway */
        dx+=0.07f*sinf(t*3.0f);
        dy+=0.03f*fabsf(sinf(t*3.0f));
    }else if(b->cham_anim==6){                              /* Spin */
        rot_y=t*2.2f;
    }
    float cr=cosf(rot_y),sr=sinf(rot_y);
    for(int i=0;i<RR_CHAM_VERTICES;++i){
        if(!isfinite(src[i].x)){dst[i]=src[i];continue;}
        float rx=(src[i].x-p->pos.x)*sx,ry=(src[i].y-p->pos.y)*sy,rz=(src[i].z-p->pos.z)*sz;
        float part_scale=(b->cham_model==1 && vertex_part(i)==1)?1.06f:1.0f;
        rx*=part_scale;rz*=part_scale;
        if(b->cham_anim==6){
            float nx=rx*cr-rz*sr, nz=rx*sr+rz*cr;
            rx=nx;rz=nz;
        }
        dst[i].x=p->pos.x+rx+dx;
        dst[i].y=p->pos.y+ry+dy;
        dst[i].z=p->pos.z+rz+dz;
    }
}

// Draw orbiting energy ring animation around the player.
static void draw_orbit_anim(const BR_Config* b,const RR_PLAYER* p,const RR_CAMERA* cam,int vw,int vh,RGBA col){
    if(b->cham_anim!=4)return;
    float t=(float)(GetTickCount64()%3600000ull)*0.0028f;
    RR_V3 ring[17];float xy[34];
    float rad=0.58f,cy=p->pos.y+0.95f+0.12f*sinf(t*1.4f);
    for(int i=0;i<17;++i){
        float a=t+(float)i*(6.2831853f/16.0f);
        ring[i].x=p->pos.x+cosf(a)*rad;
        ring[i].y=cy+0.16f*sinf(a*2.0f+t);
        ring[i].z=p->pos.z+sinf(a)*rad;
    }
    game_w2s_many(cam,ring,17,vw,vh,xy);
    for(int i=0;i<16;++i){
        if(isfinite(xy[i*2])&&isfinite(xy[i*2+1])&&isfinite(xy[i*2+2])&&isfinite(xy[i*2+3]))
            r_line_c(xy[i*2],xy[i*2+1],xy[i*2+2],xy[i*2+3],col,1.8f);
    }
}

// Handle mesh highlight.
static int mesh_highlight(const RR_PLAYER* p, const RR_CAMERA* cam, int vw, int vh, RGBA color) {
    if (!p->cham_valid) return 0;
    const float (*skin)[12] = p->cham_skin;
    CHAM_VISUAL_POSE* pose=p->cham_rig && p->cham_sample_at?cham_visual_pose(p):NULL;
    if(pose)skin=pose->skin;
    float points[RR_CHAM_VERTICES*2];
    RR_V3 local_positions[RR_CHAM_VERTICES];
    RR_V3* positions=pose?pose->vertices:local_positions;
    if(!pose || !pose->vertices_valid)for (int i=0;i<RR_CHAM_VERTICES;++i) {
        const RR_CHAM_VERTEX* v=&rr_cham_vertices[i]; RR_V3 world={0,0,0}; float total=0;
        for (int k=0;k<4;++k) {
            float w=v->weight[k]; int bone=v->bone[k];
            if (w<=0.0001f || bone>=RR_CHAM_BONES) continue;
            const float* m=skin[bone];
            world.x+=w*(m[0]*v->p[0]+m[1]*v->p[1]+m[2]*v->p[2]+m[3]);
            world.y+=w*(m[4]*v->p[0]+m[5]*v->p[1]+m[6]*v->p[2]+m[7]);
            world.z+=w*(m[8]*v->p[0]+m[9]*v->p[1]+m[10]*v->p[2]+m[11]);
            total+=w;
        }
        positions[i].x=positions[i].y=positions[i].z=NAN;
        if (total<0.9f || total>1.1f || !isfinite(world.x) || !isfinite(world.y) || !isfinite(world.z)) continue;
        float dx=world.x-p->pos.x,dy=world.y-p->pos.y,dz=world.z-p->pos.z;
        if (dx*dx+dy*dy+dz*dz>36.0f) continue;
        positions[i]=world;
    }
    if(pose)pose->vertices_valid=1;
    BR_Config* b=br_get();
    if(b->cham_model || b->cham_anim){
        RR_V3 morphed[RR_CHAM_VERTICES];
        apply_cham_model_and_anim(b,p,positions,morphed);
        game_w2s_many(cam,morphed,RR_CHAM_VERTICES,vw,vh,points);
    }else{
        game_w2s_many(cam,positions,RR_CHAM_VERTICES,vw,vh,points);
    }
    static CHAM_EDGE edges[RR_CHAM_TRIANGLES*3];static int edge_ready;
    if(!edge_ready){
        for(int i=0;i<RR_CHAM_TRIANGLES;++i)for(int e=0;e<3;++e){
            unsigned short a=rr_cham_triangles[i*3+e],v=rr_cham_triangles[i*3+(e+1)%3];
            edges[i*3+e]=(CHAM_EDGE){a<v?a:v,a<v?v:a,i};
        }
        qsort(edges,RR_CHAM_TRIANGLES*3,sizeof(edges[0]),edge_compare);edge_ready=1;
    }
    unsigned short selected[RR_CHAM_TRIANGLES*3];int triangles=0;
    float facing[RR_CHAM_TRIANGLES];unsigned char allowed[RR_CHAM_TRIANGLES];
    if(p->is_friend)color=slot_color(BR_C_FRIEND_CHAMS);
    else if(b->cham_color_mode==0)color=slot_color(23);
    RGBA part_color[3]={color,color,color};
    if((p->is_friend && b->friend_visibility)||(!p->is_friend && b->cham_visibility)){
        for(int part=0;part<3;++part){int los=p->line_of_sight[part];
            int state=los==RR_LOS_CLEAR?0:los==RR_LOS_BLOCKED?1:2;
            part_color[part]=slot_color((p->is_friend?BR_C_FRIEND_CHAMS_VISIBLE:15)+state);
        }
    }
    int shader_mode=b->cham_shader+(b->cham_material*16);
    int painted=0;
    for(int i=0;i<RR_CHAM_TRIANGLES;++i){
        int a=rr_cham_triangles[i*3],v=rr_cham_triangles[i*3+1],c=rr_cham_triangles[i*3+2];
        int part=vertex_part(a);
        allowed[i]=(b->cham_parts&part)!=0 && isfinite(points[a*2]) && isfinite(points[v*2]) && isfinite(points[c*2]);
        facing[i]=(points[v*2]-points[a*2])*(points[c*2+1]-points[a*2+1])-(points[v*2+1]-points[a*2+1])*(points[c*2]-points[a*2]);
    }
    for(int part=1;part<=4;part*=2){
        triangles=0;
        for(int i=0;i<RR_CHAM_TRIANGLES;++i)if(allowed[i] && vertex_part(rr_cham_triangles[i*3])==part){
            memcpy(selected+triangles*3,rr_cham_triangles+i*3,6);++triangles;}
        RGBA fill=part_color[part==1?0:part==2?1:2];
        fill=(fill&0xffffffu)|((RGBA)(b->cham_opacity*255/100)<<24);
        if(b->cham_style!=1)painted+=r_fill_cham_triangles(points,RR_CHAM_VERTICES,selected,triangles,fill,shader_mode);
    }
    if(b->cham_style){
        gpu_mesh_flush();
        for(int i=0;i<RR_CHAM_TRIANGLES*3;){
            int end=i+1;while(end<RR_CHAM_TRIANGLES*3 && edges[end].a==edges[i].a && edges[end].b==edges[i].b)++end;
            int positive=0,negative=0,total=0;
            for(int j=i;j<end;++j)if(allowed[edges[j].face]){++total;if(facing[edges[j].face]>=0)positive=1;else negative=1;}
            if(total && (total==1 || (positive&&negative))){
                int a=edges[i].a,c=edges[i].b,part=vertex_part(a);
                RGBA col=part_color[part==1?0:part==2?1:2];
                r_line_c(points[a*2],points[a*2+1],points[c*2],points[c*2+1],col,(float)b->cham_line);++painted;
            }
            i=end;
        }
    }
    draw_orbit_anim(b,p,cam,vw,vh,part_color[1]);
    return painted>0;
}

                                                                        
                                                                     
// Draw full skeleton.
static void draw_full_skeleton(const RR_PLAYER* p, const RR_CAMERA* cam, int vw, int vh, RGBA col, float stroke, int draw_head_circle, float fallback_hx, float fallback_hy, float bh) {
    RR_V3 world[RR_CHAM_BONES]; float xy[RR_CHAM_BONES*2];
    memcpy(world,p->bone,sizeof(world));
    game_w2s_many(cam,world,RR_CHAM_BONES,vw,vh,xy);
    for (int b=0;b<RR_CHAM_BONES;++b) {
        if (b == RR_RIG_L_ARM_0_JNT || b == RR_RIG_R_ARM_0_JNT ||
            b == RR_RIG_L_ARM_1_JNT || b == RR_RIG_R_ARM_1_JNT ||
            b == RR_RIG_M_NECK_JNT  || b == RR_RIG_M_HEAD_1_JNT || b == RR_RIG_M_HEAD_2_JNT) continue;
        int parent=rr_cham_bones[b].parent;
        if (parent<0 || parent>=RR_CHAM_BONES) continue;
        if(!rr_rig_is_skeleton_joint(b) || !rr_rig_is_skeleton_joint(parent))continue;
        float x=xy[b*2],y=xy[b*2+1],px=xy[parent*2],py=xy[parent*2+1];
        if (isfinite(x) && isfinite(y) && isfinite(px) && isfinite(py)) r_line(px,py,x,y);
    }
    /* Connect outer shoulders upward to upper spine, then a short clean neck up to head base */
    float s3x = xy[RR_RIG_M_SPINE_3_JNT * 2], s3y = xy[RR_RIG_M_SPINE_3_JNT * 2 + 1];
    float nkx = xy[RR_RIG_M_NECK_JNT * 2],    nky = xy[RR_RIG_M_NECK_JNT * 2 + 1];
    float h1x = xy[RR_RIG_M_HEAD_1_JNT * 2],  h1y = xy[RR_RIG_M_HEAD_1_JNT * 2 + 1];
    float h2x = xy[RR_RIG_M_HEAD_2_JNT * 2],  h2y = xy[RR_RIG_M_HEAD_2_JNT * 2 + 1];
    float lx  = xy[RR_RIG_L_ARM_1_JNT * 2],   ly  = xy[RR_RIG_L_ARM_1_JNT * 2 + 1];
    float rx  = xy[RR_RIG_R_ARM_1_JNT * 2],   ry  = xy[RR_RIG_R_ARM_1_JNT * 2 + 1];
    float nx = NAN, ny = NAN;
    if (isfinite(s3x) && isfinite(s3y) && isfinite(nkx) && isfinite(nky)) {
        float cx = s3x * 0.15f + nkx * 0.85f;
        float cy = s3y * 0.15f + nky * 0.85f;
        r_line(s3x, s3y, cx, cy);
        if (isfinite(lx) && isfinite(ly)) r_line(lx, ly, cx, cy);
        if (isfinite(rx) && isfinite(ry)) r_line(rx, ry, cx, cy);
        if (isfinite(h1x) && isfinite(h1y)) {
            nx = nkx * 0.55f + h1x * 0.45f;
            ny = nky * 0.55f + h1y * 0.45f;
            r_line(cx, cy, nx, ny);
        }
    } else if (isfinite(lx) && isfinite(ly) && isfinite(rx) && isfinite(ry)) {
        r_line(lx, ly, rx, ry);
    }
    if (draw_head_circle) {
        float hcx = fallback_hx, hcy = fallback_hy + bh * 0.065f;
        float hr = bh * 0.085f;
        if (isfinite(h1x) && isfinite(h1y) && isfinite(h2x) && isfinite(h2y)) {
            float d = hypotf(h2x - h1x, h2y - h1y);
            if (d > 1.0f && d < bh * 0.45f) {
                hr = d * 0.50f;
                if (hr < 3.0f) hr = 3.0f;
                if (hr > 44.0f) hr = 44.0f;
                if (isfinite(nx) && isfinite(ny)) {
                    float ux = h2x - nx, uy = h2y - ny, ul = hypotf(ux, uy);
                    if (ul > 0.5f) { hcx = nx + (ux / ul) * hr; hcy = ny + (uy / ul) * hr; }
                    else { hcx = nx; hcy = ny - hr; }
                } else {
                    hcx = h1x * 0.62f + h2x * 0.38f;
                    hcy = h1y * 0.62f + h2y * 0.38f;
                }
            }
        } else if (isfinite(nx) && isfinite(ny)) {
            hcx = nx;
            hcy = ny - hr;
        }
        if (hr < 3.0f) hr = 3.0f;
        if (hr > 44.0f) hr = 44.0f;
        if (isfinite(hcx) && isfinite(hcy)) r_ellipse(hcx, hcy, hr, hr, col, stroke);
    }
}

                                                                      
// Handle fmt num.
static void fmt_num(char* out, size_t n, unsigned v, const char* suffix) {
    char tmp[16];
    int k = 0;
    size_t o = 0;
    if (!n) return;
    do { tmp[k++] = (char)('0' + (v % 10u)); v /= 10u; } while (v && k < 15);
    while (k > 0 && o + 1 < n) out[o++] = tmp[--k];
    while (suffix && *suffix && o + 1 < n) out[o++] = *suffix++;
    out[o] = 0;
}
// Handle fmt copy.
static void fmt_copy(char* out, size_t n, const char* a, const char* b) {
    size_t o = 0;
    if (!n) return;
    if (a) while (*a && o + 1 < n) out[o++] = *a++;
    if (b) while (*b && o + 1 < n) out[o++] = *b++;
    out[o] = 0;
}

// Handle world marker visible.
static int world_marker_visible(const RR_PLAYER* p, const BR_Config* b) {
    return ((p->is_loot == 1 && b->show_loot) || (p->is_loot == 2 && b->show_loot_nodes))
               ? br_loot_rarity_visible(b, p->loot_rarity)
               : (p->is_loot == 3 && b->show_containers) ||
           (p->is_loot == 4 && b->show_doors) ||
           (p->is_loot == 5 && b->show_dropped_loot) ||
           (p->is_loot == 6 && b->show_vehicle_loot);
}

// Draw radar.
static void draw_radar(const RR_PLAYER* arr, int n, const RR_CAMERA* cam,
                       int vw, int vh, const BR_Config* b) {
    const RR_PLAYER* local = NULL;
    for (int i = 0; i < n; ++i) {
        if (arr[i].valid && arr[i].local == RR_LOCAL_CONFIRMED) {
            local = &arr[i];
            break;
        }
    }
                                                                             
    RR_VIEWPORT view=game_viewport(cam,vw,vh);
    RR_HUD_RECT rect=rr_hud_rect(b,rr_layout_bank(b,vw,vh),0,view);
    float width=rect.w,height=rect.h;
    if(b->radar_shape)width=height=fminf(width,height);
    if (!local || width < 70.0f || height < 70.0f || b->radar_range <= 0) return;
    float fx = cam->fwd.x, fz = cam->fwd.z;
    float length = hypotf(fx, fz);
    if (!(length>.01f) || !isfinite(length)) {
        fx=local->facing_valid?local->facing.x:0;
        fz=local->facing_valid?local->facing.z:1;
    } else {fx/=length;fz/=length;}
    float look_x=fx,look_z=fz;
    if(b->radar_north){fx=0;fz=1;}
    float cx=rect.x+width*.5f,cy=rect.y+height*.5f;
    float left = cx - width * .5f, top = cy - height * .5f;
                                                                            
    float plot_x=fminf(width,height)*.5f-10.0f,plot_y=plot_x;
    float range = (float)b->radar_range;
                                                                         
    RGBA background=RARGB(b->radar_background*255/100,12,15,25);
    if(b->radar_shape){r_fill_ellipse(cx,cy,width*.5f,height*.5f,background);r_ellipse(cx,cy,width*.5f,height*.5f,slot_color(21),1);}
    else {r_fill(left,top,width,height,background);r_rect(left,top,width,height,slot_color(21),1);}
    if(b->radar_grid){
        r_line_c(cx,top,cx,top+height,RARGB(80,180,180,210),1);
        r_line_c(left,cy,left+width,cy,RARGB(80,180,180,210),1);
    }
    gpu_font(10);
    r_text(cx-fx*(plot_x-3),cy-fz*(plot_y-3)-5,"N",slot_color(21),RT_CENTER|RT_OUTLINE);
                                                                         
                                                                           
                                                                                
    int have_arrow=b->radar_self_arrow==1 || local->facing_valid;
    float arrow_x=b->radar_self_arrow==1?look_x:local->facing.x;
    float arrow_z=b->radar_self_arrow==1?look_z:local->facing.z;
    float ax=arrow_x*fz-arrow_z*fx,ay=-(arrow_x*fx+arrow_z*fz);
    if(b->radar_view_cone){
        float vx=look_x*fz-look_z*fx,vy=-(look_x*fx+look_z*fz);
        RGBA cone=(slot_color(21)&0xffffffu)|0x78000000u;
        r_line_c(cx,cy,cx+vx*22-vy*9,cy+vy*22+vx*9,cone,1);
        r_line_c(cx,cy,cx+vx*22+vy*9,cy+vy*22-vx*9,cone,1);
    }
    if(b->radar_self_arrow!=2 && have_arrow){
        float arrow[]={cx+ax*8,cy+ay*8,cx-ax*5-ay*5,cy-ay*5+ax*5,
            cx-ax*5+ay*5,cy-ay*5-ax*5,cx+ax*8,cy+ay*8};
        r_polyline(arrow,4,slot_color(21),1.5f);
    }else r_fill_ellipse(cx,cy,3,3,slot_color(21));

    for (int i = 0; i < n; ++i) {
        const RR_PLAYER* p = &arr[i];
        if (!p->valid || p->local) continue;
        if (p->is_vehicle && (!b->radar_world || !b->show_vehicle_markers)) continue;
        if (p->is_loot) {
            if (!b->radar_world || !world_marker_visible(p, b) || (p->is_loot<3 && !rr_loot_allowed(p,b))) continue;
        } else if (!p->is_vehicle && !rr_friends_display_allowed(p,b)) continue;
        float dx = p->pos.x - local->pos.x, dz = p->pos.z - local->pos.z;
        float lateral=dx*fz-dz*fx, forward=dx*fx+dz*fz;
        if (!(fabsf(lateral)<=range && fabsf(forward)<=range)) continue;
        if(b->radar_shape && lateral*lateral+forward*forward>range*range)continue;
        float x = cx + lateral*(plot_x/range), y = cy - forward*(plot_y/range);
        if (p->is_vehicle) {
            if(b->radar_icons)r_rect(x-3,y-2,6,4,slot_color(25),1.5f);
            else r_fill_ellipse(x, y, 2.8f, 2.8f, slot_color(25));
            continue;
        }
        if (p->is_loot) {
            RGBA color = slot_color(28+p->is_loot);
            if(b->radar_icons){float diamond[]={x,y-3,x+3,y,x,y+3,x-3,y,x,y-3};r_polyline(diamond,5,color,1);}
            else r_fill_ellipse(x, y, 2.3f, 2.3f, color);
            continue;
        }
        RGBA color = player_color(p);
        if (p->dead) r_ellipse(x, y, 3.0f, 3.0f, color, 1.0f);
        else {
            r_fill_ellipse(x, y, 4.5f, 4.5f, RRGB(16, 11, 25));
            r_fill_ellipse(x, y, 3.1f, 3.1f, color);
        }
    }
}

// Handle vehicle mesh.
static const RR_VEHICLE_MESH* vehicle_mesh(const RR_PLAYER* p) {
    char key[RR_NAME_MAX];
    size_t length = 0;
    for (const unsigned char* c = (const unsigned char*)p->name; *c && length < sizeof(key) - 1; ++c) {
        if (isalnum(*c)) key[length++] = (char)tolower(*c);
    }
    key[length] = '\0';
    for (size_t i = 0; i < sizeof(rr_vehicle_meshes) / sizeof(rr_vehicle_meshes[0]); ++i) {
        const RR_VEHICLE_MESH* mesh = &rr_vehicle_meshes[i];
        if (strstr(key, mesh->name)) return mesh;
    }
    return NULL;
}

// Draw vehicle cham.
static void draw_vehicle_cham(const RR_PLAYER* p, const RR_CAMERA* cam, int vw, int vh) {
    const RR_VEHICLE_MESH* mesh = vehicle_mesh(p);
    if (!mesh) return;
    RR_V3 positions[RR_VEHICLE_MESH_MAX_VERTICES];
    float projected[2 * RR_VEHICLE_MESH_MAX_VERTICES];
    const float* pose = p->vehicle_pose;
    for (int i = 0; i < mesh->vertices; ++i) {
        const short* v = mesh->positions + 3 * i;
        float x = v[0] * 0.001f, y = v[1] * 0.001f, z = v[2] * 0.001f;
        positions[i] = (RR_V3){
            pose[0] * x + pose[1] * y + pose[2] * z + pose[3],
            pose[4] * x + pose[5] * y + pose[6] * z + pose[7],
            pose[8] * x + pose[9] * y + pose[10] * z + pose[11]
        };
    }
    game_w2s_many(cam, positions, mesh->vertices, vw, vh, projected);
    float left = INFINITY, top = INFINITY, right = -INFINITY, bottom = -INFINITY;
    for (int i = 0; i < mesh->vertices; ++i) {
        float x = projected[2 * i], y = projected[2 * i + 1];
        if (!isfinite(x) || !isfinite(y)) continue;
        left = fminf(left, x); right = fmaxf(right, x);
        top = fminf(top, y); bottom = fmaxf(bottom, y);
    }
    if (right < 0 || bottom < 0 || left > vw || top > vh || right - left < 2.0f || bottom - top < 2.0f) return;
                                                                             
                                             
    RGBA color = (slot_color(24)&0xffffffu)|((RGBA)(br_get()->vehicle_cham_opacity*255/100)<<24);
    r_fill_cham_triangles(projected, mesh->vertices, mesh->triangles, mesh->faces, color, br_get()->cham_shader);
}

// Draw loot.
static void draw_loot(const RR_PLAYER* p, const RR_CAMERA* cam, int vw, int vh) {
    static const char* const rarity_names[] = { "", "Common", "Uncommon", "Rare", "Epic", "Legendary", "Exotic" };
    float x, y;
    if (!game_w2s(cam, &p->pos, vw, vh, &x, &y)) return;
    BR_Config* config=br_get();
    int rarity=p->loot_rarity>=1 && p->loot_rarity<=6?p->loot_rarity:0;
    RGBA color=slot_color(p->is_loot>=3 || config->loot_color_mode==1?28+p->is_loot:config->loot_color_mode==2?29:36+rarity);
    if(config->item_icons && rr_item_icon(p->name,p->is_loot)>0){
        float size=(float)config->item_icon_size;
        RR_VIEWPORT view=game_viewport(cam,vw,vh);
        float left=x-size*.5f,top=y-size*.5f;
        float height=size+((config->item_icon_distance?1:0)+(config->item_icon_name?1:0))*config->text_size*1.4f;
        if(!label_place(&left,&top,size,height,config,view))return;
        r_round_rect(left-3,top-3,size+6,size+6,4,(color&0xffffffu)|0x96000000u,1);
        if(r_icon(rr_item_icon(p->name,p->is_loot),left,top,size)){
            float label_y=top+size+2;gpu_font((float)config->text_size);
            if(config->item_icon_name){r_text(left+size*.5f,label_y,p->name,color,RT_CENTER|RT_OUTLINE);label_y+=config->text_size*1.4f;}
            if(config->item_icon_distance){char distance[24];snprintf(distance,sizeof(distance),"%.0f m",p->dist);
                gpu_font((float)config->text_size);r_text(left+size*.5f,label_y,distance,slot_color(8),RT_CENTER|RT_OUTLINE);}
            return;
        }
    }
    const char* name = p->name[0] ? p->name : "Loot";
    const char* prefix = p->is_loot == 2 ? "Node: " : "";
    char label[RR_NAME_MAX + 48];
    snprintf(label, sizeof(label), "%s%s%s%s (%.0f m)", prefix, name,
             rarity ? " / " : "", rarity_names[rarity], p->dist);
    if (p->is_loot == 2) r_ellipse(x, y, 4.0f, 4.0f, color, 1.5f);
    else r_fill_ellipse(x, y, 4.0f, 4.0f, color);
    gpu_font(12.0f);
    r_text(x, y + 6.0f, label, color, RT_CENTER | RT_OUTLINE);
}

// Draw extra panels.
static void draw_extra_panels(const RR_PLAYER* arr,int n,const RR_CAMERA* cam,int vw,int vh,
                              BR_Config* b,int bank,RR_VIEWPORT view){
    gpu_font((float)b->text_size);
    if(b->target_panel){
        RR_HUD_RECT r=rr_hud_rect(b,bank,2,view);const RR_PLAYER* target=NULL;
        for(int i=0;i<n;++i)if(arr[i].valid && arr[i].id==b->telem_target && !arr[i].local && !arr[i].is_loot && !arr[i].is_vehicle){target=&arr[i];break;}
        r_fill(r.x,r.y,r.w,r.h,RARGB(150,14,17,28));
        char text[128];snprintf(text,sizeof(text),"TARGET: %s",target?target->name:"none");
        r_text(r.x+8,r.y+5,text,slot_color(6),RT_OUTLINE);
        if(target){snprintf(text,sizeof(text),"%.0f m  |  %s",target->dist,target->weapon);
            r_text(r.x+8,r.y+24,text,slot_color(7),RT_OUTLINE);}
    }
    if(b->loot_guide){
        const RR_PLAYER* best=NULL;float score=INFINITY;
        for(int i=0;i<n;++i){const RR_PLAYER* p=&arr[i];
            if(!p->valid || !p->is_loot || !world_marker_visible(p,b) || !rr_loot_allowed(p,b))continue;
            if(b->max_distance>0 && p->dist>b->max_distance/100.0f)continue;
            int value=rr_loot_value(p,b);float candidate=b->loot_priority?-(float)(value>=0?value:0)+p->dist*.001f:p->dist;
            if(candidate<score){score=candidate;best=p;}
        }
        RR_HUD_RECT r=rr_hud_rect(b,bank,3,view);char text[160];
        r_fill(r.x,r.y,r.w,r.h,RARGB(150,14,17,28));
        if(best){
            float dx=best->pos.x-cam->pos.x,dz=best->pos.z-cam->pos.z;
            float side=dx*cam->fwd.z-dz*cam->fwd.x,forward=dx*cam->fwd.x+dz*cam->fwd.z;
            snprintf(text,sizeof(text),"%s  %.0fm  %s",best->name,best->dist,forward<0?"BEHIND":side>2?"RIGHT":side<-2?"LEFT":"AHEAD");
            float x,y;if(game_w2s(cam,&best->pos,vw,vh,&x,&y))r_ellipse(x,y,9,9,slot_color(22),2);
        }else snprintf(text,sizeof(text),"No matching loot in the current scan");
        r_text(r.x+8,r.y+5,text,slot_color(22),RT_OUTLINE);
    }
}

// Draw frame.
static void draw_frame(const RR_PLAYER* arr, int n, const RR_CAMERA* cam, int vw, int vh) {
    BR_Config* b = br_get();
    b->telem_cham_n=0;
    g_label_count=0;
    RR_VIEWPORT view=game_viewport(cam,vw,vh);
    int bank=rr_layout_bank(b,vw,vh),arrows=0;
    b->telem_layout=bank;b->telem_view_w=(int)view.width;b->telem_view_h=(int)view.height;
    gpu_text_style(b->font_face,b->text_background,b->text_outline);
    gpu_opacity(b->esp_opacity/100.0f);
    if (!b->enabled) return;
                                                                            
    if (!arr || !cam || !cam->valid || n < 0) n = 0;

    float line_w = (float)(b->line_w < 1 ? 1 : (b->line_w > 4 ? 4 : b->line_w));
    if (vw < 16 || vh < 16) return;
    if (n>RR_MAX_ENTITIES) n=RR_MAX_ENTITIES;

    if ((b->cham_on || b->vehicle_chams) && n>0) {
                                                                                  
        if (b->vehicle_chams) for (int i=0;i<n;++i) {
            const RR_PLAYER* p=&arr[i];
            if (!p->valid || !p->is_vehicle || !p->vehicle_cham_valid ||
                (b->max_distance>0 && p->dist>(float)b->max_distance/100.0f)) continue;
            draw_vehicle_cham(p, cam, vw, vh);
        }
        gpu_mesh_flush();
        for (int i=0;i<n;++i) {
            const RR_PLAYER* p=&arr[i];
            if (!p->valid || (p->local && !b->show_self) || p->is_loot ||
                (b->max_distance>0 && p->dist>(float)b->max_distance/100.0f)) continue;
            if (p->is_vehicle) continue;
            if (!b->cham_on || !p->cham_valid) continue;
            if (!rr_friends_display_allowed(p,b)) continue;
            float fx,fy,hx,hy;
            if (!game_w2s(cam,&p->pos,vw,vh,&fx,&fy) && !game_w2s(cam,&p->head,vw,vh,&hx,&hy)) continue;
            gpu_opacity(player_alpha(p,b));
            if (mesh_highlight(p,cam,vw,vh,player_color(p))) {
                ++b->telem_cham_n;
            }
        }
        gpu_mesh_flush();
    }

    if (b->show_self) {
        const RR_PLAYER* self = NULL;
        for (int i = 0; i < n; ++i) if (arr[i].valid && arr[i].local) { self = &arr[i]; break; }
        gpu_font(12.0f);
        r_text(12.0f, 12.0f, !self ? "SELF: unavailable" :
               self->hidden_in_object ? "SELF: hiding object detected" : "SELF: no hiding object detected",
               RRGB(227, 215, 252), RT_LEFT | RT_OUTLINE);
    }
    gpu_opacity(b->esp_opacity/100.0f);
    int drawn = 0;
    for (int i = 0; i < n; i++) {
                                                                               
        if (drawn >= RR_MAX_ENTITIES) continue;
        const RR_PLAYER* p = &arr[i];
        if (!p->valid || (p->local && !b->show_self)) continue;
        if (!rr_friends_display_allowed(p,b)) continue;
                                                                            
        if (b->max_distance > 0 && p->dist > (float)b->max_distance / 100.0f) continue;

        gpu_opacity(player_alpha(p,b));

        if (p->is_loot) {
            int visible = (p->is_loot == 1 && b->show_loot) ||
                          (p->is_loot == 2 && b->show_loot_nodes) ||
                          world_marker_visible(p, b);
            if (visible && (p->is_loot>=3 || rr_loot_allowed(p,b)) && (p->is_loot > 2 || br_loot_rarity_visible(b, p->loot_rarity)))
                draw_loot(p, cam, vw, vh);
            continue;
        }
        if (p->is_vehicle) {
            if (b->show_vehicle_markers) {
                float x, y;
                if (game_w2s(cam, &p->pos, vw, vh, &x, &y)) {
                    char label[RR_NAME_MAX + 32];
                    const char* name = p->name[0] ? p->name : (p->is_vehicle == 2 ? "Helicopter" : "Car");
                    if (b->show_vehicle_occupants && p->occupants_known)
                        snprintf(label, sizeof label, "%s [%d] (%.0f m)", name, p->occupants, p->dist);
                    else snprintf(label, sizeof label, "%s (%.0f m)", name, p->dist);
                    RGBA color = slot_color(25);
                    float label_y=y+6.0f;
                    int icon=b->vehicle_icons?rr_item_icon(name,0):0;
                    float size=(float)b->item_icon_size;
                    if(icon && r_icon(icon,x-size*.5f,y-size*.5f,size))label_y=y+size*.5f+3;
                    else r_fill_ellipse(x,y,4,4,color);
                    gpu_font((float)b->text_size);r_text(x,label_y,label,color,RT_CENTER|RT_OUTLINE);
                }
            }
            continue;
        }

        float screen_x,screen_y;
        int projected=game_w2s(cam,&p->head,vw,vh,&screen_x,&screen_y);
        int on_image=projected && screen_x>=view.x && screen_x<=view.x+view.width &&
                     screen_y>=view.y && screen_y<=view.y+view.height;
        if (b->offscreen_on && !p->local && arrows < b->offscreen_count) {
            int want_arrow = !on_image;
            if (b->offscreen_fov_ring) {
                float cx = view.x + view.width * 0.5f, cy = view.y + view.height * 0.5f;
                float ring = offscreen_ring_radius(view, b);
                want_arrow = !projected || !isfinite(screen_x) || !isfinite(screen_y) ||
                             hypotf(screen_x - cx, screen_y - cy) > ring;
            }
            if (want_arrow) {
                offscreen_arrow(p, cam, view, b, projected, screen_x, screen_y);
                ++arrows;
            }
        }
        if(!on_image && !projected)continue;
        int compact=b->lod_on && p->dist>b->lod_detail;
        if(b->lod_on && b->lod_marker_only && p->dist>b->lod_dot){
            if(on_image)r_fill_ellipse(screen_x,screen_y,3,3,player_color(p));
            continue;
        }
        RR_PLAYER visual;
        if (p->cham_valid && p->cham_rig && p->cham_sample_at && (!p->inveh || p->full_rig)) {
            const CHAM_VISUAL_POSE* pose = cham_visual_pose(p);
            visual = *p;
            visual.pos = pose->anchors[0];
            visual.head = pose->anchors[1];
            memcpy(visual.bone, &pose->anchors[2], sizeof(visual.bone));
            p = &visual;
        }

        float fx = NAN, fy = NAN, hx = NAN, hy = NAN;
                                                                               
                                           
           
        RR_V3 top = p->head;
                                                                             
                                                                             
           
        if (p->inveh && !p->mesh_bones) { top.x = p->pos.x; top.y = p->pos.y + 1.45f; top.z = p->pos.z; }
        int feet_ok = game_w2s(cam, &p->pos, vw, vh, &fx, &fy);
        int head_ok = game_w2s(cam, &top, vw, vh, &hx, &hy);
        if ((!feet_ok && !head_ok) || !isfinite(fx) || !isfinite(fy) || !isfinite(hx) || !isfinite(hy)) continue;

        float bh = fabsf(fy - hy);
        if (bh < 4.0f) bh = 4.0f;
        if (p->inveh && bh > 420.0f) bh = 420.0f;
        float bw = bh * (p->inveh ? 1.30f : 0.52f) + fabsf(hx - fx);
        float bx = (fx + hx) * 0.5f - bw * 0.5f;
        float by = fminf(hy, fy);
        if (bx < -200 || bx > vw + 200 || by < -200 || by > vh + 200) continue;

        RGBA col = player_color(p);
        if (p->dead) {
                                                
            int a = 255 * 60 / 100;
            col = (col & 0x00FFFFFFu) | ((uint32_t)a << 24);
        }


        if (b->show_box) {
            gpu_opacity(player_alpha(p,b)*b->box_alpha/100);
            styled_box(p,cam,vw,vh,bx,by,bw,bh,player_style_color(p,b,b->box_visibility,BR_C_BOX_VISIBLE,b->box_custom?slot_color(20):col),b);
        }
        if (b->show_head) {
            gpu_opacity(player_alpha(p,b)*b->head_alpha/100);
            RGBA head_col=player_style_color(p,b,b->head_visibility,BR_C_HEAD_VISIBLE,b->head_custom?slot_color(19):col);
            float hcx = hx, hcy = hy + bh * 0.065f, r = fmaxf(3.0f, fminf(44.0f, bh * 0.085f));
            if (p->full_rig && p->nbones == RR_CHAM_BONES) {
                float h1x, h1y, h2x, h2y, nkx, nky;
                if (game_w2s(cam, (const RR_V3*)p->bone[RR_RIG_M_HEAD_1_JNT], vw, vh, &h1x, &h1y) &&
                    game_w2s(cam, (const RR_V3*)p->bone[RR_RIG_M_HEAD_2_JNT], vw, vh, &h2x, &h2y) &&
                    isfinite(h1x) && isfinite(h1y) && isfinite(h2x) && isfinite(h2y)) {
                    float d = hypotf(h2x - h1x, h2y - h1y);
                    if (d > 1.0f && d < bh * 0.45f) {
                        r = fmaxf(3.0f, fminf(44.0f, d * 0.50f));
                        if (game_w2s(cam, (const RR_V3*)p->bone[RR_RIG_M_NECK_JNT], vw, vh, &nkx, &nky) && isfinite(nkx) && isfinite(nky)) {
                            float nx = nkx * 0.55f + h1x * 0.45f, ny = nky * 0.55f + h1y * 0.45f;
                            float ux = h2x - nx, uy = h2y - ny, ul = hypotf(ux, uy);
                            if (ul > 0.5f) { hcx = nx + (ux / ul) * r; hcy = ny + (uy / ul) * r; }
                            else { hcx = nx; hcy = ny - r; }
                        } else {
                            hcx = h1x * 0.62f + h2x * 0.38f;
                            hcy = h1y * 0.62f + h2y * 0.38f;
                        }
                    }
                }
            }
            if(b->head_style==2){r_line_c(hcx-r,hcy,hcx+r,hcy,head_col,line_w);r_line_c(hcx,hcy-r,hcx,hcy+r,head_col,line_w);}
            else if(isfinite(hcx)&&isfinite(hcy)) r_ellipse(hcx,hcy,r,r,head_col,line_w);
        }
        if (!compact && b->show_skeleton && !p->hidden_in_object &&
            (b->skel_distance <= 0 || p->dist <= (float)b->skel_distance / 100.0f)) {
            gpu_opacity(player_alpha(p,b)*b->skeleton_alpha/100);
            RGBA skeleton_col=player_style_color(p,b,b->skeleton_visibility,BR_C_SKELETON_VISIBLE,b->skeleton_custom?slot_color(18):col);
            float skel_w = b->skeleton_width/10.0f;
            gpu_pen(skeleton_col,skel_w);
            if (p->full_rig && p->nbones == RR_CHAM_BONES) {
                draw_full_skeleton(p,cam,vw,vh,skeleton_col,skel_w,!b->show_head,hx,hy,bh);
            } else if (!p->inveh && p->nbones > 1) {
                                                                              
                for (int k = 0; k + 1 < p->nbones; k++) {
                    float ax, ay, bx2, by2;
                    if (!game_w2s(cam, (const RR_V3*)p->bone[k], vw, vh, &ax, &ay)) continue;
                    if (!game_w2s(cam, (const RR_V3*)p->bone[k + 1], vw, vh, &bx2, &by2)) continue;
                    r_line(ax, ay, bx2, by2);
                }
                float hxx, hyy;
                if (!b->show_head && game_w2s(cam, (const RR_V3*)p->bone[p->nbones - 1], vw, vh, &hxx, &hyy)) {
                    float r = fabsf(fy - hyy) * 0.075f;
                    if (r < 2.5f) r = 2.5f;
                    if (r > 18.0f) r = 18.0f;
                    r_ellipse(hxx, hyy - r * 0.5f, r, r, skeleton_col, skel_w);
                }
            } else if (!b->show_head && isfinite(hx) && isfinite(hy)) {
                float r = fmaxf(3.0f, fminf(44.0f, bh * 0.085f));
                r_ellipse(hx, hy + bh * 0.065f, r, r, skeleton_col, skel_w);
            } else {
                                                                 
                float mid = by + bh * 0.55f;
                r_line(fx, by + bh * 0.10f, fx, mid);
                r_line(fx, mid, fx, by + bh);
                r_line(fx - bw * 0.30f, mid, fx + bw * 0.30f, mid);
            }
        }
        gpu_opacity(player_alpha(p,b));
        player_labels(p,b,view,bx,by,bw,bh,0,col);
        drawn++;
    }

    gpu_opacity(b->esp_opacity/100.0f);
                                                                           
    if (b->aa_on && b->aa_show_fov) {
        int r = b->aa_fov_px;
        if (r < 10) r = 10;
        if (r > 1200) r = 1200;
        r_ellipse((float)vw * 0.5f, (float)vh * 0.5f, (float)r, (float)r,
                  slot_color(14), 1.0f);
    }
    if (b->silent_aim_on && b->silent_show_fov) {
        int r = b->silent_aim_fov;
        if (r < 20) r = 20;
        if (r > 600) r = 600;
        r_ellipse((float)vw * 0.5f, (float)vh * 0.5f, (float)r, (float)r,
                  RARGB(170, 0, 102, 255), 1.0f);
    }
    if (b->target_tracer) {
        uint64_t taddr = 0; int32_t tid = 0; int tgroup = 0;
        if (rr_target_tracer_get_id(&taddr, &tid, &tgroup)) {
            const RR_PLAYER* tp = NULL;
            for (int i = 0; i < n; ++i) {
                if (arr[i].valid && arr[i].addr == taddr && arr[i].id == tid) { tp = &arr[i]; break; }
            }
            if (tp) {
                RR_V3 tpt = tp->head;
                if (tp->full_rig && tp->nbones == RR_CHAM_BONES) {
                    int jnt = tgroup == 2 ? RR_RIG_M_SPINE_3_JNT :
                              tgroup == 3 ? RR_RIG_M_SPINE_1_JNT :
                              tgroup == 1 ? RR_RIG_M_NECK_JNT : RR_RIG_M_HEAD_1_JNT;
                    RR_V3 jb = { tp->bone[jnt][0], tp->bone[jnt][1], tp->bone[jnt][2] };
                    if (jnt == RR_RIG_M_HEAD_1_JNT) {
                        RR_V3 hb = { tp->bone[RR_RIG_M_HEAD_2_JNT][0], tp->bone[RR_RIG_M_HEAD_2_JNT][1], tp->bone[RR_RIG_M_HEAD_2_JNT][2] };
                        if (isfinite(hb.x) && isfinite(hb.y) && isfinite(hb.z) && fabsf(hb.y - tp->pos.y) < 3.0f) {
                            jb.x = (jb.x + hb.x) * 0.5f; jb.y = (jb.y + hb.y) * 0.5f; jb.z = (jb.z + hb.z) * 0.5f;
                        }
                    }
                    if (isfinite(jb.x) && isfinite(jb.y) && isfinite(jb.z) && fabsf(jb.y - tp->pos.y) < 3.0f) tpt = jb;
                }
                float tx, ty;
                if (game_w2s(cam, &tpt, vw, vh, &tx, &ty) && isfinite(tx) && isfinite(ty)) {
                    r_line_c((float)vw * 0.5f, (float)vh * 0.5f, tx, ty, slot_color(14), 1.5f);
                }
            }
        }
    }
    if (b->radar_on && n > 0) draw_radar(arr, n, cam, vw, vh, b);

    draw_extra_panels(arr,n,cam,vw,vh,b,bank,view);
    char friend_notice[160];
    if(rr_friends_notice(friend_notice,sizeof(friend_notice))){
        gpu_opacity(1);gpu_font(14);gpu_text_style(b->font_face,55,1);
        r_text(view.x+view.width*.5f,view.y+view.height*.62f,friend_notice,RRGB(190,255,215),RT_CENTER|RT_OUTLINE);
    }
    b->telem_targets = drawn;
    gpu_opacity(1);
    gpu_text_style(b->font_face,0,1);
}

typedef struct {const RR_PLAYER* player;float alpha;} DRAW_ITEM;
// Handle distance compare.
static int distance_compare(const void* a,const void* b){
    float x=((const DRAW_ITEM*)a)->player->dist,y=((const DRAW_ITEM*)b)->player->dist;
    return x<y?-1:x>y?1:0;
}
// Handle same draw entity.
static int same_draw_entity(const RR_PLAYER* a,const RR_PLAYER* b){
    if(a->addr!=b->addr || a->id!=b->id || a->is_loot!=b->is_loot || a->is_vehicle!=b->is_vehicle)return 0;
    return a->addr || (!strcmp(a->name,b->name) && fabsf(a->pos.x-b->pos.x)<.1f && fabsf(a->pos.z-b->pos.z)<.1f);
}
// Draw esp.
void draw_esp(const RR_PLAYER* arr,int n,const RR_CAMERA* cam,int vw,int vh){
    typedef struct{RR_PLAYER player;DWORD first,last;int used,seen;} FADE_ENTRY;
    static FADE_ENTRY history[RR_MAX_ENTITIES];
    static RR_PLAYER visible[RR_MAX_ENTITIES];
    static DRAW_ITEM order[RR_MAX_ENTITIES];
    BR_Config* b=br_get();DWORD now=GetTickCount();int count=0;
    if(!b->enabled || !cam || !cam->valid){memset(history,0,sizeof(history));draw_frame(arr,0,cam,vw,vh);return;}
    for(int i=0;i<RR_MAX_ENTITIES;++i)history[i].seen=0;
    if(n>RR_MAX_ENTITIES)n=RR_MAX_ENTITIES;
    for(int i=0;arr && i<n;++i){
        if(!arr[i].valid)continue;
        FADE_ENTRY* slot=history[i].used && same_draw_entity(&history[i].player,&arr[i])?&history[i]:NULL;
        FADE_ENTRY* spare=NULL;
        for(int j=0;!slot && j<RR_MAX_ENTITIES;++j){FADE_ENTRY* s=&history[j];
            int match=s->used && same_draw_entity(&s->player,&arr[i]);
            if(match){slot=s;break;}
            if(!s->seen && (!spare || (spare->used && (!s->used || now-s->last>now-spare->last))))spare=s;
        }
        if(!slot){slot=spare;if(!slot)continue;slot->used=1;slot->first=now;}
        slot->seen=1;slot->last=now;slot->player=arr[i];
        float alpha=b->fade_ms?fminf(1,(float)(now-slot->first)/b->fade_ms):1;
        order[count++]=(DRAW_ITEM){&arr[i],alpha};
    }
    for(int i=0;i<RR_MAX_ENTITIES && count<RR_MAX_ENTITIES;++i){FADE_ENTRY* s=&history[i];
        if(!s->used || s->seen)continue;
                                                                             
                                                           
        DWORD fade=s->player.is_loot?(DWORD)b->fade_ms:(DWORD)(b->fade_ms<16?b->fade_ms:16);
        if(!fade || now-s->last>=fade){s->used=0;continue;}
        order[count++]=(DRAW_ITEM){&s->player,1-(float)(now-s->last)/fade};
    }
                                                                                
    qsort(order,count,sizeof(order[0]),distance_compare);
    for(int i=0;i<count;++i){visible[i]=*order[i].player;visible[i].visual_alpha=order[i].alpha;}
    rr_friends_apply(visible,count);rr_visibility_apply(visible,count);
    draw_frame(visible,count,cam,vw,vh);
}












