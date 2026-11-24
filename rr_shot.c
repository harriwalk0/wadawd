#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <mmsystem.h>
#include <stdio.h>
#include <stdarg.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>
#include "rr_shot.h"
#include "rr_shot_shared.h"
#include "rr_shot_thunk_bytes.h"
#include "invoke_shot_offsets.h"
#include "rr_mem.h"
#include "rr_unity.h"
#include "rr_friends.h"
#include "rr_visibility.h"
#include "rr_los.h"
#include "menu_bridge.h"

typedef struct {uint64_t world;DWORD at;float yaw,pitch;int valid;} SHOT_TARGET;
static SRWLOCK g_target_lock=SRWLOCK_INIT;
static SHOT_TARGET g_target;
static SRWLOCK g_tracer_lock=SRWLOCK_INIT;
static RR_V3 g_tracer_pt;
static DWORD g_tracer_at;
static int g_tracer_valid;
static uint64_t g_tracer_addr;
static int32_t g_tracer_id;
static int g_tracer_group;

void rr_target_tracer_set(RR_V3 pt){
    if(!isfinite(pt.x)||!isfinite(pt.y)||!isfinite(pt.z))return;
    AcquireSRWLockExclusive(&g_tracer_lock);
    g_tracer_pt=pt;g_tracer_at=GetTickCount();g_tracer_valid=1;
    ReleaseSRWLockExclusive(&g_tracer_lock);
}
int rr_target_tracer_get(RR_V3* out){
    if(!out)return 0;
    DWORD now=GetTickCount();
    AcquireSRWLockShared(&g_tracer_lock);
    int ok=g_tracer_valid && (now-g_tracer_at<=160);
    if(ok)*out=g_tracer_pt;
    ReleaseSRWLockShared(&g_tracer_lock);
    return ok;
}
void rr_target_tracer_set_id(uint64_t addr, int32_t id, int group){
    if(!addr)return;
    AcquireSRWLockExclusive(&g_tracer_lock);
    g_tracer_addr=addr;g_tracer_id=id;g_tracer_group=group;g_tracer_at=GetTickCount();g_tracer_valid=1;
    ReleaseSRWLockExclusive(&g_tracer_lock);
}
int rr_target_tracer_get_id(uint64_t* addr, int32_t* id, int* group){
    DWORD now=GetTickCount();
    AcquireSRWLockShared(&g_tracer_lock);
    int ok=g_tracer_valid && g_tracer_addr && (now-g_tracer_at<=160);
    if(ok){if(addr)*addr=g_tracer_addr;if(id)*id=g_tracer_id;if(group)*group=g_tracer_group;}
    ReleaseSRWLockShared(&g_tracer_lock);
    return ok;
}
static HANDLE g_worker;
static volatile LONG g_stop;
static uint64_t g_module,g_remote,g_slot,g_original,g_owner,g_model,g_world,g_clock,g_rotation;
static DWORD g_pid;
static unsigned g_sequence;
static void shot_log(const char* fmt, ...){ (void)fmt; }
#define RR_SHOT_NEAR_BODY .25f

// Accept only the native input builder used to derive this callback.
static int supported_build(void){
    uint32_t size=0,pe,stamp,image;unsigned char dos[64],headers[128],code[sizeof(rr_shot_update_signature)];
    if(!rr_module("GameAssembly.dll",&g_module,&size)||!rr_read(g_module,dos,sizeof(dos))||memcmp(dos,"MZ",2))return 0;
    memcpy(&pe,dos+0x3c,4);
    if(pe>0x1000||!rr_read(g_module+pe,headers,sizeof(headers))||memcmp(headers,"PE\0\0",4))return 0;
    memcpy(&stamp,headers+8,4);memcpy(&image,headers+80,4);
    if(stamp==RR_SHOT_GAME_TIMESTAMP && image==RR_SHOT_GAME_IMAGE_SIZE)
        return rr_read(g_module+RR_SHOT_UPDATE,code,sizeof(code))&&!memcmp(code,rr_shot_update_signature,sizeof(code));
    return rr_read(g_module+RR_SHOT_UPDATE,code,sizeof(code))&&!memcmp(code,rr_shot_update_signature,sizeof(code));
}

