#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <mmsystem.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>
#include "rr_camera.h"
#include "rr_mem.h"
#include "rr_unity.h"
#include "menu_bridge.h"
#include "invoke_camera_mod_offsets.h"
#include "invoke_camera_offsets.h"

typedef struct {uint64_t object,klass,address;float original,last;int used;} LENS_LEASE;
typedef struct {uint64_t owner,klass,address;unsigned bit;} INPUT_REF;
typedef struct {const char* name;unsigned offset,bit;} INPUT_KIND;
static const INPUT_KIND input_kinds[]={
    {"LookInputUpdater",0x10,1},{"PlayerLookRotationUpdater",0x18,2},
    {"PlayerRotationPitchUpdater",0x10,4},{"MoveShooterKeyboardInputUpdater",0x20,8},
    {"MoveShooterTouchInputUpdater",0x28,16},{"PlayerMovementIntentionDirectionUpdater",0x18,32},
    {"PlayerDesiredYawInVehicleUpdater",0x20,64},
    {"AutomaticShotInputUpdater",0x28,128},{"ManualShotInputUpdater",0x28,256}
};
static uint64_t g_world,g_brain,g_input,g_output,g_native,g_output_class,g_player_module;
static DWORD g_validated,g_walked;
static float g_original_fov,g_last_fov,g_original_aspect,g_last_aspect;
static unsigned g_original_proj_mode;
static unsigned char g_original_auto;
static int g_fov_owned,g_aspect_owned,g_proj_mode_owned,g_supported,g_rot_supported;
static LENS_LEASE g_lenses[32];
static int g_lens_count;
static HANDLE g_discovery;
static volatile LONG g_stop;
static SRWLOCK g_refs_lock=SRWLOCK_INIT;
static INPUT_REF g_refs[32];
static int g_ref_count;
static uint64_t g_refs_world,g_refs_input,g_shadow;
static DWORD g_shadow_pid;
static unsigned char g_rotation[RR_CAMERA_ROTATION_BYTES];
static int g_free_active,g_free_blocked;
static int g_fc_active;
static RR_V3 g_fc_pos;
static float g_fc_yaw,g_fc_pitch;
static DWORD g_fc_last_ms;
static POINT g_fc_last_cursor;
static int g_fc_has_cursor;
static float g_zoom_weight;
static DWORD g_zoom_at;

// Validate the installed native camera layout.
static int supported_player(void){
    uint32_t size=0;uint64_t module=0;
    g_rot_supported=0;
    if(!rr_module("UnityPlayer.dll",&module,&size) || !PL(module))return 0;
    g_player_module=module;
    unsigned char sig[16];
    if(rr_module("GameAssembly.dll",&module,&size) &&
       rr_read(module+RR_CAMERA_ROTATION_SETTER,sig,sizeof(rr_camera_rotation_signature)) &&
       memcmp(sig,rr_camera_rotation_signature,sizeof(rr_camera_rotation_signature))==0)
        g_rot_supported=1;
    return 1;
}

// Restore changed virtual camera lenses.
static void restore_lenses(void){
    for(int i=0;i<g_lens_count;++i){LENS_LEASE* lease=&g_lenses[i];float current;
        if(lease->used && rr_u64(lease->object)==lease->klass && rr_read(lease->address,&current,4) && fabsf(current-lease->last)<.01f)
            rr_write(lease->address,&lease->original,4);
    }
    g_lens_count=0;
}

// Invalidate native projection caches.
static int dirty_projection(void){
    unsigned short flags=0x101;
    return rr_write(g_native+RR_CAMERA_NATIVE_PROJECTION_DIRTY,&flags,sizeof(flags));
}

// Restore native lens values on release.
static void restore_native(void){
    if(PL(g_output) && rr_u64(g_output)==g_output_class && rr_u64(g_output+0x10)==g_native){
        float value;int changed=0;
        if(g_fov_owned && rr_read(g_native+RR_CAMERA_NATIVE_FOV,&value,4) && fabsf(value-g_last_fov)<.01f)
            changed|=rr_write(g_native+RR_CAMERA_NATIVE_FOV,&g_original_fov,4);
        if(g_aspect_owned && rr_read(g_native+RR_CAMERA_NATIVE_ASPECT,&value,4) && fabsf(value-g_last_aspect)<.001f){
            changed|=rr_write(g_native+RR_CAMERA_NATIVE_ASPECT,&g_original_aspect,4);
            rr_write(g_native+RR_CAMERA_NATIVE_AUTO_ASPECT,&g_original_auto,1);
        }
        if(g_proj_mode_owned){
            changed|=rr_write(g_native+RR_CAMERA_NATIVE_PROJECTION_MODE,&g_original_proj_mode,4);
        }
        if(changed)dirty_projection();
    }
    g_fov_owned=g_aspect_owned=g_proj_mode_owned=0;restore_lenses();
}

