/* SPDX-License-Identifier: MIT
 * Execute the production geometry helper with real Mesa draw/NIR and a checked
 * memory-backed pipe adapter. This checks emitted geometry, not GPU pixels.
 */
#include "pan_context.h"
#include "pan_sw_polygon.h"
#include "compiler/nir/nir_builder.h"
#include "compiler/nir/nir_serialize.h"
#include "nir/nir_to_tgsi.h"
#include "draw/draw_context.h"
#include "util/blob.h"
#include "util/mesa-sha1.h"
#include "util/u_inlines.h"
#include "util/u_upload_mgr.h"
#include "util/u_helpers.h"
#include <math.h>
#include <stdio.h>

#define CHECK(x) do { if (!(x)) { fprintf(stderr, "%s:%d: %s\n", __FILE__, __LINE__, #x); abort(); } } while (0)
#define MAX_COLLECTED 20000
struct vertex { float pos[4], color[4], edge[4]; };
struct resource {
   struct pipe_resource base;
   unsigned maps;
   unsigned char *bytes;
};
struct layout { unsigned count; struct pipe_vertex_element elements[PIPE_MAX_ATTRIBS]; };
struct collected { enum mesa_prim mode; float pos[4], color[4]; };
struct fixture {
   struct panfrost_context ctx;
   struct pipe_screen screen;
   struct layout original_layout;
   struct pipe_rasterizer_state original_rasterizer;
   struct panfrost_uncompiled_shader vs, fs;
   struct collected collected[MAX_COLLECTED];
   unsigned collected_count, batches, resources, maps, map_attempts;
   unsigned fail_map_at, live_vs, live_elements, live_rasterizers;
};
static struct fixture *fixture(struct pipe_context *pipe) { return (void *)pipe; }
static struct fixture *screen_fixture(struct pipe_screen *s) { return container_of(s, struct fixture, screen); }

