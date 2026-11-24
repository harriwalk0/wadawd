                                                     
  
                                                                           
                                                                          
                                                                             
                                                                  
   
#ifndef WDE_GPU_H
#define WDE_GPU_H

#include <stdint.h>
#include <windows.h>

#ifdef __cplusplus
extern "C" {
#endif

                                                                           
typedef uint32_t RGBA;

#define RRGB(r,g,b)      (0xFF000000u | ((uint32_t)(r)<<16) | ((uint32_t)(g)<<8) | (uint32_t)(b))
#define RARGB(a,r,g,b)   (((uint32_t)(a)<<24) | ((uint32_t)(r)<<16) | ((uint32_t)(g)<<8) | (uint32_t)(b))
#define RCOLORREF(c)     (0xFF000000u | ((uint32_t)GetRValue(c)<<16) | ((uint32_t)GetGValue(c)<<8) | (uint32_t)GetBValue(c))
#define RA(c)            ((int)(((c)>>24)&0xFF))

                
int  gpu_init(HWND hwnd, int w, int h);                                            
void gpu_shutdown(void);
int  gpu_ready(void);
void gpu_size(int w, int h);

            
void gpu_begin(void);                                                   
int  gpu_present(void);                                                     
int  gpu_frame_ready(void);
HANDLE gpu_frame_handle(void);
void gpu_grant_frame(void);                                     

            
void gpu_pen(RGBA c, float width);
void gpu_font(float px);                                              
void gpu_opacity(float opacity);
void gpu_text_style(int face,int background,int outline);

                                        
void r_line(float x1, float y1, float x2, float y2);
void r_line_c(float x1, float y1, float x2, float y2, RGBA c, float w);
void r_rect(float x, float y, float w, float h, RGBA c, float stroke);
void r_round_rect(float x,float y,float w,float h,float radius,RGBA c,float stroke);
int r_icon(int resource_id,float x,float y,float size);
void r_fill(float x, float y, float w, float h, RGBA c);
void r_ellipse(float cx, float cy, float rx, float ry, RGBA c, float stroke);
void r_fill_ellipse(float cx, float cy, float rx, float ry, RGBA c);
int  r_fill_triangles(const float* xy, int vertex_count, const unsigned short* indices, int triangle_count, RGBA color);
int  r_fill_cham_triangles(const float* xy, int vertex_count, const unsigned short* indices, int triangle_count, RGBA color, int shader_mode);
void gpu_draw_skybox(int shader_mode, int opacity,
                     float fx, float fy, float fz,
                     float rx, float ry, float rz,
                     float ux, float uy, float uz,
                     float fov_deg, float aspect);
void gpu_mesh_flush(void);
void r_polyline(const float* xy, int n, RGBA c, float w);

                                                                          
                                                                        
   
#define RT_LEFT    0
#define RT_CENTER  1
#define RT_RIGHT   2
#define RT_OUTLINE 4
void r_text(float x, float y, const char* utf8, RGBA c, int flags);
void r_text_w(float x, float y, const wchar_t* s, int len, RGBA c, int flags);
float r_text_width(const char* utf8);
float r_text_height(void);

                        
                                                                           
                                                                            
   
void gpu_chams_alloc(int w, int h);
void gpu_chams_upload(const uint32_t* px, int w, int h, int stride,
                      int x0, int y0, int x1, int y1);
void gpu_chams_draw(int outline_px, RGBA outline_col, float opacity);

#ifdef __cplusplus
}
#endif

#endif