// Restore live input references before returning the camera.
static void release_freelook(void){
    g_fc_active=0;g_fc_has_cursor=0;
    if(!g_free_active)return;
    if(g_shadow_pid==g_rr_pid && PL(g_input) && rr_u64(g_input)==*(uint64_t*)g_rotation)
        rr_write(g_input+0x10,g_rotation+0x10,sizeof(g_rotation)-0x10);
    AcquireSRWLockShared(&g_refs_lock);
    for(int i=0;i<g_ref_count;++i){const INPUT_REF* ref=&g_refs[i];
        if(rr_u64(ref->owner)==ref->klass && rr_u64(ref->address)==g_shadow)rr_write(ref->address,&g_input,8);
    }
    ReleaseSRWLockShared(&g_refs_lock);
    g_free_active=0;br_get()->telem_freelook=0;
}

// Check that both facing and movement can stay independent.
static int input_complete(unsigned mask){return (mask&39)==39 && (mask&24)!=0;}

// Order scan regions around the local input model.
typedef struct {RR_CAMERA_REGION region;uint64_t distance;} INPUT_REGION;
static int region_compare(const void* a,const void* b){
    uint64_t x=((const INPUT_REGION*)a)->distance,y=((const INPUT_REGION*)b)->distance;
    return x<y?-1:x>y?1:0;
}

// Find typed references without blocking rendering.
static DWORD WINAPI discover_input(LPVOID unused){
    (void)unused;static unsigned char data[0x10000];
    while(!InterlockedCompareExchange(&g_stop,0,0)){
        BR_Config* config=br_get();uint64_t world=0,brain=0,input=0;
        if(!config->freelook_on || config->telem_freelook || !game_camera_context(&world,&brain,&input)) {Sleep(100);continue;}
        AcquireSRWLockShared(&g_refs_lock);
        unsigned existing=0;
        if(g_refs_world==world && g_refs_input==input)for(int i=0;i<g_ref_count;++i)
            if(rr_u64(g_refs[i].owner)==g_refs[i].klass && rr_u64(g_refs[i].address)==input)existing|=g_refs[i].bit;
        ReleaseSRWLockShared(&g_refs_lock);
        if(input_complete(existing)){Sleep(250);continue;}
        RR_CAMERA_REGION source[512];INPUT_REGION regions[512];
        int count=game_camera_regions(input,source,512);
        for(int i=0;i<count;++i){regions[i].region=source[i];uint64_t base=source[i].base;regions[i].distance=base>input?base-input:input-base;}
        qsort(regions,count,sizeof(regions[0]),region_compare);
        INPUT_REF found[32];int found_count=0;unsigned mask=0;
        for(int r=0;r<count && !input_complete(mask);++r){
            RR_CAMERA_REGION region=regions[r].region;
            for(uint64_t offset=0;offset<region.size;offset+=sizeof(data)){
                if(InterlockedCompareExchange(&g_stop,0,0)||!config->freelook_on||config->telem_freelook||world!=game_world_addr())goto finished;
                size_t bytes=(size_t)(region.size-offset);if(bytes>sizeof(data))bytes=sizeof(data);
                if(!rr_read(region.base+offset,data,bytes))continue;
                for(size_t at=0;at+8<=bytes;at+=8){uint64_t value;memcpy(&value,data+at,8);if(value!=input)continue;
                    for(int k=0;k<(int)_countof(input_kinds);++k){const INPUT_KIND* kind=&input_kinds[k];
                        uint64_t address=region.base+offset+at;if(address<kind->offset)continue;
                        uint64_t owner=address-kind->offset,klass=rr_u64(owner);char name[96];
                        if(!PL(klass)||!u_class_name(klass,name,sizeof(name))||strcmp(name,kind->name))continue;
                        int duplicate=0;for(int j=0;j<found_count;++j)if(found[j].address==address)duplicate=1;
                        if(!duplicate && found_count<32){found[found_count++]=(INPUT_REF){owner,klass,address,kind->bit};mask|=kind->bit;}
                    }
                }
                if((offset&0xfffff)==0)Sleep(1);
            }
        }
finished:
        if(world==game_world_addr() && !config->telem_freelook){
            AcquireSRWLockExclusive(&g_refs_lock);
            if(!config->telem_freelook){memcpy(g_refs,found,found_count*sizeof(found[0]));g_ref_count=found_count;g_refs_input=input;g_refs_world=world;}
            ReleaseSRWLockExclusive(&g_refs_lock);
        }
        Sleep(100);
    }
    return 0;
}

