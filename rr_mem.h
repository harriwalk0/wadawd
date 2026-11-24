                                                                    
  
                                                                          
                                                                            
                                                                             
                                                                            
                                              
   
#ifndef RR_MEM_H
#define RR_MEM_H

#include <stdint.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

                
int  rr_drv_open(void);                                                        
void rr_drv_close(void);
int  rr_drv_live(void);                                                        
int  rr_drv_ready(void);                                                       

                                                                        
                                                                       
                                                                              
   
int  rr_drv_hide_process(uint32_t pid, int hide);

                                                               
int  rr_read (uint64_t addr, void* out, size_t n);

                                                                            
                                                                            
                                                                                
                                                                      
   
typedef struct RR_RD_REQ {
    uint64_t addr;
    uint32_t size;
    void*    dst;
    int      ok;
} RR_RD_REQ;
int  rr_read_batch(RR_RD_REQ* items, int n);
extern long g_batch_status, g_batch_info, g_batch_expected;
int  rr_write(uint64_t addr, const void* in, size_t n);

                                                                       
typedef struct RR_REGION {
    uint64_t base, size;
    uint32_t state, protect, type;
} RR_REGION;
int  rr_query_region(uint64_t addr, RR_REGION* out);

                                                                
                                                                         
                                                     
                                                            
   
int  rr_drv_alloc_remote(uint32_t pid, size_t size, uint32_t protect, uint64_t* out);

                                                               
                                                                  
typedef struct RR_THREAD_CTX {
    uint64_t rip, rsp, rbp, rflags;
    uint32_t tid, ok;
} RR_THREAD_CTX;
long rr_drv_get_context(uint32_t thread_id, RR_THREAD_CTX* out);

                                                                          
                                               
   
uint64_t rr_u64(uint64_t addr);
uint32_t rr_u32(uint64_t addr);
int32_t  rr_i32(uint64_t addr);
float    rr_f32(uint64_t addr);
int      rr_ok(void);                                                          

                     
extern uint32_t g_rr_pid;
int      rr_attach(const char* exe_name);                                     
void     rr_force_pid(uint32_t pid);                                         
                                                                       
                                                                         
   
int      rr_process_visible(uint32_t pid);
uint64_t rr_image_base(void);                                                 
uint64_t rr_peb(void);                                                      

                                                                       
                                                                        
                                     
int rr_module(const char* name, uint64_t* base, uint32_t* size);

                                                                          
                                                                        
                                                                           
                            
int rr_modules(char* names, uint64_t* bases, uint32_t* sizes, int max);

                                       
                                                                      
                                                                               
   
int rr_scan(uint64_t start, uint64_t size, const uint8_t* pat, const char* mask,
            uint64_t* hits, int max_hits);

                  
long rr_stat_calls(void);                                                      
long rr_stat_bytes(void);
void rr_stat_reset(void);

#ifdef __cplusplus
}
#endif

#endif               

