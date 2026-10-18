// GPU pixels, independent eye routing, pass failure and hostile engine GL
// state: these checks exercise the actual shader on the hidden GL context.
static void S_Postprocess()
{
    printf("--- per-eye colour processing: pixels / eye isolation / GL state ---\n");
    ResetAll();
    VRPostprocessConfig fallback = {};
    VR_GetPostprocessConfig(&fallback);
    CHECK(fallback.bloom==0,"post: unconfigured source fallback leaves bloom disabled");
    VR_SetPostprocessEnabled(1);
    VR_SetPostprocessSharpen(0.35f);
    VR_SetPostprocessBloom(0.08f);
    VR_SetPostprocessContrast(0.035f);
    VR_SetPostprocessSaturation(1.015f);
    VR_SetPostprocessShadowLift(0.02f);
    CHECK(VR_CaptureEye(0,64,64,false) && VR_CaptureEye(1,64,64,false),
          "post: both independent eye captures created");
    typedef void (WINAPI *GetTexFn)(unsigned,int,unsigned,unsigned,void*);
    GetTexFn readTex=(GetTexFn)CODVR_GetGLProc("glGetTexImage");
    if (!readTex || !tg_texImg || !tg_bindTex) {
        CHECK(false,"post: GPU readback available");
        return;
    }
    unsigned char pixels[64*64*4], result[64*64*4], other[64*64*4];
    auto fill = [&](unsigned char value) {
        for (int i=0;i<64*64;++i) {
            pixels[i*4]=pixels[i*4+1]=pixels[i*4+2]=value; pixels[i*4+3]=177;
        }
    };
    auto upload = [&](int eye) {
        tg_activeTexture(T_GL_TEXTURE0);
        tg_bindTex(T_GL_TEXTURE_2D,VR_EyeTexture(eye));
        tg_texImg(T_GL_TEXTURE_2D,0,T_GL_RGBA8,64,64,0,T_GL_RGBA,T_GL_UNSIGNED_BYTE,pixels);
        tg_bindTex(T_GL_TEXTURE_2D,0);
    };
    auto read = [&](int eye,unsigned char* out) {
        tg_activeTexture(T_GL_TEXTURE0);
        tg_bindTex(T_GL_TEXTURE_2D,VR_EyeTexture(eye));
        readTex(T_GL_TEXTURE_2D,0,T_GL_RGBA,T_GL_UNSIGNED_BYTE,out);
        tg_bindTex(T_GL_TEXTURE_2D,0);
    };

    // Missing config must not add a glow beside bright stock-HUD pixels.
    // Other colour controls retain their ordinary default values here.
    VR_SetPostprocessBloom(fallback.bloom);
    fill(128);
    for(int y=31;y<=33;++y) for(int x=31;x<=33;++x)
        for(int k=0;k<3;++k) pixels[(y*64+x)*4+k]=255;
    upload(0);
    CHECK(VR_PostprocessEye(0),"post: source-fallback highlight fixture runs");
    read(0,result);
    CHECK(result[(32*64+29)*4]==128,"post: source fallback adds no bloom beside a bright HUD-like mark (%u)",result[(32*64+29)*4]);
    VR_SetPostprocessBloom(0.08f);

    fill(12); upload(0);
    CHECK(VR_PostprocessEye(0),"post: GLSL colour pass compiles and runs");
    read(0,result);
    int darkMaxError=0, alphaMaxError=0;
    for(int i=0;i<64*64;++i) {
        for(int k=0;k<3;++k) { int d=abs((int)result[i*4+k]-12); if(d>darkMaxError) darkMaxError=d; }
        int a=abs((int)result[i*4+3]-177); if(a>alphaMaxError) alphaMaxError=a;
    }
    CHECK(darkMaxError<=1,"post: dark-scene visibility preserved over the full image (max %d)",darkMaxError);
    CHECK(alphaMaxError==0,"post: captured alpha preserved");

    VR_SetPostprocessSharpen(0); VR_SetPostprocessBloom(0);
    VR_SetPostprocessContrast(0); VR_SetPostprocessSaturation(1);
    fill(0); upload(0); CHECK(VR_PostprocessEye(0),"post: black-reference pass runs"); read(0,result);
    CHECK(result[0]==0,"post: shadow lift preserves exact black");
    fill(38); upload(0); CHECK(VR_PostprocessEye(0),"post: shadow-detail pass runs"); read(0,result);
    CHECK(result[0]>38 && result[0]<=42,"post: restrained shadow lift reveals detail without a grey veil (%u)",result[0]);
    fill(245); upload(0); CHECK(VR_PostprocessEye(0),"post: highlight-reference pass runs"); read(0,result);
    CHECK(result[0]<245 && result[0]>=240,"post: highlight shoulder is gentle and retains near-white detail (%u)",result[0]);

    VR_SetPostprocessContrast(0); VR_SetPostprocessSaturation(1); VR_SetPostprocessShadowLift(0);
    VR_SetPostprocessBloom(0); VR_SetPostprocessSharpen(0.60f);
    fill(64);
    for(int y=0;y<64;++y) for(int x=16;x<64;++x) {
        unsigned char v=x==16 ? 100 : x==17 ? 110 : 160;
        for(int k=0;k<3;++k) pixels[(y*64+x)*4+k]=v;
    }
    upload(0);
    CHECK(VR_PostprocessEye(0),"post: clarity pass runs on a measured edge"); read(0,result);
    CHECK(result[(32*64+16)*4]>100 && result[(32*64+17)*4]<110,
          "post: real pixel contrast increases at a soft texture edge (%u/%u)",
          result[(32*64+16)*4],result[(32*64+17)*4]);
    int minValue=255,maxValue=0;
    for(int i=0;i<64*64;++i) { if(result[i*4]<minValue) minValue=result[i*4]; if(result[i*4]>maxValue) maxValue=result[i*4]; }
    CHECK(minValue>=64 && maxValue<=160,"post: local limiter prevents sharpening halos (%d..%d)",minValue,maxValue);

    VR_SetPostprocessSharpen(0); VR_SetPostprocessBloom(0.20f);
    fill(128);
    for(int y=31;y<=33;++y) for(int x=31;x<=33;++x)
        for(int k=0;k<3;++k) pixels[(y*64+x)*4+k]=255;
    upload(0);
    fill(30); upload(1);
    CHECK(VR_PostprocessEye(0),"post: selective highlight bloom runs");
    read(0,result); read(1,other);
    CHECK(result[(32*64+29)*4]>128,"post: bright source produces a restrained nearby glow (%u)",result[(32*64+29)*4]);
    CHECK(result[(4*64+4)*4]==128,"post: bloom leaves distant midtones unchanged");
    bool separate=true;
    for(int i=0;i<64*64;++i) if(other[i*4]!=30 || other[i*4+3]!=177) separate=false;
    CHECK(separate,"post: processing left eye never writes the right eye");
    CHECK(VR_PostprocessEye(1),"post: right eye uses its own source"); read(1,other);
    separate=true;
    for(int i=0;i<64*64;++i) if(other[i*4]!=30) separate=false;
    CHECK(separate,"post: bright left-eye content never leaks into right-eye bloom");

    fill(217); upload(0);
    VR_SetPostprocessEnabled(0);
    CHECK(VR_PostprocessEye(0),"post: disabled pass succeeds without processing"); read(0,result);
    CHECK(memcmp(result,pixels,sizeof(pixels))==0,"post: disabled output is byte-identical to capture");
    VR_SetPostprocessEnabled(1); VR_TestFailNextPostprocess();
    CHECK(!VR_PostprocessEye(0),"post: target failure returns to the original capture"); read(0,result);
    CHECK(memcmp(result,pixels,sizeof(pixels))==0,"post: failed pass cannot publish partial or black output");

    // Nonzero GLSL program and separate read/draw FBOs catch state leaks that
    // ordinary fixed-function setup cannot reveal.
    typedef unsigned (WINAPI *CreateFn)(unsigned);
    typedef void (WINAPI *SourceFn)(unsigned,int,const char* const*,const int*);
    typedef void (WINAPI *ObjFn)(unsigned);
    typedef unsigned (WINAPI *CreateProgramFn)();
    typedef void (WINAPI *AttachFn)(unsigned,unsigned);
    typedef void (WINAPI *ParamFn)(unsigned,unsigned,int*);
    typedef void (WINAPI *GenFboFn)(int,unsigned*);
    typedef void (WINAPI *BindFboFn)(unsigned,unsigned);
    typedef void (WINAPI *PolyFn)(unsigned,unsigned);
    typedef void (WINAPI *CoordFn)(unsigned,float,float,float,float);
    CreateFn createShader=(CreateFn)CODVR_GetGLProc("glCreateShader");
    SourceFn shaderSource=(SourceFn)CODVR_GetGLProc("glShaderSource");
    ObjFn compile=(ObjFn)CODVR_GetGLProc("glCompileShader"),link=(ObjFn)CODVR_GetGLProc("glLinkProgram");
    ObjFn use=(ObjFn)CODVR_GetGLProc("glUseProgram"),deleteShader=(ObjFn)CODVR_GetGLProc("glDeleteShader");
    ObjFn deleteProgram=(ObjFn)CODVR_GetGLProc("glDeleteProgram");
    CreateProgramFn createProgram=(CreateProgramFn)CODVR_GetGLProc("glCreateProgram");
    AttachFn attach=(AttachFn)CODVR_GetGLProc("glAttachShader");
    ParamFn programParam=(ParamFn)CODVR_GetGLProc("glGetProgramiv");
    GenFboFn genFbo=(GenFboFn)CODVR_GetGLProc("glGenFramebuffers");
    BindFboFn bindFbo=(BindFboFn)CODVR_GetGLProc("glBindFramebuffer");
    PolyFn polygon=(PolyFn)CODVR_GetGLProc("glPolygonMode");
    CoordFn texCoord=(CoordFn)CODVR_GetGLProc("glMultiTexCoord4f");
    const char* vs="#version 120\nvoid main(){gl_Position=gl_Vertex;}\n";
    const char* fs="#version 120\nvoid main(){gl_FragColor=vec4(0.1,0.2,0.3,1.0);}\n";
    unsigned sv=createShader(0x8B31),sf=createShader(0x8B30);
    shaderSource(sv,1,&vs,nullptr); shaderSource(sf,1,&fs,nullptr); compile(sv); compile(sf);
    unsigned program=createProgram(); attach(program,sv); attach(program,sf); link(program);
    int linked=0; programParam(program,0x8B82,&linked);
    CHECK(linked!=0,"post: independent nonzero engine-program fixture linked");
    unsigned fb[2]={},textures[2]={}; genFbo(2,fb); tg_genTex(2,textures);
    tg_activeTexture(T_GL_TEXTURE0); tg_bindTex(T_GL_TEXTURE_2D,textures[0]);
    texCoord(T_GL_TEXTURE0,0.12f,0.34f,0.56f,0.78f);
    tg_activeTexture(T_GL_TEXTURE0+1); tg_bindTex(T_GL_TEXTURE_2D,textures[1]);
    bindFbo(0x8CA9,fb[0]); bindFbo(0x8CA8,fb[1]); use(program);
    tg_viewport(8,9,10,11); tg_colorMask(0,1,0,1); tg_scissor(1,2,3,4);
    polygon(0x0404,0x1B01); polygon(0x0405,0x1B00);
    const unsigned hostile[]={T_GL_DEPTH_TEST,T_GL_BLEND,T_GL_SCISSOR_TEST,T_GL_CULL_FACE,
                             T_GL_ALPHA_TEST,T_GL_FOG,T_GL_LIGHTING,0x0B90,0x0BF2,0x0BD0,0x0B41,0x0B42,0x3000};
    for(int i=0;i<(int)(sizeof(hostile)/sizeof(hostile[0]));++i) tg_enable(hostile[i]);
    while(tg_getError && tg_getError()!=0) {}
    CHECK(VR_PostprocessEye(0),"post: pass survives hostile fixed-function and GLSL state");
    CHECK(!tg_getError || tg_getError()==0,"post: hostile-state draw raises no GL error");
    int value=0,rect[4]={},mask[4]={},mode[2]={};
    tg_getIntegerv(0x8B8D,&value); CHECK(value==(int)program,"post: original GLSL program restored");
    tg_getIntegerv(T_GL_ACTIVE_TEXTURE,&value); CHECK(value==(int)T_GL_TEXTURE0+1,"post: active texture unit restored");
    tg_getIntegerv(T_GL_TEXTURE_BINDING_2D,&value); CHECK(value==(int)textures[1],"post: other texture unit binding untouched");
    tg_activeTexture(T_GL_TEXTURE0); tg_getIntegerv(T_GL_TEXTURE_BINDING_2D,&value);
    CHECK(value==(int)textures[0],"post: source-unit engine texture binding restored");
    float coord[4]={}; tg_getFloatv(0x0B03,coord);
    CHECK(fabsf(coord[0]-.12f)<1e-6f && fabsf(coord[3]-.78f)<1e-6f,"post: immediate texture coordinates restored");
    tg_getIntegerv(T_GL_DRAW_FB_BINDING,&value); CHECK(value==(int)fb[0],"post: separate draw FBO restored");
    tg_getIntegerv(T_GL_READ_FB_BINDING,&value); CHECK(value==(int)fb[1],"post: separate read FBO restored");
    tg_getIntegerv(T_GL_VIEWPORT,rect); CHECK(rect[0]==8 && rect[1]==9 && rect[2]==10 && rect[3]==11,"post: viewport restored");
    tg_getIntegerv(T_GL_COLOR_WRITEMASK,mask); CHECK(mask[0]==0 && mask[1]==1 && mask[2]==0 && mask[3]==1,"post: engine colour mask restored");
    tg_getIntegerv(0x0B40,mode); CHECK(mode[0]==0x1B01 && mode[1]==0x1B00,"post: front/back polygon modes restored");
    bool caps=true;
    for(int i=0;i<(int)(sizeof(hostile)/sizeof(hostile[0]));++i) if(!tg_isEnabled(hostile[i])) caps=false;
    CHECK(caps,"post: depth/blend/scissor/stencil/fog/alpha/light/logic/clip states restored");
    tg_getIntegerv(T_GL_SCISSOR_BOX,rect); CHECK(rect[0]==1 && rect[1]==2 && rect[2]==3 && rect[3]==4,"post: scissor rectangle untouched");

    for(int i=0;i<(int)(sizeof(hostile)/sizeof(hostile[0]));++i) tg_disable(hostile[i]);
    use(0); bindFbo(0x8CA9,0); bindFbo(0x8CA8,0); tg_colorMask(1,1,1,1);
    polygon(0x0408,0x1B02); tg_viewport(0,0,640,480);
    tg_activeTexture(T_GL_TEXTURE0+1); tg_bindTex(T_GL_TEXTURE_2D,0);
    tg_activeTexture(T_GL_TEXTURE0); tg_bindTex(T_GL_TEXTURE_2D,0);
    deleteShader(sv); deleteShader(sf); deleteProgram(program);
    VR_SetPostprocessSharpen(0.35f); VR_SetPostprocessBloom(fallback.bloom);
    VR_SetPostprocessContrast(0.035f); VR_SetPostprocessSaturation(1.015f);
    VR_SetPostprocessShadowLift(0.02f);
}