// Keep camera rotation live while other consumers use a frozen model.
static int begin_freelook(void){
    if(!PL(g_input) || !u_is(g_input,"PlayerRotationInputModel"))return 0;
    AcquireSRWLockExclusive(&g_refs_lock);
    unsigned mask=0;
    for(int i=0;i<g_ref_count;++i)if(g_refs_world==g_world && g_refs_input==g_input &&
        rr_u64(g_refs[i].owner)==g_refs[i].klass && rr_u64(g_refs[i].address)==g_input)mask|=g_refs[i].bit;
    if(!input_complete(mask)){ReleaseSRWLockExclusive(&g_refs_lock);return 0;}
    if(g_shadow_pid!=g_rr_pid){g_shadow=0;g_shadow_pid=g_rr_pid;}
    int ok=rr_read(g_input,g_rotation,sizeof(g_rotation));
    float* angles=(float*)(g_rotation+0x10);
    if(!isfinite(angles[0])||!isfinite(angles[1])||!isfinite(angles[2]))ok=0;
    if(ok && !g_shadow)ok=rr_drv_alloc_remote(g_rr_pid,0x1000,PAGE_READWRITE,&g_shadow);
    if(ok){unsigned char frozen[RR_CAMERA_ROTATION_BYTES];memcpy(frozen,g_rotation,sizeof(frozen));
        memset(frozen+8,0,8);ok=rr_write(g_shadow,frozen,sizeof(frozen));}
    int patched=0;unsigned patched_mask=0;
    if(ok)for(int i=0;i<g_ref_count;++i){const INPUT_REF* ref=&g_refs[i];
        if(rr_u64(ref->owner)!=ref->klass || rr_u64(ref->address)!=g_input)continue;
        if(!rr_write(ref->address,&g_shadow,8)){ok=0;break;}++patched;patched_mask|=ref->bit;
    }
    if(!ok || !patched || !input_complete(patched_mask)){
        for(int i=0;i<g_ref_count;++i)
            if(rr_u64(g_refs[i].owner)==g_refs[i].klass && rr_u64(g_refs[i].address)==g_shadow)rr_write(g_refs[i].address,&g_input,8);
    }else{g_free_active=1;br_get()->telem_freelook=1;}
    ReleaseSRWLockExclusive(&g_refs_lock);return g_free_active;
}

// Register a reversible virtual lens change.
static void add_lens(uint64_t object){
    for(int i=0;i<g_lens_count;++i)if(g_lenses[i].object==object)return;
    if(g_lens_count>=32)return;
    char name[96];
    if(!u_class_name(rr_u64(object),name,sizeof(name))||(!strstr(name,"VirtualCamera")&&!strstr(name,"CinemachineCamera")))return;
    uint64_t klass=rr_u64(object);float fov;
    if(!rr_read(object+RR_CAMERA_VCAM_LENS,&fov,4)||!isfinite(fov)||fov<5||fov>170)return;
    g_lenses[g_lens_count++]=(LENS_LEASE){object,klass,object+RR_CAMERA_VCAM_LENS,fov,fov,1};
}

// Read a managed camera collection.
static int camera_children(uint64_t collection,uint64_t* items){
    if(!PL(collection))return 0;
    uint64_t length=rr_u64(collection+0x18);
    if(length>0 && length<=32)return rr_read(collection+0x20,items,(size_t)length*8)?(int)length:0;
    int count=rr_i32(collection+0x18);uint64_t array=rr_u64(collection+0x10);
    if(count<1||count>32||!PL(array)||rr_u64(array+0x18)<(uint64_t)count)return 0;
    return rr_read(array+0x20,items,(size_t)count*8)?count:0;
}

