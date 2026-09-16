/* Exercise real draw/TGSI shader replacement with deterministic allocator reuse. */
#include "pipe/p_context.h"
#include "pipe/p_screen.h"
#include "draw/draw_context.h"
#include "draw/draw_private.h"
#include "draw/draw_vs.h"
#include "tgsi/tgsi_parse.h"
#include "tgsi/tgsi_text.h"
#include "util/u_memory.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define CHECK(x) do { if (!(x)) { fprintf(stderr,"fixture error at %d: %s\n",__LINE__,#x); abort(); } } while (0)
struct slot { void *data; bool loaned; };
static struct slot pool[2];
static unsigned loans,returns,reuses,checks,failures;
void __real_free(void *p);
void __wrap_free(void *p);
struct tgsi_token *__wrap_tgsi_dup_tokens(const struct tgsi_token *tokens);

/* Return a retired allocation at exactly the same address. The interpreter
 * and shader lifecycle code are unchanged; only token allocation is controlled.
 * Two blocks also allow deletion of an inactive shader while another is live.
 */
struct tgsi_token *__wrap_tgsi_dup_tokens(const struct tgsi_token *tokens)
{
   unsigned bytes=tgsi_num_tokens(tokens)*sizeof(*tokens);
   CHECK(bytes<=4096);
   for (unsigned i=0;i<2;i++) {
      if (pool[i].loaned) continue;
      if (!pool[i].data) { pool[i].data=malloc(4096); CHECK(pool[i].data); }
      else reuses++;
      pool[i].loaned=true;loans++;
      memcpy(pool[i].data,tokens,bytes);
      return pool[i].data;
   }
   CHECK(false);return NULL;
}

void __wrap_free(void *p)
{
   for (unsigned i=0;i<2;i++) {
      if (p && p==pool[i].data) {
         CHECK(pool[i].loaned);pool[i].loaned=false;returns++;return;
      }
   }
   __real_free(p);
}

static struct draw_vertex_shader *make_shader(struct draw_context *draw,unsigned value)
{
   char text[512];
   snprintf(text,sizeof(text),"VERT\nDCL IN[0]\nDCL OUT[0], POSITION\n"
      "DCL OUT[1].x, EDGEFLAG\nIMM[0] FLT32 {%u, 0, 0, 0}\n"
      "MOV OUT[0], IN[0]\nMOV OUT[1].x, IMM[0].xxxx\nEND\n",value);
   struct tgsi_token tokens[128];
   CHECK(tgsi_text_translate(text,tokens,128));
   struct pipe_shader_state state={.type=PIPE_SHADER_IR_TGSI,.tokens=tokens};
   struct draw_vertex_shader *vs=draw_create_vs_exec(draw,&state);CHECK(vs);
   return vs;
}

static void run_shader(struct draw_context *draw,struct draw_vertex_shader *vs,unsigned value)
{
   float input[4][1][4]={{ {-0.75f,-0.75f,0,1} },{{0.75f,-0.75f,0,1}},
      {{0.75f,0.75f,0,1}},{{-0.75f,0.75f,0,1}}};
   float output[4][2][4];memset(output,0xcc,sizeof(output));
   struct draw_buffer_info constants[PIPE_MAX_CONSTANT_BUFFERS]={0};
   draw_bind_vertex_shader(draw,vs);
   vs->prepare(vs,draw);
   vs->run_linear(vs,input[0],output[0],constants,4,sizeof(input[0]),sizeof(output[0]),NULL);
   unsigned bad=0;
   for (unsigned i=0;i<4;i++) {
      for (unsigned j=0;j<4;j++) if(output[i][0][j]!=input[i][0][j]) bad++;
      if(output[i][1][0]!=(float)value) bad++;
   }
   checks++;failures+=bad!=0;
   printf("TOKEN_LIFETIME_CHECK index=%u expected=%u edge=%g,%g,%g,%g pass=%u\n",
      checks,value,output[0][1][0],output[1][1][0],output[2][1][0],output[3][1][0],bad==0);
}

int main(void)
{
   struct pipe_screen screen={0};struct pipe_context pipe={.screen=&screen};
   draw_init_shader_caps((struct pipe_shader_caps *)&screen.shader_caps[MESA_SHADER_VERTEX]);
   struct draw_context *draw=draw_create_no_llvm(&pipe);CHECK(draw);
   struct pipe_rasterizer_state rasterizer={0};
   draw_set_rasterizer_state(draw,&rasterizer,NULL);
   for(unsigned i=0;i<8;i++) {
      struct draw_vertex_shader *vs=make_shader(draw,i&1);
      run_shader(draw,vs,i&1);
      draw_bind_vertex_shader(draw,NULL);
      draw_delete_vertex_shader(draw,vs);
   }
   struct draw_vertex_shader *inactive=make_shader(draw,0),*active=make_shader(draw,1);
   run_shader(draw,active,1);
   draw_delete_vertex_shader(draw,inactive);
   run_shader(draw,active,1);
   draw_bind_vertex_shader(draw,NULL);
   draw_delete_vertex_shader(draw,active);
   draw_destroy(draw);
   CHECK(loans==returns && reuses>=8);
   for(unsigned i=0;i<2;i++) {CHECK(!pool[i].loaned);__real_free(pool[i].data);}
   printf("TOKEN_LIFETIME_RESULT checks=%u failures=%u allocations=%u releases=%u reuses=%u\n",checks,failures,loans,returns,reuses);
   return failures?1:0;
}
