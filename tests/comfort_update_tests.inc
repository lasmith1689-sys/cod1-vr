#include "vr_handling.h"

static float CUAngle(const float a[9], const float b[9])
{
    float d=0; for(int i=0;i<9;++i) d+=a[i]*b[i];
    return acosf(VRHandlingClamp((d-1)*0.5f,-1,1))*57.2957795f;
}
static void S_ComfortHandling()
{
    printf("--- comfort: supported weight, recoil, pivot and aim contract ---\n");
    float ident[9], turn[9], out[9]; NHIdentityAxis(ident);
    HpAxisFromEuler(0,10,0,turn);
    VRHandlingFilter f={};
    VRHandlingFilterRows(&f,ident,1.0f/90,0.75f,1,out);
    VRHandlingFilterRows(&f,turn,1.0f/90,0.75f,1,out);
    CHECK(CUAngle(ident,out)>0.1f && CUAngle(ident,out)<6,
          "supported turn moves gradually instead of jumping 10 deg (%.2f)",CUAngle(ident,out));
    for(int i=0;i<90;++i) VRHandlingFilterRows(&f,turn,1.0f/90,0.75f,1,out);
    CHECK(CUAngle(turn,out)<0.1f,"settles to the requested orientation without drift");
    float maxJitter=0;
    for(int i=0;i<180;++i) {
        HpAxisFromEuler(0,10+((i&1)?1.0f:-1.0f),0,turn);
        VRHandlingFilterRows(&f,turn,1.0f/90,0.75f,1,out);
        float center[9]; HpAxisFromEuler(0,10,0,center);
        maxJitter=fmaxf(maxJitter,CUAngle(center,out));
    }
    CHECK(maxJitter<0.4f,"one-degree tremor is reduced by over 60 percent (%.3f deg)",maxJitter);
    VRHandlingFilterRows(&f,ident,1.0f/90,0.75f,0,out);
    CHECK(memcmp(out,ident,sizeof(out))==0 && !f.valid,"release restores direct orientation immediately");
    HpAxisFromEuler(0,180,0,turn);
    VRHandlingFilterRows(&f,turn,1.0f/90,0.75f,1,out);
    CHECK(memcmp(out,turn,sizeof(out))==0,"reacquire seeds the current pose, never stale history");
    VRHandlingFilterRows(&f,ident,0.4f,0.75f,1,out);
    CHECK(memcmp(out,ident,sizeof(out))==0,"a long frame gap resets instead of dragging the old pose");
    HpAxisFromEuler(0,0,10,turn);
    VRHandlingBlendRows(ident,turn,0.20f,out);
    CHECK(fabsf(CUAngle(ident,out)-2.0f)<0.05f,"80 percent kick reduction retains 2 of 10 degrees");
    // Production two-hand fixture: measure opposite support-hand jitter with
    // the filter on/off, and compare the published bore to the rendered axis.
    float travel[2]={};
    for(int mode=0;mode<2;++mode) {
        THRig r; if(!THBringUp(&r)) { CHECK(0,"handling fixture");return; }
        r.in.valid=r.in.focused=1;
        VRNH_SetTwoHandTuning(0.12f,0.25f,0.15f,0.0f);
        VRNH_SetHandling(mode?0.75f:0.0f,0.80f);
        // This test is about the physical palm pivot; the drawn hand riding
        // the filtered weapon (vr_trigger_hand_lock) is covered by
        // S_TriggerHandRidesGun.
        VRNH_SetTriggerHandLock(0);
        float sup[3],bore[3],up[3];
        if(!THSupportEyeish(&r,sup,bore,up)) { CHECK(0,"support point");return; }
        THPlaceLeft(&r,sup,1);
        for(int i=0;i<12;++i) THPair(&r);
        float previous[3]={};
        VRNHDebug d;
        for(int i=0;i<80;++i) {
            float p[3]; for(int k=0;k<3;++k) p[k]=sup[k]+((i&1)?0.12f:-0.12f)*up[k];
            THPlaceLeft(&r,p,1); THPair(&r); VRNH_GetDebug(&d);
            if(i>10) for(int k=0;k<3;++k) travel[mode]+=fabsf(d.gun.barrelRoom[k]-previous[k]);
            memcpy(previous,d.gun.barrelRoom,sizeof(previous));
        }
        CHECK(d.gun.twoHandState==VRNH_2H_ATTACHED,"stabilization retains support ownership");
        float actual[3],room[3];
        VRM_LocalToWorldVec(VRObj_Set()->gun.axis,r.gun->boreFwd,actual);
        VRM_GLEyeToQuakeVec(actual,room);
        const float len=sqrtf(PADot(room,room)); for(int k=0;k<3;++k) room[k]/=len;
        CHECK(HpDist(room,d.gun.barrelRoom)<0.001f,"shot target follows the rendered filtered bore");
        const float* palm=VRObj_Set()->hand[VROBJ_HAND_R].pos;
        CHECK(HpDist(palm,d.hand[VRI_RIGHT].targetEyeish)<0.001f,"right palm pivot stays controller-owned");
        r.in.hand[VRI_LEFT].squeezeDown=0; THPair(&r); VRNH_GetDebug(&d);
        CHECK(d.gun.twoHandState==VRNH_2H_LEFT_FREE,"release exits assisted support");
    }
    CHECK(travel[1]<travel[0]*0.4f,"production jitter travel %.4f -> %.4f (over 60 percent reduction)",travel[0],travel[1]);
    float kick[2]={};
    for(int mode=0;mode<2;++mode) {
        THRig r; if(!THBringUp(&r)) { CHECK(0,"recoil fixture");return; }
        r.in.valid=r.in.focused=1;
        VRNH_SetTwoHandTuning(0.12f,0.25f,0.15f,0.0f);
        VRNH_SetHandling(0,mode?0.80f:0.0f);
        float sup[3],bore[3],up[3]; THSupportEyeish(&r,sup,bore,up);
        THPlaceLeft(&r,sup,1); for(int i=0;i<20;++i) THPair(&r);
        float rest[9]; memcpy(rest,VRObj_Set()->gun.axis,sizeof(rest));
        r.in.hand[VRI_RIGHT].triggerDown=1;
        HpAxisFromEuler(0,0,10,r.gAxis0); THPair(&r);
        kick[mode]=CUAngle(rest,VRObj_Set()->gun.axis);
    }
    CHECK(kick[0]>0.5f && kick[1]<kick[0]*0.4f,
          "production supported firing kick %.2f -> %.2f degrees",kick[0],kick[1]);
    VRNH_TestReset();
}

