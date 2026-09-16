/* Exercise the actual Mesa reset functions with a queue-state provider. */
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <errno.h>
#include "haiku_csf_native.h"

enum pipe_reset_status {
   PIPE_NO_RESET, PIPE_GUILTY_CONTEXT_RESET, PIPE_INNOCENT_CONTEXT_RESET,
   PIPE_UNKNOWN_CONTEXT_RESET
};
struct pipe_context { void *screen; };
struct panfrost_context {
   struct pipe_context base;
   struct {
      unsigned group_handle;
      enum pipe_reset_status reset_status;
      bool reset_reported;
   } csf;
};
struct panfrost_device { struct { void *dev, *vm; } kmod; };
static struct panfrost_device device;
static struct panfrost_device *pan_device(void *screen) { (void)screen; return &device; }
static struct panfrost_context *pan_context(struct pipe_context *ctx)
{ return (struct panfrost_context *)ctx; }
static unsigned native_state, queries, reinitializations, vm_queries;
static int query_error;
static int haiku_kmod_group_get_reset_info(void *dev, unsigned group,
                                         struct pan_haiku_reset_info *info)
{
   (void)dev;
   assert(group == 17 || group == 18);
   queries++;
   if (query_error) return -1;
   info->state = native_state;
   info->error = native_state ? -1 : 0;
   return 0;
}
#define DRM_PANTHOR_GROUP_STATE_FATAL_FAULT 1
#define DRM_PANTHOR_GROUP_STATE_TIMEDOUT 2
#define DRM_PANTHOR_GROUP_STATE_INNOCENT 4
#define PAN_KMOD_VM_USABLE 0
struct drm_panthor_group_get_state { unsigned group_handle, state; };
static int pan_csf_group_get_state(void *dev, struct drm_panthor_group_get_state *s)
{ (void)dev; s->state = DRM_PANTHOR_GROUP_STATE_FATAL_FAULT; return 0; }
static int pan_kmod_vm_query_state(void *vm) { (void)vm; vm_queries++; return 1; }
static void panfrost_context_reinit(struct panfrost_context *ctx)
{ (void)ctx; reinitializations++; }
#define mesa_loge(...) ((void)0)

#include "reset-functions.inc"

static struct panfrost_context fresh(unsigned group)
{
   struct panfrost_context ctx = {0};
   ctx.csf.group_handle = group;
   query_error = 0;
   return ctx;
}

int main(void)
{
   struct panfrost_context ctx = fresh(17);
   native_state = PAN_HAIKU_RESET_NONE;
   for (unsigned i = 0; i < 64; i++)
      assert(get_device_reset_status(&ctx.base) == PIPE_NO_RESET);
   assert(queries == 64 && !ctx.csf.reset_reported);

   /* Idle contexts must observe a reset without another submission. */
   native_state = PAN_HAIKU_RESET_QUIESCENT;
   assert(get_device_reset_status(&ctx.base) == PIPE_UNKNOWN_CONTEXT_RESET);
   assert(get_device_reset_status(&ctx.base) == PIPE_NO_RESET);
   assert(get_device_reset_status(&ctx.base) == PIPE_NO_RESET);

   ctx = fresh(17);
   native_state = PAN_HAIKU_RESET_PENDING;
   for (unsigned i = 0; i < 64; i++)
      assert(get_device_reset_status(&ctx.base) == PIPE_UNKNOWN_CONTEXT_RESET);
   native_state = PAN_HAIKU_RESET_QUIESCENT;
   assert(get_device_reset_status(&ctx.base) == PIPE_NO_RESET);

   ctx = fresh(17);
   native_state = PAN_HAIKU_RESET_UNRECOVERABLE;
   for (unsigned i = 0; i < 64; i++)
      assert(get_device_reset_status(&ctx.base) == PIPE_UNKNOWN_CONTEXT_RESET);

   ctx = fresh(17);
   native_state = PAN_HAIKU_RESET_LOCAL_ERROR;
   assert(get_device_reset_status(&ctx.base) == PIPE_UNKNOWN_CONTEXT_RESET);
   assert(get_device_reset_status(&ctx.base) == PIPE_NO_RESET);

   ctx = fresh(17);
   query_error = 1;
   assert(get_device_reset_status(&ctx.base) == PIPE_UNKNOWN_CONTEXT_RESET);
   assert(!ctx.csf.reset_reported);
   query_error = 0;
   native_state = PAN_HAIKU_RESET_NONE;
   assert(get_device_reset_status(&ctx.base) == PIPE_NO_RESET);

   /* Submission failure must neither query the faulty VM nor assert during
    * an attempted eager context recreation. The cached event is preserved.
    */
   ctx = fresh(17);
   native_state = PAN_HAIKU_RESET_PENDING;
   csf_check_ctx_state_and_reinit(&ctx);
   assert(ctx.csf.reset_status == PIPE_UNKNOWN_CONTEXT_RESET);
   native_state = PAN_HAIKU_RESET_QUIESCENT;
   assert(get_device_reset_status(&ctx.base) == PIPE_UNKNOWN_CONTEXT_RESET);
   assert(get_device_reset_status(&ctx.base) == PIPE_NO_RESET);
   assert(reinitializations == 0 && vm_queries == 0);

   /* Two contexts have independent notification consumption. */
   ctx = fresh(17);
   struct panfrost_context shared = fresh(18);
   assert(get_device_reset_status(&ctx.base) == PIPE_UNKNOWN_CONTEXT_RESET);
   assert(get_device_reset_status(&ctx.base) == PIPE_NO_RESET);
   assert(get_device_reset_status(&shared.base) == PIPE_UNKNOWN_CONTEXT_RESET);
   assert(get_device_reset_status(&shared.base) == PIPE_NO_RESET);
   puts("MESA_HAIKU_RESET_STATUS_TEST_PASS");
   return 0;
}
