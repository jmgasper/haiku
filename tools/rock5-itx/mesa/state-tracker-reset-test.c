/* Exercise the actual Mesa state-tracker reset notification functions. */
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>

typedef unsigned GLenum;
#define GL_NO_ERROR 0
#define GL_GUILTY_CONTEXT_RESET_ARB 0x8253
#define GL_INNOCENT_CONTEXT_RESET_ARB 0x8254
#define GL_UNKNOWN_CONTEXT_RESET_ARB 0x8255
enum pipe_reset_status {
   PIPE_NO_RESET, PIPE_GUILTY_CONTEXT_RESET, PIPE_INNOCENT_CONTEXT_RESET,
   PIPE_UNKNOWN_CONTEXT_RESET
};
struct pipe_context {
   enum pipe_reset_status (*get_device_reset_status)(struct pipe_context *);
};
struct st_context;
struct gl_context { struct st_context *st; unsigned lost_dispatches; };
struct st_context {
   struct pipe_context *pipe;
   struct gl_context *ctx;
   enum pipe_reset_status reset_status;
};
static struct st_context *st_context(struct gl_context *ctx) { return ctx->st; }
static void _mesa_set_context_lost_dispatch(struct gl_context *ctx)
{ ctx->lost_dispatches++; }
#include "state-tracker-reset-functions.inc"

static enum pipe_reset_status next_status;
static unsigned queries;
static bool persistent;
static enum pipe_reset_status query(struct pipe_context *pipe)
{
   (void)pipe;
   queries++;
   enum pipe_reset_status status = next_status;
   if (!persistent) next_status = PIPE_NO_RESET;
   return status;
}

int main(void)
{
   setbuf(stdout, NULL);
   struct pipe_context pipe = {query};
   struct gl_context ctx = {0};
   struct st_context st = {.pipe = &pipe, .ctx = &ctx};
   ctx.st = &st;
   assert(st_get_graphics_reset_status(&ctx) == GL_NO_ERROR && queries == 1);

   next_status = PIPE_UNKNOWN_CONTEXT_RESET;
   GLenum first = st_get_graphics_reset_status(&ctx);
   GLenum second = st_get_graphics_reset_status(&ctx);
   GLenum third = st_get_graphics_reset_status(&ctx);
   printf("MESA_ST_RESET_OBSERVED first=%04x second=%04x third=%04x queries=%u\n",
          first, second, third, queries);
   assert(first == GL_UNKNOWN_CONTEXT_RESET_ARB && second == GL_NO_ERROR
          && third == GL_NO_ERROR && queries == 4);
   assert(ctx.lost_dispatches > 0 && st.reset_status == PIPE_NO_RESET);

   /* Pending or unrecoverable state must be queried again on every call. */
   persistent = true;
   next_status = PIPE_UNKNOWN_CONTEXT_RESET;
   unsigned before = queries;
   for (unsigned i = 0; i < 64; i++)
      assert(st_get_graphics_reset_status(&ctx) == GL_UNKNOWN_CONTEXT_RESET_ARB);
   assert(queries == before + 64);
   next_status = PIPE_NO_RESET;
   assert(st_get_graphics_reset_status(&ctx) == GL_NO_ERROR);
   persistent = false;

   /* Preserve an actual callback event until the application consumes it. */
   before = queries;
   st_device_reset_callback(&st, PIPE_INNOCENT_CONTEXT_RESET);
   assert(st_get_graphics_reset_status(&ctx) == GL_INNOCENT_CONTEXT_RESET_ARB);
   assert(queries == before && st.reset_status == PIPE_NO_RESET);
   assert(st_get_graphics_reset_status(&ctx) == GL_NO_ERROR && queries == before + 1);
   st_device_reset_callback(&st, PIPE_GUILTY_CONTEXT_RESET);
   assert(st_get_graphics_reset_status(&ctx) == GL_GUILTY_CONTEXT_RESET_ARB);
   assert(st_get_graphics_reset_status(&ctx) == GL_NO_ERROR);
   assert(ctx.lost_dispatches > 0);
   puts("MESA_HAIKU_STATE_TRACKER_RESET_TEST_PASS");
   return 0;
}