static struct pipe_resource *create_resource(struct pipe_screen *s, const struct pipe_resource *t)
{
   struct resource *r = calloc(1, sizeof(*r)); CHECK(r);
   r->base = *t; r->base.screen = s; pipe_reference_init(&r->base.reference, 1);
   r->bytes = malloc(t->width0 + 128); CHECK(r->bytes);
   memset(r->bytes, 0xa5, t->width0 + 128);
   screen_fixture(s)->resources++;
   return &r->base;
}
static void destroy_resource(struct pipe_screen *s, struct pipe_resource *p)
{
   struct resource *r = (void *)p; CHECK(!r->maps);
   for (unsigned i = 0; i < 64; ++i) {
      CHECK(r->bytes[i] == 0xa5);
      CHECK(r->bytes[64 + p->width0 + i] == 0xa5);
   }
   CHECK(screen_fixture(s)->resources); screen_fixture(s)->resources--;
   free(r->bytes); free(r);
}
static void *map_buffer(struct pipe_context *p, struct pipe_resource *res, unsigned level,
                        unsigned usage, const struct pipe_box *box, struct pipe_transfer **out)
{
   struct fixture *f = fixture(p); struct resource *r = (void *)res;
   CHECK(!level && box->x >= 0 && box->width >= 0);
   CHECK((unsigned)box->x <= res->width0 && (unsigned)box->width <= res->width0 - box->x);
   CHECK(!(usage & PIPE_MAP_READ) || !(usage & PIPE_MAP_UNSYNCHRONIZED));
   if (++f->map_attempts == f->fail_map_at) return NULL;
   struct pipe_transfer *t = calloc(1, sizeof(*t)); CHECK(t);
   pipe_resource_reference(&t->resource, res); t->box = *box; t->usage = usage;
   *out = t; r->maps++; f->maps++;
   return r->bytes + 64 + box->x;
}
static void unmap_buffer(struct pipe_context *p, struct pipe_transfer *t)
{
   struct resource *r = (void *)t->resource;
   CHECK(r->maps && fixture(p)->maps); r->maps--; fixture(p)->maps--;
   pipe_resource_reference(&t->resource, NULL); free(t);
}
static void flush_region(struct pipe_context *p, struct pipe_transfer *t, const struct pipe_box *box)
{
   (void)p; CHECK(t->usage & PIPE_MAP_WRITE);
   CHECK(box->x >= 0 && box->x + box->width <= t->box.width);
}
static void set_buffers(struct pipe_context *p, unsigned count, const struct pipe_vertex_buffer *buffers)
{
   struct panfrost_context *c = &fixture(p)->ctx;
   util_set_vertex_buffers_mask(c->vertex_buffers, &c->vb_mask, buffers, count);
}
static void *create_elements(struct pipe_context *p, unsigned count, const struct pipe_vertex_element *e)
{
   CHECK(count <= PIPE_MAX_ATTRIBS); struct layout *l = calloc(1, sizeof(*l)); CHECK(l);
   l->count = count; memcpy(l->elements, e, count * sizeof(*e)); fixture(p)->live_elements++; return l;
}
static void bind_elements(struct pipe_context *p, void *l) { fixture(p)->ctx.vertex = l; }
static void delete_elements(struct pipe_context *p, void *l)
{
   CHECK(l != fixture(p)->ctx.vertex); CHECK(fixture(p)->live_elements);
   fixture(p)->live_elements--; free(l);
}
static void *create_rasterizer(struct pipe_context *p, const struct pipe_rasterizer_state *s)
{
   void *r = malloc(sizeof(*s)); CHECK(r); memcpy(r, s, sizeof(*s)); fixture(p)->live_rasterizers++; return r;
}
static void bind_rasterizer(struct pipe_context *p, void *r) { fixture(p)->ctx.rasterizer = r; }
static void delete_rasterizer(struct pipe_context *p, void *r)
{
   CHECK(r != fixture(p)->ctx.rasterizer); CHECK(fixture(p)->live_rasterizers);
   fixture(p)->live_rasterizers--; free(r);
}
static void *create_vs(struct pipe_context *p, const struct pipe_shader_state *s)
{
   struct panfrost_uncompiled_shader *vs = calloc(1, sizeof(*vs)); CHECK(vs);
   vs->test_native = true;
   if (s->type == PIPE_SHADER_IR_NIR) vs->test_native_nir = s->ir.nir;
   fixture(p)->live_vs++; return vs;
}
static void bind_vs(struct pipe_context *p, void *vs) { fixture(p)->ctx.uncompiled[MESA_SHADER_VERTEX] = vs; }
static void delete_vs(struct pipe_context *p, void *state)
{
   struct panfrost_uncompiled_shader *vs = state;
   CHECK(vs != fixture(p)->ctx.uncompiled[MESA_SHADER_VERTEX] && vs->test_native);
   CHECK(fixture(p)->live_vs); fixture(p)->live_vs--;
   ralloc_free(vs->test_native_nir); free(vs);
}
static void collect_draw(struct pipe_context *p, const struct pipe_draw_info *info, unsigned drawid,
                         const struct pipe_draw_indirect_info *indirect,
                         const struct pipe_draw_start_count_bias *draws, unsigned count)
{
   struct fixture *f = fixture(p); struct panfrost_context *c = &f->ctx;
   struct layout *l = c->vertex; struct pipe_rasterizer_state *r = c->rasterizer;
   CHECK(c->sw_polygon_active && c->uncompiled[MESA_SHADER_VERTEX]->test_native);
   CHECK(!info->index_size && info->instance_count == 1 && !indirect && !drawid && count == 1);
   CHECK(r->fill_front == PIPE_POLYGON_MODE_FILL && r->fill_back == PIPE_POLYGON_MODE_FILL);
   CHECK(!r->cull_face && !r->light_twoside && !r->clip_plane_enable);
   CHECK(l->count >= 2 && c->vb_mask == 1);
   struct resource *buf = (void *)c->vertex_buffers[0].buffer.resource;
   CHECK(buf && !buf->maps); f->batches++;
   CHECK(f->collected_count + draws[0].count <= MAX_COLLECTED);
   for (unsigned i = 0; i < draws[0].count; ++i) {
      struct collected *out = &f->collected[f->collected_count++]; out->mode = info->mode;
      for (unsigned a = 0; a < 2; ++a) {
         unsigned offset = c->vertex_buffers[0].buffer_offset +
            l->elements[a].src_offset + (draws[0].start + i) * l->elements[a].src_stride;
         CHECK(offset + 16 <= buf->base.width0);
         memcpy(a ? out->color : out->pos, buf->bytes + 64 + offset, 16);
      }
   }
   c->prims_generated += 123456; c->draw_calls++;
}