static void S_ComfortBindings()
{
    printf("--- comfort controls: X tap/hold and turning off ---\n");
    ResetAll(); if(!BringUpNative()) { CHECK(0,"controls stack");return; }
    VRBinds_SetWeaponCycleBinding(2);
    StepNativeFrames(120,0); SinkClear();
    g_mock.btnX=true; StepNativeFrames(10,0);
    CHECK(CountEvents(VRB_EV_KEYDOWN,'R')==0 && CountEvents(VRB_EV_WHEEL,-1)==0,
          "X press waits for tap/hold intent");
    g_mock.btnX=false; StepNativeFrames(4,0);
    CHECK(CountEvents(VRB_EV_KEYDOWN,'R')==1 && CountEvents(VRB_EV_WHEEL,-1)==0,
          "X tap reloads exactly once and never switches weapons");
    SinkClear(); g_mock.btnX=true; StepNativeFrames(150,0);
    g_mock.btnX=false; StepNativeFrames(4,0);
    CHECK(CountEvents(VRB_EV_WHEEL,-1)==1 && CountEvents(VRB_EV_KEYDOWN,'R')==0,
          "X hold switches once and never reloads on release");
    SinkClear(); g_mock.btnX=true; StepNativeFrames(10,0);
    StepNativeFrames(10,1); g_mock.btnX=false; StepNativeFrames(10,1);
    CHECK(CountEvents(VRB_EV_KEYDOWN,'R')==0 && CountEvents(VRB_EV_WHEEL,-1)==0,
          "menu entry cancels a pending X tap");
    StepNativeFrames(120,0); VRBinds_SetTurnMode(2);
    VRBindsCounters before,after; VRBinds_GetCounters(&before);
    g_mock.stickX[VRI_RIGHT]=1; StepNativeFrames(60,0);
    g_mock.stickX[VRI_RIGHT]=0; VRBinds_GetCounters(&after);
    CHECK(after.turnEvents==before.turnEvents,"turning OFF ignores the turn stick");
    VRBinds_GetCounters(&before);
    g_mock.stickClick[VRI_LEFT]=true; StepNativeFrames(100,0);
    g_mock.stickClick[VRI_LEFT]=false; StepNativeFrames(4,0);
    VRBinds_GetCounters(&after);
    CHECK(after.recenters==before.recenters,"holding the options shortcut does not recenter the world");
    CHECK(g_mock.violations==0,"new controls keep the XR protocol valid");
}

