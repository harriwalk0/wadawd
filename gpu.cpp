


#define _CRT_SECURE_NO_WARNINGS
#include "gpu.h"
#include <wincodec.h>
#include "item_icon_map.h"
#pragma comment(lib,"windowscodecs.lib")
#include <stdio.h>
#include <string.h>
#include <math.h>
#include <vector>
#include <string>

#include <d3d11.h>
#include <dxgi1_3.h>
#include <d2d1_1.h>
#include <d2d1helper.h>
#include <dwrite_3.h>
#include <dcomp.h>
#include <d3dcompiler.h>

#pragma comment(lib, "d3d11.lib")
#pragma comment(lib, "dxgi.lib")
#pragma comment(lib, "d2d1.lib")
#pragma comment(lib, "dwrite.lib")
#pragma comment(lib, "dcomp.lib")
#pragma comment(lib, "d3dcompiler.lib")


static HWND                     g_hwnd;
static int                      g_w, g_h, g_ready;

static ID3D11Device*            g_dev;
static ID3D11DeviceContext*     g_ctx;
static IDXGISwapChain1*         g_swap;
static HANDLE                  g_frame_event;
static UINT                    g_swap_flags;
static volatile LONG           g_frame_granted;
static IDCompositionDevice*     g_dcomp;
static IDCompositionTarget*     g_dtarget;
static IDCompositionVisual*     g_dvisual;

static ID2D1Factory1*           g_d2df;
static ID2D1Device*             g_d2ddev;
static ID2D1DeviceContext*      g_d2dc;
static ID2D1Bitmap1*            g_target;
static ID3D11Texture2D*         g_back;
static ID3D11RenderTargetView*  g_rtv;

static IDWriteFactory5*         g_dwf;
static IDWriteFontCollection1*  g_fontcoll;
static IDWriteTextFormat*       g_fmt;
static float                    g_fmt_px = 13.0f;
struct FONT_ENTRY { IDWriteTextFormat* format; float px; int face; };
static FONT_ENTRY g_fonts[16];
static unsigned g_font_cursor;

static ID2D1SolidColorBrush*    g_brush;
static RGBA                     g_brush_col;
static int                      g_brush_valid;

static RGBA                     g_pen_col = 0xFFFFFFFFu;
static float                    g_pen_w   = 1.0f;
static int                      g_batch;


static ID3D11Texture2D*         g_chtex;
static ID3D11ShaderResourceView* g_chsrv;
static ID3D11SamplerState*      g_chsmp;
static ID3D11VertexShader*      g_vs;
static ID3D11PixelShader*       g_ps;
static ID3D11Buffer*            g_cb;
static ID3D11BlendState*        g_blend;
static ID3D11RasterizerState*   g_rast;
static ID3D11DepthStencilState* g_nodepth;
static int                      g_chw, g_chh;
static int                      g_ch_ok;
struct CHAM_VERTEX { float x,y,r,g,b,a,mode,u,v; };
static std::vector<CHAM_VERTEX> g_mesh_vertices;
static ID3D11VertexShader* g_mesh_vs;
static ID3D11PixelShader* g_mesh_ps;
static ID3D11InputLayout* g_mesh_layout;
static ID3D11Buffer* g_mesh_buffer;
static ID3D11Buffer* g_mesh_cb;
static unsigned g_mesh_buffer_bytes;
static ID3D11VertexShader* g_sky_vs;
static ID3D11PixelShader* g_sky_ps;
static ID3D11Buffer* g_sky_cb;

typedef struct { float texel[2]; float radius; float opacity; float col[4]; } CH_PARAMS;


static const char* VS_SRC =
"struct VSOut { float4 pos : SV_POSITION; float2 uv : TEXCOORD0; };\n"
"VSOut main(uint id : SV_VertexID) {\n"
"    VSOut o;\n"
"    float2 p = float2((id == 2) ? 3.0 : -1.0, (id == 1) ? 3.0 : -1.0);\n"
"    o.pos = float4(p, 0.0, 1.0);\n"
"    o.uv  = float2((p.x + 1.0) * 0.5, 1.0 - (p.y + 1.0) * 0.5);\n"
"    return o;\n"
"}\n";


static const char* PS_SRC =
"Texture2D tex : register(t0);\n"
"SamplerState smp : register(s0);\n"
"cbuffer P : register(b0) { float2 texel; float radius; float opacity; float4 col; };\n"
"float4 main(float4 pos : SV_POSITION, float2 uv : TEXCOORD0) : SV_TARGET {\n"
"    float4 s = tex.Sample(smp, uv);\n"
"    if (s.a > 0.004) return float4(s.rgb, s.a * opacity);\n"
"    if (radius <= 0.0) return float4(0,0,0,0);\n"
"    float m = 0.0;\n"
"    float2 d = float2(texel.x * radius, texel.y * radius);\n"
"    [unroll] for (int i = 0; i < 12; i++) {\n"
"        float a = (float)i * 0.5235988;\n"
"        float2 o = float2(cos(a), sin(a)) * d;\n"
"        m = max(m, tex.Sample(smp, uv + o).a);\n"
"        m = max(m, tex.Sample(smp, uv + o * 0.5).a);\n"
"    }\n"
"    if (m > 0.004) return float4(col.rgb * col.a, col.a);\n"
"    return float4(0,0,0,0);\n"
"}\n";


// Release target.
static void release_target(void) {
    if(g_d2dc)g_d2dc->SetTarget(NULL);
    if (g_target) { g_target->Release(); g_target = NULL; }
    if(g_rtv){g_rtv->Release();g_rtv=NULL;}
    if(g_back){g_back->Release();g_back=NULL;}
}

// Handle ensure batch.
static void ensure_batch(void) {
    if (g_batch || !g_d2dc) return;
    g_d2dc->BeginDraw();
    g_d2dc->SetAntialiasMode(D2D1_ANTIALIAS_MODE_PER_PRIMITIVE);
    g_d2dc->SetTextAntialiasMode(D2D1_TEXT_ANTIALIAS_MODE_GRAYSCALE);
    g_d2dc->SetTransform(D2D1::Matrix3x2F::Identity());
    g_batch = 1;
}