// Follow active camera blends and mixers.
static void walk_cameras(uint64_t object,int depth,uint64_t* visited,int* count){
    if(!PL(object)||depth>7||*count>=48)return;
    for(int i=0;i<*count;++i)if(visited[i]==object)return;visited[(*count)++]=object;
    char name[96];if(!u_class_name(rr_u64(object),name,sizeof(name)))return;
    if(strstr(name,"VirtualCamera")||strstr(name,"CinemachineCamera")){add_lens(object);return;}
    if(!strcmp(name,"CinemachineBlend")){
        walk_cameras(rr_u64(object+0x10),depth+1,visited,count);walk_cameras(rr_u64(object+0x18),depth+1,visited,count);return;
    }
    if(!strcmp(name,"BlendSourceVirtualCamera")){walk_cameras(rr_u64(object+0x10),depth+1,visited,count);return;}
    unsigned children=0;
    if(!strcmp(name,"MixingCamera")||!strcmp(name,"ZoomCamera"))children=RR_CAMERA_MIXING_CHILDREN;
    else if(!strcmp(name,"CinemachineMixingCamera"))children=0x1c8;
    if(children){uint64_t items[32];int n=camera_children(rr_u64(object+children),items);
        for(int i=0;i<n;++i)walk_cameras(items[i],depth+1,visited,count);
    }
    if(!strcmp(name,"ZoomCamera")){
        walk_cameras(rr_u64(object+RR_CAMERA_ZOOM_IN_CAMERA),depth+1,visited,count);
        walk_cameras(rr_u64(object+RR_CAMERA_ZOOM_OUT_CAMERA),depth+1,visited,count);
    }
}

// Apply virtual lenses with individual read status.
static int apply_lenses(float desired){
    RR_RD_REQ requests[64];uint64_t klass[32];float values[32];int ok=1;
    for(int i=0;i<g_lens_count;++i){klass[i]=0;values[i]=NAN;
        requests[i*2]=(RR_RD_REQ){g_lenses[i].object,8,&klass[i],0};
        requests[i*2+1]=(RR_RD_REQ){g_lenses[i].address,4,&values[i],0};
    }
    if(g_lens_count && rr_read_batch(requests,g_lens_count*2)<0)
        for(int i=0;i<g_lens_count*2;++i)rr_read(requests[i].addr,requests[i].dst,requests[i].size);
    for(int i=0;i<g_lens_count;++i){LENS_LEASE* lease=&g_lenses[i];float value=values[i];
        if(klass[i]!=lease->klass || !isfinite(value)||value<5||value>170)continue;
        if(fabsf(value-lease->last)>.01f)lease->original=value;
        if(fabsf(value-desired)>.005f && !rr_write(lease->address,&desired,4))ok=0;
        lease->last=desired;
    }
    return ok;
}