// Resolve the same system clock inside the target process.
static uint64_t remote_clock(void){
    HMODULE module=GetModuleHandleW(L"kernelbase.dll");FARPROC clock=module?GetProcAddress(module,"GetTickCount64"):NULL;
    MEMORY_BASIC_INFORMATION info;wchar_t path[MAX_PATH];char name[MAX_PATH];uint64_t remote=0;uint32_t size=0;
    if(!clock||!VirtualQuery((const void*)clock,&info,sizeof(info))||
       !GetModuleFileNameW((HMODULE)info.AllocationBase,path,_countof(path)))return 0;
    const wchar_t* leaf=wcsrchr(path,L'\\');leaf=leaf?leaf+1:path;
    if(!WideCharToMultiByte(CP_UTF8,0,leaf,-1,name,sizeof(name),NULL,NULL)||!rr_module(name,&remote,&size))return 0;
    uint64_t offset=(uint64_t)(uintptr_t)clock-(uint64_t)(uintptr_t)info.AllocationBase;
    return offset<size?remote+offset:0;
}

// Leave in-flight callbacks allocated but inert until the process exits.
static void release_callback(void){
    if(g_pid==g_rr_pid && g_remote){uint32_t disabled=0;rr_write(g_remote+0x1000+offsetof(RR_SHOT_CONTROL,enabled),&disabled,4);}
    if(g_pid==g_rr_pid && g_slot && rr_u64(g_slot)==g_remote)rr_write(g_slot,&g_original,8);
    g_slot=g_owner=g_model=g_world=g_rotation=0;
}

// Install through one writable IL2CPP virtual slot.
static int install_callback(uint64_t owner,uint64_t model,uint64_t world,uint64_t rotation){
    uint64_t klass=rr_u64(owner),original=g_module+RR_SHOT_UPDATE;
    if(!u_is(owner,"BattleInputUpdater")||rr_u64(owner+0x10)!=model)return 0;
    uint64_t slot=klass+RR_SHOT_VTABLE+RR_SHOT_UPDATE_SLOT*16,method=rr_u64(slot+8);
    if(rr_u64(slot)!=original||!PL(method)||rr_u64(method)!=original){
        uint64_t found_slot=0;
        for(uint64_t off=0x100;off<=0x260;off+=16){
            uint64_t s=klass+off,m=rr_u64(s+8);
            if(rr_u64(s)==original && PL(m) && rr_u64(m)==original){found_slot=s;method=m;break;}
        }
        if(!found_slot){
            shot_log("install_callback: vtable mismatch klass=%llX slot=%llX val=%llX orig=%llX",
                     (unsigned long long)klass,(unsigned long long)slot,(unsigned long long)rr_u64(slot),(unsigned long long)original);
            return 0;
        }
        slot=found_slot;
    }
    uint64_t clock=remote_clock();if(!clock)return -1;
    if(!g_remote && !rr_drv_alloc_remote(g_rr_pid,0x2000,PAGE_EXECUTE_READWRITE,&g_remote))return -1;
    unsigned char code[sizeof(rr_shot_thunk_bytes)];memcpy(code,rr_shot_thunk_bytes,sizeof(code));
    uint64_t control_address=g_remote+0x1000;memcpy(code+RR_SHOT_CONTROL_PATCH,&control_address,8);
    RR_SHOT_CONTROL control={0};control.original=original;control.tick=clock;control.owner=owner;control.model=model;control.rotation=rotation;
    control.tolerance_squared=.00121847f;
    if(!rr_write(g_remote,code,sizeof(code))||!rr_write(control_address,&control,sizeof(control))||
       rr_u64(slot)!=original||!rr_write(slot,&g_remote,8))return -1;
    if(rr_u64(slot)!=g_remote)return -1;
    g_clock=clock;g_slot=slot;g_original=original;g_owner=owner;g_model=model;g_world=world;g_rotation=rotation;g_sequence=0;return 1;
}