static float g_opacity=1.0f;
static ID2D1Bitmap* g_icon_bitmaps[INVOKE_ICON_COUNT];
struct ICON_PIXELS { std::vector<BYTE> bytes; UINT width=0,height=0; volatile LONG state=0; };
static ICON_PIXELS g_icons[INVOKE_ICON_COUNT];
static HANDLE g_icon_thread,g_icon_wake;
static volatile LONG g_icon_stop;
static unsigned g_icons_uploaded;
// Decode original icons off the render thread.
static int decode_icon(IWICImagingFactory* factory,int index){
    HMODULE module=GetModuleHandleW(NULL);
    HRSRC resource=FindResourceW(module,MAKEINTRESOURCEW(INVOKE_ICON_FIRST+index),MAKEINTRESOURCEW(10));
    if(!resource)return 0;
    BYTE* data=(BYTE*)LockResource(LoadResource(module,resource));DWORD bytes=SizeofResource(module,resource);
    if(!data || !bytes)return 0;
    IWICStream* stream=NULL;IWICBitmapDecoder* decoder=NULL;IWICBitmapFrameDecode* frame=NULL;
    IWICBitmapScaler* scaler=NULL;IWICFormatConverter* format=NULL;UINT width=0,height=0;
    HRESULT hr=factory->CreateStream(&stream);
    if(SUCCEEDED(hr))hr=stream->InitializeFromMemory(data,bytes);
    if(SUCCEEDED(hr))hr=factory->CreateDecoderFromStream(stream,NULL,WICDecodeMetadataCacheOnLoad,&decoder);
    if(SUCCEEDED(hr))hr=decoder->GetFrame(0,&frame);
    if(SUCCEEDED(hr))hr=frame->GetSize(&width,&height);
    if(SUCCEEDED(hr) && (!width || !height || width>8192 || height>8192))hr=E_INVALIDARG;
    IWICBitmapSource* source=frame;
    if(SUCCEEDED(hr) && (width>128 || height>128)){
        float scale=128.0f/(float)(width>height?width:height);
        width=(UINT)fmaxf(1,floorf(width*scale));height=(UINT)fmaxf(1,floorf(height*scale));
        hr=factory->CreateBitmapScaler(&scaler);
        if(SUCCEEDED(hr))hr=scaler->Initialize(frame,width,height,WICBitmapInterpolationModeFant);
        source=scaler;
    }
    if(SUCCEEDED(hr))hr=factory->CreateFormatConverter(&format);
    if(SUCCEEDED(hr))hr=format->Initialize(source,GUID_WICPixelFormat32bppPBGRA,WICBitmapDitherTypeNone,NULL,0,WICBitmapPaletteTypeCustom);
    ICON_PIXELS& icon=g_icons[index];
    if(SUCCEEDED(hr)){
        icon.bytes.resize((size_t)width*height*4);
        hr=format->CopyPixels(NULL,width*4,(UINT)icon.bytes.size(),icon.bytes.data());
        if(SUCCEEDED(hr)){icon.width=width;icon.height=height;}
    }
    if(format)format->Release();if(scaler)scaler->Release();if(frame)frame->Release();
    if(decoder)decoder->Release();if(stream)stream->Release();
    if(FAILED(hr))icon.bytes.clear();return SUCCEEDED(hr);
}
// Process queued icon decoding.
static DWORD WINAPI icon_worker(LPVOID unused){
    (void)unused;HRESULT initialized=CoInitializeEx(NULL,COINIT_MULTITHREADED);
    IWICImagingFactory* factory=NULL;
    HRESULT hr=SUCCEEDED(initialized)?CoCreateInstance(CLSID_WICImagingFactory,NULL,CLSCTX_INPROC_SERVER,IID_PPV_ARGS(&factory)):initialized;
    while(!InterlockedCompareExchange(&g_icon_stop,0,0)){
        WaitForSingleObject(g_icon_wake,250);
        for(int i=0;i<INVOKE_ICON_COUNT && !InterlockedCompareExchange(&g_icon_stop,0,0);++i){
            if(InterlockedCompareExchange(&g_icons[i].state,4,1)!=1)continue;
            int ok=0;
            try{ok=SUCCEEDED(hr)&&factory&&decode_icon(factory,i);}catch(...){ok=0;}
            InterlockedExchange(&g_icons[i].state,ok?2:-1);
        }
    }
    if(factory)factory->Release();if(SUCCEEDED(initialized))CoUninitialize();return 0;
}
// Draw a decoded icon without blocking.
int r_icon(int id,float x,float y,float size){
    if(!g_d2dc || id<INVOKE_ICON_FIRST || id>INVOKE_ICON_LAST || size<=0)return 0;
    int index=id-INVOKE_ICON_FIRST;ICON_PIXELS& icon=g_icons[index];
    ID2D1Bitmap*& bitmap=g_icon_bitmaps[index];
    if(!bitmap){
        LONG state=InterlockedCompareExchange(&icon.state,0,0);
        if(state==0 && g_icon_thread && InterlockedCompareExchange(&icon.state,1,0)==0)SetEvent(g_icon_wake);
        if(state!=2 || g_icons_uploaded>=2)return 0;
        D2D1_BITMAP_PROPERTIES properties=D2D1::BitmapProperties(D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM,D2D1_ALPHA_MODE_PREMULTIPLIED),96,96);
        HRESULT hr=g_d2dc->CreateBitmap(D2D1::SizeU(icon.width,icon.height),icon.bytes.data(),icon.width*4,properties,&bitmap);
        ++g_icons_uploaded;
        std::vector<BYTE>().swap(icon.bytes);InterlockedExchange(&icon.state,SUCCEEDED(hr)?3:-1);
        if(FAILED(hr))return 0;
    }
    ensure_batch();D2D1_SIZE_F dimensions=bitmap->GetSize();
    float scale=fminf(size/dimensions.width,size/dimensions.height);
    float width=dimensions.width*scale,height=dimensions.height*scale;
    float left=x+(size-width)*.5f,top=y+(size-height)*.5f;
    g_d2dc->DrawBitmap(bitmap,D2D1::RectF(left,top,left+width,top+height),g_opacity,D2D1_BITMAP_INTERPOLATION_MODE_LINEAR);
    return 1;
}
static int g_font_face=0,g_text_background=0,g_text_outline=1;
// Handle opacity.
void gpu_opacity(float opacity){g_opacity=fmaxf(0,fminf(1,opacity));}
// Handle text style.
void gpu_text_style(int face,int background,int outline){
    if(face!=g_font_face){g_font_face=face;g_fmt=NULL;}
    g_text_background=background;g_text_outline=outline;
}
// Handle opacity color.
static RGBA opacity_color(RGBA c){return (c&0xffffffu)|((RGBA)(RA(c)*g_opacity)<<24);}
// Handle brush for.
static ID2D1SolidColorBrush* brush_for(RGBA c) {
    c=opacity_color(c);
    if (g_brush_valid && g_brush_col == c && g_brush) return g_brush;
    float r = ((c >> 16) & 0xFF) / 255.0f;
    float g = ((c >> 8) & 0xFF) / 255.0f;
    float b = (c & 0xFF) / 255.0f;
    float a = ((c >> 24) & 0xFF) / 255.0f;
    if(g_brush){g_brush->SetColor(D2D1::ColorF(r,g,b,a));}
    else if (FAILED(g_d2dc->CreateSolidColorBrush(D2D1::ColorF(r, g, b, a), &g_brush))) {
        g_brush = NULL; g_brush_valid = 0; return NULL;
    }
    g_brush_col = c; g_brush_valid = 1;
    return g_brush;
}


// Handle font from file.
static void font_from_file(void) {
    char exe[MAX_PATH] = {0}, path[MAX_PATH * 2];
    GetModuleFileNameA(NULL, exe, sizeof(exe) - 1);
    char* slash = strrchr(exe, '\\');
    if (slash) *(slash + 1) = 0;
    snprintf(path, sizeof(path), "%sTTNormsTrlCnd-Medium.otf", exe);

    wchar_t wpath[MAX_PATH * 2];
    MultiByteToWideChar(CP_UTF8, 0, path, -1, wpath, MAX_PATH * 2);
    if (GetFileAttributesW(wpath) == INVALID_FILE_ATTRIBUTES) return;

    IDWriteFontFile* file = NULL;
    if (FAILED(g_dwf->CreateFontFileReference(wpath, NULL, &file))) return;

    IDWriteFontSetBuilder1* b = NULL;
    IDWriteFontSet* set = NULL;
    if (SUCCEEDED(g_dwf->CreateFontSetBuilder(&b)) && b) {
        if (SUCCEEDED(b->AddFontFile(file)) && SUCCEEDED(b->CreateFontSet(&set)) && set)
            g_dwf->CreateFontCollectionFromFontSet(set, &g_fontcoll);
    }
    if (set) set->Release();
    if (b) b->Release();
    file->Release();
}

// Handle make format.
static int make_format(float px) {
    for(const auto& entry:g_fonts)if(entry.format && entry.face==g_font_face && entry.px==px){
        g_fmt=entry.format;g_fmt_px=px;return 1;
    }
    g_fmt=NULL;
    const wchar_t* fam = L"TT Norms Std Trl Cnd";
    HRESULT hr = E_FAIL;
    if (g_fontcoll && g_font_face==0) {
        hr = g_dwf->CreateTextFormat(fam, g_fontcoll, DWRITE_FONT_WEIGHT_MEDIUM,
                                     DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL,
                                     px, L"en-us", &g_fmt);
    }
    if (FAILED(hr) || !g_fmt) {
        const wchar_t* system_face=g_font_face==1?L"Consolas":g_font_face==2?L"Arial":L"Segoe UI";
        hr = g_dwf->CreateTextFormat(system_face, NULL, DWRITE_FONT_WEIGHT_NORMAL,
                                     DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL,
                                     px, L"en-us", &g_fmt);
    }
    if (FAILED(hr) || !g_fmt) return 0;
    g_fmt->SetWordWrapping(DWRITE_WORD_WRAPPING_NO_WRAP);
    g_fmt->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_LEADING);
    g_fmt->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_NEAR);
    g_fmt_px = px;
    FONT_ENTRY& entry=g_fonts[g_font_cursor++%ARRAYSIZE(g_fonts)];
    if(entry.format)entry.format->Release();
    entry={g_fmt,px,g_font_face};
    return 1;
}

// Handle font.
void gpu_font(float px) {
    if (px < 6.0f) px = 6.0f;
    if (px > 96.0f) px = 96.0f;
    if (g_fmt && fabsf(px - g_fmt_px) < 0.5f) return;
    make_format(px);
}

// Handle r round rect.
void r_round_rect(float x,float y,float w,float h,float radius,RGBA c,float stroke){
    if(!g_d2dc)return;ensure_batch();
    ID2D1SolidColorBrush* brush=brush_for(c);
    if(brush)g_d2dc->DrawRoundedRectangle(D2D1::RoundedRect(D2D1::RectF(x,y,x+w,y+h),radius,radius),brush,stroke);
}