enum { SHADER_EDGE = 1, SHADER_CLIP = 2, SHADER_UNIFORM = 4 };
static nir_shader *make_vs(unsigned flags)
{
   nir_builder b = nir_builder_init_simple_shader(MESA_SHADER_VERTEX, &nir_to_tgsi_compiler_options, "polygon test VS");
   nir_variable *ip = nir_variable_create(b.shader, nir_var_shader_in, glsl_vec4_type(), "position");
   ip->data.location = VERT_ATTRIB_GENERIC0; ip->data.driver_location = 0;
   nir_variable *ic = nir_variable_create(b.shader, nir_var_shader_in, glsl_vec4_type(), "color");
   ic->data.location = VERT_ATTRIB_GENERIC1; ic->data.driver_location = 1;
   nir_variable *op = nir_variable_create(b.shader, nir_var_shader_out, glsl_vec4_type(), "gl_Position");
   op->data.location = VARYING_SLOT_POS; op->data.driver_location = 0;
   nir_variable *oc = nir_variable_create(b.shader, nir_var_shader_out, glsl_vec4_type(), "color");
   oc->data.location = VARYING_SLOT_COL0; oc->data.driver_location = 1;
   nir_def *position = nir_load_var(&b, ip);
   if (flags & SHADER_UNIFORM) {
      nir_def *delta = nir_load_ubo(&b, 4, 32, nir_imm_int(&b, 0), nir_imm_int(&b, 0), .align_mul = 16, .range = 16);
      position = nir_fadd(&b, position, delta); b.shader->info.num_ubos = 1;
   }
   nir_store_var(&b, op, position, 15); nir_store_var(&b, oc, nir_load_var(&b, ic), 15);
   if (flags & SHADER_EDGE) {
      nir_variable *ie = nir_variable_create(b.shader, nir_var_shader_in, glsl_float_type(), "edge");
      ie->data.location = VERT_ATTRIB_GENERIC2; ie->data.driver_location = 2;
      nir_variable *oe = nir_variable_create(b.shader, nir_var_shader_out, glsl_float_type(), "gl_EdgeFlag");
      oe->data.location = VARYING_SLOT_EDGE; oe->data.driver_location = 2;
      nir_store_var(&b, oe, nir_load_var(&b, ie), 1);
   }
   if (flags & SHADER_CLIP) {
      nir_variable *cl = nir_variable_create(b.shader, nir_var_shader_out, glsl_array_type(glsl_float_type(), 1, 0), "gl_ClipDistance");
      cl->data.location = VARYING_SLOT_CLIP_DIST0; cl->data.driver_location = 2; cl->data.compact = true;
      nir_deref_instr *d = nir_build_deref_array_imm(&b, nir_build_deref_var(&b, cl), 0);
      nir_store_deref(&b, d, nir_fadd(&b, nir_channel(&b, position, 0), nir_imm_float(&b, 0.25)), 1);
      b.shader->info.clip_distance_array_size = 1;
   }
   b.shader->num_inputs = flags & SHADER_EDGE ? 3 : 2;
   b.shader->num_outputs = flags & (SHADER_EDGE | SHADER_CLIP) ? 3 : 2;
   nir_shader_gather_info(b.shader, b.impl); return b.shader;
}
static nir_shader *make_fs_variant(bool integer)
{
   nir_builder b = nir_builder_init_simple_shader(MESA_SHADER_FRAGMENT, &nir_to_tgsi_compiler_options, "polygon test FS");
   nir_variable *in = nir_variable_create(b.shader, nir_var_shader_in, integer ? glsl_ivec4_type() : glsl_vec4_type(), "color");
   in->data.location = VARYING_SLOT_COL0; in->data.driver_location = 0;
   nir_variable *out = nir_variable_create(b.shader, nir_var_shader_out, glsl_vec4_type(), "result");
   out->data.location = FRAG_RESULT_COLOR; out->data.driver_location = 0;
   nir_def *color = nir_load_var(&b, in);
   if (integer) color = nir_i2f32(&b, color);
   nir_store_var(&b, out, color, 15);
   b.shader->num_inputs = b.shader->num_outputs = 1;
   nir_shader_gather_info(b.shader, b.impl); return b.shader;
}
static nir_shader *make_fs(void) { return make_fs_variant(false); }
static void set_shader(struct panfrost_uncompiled_shader *s, nir_shader *nir)
{
   ralloc_free(s->sw_polygon_nir); s->sw_polygon_nir = nir;
   struct blob blob; blob_init(&blob); nir_serialize(&blob, nir, false);
   _mesa_sha1_compute(blob.data, blob.size, s->nir_sha1); blob_finish(&blob);
}
static void init_fixture(struct fixture *f)
{
   memset(f, 0, sizeof(*f)); struct pipe_context *p = &f->ctx.base;
   p->screen = &f->screen; f->screen.resource_create = create_resource; f->screen.resource_destroy = destroy_resource;
   draw_init_shader_caps((struct pipe_shader_caps *)&f->screen.shader_caps[MESA_SHADER_VERTEX]);
   draw_init_shader_caps((struct pipe_shader_caps *)&f->screen.shader_caps[MESA_SHADER_FRAGMENT]);
   struct pipe_caps *caps = (struct pipe_caps *)&f->screen.caps;
   caps->glsl_feature_level = 330; caps->quads_follow_provoking_vertex_convention = true;
   p->set_vertex_buffers = set_buffers; p->create_vertex_elements_state = create_elements;
   p->bind_vertex_elements_state = bind_elements; p->delete_vertex_elements_state = delete_elements;
   p->create_rasterizer_state = create_rasterizer; p->bind_rasterizer_state = bind_rasterizer; p->delete_rasterizer_state = delete_rasterizer;
   p->create_vs_state = create_vs; p->bind_vs_state = bind_vs; p->delete_vs_state = delete_vs;
   p->draw_vbo = collect_draw; p->buffer_map = map_buffer; p->buffer_unmap = unmap_buffer; p->transfer_flush_region = flush_region;
   p->resource_release = u_default_resource_release;
   p->stream_uploader = u_upload_create_default(p); CHECK(p->stream_uploader);
   set_shader(&f->vs, make_vs(0)); set_shader(&f->fs, make_fs());
   f->ctx.uncompiled[MESA_SHADER_VERTEX] = &f->vs; f->ctx.uncompiled[MESA_SHADER_FRAGMENT] = &f->fs;
   f->ctx.rasterizer = &f->original_rasterizer; f->ctx.vertex = &f->original_layout;
   f->ctx.pipe_viewport = (struct pipe_viewport_state){.scale = {32, 32, 0.5}, .translate = {32, 32, 0.5}};
   /* Gallium uses a top-left surface convention. A positive-Y FBO viewport
    * pairs with front_ccw=false for this GL counter-clockwise triangle. */
   f->original_rasterizer = (struct pipe_rasterizer_state){.fill_front = PIPE_POLYGON_MODE_LINE, .fill_back = PIPE_POLYGON_MODE_LINE, .front_ccw = false, .point_size = 1, .line_width = 1, .depth_clip_near = true, .depth_clip_far = true};
   f->original_layout.count = 3;
   for (unsigned i = 0; i < 3; ++i) f->original_layout.elements[i] = (struct pipe_vertex_element){.src_offset = i * 16, .src_stride = sizeof(struct vertex), .src_format = PIPE_FORMAT_R32G32B32A32_FLOAT};
   f->ctx.active_queries = true; f->ctx.prims_generated = 7; f->ctx.draw_calls = 11;
}
static void cleanup_fixture(struct fixture *f)
{
   CHECK(!f->maps && !f->ctx.sw_polygon_active);
   panfrost_sw_polygon_destroy(&f->ctx); u_upload_destroy(f->ctx.base.stream_uploader);
   set_buffers(&f->ctx.base, 0, NULL);
   CHECK(!f->resources && !f->live_vs && !f->live_elements && !f->live_rasterizers);
   ralloc_free(f->vs.sw_polygon_nir); ralloc_free(f->fs.sw_polygon_nir);
}
static void bind_vertices(struct fixture *f, const struct vertex *v, unsigned count)
{
   struct pipe_resource *r = pipe_buffer_create(&f->screen, PIPE_BIND_VERTEX_BUFFER, PIPE_USAGE_DEFAULT, count * sizeof(*v) + 32);
   memcpy(((struct resource *)r)->bytes + 64 + 32, v, count * sizeof(*v));
   struct pipe_vertex_buffer vb = {.buffer_offset = 32, .buffer.resource = r};
   set_buffers(&f->ctx.base, 1, &vb); pipe_resource_reference(&r, NULL);
}
static bool run_draw(struct fixture *f, const struct pipe_draw_info *info,
                     const struct pipe_draw_start_count_bias *draws, unsigned num_draws)
{
   f->collected_count = f->batches = f->map_attempts = 0;
   struct pipe_resource *old = f->ctx.vertex_buffers[0].buffer.resource;
   unsigned refs = old->reference.count; uint64_t old_calls = f->ctx.draw_calls;
   bool ok = panfrost_sw_polygon_draw(&f->ctx, &f->original_rasterizer, f->original_layout.count,
      f->original_layout.elements, info, 0, NULL, draws, num_draws);
   CHECK(!f->ctx.sw_polygon_active && !f->maps);
   CHECK(f->ctx.uncompiled[MESA_SHADER_VERTEX] == &f->vs && f->ctx.uncompiled[MESA_SHADER_FRAGMENT] == &f->fs);
   CHECK(f->ctx.rasterizer == &f->original_rasterizer && f->ctx.vertex == &f->original_layout);
   CHECK(f->ctx.vb_mask == 1 && f->ctx.vertex_buffers[0].buffer.resource == old && old->reference.count == refs);
   CHECK(f->ctx.vertex_buffers[0].buffer_offset == 32 && f->ctx.draw_calls == old_calls + 1);
   return ok;
}
static struct vertex triangle[3] = {
   {{-0.75, -0.75, 0, 1}, {1, 0, 0, 1}, {1}},
   {{ 0.75, -0.75, 0, 1}, {0, 1, 0, 1}, {1}},
   {{ 0,     0.75, 0, 1}, {0, 0, 1, 1}, {1}},
};
static void check_vertices(struct fixture *f, enum mesa_prim mode, unsigned count)
{
   fprintf(stderr, "geometry mode=%u expected=%u actual=%u batches=%u first_mode=%u\n", mode, count, f->collected_count, f->batches, f->collected_count ? f->collected[0].mode : 0);
   CHECK(f->collected_count == count);
   for (unsigned i = 0; i < count; ++i) {
      CHECK(f->collected[i].mode == mode);
      for (unsigned c = 0; c < 4; ++c) CHECK(isfinite(f->collected[i].pos[c]));
      CHECK(f->collected[i].pos[3] > 0);
   }
}
int main(void)
{
   glsl_type_singleton_init_or_ref();
   struct fixture *f = calloc(1, sizeof(*f)); CHECK(f); init_fixture(f); bind_vertices(f, triangle, 3);
   struct pipe_draw_info info = {.mode = MESA_PRIM_TRIANGLES, .instance_count = 1};
   struct pipe_draw_start_count_bias draw = {.count = 3};
   CHECK(run_draw(f, &info, &draw, 1)); check_vertices(f, MESA_PRIM_LINES, 6);
   CHECK(f->ctx.prims_generated == 8);
   for (unsigned i = 0; i < 6; ++i) {
      bool found = false;
      for (unsigned v = 0; v < 3; ++v)
         if (!memcmp(f->collected[i].pos, triangle[v].pos, 16) && !memcmp(f->collected[i].color, triangle[v].color, 16)) found = true;
      CHECK(found);
   }
   f->original_rasterizer.fill_front = f->original_rasterizer.fill_back = PIPE_POLYGON_MODE_POINT;
   CHECK(run_draw(f, &info, &draw, 1)); check_vertices(f, MESA_PRIM_POINTS, 3);
   f->original_rasterizer.fill_front = f->original_rasterizer.fill_back = PIPE_POLYGON_MODE_LINE;
   f->original_rasterizer.cull_face = PIPE_FACE_FRONT;
   CHECK(run_draw(f, &info, &draw, 1)); check_vertices(f, MESA_PRIM_LINES, 0);
   f->original_rasterizer.cull_face = PIPE_FACE_BACK;
   CHECK(run_draw(f, &info, &draw, 1)); check_vertices(f, MESA_PRIM_LINES, 6);
   f->original_rasterizer.cull_face = PIPE_FACE_NONE;

   f->ctx.pipe_viewport.scale[1] = -32; f->original_rasterizer.front_ccw = true;
   f->original_rasterizer.cull_face = PIPE_FACE_BACK;
   CHECK(run_draw(f, &info, &draw, 1)); check_vertices(f, MESA_PRIM_LINES, 6);
   f->ctx.pipe_viewport.scale[1] = 32; f->original_rasterizer.front_ccw = false;
   f->original_rasterizer.cull_face = PIPE_FACE_NONE;
   puts("POLYGON_CASE basic_modes_and_both_viewport_orientations");

   struct vertex reversed[3] = {triangle[0], triangle[2], triangle[1]};
   f->original_rasterizer.fill_back = PIPE_POLYGON_MODE_FILL;
   CHECK(run_draw(f, &info, &draw, 1)); check_vertices(f, MESA_PRIM_LINES, 6);
   bind_vertices(f, reversed, 3);
   CHECK(run_draw(f, &info, &draw, 1)); check_vertices(f, MESA_PRIM_TRIANGLES, 3);
   bind_vertices(f, triangle, 3);
   f->original_rasterizer.fill_back = PIPE_POLYGON_MODE_LINE;
   puts("POLYGON_CASE mixed_front_back_modes");

   f->original_rasterizer.flatshade = true;
   CHECK(run_draw(f, &info, &draw, 1)); check_vertices(f, MESA_PRIM_LINES, 6);
   for (unsigned i = 0; i < 6; ++i) CHECK(!memcmp(f->collected[i].color, triangle[2].color, 16));
   f->original_rasterizer.flatshade_first = true;
   CHECK(run_draw(f, &info, &draw, 1)); check_vertices(f, MESA_PRIM_LINES, 6);
   for (unsigned i = 0; i < 6; ++i) CHECK(!memcmp(f->collected[i].color, triangle[0].color, 16));
   f->original_rasterizer.flatshade = f->original_rasterizer.flatshade_first = false;
   puts("POLYGON_CASE flatshade_provoking_vertices");

   set_shader(&f->vs, make_vs(SHADER_EDGE));
   struct vertex edged[3]; memcpy(edged, triangle, sizeof(edged)); edged[0].edge[0] = 0;
   bind_vertices(f, edged, 3);
   CHECK(run_draw(f, &info, &draw, 1)); check_vertices(f, MESA_PRIM_LINES, 4);
   f->original_rasterizer.fill_front = f->original_rasterizer.fill_back = PIPE_POLYGON_MODE_POINT;
   CHECK(run_draw(f, &info, &draw, 1)); check_vertices(f, MESA_PRIM_POINTS, 2);
   f->original_rasterizer.fill_front = f->original_rasterizer.fill_back = PIPE_POLYGON_MODE_LINE;
   set_shader(&f->vs, make_vs(SHADER_CLIP)); bind_vertices(f, triangle, 3);
   f->original_rasterizer.clip_plane_enable = 1;
   CHECK(run_draw(f, &info, &draw, 1)); check_vertices(f, MESA_PRIM_LINES, 8);
   unsigned clipped = 0;
   for (unsigned i = 0; i < 8; ++i) {
      CHECK(f->collected[i].pos[0] >= -0.250001f);
      clipped += fabsf(f->collected[i].pos[0] + 0.25f) < 0.000001f;
   }
   CHECK(clipped == 4);
   f->original_rasterizer.clip_plane_enable = 0; set_shader(&f->vs, make_vs(0));
   puts("POLYGON_CASE_edge_flags_and_clipped_polygon_boundary");

   struct vertex perspective[3]; memcpy(perspective, triangle, sizeof(perspective));
   for (unsigned v = 0; v < 3; ++v) for (unsigned c = 0; c < 4; ++c) perspective[v].pos[c] *= 1u << v;
   bind_vertices(f, perspective, 3);
   CHECK(run_draw(f, &info, &draw, 1)); check_vertices(f, MESA_PRIM_LINES, 6);
   for (unsigned i = 0; i < 6; ++i) {
      bool found = false;
      for (unsigned v = 0; v < 3; ++v)
         if (!memcmp(f->collected[i].pos, perspective[v].pos, 16)) found = true;
      CHECK(found);
   }
   f->ctx.pipe_viewport.scale[2] = 0;
   CHECK(run_draw(f, &info, &draw, 1)); check_vertices(f, MESA_PRIM_LINES, 6);
   f->ctx.pipe_viewport.scale[2] = 0.5f; bind_vertices(f, triangle, 3);
   puts("POLYGON_CASE_homogeneous_w_and_zero_depth_scale");

   f->original_rasterizer.offset_line = true;
   f->original_rasterizer.offset_units = 4096;
   struct pipe_resource *depth = pipe_buffer_create(&f->screen, PIPE_BIND_DEPTH_STENCIL, PIPE_USAGE_DEFAULT, 128);
   f->ctx.pipe_framebuffer.zsbuf.texture = depth;
   f->ctx.pipe_framebuffer.zsbuf.format = PIPE_FORMAT_Z16_UNORM;
   CHECK(run_draw(f, &info, &draw, 1)); check_vertices(f, MESA_PRIM_LINES, 6);
   for (unsigned i = 0; i < 6; ++i) CHECK(fabsf(f->collected[i].pos[2] - 16384.0f / 65535.0f) < 0.000001f);
   f->ctx.pipe_framebuffer.zsbuf.texture = NULL;
   pipe_resource_reference(&depth, NULL);
   f->original_rasterizer.offset_line = false;
   f->original_rasterizer.offset_units = 0;
   f->original_rasterizer.line_width = 4;
   CHECK(run_draw(f, &info, &draw, 1)); check_vertices(f, MESA_PRIM_LINES, 6);
   f->original_rasterizer.line_width = 1;
   f->original_rasterizer.fill_front = f->original_rasterizer.fill_back = PIPE_POLYGON_MODE_POINT;
   f->original_rasterizer.point_size = 8;
   f->original_rasterizer.sprite_coord_enable = 1;
   f->original_rasterizer.point_quad_rasterization = true;
   CHECK(run_draw(f, &info, &draw, 1)); check_vertices(f, MESA_PRIM_POINTS, 3);
   f->original_rasterizer.fill_front = f->original_rasterizer.fill_back = PIPE_POLYGON_MODE_LINE;
   f->original_rasterizer.point_size = 1; f->original_rasterizer.sprite_coord_enable = 0;
   f->original_rasterizer.point_quad_rasterization = false;
   puts("POLYGON_CASE_polygon_offset_and_native_width_sprite_ownership");

   uint16_t indices[] = {99, 1, 2, 3, 0xffff, 1, 2, 3};
   struct pipe_resource *ib = pipe_buffer_create(&f->screen, PIPE_BIND_INDEX_BUFFER, PIPE_USAGE_DEFAULT, sizeof(indices));
   memcpy(((struct resource *)ib)->bytes + 64, indices, sizeof(indices));
   info.index_size = 2; info.index.resource = ib; info.primitive_restart = true; info.restart_index = 0xffff;
   draw.start = 1; draw.count = 7; draw.index_bias = -1;
   CHECK(run_draw(f, &info, &draw, 1)); check_vertices(f, MESA_PRIM_LINES, 12);
   info.has_user_indices = true; info.index.user = indices;
   CHECK(run_draw(f, &info, &draw, 1)); check_vertices(f, MESA_PRIM_LINES, 12);
   info.has_user_indices = false; info.index.resource = ib;
   puts("POLYGON_CASE_indexed_start_bias_restart_and_user_indices");

   set_shader(&f->vs, make_vs(SHADER_UNIFORM));
   struct pipe_resource *cb = pipe_buffer_create(&f->screen, PIPE_BIND_CONSTANT_BUFFER, PIPE_USAGE_DEFAULT, 64);
   float constants[16] = {0}; constants[8] = 0.125f;
   memcpy(((struct resource *)cb)->bytes + 64, constants, sizeof(constants));
   f->ctx.constant_buffer[MESA_SHADER_VERTEX].cb[0] = (struct pipe_constant_buffer){.buffer = cb, .buffer_offset = 32, .buffer_size = 16};
   CHECK(run_draw(f, &info, &draw, 1)); check_vertices(f, MESA_PRIM_LINES, 12);
   for (unsigned i = 0; i < 12; ++i) CHECK(f->collected[i].pos[0] >= -0.625f && f->collected[i].pos[0] <= 0.875f);
   f->ctx.constant_buffer[MESA_SHADER_VERTEX].cb[0] = (struct pipe_constant_buffer){.user_buffer = constants, .buffer_offset = 32, .buffer_size = 16};
   CHECK(run_draw(f, &info, &draw, 1)); check_vertices(f, MESA_PRIM_LINES, 12);
   CHECK(f->collected[0].pos[0] == 0.125f);
   f->ctx.constant_buffer[MESA_SHADER_VERTEX].cb[0] = (struct pipe_constant_buffer){.buffer = cb, .buffer_offset = 32, .buffer_size = 16};
   puts("POLYGON_CASE_uniform_resource_and_user_offsets");

   for (unsigned i = 1; i <= 4; ++i) {
      f->fail_map_at = i;
      CHECK(!run_draw(f, &info, &draw, 1)); CHECK(!f->collected_count);
      f->fail_map_at = 0;
      CHECK(run_draw(f, &info, &draw, 1)); check_vertices(f, MESA_PRIM_LINES, 12);
   }
   f->ctx.constant_buffer[MESA_SHADER_VERTEX].cb[0].buffer_offset = 60;
   CHECK(!run_draw(f, &info, &draw, 1)); CHECK(!f->collected_count);
   memset(&f->ctx.constant_buffer[MESA_SHADER_VERTEX].cb[0], 0, sizeof(struct pipe_constant_buffer));
   pipe_resource_reference(&cb, NULL); pipe_resource_reference(&ib, NULL); set_shader(&f->vs, make_vs(0));
   puts("POLYGON_CASE_vb_ib_cb_upload_map_failures_and_range_failure_recover");

   info = (struct pipe_draw_info){.mode = MESA_PRIM_TRIANGLES, .instance_count = 2};
   draw = (struct pipe_draw_start_count_bias){.count = 3};
   CHECK(run_draw(f, &info, &draw, 1)); check_vertices(f, MESA_PRIM_LINES, 12);
   info.instance_count = 1;
   struct pipe_draw_start_count_bias multi[2] = {draw, draw};
   CHECK(run_draw(f, &info, multi, 2)); check_vertices(f, MESA_PRIM_LINES, 12);
   struct vertex *large = calloc(3000, sizeof(*large)); CHECK(large);
   for (unsigned i = 0; i < 1000; ++i) memcpy(large + i * 3, triangle, sizeof(triangle));
   bind_vertices(f, large, 3000); free(large); draw.count = 3000;
   CHECK(run_draw(f, &info, &draw, 1)); check_vertices(f, MESA_PRIM_LINES, 6000); CHECK(f->batches >= 3);
   puts("POLYGON_CASE_instancing_multidraw_and_bounded_batching");

   bind_vertices(f, triangle, 3); draw.count = 3;
   set_shader(&f->fs, make_fs_variant(true));
   CHECK(!run_draw(f, &info, &draw, 1)); CHECK(!f->collected_count);
   set_shader(&f->fs, make_fs());
   CHECK(run_draw(f, &info, &draw, 1)); check_vertices(f, MESA_PRIM_LINES, 6);
   puts("POLYGON_CASE_integer_varying_development_limit_and_recovery");

   cleanup_fixture(f); free(f); glsl_type_singleton_decref();
   puts("POLYGON_HELPER_PASS geometry_resources_state_and_cleanup"); return 0;
}