static void CUSaveMenuPreview(const char* name)
{
    char dir[MAX_PATH];
    if(!GetEnvironmentVariableA("CODVR_REVIEW_DIR",dir,sizeof(dir))) return;
    typedef void (WINAPI *Bind)(unsigned,unsigned);
    typedef void (WINAPI *Read)(unsigned,int,unsigned,unsigned,void*);
    typedef void (WINAPI *Get)(unsigned,int*);
    typedef void (WINAPI *Store)(unsigned,int);
    Bind bind=(Bind)CODVR_GetGLProc("glBindTexture");
    Read read=(Read)CODVR_GetGLProc("glGetTexImage");
    Get get=(Get)CODVR_GetGLProc("glGetIntegerv");
    Store store=(Store)CODVR_GetGLProc("glPixelStorei");
    if(!bind||!read||!get||!store) return;
    int w,h; VR_EyeTextureSizeFor(0,&w,&h);
    unsigned char* raw=(unsigned char*)malloc((size_t)w*h*3);
    unsigned char* top=(unsigned char*)malloc((size_t)w*h*3);
    if(!raw||!top) { free(raw);free(top);return; }
    int tex,pack; get(0x8069,&tex);get(0x0D05,&pack);
    store(0x0D05,1); bind(0x0DE1,VR_EyeTexture(0));
    read(0x0DE1,0,0x1907,0x1401,raw);
    bind(0x0DE1,(unsigned)tex); store(0x0D05,pack);
    for(int y=0;y<h;++y) memcpy(top+(size_t)y*w*3,raw+(size_t)(h-1-y)*w*3,(size_t)w*3);
    char path[MAX_PATH]; snprintf(path,sizeof(path),"%s\\%s.png",dir,name);
    CHECK(VRSnap_WritePng(path,top,w,h)>0,"menu preview written: %s",name);
    free(raw);free(top);
}

static void S_ComfortMenu()
{
    printf("--- comfort options: real settings and precise persistence ---\n");
    VRMenu_Init(nullptr,&MenuTestApply);
    const char* keys[]={"vr_height_adjust","vr_physical_crouch","vr_haptics_strength",
        "vr_twohand_stability","vr_twohand_recoil_reduction","vr_menu_scale","vr_sleeve_visible",
        "vr_weapon_cycle_binding","vr_native_hands_reload"};
    for(int i=0;i<(int)(sizeof(keys)/sizeof(keys[0]));++i)
        CHECK(MenuFindItem(keys[i])>=0,"options exposes %s",keys[i]);
    VRMenu_NoteConfig("vr_native_hands_r_pitch","-4.93",-4.93f);
    VRMenu_NoteConfig("vr_native_hands_r_yaw","22.56",22.56f);
    char saved[8192]; VRMenu_Serialize(saved,sizeof(saved));
    CHECK(strstr(saved,"vr_native_hands_r_pitch -4.93") && strstr(saved,"vr_native_hands_r_yaw 22.56"),
          "saving another option preserves the measured grip calibration to 0.01 degrees");
    ResetAll(); CHECK(BringUp(),"smaller menu stack"); StepPair();
    VR_SetMenuScale(0.65f); StepGameFlat();
    float cx,cy,vx,vy; CHECK(VR_GetMenuSafeArea(&cx,&cy,&vx,&vy),"smaller menu fit recorded");
    CHECK(vx-cx>12 && vy-cy>12,"shipping menu has >12 degrees margin (%.1f / %.1f)",vx-cx,vy-cy);
    VRMenu_Init(nullptr,&MenuTestApply); VRMenu_TestSetOpen(1);
    CHECK(VRMenu_Draw(0) && VRMenu_Draw(1),"options render into both eye textures");
    CUSaveMenuPreview("vr-options");
    for(int i=0;i<VRMenu_ItemCount();++i) {
        if(VRMenu_Item(VRMenu_Selected())->kind==4) break;
        VRMenu_TestFeed(0,1,0,0,0,0,0);
    }
    VRMenu_TestFeed(0,0,0,0,1,0,0);
    StepGameFlat(); CHECK(VRMenu_Draw(0),"controller reference renders");
    CUSaveMenuPreview("vr-controls");
    VRMenu_TestFeed(0,0,0,0,0,1,0);
    CHECK(VRMenu_Open(),"B returns from reference without closing the options");
    VRMenu_TestSetOpen(0);
}