struct TEXT_ENTRY {
    std::wstring text;
    IDWriteTextLayout* layout=NULL;
    uint64_t hash=0,touched=0;
    float width=0,px=0;
    int face=0;
};
static TEXT_ENTRY g_text_cache[1024];
static uint64_t g_text_clock;
// Reuse measured text layouts.
static TEXT_ENTRY* cached_text(const wchar_t* text,int count){
    if(!g_dwf || !g_fmt || !text || count<=0 || count>4096)return NULL;
    uint64_t hash=1469598103934665603ull;
    for(int i=0;i<count;++i){hash^=(unsigned short)text[i];hash*=1099511628211ull;}
    TEXT_ENTRY* bucket=&g_text_cache[(hash%256)*4],*victim=&bucket[0];
    for(int i=0;i<4;++i){TEXT_ENTRY* entry=&bucket[i];
        if(entry->layout && entry->hash==hash && entry->px==g_fmt_px && entry->face==g_font_face &&
            entry->text.size()==(size_t)count && !memcmp(entry->text.data(),text,(size_t)count*sizeof(wchar_t))){
            entry->touched=++g_text_clock;return entry;
        }
        if(!entry->layout || (victim->layout && entry->touched<victim->touched))victim=entry;
    }
    IDWriteTextLayout* layout=NULL;
    if(FAILED(g_dwf->CreateTextLayout(text,(UINT32)count,g_fmt,4096,4096,&layout)) || !layout)return NULL;
    DWRITE_TEXT_METRICS metrics={};
    if(FAILED(layout->GetMetrics(&metrics))){layout->Release();return NULL;}
    if(victim->layout)victim->layout->Release();
    victim->text.assign(text,count);victim->layout=layout;victim->hash=hash;
    victim->px=g_fmt_px;victim->face=g_font_face;victim->width=metrics.width;victim->touched=++g_text_clock;
    return victim;
}
// Draw cached text.
static void text_run(float x, float y, const wchar_t* s, int n, RGBA c, int flags) {
    if (!g_d2dc || !g_fmt || n <= 0) return;
    ensure_batch();

    TEXT_ENTRY* cached=cached_text(s,n);if(!cached)return;
    float w=cached->width;
    IDWriteTextLayout* lay=cached->layout;
    float dx = 0.0f;
    if (flags & RT_CENTER) dx = -w * 0.5f;
    else if (flags & RT_RIGHT) dx = -w;

    D2D1_RECT_F rc = D2D1::RectF(x + dx, y, x + dx + 4096.0f, y + g_fmt_px * 3.0f);
    if(!lay)return;
    if(g_text_background>0){
        ID2D1SolidColorBrush* bg=brush_for(RARGB(g_text_background*255/100,10,12,20));
        if(bg)g_d2dc->FillRoundedRectangle(D2D1::RoundedRect(D2D1::RectF(x+dx-3,y-1,x+dx+w+3,y+g_fmt_px*1.35f),3,3),bg);
    }
    if ((flags & RT_OUTLINE) && g_text_outline) {
        RGBA halo = 0xC0000000u;
        ID2D1SolidColorBrush* hb = brush_for(halo);
        if (hb) {
            static const float ox[4] = { -1.0f, 1.0f, 0.0f, 0.0f };
            static const float oy[4] = { 0.0f, 0.0f, -1.0f, 1.0f };
            for (int i = 0; i < 4; i++) {
                D2D1_RECT_F r2 = D2D1::RectF(rc.left + ox[i], rc.top + oy[i],
                                             rc.right + ox[i], rc.bottom + oy[i]);
                g_d2dc->DrawTextLayout(D2D1::Point2F(r2.left, r2.top), lay, hb,
                                       D2D1_DRAW_TEXT_OPTIONS_NONE);
            }
            g_brush_valid = 0;
        }
    }
    ID2D1SolidColorBrush* fb = brush_for(c);
    if (fb) g_d2dc->DrawTextLayout(D2D1_POINT_2F{rc.left, rc.top}, lay, fb,
                                   D2D1_DRAW_TEXT_OPTIONS_NONE);
}

// Handle r text w.
void r_text_w(float x, float y, const wchar_t* s, int len, RGBA c, int flags) {
    text_run(x, y, s, len, c, flags);
}

// Handle r text.
void r_text(float x, float y, const char* utf8, RGBA c, int flags) {
    if (!utf8 || !*utf8) return;
    wchar_t buf[512];
    int n = MultiByteToWideChar(CP_UTF8, 0, utf8, -1, buf, 512);
    if (n <= 0) return;
    text_run(x, y, buf, n - 1, c, flags);
}

// Handle r text height.
float r_text_height(void) { return g_fmt_px * 1.30f; }

// Handle r text width.
float r_text_width(const char* utf8) {
    if (!utf8 || !*utf8 || !g_dwf || !g_fmt) return 0.0f;
    wchar_t buf[512];
    int n = MultiByteToWideChar(CP_UTF8, 0, utf8, -1, buf, 512);
    if (n <= 0) return 0.0f;
    TEXT_ENTRY* cached=cached_text(buf,n-1);
    return cached?cached->width:0;
}


// Handle pen.
void gpu_pen(RGBA c, float width) { g_pen_col = c; g_pen_w = width < 1.0f ? 1.0f : width; }

// Handle r line.
void r_line(float x1, float y1, float x2, float y2) {
    if (!g_d2dc) return;
    ensure_batch();
    ID2D1SolidColorBrush* b = brush_for(g_pen_col);
    if (b) g_d2dc->DrawLine(D2D1::Point2F(x1, y1), D2D1::Point2F(x2, y2), b, g_pen_w);
}

// Handle r line c.
void r_line_c(float x1, float y1, float x2, float y2, RGBA c, float w) {
    if (!g_d2dc) return;
    ensure_batch();
    ID2D1SolidColorBrush* b = brush_for(c);
    if (b) g_d2dc->DrawLine(D2D1::Point2F(x1, y1), D2D1::Point2F(x2, y2), b, w < 1.0f ? 1.0f : w);
}

// Handle r rect.
void r_rect(float x, float y, float w, float h, RGBA c, float stroke) {
    if (!g_d2dc || w <= 0.0f || h <= 0.0f) return;
    ensure_batch();
    ID2D1SolidColorBrush* b = brush_for(c);
    if (b) g_d2dc->DrawRectangle(D2D1::RectF(x, y, x + w, y + h), b,
                                 stroke < 1.0f ? 1.0f : stroke);
}

// Handle r fill.
void r_fill(float x, float y, float w, float h, RGBA c) {
    if (!g_d2dc || w <= 0.0f || h <= 0.0f) return;
    ensure_batch();
    ID2D1SolidColorBrush* b = brush_for(c);
    if (b) g_d2dc->FillRectangle(D2D1::RectF(x, y, x + w, y + h), b);
}

// Handle r ellipse.
void r_ellipse(float cx, float cy, float rx, float ry, RGBA c, float stroke) {
    if (!g_d2dc || rx <= 0.0f) return;
    ensure_batch();
    ID2D1SolidColorBrush* b = brush_for(c);
    if (b) g_d2dc->DrawEllipse(D2D1::Ellipse(D2D1::Point2F(cx, cy), rx, ry), b,
                               stroke < 1.0f ? 1.0f : stroke);
}

// Handle r fill ellipse.
void r_fill_ellipse(float cx, float cy, float rx, float ry, RGBA c) {
    if (!g_d2dc || rx <= 0.0f) return;
    ensure_batch();
    ID2D1SolidColorBrush* b = brush_for(c);
    if (b) g_d2dc->FillEllipse(D2D1::Ellipse(D2D1::Point2F(cx, cy), rx, ry), b);
}