typedef struct {RR_CAMERA_REGION region;uint64_t distance;} SHOT_REGION;
static int region_order(const void* a,const void* b){
    uint64_t x=((const SHOT_REGION*)a)->distance,y=((const SHOT_REGION*)b)->distance;return x<y?-1:x>y?1:0;
}

// Find the typed input consumer without holding render locks.
static int discover_callback(uint64_t model,uint64_t world,uint64_t rotation){
    static RR_CAMERA_REGION source[16384];static SHOT_REGION regions[16384];static unsigned char bytes[0x10000];
    int count=game_all_regions(source,16384);
    for(int i=0;i<count;++i){regions[i].region=source[i];regions[i].distance=source[i].base>model?source[i].base-model:model-source[i].base;}
    qsort(regions,count,sizeof(regions[0]),region_order);
    for(int i=0;i<count;++i)for(uint64_t offset=0;offset<regions[i].region.size;offset+=sizeof(bytes)){
        if(InterlockedCompareExchange(&g_stop,0,0)||!(br_get()->always_head_on||br_get()->silent_aim_on)||g_pid!=g_rr_pid||world!=game_world_addr())return 0;
        size_t size=(size_t)(regions[i].region.size-offset);if(size>sizeof(bytes))size=sizeof(bytes);
        uint64_t base=regions[i].region.base+offset;if(!rr_read(base,bytes,size))continue;
        for(size_t j=0;j+8<=size;j+=8){uint64_t value;memcpy(&value,bytes+j,8);if(value!=model)continue;
            static const uint64_t offs[]={0x10,0x18,0x20,0x28,0x30,0x38};
            for(int oi=0;oi<6;++oi){
                if(base+j<offs[oi])continue;
                uint64_t owner=base+j-offs[oi];
                if(u_is(owner,"BattleInputUpdater")){
                    if(offs[oi]!=0x10)shot_log("discover_callback: BattleInputUpdater has model at off=0x%X owner=%llX",(unsigned)offs[oi],(unsigned long long)owner);
                    int result=install_callback(owner,model,world,rotation);
                    if(result)return result;
                }
            }
        }
        if((offset&0xfffff)==0)Sleep(1);
    }
    shot_log("discover_callback: 0 matches in %d regions (model=%llX)",count,(unsigned long long)model);
    return 0;
}

// Publish a short-lived correction with a sequence guard.
static void publish_target(const SHOT_TARGET* target){
    uint64_t address=g_remote+0x1000;uint32_t disabled=0,odd=++g_sequence;
    if(!(odd&1))odd=++g_sequence;
    rr_write(address+offsetof(RR_SHOT_CONTROL,sequence),&odd,4);
    RR_SHOT_CONTROL control={0};control.original=g_original;control.tick=g_clock;control.owner=g_owner;control.model=g_model;control.rotation=g_rotation;
    control.sequence=odd;control.tolerance_squared=0.15f;
    if(target->valid && target->world==g_world && GetTickCount()-target->at<=150 &&
       (br_get()->always_head_on || br_get()->silent_aim_on) && !br_get()->menu_open && !br_get()->telem_freelook){
        float look[2];
        if(rr_read(g_rotation+0x10,look,sizeof(look))&&isfinite(look[0])&&isfinite(look[1])){
            control.yaw=target->yaw;control.pitch=target->pitch;control.source_yaw=look[0];control.source_pitch=look[1];
            control.expires=GetTickCount64()+150;control.enabled=1;
        }
    }
    if(!control.tick || !rr_write(address,&control,offsetof(RR_SHOT_CONTROL,applied))){rr_write(address+offsetof(RR_SHOT_CONTROL,enabled),&disabled,4);return;}
    uint32_t even=++g_sequence;rr_write(address+offsetof(RR_SHOT_CONTROL,sequence),&even,4);
}

