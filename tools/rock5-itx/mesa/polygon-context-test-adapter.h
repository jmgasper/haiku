/* Test adapter: only the Panfrost context fields used by the unchanged helper.
 * Gallium interfaces, resource references, NIR and draw are real Mesa code.
 * This does not model hardware, driver callbacks or their private layouts.
 */
#ifndef POLYGON_TEST_CONTEXT_H
#define POLYGON_TEST_CONTEXT_H
#include "pipe/p_context.h"
#include "pipe/p_screen.h"
#include "pipe/p_state.h"
#include "compiler/nir/nir.h"
#include "util/u_math.h"
struct panfrost_uncompiled_shader {
   nir_shader *sw_polygon_nir;
   unsigned char nir_sha1[20];
   struct pipe_stream_output_info stream_output;
   bool test_native;
   nir_shader *test_native_nir;
};
struct panfrost_context {
   struct pipe_context base;
   struct panfrost_sw_polygon *sw_polygon;
   bool sw_polygon_active;
   struct panfrost_uncompiled_shader *uncompiled[MESA_SHADER_STAGES];
   struct { unsigned num_targets; } streamout;
   struct pipe_viewport_state pipe_viewport;
   struct pipe_framebuffer_state pipe_framebuffer;
   unsigned vb_mask;
   struct pipe_vertex_buffer vertex_buffers[PIPE_MAX_ATTRIBS];
   void *rasterizer, *vertex;
   uint64_t prims_generated, draw_calls;
   bool active_queries;
   struct { struct pipe_constant_buffer cb[PIPE_MAX_CONSTANT_BUFFERS]; }
      constant_buffer[MESA_SHADER_STAGES];
};
#endif