// Handle mesh resources.
static int mesh_resources(void) {
    if (g_mesh_vs && g_mesh_ps && g_mesh_layout && g_mesh_cb) return 1;
    if (!g_dev) return 0;
    static const char* vs=
        "struct I{float2 p:POSITION;float4 c:COLOR;float m:TEXCOORD0;float2 uv:TEXCOORD1;};"
        "struct O{float4 p:SV_POSITION;float4 c:COLOR;float m:TEXCOORD0;float2 uv:TEXCOORD1;};"
        "O main(I i){O o;o.p=float4(i.p,0,1);o.c=i.c;o.m=i.m;o.uv=i.uv;return o;}";
    static const char* ps=
        "cbuffer CB:register(b0){float time;float w;float h;float pad;};\n"
        "float hash21(float2 p){p=frac(p*float2(123.34,456.21));p+=dot(p,p+45.32);return frac(p.x*p.y);}\n"
        "float noise2(float2 p){float2 i=floor(p),f=frac(p);f=f*f*(3.0-2.0*f);return lerp(lerp(hash21(i),hash21(i+float2(1,0)),f.x),lerp(hash21(i+float2(0,1)),hash21(i+float2(1,1)),f.x),f.y);}\n"
        "float fbm2(float2 p){float v=0.0,a=0.5;for(int i=0;i<3;i++){v+=a*noise2(p);p=p*2.07+float2(13.1,29.3);a*=0.5;}return v;}\n"
        "float3 hsv2rgb(float3 c){float4 K=float4(1.0,2.0/3.0,1.0/3.0,3.0);float3 p=abs(frac(c.xxx+K.xyz)*6.0-K.www);return c.z*lerp(K.xxx,saturate(p-K.xxx),c.y);}\n"
        "float4 main(float4 p:SV_POSITION,float4 c:COLOR,float m:TEXCOORD0,float2 luv:TEXCOORD1):SV_TARGET{\n"
        "    int packed=(int)(m+0.5);\n"
        "    int mode=packed%16;\n"
        "    int mat=packed/16;\n"
        "    if(c.a<=0.001)return c;\n"
        "    float3 base=c.rgb/max(c.a,0.001);\n"
        "    float2 uv=saturate(luv);\n"
        "    float nx=clamp((uv.x-0.5)*2.0,-1.0,1.0);\n"
        "    float nz=sqrt(max(0.05,1.0-nx*nx));\n"
        "    float rim=pow(saturate(1.0-nz),1.8);\n"
        "    float3 col=base;\n"
        "    if(mode==1){\n" // Hologram
        "        float scan=0.4+0.6*sin(uv.y*75.0-time*7.0);\n"
        "        float sweep=exp(-pow(frac(uv.y-time*0.55)-0.5,2.0)*32.0);\n"
        "        float3 holo=lerp(base*0.35,saturate(base*1.2+float3(0.15,0.75,1.0)),scan*0.65+sweep*0.75);\n"
        "        col=holo+float3(0.35,0.92,1.0)*rim*0.75;\n"
        "    }else if(mode==2){\n" // Plasma
        "        float2 q=uv*float2(3.5,6.5);\n"
        "        float n1=fbm2(q+float2(time*0.55,-time*0.45));\n"
        "        float n2=fbm2(q*1.8+float2(n1*2.4-time*0.4,time*0.5));\n"
        "        float3 c1=base*0.35;\n"
        "        float3 c2=lerp(base,float3(0.10,0.92,1.0),0.72);\n"
        "        float3 c3=lerp(base,float3(1.0,0.22,0.88),0.78);\n"
        "        col=lerp(lerp(c1,c2,smoothstep(0.2,0.62,n1)),c3,smoothstep(0.45,0.85,n2));\n"
        "    }else if(mode==3){\n" // Galaxy
        "        float n1=fbm2(uv*float2(3.0,5.5)+float2(time*0.12,-time*0.08));\n"
        "        float n2=fbm2(uv*float2(6.0,10.0)-float2(time*0.18,time*0.10));\n"
        "        float3 deep=float3(0.04,0.02,0.14);\n"
        "        float3 neb=lerp(float3(0.48,0.12,0.92),float3(0.10,0.78,1.0),n2);\n"
        "        float3 space=lerp(deep,lerp(neb,base,0.25),smoothstep(0.22,0.82,n1));\n"
        "        float2 cell=floor(uv*float2(18.0,36.0));\n"
        "        float st=hash21(cell);\n"
        "        float tw=step(0.92,st)*(0.45+0.55*sin(time*5.5+st*62.83));\n"
        "        col=space+float3(1.0,0.98,1.0)*tw*1.4;\n"
        "    }else if(mode==4){\n" // Rainbow
        "        float wave=0.10*sin(uv.y*8.0+time*2.6);\n"
        "        float hue=frac(uv.x*0.55+uv.y*0.85+wave-time*0.28);\n"
        "        col=hsv2rgb(float3(hue,0.85,1.0));\n"
        "    }else if(mode==5){\n" // Pulse
        "        float beat=pow(0.5+0.5*sin(uv.y*10.0-time*5.2),3.0);\n"
        "        col=lerp(base*0.32,saturate(base*1.6+float3(0.35,0.45,0.55)),beat)+base*rim*0.6;\n"
        "    }else if(mode==6){\n" // Aurora
        "        float n=fbm2(uv*float2(3.5,6.0)+float2(time*0.25,-time*0.15));\n"
        "        float curtain=0.5+0.5*sin(uv.x*10.0+n*6.0-time*2.2);\n"
        "        col=lerp(lerp(float3(0.38,0.15,0.95),float3(0.05,0.78,1.0),curtain),float3(0.08,1.0,0.65),smoothstep(0.32,0.82,n));\n"
        "    }else if(mode==7){\n" // Iridescent Pearl
        "        float shift=fbm2(uv*4.5+float2(time*0.25,time*0.18))*1.8+uv.x*0.7+uv.y*0.5;\n"
        "        float3 pearl=hsv2rgb(float3(frac(shift*0.5+time*0.15),0.58,1.0));\n"
        "        col=lerp(pearl,float3(1.0,0.98,1.0),rim*0.55);\n"
        "    }else if(mode==8){\n" // Liquid Gold
        "        float n=fbm2(uv*float2(4.0,7.0)+float2(time*0.3,-time*0.22));\n"
        "        float spec=pow(saturate(nz),5.0);\n"
        "        col=lerp(float3(0.38,0.20,0.03),float3(1.0,0.82,0.24),smoothstep(0.2,0.8,n))+float3(1.0,0.96,0.72)*spec*0.7;\n"
        "    }else if(mode==9){\n" // Neon Grid
        "        float2 g=abs(frac(uv*float2(8.0,18.0)+float2(0.0,-time*0.6))-0.5);\n"
        "        float wire=1.0-smoothstep(0.04,0.14,min(g.x,g.y));\n"
        "        float3 neon=lerp(float3(0.0,0.95,1.0),float3(1.0,0.16,0.84),uv.y);\n"
        "        col=lerp(base*0.16,neon,wire)+neon*rim*0.45;\n"
        "    }else if(mode==10){\n" // Void
        "        float n=fbm2(uv*float2(4.5,8.0)-float2(time*0.4,time*0.3));\n"
        "        float tendril=pow(1.0-abs(n-0.5)*2.0,3.2);\n"
        "        float3 edge=lerp(float3(0.68,0.12,1.0),float3(1.0,0.10,0.42),0.5+0.5*sin(time*2.0+uv.y*6.0));\n"
        "        col=lerp(float3(0.03,0.01,0.07),edge,saturate(tendril+rim*0.65));\n"
        "    }else if(mode>=11){\n" // Inferno
        "        float n=fbm2(uv*float2(4.0,7.5)+float2(0.0,time*0.95));\n"
        "        float flame=saturate(n*1.15+(1.0-uv.y)*0.35);\n"
        "        col=lerp(float3(0.42,0.04,0.02),lerp(float3(1.0,0.32,0.04),float3(1.0,0.94,0.38),pow(flame,2.0)),smoothstep(0.2,0.9,flame));\n"
        "    }\n"
        "    float outA=c.a;\n"
        "    if(mat==1){\n" // Glossy
        "        float3 L=normalize(float3(-0.45,0.55,0.70));\n"
        "        float3 N=normalize(float3(nx,(0.5-uv.y)*0.5,nz));\n"
        "        float spec=pow(saturate(dot(N,L)),14.0);\n"
        "        col=saturate(col*(0.55+0.65*nz)+float3(1.0,1.0,1.0)*spec*0.95+col*rim*0.35);\n"
        "    }else if(mat==2){\n" // Metallic / Chrome
        "        float refl=smoothstep(-0.15,0.15,sin(uv.y*9.0+nx*2.5+time*0.8));\n"
        "        float3 env=lerp(float3(0.10,0.12,0.18),float3(0.95,0.97,1.0),refl);\n"
        "        col=saturate(lerp(col*0.45,col*env*1.55,0.68)+float3(1.0,1.0,1.0)*pow(nz,6.0)*0.45);\n"
        "    }else if(mat==3){\n" // Matte
        "        float lum=dot(col,float3(0.299,0.587,0.114));\n"
        "        col=lerp(float3(lum,lum,lum),col,0.68)*(0.62+0.32*nz);\n"
        "    }else if(mat==4){\n" // Glass
        "        float glint=pow(saturate(1.0-abs(uv.x+uv.y*0.6-0.8-0.15*sin(time))*4.5),3.0);\n"
        "        col=saturate(col*0.55+float3(0.75,0.94,1.0)*(rim*0.95+glint*0.85));\n"
        "        outA=c.a*saturate(0.32+rim*0.68+glint*0.4);\n"
        "    }else if(mat>=5){\n" // Neon Glow
        "        col=saturate(col*1.55+col*rim*1.1+float3(0.15,0.15,0.15)*rim);\n"
        "    }\n"
        "    return float4(saturate(col)*outA,outA);\n"
        "}\n";
    ID3DBlob *vb=NULL,*pb=NULL,*error=NULL;
    if (FAILED(D3DCompile(vs,strlen(vs),NULL,NULL,NULL,"main","vs_5_0",0,0,&vb,&error))) { if(error)error->Release(); return 0; }
    if (FAILED(D3DCompile(ps,strlen(ps),NULL,NULL,NULL,"main","ps_5_0",0,0,&pb,&error))) { if(error)error->Release(); vb->Release(); return 0; }
    D3D11_INPUT_ELEMENT_DESC layout[4]={{"POSITION",0,DXGI_FORMAT_R32G32_FLOAT,0,0,D3D11_INPUT_PER_VERTEX_DATA,0},
                                        {"COLOR",0,DXGI_FORMAT_R32G32B32A32_FLOAT,0,8,D3D11_INPUT_PER_VERTEX_DATA,0},
                                        {"TEXCOORD",0,DXGI_FORMAT_R32_FLOAT,0,24,D3D11_INPUT_PER_VERTEX_DATA,0},
                                        {"TEXCOORD",1,DXGI_FORMAT_R32G32_FLOAT,0,28,D3D11_INPUT_PER_VERTEX_DATA,0}};
    HRESULT a=g_dev->CreateVertexShader(vb->GetBufferPointer(),vb->GetBufferSize(),NULL,&g_mesh_vs);
    HRESULT b=g_dev->CreatePixelShader(pb->GetBufferPointer(),pb->GetBufferSize(),NULL,&g_mesh_ps);
    HRESULT c=g_dev->CreateInputLayout(layout,4,vb->GetBufferPointer(),vb->GetBufferSize(),&g_mesh_layout);
    vb->Release(); pb->Release();
    D3D11_BUFFER_DESC cbd={}; cbd.ByteWidth=16; cbd.Usage=D3D11_USAGE_DYNAMIC;
    cbd.BindFlags=D3D11_BIND_CONSTANT_BUFFER; cbd.CPUAccessFlags=D3D11_CPU_ACCESS_WRITE;
    HRESULT d=g_dev->CreateBuffer(&cbd,NULL,&g_mesh_cb);
    if (SUCCEEDED(a)&&SUCCEEDED(b)&&SUCCEEDED(c)&&SUCCEEDED(d)) return 1;
    if(g_mesh_vs){g_mesh_vs->Release();g_mesh_vs=NULL;}
    if(g_mesh_ps){g_mesh_ps->Release();g_mesh_ps=NULL;}
    if(g_mesh_layout){g_mesh_layout->Release();g_mesh_layout=NULL;}
    if(g_mesh_cb){g_mesh_cb->Release();g_mesh_cb=NULL;}
    return 0;
}