// Keep discovery and remote writes outside rendering and mouse input.
static DWORD WINAPI shot_worker(LPVOID unused){
    (void)unused;DWORD validated=0,counted=0,last_log=0;uint64_t context_world=0,context_model=0,context_rotation=0;
    while(!InterlockedCompareExchange(&g_stop,0,0)){
        if(g_pid!=g_rr_pid){g_slot=g_remote=g_owner=g_model=g_world=g_module=g_clock=g_rotation=0;g_pid=g_rr_pid;validated=0;}
        BR_Config* config=br_get();
        if(!(config->always_head_on||config->silent_aim_on)||!g_pid){release_callback();config->telem_always_head=0;Sleep(50);continue;}
        DWORD now=GetTickCount();
        if(!validated || now-validated>250){
            if(!game_shot_input_context(&context_world,&context_model,&context_rotation)){
                if(now-last_log>2000){last_log=now;shot_log("shot_worker: game_shot_input_context=0 pid=%u",(unsigned)g_pid);}
                release_callback();config->telem_always_head=1;Sleep(50);continue;
            }
            validated=now;
        }
        if(g_slot && (g_world!=context_world||g_model!=context_model||g_rotation!=context_rotation||rr_u64(g_owner+0x10)!=g_model||rr_u64(g_slot)!=g_remote))release_callback();
        if(!g_slot){
            config->telem_always_head=1;
            if(!supported_build()){config->telem_always_head=-1;Sleep(500);continue;}
            int installed=discover_callback(context_model,context_world,context_rotation);
            if(installed<=0){
                if(now-last_log>2000){last_log=now;shot_log("shot_worker: discover_callback=%d model=%llX",(int)installed,(unsigned long long)context_model);}
                config->telem_always_head=installed<0?-2:1;Sleep(100);continue;
            }
            shot_log("shot_worker: installed slot=%llX owner=%llX model=%llX",(unsigned long long)g_slot,(unsigned long long)g_owner,(unsigned long long)g_model);
        }
        AcquireSRWLockShared(&g_target_lock);SHOT_TARGET target=g_target;ReleaseSRWLockShared(&g_target_lock);
        publish_target(&target);config->telem_always_head=target.valid && now-target.at<=150?3:2;Sleep(3);
        if(now-counted>=100){config->telem_always_head_shots=(int)(rr_u32(g_remote+0x1000+offsetof(RR_SHOT_CONTROL,applied))&0x7fffffffu);counted=now;}
    }
    release_callback();br_get()->telem_always_head=0;return 0;
}

// Measure the distance from the shot ray to the player's body segment.
static float body_ray_distance(RR_V3 origin,RR_V3 ray,RR_V3 feet,RR_V3 head){
    RR_V3 v={head.x-feet.x,head.y-feet.y,head.z-feet.z},w={origin.x-feet.x,origin.y-feet.y,origin.z-feet.z};
    float b=ray.x*v.x+ray.y*v.y+ray.z*v.z,c=v.x*v.x+v.y*v.y+v.z*v.z;
    float d=ray.x*w.x+ray.y*w.y+ray.z*w.z,e=v.x*w.x+v.y*w.y+v.z*w.z;
    if(fmaxf(-d,b-d)<=0)return INFINITY;
    float denominator=c-b*b,u=c>.000001f?(denominator>.000001f?(e-b*d)/denominator:e/c):0;
    u=fmaxf(0,fminf(1,u));float t=b*u-d;
    if(t<0){t=0;u=c>.000001f?fmaxf(0,fminf(1,e/c)):0;}
    float x=w.x+ray.x*t-v.x*u,y=w.y+ray.y*t-v.y*u,z=w.z+ray.z*t-v.z*u;
    return x*x+y*y+z*z;
}

// Read a finite native joint independently of screen projection.
static int shot_joint(const RR_PLAYER* player,int index,RR_V3* out){
    if(!player->full_rig||player->nbones!=RR_CHAM_BONES||index<0||index>=RR_CHAM_BONES)return 0;
    *out=(RR_V3){player->bone[index][0],player->bone[index][1],player->bone[index][2]};
    float x=out->x-player->pos.x,y=out->y-player->pos.y,z=out->z-player->pos.z;
    return isfinite(x)&&isfinite(y)&&isfinite(z)&&x*x+y*y+z*z<36;
}