static void S_ComfortProductionSoak()
{
    printf("--- comfort production soak: 2000 stereo pairs with shipping settings ---\n");
    const CodvrGunModel* guns[]={VRGunPose_ByVertexCount(326),VRGunPose_ByVertexCount(516)};
    if(!guns[0]||!guns[1]||!TmpBringUp(guns[0],0.064f)) { CHECK(0,"comfort soak setup");return; }
    VRNH_SetCalibrationTable(1);
    const float zero[3]={};
    VRNH_SetCalibration(VRI_RIGHT,zero,kTrimR); VRNH_SetCalibration(VRI_LEFT,zero,kTrimL);
    VRNH_SetTwoHandEnabled(1); VRNH_SetHandling(0.75f,0.80f);
    GLSurf_SetSleeveVisible(0);
    unsigned braced=0,shotFrames=0,badAim=0,badStereo=0,badVisibility=0;
    const unsigned mutations=CODVR_CollisionHookCameraMutationCount();
    for(int p=0;p<2000;++p) {
        const CodvrGunModel* gun=guns[(p/500)%2];
        const VRObjectSet* set=VRObj_Set();
        VRNHDebug old; VRNH_GetDebug(&old);
        const CodvrGunModel* prev=VRGunPose_ByVertexCount(old.gun.vertexCount);
        if(set->gun.valid && prev && prev->supportUsable) {
            float v[3],e[3],local[3],room[3];
            for(int k=0;k<3;++k) v[k]=(prev->supportM[k]-prev->rearGripM[k])*39.3701f;
            VRM_LocalToWorldVec(set->gun.axis,v,e);
            for(int k=0;k<3;++k) e[k]=(e[k]+set->gun.pos[k])/39.3701f;
            VRM_GLEyeToQuakeVec(e,local); VRM_LocalToWorldVec(set->head.axis,local,room);
            for(int k=0;k<3;++k) room[k]+=set->head.pos[k];
            SetHandGripPos(VRI_LEFT,-room[1]+0.002f*sinf(p*0.9f),room[2],-room[0]);
        }
        g_mock.squeeze[VRI_LEFT]=(p%160)<120 ? 0.9f : 0;
        g_mock.trigger[VRI_RIGHT]=(p%50)<20 ? 0.9f : 0;
        SetMockHead(15.0f*sinf(p*0.007f),0,0,0,0.064f);
        TmpScene a,b; TmpDefaultScene(&a,gun);
        a.sleeves=1; a.swapped=(p&1); b=a; b.swapped=!a.swapped;
        TmpAdvance(&b,0.02f,0.4f);
        TmpPair(&a,&b);
        VRNHDebug d; VRNH_GetDebug(&d);
        if(d.gun.twoHandState==VRNH_2H_ATTACHED) ++braced;
        if(d.gun.twoHandState==VRNH_2H_ATTACHED && g_mock.trigger[VRI_RIGHT]>0.6f) ++shotFrames;
        if(!GLSurf_ClassHidden(GLSC_SLEEVE_R)||!GLSurf_ClassHidden(GLSC_SLEEVE_L)) ++badVisibility;
        VRNHPairVerdict verdict; VRNH_GetLastVerdict(&verdict);
        if(!verdict.ok) ++badStereo;
        if(d.gun.targetInstalled && d.gun.barrelValid) {
            float eye[3],local[3],room[3];
            // At a model switch the pair target is tagged with the previous
            // completed baseline's model; do not interpret it with the new
            // draw's bore. The following pair adopts the fresh model.
            const CodvrGunModel* poseModel=VRGunPose_ByVertexCount(d.gun.vertexCount);
            if(!poseModel) { ++badAim; continue; }
            VRM_LocalToWorldVec(VRObj_Set()->gun.axis,poseModel->boreFwd,eye);
            VRM_GLEyeToQuakeVec(eye,local);
            VRM_LocalToWorldVec(VRObj_Set()->head.axis,local,room);
            const float len=sqrtf(PADot(room,room));
            for(int k=0;k<3;++k) room[k]/=len;
            if(HpDist(room,d.gun.barrelRoom)>0.001f) ++badAim;
        }
    }
    CHECK(braced>1000 && shotFrames>250,"production support exercised on %u pairs, firing on %u",braced,shotFrames);
    CHECK(badStereo==0,"new handling: zero stereo rejections on uncorrupted pairs (%u)",badStereo);
    CHECK(badAim==0,"new handling: zero published gun-bore / shot-target mismatches (%u)",badAim);
    CHECK(badVisibility==0,"floating hands: both complete sleeve classes hidden on every pair");
    CHECK(CODVR_CollisionHookCameraMutationCount()==mutations,"new handling: zero camera mutations");
    CHECK(g_mock.violations==0,"comfort soak: zero XR protocol violations");
}