int r_fill_cham_triangles(const float* xy, int vertex_count, const unsigned short* indices, int triangle_count, RGBA color, int shader_mode) {
    color=opacity_color(color);
    if (!xy || !indices || vertex_count<=0 || triangle_count<=0 || g_w<=0 || g_h<=0 || !mesh_resources()) return 0;
    float minX=1e9f,minY=1e9f,maxX=-1e9f,maxY=-1e9f;
    for(int i=0;i<vertex_count;++i){
        float vx=xy[i*2],vy=xy[i*2+1];
        if(!isfinite(vx)||!isfinite(vy)||fabsf(vx)>g_w*4.0f||fabsf(vy)>g_h*4.0f)continue;
        if(vx<minX)minX=vx; if(vx>maxX)maxX=vx;
        if(vy<minY)minY=vy; if(vy>maxY)maxY=vy;
    }
    float spanX=(maxX-minX)>2.0f?(maxX-minX):2.0f;
    float spanY=(maxY-minY)>2.0f?(maxY-minY):2.0f;
    float alpha=((color>>24)&255)/255.0f;
    float r=((color>>16)&255)/255.0f*alpha, g=((color>>8)&255)/255.0f*alpha, b=(color&255)/255.0f*alpha;
    float mode=(float)shader_mode;
    int drawn=0;
    for (int i=0;i<triangle_count;++i) {
        int ia=indices[i*3],ib=indices[i*3+1],ic=indices[i*3+2];
        if (ia>=vertex_count || ib>=vertex_count || ic>=vertex_count) continue;
        float ax=xy[ia*2],ay=xy[ia*2+1], bx=xy[ib*2],by=xy[ib*2+1], cx=xy[ic*2],cy=xy[ic*2+1];
        if (!isfinite(ax)||!isfinite(ay)||!isfinite(bx)||!isfinite(by)||!isfinite(cx)||!isfinite(cy)) continue;
        if (fabsf(ax)>g_w*4.0f || fabsf(bx)>g_w*4.0f || fabsf(cx)>g_w*4.0f ||
            fabsf(ay)>g_h*4.0f || fabsf(by)>g_h*4.0f || fabsf(cy)>g_h*4.0f) continue;
        if (g_mesh_vertices.size()+3>400000) break;
        g_mesh_vertices.push_back({ax*2/g_w-1,1-ay*2/g_h,r,g,b,alpha,mode,(ax-minX)/spanX,(ay-minY)/spanY});
        g_mesh_vertices.push_back({bx*2/g_w-1,1-by*2/g_h,r,g,b,alpha,mode,(bx-minX)/spanX,(by-minY)/spanY});
        g_mesh_vertices.push_back({cx*2/g_w-1,1-cy*2/g_h,r,g,b,alpha,mode,(cx-minX)/spanX,(cy-minY)/spanY}); ++drawn;
    }
    return drawn;
}

// Handle r fill triangles.
int r_fill_triangles(const float* xy, int vertex_count, const unsigned short* indices, int triangle_count, RGBA color) {
    return r_fill_cham_triangles(xy, vertex_count, indices, triangle_count, color, 0);
}

// Handle mesh flush.
void gpu_mesh_flush(void) {
    if (g_mesh_vertices.empty() || !g_ctx || !g_dev || !g_rast || !g_nodepth || !g_blend) return;
    if(g_batch && g_d2dc){g_d2dc->EndDraw();g_batch=0;}
    unsigned bytes=(unsigned)(g_mesh_vertices.size()*sizeof(CHAM_VERTEX));
    if (!g_mesh_buffer || bytes>g_mesh_buffer_bytes) {
        if (g_mesh_buffer) { g_mesh_buffer->Release(); g_mesh_buffer=NULL; }
        g_mesh_buffer_bytes=(bytes+65535u)&~65535u;
        D3D11_BUFFER_DESC desc={}; desc.ByteWidth=g_mesh_buffer_bytes; desc.Usage=D3D11_USAGE_DYNAMIC;
        desc.BindFlags=D3D11_BIND_VERTEX_BUFFER; desc.CPUAccessFlags=D3D11_CPU_ACCESS_WRITE;
        if (FAILED(g_dev->CreateBuffer(&desc,NULL,&g_mesh_buffer))) { g_mesh_buffer_bytes=0; return; }
    }
    D3D11_MAPPED_SUBRESOURCE mapped={};
    if (FAILED(g_ctx->Map(g_mesh_buffer,0,D3D11_MAP_WRITE_DISCARD,0,&mapped))) return;
    memcpy(mapped.pData,g_mesh_vertices.data(),bytes); g_ctx->Unmap(g_mesh_buffer,0);
    D3D11_MAPPED_SUBRESOURCE cbm={};
    if (g_mesh_cb && SUCCEEDED(g_ctx->Map(g_mesh_cb,0,D3D11_MAP_WRITE_DISCARD,0,&cbm))) {
        float* cb=(float*)cbm.pData;
        cb[0]=(float)(GetTickCount64()%3600000ull)*0.001f; cb[1]=(float)g_w; cb[2]=(float)g_h; cb[3]=0.0f;
        g_ctx->Unmap(g_mesh_cb,0);
    }
    UINT stride=sizeof(CHAM_VERTEX),offset=0; float factor[4]={0,0,0,0};
    g_ctx->IASetInputLayout(g_mesh_layout); g_ctx->IASetVertexBuffers(0,1,&g_mesh_buffer,&stride,&offset);
    g_ctx->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    g_ctx->VSSetShader(g_mesh_vs,NULL,0); g_ctx->PSSetShader(g_mesh_ps,NULL,0);
    g_ctx->PSSetConstantBuffers(0,1,&g_mesh_cb);


    g_ctx->RSSetState(g_rast); g_ctx->OMSetBlendState(g_blend,factor,0xFFFFFFFF);
    g_ctx->OMSetDepthStencilState(g_nodepth,0); g_ctx->Draw((UINT)g_mesh_vertices.size(),0);
    g_ctx->OMSetBlendState(NULL,factor,0xFFFFFFFF);
    g_mesh_vertices.clear();
}