// Measure the gap to body capsules, including arms and legs.
static float shot_body_gap(const RR_PLAYER* player,RR_V3 origin,RR_V3 ray,RR_V3 head){
    static const struct {unsigned char a,b;float radius;} parts[]={
        {0,2,.22f},{2,3,.17f},{3,4,.12f},{4,5,.12f},
        {3,40,.12f},{40,41,.10f},{41,42,.09f},{3,60,.12f},{60,61,.10f},{61,62,.09f},
        {0,84,.15f},{84,85,.13f},{85,86,.10f},{86,88,.09f},
        {0,89,.15f},{89,90,.13f},{90,91,.10f},{91,93,.09f}
    };
    float gap=INFINITY;int sampled=0;
    for(int i=0;i<(int)_countof(parts);++i){RR_V3 a,b;
        if(!shot_joint(player,parts[i].a,&a)||!shot_joint(player,parts[i].b,&b))continue;
        float squared=body_ray_distance(origin,ray,a,b);if(!isfinite(squared))continue;
        float value=fmaxf(0,sqrtf(squared)-parts[i].radius);if(value<gap)gap=value;++sampled;
    }
    if(!sampled){float squared=body_ray_distance(origin,ray,player->pos,head);
        if(isfinite(squared))gap=fmaxf(0,sqrtf(squared)-.28f);
    }
    return gap;
}