// Drive 3D freecam position and orientation in CinemachineBrain & local camera.
static void update_freecam(RR_CAMERA* camera,const BR_Config* config,DWORD now){
    if(!camera||!camera->valid)return;
    if(!g_fc_active){
        g_fc_pos=camera->pos;
        float fy=camera->fwd.y;if(fy>1.0f)fy=1.0f;if(fy<-1.0f)fy=-1.0f;
        g_fc_pitch=asinf(fy);
        g_fc_yaw=atan2f(camera->fwd.x,camera->fwd.z);
        g_fc_last_ms=now;
        g_fc_has_cursor=GetCursorPos(&g_fc_last_cursor)?1:0;
        g_fc_active=1;
    }
    float dt=g_fc_last_ms?(float)(now-g_fc_last_ms)*0.001f:0.005f;
    if(dt<0.0f)dt=0.0f;if(dt>0.05f)dt=0.05f;
    g_fc_last_ms=now;

    POINT cur;
    if(GetCursorPos(&cur)){
        if(g_fc_has_cursor){
            float dx=(float)(cur.x-g_fc_last_cursor.x);
            float dy=(float)(cur.y-g_fc_last_cursor.y);
            if(fabsf(dx)<300.0f && fabsf(dy)<300.0f){
                g_fc_yaw+=dx*0.0028f;
                g_fc_pitch-=dy*0.0028f;
            }
        }
        g_fc_last_cursor=cur;
        g_fc_has_cursor=1;
    }
    if(GetAsyncKeyState(VK_LEFT)&0x8000)g_fc_yaw-=1.8f*dt;
    if(GetAsyncKeyState(VK_RIGHT)&0x8000)g_fc_yaw+=1.8f*dt;
    if(GetAsyncKeyState(VK_UP)&0x8000)g_fc_pitch+=1.4f*dt;
    if(GetAsyncKeyState(VK_DOWN)&0x8000)g_fc_pitch-=1.4f*dt;
    if(g_fc_pitch>1.54f)g_fc_pitch=1.54f;
    if(g_fc_pitch<-1.54f)g_fc_pitch=-1.54f;

    float cp=cosf(g_fc_pitch),sp=sinf(g_fc_pitch),cy=cosf(g_fc_yaw),sy=sinf(g_fc_yaw);
    RR_V3 fwd={cp*sy,sp,cp*cy};
    RR_V3 right={cy,0.0f,-sy};
    RR_V3 up={right.y*fwd.z-right.z*fwd.y,right.z*fwd.x-right.x*fwd.z,right.x*fwd.y-right.y*fwd.x};

    float speed=((float)(config->freecam_speed>0?config->freecam_speed:1200)/100.0f);
    if(GetAsyncKeyState(VK_SHIFT)&0x8000)speed*=2.5f;
    if(GetAsyncKeyState(VK_MENU)&0x8000)speed*=0.35f;

    RR_V3 move={0,0,0};
    if(GetAsyncKeyState('W')&0x8000){move.x+=fwd.x;move.y+=fwd.y;move.z+=fwd.z;}
    if(GetAsyncKeyState('S')&0x8000){move.x-=fwd.x;move.y-=fwd.y;move.z-=fwd.z;}
    if(GetAsyncKeyState('D')&0x8000){move.x+=right.x;move.y+=right.y;move.z+=right.z;}
    if(GetAsyncKeyState('A')&0x8000){move.x-=right.x;move.y-=right.y;move.z-=right.z;}
    if(GetAsyncKeyState(VK_SPACE)&0x8000)move.y+=1.0f;
    if((GetAsyncKeyState(VK_LCONTROL)&0x8000)||(GetAsyncKeyState('C')&0x8000))move.y-=1.0f;
    float mlen=sqrtf(move.x*move.x+move.y*move.y+move.z*move.z);
    if(mlen>1e-4f){
        float step=speed*dt/mlen;
        g_fc_pos.x+=move.x*step;g_fc_pos.y+=move.y*step;g_fc_pos.z+=move.z*step;
    }

    float half_y=g_fc_yaw*0.5f,half_p=-g_fc_pitch*0.5f;
    float cy2=cosf(half_y),sy2=sinf(half_y),cp2=cosf(half_p),sp2=sinf(half_p);
    RR_Q4 q={sp2*cy2,cp2*sy2,-sp2*sy2,cp2*cy2};
    if(PL(g_brain)){
        RR_V3 zero={0,0,0};RR_Q4 ident={0,0,0,1};
        rr_write(g_brain+RR_CM_BRAIN_STATE+RR_CM_STATE_POSITION,&g_fc_pos,sizeof(g_fc_pos));
        rr_write(g_brain+RR_CM_BRAIN_STATE+RR_CM_STATE_ROTATION,&q,sizeof(q));
        rr_write(g_brain+RR_CM_BRAIN_STATE+RR_CM_STATE_POSITION_CORRECTION,&zero,sizeof(zero));
        rr_write(g_brain+RR_CM_BRAIN_STATE+RR_CM_STATE_ROTATION_CORRECTION,&ident,sizeof(ident));
    }
    camera->pos=g_fc_pos;camera->fwd=fwd;camera->right=right;camera->up=up;camera->rot=q;
}