static int skybox_resources(void) {
    if (g_sky_vs && g_sky_ps && g_sky_cb) return 1;
    if (!g_dev) return 0;
    static const char* ps=
        "cbuffer SKY:register(b0){float4 fwd_time;float4 right_op;float4 up_mode;float4 fov_asp;};\n"
        "float hash21(float2 p){p=frac(p*float2(234.34,435.345));p+=dot(p,p+34.23);return frac(p.x*p.y);}\n"
        "float noise(float2 p){float2 i=floor(p),f=frac(p);f=f*f*(3.0-2.0*f);return lerp(lerp(hash21(i),hash21(i+float2(1,0)),f.x),lerp(hash21(i+float2(0,1)),hash21(i+float2(1,1)),f.x),f.y);}\n"
        "float fbm(float2 p){float v=0,a=0.5;for(int i=0;i<4;i++){v+=a*noise(p);p=p*2.03+float2(17.1,31.7);a*=0.5;}return v;}\n"
        "float stars(float2 sph,float scale,float thresh,float time){\n"
        "    float2 cell=floor(sph*scale);\n"
        "    float2 f=frac(sph*scale)-0.5;\n"
        "    float h=hash21(cell);\n"
        "    float d=length(f);\n"
        "    return step(thresh,h)*smoothstep(0.32,0.04,d)*(0.45+0.55*sin(time*3.5+h*62.8));\n"
        "}\n"
        "float4 main(float4 pos:SV_POSITION,float2 uv:TEXCOORD0):SV_TARGET{\n"
        "    float time=fwd_time.w, opacity=right_op.w, mode=up_mode.w, tanH=fov_asp.x, asp=fov_asp.y;\n"
        "    float2 ndc=float2(uv.x*2.0-1.0,1.0-uv.y*2.0);\n"
        "    float3 rd=normalize(fwd_time.xyz + right_op.xyz*(ndc.x*asp*tanH) + up_mode.xyz*(ndc.y*tanH));\n"
        "    float skyMask=smoothstep(-0.12,0.12,rd.y);\n"
        "    if(skyMask<=0.002)return float4(0,0,0,0);\n"
        "    float phi=atan2(rd.z,rd.x)/6.2831853+0.5;\n"
        "    float theta=asin(clamp(rd.y,-1.0,1.0))/1.5707963;\n"
        "    float2 sph=float2(phi*4.0,theta);\n"
        "    float3 col=float3(0,0,0);\n"
        "    float detailAlpha=0.0;\n"
        "    int m=(int)(mode+0.5);\n"
        "    if(m==0){\n" // Crisp Aurora Curtains + Starfield
        "        float n1=fbm(float2(phi*10.0+time*0.025,theta*2.0-time*0.015));\n"
        "        float rays=0.65+0.35*sin(phi*90.0+n1*8.0+time*0.8);\n"
        "        float ribbon=exp(-pow((theta-0.30-0.13*sin(phi*12.566+time*0.4+n1*3.0))*7.5,2.0))*rays;\n"
        "        float ribbon2=exp(-pow((theta-0.48-0.10*cos(phi*18.85-time*0.3+n1*2.5))*9.0,2.0));\n"
        "        float3 aur=lerp(float3(0.05,1.0,0.62),float3(0.55,0.22,1.0),0.5+0.5*sin(phi*12.566-time*0.35));\n"
        "        float st=stars(sph,75.0,0.975,time)+stars(sph,40.0,0.988,time*0.7)*1.4;\n"
        "        col=aur*(ribbon*1.15+ribbon2*0.65)+float3(0.92,0.97,1.0)*st;\n"
        "        detailAlpha=saturate(ribbon*0.85+ribbon2*0.45+st+0.28);\n"
        "    }else if(m==1){\n" // Deep Nebula & Milky Way
        "        float n1=fbm(sph*2.8+float2(time*0.015,-time*0.01));\n"
        "        float n2=fbm(sph*5.8-float2(time*0.02,time*0.015));\n"
        "        float dust=smoothstep(0.32,0.85,n1);\n"
        "        col=lerp(float3(0.02,0.01,0.07),float3(0.45,0.12,0.78),dust)+float3(0.12,0.68,1.0)*pow(n2,2.4);\n"
        "        float st=stars(sph,85.0,0.972,time)+stars(sph,42.0,0.986,time*0.6)*1.6;\n"
        "        col+=float3(1.0,0.96,0.92)*st;\n"
        "        detailAlpha=saturate(dust*0.75+pow(n2,2.0)*0.6+st+0.32);\n"
        "    }else if(m==2){\n" // Crisp Vaporwave Sunset
        "        float t=saturate(theta*1.5);\n"
        "        col=lerp(float3(1.0,0.32,0.28),lerp(float3(0.62,0.12,0.58),float3(0.05,0.03,0.18),t),t);\n"
        "        float3 sunDir=normalize(float3(0.0,0.28,1.0));\n"
        "        float sd=acos(clamp(dot(rd,sunDir),-1.0,1.0));\n"
        "        float sun=smoothstep(0.19,0.17,sd);\n"
        "        float bands=step(0.22,frac(rd.y*38.0-time*0.45));\n"
        "        col=lerp(col,lerp(float3(1.0,0.90,0.30),float3(1.0,0.25,0.62),saturate((0.38-rd.y)*3.2)),sun*bands);\n"
        "        float st=stars(sph,80.0,0.98,time)*smoothstep(0.28,0.65,theta);\n"
        "        col+=float3(1.0,0.95,1.0)*st;\n"
        "        detailAlpha=saturate(0.45+sun*0.55+st);\n"
        "    }else if(m==3){\n" // Cyber Dome
        "        float t=saturate(theta*1.4);\n"
        "        float2 g=abs(frac(float2(phi*28.0,theta*16.0+time*0.16))-0.5);\n"
        "        float gridLine=smoothstep(0.055,0.0,min(g.x,g.y));\n"
        "        col=lerp(float3(0.0,0.95,1.0),float3(1.0,0.16,0.78),t)*gridLine*1.2;\n"
        "        detailAlpha=saturate(gridLine*0.92+0.15);\n"
        "    }else if(m==4){\n" // Storm Clouds & Lightning
        "        float c1=fbm(sph*3.4+float2(time*0.06,time*0.015));\n"
        "        float c2=fbm(sph*6.8-float2(time*0.10,0.0));\n"
        "        float flash=pow(max(0.0,sin(time*2.1)*sin(time*5.3)),14.0);\n"
        "        col=lerp(float3(0.03,0.04,0.08),float3(0.25,0.32,0.45),smoothstep(0.25,0.8,c1))+float3(0.75,0.90,1.0)*flash*c2*1.8;\n"
        "        detailAlpha=saturate(smoothstep(0.2,0.85,c1)*0.75+flash*c2+0.25);\n"
        "    }else{\n" // Crescent Moon & Midnight
        "        float3 moonDir=normalize(float3(0.32,0.44,0.84));\n"
        "        float md=acos(clamp(dot(rd,moonDir),-1.0,1.0));\n"
        "        float moon=smoothstep(0.085,0.074,md);\n"
        "        float halo=exp(-md*9.0)*0.4;\n"
        "        float st=stars(sph,80.0,0.974,time)+stars(sph,40.0,0.988,time*0.8)*1.5;\n"
        "        col=float3(0.03,0.04,0.12)+float3(0.98,0.96,0.90)*(moon+halo)+float3(0.88,0.94,1.0)*st;\n"
        "        detailAlpha=saturate(moon+halo*0.6+st+0.28);\n"
        "    }\n"
        "    float a=saturate(opacity)*skyMask*detailAlpha;\n"
        "    return float4(saturate(col)*a,a);\n"
        "}\n";
    ID3DBlob *vb=NULL,*pb=NULL,*err=NULL;
    if (FAILED(D3DCompile(VS_SRC,strlen(VS_SRC),NULL,NULL,NULL,"main","vs_5_0",0,0,&vb,&err))) { if(err)err->Release(); return 0; }
    if (FAILED(D3DCompile(ps,strlen(ps),NULL,NULL,NULL,"main","ps_5_0",0,0,&pb,&err))) { if(err)err->Release(); vb->Release(); return 0; }
    HRESULT a=g_dev->CreateVertexShader(vb->GetBufferPointer(),vb->GetBufferSize(),NULL,&g_sky_vs);
    HRESULT b=g_dev->CreatePixelShader(pb->GetBufferPointer(),pb->GetBufferSize(),NULL,&g_sky_ps);
    vb->Release(); pb->Release();
    D3D11_BUFFER_DESC cbd={}; cbd.ByteWidth=64; cbd.Usage=D3D11_USAGE_DYNAMIC;
    cbd.BindFlags=D3D11_BIND_CONSTANT_BUFFER; cbd.CPUAccessFlags=D3D11_CPU_ACCESS_WRITE;
    HRESULT c=g_dev->CreateBuffer(&cbd,NULL,&g_sky_cb);
    if (SUCCEEDED(a)&&SUCCEEDED(b)&&SUCCEEDED(c)) return 1;
    if(g_sky_vs){g_sky_vs->Release();g_sky_vs=NULL;}
    if(g_sky_ps){g_sky_ps->Release();g_sky_ps=NULL;}
    if(g_sky_cb){g_sky_cb->Release();g_sky_cb=NULL;}
    return 0;
}

void gpu_draw_skybox(int shader_mode, int opacity,
                     float fx, float fy, float fz,
                     float rx, float ry, float rz,
                     float ux, float uy, float uz,
                     float fov_deg, float aspect) {
    if (!g_ctx || !g_dev || !g_rtv || !g_blend || !g_rast || !g_nodepth || g_w<=0 || g_h<=0) return;
    if (!skybox_resources()) return;
    if (g_batch && g_d2dc) { g_d2dc->EndDraw(); g_batch=0; }
    float tanH = tanf((fov_deg > 1.0f && fov_deg < 179.0f ? fov_deg : 60.0f) * 3.14159265f / 360.0f);
    float asp = (aspect > 0.25f && aspect < 8.0f) ? aspect : ((float)g_w / (float)g_h);
    D3D11_MAPPED_SUBRESOURCE cbm={};
    if (SUCCEEDED(g_ctx->Map(g_sky_cb,0,D3D11_MAP_WRITE_DISCARD,0,&cbm))) {
        float* cb=(float*)cbm.pData;
        cb[0]=fx; cb[1]=fy; cb[2]=fz; cb[3]=(float)(GetTickCount64()%3600000ull)*0.001f;
        cb[4]=rx; cb[5]=ry; cb[6]=rz; cb[7]=(float)(opacity<0?0:(opacity>100?100:opacity))/100.0f;
        cb[8]=ux; cb[9]=uy; cb[10]=uz; cb[11]=(float)shader_mode;
        cb[12]=tanH; cb[13]=asp; cb[14]=(float)g_w; cb[15]=(float)g_h;
        g_ctx->Unmap(g_sky_cb,0);
    }
    D3D11_VIEWPORT vp={0,0,(float)g_w,(float)g_h,0,1};
    float factor[4]={0,0,0,0};
    g_ctx->OMSetRenderTargets(1,&g_rtv,NULL);
    g_ctx->RSSetViewports(1,&vp);
    g_ctx->IASetInputLayout(NULL);
    g_ctx->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    g_ctx->VSSetShader(g_sky_vs,NULL,0);
    g_ctx->PSSetShader(g_sky_ps,NULL,0);
    g_ctx->PSSetConstantBuffers(0,1,&g_sky_cb);
    g_ctx->RSSetState(g_rast);
    g_ctx->OMSetBlendState(g_blend,factor,0xFFFFFFFF);
    g_ctx->OMSetDepthStencilState(g_nodepth,0);
    g_ctx->Draw(3,0);
    g_ctx->PSSetShader(NULL,NULL,0);
}

// Handle r polyline.
void r_polyline(const float* xy, int n, RGBA c, float w) {
    if (!g_d2dc || !xy || n < 2) return;
    ensure_batch();
    ID2D1SolidColorBrush* b = brush_for(c);
    if (!b) return;

    for (int i = 0; i + 1 < n; i++)
        g_d2dc->DrawLine(D2D1::Point2F(xy[i * 2], xy[i * 2 + 1]),
                         D2D1::Point2F(xy[i * 2 + 2], xy[i * 2 + 3]), b, w < 1.0f ? 1.0f : w);
}