// Select a nearby head or silent aim target without changing camera input or firing.
void rr_shot_tick(RR_PLAYER* players,int count,const RR_CAMERA* camera,int width,int height,unsigned sampled,int usable){
    BR_Config* config=br_get();SHOT_TARGET target={0};DWORD now=GetTickCount();DWORD sample_now=timeGetTime();
    if((config->always_head_on||config->silent_aim_on) && !InterlockedCompareExchange(&g_stop,0,0) && usable && camera && camera->valid &&
       !config->menu_open && !config->telem_freelook && sample_now-sampled<=200){
        int local_ready=0, local_dead=0;
        for(int i=0;i<count;++i){
            if(!players[i].local)continue;
            if(players[i].dead)local_dead=1;
            else if(players[i].valid && !players[i].inveh)local_ready=1;
        }
        if(local_ready || (config->silent_aim_on && !local_dead)){
            rr_visibility_apply(players,count);
            if(config->silent_aim_on && width>=16 && height>=16){
                int key_ok = !config->silent_key_mode || (GetAsyncKeyState(config->silent_key) & 0x8000) != 0;
                int chance_ok = config->silent_hitchance >= 100 || (rand() % 100) < config->silent_hitchance;
                if(key_ok && chance_ok){
                    float fov=(float)(config->silent_aim_fov>0?config->silent_aim_fov:140);
                    float max_r2=fov*fov, best=INFINITY;
                    const RR_PLAYER* best_p=NULL;
                    for(int i=0;i<count;++i){RR_PLAYER* p=&players[i];RR_V3 aim_pt;
                        if(!p->valid||!p->addr||p->local||p->is_vehicle||p->is_loot||p->hidden_in_object||
                           ((config->dead_check||config->silent_skip_downed)&&(p->dead||p->health==0))||!isfinite(p->dist)||(config->silent_dist>0&&p->dist>config->silent_dist)||
                           (config->ignore_friends&&rr_friends_contains(p->public_id))||
                           !rr_relationship_allowed(p->relationships,config->aa_target_clan,config->aa_target_police))continue;
                        if(config->silent_los_on && p->line_of_sight[0]==RR_LOS_BLOCKED)continue;
                        RR_V3 top;
                        int bone_jnt = config->silent_bone==2 ? RR_RIG_M_SPINE_3_JNT :
                                       config->silent_bone==3 ? RR_RIG_M_SPINE_1_JNT :
                                       config->silent_bone==1 ? RR_RIG_M_NECK_JNT : RR_RIG_M_HEAD_1_JNT;
                        if(shot_joint(p,bone_jnt,&aim_pt)){
                            if(bone_jnt==RR_RIG_M_HEAD_1_JNT && shot_joint(p,RR_RIG_M_HEAD_2_JNT,&top))
                                aim_pt=(RR_V3){(aim_pt.x+top.x)*.5f,(aim_pt.y+top.y)*.5f,(aim_pt.z+top.z)*.5f};
                        }else{aim_pt=p->head;aim_pt.y-=.08f;}
                        float sx,sy;
                        if(!game_w2s(camera,&aim_pt,width,height,&sx,&sy)||!isfinite(sx)||!isfinite(sy))continue;
                        float dx=sx-width*.5f,dy=sy-height*.5f,r2=dx*dx+dy*dy;
                        if(r2>max_r2||r2>=best)continue;
                        RR_V3 direction={aim_pt.x-camera->pos.x,aim_pt.y-camera->pos.y,aim_pt.z-camera->pos.z};
                        float distance=sqrtf(direction.x*direction.x+direction.y*direction.y+direction.z*direction.z);
                        if(!(distance>.05f)||!isfinite(distance))continue;
                        target.yaw=atan2f(direction.x,direction.z);target.pitch=-asinf(fmaxf(-1,fminf(1,direction.y/distance)));
                        target.world=game_world_addr();target.at=now;target.valid=isfinite(target.yaw)&&isfinite(target.pitch);
                        if(target.valid){best=r2;best_p=p;}
                    }
                    if(target.valid && best_p)rr_target_tracer_set_id(best_p->addr,best_p->id,config->silent_bone);
                }
            }
            if(!target.valid && config->always_head_on){
                RR_V3 ray=camera->fwd;
                float length=sqrtf(ray.x*ray.x+ray.y*ray.y+ray.z*ray.z),best=INFINITY;
                if(length>.5f && length<1.5f){ray.x/=length;ray.y/=length;ray.z/=length;
                    for(int i=0;i<count;++i){RR_PLAYER* p=&players[i];RR_V3 head;
                        if(!p->valid||!p->addr||p->local||p->is_vehicle||p->is_loot||p->hidden_in_object||
                           (p->dead&&config->aa_skip_downed)||!isfinite(p->dist)||(config->aa_dist>0&&p->dist>config->aa_dist)||
                           (config->ignore_friends&&rr_friends_contains(p->public_id))||
                           !rr_relationship_allowed(p->relationships,config->aa_target_clan,config->aa_target_police)||
                           p->line_of_sight[0]!=RR_LOS_CLEAR)continue;
                        RR_V3 top;
                        if(shot_joint(p,RR_RIG_M_HEAD_1_JNT,&head)){
                            if(shot_joint(p,RR_RIG_M_HEAD_2_JNT,&top))head=(RR_V3){(head.x+top.x)*.5f,(head.y+top.y)*.5f,(head.z+top.z)*.5f};
                        }else{head=p->head;head.y-=.08f;}
                        float miss=shot_body_gap(p,camera->pos,ray,head);
                        if(!isfinite(miss)||miss>RR_SHOT_NEAR_BODY)continue;
                        RR_V3 direction={head.x-camera->pos.x,head.y-camera->pos.y,head.z-camera->pos.z};
                        float distance=sqrtf(direction.x*direction.x+direction.y*direction.y+direction.z*direction.z);
                        if(!(distance>.05f)||!isfinite(distance))continue;
                        float score=miss+p->dist*.00001f;if(score>=best)continue;
                        target.yaw=atan2f(direction.x,direction.z);target.pitch=-asinf(fmaxf(-1,fminf(1,direction.y/distance)));
                        target.world=game_world_addr();target.at=now;target.valid=isfinite(target.yaw)&&isfinite(target.pitch);best=score;
                    }
                }
            }
        }
    }
    AcquireSRWLockExclusive(&g_target_lock);g_target=target;ReleaseSRWLockExclusive(&g_target_lock);
}

// Start the outgoing shot worker.
void rr_shot_start(void){if(!g_worker){InterlockedExchange(&g_stop,0);g_worker=CreateThread(NULL,0,shot_worker,NULL,0,NULL);}}
// Restore the input callback on normal exit.
void rr_shot_stop(void){InterlockedExchange(&g_stop,1);if(g_worker){WaitForSingleObject(g_worker,5000);CloseHandle(g_worker);g_worker=NULL;}}