// Compose game FOV, custom FOV and temporary magnification.
void rr_camera_tick(RR_CAMERA* camera,int focused){
    static int f3_prev=0;
    BR_Config* config=br_get();DWORD now=timeGetTime();
    int usable=focused&&!config->menu_open&&camera&&camera->valid;
    int f3_down=focused && !config->menu_open && (((GetAsyncKeyState(VK_F3)&0x8000)!=0) ||
                (config->freelook_key>0 && (GetAsyncKeyState(config->freelook_key)&0x8000)!=0));
    if(f3_down && !f3_prev){config->freelook_on=!config->freelook_on;}
    f3_prev=f3_down;
    int key=config->freelook_on;
    if(!key)g_free_blocked=0;
    if(!usable){if(key)g_free_blocked=1;release_freelook();}
    if(!config->freelook_on)release_freelook();
    if(config->freelook_on && !g_discovery){InterlockedExchange(&g_stop,0);g_discovery=CreateThread(NULL,0,discover_input,NULL,0,NULL);}
    uint64_t world=game_world_addr();
    if(world!=g_world || now-g_validated>=250 || !PL(g_brain)){
        uint64_t next_world=0,next_brain=0,next_input=0;
        int bound=game_camera_context(&next_world,&next_brain,&next_input);
        if(!bound){release_freelook();restore_native();g_brain=g_native=0;config->telem_camera_mod=1;return;}
        if(next_world!=g_world || next_brain!=g_brain || next_input!=g_input){release_freelook();restore_native();g_output=g_native=0;}
        g_world=next_world;g_brain=next_brain;g_input=next_input;g_validated=now;
        uint64_t output=rr_u64(g_brain+RR_CAMERA_BRAIN_OUTPUT),native=rr_u64(output+0x10);
        if(output!=g_output || native!=g_native){restore_native();g_output=output;g_native=native;g_output_class=rr_u64(output);g_supported=supported_player();}
        if(!u_is(output,"Camera")||!PL(native)){g_native=0;config->telem_camera_mod=1;return;}
    }
    if(g_supported && usable && config->freelook_on && key && !g_free_blocked){
        if(!g_free_active && PL(g_input))begin_freelook();
        update_freecam(camera,config,now);
    }
    config->telem_freelook=(g_free_active || g_fc_active)?1:0;
    AcquireSRWLockShared(&g_refs_lock);unsigned ready=0;
    if(g_refs_world==g_world && g_refs_input==g_input)for(int i=0;i<g_ref_count;++i)ready|=g_refs[i].bit;
    ReleaseSRWLockShared(&g_refs_lock);config->telem_freelook_ready=input_complete(ready);
    if(!g_supported || !PL(g_native) || rr_u64(g_output+0x10)!=g_native){config->telem_camera_mod=g_supported?1:3;return;}
    float fov=NAN,rect[4]={0};unsigned char native_state[0x34]={0};
    RR_RD_REQ reads[]={{g_native+RR_CAMERA_NATIVE_FOV,4,&fov,0},
        {g_native+RR_CAMERA_NATIVE_RECT,sizeof(rect),rect,0},
        {g_native+RR_CAMERA_NATIVE_ASPECT,sizeof(native_state),native_state,0}};
    int read=rr_read_batch(reads,3);if(read<0){read=0;for(int i=0;i<3;++i)read+=rr_read(reads[i].addr,reads[i].dst,reads[i].size);}
    float aspect;unsigned mode;memcpy(&aspect,native_state,4);memcpy(&mode,native_state+0x24,4);
    if(read!=3||!isfinite(fov)||fov<5||fov>170||!isfinite(aspect)||aspect<.25f||aspect>8){config->telem_camera_mod=1;return;}
    if(!g_fov_owned || fabsf(fov-g_last_fov)>.01f)g_original_fov=fov;
    if(!g_aspect_owned || fabsf(aspect-g_last_aspect)>.001f){g_original_aspect=aspect;g_original_auto=native_state[0x32];}
    int held=usable && config->zoom_on && (GetAsyncKeyState(config->zoom_key)&0x8000);
    float dt=g_zoom_at?(float)(now-g_zoom_at):1;g_zoom_at=now;
    if(!usable || !config->zoom_on)g_zoom_weight=0;
    else if(config->zoom_smooth){float step=dt/config->zoom_smooth;g_zoom_weight=held?fminf(1,g_zoom_weight+step):fmaxf(0,g_zoom_weight-step);}
    else g_zoom_weight=held?1.0f:0.0f;
    int fov_active=config->fov_on && config->camera_fov>0;
    int change_fov=camera && camera->valid && (fov_active || g_zoom_weight>.0001f);
    int change_aspect=camera && camera->valid && config->aspect_on && config->aspect_pct>0;
    int ok=1,changed=0;
    if((change_fov || change_aspect) && mode!=0){
        if(!g_proj_mode_owned){g_original_proj_mode=mode;g_proj_mode_owned=1;}
        unsigned zero=0;
        ok&=rr_write(g_native+RR_CAMERA_NATIVE_PROJECTION_MODE,&zero,4);
        changed=1;
    }else if(!change_fov && !change_aspect && g_proj_mode_owned){
        ok&=rr_write(g_native+RR_CAMERA_NATIVE_PROJECTION_MODE,&g_original_proj_mode,4);
        g_proj_mode_owned=0;changed=1;
    }
    if(change_fov){
        float base=fov_active?(float)config->camera_fov:g_original_fov;
        float power=1+(config->zoom_power/100.0f-1)*g_zoom_weight;
        float desired=2*atanf(tanf(base*3.14159265f/360)/power)*180/3.14159265f;desired=fmaxf(5,fminf(140,desired));
        if(now-g_walked>=100 || !g_lens_count){uint64_t visited[48];int count=0;
            walk_cameras(rr_u64(g_brain+RR_CAMERA_BRAIN_BLEND),0,visited,&count);
            walk_cameras(rr_u64(g_brain+RR_CAMERA_BRAIN_ACTIVE),0,visited,&count);g_walked=now;
        }
        ok&=apply_lenses(desired);
        if(PL(g_brain))rr_write(g_brain+RR_CM_BRAIN_STATE+RR_CM_LENS_FOV,&desired,4);
        ok&=rr_write(g_native+RR_CAMERA_NATIVE_FOV,&desired,4);
        changed=1;
        if(fabsf(fov-desired)<=0.05f)fov=desired;
        g_last_fov=desired;g_fov_owned=1;
    }else if(g_fov_owned){
        restore_lenses();if(fabsf(fov-g_last_fov)<.01f){ok&=rr_write(g_native+RR_CAMERA_NATIVE_FOV,&g_original_fov,4);fov=g_original_fov;changed=1;}g_fov_owned=0;
    }
    if(change_aspect){
        float desired=g_original_aspect*((float)config->aspect_pct/100.0f);
        if(desired<.25f)desired=.25f;if(desired>8.0f)desired=8.0f;
        unsigned char automatic=0;
        ok&=rr_write(g_native+RR_CAMERA_NATIVE_ASPECT,&desired,4);
        ok&=rr_write(g_native+RR_CAMERA_NATIVE_AUTO_ASPECT,&automatic,1);
        if(PL(g_brain)){
            float sensor[2]={0,0};
            if(rr_read(g_brain+RR_CM_BRAIN_STATE+RR_CM_LENS_SENSOR,sensor,sizeof(sensor)) && isfinite(sensor[1]) && sensor[1]>0.1f){
                sensor[0]=sensor[1]*desired;
                rr_write(g_brain+RR_CM_BRAIN_STATE+RR_CM_LENS_SENSOR,sensor,sizeof(sensor));
            }
        }
        changed=1;
        if(fabsf(aspect-desired)<=0.05f)aspect=desired;
        g_last_aspect=desired;g_aspect_owned=1;
    }else if(g_aspect_owned){
        if(fabsf(aspect-g_last_aspect)<.001f){ok&=rr_write(g_native+RR_CAMERA_NATIVE_ASPECT,&g_original_aspect,4);
            ok&=rr_write(g_native+RR_CAMERA_NATIVE_AUTO_ASPECT,&g_original_auto,1);aspect=g_original_aspect;changed=1;}
        g_aspect_owned=0;
    }
    if(changed)ok&=dirty_projection();
    if(camera){camera->fov=fov;camera->aspect=aspect;camera->viewport_aspect=g_aspect_owned?g_original_aspect:aspect;camera->viewport_valid=0;
        if(isfinite(rect[0])&&isfinite(rect[1])&&isfinite(rect[2])&&isfinite(rect[3])&&rect[0]>=0&&rect[1]>=0&&rect[2]>.01f&&rect[3]>.01f&&rect[0]+rect[2]<=1.01f&&rect[1]+rect[3]<=1.01f){
            memcpy(camera->viewport_rect,rect,sizeof(rect));camera->viewport_valid=fabsf(rect[0])+fabsf(rect[1])+fabsf(rect[2]-1)+fabsf(rect[3]-1)>.0001f;
        }
    }
    config->telem_camera_fov=(int)(fov*100);config->telem_camera_aspect=(int)(aspect*1000);
    if(camera){LARGE_INTEGER stamp;QueryPerformanceCounter(&stamp);camera->sampled_qpc=(uint64_t)stamp.QuadPart;camera->sampled_at=timeGetTime();}
    config->telem_zoom=held;config->telem_camera_mod=ok?2:4;
}

// Release all temporary camera and input changes.
void rr_camera_shutdown(void){
    InterlockedExchange(&g_stop,1);release_freelook();restore_native();
    if(g_discovery){WaitForSingleObject(g_discovery,3000);CloseHandle(g_discovery);g_discovery=NULL;}
}