// Handle chams resources.
static int chams_resources(void) {
    ID3DBlob* vsb = NULL, *psb = NULL, *err = NULL;
    if (FAILED(D3DCompile(VS_SRC, strlen(VS_SRC), NULL, NULL, NULL, "main", "vs_5_0", 0, 0, &vsb, &err))) {
        if (err) err->Release();
        return 0;
    }
    if (FAILED(D3DCompile(PS_SRC, strlen(PS_SRC), NULL, NULL, NULL, "main", "ps_5_0", 0, 0, &psb, &err))) {
        if (err) err->Release();
        vsb->Release();
        return 0;
    }
    g_dev->CreateVertexShader(vsb->GetBufferPointer(), vsb->GetBufferSize(), NULL, &g_vs);
    g_dev->CreatePixelShader(psb->GetBufferPointer(), psb->GetBufferSize(), NULL, &g_ps);
    vsb->Release(); psb->Release();

    D3D11_BUFFER_DESC cbd = {0};
    cbd.ByteWidth = sizeof(CH_PARAMS);
    cbd.Usage = D3D11_USAGE_DYNAMIC;
    cbd.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
    cbd.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
    g_dev->CreateBuffer(&cbd, NULL, &g_cb);

    D3D11_SAMPLER_DESC sd = {0};
    sd.Filter = D3D11_FILTER_MIN_MAG_MIP_POINT;
    sd.AddressU = sd.AddressV = sd.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
    sd.MaxLOD = D3D11_FLOAT32_MAX;
    g_dev->CreateSamplerState(&sd, &g_chsmp);

    D3D11_BLEND_DESC bd = {0};
    bd.RenderTarget[0].BlendEnable = TRUE;
    bd.RenderTarget[0].SrcBlend = D3D11_BLEND_ONE;
    bd.RenderTarget[0].DestBlend = D3D11_BLEND_INV_SRC_ALPHA;
    bd.RenderTarget[0].BlendOp = D3D11_BLEND_OP_ADD;
    bd.RenderTarget[0].SrcBlendAlpha = D3D11_BLEND_ONE;
    bd.RenderTarget[0].DestBlendAlpha = D3D11_BLEND_INV_SRC_ALPHA;
    bd.RenderTarget[0].BlendOpAlpha = D3D11_BLEND_OP_ADD;
    bd.RenderTarget[0].RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_ALL;
    g_dev->CreateBlendState(&bd, &g_blend);

    D3D11_RASTERIZER_DESC rd = {0};
    rd.FillMode = D3D11_FILL_SOLID;
    rd.CullMode = D3D11_CULL_NONE;
    rd.ScissorEnable = FALSE;
    rd.DepthClipEnable = TRUE;
    g_dev->CreateRasterizerState(&rd, &g_rast);

    D3D11_DEPTH_STENCIL_DESC dd = {0};
    dd.DepthEnable = FALSE;
    g_dev->CreateDepthStencilState(&dd, &g_nodepth);

    return g_vs && g_ps && g_cb && g_chsmp && g_blend && g_rast && g_nodepth;
}

// Handle chams alloc.
void gpu_chams_alloc(int w, int h) {
    if (!g_dev) return;
    if (g_chtex && g_chw == w && g_chh == h) return;
    if (g_chsrv) { g_chsrv->Release(); g_chsrv = NULL; }
    if (g_chtex) { g_chtex->Release(); g_chtex = NULL; }
    if (w <= 0 || h <= 0) return;

    D3D11_TEXTURE2D_DESC td = {0};
    td.Width = w; td.Height = h;
    td.MipLevels = 1; td.ArraySize = 1;
    td.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    td.SampleDesc.Count = 1;
    td.Usage = D3D11_USAGE_DEFAULT;
    td.BindFlags = D3D11_BIND_SHADER_RESOURCE;
    if (FAILED(g_dev->CreateTexture2D(&td, NULL, &g_chtex))) { g_chtex = NULL; return; }
    g_dev->CreateShaderResourceView(g_chtex, NULL, &g_chsrv);
    g_chw = w; g_chh = h;

    if (!g_ch_ok) g_ch_ok = chams_resources();
}

// Handle chams upload.
void gpu_chams_upload(const uint32_t* px, int w, int h, int stride,
                      int x0, int y0, int x1, int y1) {
    if (!g_chtex || !px || !g_ctx) return;
    if (x0 < 0) x0 = 0;
    if (y0 < 0) y0 = 0;
    if (x1 > w) x1 = w;
    if (y1 > h) y1 = h;
    if (x1 <= x0 || y1 <= y0) return;
    if (x1 > g_chw) x1 = g_chw;
    if (y1 > g_chh) y1 = g_chh;

    D3D11_BOX box = { (UINT)x0, (UINT)y0, 0, (UINT)x1, (UINT)y1, 1 };
    const uint8_t* src = (const uint8_t*)px + (size_t)y0 * stride + (size_t)x0 * 4;
    g_ctx->UpdateSubresource(g_chtex, 0, &box, src, (UINT)stride, 0);
}

// Handle chams draw.
void gpu_chams_draw(int outline_px, RGBA outline_col, float opacity) {
    if (!g_chtex || !g_chsrv || !g_ch_ok || g_batch) return;
    if (opacity <= 0.0f) return;

    D3D11_MAPPED_SUBRESOURCE m;
    if (SUCCEEDED(g_ctx->Map(g_cb, 0, D3D11_MAP_WRITE_DISCARD, 0, &m))) {
        CH_PARAMS* p = (CH_PARAMS*)m.pData;
        p->texel[0] = 1.0f / (float)g_chw;
        p->texel[1] = 1.0f / (float)g_chh;
        p->radius = (float)(outline_px < 0 ? 0 : outline_px);
        p->opacity = opacity;
        p->col[0] = ((outline_col >> 16) & 0xFF) / 255.0f;
        p->col[1] = ((outline_col >> 8) & 0xFF) / 255.0f;
        p->col[2] = (outline_col & 0xFF) / 255.0f;
        p->col[3] = ((outline_col >> 24) & 0xFF) / 255.0f;
        g_ctx->Unmap(g_cb, 0);
    }

    ID3D11ShaderResourceView* srv = g_chsrv;
    ID3D11Buffer* cb = g_cb;
    ID3D11SamplerState* smp = g_chsmp;
    ID3D11BlendState* bl = g_blend;
    ID3D11RasterizerState* rs = g_rast;
    ID3D11DepthStencilState* ds = g_nodepth;

    g_ctx->IASetInputLayout(NULL);
    g_ctx->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    g_ctx->VSSetShader(g_vs, NULL, 0);
    g_ctx->PSSetShader(g_ps, NULL, 0);
    g_ctx->PSSetShaderResources(0, 1, &srv);
    g_ctx->PSSetSamplers(0, 1, &smp);
    g_ctx->PSSetConstantBuffers(0, 1, &cb);
    g_ctx->OMSetBlendState(bl, NULL, 0xFFFFFFFF);
    g_ctx->OMSetDepthStencilState(ds, 0);
    g_ctx->RSSetState(rs);
    g_ctx->Draw(3, 0);

    g_ctx->OMSetBlendState(NULL, NULL, 0xFFFFFFFF);
    g_ctx->OMSetDepthStencilState(NULL, 0);
    g_ctx->RSSetState(NULL);
}


// Create swap.
static int create_swap(void) {
    IDXGIDevice* dxdev = NULL;
    if (FAILED(g_dev->QueryInterface(__uuidof(IDXGIDevice), (void**)&dxdev))) return 0;
    IDXGIAdapter* adapter = NULL;
    dxdev->GetAdapter(&adapter);
    IDXGIFactory2* fac = NULL;
    adapter->GetParent(__uuidof(IDXGIFactory2), (void**)&fac);

    DXGI_SWAP_CHAIN_DESC1 sd = {0};
    sd.Width = g_w;
    sd.Height = g_h;
    sd.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    sd.SampleDesc.Count = 1;
    sd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    sd.BufferCount = 2;
    sd.SwapEffect = DXGI_SWAP_EFFECT_FLIP_SEQUENTIAL;
    sd.AlphaMode = DXGI_ALPHA_MODE_PREMULTIPLIED;
    sd.Scaling = DXGI_SCALING_STRETCH;
    sd.Flags = DXGI_SWAP_CHAIN_FLAG_FRAME_LATENCY_WAITABLE_OBJECT;

    HRESULT hr = fac->CreateSwapChainForComposition(g_dev, &sd, NULL, &g_swap);
    if (FAILED(hr)) {
        sd.Flags = 0;
        hr = fac->CreateSwapChainForComposition(g_dev, &sd, NULL, &g_swap);
    }
    g_swap_flags = sd.Flags;
    IDXGIDevice1* latency_device=NULL;
    if(SUCCEEDED(dxdev->QueryInterface(IID_PPV_ARGS(&latency_device)))){
        latency_device->SetMaximumFrameLatency(1);latency_device->Release();
    }
    if (SUCCEEDED(hr) && g_swap && sd.Flags) {
        IDXGISwapChain2* paced = NULL;
        if (SUCCEEDED(g_swap->QueryInterface(__uuidof(IDXGISwapChain2), (void**)&paced))) {
            if (SUCCEEDED(paced->SetMaximumFrameLatency(1)))
                g_frame_event = paced->GetFrameLatencyWaitableObject();
            paced->Release();
        }
    }
    fac->Release();
    adapter->Release();
    dxdev->Release();
    return SUCCEEDED(hr) && g_swap;
}

// Initialize.
int gpu_init(HWND hwnd, int w, int h) {
    g_hwnd = hwnd; g_w = w; g_h = h;

    D3D_FEATURE_LEVEL lv[] = { D3D_FEATURE_LEVEL_11_1, D3D_FEATURE_LEVEL_11_0,
                               D3D_FEATURE_LEVEL_10_1, D3D_FEATURE_LEVEL_10_0 };
    UINT flags = D3D11_CREATE_DEVICE_BGRA_SUPPORT;
    HRESULT hr = D3D11CreateDevice(NULL, D3D_DRIVER_TYPE_HARDWARE, NULL, flags,
                                   lv, ARRAYSIZE(lv), D3D11_SDK_VERSION,
                                   &g_dev, NULL, &g_ctx);
    if (FAILED(hr)) return 0;

    if (!create_swap()) return 0;

    IDXGIDevice* dxdev = NULL;
    g_dev->QueryInterface(__uuidof(IDXGIDevice), (void**)&dxdev);

    D2D1_FACTORY_OPTIONS fo = { D2D1_DEBUG_LEVEL_NONE };
    if (FAILED(D2D1CreateFactory(D2D1_FACTORY_TYPE_SINGLE_THREADED,
                                 __uuidof(ID2D1Factory1), &fo, (void**)&g_d2df))) return 0;
    if (FAILED(g_d2df->CreateDevice(dxdev, &g_d2ddev))) return 0;
    if (FAILED(g_d2ddev->CreateDeviceContext(D2D1_DEVICE_CONTEXT_OPTIONS_NONE, &g_d2dc))) return 0;
    g_d2dc->SetDpi(96.0f, 96.0f);

    if (FAILED(DWriteCreateFactory(DWRITE_FACTORY_TYPE_SHARED, __uuidof(IDWriteFactory5),
                                   (IUnknown**)&g_dwf))) return 0;
    font_from_file();
    make_format(13.0f);

    if (FAILED(DCompositionCreateDevice(dxdev, __uuidof(IDCompositionDevice), (void**)&g_dcomp)))
        return 0;
    if (FAILED(g_dcomp->CreateTargetForHwnd(hwnd, TRUE, &g_dtarget))) return 0;
    if (FAILED(g_dcomp->CreateVisual(&g_dvisual))) return 0;
    g_dvisual->SetContent(g_swap);
    g_dtarget->SetRoot(g_dvisual);
    g_dcomp->Commit();

    if (dxdev) dxdev->Release();

    gpu_chams_alloc(w, h);
    InterlockedExchange(&g_icon_stop,0);
    g_icon_wake=CreateEventW(NULL,FALSE,FALSE,NULL);
    if(g_icon_wake)g_icon_thread=CreateThread(NULL,0,icon_worker,NULL,0,NULL);
    g_ready = 1;
    return 1;
}

// Handle ready.
int gpu_ready(void) { return g_ready; }

// Release.
void gpu_shutdown(void) {
    InterlockedExchange(&g_icon_stop,1);
    if(g_icon_wake)SetEvent(g_icon_wake);
    if(g_icon_thread){WaitForSingleObject(g_icon_thread,INFINITE);CloseHandle(g_icon_thread);g_icon_thread=NULL;}
    if(g_icon_wake){CloseHandle(g_icon_wake);g_icon_wake=NULL;}
    for(auto& icon:g_icons){std::vector<BYTE>().swap(icon.bytes);InterlockedExchange(&icon.state,0);}
    if(g_batch && g_d2dc)g_d2dc->EndDraw();g_batch=0;
    for(auto& bitmap:g_icon_bitmaps){if(bitmap){bitmap->Release();bitmap=NULL;}}

    if (g_frame_event) { CloseHandle(g_frame_event); g_frame_event = NULL; }
    InterlockedExchange(&g_frame_granted, 0);
    release_target();
    if (g_mesh_buffer) { g_mesh_buffer->Release(); g_mesh_buffer=NULL; g_mesh_buffer_bytes=0; }
    if (g_mesh_layout) { g_mesh_layout->Release(); g_mesh_layout=NULL; }
    if (g_mesh_vs) { g_mesh_vs->Release(); g_mesh_vs=NULL; }
    if (g_mesh_ps) { g_mesh_ps->Release(); g_mesh_ps=NULL; }
    if (g_mesh_cb) { g_mesh_cb->Release(); g_mesh_cb=NULL; }
    if (g_sky_vs) { g_sky_vs->Release(); g_sky_vs=NULL; }
    if (g_sky_ps) { g_sky_ps->Release(); g_sky_ps=NULL; }
    if (g_sky_cb) { g_sky_cb->Release(); g_sky_cb=NULL; }
    g_mesh_vertices.clear();
    if(g_chsrv){g_chsrv->Release();g_chsrv=NULL;}
    if(g_chtex){g_chtex->Release();g_chtex=NULL;}
    if(g_chsmp){g_chsmp->Release();g_chsmp=NULL;}
    if(g_vs){g_vs->Release();g_vs=NULL;}if(g_ps){g_ps->Release();g_ps=NULL;}
    if(g_cb){g_cb->Release();g_cb=NULL;}if(g_blend){g_blend->Release();g_blend=NULL;}
    if(g_rast){g_rast->Release();g_rast=NULL;}if(g_nodepth){g_nodepth->Release();g_nodepth=NULL;}
    g_ch_ok=0;g_chw=g_chh=0;
    if (g_brush) { g_brush->Release(); g_brush = NULL; }
    for(auto& entry:g_text_cache){if(entry.layout){entry.layout->Release();entry.layout=NULL;}entry.text.clear();}
    for(auto& entry:g_fonts){if(entry.format){entry.format->Release();entry.format=NULL;}}
    g_fmt=NULL;g_font_cursor=0;g_brush_valid=0;
    if (g_fontcoll) { g_fontcoll->Release(); g_fontcoll = NULL; }
    if (g_dwf) { g_dwf->Release(); g_dwf = NULL; }
    if (g_dvisual) { g_dvisual->Release(); g_dvisual = NULL; }
    if (g_dtarget) { g_dtarget->Release(); g_dtarget = NULL; }
    if (g_dcomp) { g_dcomp->Release(); g_dcomp = NULL; }
    if (g_d2dc) { g_d2dc->Release(); g_d2dc = NULL; }
    if (g_d2ddev) { g_d2ddev->Release(); g_d2ddev = NULL; }
    if (g_d2df) { g_d2df->Release(); g_d2df = NULL; }
    if (g_swap) { g_swap->Release(); g_swap = NULL; }
    if (g_ctx) { g_ctx->Release(); g_ctx = NULL; }
    if (g_dev) { g_dev->Release(); g_dev = NULL; }
    g_ready = 0;
}

// Handle size.
void gpu_size(int w, int h) {
    if (!g_swap || w <= 0 || h <= 0) return;
    if (w == g_w && h == g_h) return;
    if(g_batch && g_d2dc)g_d2dc->EndDraw();
    g_batch=0;
    release_target();
    g_ctx->OMSetRenderTargets(0, NULL, NULL);
    if (FAILED(g_swap->ResizeBuffers(0, w, h, DXGI_FORMAT_UNKNOWN, g_swap_flags))) return;
    g_w = w; g_h = h;
    gpu_chams_alloc(w, h);
}


// Handle begin.
void gpu_begin(void) {
    if(!g_ready || !g_swap || g_batch)return;
    g_mesh_vertices.clear();g_icons_uploaded=0;
    ID3D11Texture2D* back=NULL;
    if(FAILED(g_swap->GetBuffer(0,IID_PPV_ARGS(&back))) || !back)return;
    if(back!=g_back || !g_target || !g_rtv){
        release_target();g_back=back;back=NULL;
        if(FAILED(g_dev->CreateRenderTargetView(g_back,NULL,&g_rtv)))return;
        IDXGISurface* surface=NULL;
        if(FAILED(g_back->QueryInterface(IID_PPV_ARGS(&surface))))return;
        D2D1_BITMAP_PROPERTIES1 properties={};
        properties.pixelFormat=D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM,D2D1_ALPHA_MODE_PREMULTIPLIED);
        properties.bitmapOptions=D2D1_BITMAP_OPTIONS_TARGET|D2D1_BITMAP_OPTIONS_CANNOT_DRAW;
        properties.dpiX=properties.dpiY=96;
        HRESULT hr=g_d2dc->CreateBitmapFromDxgiSurface(surface,&properties,&g_target);
        surface->Release();if(FAILED(hr))return;
    }
    if(back)back->Release();
    const float clear[4]={0,0,0,0};g_ctx->ClearRenderTargetView(g_rtv,clear);
    g_ctx->OMSetRenderTargets(1,&g_rtv,NULL);
    D3D11_VIEWPORT viewport={0,0,(float)g_w,(float)g_h,0,1};g_ctx->RSSetViewports(1,&viewport);
    g_d2dc->SetTarget(g_target);
}

// Expose the compositor pacing signal.
HANDLE gpu_frame_handle(void){return g_frame_event;}
// Preserve a consumed compositor signal.
void gpu_grant_frame(void){InterlockedExchange(&g_frame_granted,1);}
// Wait for compositor capacity without a render polling interval.
int gpu_frame_ready(void) {
    static DWORD last_ready;
    if(!g_ready)return 1;
    if(InterlockedCompareExchange(&g_frame_granted,0,0))return 1;
    if(!g_frame_event){
        static LARGE_INTEGER frequency,last;LARGE_INTEGER now;QueryPerformanceCounter(&now);
        if(!frequency.QuadPart)QueryPerformanceFrequency(&frequency);
        if(now.QuadPart-last.QuadPart<frequency.QuadPart/240)return 0;
        last=now;gpu_grant_frame();return 1;
    }
    DWORD now=GetTickCount();
    if(WaitForSingleObject(g_frame_event,0)!=WAIT_OBJECT_0 && now-last_ready<100)return 0;
    last_ready=now;InterlockedExchange(&g_frame_granted,1);return 1;
}

// Present the latest frame.
int gpu_present(void) {
    if(!g_ready || !g_swap)return 0;
    HRESULT drawn=S_OK;
    if(g_batch){drawn=g_d2dc->EndDraw();g_batch=0;}
    g_d2dc->SetTarget(NULL);
    if(FAILED(drawn)){
        if(drawn==D2DERR_RECREATE_TARGET){gpu_shutdown();return 0;}
        release_target();return 0;
    }
    HRESULT hr=g_swap->Present(0,DXGI_PRESENT_DO_NOT_WAIT);
    if(hr==DXGI_ERROR_DEVICE_REMOVED || hr==DXGI_ERROR_DEVICE_RESET){gpu_shutdown();return 0;}
    if(FAILED(hr)){InterlockedExchange(&g_frame_granted,0);return 0;}
    InterlockedExchange(&g_frame_granted,0);return 1;
}
