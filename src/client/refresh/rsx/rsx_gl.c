/*
 * Fixed-function compatibility layer for the native PlayStation 3 renderer.
 * Quake II's GL1 frontend supplies visibility, model interpolation, lightmap
 * construction and draw ordering; this file performs the actual work on RSX.
 */
#include "header/rsx_gl.h"
#include "../../../common/header/common.h"

#include <limits.h>
#include <math.h>
#include <malloc.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <sys/systime.h>

#include "rsx_fixed_vpo.h"
#include "rsx_fixed_fpo.h"
#include "rsx_lightmap_vpo.h"
#include "rsx_lightmap_fpo.h"
#include "rsx_filter_fpo.h"

qboolean GLimp_WaitForScheduledFlipQueue(void);

#define RSXGL_DISPLAY_BUFFERS 2
#define RSXGL_MAX_TEXTURES 4096
#define RSXGL_MATRIX_DEPTH 32
#define RSXGL_IMMEDIATE_VERTICES 16384
#define RSXGL_VERTEX_ARENA_SIZE (8 * 1024 * 1024)
#define RSXGL_INDEX_ARENA_SIZE (2 * 1024 * 1024)
#define RSXGL_STREAM_ARENA_SIZE (RSXGL_VERTEX_ARENA_SIZE + RSXGL_INDEX_ARENA_SIZE)
#define RSXGL_STATIC_VERTEX_ARENA_SIZE (8 * 1024 * 1024)
#define RSXGL_STATIC_VERTEX_PAGE_VERTICES 65535
#define RSXGL_UI_VERTEX_ARENA_SIZE (8 * 1024 * 1024)
#define RSXGL_UI_VERTEX_RING_SIZE (RSXGL_UI_VERTEX_ARENA_SIZE * 2)
#define RSXGL_UI_LOCAL_RING_SIZE (RSXGL_UI_VERTEX_ARENA_SIZE * 2)
#define RSXGL_MAIN_MEMORY_ALIGNMENT (1024 * 1024)
#define RSXGL_PPU_CACHE_LINE_SIZE 128
#define RSXGL_INDEXED_BATCH_MAX_VERTICES 65535
#define RSXGL_TEXTURE_ENV_COMBINE 0x8570
#define RSXGL_MAX_RETIRED_TEXTURES 512
#define RSXGL_MAX_RETIRED_TEXTURE_BYTES (16u * 1024u * 1024u)
#define RSXGL_FRAME_PACKED_EYE_HEIGHT 720
#define RSXGL_MAX_MIP_LEVELS 13
#define RSXGL_STATS_INTERVAL 120
#define RSXGL_60FPS_BUDGET_US 16667ull
#define RSXGL_30FPS_BUDGET_US 33334ull
#define RSXGL_INLINE_TEXT_CHUNK_GLYPHS 128
#define RSXGL_INLINE_BEGIN_WORDS 2
#define RSXGL_INLINE_COLOR_WORDS 5
#define RSXGL_INLINE_END_WORDS 2
#define RSXGL_INLINE_TEXT_BASE_WORDS \
	(RSXGL_INLINE_BEGIN_WORDS + RSXGL_INLINE_COLOR_WORDS + \
	RSXGL_INLINE_END_WORDS)
/* TEX0 2f consumes three command words and POSITION 3f consumes four. Six
 * explicit triangle vertices therefore require exactly 42 words per glyph. */
#define RSXGL_INLINE_TEXT_WORDS_PER_GLYPH 42
#define RSXGL_INLINE_QUAD_WORDS \
	(RSXGL_INLINE_TEXT_BASE_WORDS + RSXGL_INLINE_TEXT_WORDS_PER_GLYPH)
#define RSXGL_COLOR_TILE_BASE 0
#define RSXGL_DEPTH_TILE_INDEX (RSXGL_COLOR_TILE_BASE + RSXGL_DISPLAY_BUFFERS)

typedef struct
{
	/* Keep each attribute and every vertex naturally eight-byte aligned.  The
	 * PPU may combine adjacent float copies into ld/std pairs, which fault when
	 * the old 36-byte layout placed st/color at four-byte-only addresses.  The
	 * fourth position component is padding; RSX still consumes three floats. */
	float xyz[4];
	float st[2];
	float color[4];
} rsxgl_vertex_t;

typedef char rsxgl_vertex_size_must_be_40[
	(sizeof(rsxgl_vertex_t) == 40) ? 1 : -1];
typedef char rsxgl_vertex_st_must_be_aligned[
	((offsetof(rsxgl_vertex_t, st) & 7) == 0) ? 1 : -1];
typedef char rsxgl_vertex_color_must_be_aligned[
	((offsetof(rsxgl_vertex_t, color) & 7) == 0) ? 1 : -1];
typedef char rsxgl_stream_size_must_be_1m_aligned[
	((RSXGL_STREAM_ARENA_SIZE & (RSXGL_MAIN_MEMORY_ALIGNMENT - 1)) == 0) ? 1 : -1];

typedef struct
{
	void *pixels;
	u32 offset;
	int width;
	int height;
	int pitch;
	size_t allocation_size;
	int mipmap_levels;
	int mipmap_capacity;
	unsigned int uploaded_mipmap_mask;
	GLint min_filter;
	GLint mag_filter;
	GLint max_anisotropy;
	GLint wrap_s;
	GLint wrap_t;
	GLboolean generate_mipmap;
	GLboolean parameters_initialized;
	GLboolean ever_applied;
	void *last_sampled_pixels;
	u32 last_sampled_frame;
} rsxgl_texture_t;

typedef struct
{
	const uint8_t *pointer;
	GLint size;
	GLenum type;
	GLsizei stride;
	GLboolean enabled;
} rsxgl_array_t;

static gcmContextData *rsxgl_context;
static gcmSurface rsxgl_surface;
static void *rsxgl_color[RSXGL_DISPLAY_BUFFERS];
static u32 rsxgl_color_offset[RSXGL_DISPLAY_BUFFERS];
static int rsxgl_color_pitch;
static void *rsxgl_readback_main_allocation;
static u32 rsxgl_readback_main_offset;
static size_t rsxgl_readback_mapping_size;
static int rsxgl_readback_main_mapped;
static void *rsxgl_depth;
static u32 rsxgl_depth_offset;
static int rsxgl_depth_pitch;
static size_t rsxgl_color_allocation_size;
static size_t rsxgl_depth_allocation_size;
static int rsxgl_tiled_targets_requested;
static int rsxgl_zcull_requested;
static int rsxgl_bound_tile_count;
static int rsxgl_zcull_bound;
static int rsxgl_output_width;
static int rsxgl_output_height;
static int rsxgl_virtual_width;
static int rsxgl_virtual_height;
static int rsxgl_target_y;
static int rsxgl_target_height;
static int rsxgl_frame_packed_output;
static int rsxgl_top_bottom_output;
static int rsxgl_stereo_eye_pass;
static int rsxgl_draw_buffer;
static int rsxgl_vsync = 1;
static int rsxgl_initialized;
static u32 rsxgl_finish_reference = 1;

/* Cache renderer cvars once at context initialization. Several of these are
 * sampled every frame or on every dynamic-lightmap update; retaining the
 * cvar objects preserves live toggles without repeating a name-table lookup
 * in those hot paths. */
static cvar_t *rsxgl_cvar_hw_cull_mode;
static cvar_t *rsxgl_cvar_stats;
static cvar_t *rsxgl_cvar_tiled_targets;
static cvar_t *rsxgl_cvar_ztrick;
static cvar_t *rsxgl_cvar_stream_main_memory;
static cvar_t *rsxgl_cvar_ui_main_memory;
static cvar_t *rsxgl_cvar_ui_vertex_ring;
static cvar_t *rsxgl_cvar_ui_local_stage;
static cvar_t *rsxgl_cvar_batch;
static cvar_t *rsxgl_cvar_indexed_batch;
static cvar_t *rsxgl_cvar_state_filter;
static cvar_t *rsxgl_cvar_fast_arrays;
static cvar_t *rsxgl_cvar_stereo_enable;
static cvar_t *rsxgl_cvar_stereo_output;
static cvar_t *rsxgl_cvar_stereo_screenshot_eye;
static cvar_t *rsxgl_cvar_texture_rename;
static cvar_t *rsxgl_cvar_texture_frame_reuse;
static cvar_t *rsxgl_cvar_combined_lightmaps;
static cvar_t *rsxgl_cvar_static_world;
static cvar_t *rsxgl_cvar_filter;
static cvar_t *rsxgl_cvar_filter_strength;
static cvar_t *rsxgl_cvar_performance_profile;
static cvar_t *rsxgl_cvar_fps_target;

static rsxVertexProgram *rsxgl_vp = (rsxVertexProgram *)rsx_fixed_vpo;
static rsxFragmentProgram *rsxgl_fp = (rsxFragmentProgram *)rsx_fixed_fpo;
static void *rsxgl_vp_ucode;
static void *rsxgl_fp_ucode;
static u32 *rsxgl_fp_buffer;
static u32 rsxgl_fp_offset;
static rsxProgramConst *rsxgl_mvp_parameter;
static rsxProgramAttrib *rsxgl_texture_parameter;
static rsxVertexProgram *rsxgl_lightmap_vp =
	(rsxVertexProgram *)rsx_lightmap_vpo;
static rsxFragmentProgram *rsxgl_lightmap_fp =
	(rsxFragmentProgram *)rsx_lightmap_fpo;
static void *rsxgl_lightmap_vp_ucode;
static void *rsxgl_lightmap_fp_ucode;
static u32 *rsxgl_lightmap_fp_buffer;
static u32 rsxgl_lightmap_fp_offset;
static rsxProgramConst *rsxgl_lightmap_mvp_parameter;
static rsxProgramConst *rsxgl_lightmap_scale_parameter;
static rsxProgramAttrib *rsxgl_lightmap_texture_parameter;
static rsxProgramAttrib *rsxgl_lightmap_parameter;
static int rsxgl_lightmap_program_active;
static rsxFragmentProgram *rsxgl_filter_fp =
	(rsxFragmentProgram *)rsx_filter_fpo;
static void *rsxgl_filter_fp_ucode;
static u32 *rsxgl_filter_fp_buffer;
static u32 rsxgl_filter_fp_offset;
static rsxProgramConst *rsxgl_filter_params0_parameter;
static rsxProgramConst *rsxgl_filter_params1_parameter;
static int rsxgl_filter_program_active;
static float rsxgl_filter_params0[4];
static float rsxgl_filter_params1[4];
static GLuint rsxgl_lightmap_base_texture;
static GLuint rsxgl_lightmap_texture;
/* Track the two combined-lighting samplers independently. Texture-sorted
 * world chains keep sampler 0 (the base image) constant while sampler 1 walks
 * lightmap pages; treating the pair as one dirty bit needlessly re-emitted the
 * complete base descriptor for every page transition. */
static int rsxgl_lightmap_base_sampler_dirty;
static int rsxgl_lightmap_page_sampler_dirty;
/* World surfaces arrive in base-texture/lightmap-page buckets. Prepare the
 * invariant program, sampler pair, stream mode, and light scale once for the
 * bucket rather than repeating that work for every BSP polygon. */
static int rsxgl_lightmap_batch_active;
static int rsxgl_lightmap_batch_streamed;
static int rsxgl_lightmap_batch_indexed;
static float rsxgl_lightmap_batch_color[4];
static float rsxgl_lightmap_scale[4] = {1, 1, 1, 1};
static int rsxgl_lightmap_scale_dirty = 1;

static rsxgl_texture_t rsxgl_textures[RSXGL_MAX_TEXTURES];
static GLuint rsxgl_bound_texture;
static void *rsxgl_white_pixels;
static u32 rsxgl_white_offset;
static void *rsxgl_retired_textures[RSXGL_MAX_RETIRED_TEXTURES];
static unsigned int rsxgl_retired_texture_count;
static size_t rsxgl_retired_texture_bytes;
static int rsxgl_frame_active;
static int rsxgl_frame_ready;
static u32 rsxgl_frame_serial;
static int rsxgl_traced_texture_rename;
static int rsxgl_traced_texture_frame_reuse;
static int rsxgl_traced_texture_rename_fallback;
static int rsxgl_traced_texture_retire_pressure;
static int rsxgl_traced_readback_dma;
static int rsxgl_traced_first_upload;
static int rsxgl_traced_first_finish;
static int rsxgl_traced_flip_timeout;
static int rsxgl_traced_first_mipmap;
static int rsxgl_traced_generated_mipmap;
static int rsxgl_traced_texture_redefine;
static int rsxgl_traced_stereo_readback;
static int rsxgl_traced_matrix_cache;
static int rsxgl_traced_sampler_cache;
static int rsxgl_traced_triangle_batch;
static int rsxgl_traced_indexed_triangle_batch;
static int rsxgl_traced_state_filter;
static int rsxgl_traced_fast_arrays;
static int rsxgl_traced_typed_particles;
static int rsxgl_traced_typed_alias;
static int rsxgl_traced_flattened_alias;
static int rsxgl_traced_vertex_cache_sync;
static int rsxgl_traced_deterministic_2d;
static int rsxgl_traced_ui_ring_wrap;
static int rsxgl_traced_ui_local_ring_wrap;
static int rsxgl_traced_ui_local_stage;
static int rsxgl_traced_inline_text;
static int rsxgl_traced_planar_text;
static int rsxgl_traced_text_geometry;
static int rsxgl_traced_combined_lightmaps;
static int rsxgl_traced_static_world;
static int rsxgl_traced_static_indices;
static int rsxgl_traced_display_filter;
static int rsxgl_vertex_cache_needs_invalidate;
static int rsxgl_traced_noop_transform;
/* Orthographic UI geometry is deliberately kept off the dynamic index stream.
 * Physical RSX has shown retained/stale fan indices in exactly the console,
 * menu, and HUD path while RPCS3 remains clean. Expanded 2D triangles cost a
 * small amount of bandwidth but give every displayed glyph a single, ordered
 * vertex stream with no second cacheable input. */
static int rsxgl_2d_mode;
/* The native renderer owns the complete 2D transform.  Keep Quake's UI in its
 * ordinary top-left, y-down coordinate system until vertex emission, then map
 * it directly to clip space.  This avoids inheriting a stale 640x480 ortho
 * constant at boot or the preceding world's projection when returning to a
 * menu.  Generic glOrtho users retain the compatibility matrix path. */
static int rsxgl_direct_2d_clip;
static float rsxgl_direct_2d_scale_x;
static float rsxgl_direct_2d_scale_y;

/* Quake's GL1 frontend emits many small polygons with unchanged fixed-function
 * state. Keep the single native shader pair resident, upload its MVP only when
 * a matrix actually changes, and rebind/invalidate texture state only after a
 * relevant GL call. This avoids several RSX methods per polygon without
 * weakening the fences around dynamic texture updates. */
static int rsxgl_programs_bound;
static int rsxgl_mvp_valid;
static u32 rsxgl_matrix_revision = 1;
static u32 rsxgl_uploaded_matrix_revision;
static int rsxgl_sampler_dirty = 1;
static int rsxgl_texture_cache_dirty = 1;

/* Mirror the fixed-function state emitted to RSX. GL1 deliberately caches a
 * few states itself, but many setup paths still repeat setters every frame.
 * A redundant setter must not split an otherwise compatible native batch. */
static int rsxgl_alpha_test_enabled;
static int rsxgl_blend_enabled;
static int rsxgl_cull_face_enabled;
static int rsxgl_hw_cull_mode;
static int rsxgl_depth_test_enabled;
static int rsxgl_stencil_test_enabled;
static int rsxgl_polygon_offset_fill_enabled;
static GLenum rsxgl_alpha_func_state;
static u32 rsxgl_alpha_ref_state;
static int rsxgl_alpha_func_valid;
static GLenum rsxgl_blend_src_state;
static GLenum rsxgl_blend_dst_state;
static int rsxgl_blend_func_valid;
static u32 rsxgl_color_mask_state;
static GLenum rsxgl_cull_face_state;
static GLenum rsxgl_depth_func_state;
static GLboolean rsxgl_depth_mask_state;
static GLfloat rsxgl_point_size_state;
static int rsxgl_point_size_valid;
static GLenum rsxgl_front_polygon_mode_state;
static GLenum rsxgl_back_polygon_mode_state;
static int rsxgl_front_polygon_mode_valid;
static int rsxgl_back_polygon_mode_valid;
static GLfloat rsxgl_polygon_offset_factor_state;
static GLfloat rsxgl_polygon_offset_units_state;
static int rsxgl_polygon_offset_valid;
static GLenum rsxgl_shade_model_state;
static int rsxgl_shade_model_valid;
static GLenum rsxgl_stencil_func_state;
static GLint rsxgl_stencil_ref_state;
static GLuint rsxgl_stencil_func_mask_state;
static int rsxgl_stencil_func_valid;
static GLuint rsxgl_stencil_write_mask_state;
static int rsxgl_stencil_mask_valid;
static GLenum rsxgl_stencil_fail_state;
static GLenum rsxgl_stencil_zfail_state;
static GLenum rsxgl_stencil_zpass_state;
static int rsxgl_stencil_op_valid;

static int
rsxgl_requested_hardware_cull_mode(void)
{
	int mode = rsxgl_cvar_hw_cull_mode ?
		(int)rsxgl_cvar_hw_cull_mode->value : 0;
	return mode >= 0 && mode <= 4 ? mode : 0;
}

static void
rsxgl_trace_hardware_cull_mode(void)
{
	switch (rsxgl_hw_cull_mode)
	{
		case 1:
			PS3_RUNTIME_TRACE("RSX renderer: hardware face culling mode 1 (direct, CCW)");
			break;
		case 2:
			PS3_RUNTIME_TRACE("RSX renderer: hardware face culling mode 2 (direct, CW)");
			break;
		case 3:
			PS3_RUNTIME_TRACE("RSX renderer: hardware face culling mode 3 (inverted, CCW)");
			break;
		case 4:
			PS3_RUNTIME_TRACE("RSX renderer: hardware face culling mode 4 (inverted, CW)");
			break;
		default:
			PS3_RUNTIME_TRACE("RSX renderer: hardware face culling suppressed for compatibility");
			break;
	}
}

static void
rsxgl_apply_hardware_cull_state(void)
{
	GLenum face = rsxgl_cull_face_state;
	int inverted = rsxgl_hw_cull_mode >= 3;
	int clockwise = rsxgl_hw_cull_mode == 2 || rsxgl_hw_cull_mode == 4;

	if (!rsxgl_context)
	{
		return;
	}
	if (inverted)
	{
		face = face == GL_FRONT ? GL_BACK : GL_FRONT;
	}
	rsxSetFrontFace(rsxgl_context,
		clockwise ? GCM_FRONTFACE_CW : GCM_FRONTFACE_CCW);
	rsxSetCullFace(rsxgl_context,
		face == GL_FRONT ? GCM_CULL_FRONT : GCM_CULL_BACK);
	rsxSetCullFaceEnable(rsxgl_context,
		rsxgl_cull_face_enabled && rsxgl_hw_cull_mode != 0 ?
		GCM_TRUE : GCM_FALSE);
}

static rsxgl_vertex_t *rsxgl_vertex_arena;
static size_t rsxgl_vertex_arena_used;
static size_t rsxgl_vertex_arena_size = RSXGL_VERTEX_ARENA_SIZE;
static u16 *rsxgl_index_arena;
static size_t rsxgl_index_arena_used;
static u32 rsxgl_vertex_arena_offset;
static u32 rsxgl_index_arena_offset;
static void *rsxgl_stream_main_allocation;
static int rsxgl_stream_main_mapped;
static u8 rsxgl_stream_location = GCM_LOCATION_RSX;
/* Keep orthographic UI writes out of the uncached RSX-local aperture.  The
 * physical console can retain old fetches when that memory is rewritten even
 * though RPCS3 is clean.  A separately mapped XDR arena gives the PPU normal
 * cacheable stores, followed by the explicit cache-line publication already
 * used by the optional main-memory stream. */
static rsxgl_vertex_t *rsxgl_world_vertex_arena;
static u32 rsxgl_world_vertex_arena_offset;
static u8 rsxgl_world_stream_location = GCM_LOCATION_RSX;
static size_t rsxgl_world_vertex_arena_used;
/* Static opaque BSP vertices are packed once during model registration. They
 * are divided into <=65535-vertex pages so the normal compact 16-bit dynamic
 * index stream can batch visible fans without rewriting vertex payloads. */
static rsxgl_vertex_t *rsxgl_static_vertex_arena;
static u32 rsxgl_static_vertex_arena_offset;
static size_t rsxgl_static_vertex_arena_used;
static size_t rsxgl_static_vertex_page_offset;
static unsigned int rsxgl_static_vertex_page_used;
static unsigned int rsxgl_static_vertex_generation = 1;
static unsigned int rsxgl_static_registered_vertices;
static int rsxgl_static_geometry_dirty;
static void *rsxgl_ui_stream_main_allocation;
static rsxgl_vertex_t *rsxgl_ui_vertex_arena;
static u32 rsxgl_ui_vertex_arena_offset;
static size_t rsxgl_ui_vertex_arena_used;
static size_t rsxgl_ui_vertex_arena_size = RSXGL_UI_VERTEX_ARENA_SIZE;
static int rsxgl_ui_stream_main_mapped;
static int rsxgl_ui_stream_selected;
/* Physical RSX can observe stale cache lines when its vertex fetcher reads a
 * cacheable XDR stream directly. The preferred path packs each complete UI
 * batch at its final address in a rotating local ring. The prior XDR-to-local
 * transfer remains mode 1 for hardware A/B tests and low-memory fallback. */
static rsxgl_vertex_t *rsxgl_ui_local_vertex_arena;
static u32 rsxgl_ui_local_vertex_arena_offset;
static size_t rsxgl_ui_local_vertex_arena_used;
static size_t rsxgl_ui_local_vertex_arena_size = RSXGL_UI_VERTEX_ARENA_SIZE;
static int rsxgl_ui_stream_direct_local;
static rsxgl_vertex_t *rsxgl_triangle_batch;
static u16 *rsxgl_triangle_batch_indices;
static int rsxgl_triangle_batch_vertices;
static int rsxgl_triangle_batch_index_count;
static int rsxgl_triangle_batch_draws;
static u16 *rsxgl_static_batch_indices;
static u32 rsxgl_static_batch_vertex_offset;
static int rsxgl_static_batch_index_count;
static int rsxgl_static_batch_draws;
static int rsxgl_triangle_batch_enabled = 1;
static int rsxgl_indexed_batch_enabled;
static int rsxgl_state_filter_enabled = 1;
static int rsxgl_fast_arrays_enabled = 1;
static int rsxgl_stats_enabled;
static unsigned int rsxgl_frame_api_draws;
static unsigned int rsxgl_frame_gpu_draws;
static unsigned int rsxgl_frame_batched_draws;
static unsigned int rsxgl_frame_batch_flushes;
static unsigned int rsxgl_frame_vertices;
static unsigned int rsxgl_frame_indices;
static unsigned int rsxgl_frame_state_skips;
static unsigned int rsxgl_frame_raster_state_hits;
static unsigned int rsxgl_frame_fast_array_vertices;
static unsigned int rsxgl_frame_ui_strings;
static unsigned int rsxgl_frame_ui_glyphs;
static unsigned int rsxgl_frame_ui_duplicate_strings;
static unsigned int rsxgl_frame_ui_state_hits;
static unsigned int rsxgl_frame_ui_batch_starts;
static unsigned int rsxgl_frame_ui_batch_merges;
static unsigned int rsxgl_frame_ortho_hits;
static unsigned int rsxgl_frame_texture_updates;
static unsigned int rsxgl_frame_texture_inplace;
static unsigned int rsxgl_frame_texture_renames;
static unsigned int rsxgl_frame_texture_rename_kib;
static unsigned int rsxgl_frame_combined_lightmap_draws;
static unsigned int rsxgl_frame_combined_lightmap_vertices;
static unsigned int rsxgl_frame_static_lightmap_vertices;
static unsigned int rsxgl_frame_stereo_lightmap_reuse_pixels;
static unsigned int rsxgl_frame_lightmap_base_sampler_sets;
static unsigned int rsxgl_frame_lightmap_page_sampler_sets;
static unsigned int rsxgl_frame_final_light_cache_hits;
static unsigned int rsxgl_frame_world_clip_tests;
static unsigned int rsxgl_frame_world_clip_skips;
static unsigned int rsxgl_frame_alias_sources;
static unsigned int rsxgl_frame_alias_refs;
static unsigned int rsxgl_frame_finishes;
static unsigned long long rsxgl_frame_wait_us;
static unsigned long long rsxgl_frame_render_us;
static unsigned long long rsxgl_frame_render_start_us;
static unsigned long long rsxgl_frame_cadence_us;
static unsigned int rsxgl_stats_frames;
static unsigned int rsxgl_stats_api_draws;
static unsigned int rsxgl_stats_gpu_draws;
static unsigned int rsxgl_stats_batched_draws;
static unsigned int rsxgl_stats_batch_flushes;
static unsigned int rsxgl_stats_vertices;
static unsigned int rsxgl_stats_indices;
static unsigned int rsxgl_stats_state_skips;
static unsigned int rsxgl_stats_raster_state_hits;
static unsigned int rsxgl_stats_fast_array_vertices;
static unsigned int rsxgl_stats_ui_strings;
static unsigned int rsxgl_stats_ui_glyphs;
static unsigned int rsxgl_stats_ui_duplicate_strings;
static unsigned int rsxgl_stats_ui_state_hits;
static unsigned int rsxgl_stats_ui_batch_starts;
static unsigned int rsxgl_stats_ui_batch_merges;
static unsigned int rsxgl_stats_ortho_hits;
static unsigned int rsxgl_stats_texture_updates;
static unsigned int rsxgl_stats_texture_inplace;
static unsigned int rsxgl_stats_texture_renames;
static unsigned int rsxgl_stats_texture_rename_kib;
static unsigned int rsxgl_stats_combined_lightmap_draws;
static unsigned int rsxgl_stats_combined_lightmap_vertices;
static unsigned int rsxgl_stats_static_lightmap_vertices;
static unsigned int rsxgl_stats_stereo_lightmap_reuse_pixels;
static unsigned int rsxgl_stats_lightmap_base_sampler_sets;
static unsigned int rsxgl_stats_lightmap_page_sampler_sets;
static unsigned int rsxgl_stats_final_light_cache_hits;
static unsigned int rsxgl_stats_world_clip_tests;
static unsigned int rsxgl_stats_world_clip_skips;
static unsigned int rsxgl_stats_alias_sources;
static unsigned int rsxgl_stats_alias_refs;
static unsigned int rsxgl_stats_finishes;
static unsigned long long rsxgl_stats_wait_us;
static unsigned long long rsxgl_stats_wait_max_us;
static unsigned long long rsxgl_stats_render_us;
static unsigned long long rsxgl_stats_render_max_us;
static unsigned long long rsxgl_stats_cadence_us;
static unsigned long long rsxgl_stats_cadence_max_us;
static unsigned long long rsxgl_stats_previous_begin_us;
static unsigned int rsxgl_stats_cadence_samples;
static unsigned long long rsxgl_frame_render_stages_us[
	RSXGL_RENDER_STAGE_COUNT];
static unsigned long long rsxgl_stats_render_stages_us[
	RSXGL_RENDER_STAGE_COUNT];
static unsigned long long rsxgl_stats_render_stage_max_us[
	RSXGL_RENDER_STAGE_COUNT];
static unsigned int rsxgl_stats_scene_frames;
static unsigned long long rsxgl_stats_scene_us;
static unsigned long long rsxgl_stats_scene_max_us;
static unsigned long long rsxgl_stats_other_us;
static unsigned long long rsxgl_stats_other_max_us;
static unsigned long long rsxgl_stats_total_max_us;
static unsigned int rsxgl_stats_over_20ms;
static unsigned int rsxgl_stats_over_34ms;
static unsigned int rsxgl_stats_cadence_over_20ms;
static unsigned int rsxgl_stats_cadence_over_34ms;
static unsigned int rsxgl_stats_cadence_over_16667us;
static unsigned int rsxgl_stats_cadence_over_33334us;
static unsigned int rsxgl_floor_miss_streak;
static unsigned int rsxgl_stats_floor_miss_streak_max;
static rsxgl_vertex_t rsxgl_immediate[RSXGL_IMMEDIATE_VERTICES];
static int rsxgl_immediate_count;
static GLenum rsxgl_immediate_mode;
static int rsxgl_inside_begin;

static void rsxgl_submit_vertices(GLenum mode,
	rsxgl_vertex_t *gpu_vertices, int count);
static void rsxgl_flush_static_triangle_batch(void);
static void rsxgl_flush_triangle_batch(void);
static void rsxgl_flush_addressable_triangle_batch(void);
static void rsxgl_end_inline_2d_batch(void);

/* librsx exposes this command-buffer reservation helper to its safe command
 * entry points but does not publish the declaration in commands.h. Reserving
 * a complete inline-text chunk once lets the lower-overhead Unsafe emitters
 * run without ever crossing a command-buffer boundary inside Begin/End. */
extern s32 rsxContextCallback(gcmContextData *context, u32 count);

static rsxgl_array_t rsxgl_vertex_array;
static rsxgl_array_t rsxgl_texcoord_array;
static rsxgl_array_t rsxgl_color_array;
static float rsxgl_current_color[4] = {1, 1, 1, 1};
static float rsxgl_current_texcoord[2] = {0, 0};

/* Adjacent console rows, menu labels, and their ordinary 2D layers are one
 * triangle stream while shader, matrix, sampler, and fixed state remain
 * unchanged. Every existing state/buffer boundary closes this stream before
 * emitting another RSX method. Two words are kept reserved for its STOP
 * command, so command-buffer rollover can never strand an open primitive. */
static int rsxgl_inline_2d_batch_active;
static unsigned int rsxgl_inline_2d_batch_vertices;
static float rsxgl_inline_2d_batch_color[4];
static int rsxgl_inline_2d_batch_color_valid;

static float rsxgl_modelview[RSXGL_MATRIX_DEPTH][16];
static float rsxgl_projection[RSXGL_MATRIX_DEPTH][16];
static int rsxgl_modelview_top;
static int rsxgl_projection_top;
static GLenum rsxgl_matrix_mode = GL_MODELVIEW;
static int rsxgl_texture_enabled;
static GLint rsxgl_texture_env = GL_MODULATE;
static float rsxgl_depth_near;
static float rsxgl_depth_far = 1.0f;
static GLint rsxgl_viewport_x;
static GLint rsxgl_viewport_y;
static GLsizei rsxgl_viewport_width;
static GLsizei rsxgl_viewport_height;
static GLint rsxgl_scissor_x;
static GLint rsxgl_scissor_y;
static GLsizei rsxgl_scissor_width;
static GLsizei rsxgl_scissor_height;
static int rsxgl_scissor_enabled;
/* GL1 stores logical raster rectangles, while frame-packed output resolves
 * those rectangles to a different physical Y range for each eye. Cache the
 * resolved values as well: repeated logical calls and the acquired-surface
 * full scissor can otherwise emit identical RSX methods even though no raster
 * state changed. */
static u16 rsxgl_physical_viewport_x;
static u16 rsxgl_physical_viewport_y;
static u16 rsxgl_physical_viewport_width;
static u16 rsxgl_physical_viewport_height;
static float rsxgl_physical_viewport_near;
static float rsxgl_physical_viewport_far;
static int rsxgl_physical_viewport_valid;
static u16 rsxgl_physical_scissor_x;
static u16 rsxgl_physical_scissor_y;
static u16 rsxgl_physical_scissor_width;
static u16 rsxgl_physical_scissor_height;
static int rsxgl_physical_scissor_valid;
static float rsxgl_rgb_scale = 1.0f;
static u32 rsxgl_clear_color;
static u32 rsxgl_clear_stencil;
static int rsxgl_traced_first_draw;
static int rsxgl_traced_first_frame;
static int rsxgl_traced_first_clear;
static int rsxgl_traced_stereo_left;
static int rsxgl_traced_stereo_right;
static int rsxgl_traced_stereo_clear;
static int rsxgl_traced_acquired_surface_clear;
static int rsxgl_traced_out_of_frame_2d;
static int rsxgl_traced_inline_text_word_mismatch;
static int rsxgl_traced_inline_quad_word_mismatch;
static int rsxgl_traced_inline_quad;
static int rsxgl_traced_inline_2d_state_cache;
static int rsxgl_traced_inline_2d_batch;
static int rsxgl_traced_2d_projection_cache;
static int rsxgl_traced_ui_duplicate_string;
static int rsxgl_traced_physical_raster_cache;

#define RSXGL_UI_SIGNATURE_BUCKETS 512
static uint64_t rsxgl_ui_string_signatures[RSXGL_UI_SIGNATURE_BUCKETS];
static unsigned char rsxgl_ui_string_signature_valid[RSXGL_UI_SIGNATURE_BUCKETS];

static void
rsxgl_identity(float m[16])
{
	memset(m, 0, sizeof(float) * 16);
	m[0] = m[5] = m[10] = m[15] = 1.0f;
}

static void
rsxgl_build_ortho(float matrix[16], GLdouble left, GLdouble right,
	GLdouble bottom, GLdouble top, GLdouble near_value, GLdouble far_value)
{
	rsxgl_identity(matrix);
	matrix[0] = 2.0 / (right - left);
	matrix[5] = 2.0 / (top - bottom);
	matrix[10] = -2.0 / (far_value - near_value);
	matrix[12] = -(right + left) / (right - left);
	matrix[13] = -(top + bottom) / (top - bottom);
	matrix[14] = -(far_value + near_value) / (far_value - near_value);
	/* Identity multiplication canonicalizes -0.0f to +0.0f. Preserve that exact
	 * legacy bit pattern so the direct 2D matrix is also memcmp-stable. */
	if (matrix[14] == 0.0f)
	{
		matrix[14] = 0.0f;
	}
}

static void
rsxgl_multiply(float result[16], const float a[16], const float b[16])
{
	float temporary[16];
	int column;
	int row;
	int k;

	for (column = 0; column < 4; column++)
	{
		for (row = 0; row < 4; row++)
		{
			float value = 0.0f;
			for (k = 0; k < 4; k++)
			{
				value += a[k * 4 + row] * b[column * 4 + k];
			}
			temporary[column * 4 + row] = value;
		}
	}

	memcpy(result, temporary, sizeof(temporary));
}

static float *
rsxgl_current_matrix(void)
{
	return (rsxgl_matrix_mode == GL_PROJECTION) ?
		rsxgl_projection[rsxgl_projection_top] :
		rsxgl_modelview[rsxgl_modelview_top];
}

static void
rsxgl_touch_matrix(void)
{
	/* Zero is reserved for "no matrix has been uploaded". */
	rsxgl_matrix_revision++;
	if (!rsxgl_matrix_revision) rsxgl_matrix_revision = 1;
}

static void
rsxgl_reset_stats_accumulator(void)
{
	int i;

	rsxgl_stats_frames = 0;
	rsxgl_stats_api_draws = 0;
	rsxgl_stats_gpu_draws = 0;
	rsxgl_stats_batched_draws = 0;
	rsxgl_stats_batch_flushes = 0;
	rsxgl_stats_vertices = 0;
	rsxgl_stats_indices = 0;
	rsxgl_stats_state_skips = 0;
	rsxgl_stats_raster_state_hits = 0;
	rsxgl_stats_fast_array_vertices = 0;
	rsxgl_stats_ui_strings = 0;
	rsxgl_stats_ui_glyphs = 0;
	rsxgl_stats_ui_duplicate_strings = 0;
	rsxgl_stats_ui_state_hits = 0;
	rsxgl_stats_ui_batch_starts = 0;
	rsxgl_stats_ui_batch_merges = 0;
	rsxgl_stats_ortho_hits = 0;
	rsxgl_stats_texture_updates = 0;
	rsxgl_stats_texture_inplace = 0;
	rsxgl_stats_texture_renames = 0;
	rsxgl_stats_texture_rename_kib = 0;
	rsxgl_stats_combined_lightmap_draws = 0;
	rsxgl_stats_combined_lightmap_vertices = 0;
	rsxgl_stats_static_lightmap_vertices = 0;
	rsxgl_stats_stereo_lightmap_reuse_pixels = 0;
	rsxgl_stats_lightmap_base_sampler_sets = 0;
	rsxgl_stats_lightmap_page_sampler_sets = 0;
	rsxgl_stats_final_light_cache_hits = 0;
	rsxgl_stats_world_clip_tests = 0;
	rsxgl_stats_world_clip_skips = 0;
	rsxgl_stats_alias_sources = 0;
	rsxgl_stats_alias_refs = 0;
	rsxgl_stats_finishes = 0;
	rsxgl_stats_wait_us = 0;
	rsxgl_stats_wait_max_us = 0;
	rsxgl_stats_render_us = 0;
	rsxgl_stats_render_max_us = 0;
	rsxgl_stats_cadence_us = 0;
	rsxgl_stats_cadence_max_us = 0;
	rsxgl_stats_cadence_samples = 0;
	for (i = 0; i < RSXGL_RENDER_STAGE_COUNT; i++)
	{
		rsxgl_stats_render_stages_us[i] = 0;
		rsxgl_stats_render_stage_max_us[i] = 0;
	}
	rsxgl_stats_scene_frames = 0;
	rsxgl_stats_scene_us = 0;
	rsxgl_stats_scene_max_us = 0;
	rsxgl_stats_other_us = 0;
	rsxgl_stats_other_max_us = 0;
	rsxgl_stats_total_max_us = 0;
	rsxgl_stats_over_20ms = 0;
	rsxgl_stats_over_34ms = 0;
	rsxgl_stats_cadence_over_20ms = 0;
	rsxgl_stats_cadence_over_34ms = 0;
	rsxgl_stats_cadence_over_16667us = 0;
	rsxgl_stats_cadence_over_33334us = 0;
	/* Preserve a miss run which crosses the 120-frame report boundary. */
	rsxgl_stats_floor_miss_streak_max = rsxgl_floor_miss_streak;
}

static void
rsxgl_begin_frame_stats(void)
{
	int i;
	int enabled = rsxgl_cvar_stats && rsxgl_cvar_stats->value != 0.0f;

	if (enabled != rsxgl_stats_enabled)
	{
		rsxgl_stats_enabled = enabled;
		rsxgl_floor_miss_streak = 0;
		rsxgl_reset_stats_accumulator();
		rsxgl_stats_previous_begin_us = 0;
	}
	rsxgl_frame_api_draws = 0;
	rsxgl_frame_gpu_draws = 0;
	rsxgl_frame_batched_draws = 0;
	rsxgl_frame_batch_flushes = 0;
	rsxgl_frame_vertices = 0;
	rsxgl_frame_indices = 0;
	rsxgl_frame_state_skips = 0;
	rsxgl_frame_raster_state_hits = 0;
	rsxgl_frame_fast_array_vertices = 0;
	rsxgl_frame_ui_strings = 0;
	rsxgl_frame_ui_glyphs = 0;
	rsxgl_frame_ui_duplicate_strings = 0;
	rsxgl_frame_ui_state_hits = 0;
	rsxgl_frame_ui_batch_starts = 0;
	rsxgl_frame_ui_batch_merges = 0;
	rsxgl_frame_ortho_hits = 0;
	rsxgl_frame_texture_updates = 0;
	rsxgl_frame_texture_inplace = 0;
	rsxgl_frame_texture_renames = 0;
	rsxgl_frame_texture_rename_kib = 0;
	rsxgl_frame_combined_lightmap_draws = 0;
	rsxgl_frame_combined_lightmap_vertices = 0;
	rsxgl_frame_static_lightmap_vertices = 0;
	rsxgl_frame_stereo_lightmap_reuse_pixels = 0;
	rsxgl_frame_lightmap_base_sampler_sets = 0;
	rsxgl_frame_lightmap_page_sampler_sets = 0;
	rsxgl_frame_final_light_cache_hits = 0;
	rsxgl_frame_world_clip_tests = 0;
	rsxgl_frame_world_clip_skips = 0;
	rsxgl_frame_alias_sources = 0;
	rsxgl_frame_alias_refs = 0;
	rsxgl_frame_finishes = 0;
	rsxgl_frame_wait_us = 0;
	rsxgl_frame_render_us = 0;
	rsxgl_frame_render_start_us = 0;
	rsxgl_frame_cadence_us = 0;
	for (i = 0; i < RSXGL_RENDER_STAGE_COUNT; i++)
	{
		rsxgl_frame_render_stages_us[i] = 0;
	}
}

int
RSXGL_StatsEnabled(void)
{
	return rsxgl_stats_enabled;
}

void
RSXGL_RecordRenderStage(enum rsxgl_render_stage stage,
	unsigned long long elapsed_us)
{
	if (!rsxgl_stats_enabled || stage < 0 ||
		stage >= RSXGL_RENDER_STAGE_COUNT)
	{
		return;
	}
	rsxgl_frame_render_stages_us[stage] += elapsed_us;
}

void
RSXGL_RecordStereoLightmapReuse(unsigned int pixels)
{
	if (rsxgl_stats_enabled && rsxgl_frame_ready)
	{
		rsxgl_frame_stereo_lightmap_reuse_pixels += pixels;
	}
}

void
RSXGL_RecordFinalLightCacheHit(void)
{
	if (rsxgl_stats_enabled && rsxgl_frame_ready)
	{
		rsxgl_frame_final_light_cache_hits++;
	}
}

void
RSXGL_RecordWorldClip(unsigned int plane_tests,
	unsigned int inherited_plane_skips)
{
	if (rsxgl_stats_enabled && rsxgl_frame_ready)
	{
		rsxgl_frame_world_clip_tests += plane_tests;
		rsxgl_frame_world_clip_skips += inherited_plane_skips;
	}
}

static void
rsxgl_note_state_skip(void)
{
	if (!rsxgl_state_filter_enabled || !rsxgl_frame_ready)
	{
		return;
	}
	rsxgl_frame_state_skips++;
	if (!rsxgl_traced_state_filter)
	{
		PS3_RUNTIME_TRACE("RSX renderer: redundant fixed-state filtering active");
		rsxgl_traced_state_filter = 1;
	}
}

static void
rsxgl_note_physical_raster_hit(void)
{
	if (!rsxgl_state_filter_enabled)
	{
		return;
	}
	rsxgl_note_state_skip();
	if (rsxgl_frame_ready)
	{
		rsxgl_frame_raster_state_hits++;
	}
	if (!rsxgl_traced_physical_raster_cache)
	{
		PS3_RUNTIME_TRACE(
			"RSX renderer: resolved physical raster-state filtering active");
		rsxgl_traced_physical_raster_cache = 1;
	}
}

static void
rsxgl_note_api_draw(int vertices, int indices, int batched)
{
	if (!rsxgl_frame_ready)
	{
		return;
	}
	rsxgl_frame_api_draws++;
	rsxgl_frame_vertices += vertices > 0 ? (unsigned int)vertices : 0;
	rsxgl_frame_indices += indices > 0 ? (unsigned int)indices : 0;
	if (batched)
	{
		rsxgl_frame_batched_draws++;
	}
}

static void
rsxgl_end_frame_stats(void)
{
	char line[1536];
	char stage_line[512];
	unsigned long long total_us;
	unsigned long long scene_us = 0;
	unsigned long long other_us;
	unsigned int stage_average_us[RSXGL_RENDER_STAGE_COUNT];
	unsigned int stage_maximum_us[RSXGL_RENDER_STAGE_COUNT];
	unsigned int scene_average_us;
	unsigned int scene_maximum_us;
	unsigned int other_average_us;
	unsigned int other_maximum_us;
	int i;
	unsigned int wait_average_x100;
	unsigned int wait_maximum_x100;
	unsigned int render_average_x100;
	unsigned int render_maximum_x100;
	unsigned int cadence_average_x100;
	unsigned int cadence_maximum_x100;
	unsigned int total_maximum_x100;
	unsigned int target_fps = 30;
	unsigned int target_budget_us = (unsigned int)RSXGL_30FPS_BUDGET_US;
	unsigned int target_misses = 0;

	if (!rsxgl_stats_enabled)
	{
		return;
	}
	rsxgl_stats_frames++;
	rsxgl_stats_api_draws += rsxgl_frame_api_draws;
	rsxgl_stats_gpu_draws += rsxgl_frame_gpu_draws;
	rsxgl_stats_batched_draws += rsxgl_frame_batched_draws;
	rsxgl_stats_batch_flushes += rsxgl_frame_batch_flushes;
	rsxgl_stats_vertices += rsxgl_frame_vertices;
	rsxgl_stats_indices += rsxgl_frame_indices;
	rsxgl_stats_state_skips += rsxgl_frame_state_skips;
	rsxgl_stats_raster_state_hits += rsxgl_frame_raster_state_hits;
	rsxgl_stats_fast_array_vertices += rsxgl_frame_fast_array_vertices;
	rsxgl_stats_ui_strings += rsxgl_frame_ui_strings;
	rsxgl_stats_ui_glyphs += rsxgl_frame_ui_glyphs;
	rsxgl_stats_ui_duplicate_strings += rsxgl_frame_ui_duplicate_strings;
	rsxgl_stats_ui_state_hits += rsxgl_frame_ui_state_hits;
	rsxgl_stats_ui_batch_starts += rsxgl_frame_ui_batch_starts;
	rsxgl_stats_ui_batch_merges += rsxgl_frame_ui_batch_merges;
	rsxgl_stats_ortho_hits += rsxgl_frame_ortho_hits;
	rsxgl_stats_texture_updates += rsxgl_frame_texture_updates;
	rsxgl_stats_texture_inplace += rsxgl_frame_texture_inplace;
	rsxgl_stats_texture_renames += rsxgl_frame_texture_renames;
	rsxgl_stats_texture_rename_kib += rsxgl_frame_texture_rename_kib;
	rsxgl_stats_combined_lightmap_draws +=
		rsxgl_frame_combined_lightmap_draws;
	rsxgl_stats_combined_lightmap_vertices +=
		rsxgl_frame_combined_lightmap_vertices;
	rsxgl_stats_static_lightmap_vertices +=
		rsxgl_frame_static_lightmap_vertices;
	rsxgl_stats_stereo_lightmap_reuse_pixels +=
		rsxgl_frame_stereo_lightmap_reuse_pixels;
	rsxgl_stats_lightmap_base_sampler_sets +=
		rsxgl_frame_lightmap_base_sampler_sets;
	rsxgl_stats_lightmap_page_sampler_sets +=
		rsxgl_frame_lightmap_page_sampler_sets;
	rsxgl_stats_final_light_cache_hits +=
		rsxgl_frame_final_light_cache_hits;
	rsxgl_stats_world_clip_tests += rsxgl_frame_world_clip_tests;
	rsxgl_stats_world_clip_skips += rsxgl_frame_world_clip_skips;
	rsxgl_stats_alias_sources += rsxgl_frame_alias_sources;
	rsxgl_stats_alias_refs += rsxgl_frame_alias_refs;
	rsxgl_stats_finishes += rsxgl_frame_finishes;
	rsxgl_stats_wait_us += rsxgl_frame_wait_us;
	rsxgl_stats_render_us += rsxgl_frame_render_us;
	for (i = 0; i < RSXGL_RENDER_STAGE_COUNT; i++)
	{
		unsigned long long stage_us = rsxgl_frame_render_stages_us[i];
		scene_us += stage_us;
		rsxgl_stats_render_stages_us[i] += stage_us;
		if (stage_us > rsxgl_stats_render_stage_max_us[i])
		{
			rsxgl_stats_render_stage_max_us[i] = stage_us;
		}
	}
	if (scene_us)
	{
		rsxgl_stats_scene_frames++;
		rsxgl_stats_scene_us += scene_us;
		if (scene_us > rsxgl_stats_scene_max_us)
		{
			rsxgl_stats_scene_max_us = scene_us;
		}
		other_us = rsxgl_frame_render_us > scene_us ?
			rsxgl_frame_render_us - scene_us : 0;
		rsxgl_stats_other_us += other_us;
		if (other_us > rsxgl_stats_other_max_us)
		{
			rsxgl_stats_other_max_us = other_us;
		}
	}
	if (rsxgl_frame_cadence_us)
	{
		rsxgl_stats_cadence_us += rsxgl_frame_cadence_us;
		rsxgl_stats_cadence_samples++;
		if (rsxgl_frame_cadence_us > rsxgl_stats_cadence_max_us)
			rsxgl_stats_cadence_max_us = rsxgl_frame_cadence_us;
		if (rsxgl_frame_cadence_us > 20000ull)
			rsxgl_stats_cadence_over_20ms++;
		if (rsxgl_frame_cadence_us > 34000ull)
			rsxgl_stats_cadence_over_34ms++;
		if (rsxgl_frame_cadence_us > RSXGL_60FPS_BUDGET_US)
			rsxgl_stats_cadence_over_16667us++;
		if (rsxgl_frame_cadence_us > RSXGL_30FPS_BUDGET_US)
		{
			rsxgl_stats_cadence_over_33334us++;
			rsxgl_floor_miss_streak++;
			if (rsxgl_floor_miss_streak >
				rsxgl_stats_floor_miss_streak_max)
			{
				rsxgl_stats_floor_miss_streak_max =
					rsxgl_floor_miss_streak;
			}
		}
		else
		{
			rsxgl_floor_miss_streak = 0;
		}
	}
	if (rsxgl_frame_wait_us > rsxgl_stats_wait_max_us)
		rsxgl_stats_wait_max_us = rsxgl_frame_wait_us;
	if (rsxgl_frame_render_us > rsxgl_stats_render_max_us)
		rsxgl_stats_render_max_us = rsxgl_frame_render_us;
	total_us = rsxgl_frame_wait_us + rsxgl_frame_render_us;
	if (total_us > rsxgl_stats_total_max_us)
		rsxgl_stats_total_max_us = total_us;
	if (total_us > 20000ull) rsxgl_stats_over_20ms++;
	if (total_us > 34000ull) rsxgl_stats_over_34ms++;
	if (rsxgl_stats_frames < RSXGL_STATS_INTERVAL)
	{
		return;
	}

	/* One hundredth of a millisecond is ten microseconds. Keep the trace fields
	 * integer-only so the PS3 libc does not need to format floating point. */
	wait_average_x100 = (unsigned int)(rsxgl_stats_wait_us /
		(rsxgl_stats_frames * 10ull));
	wait_maximum_x100 = (unsigned int)(rsxgl_stats_wait_max_us / 10ull);
	render_average_x100 = (unsigned int)(rsxgl_stats_render_us /
		(rsxgl_stats_frames * 10ull));
	render_maximum_x100 = (unsigned int)(rsxgl_stats_render_max_us / 10ull);
	cadence_average_x100 = rsxgl_stats_cadence_samples ?
		(unsigned int)(rsxgl_stats_cadence_us /
		(rsxgl_stats_cadence_samples * 10ull)) : 0;
	cadence_maximum_x100 = (unsigned int)(rsxgl_stats_cadence_max_us / 10ull);
	total_maximum_x100 = (unsigned int)(rsxgl_stats_total_max_us / 10ull);
	/* Thirty FPS is the acceptance floor for every resolution, stereo mode,
	 * filter, framebuffer layout, and quality combination. The exact integer
	 * microsecond gates admit the rounded 16,667/33,334 us cadence themselves
	 * and count only slower samples. Keep the older 20/34 ms fields beside them
	 * for continuity with archived physical logs. */
	target_misses = rsxgl_stats_cadence_over_33334us;
	if ((rsxgl_cvar_fps_target &&
		(int)rsxgl_cvar_fps_target->value == 2) ||
		(rsxgl_cvar_fps_target &&
		(int)rsxgl_cvar_fps_target->value == 0 &&
		rsxgl_cvar_performance_profile &&
		(int)rsxgl_cvar_performance_profile->value == 1))
	{
		target_fps = 60;
		target_budget_us = (unsigned int)RSXGL_60FPS_BUDGET_US;
		target_misses = rsxgl_stats_cadence_over_16667us;
	}
	Com_sprintf(line, sizeof(line),
		"RSX stats: frames=%u api=%u gpu=%u batched=%u flushes=%u vertices=%u indices=%u state_skips=%u raster_hits=%u fast_vertices=%u ui_strings=%u ui_glyphs=%u ui_dupes=%u ui_state_hits=%u ui_batches=%u ui_merges=%u ortho_hits=%u updates=%u inplace=%u renames=%u rename_kib=%u lm_draws=%u lm_vertices=%u static_lm_vertices=%u stereo_lm_reuse_pixels=%u lm_base_sets=%u lm_page_sets=%u final_light_hits=%u world_clip_tests=%u world_clip_skips=%u finishes=%u wait_avg=%u.%02u wait_max=%u.%02u render_avg=%u.%02u render_max=%u.%02u cadence_avg=%u.%02u cadence_max=%u.%02u cadence_over16667=%u cadence_over20=%u cadence_over33334=%u cadence_over34=%u floor_fps=30 floor_budget_us=33334 floor_misses=%u floor_streak_max=%u target_fps=%u target_budget_us=%u target_samples=%u target_misses=%u total_max=%u.%02u over20=%u over34=%u",
		rsxgl_stats_frames, rsxgl_stats_api_draws, rsxgl_stats_gpu_draws,
		rsxgl_stats_batched_draws, rsxgl_stats_batch_flushes,
		rsxgl_stats_vertices, rsxgl_stats_indices,
		rsxgl_stats_state_skips, rsxgl_stats_raster_state_hits,
		rsxgl_stats_fast_array_vertices,
		rsxgl_stats_ui_strings, rsxgl_stats_ui_glyphs,
		rsxgl_stats_ui_duplicate_strings,
		rsxgl_stats_ui_state_hits,
		rsxgl_stats_ui_batch_starts, rsxgl_stats_ui_batch_merges,
		rsxgl_stats_ortho_hits,
		rsxgl_stats_texture_updates, rsxgl_stats_texture_inplace,
		rsxgl_stats_texture_renames, rsxgl_stats_texture_rename_kib,
		rsxgl_stats_combined_lightmap_draws,
		rsxgl_stats_combined_lightmap_vertices,
		rsxgl_stats_static_lightmap_vertices,
		rsxgl_stats_stereo_lightmap_reuse_pixels,
		rsxgl_stats_lightmap_base_sampler_sets,
		rsxgl_stats_lightmap_page_sampler_sets,
		rsxgl_stats_final_light_cache_hits,
		rsxgl_stats_world_clip_tests,
		rsxgl_stats_world_clip_skips,
		rsxgl_stats_finishes,
		wait_average_x100 / 100, wait_average_x100 % 100,
		wait_maximum_x100 / 100, wait_maximum_x100 % 100,
		render_average_x100 / 100, render_average_x100 % 100,
		render_maximum_x100 / 100, render_maximum_x100 % 100,
		cadence_average_x100 / 100, cadence_average_x100 % 100,
		cadence_maximum_x100 / 100, cadence_maximum_x100 % 100,
		rsxgl_stats_cadence_over_16667us,
		rsxgl_stats_cadence_over_20ms,
		rsxgl_stats_cadence_over_33334us,
		rsxgl_stats_cadence_over_34ms,
		rsxgl_stats_cadence_over_33334us,
		rsxgl_stats_floor_miss_streak_max,
		target_fps, target_budget_us,
		rsxgl_stats_cadence_samples, target_misses,
		total_maximum_x100 / 100, total_maximum_x100 % 100,
		rsxgl_stats_over_20ms, rsxgl_stats_over_34ms);
	PS3_RUNTIME_TRACE(line);
	if (rsxgl_stats_scene_frames)
	{
		for (i = 0; i < RSXGL_RENDER_STAGE_COUNT; i++)
		{
			stage_average_us[i] = (unsigned int)(
				rsxgl_stats_render_stages_us[i] /
				rsxgl_stats_scene_frames);
			stage_maximum_us[i] = (unsigned int)
				rsxgl_stats_render_stage_max_us[i];
		}
		scene_average_us = (unsigned int)(rsxgl_stats_scene_us /
			rsxgl_stats_scene_frames);
		scene_maximum_us = (unsigned int)rsxgl_stats_scene_max_us;
		other_average_us = (unsigned int)(rsxgl_stats_other_us /
			rsxgl_stats_scene_frames);
		other_maximum_us = (unsigned int)rsxgl_stats_other_max_us;
		Com_sprintf(stage_line, sizeof(stage_line),
			"PPU stages: scene_frames=%u scene_avg_us=%u scene_max_us=%u dlights_avg_us=%u dlights_max_us=%u setup_avg_us=%u setup_max_us=%u world_avg_us=%u world_max_us=%u entities_avg_us=%u entities_max_us=%u effects_avg_us=%u effects_max_us=%u alpha_avg_us=%u alpha_max_us=%u other_avg_us=%u other_max_us=%u alias_sources=%u alias_refs=%u",
			rsxgl_stats_scene_frames, scene_average_us, scene_maximum_us,
			stage_average_us[RSXGL_RENDER_STAGE_DLIGHTS],
			stage_maximum_us[RSXGL_RENDER_STAGE_DLIGHTS],
			stage_average_us[RSXGL_RENDER_STAGE_SETUP],
			stage_maximum_us[RSXGL_RENDER_STAGE_SETUP],
			stage_average_us[RSXGL_RENDER_STAGE_WORLD],
			stage_maximum_us[RSXGL_RENDER_STAGE_WORLD],
			stage_average_us[RSXGL_RENDER_STAGE_ENTITIES],
			stage_maximum_us[RSXGL_RENDER_STAGE_ENTITIES],
			stage_average_us[RSXGL_RENDER_STAGE_EFFECTS],
			stage_maximum_us[RSXGL_RENDER_STAGE_EFFECTS],
			stage_average_us[RSXGL_RENDER_STAGE_ALPHA],
			stage_maximum_us[RSXGL_RENDER_STAGE_ALPHA],
			other_average_us, other_maximum_us,
			rsxgl_stats_alias_sources, rsxgl_stats_alias_refs);
		PS3_RUNTIME_TRACE(stage_line);
	}
	rsxgl_reset_stats_accumulator();
}

static u32
rsxgl_compare(GLenum value)
{
	switch (value)
	{
		case GL_NEVER: return GCM_NEVER;
		case GL_EQUAL: return GCM_EQUAL;
		case GL_LEQUAL: return GCM_LEQUAL;
		case GL_GREATER: return GCM_GREATER;
		case GL_GEQUAL: return GCM_GEQUAL;
		case GL_LESS: return GCM_LESS;
		default: return GCM_ALWAYS;
	}
}

static u32
rsxgl_primitive(GLenum mode)
{
	switch (mode)
	{
		case GL_POINTS: return GCM_TYPE_POINTS;
		case GL_LINES: return GCM_TYPE_LINES;
		case GL_LINE_STRIP: return GCM_TYPE_LINE_STRIP;
		case GL_TRIANGLE_STRIP: return GCM_TYPE_TRIANGLE_STRIP;
		case GL_TRIANGLE_FAN: return GCM_TYPE_TRIANGLE_FAN;
		case GL_QUADS: return GCM_TYPE_QUADS;
		default: return GCM_TYPE_TRIANGLES;
	}
}

static u8
rsxgl_min_filter(GLint filter)
{
	switch (filter)
	{
		case GL_NEAREST: return GCM_TEXTURE_NEAREST;
		case GL_NEAREST_MIPMAP_NEAREST:
			return GCM_TEXTURE_NEAREST_MIPMAP_NEAREST;
		case GL_LINEAR_MIPMAP_NEAREST:
			return GCM_TEXTURE_LINEAR_MIPMAP_NEAREST;
		case GL_NEAREST_MIPMAP_LINEAR:
			return GCM_TEXTURE_NEAREST_MIPMAP_LINEAR;
		case GL_LINEAR_MIPMAP_LINEAR:
			return GCM_TEXTURE_LINEAR_MIPMAP_LINEAR;
		default: return GCM_TEXTURE_LINEAR;
	}
}

static u8
rsxgl_mag_filter(GLint filter)
{
	return (filter == GL_NEAREST) ? GCM_TEXTURE_NEAREST : GCM_TEXTURE_LINEAR;
}

static u8
rsxgl_anisotropy(GLint value)
{
	if (value >= 16) return GCM_TEXTURE_MAX_ANISO_16;
	if (value >= 12) return GCM_TEXTURE_MAX_ANISO_12;
	if (value >= 10) return GCM_TEXTURE_MAX_ANISO_10;
	if (value >= 8) return GCM_TEXTURE_MAX_ANISO_8;
	if (value >= 6) return GCM_TEXTURE_MAX_ANISO_6;
	if (value >= 4) return GCM_TEXTURE_MAX_ANISO_4;
	if (value >= 2) return GCM_TEXTURE_MAX_ANISO_2;
	return GCM_TEXTURE_MAX_ANISO_1;
}

static void
rsxgl_initialize_texture_parameters(rsxgl_texture_t *slot)
{
	if (slot->parameters_initialized) return;
	/* Preserve the compatibility layer's proven non-mip default. Quake sets an
	 * explicit mip filter after uploading every world-texture chain. */
	slot->min_filter = GL_LINEAR;
	slot->mag_filter = GL_LINEAR;
	slot->max_anisotropy = 1;
	slot->wrap_s = GL_REPEAT;
	slot->wrap_t = GL_REPEAT;
	slot->generate_mipmap = GL_FALSE;
	slot->parameters_initialized = GL_TRUE;
}

static int
rsxgl_full_mipmap_count(int width, int height)
{
	int levels = 1;
	while ((width > 1 || height > 1) && levels < RSXGL_MAX_MIP_LEVELS)
	{
		if (width > 1) width >>= 1;
		if (height > 1) height >>= 1;
		levels++;
	}
	return levels;
}

static int
rsxgl_mipmap_dimensions(const rsxgl_texture_t *slot, int level,
	int *width, int *height)
{
	int w;
	int h;
	int i;
	if (!slot || level < 0 || level >= rsxgl_full_mipmap_count(
		slot->width, slot->height)) return 0;
	w = slot->width;
	h = slot->height;
	for (i = 0; i < level; i++)
	{
		if (w > 1) w >>= 1;
		if (h > 1) h >>= 1;
	}
	if (width) *width = w;
	if (height) *height = h;
	return 1;
}

static size_t
rsxgl_mipmap_offset(const rsxgl_texture_t *slot, int level)
{
	size_t offset = 0;
	int height = slot->height;
	int i;
	for (i = 0; i < level; i++)
	{
		offset += (size_t)slot->pitch * height;
		if (height > 1) height >>= 1;
	}
	return offset;
}

static size_t
rsxgl_mipmap_storage_size(int pitch, int height, int levels)
{
	size_t size = 0;
	int i;
	for (i = 0; i < levels; i++)
	{
		size_t level_size = (size_t)pitch * height;
		if (level_size > (size_t)-1 - size) return 0;
		size += level_size;
		if (height > 1) height >>= 1;
	}
	return size;
}

static size_t
rsxgl_align_size(size_t value, size_t alignment)
{
	if (!alignment || value > (size_t)-1 - (alignment - 1))
	{
		return 0;
	}
	return (value + alignment - 1) & ~(alignment - 1);
}

static void
rsxgl_store_ppu_cache_range(const void *pointer, size_t size)
{
	uintptr_t address;
	uintptr_t end;

	if (!pointer || !size)
	{
		return;
	}
	address = (uintptr_t)pointer &
		~((uintptr_t)RSXGL_PPU_CACHE_LINE_SIZE - 1);
	end = (uintptr_t)pointer + size;
	while (address < end)
	{
		/* Publish dirty PPU stores for an RSX read while retaining the line in
		 * the PPU cache. The caller batches all affected ranges behind one sync. */
		__asm__ volatile("dcbst 0,%0" : : "r"((void *)address) : "memory");
		address += RSXGL_PPU_CACHE_LINE_SIZE;
	}
}

static void
rsxgl_flush_ppu_cache_range(const void *pointer, size_t size)
{
	uintptr_t address;
	uintptr_t end;

	if (!pointer || !size)
	{
		return;
	}
	address = (uintptr_t)pointer &
		~((uintptr_t)RSXGL_PPU_CACHE_LINE_SIZE - 1);
	end = (uintptr_t)pointer + size;
	while (address < end)
	{
		/* Unlike dcbst, dcbf also invalidates the PPU copy. That is desirable
		 * for the append-only UI arena: the PPU does not revisit a submitted
		 * range, and the stronger operation guarantees the following RSX DMA
		 * reads storage rather than a dirty processor-cache generation. */
		__asm__ volatile("dcbf 0,%0" : : "r"((void *)address) : "memory");
		address += RSXGL_PPU_CACHE_LINE_SIZE;
	}
	__sync_synchronize();
}

static void
rsxgl_invalidate_ppu_cache_for_gpu_write(void *pointer, size_t size)
{
	uintptr_t address;
	uintptr_t end;

	if (!pointer || !size)
	{
		return;
	}
	address = (uintptr_t)pointer &
		~((uintptr_t)RSXGL_PPU_CACHE_LINE_SIZE - 1);
	end = (uintptr_t)pointer + size;
	while (address < end)
	{
		/* Write back and invalidate before DMA so no stale dirty PPU line can
		 * later overwrite the RSX result, and the first post-DMA read misses. */
		__asm__ volatile("dcbf 0,%0" : : "r"((void *)address) : "memory");
		address += RSXGL_PPU_CACHE_LINE_SIZE;
	}
	__sync_synchronize();
}

static void
rsxgl_disable_tiled_targets(void)
{
	int i;

	if (rsxgl_zcull_bound)
	{
		gcmUnbindZcull(0);
		rsxgl_zcull_bound = 0;
	}
	for (i = rsxgl_bound_tile_count - 1; i >= 0; i--)
	{
		gcmUnbindTile(RSXGL_COLOR_TILE_BASE + i);
	}
	rsxgl_bound_tile_count = 0;
}

static int
rsxgl_configure_tiled_targets(void)
{
	int i;
	u8 depth_compression;

	if (!rsxgl_tiled_targets_requested)
	{
		return 0;
	}
	for (i = 0; i < RSXGL_DISPLAY_BUFFERS; i++)
	{
		int tile = RSXGL_COLOR_TILE_BASE + i;
		if (gcmSetTileInfo(tile, GCM_LOCATION_RSX, rsxgl_color_offset[i],
			(u32)rsxgl_color_allocation_size, rsxgl_color_pitch,
			GCM_COMPMODE_DISABLED, 0, 0) != 0 ||
			gcmBindTile(tile) != 0)
		{
			rsxgl_disable_tiled_targets();
			return 0;
		}
		rsxgl_bound_tile_count++;
	}
	depth_compression = rsxgl_zcull_requested ?
		GCM_COMPMODE_Z32_SEPSTENCIL : GCM_COMPMODE_DISABLED;
	if (gcmSetTileInfo(RSXGL_DEPTH_TILE_INDEX, GCM_LOCATION_RSX,
		rsxgl_depth_offset, (u32)rsxgl_depth_allocation_size,
		rsxgl_depth_pitch, depth_compression, 0, 2) != 0 ||
		gcmBindTile(RSXGL_DEPTH_TILE_INDEX) != 0)
	{
		rsxgl_disable_tiled_targets();
		return 0;
	}
	rsxgl_bound_tile_count++;

	if (rsxgl_zcull_requested)
	{
		/* Quake clears Z to one and normally compares LEQUAL, matching the
		 * native LESS-direction layout. The depth allocation has already been
		 * rounded to this registration's 64-row extent. */
		gcmSetZcull(0, rsxgl_depth_offset,
			(rsxgl_output_width + GCM_ZCULL_ALIGN_WIDTH - 1) &
				~(GCM_ZCULL_ALIGN_WIDTH - 1),
			(rsxgl_output_height + GCM_ZCULL_ALIGN_HEIGHT - 1) &
				~(GCM_ZCULL_ALIGN_HEIGHT - 1),
			0, GCM_ZCULL_Z24S8, GCM_SURFACE_CENTER_1, GCM_ZCULL_LESS,
			GCM_ZCULL_LONES, GCM_SCULL_SFUNC_LESS, 1, 0xff);
		rsxgl_zcull_bound = 1;
	}
	return 1;
}

static int
rsxgl_wait_for_flip(void)
{
	u32 iterations = 0;
	while (gcmGetFlipStatus() != 0 && iterations++ < 100000)
	{
		/* XMB exit takes ownership of video output, so waiting for a flip can
		 * otherwise hide the system event for the full timeout. */
		if ((iterations % 200) == 0 && Sys_PS3_CheckCallbacks())
		{
			PS3_RUNTIME_TRACE("RSX renderer: XMB exit interrupted flip wait");
			return 0;
		}
		/* A 200 us poll interval consumed over one percent of a 60 Hz frame in
		 * worst-case wake-up latency. The shorter wait keeps command generation
		 * closer to the completed vblank while retaining a bounded five-second
		 * timeout and low PPU use. */
		sysUsleep(50);
	}
	if (gcmGetFlipStatus() != 0)
	{
		if (!rsxgl_traced_flip_timeout)
		{
			PS3_RUNTIME_TRACE("RSX renderer: flip wait timed out; frame submission paused");
			rsxgl_traced_flip_timeout = 1;
		}
		return 0;
	}
	gcmResetFlipStatus();
	return 1;
}

static void
rsxgl_finish(void)
{
	/* A deferred compatibility draw must enter the command stream before the
	 * fence. This also makes every existing texture-memory fence a safe batch
	 * boundary without duplicating special cases in its callers. */
	rsxgl_flush_triangle_batch();
	if (rsxgl_frame_ready)
	{
		rsxgl_frame_finishes++;
	}
	if (rsxgl_context)
	{
		if (!rsxgl_traced_first_finish)
		{
			PS3_BOOT_TRACE("RSX renderer: entering first GPU finish");
		}
		rsxFinish(rsxgl_context, rsxgl_finish_reference++);
		if (!rsxgl_traced_first_finish)
		{
			PS3_BOOT_TRACE("RSX renderer: first GPU finish complete");
			rsxgl_traced_first_finish = 1;
		}
	}
}

static void
rsxgl_release_retired_textures(void)
{
	unsigned int i;
	for (i = 0; i < rsxgl_retired_texture_count; i++)
	{
		rsxFree(rsxgl_retired_textures[i]);
	}
	rsxgl_retired_texture_count = 0;
	rsxgl_retired_texture_bytes = 0;
}

static void
rsxgl_retire_texture(void *pixels, size_t size)
{
	if (!pixels)
	{
		return;
	}
	if (size > RSXGL_MAX_RETIRED_TEXTURE_BYTES)
	{
		/* An unusually large dynamic texture cannot be retained inside the
		 * bounded copy-on-update budget. Complete its earlier readers now. */
		rsxgl_finish();
		rsxgl_release_retired_textures();
		rsxFree(pixels);
		if (!rsxgl_traced_texture_retire_pressure)
		{
			PS3_BOOT_TRACE("RSX renderer: texture rename budget forced GPU finish");
			rsxgl_traced_texture_retire_pressure = 1;
		}
		return;
	}
	if (rsxgl_retired_texture_count >= RSXGL_MAX_RETIRED_TEXTURES ||
		rsxgl_retired_texture_bytes >
			RSXGL_MAX_RETIRED_TEXTURE_BYTES - size)
	{
		/* Bound transient RSX memory even during an extreme dynamic-light
		 * frame. Earlier draws are completed before any allocation is recycled. */
		rsxgl_finish();
		rsxgl_release_retired_textures();
		if (!rsxgl_traced_texture_retire_pressure)
		{
			PS3_BOOT_TRACE("RSX renderer: texture rename budget forced GPU finish");
			rsxgl_traced_texture_retire_pressure = 1;
		}
	}
	rsxgl_retired_textures[rsxgl_retired_texture_count++] = pixels;
	rsxgl_retired_texture_bytes += size;
}

static void
rsxgl_set_surface(int index)
{
	rsxgl_surface.colorOffset[0] = rsxgl_color_offset[index];
	rsxSetSurface(rsxgl_context, &rsxgl_surface);
}

static void
rsxgl_clear_acquired_surface(void)
{
	u32 clear_mask = GCM_CLEAR_R | GCM_CLEAR_G | GCM_CLEAR_B |
		GCM_CLEAR_A | GCM_CLEAR_S;
	/* Clear immediately after binding the new back buffer under an explicit
	 * full-target scissor. GL1's clear inherited the preceding pass's scissor
	 * and stereo-eye rectangle on physical RSX, allowing old console rows and
	 * translucent objects to survive. One acquisition clear also replaces the
	 * redundant per-eye color/depth clears in frame-packed stereo. */
	rsxSetScissor(rsxgl_context, 0, 0, rsxgl_output_width,
		rsxgl_output_height);
	rsxgl_physical_scissor_x = 0;
	rsxgl_physical_scissor_y = 0;
	rsxgl_physical_scissor_width = (u16)rsxgl_output_width;
	rsxgl_physical_scissor_height = (u16)rsxgl_output_height;
	rsxgl_physical_scissor_valid = 1;
	rsxSetColorMask(rsxgl_context, GCM_COLOR_MASK_R | GCM_COLOR_MASK_G |
		GCM_COLOR_MASK_B | GCM_COLOR_MASK_A);
	rsxSetDepthWriteEnable(rsxgl_context, GCM_TRUE);
	rsxSetStencilMask(rsxgl_context, 0xff);
	rsxSetClearColor(rsxgl_context, 0x00000000);
	rsxSetClearDepthStencil(rsxgl_context, 0xffffff00u);
	if (!rsxgl_cvar_ztrick || rsxgl_cvar_ztrick->value == 0.0f)
	{
		clear_mask |= GCM_CLEAR_Z;
	}
	rsxClearSurface(rsxgl_context, clear_mask);

	/* Synchronize the fixed-state mirror with the methods emitted directly
	 * above, so filtering cannot suppress a required later state transition. */
	rsxgl_color_mask_state = GCM_COLOR_MASK_R | GCM_COLOR_MASK_G |
		GCM_COLOR_MASK_B | GCM_COLOR_MASK_A;
	rsxgl_depth_mask_state = GL_TRUE;
	rsxgl_stencil_write_mask_state = 0xff;
	rsxgl_stencil_mask_valid = 1;
	if (!rsxgl_traced_acquired_surface_clear)
	{
		PS3_RUNTIME_TRACE("RSX renderer: acquired surfaces receive full ordered clear");
		rsxgl_traced_acquired_surface_clear = 1;
	}
}

static void
rsxgl_apply_viewport(GLint x, GLint y, GLsizei width, GLsizei height)
{
	float scale_x = (float)rsxgl_output_width / (float)rsxgl_virtual_width;
	float scale_y = (float)rsxgl_target_height / (float)rsxgl_virtual_height;
	u16 physical_x = (u16)(x * scale_x);
	u16 physical_width = (u16)(width * scale_x);
	u16 physical_height = (u16)(height * scale_y);
	u16 physical_y = (u16)(rsxgl_target_y + rsxgl_target_height -
		(y + height) * scale_y);
	float scale[4];
	float offset[4];

	if (physical_width < 1) physical_width = 1;
	if (physical_height < 1) physical_height = 1;
	if (rsxgl_state_filter_enabled && rsxgl_physical_viewport_valid &&
		rsxgl_physical_viewport_x == physical_x &&
		rsxgl_physical_viewport_y == physical_y &&
		rsxgl_physical_viewport_width == physical_width &&
		rsxgl_physical_viewport_height == physical_height &&
		rsxgl_physical_viewport_near == rsxgl_depth_near &&
		rsxgl_physical_viewport_far == rsxgl_depth_far)
	{
		rsxgl_note_physical_raster_hit();
		return;
	}
	scale[0] = physical_width * 0.5f;
	scale[1] = physical_height * -0.5f;
	scale[2] = (rsxgl_depth_far - rsxgl_depth_near) * 0.5f;
	scale[3] = 0.0f;
	offset[0] = physical_x + physical_width * 0.5f;
	offset[1] = physical_y + physical_height * 0.5f;
	offset[2] = (rsxgl_depth_far + rsxgl_depth_near) * 0.5f;
	offset[3] = 0.0f;
	rsxSetViewport(rsxgl_context, physical_x, physical_y,
		physical_width, physical_height, rsxgl_depth_near, rsxgl_depth_far,
		scale, offset);
	rsxgl_physical_viewport_x = physical_x;
	rsxgl_physical_viewport_y = physical_y;
	rsxgl_physical_viewport_width = physical_width;
	rsxgl_physical_viewport_height = physical_height;
	rsxgl_physical_viewport_near = rsxgl_depth_near;
	rsxgl_physical_viewport_far = rsxgl_depth_far;
	rsxgl_physical_viewport_valid = 1;
}

static void
rsxgl_apply_scissor(void)
{
	float scale_x = (float)rsxgl_output_width / rsxgl_virtual_width;
	float scale_y = (float)rsxgl_target_height / rsxgl_virtual_height;
	int target_bottom = rsxgl_target_y + rsxgl_target_height;
	int x;
	int y;
	int width;
	int height;

	if (!rsxgl_scissor_enabled)
	{
		x = 0;
		y = rsxgl_target_y;
		width = rsxgl_output_width;
		height = rsxgl_target_height;
	}
	else
	{
		x = (int)(rsxgl_scissor_x * scale_x);
		y = (int)(rsxgl_target_y + rsxgl_target_height -
			(rsxgl_scissor_y + rsxgl_scissor_height) * scale_y);
		width = (int)(rsxgl_scissor_width * scale_x);
		height = (int)(rsxgl_scissor_height * scale_y);
		if (x < 0) { width += x; x = 0; }
		if (y < rsxgl_target_y)
		{
			height -= rsxgl_target_y - y;
			y = rsxgl_target_y;
		}
		if (x + width > rsxgl_output_width)
			width = rsxgl_output_width - x;
		if (y + height > target_bottom)
			height = target_bottom - y;
		if (width < 1) width = 1;
		if (height < 1) height = 1;
	}
	if (rsxgl_state_filter_enabled && rsxgl_physical_scissor_valid &&
		rsxgl_physical_scissor_x == (u16)x &&
		rsxgl_physical_scissor_y == (u16)y &&
		rsxgl_physical_scissor_width == (u16)width &&
		rsxgl_physical_scissor_height == (u16)height)
	{
		rsxgl_note_physical_raster_hit();
		return;
	}
	rsxSetScissor(rsxgl_context, x, y, width, height);
	rsxgl_physical_scissor_x = (u16)x;
	rsxgl_physical_scissor_y = (u16)y;
	rsxgl_physical_scissor_width = (u16)width;
	rsxgl_physical_scissor_height = (u16)height;
	rsxgl_physical_scissor_valid = 1;
}

static void
rsxgl_bind_white_texture(u8 unit)
{
	gcmTexture texture;
	memset(&texture, 0, sizeof(texture));
	texture.format = GCM_TEXTURE_FORMAT_A8R8G8B8 | GCM_TEXTURE_FORMAT_LIN;
	texture.mipmap = 1;
	texture.dimension = GCM_TEXTURE_DIMS_2D;
	texture.width = 1;
	texture.height = 1;
	texture.depth = 1;
	texture.location = GCM_LOCATION_RSX;
	texture.pitch = 64;
	texture.offset = rsxgl_white_offset;
	texture.remap =
		(GCM_TEXTURE_REMAP_TYPE_REMAP << GCM_TEXTURE_REMAP_TYPE_B_SHIFT) |
		(GCM_TEXTURE_REMAP_TYPE_REMAP << GCM_TEXTURE_REMAP_TYPE_G_SHIFT) |
		(GCM_TEXTURE_REMAP_TYPE_REMAP << GCM_TEXTURE_REMAP_TYPE_R_SHIFT) |
		(GCM_TEXTURE_REMAP_TYPE_REMAP << GCM_TEXTURE_REMAP_TYPE_A_SHIFT) |
		(GCM_TEXTURE_REMAP_COLOR_B << GCM_TEXTURE_REMAP_COLOR_B_SHIFT) |
		(GCM_TEXTURE_REMAP_COLOR_G << GCM_TEXTURE_REMAP_COLOR_G_SHIFT) |
		(GCM_TEXTURE_REMAP_COLOR_R << GCM_TEXTURE_REMAP_COLOR_R_SHIFT) |
		(GCM_TEXTURE_REMAP_COLOR_A << GCM_TEXTURE_REMAP_COLOR_A_SHIFT);
	rsxLoadTexture(rsxgl_context, unit, &texture);
	rsxTextureControl(rsxgl_context, unit,
		GCM_TRUE, 0, 0, GCM_TEXTURE_MAX_ANISO_1);
	rsxTextureFilter(rsxgl_context, unit, 0,
		GCM_TEXTURE_NEAREST, GCM_TEXTURE_NEAREST, GCM_TEXTURE_CONVOLUTION_QUINCUNX);
	rsxTextureWrapMode(rsxgl_context, unit,
		GCM_TEXTURE_CLAMP_TO_EDGE, GCM_TEXTURE_CLAMP_TO_EDGE,
		GCM_TEXTURE_CLAMP_TO_EDGE, 0, GCM_TEXTURE_ZFUNC_LESS, 0);
}

static void
rsxgl_load_texture_unit(GLuint texture_id, u8 unit)
{
	rsxgl_texture_t *slot;
	gcmTexture texture;
	u8 wrap_s;
	u8 wrap_t;

	if (texture_id >= RSXGL_MAX_TEXTURES ||
		!rsxgl_textures[texture_id].pixels)
	{
		rsxgl_bind_white_texture(unit);
		return;
	}

	slot = &rsxgl_textures[texture_id];
	memset(&texture, 0, sizeof(texture));
	texture.format = GCM_TEXTURE_FORMAT_A8R8G8B8 | GCM_TEXTURE_FORMAT_LIN;
	texture.mipmap = slot->mipmap_levels > 0 ? slot->mipmap_levels : 1;
	texture.dimension = GCM_TEXTURE_DIMS_2D;
	texture.width = slot->width;
	texture.height = slot->height;
	texture.depth = 1;
	texture.location = GCM_LOCATION_RSX;
	texture.pitch = slot->pitch;
	texture.offset = slot->offset;
	texture.remap =
		(GCM_TEXTURE_REMAP_TYPE_REMAP << GCM_TEXTURE_REMAP_TYPE_B_SHIFT) |
		(GCM_TEXTURE_REMAP_TYPE_REMAP << GCM_TEXTURE_REMAP_TYPE_G_SHIFT) |
		(GCM_TEXTURE_REMAP_TYPE_REMAP << GCM_TEXTURE_REMAP_TYPE_R_SHIFT) |
		(GCM_TEXTURE_REMAP_TYPE_REMAP << GCM_TEXTURE_REMAP_TYPE_A_SHIFT) |
		(GCM_TEXTURE_REMAP_COLOR_B << GCM_TEXTURE_REMAP_COLOR_B_SHIFT) |
		(GCM_TEXTURE_REMAP_COLOR_G << GCM_TEXTURE_REMAP_COLOR_G_SHIFT) |
		(GCM_TEXTURE_REMAP_COLOR_R << GCM_TEXTURE_REMAP_COLOR_R_SHIFT) |
		(GCM_TEXTURE_REMAP_COLOR_A << GCM_TEXTURE_REMAP_COLOR_A_SHIFT);
	wrap_s = slot->wrap_s == GL_REPEAT ?
		GCM_TEXTURE_REPEAT : GCM_TEXTURE_CLAMP_TO_EDGE;
	wrap_t = slot->wrap_t == GL_REPEAT ?
		GCM_TEXTURE_REPEAT : GCM_TEXTURE_CLAMP_TO_EDGE;
	rsxLoadTexture(rsxgl_context, unit, &texture);
	rsxTextureControl(rsxgl_context, unit, GCM_TRUE, 0,
		(texture.mipmap - 1) << 8, rsxgl_anisotropy(slot->max_anisotropy));
	rsxTextureFilter(rsxgl_context, unit, 0,
		rsxgl_min_filter(slot->min_filter), rsxgl_mag_filter(slot->mag_filter),
		GCM_TEXTURE_CONVOLUTION_QUINCUNX);
	rsxTextureWrapMode(rsxgl_context, unit, wrap_s, wrap_t,
		GCM_TEXTURE_CLAMP_TO_EDGE, 0, GCM_TEXTURE_ZFUNC_LESS, 0);
	slot->ever_applied = GL_TRUE;
	slot->last_sampled_pixels = slot->pixels;
	slot->last_sampled_frame = rsxgl_frame_serial;
	if (texture.mipmap > 1 && !rsxgl_traced_first_mipmap)
	{
		PS3_RUNTIME_TRACE("RSX renderer: native mipmapped texture sampling active");
		rsxgl_traced_first_mipmap = 1;
	}
}

static void
rsxgl_note_texture_sample(GLuint texture_id)
{
	if (texture_id >= RSXGL_MAX_TEXTURES ||
		!rsxgl_textures[texture_id].pixels)
	{
		return;
	}
	rsxgl_textures[texture_id].last_sampled_pixels =
		rsxgl_textures[texture_id].pixels;
	rsxgl_textures[texture_id].last_sampled_frame = rsxgl_frame_serial;
}

static void
rsxgl_dirty_lightmap_sampler_for_texture(GLuint texture_id)
{
	if (texture_id == rsxgl_lightmap_base_texture)
	{
		rsxgl_lightmap_base_sampler_dirty = 1;
	}
	if (texture_id == rsxgl_lightmap_texture)
	{
		rsxgl_lightmap_page_sampler_dirty = 1;
	}
}

static void
rsxgl_apply_lightmap_textures(void)
{
	if (rsxgl_texture_cache_dirty)
	{
		rsxInvalidateTextureCache(rsxgl_context, GCM_INVALIDATE_TEXTURE);
		rsxgl_texture_cache_dirty = 0;
	}
	if (!rsxgl_lightmap_base_sampler_dirty &&
		!rsxgl_lightmap_page_sampler_dirty)
	{
		/* Preserve copy-on-update hazard tracking even when sampler methods are
		 * already resident for this texture pair. */
		rsxgl_note_texture_sample(rsxgl_lightmap_base_texture);
		rsxgl_note_texture_sample(rsxgl_lightmap_texture);
		return;
	}

	if (rsxgl_lightmap_base_sampler_dirty)
	{
		rsxgl_load_texture_unit(rsxgl_lightmap_base_texture,
			rsxgl_lightmap_texture_parameter->index);
		rsxgl_lightmap_base_sampler_dirty = 0;
		if (rsxgl_frame_ready)
		{
			rsxgl_frame_lightmap_base_sampler_sets++;
		}
	}
	else
	{
		rsxgl_note_texture_sample(rsxgl_lightmap_base_texture);
	}

	if (rsxgl_lightmap_page_sampler_dirty)
	{
		rsxgl_load_texture_unit(rsxgl_lightmap_texture,
			rsxgl_lightmap_parameter->index);
		rsxgl_lightmap_page_sampler_dirty = 0;
		if (rsxgl_frame_ready)
		{
			rsxgl_frame_lightmap_page_sampler_sets++;
		}
	}
	else
	{
		rsxgl_note_texture_sample(rsxgl_lightmap_texture);
	}
}

static void
rsxgl_apply_texture(void)
{
	rsxgl_texture_t *slot;
	gcmTexture texture;
	u8 wrap_s;
	u8 wrap_t;

	if (rsxgl_texture_cache_dirty)
	{
		rsxInvalidateTextureCache(rsxgl_context, GCM_INVALIDATE_TEXTURE);
		rsxgl_texture_cache_dirty = 0;
	}
	if (!rsxgl_sampler_dirty)
	{
		if (rsxgl_texture_enabled &&
			rsxgl_bound_texture < RSXGL_MAX_TEXTURES &&
			rsxgl_textures[rsxgl_bound_texture].pixels)
		{
			rsxgl_textures[rsxgl_bound_texture].last_sampled_pixels =
				rsxgl_textures[rsxgl_bound_texture].pixels;
			rsxgl_textures[rsxgl_bound_texture].last_sampled_frame =
				rsxgl_frame_serial;
		}
		if (!rsxgl_traced_sampler_cache)
		{
			PS3_BOOT_TRACE("RSX renderer: sampler-state cache active");
			rsxgl_traced_sampler_cache = 1;
		}
		return;
	}

	if (!rsxgl_texture_enabled || rsxgl_bound_texture >= RSXGL_MAX_TEXTURES ||
		!rsxgl_textures[rsxgl_bound_texture].pixels)
	{
		rsxgl_bind_white_texture(rsxgl_texture_parameter->index);
		rsxgl_sampler_dirty = 0;
		return;
	}

	slot = &rsxgl_textures[rsxgl_bound_texture];
	memset(&texture, 0, sizeof(texture));
	texture.format = GCM_TEXTURE_FORMAT_A8R8G8B8 | GCM_TEXTURE_FORMAT_LIN;
	texture.mipmap = slot->mipmap_levels > 0 ? slot->mipmap_levels : 1;
	texture.dimension = GCM_TEXTURE_DIMS_2D;
	texture.width = slot->width;
	texture.height = slot->height;
	texture.depth = 1;
	texture.location = GCM_LOCATION_RSX;
	texture.pitch = slot->pitch;
	texture.offset = slot->offset;
	texture.remap =
		(GCM_TEXTURE_REMAP_TYPE_REMAP << GCM_TEXTURE_REMAP_TYPE_B_SHIFT) |
		(GCM_TEXTURE_REMAP_TYPE_REMAP << GCM_TEXTURE_REMAP_TYPE_G_SHIFT) |
		(GCM_TEXTURE_REMAP_TYPE_REMAP << GCM_TEXTURE_REMAP_TYPE_R_SHIFT) |
		(GCM_TEXTURE_REMAP_TYPE_REMAP << GCM_TEXTURE_REMAP_TYPE_A_SHIFT) |
		(GCM_TEXTURE_REMAP_COLOR_B << GCM_TEXTURE_REMAP_COLOR_B_SHIFT) |
		(GCM_TEXTURE_REMAP_COLOR_G << GCM_TEXTURE_REMAP_COLOR_G_SHIFT) |
		(GCM_TEXTURE_REMAP_COLOR_R << GCM_TEXTURE_REMAP_COLOR_R_SHIFT) |
		(GCM_TEXTURE_REMAP_COLOR_A << GCM_TEXTURE_REMAP_COLOR_A_SHIFT);
	wrap_s = (slot->wrap_s == GL_REPEAT) ? GCM_TEXTURE_REPEAT : GCM_TEXTURE_CLAMP_TO_EDGE;
	wrap_t = (slot->wrap_t == GL_REPEAT) ? GCM_TEXTURE_REPEAT : GCM_TEXTURE_CLAMP_TO_EDGE;
	rsxLoadTexture(rsxgl_context, rsxgl_texture_parameter->index, &texture);
	rsxTextureControl(rsxgl_context, rsxgl_texture_parameter->index,
		GCM_TRUE, 0, (texture.mipmap - 1) << 8,
		rsxgl_anisotropy(slot->max_anisotropy));
	rsxTextureFilter(rsxgl_context, rsxgl_texture_parameter->index, 0,
		rsxgl_min_filter(slot->min_filter), rsxgl_mag_filter(slot->mag_filter),
		GCM_TEXTURE_CONVOLUTION_QUINCUNX);
	rsxTextureWrapMode(rsxgl_context, rsxgl_texture_parameter->index,
		wrap_s, wrap_t, GCM_TEXTURE_CLAMP_TO_EDGE, 0, GCM_TEXTURE_ZFUNC_LESS, 0);
	slot->ever_applied = GL_TRUE;
	slot->last_sampled_pixels = slot->pixels;
	slot->last_sampled_frame = rsxgl_frame_serial;
	rsxgl_sampler_dirty = 0;
	if (texture.mipmap > 1 && !rsxgl_traced_first_mipmap)
	{
		PS3_RUNTIME_TRACE("RSX renderer: native mipmapped texture sampling active");
		rsxgl_traced_first_mipmap = 1;
	}
}

static rsxgl_vertex_t *
rsxgl_allocate_vertices(int count)
{
	size_t bytes = count * sizeof(rsxgl_vertex_t);

	/* Non-triangle primitives are submitted immediately and cannot share the
	 * contiguous triangle stream. Preserve API order before allocating them. */
	rsxgl_flush_triangle_batch();
	bytes = (bytes + 127) & ~((size_t)127);
	if (bytes > rsxgl_vertex_arena_size)
	{
		return NULL;
	}
	if (rsxgl_vertex_arena_used + bytes > rsxgl_vertex_arena_size)
	{
		rsxgl_finish();
		rsxgl_vertex_arena_used = 0;
		rsxgl_vertex_cache_needs_invalidate = 1;
	}
	{
		rsxgl_vertex_t *result = (rsxgl_vertex_t *)
			((uint8_t *)rsxgl_vertex_arena + rsxgl_vertex_arena_used);
		rsxgl_vertex_arena_used += bytes;
		return result;
	}
}

static int
rsxgl_arena_offset(const void *pointer, const void *arena, size_t arena_size,
	u32 arena_offset, u32 *offset)
{
	uintptr_t address = (uintptr_t)pointer;
	uintptr_t base = (uintptr_t)arena;
	uintptr_t delta;

	if (!pointer || !arena || !offset || address < base)
	{
		return 0;
	}
	delta = address - base;
	if (delta >= arena_size || delta > UINT32_MAX - arena_offset)
	{
		return 0;
	}
	*offset = arena_offset + (u32)delta;
	return 1;
}

static void
rsxgl_set_lightmap_program(int enabled)
{
	enabled = enabled != 0;
	if (enabled == rsxgl_lightmap_program_active &&
		!rsxgl_filter_program_active)
	{
		return;
	}
	/* Program and vertex-layout changes are hard batch boundaries. Submit the
	 * current batch with the program that was active while it was assembled. */
	rsxgl_flush_triangle_batch();
	rsxgl_filter_program_active = 0;
	rsxgl_lightmap_program_active = enabled;
	rsxgl_programs_bound = 0;
	rsxgl_mvp_valid = 0;
	if (enabled)
	{
		/* The ordinary program may have reused either hardware unit. Re-establish
		 * both descriptors on entry, then track page-only changes independently. */
		rsxgl_lightmap_base_sampler_dirty = 1;
		rsxgl_lightmap_page_sampler_dirty = 1;
		rsxgl_lightmap_scale_dirty = 1;
	}
	else
	{
		rsxgl_sampler_dirty = 1;
	}
}

static void
rsxgl_set_filter_program(int enabled)
{
	enabled = enabled != 0;
	if (enabled == rsxgl_filter_program_active &&
		(!enabled || !rsxgl_lightmap_program_active))
	{
		return;
	}

	/* The presentation shader consumes the fixed vertex layout but replaces
	 * the fragment program. Keep that transition outside every ordinary GL1
	 * batch so no scene or glyph vertices can inherit the procedural mask. */
	rsxgl_flush_triangle_batch();
	rsxgl_filter_program_active = enabled;
	if (enabled)
	{
		rsxgl_lightmap_program_active = 0;
	}
	rsxgl_programs_bound = 0;
	rsxgl_mvp_valid = 0;
	if (!enabled)
	{
		rsxgl_sampler_dirty = 1;
	}
}

static void
rsxgl_bind_vertex_arrays(u32 offset, u8 location)
{
	rsxBindVertexArrayAttrib(rsxgl_context, GCM_VERTEX_ATTRIB_POS, 0,
		offset + offsetof(rsxgl_vertex_t, xyz), sizeof(rsxgl_vertex_t),
		3, GCM_VERTEX_DATA_TYPE_F32, location);
	rsxBindVertexArrayAttrib(rsxgl_context, GCM_VERTEX_ATTRIB_TEX0, 0,
		offset + offsetof(rsxgl_vertex_t, st), sizeof(rsxgl_vertex_t),
		2, GCM_VERTEX_DATA_TYPE_F32, location);
	if (rsxgl_lightmap_program_active)
	{
		/* The combined program consumes its second UV pair and scale through
		 * TEXCOORD1, but the data occupies the ordinary layout's COLOR0 slot. */
		rsxBindVertexArrayAttrib(rsxgl_context, GCM_VERTEX_ATTRIB_TEX1, 0,
			offset + offsetof(rsxgl_vertex_t, color), sizeof(rsxgl_vertex_t),
			4, GCM_VERTEX_DATA_TYPE_F32, location);
	}
	else
	{
		rsxBindVertexArrayAttrib(rsxgl_context, GCM_VERTEX_ATTRIB_COLOR0, 0,
			offset + offsetof(rsxgl_vertex_t, color), sizeof(rsxgl_vertex_t),
			4, GCM_VERTEX_DATA_TYPE_F32, location);
	}
}

static int
rsxgl_prepare_draw_state(void)
{
	float mvp[16];
	float mvp_upload[16];
	rsxVertexProgram *vertex_program = rsxgl_lightmap_program_active ?
		rsxgl_lightmap_vp : rsxgl_vp;
	rsxFragmentProgram *fragment_program = rsxgl_filter_program_active ?
		rsxgl_filter_fp : (rsxgl_lightmap_program_active ?
		rsxgl_lightmap_fp : rsxgl_fp);
	void *vertex_ucode = rsxgl_lightmap_program_active ?
		rsxgl_lightmap_vp_ucode : rsxgl_vp_ucode;
	u32 fragment_offset = rsxgl_filter_program_active ?
		rsxgl_filter_fp_offset : (rsxgl_lightmap_program_active ?
		rsxgl_lightmap_fp_offset : rsxgl_fp_offset);
	rsxProgramConst *mvp_parameter = rsxgl_lightmap_program_active ?
		rsxgl_lightmap_mvp_parameter : rsxgl_mvp_parameter;
	int row;
	int column;

	/* Generic preparation may be reached by an immediate/non-batched draw that
	 * did not need a preceding GL state setter. Never emit program, constant,
	 * texture, or array methods inside a retained UI Begin/End. */
	rsxgl_end_inline_2d_batch();
	if (!rsxgl_initialized || !rsxgl_frame_ready)
	{
		return 0;
	}
	if (!rsxgl_traced_first_draw)
	{
		PS3_BOOT_TRACE("RSX renderer: first GPU draw entered");
	}

	if (!rsxgl_programs_bound)
	{
		rsxLoadVertexProgram(rsxgl_context, vertex_program, vertex_ucode);
	}
	if (!rsxgl_mvp_valid ||
		rsxgl_uploaded_matrix_revision != rsxgl_matrix_revision)
	{
		rsxgl_multiply(mvp, rsxgl_projection[rsxgl_projection_top],
			rsxgl_modelview[rsxgl_modelview_top]);
		/* OpenGL matrices are column-major. RSX vertex constants are loaded as
		 * consecutive Cg rows, matching the transpose used by PSL1GHT samples. */
		for (row = 0; row < 4; row++)
		{
			for (column = 0; column < 4; column++)
			{
				mvp_upload[row * 4 + column] = mvp[column * 4 + row];
			}
		}
		rsxSetVertexProgramParameter(rsxgl_context, vertex_program,
			mvp_parameter, mvp_upload);
		rsxgl_uploaded_matrix_revision = rsxgl_matrix_revision;
		rsxgl_mvp_valid = 1;
	}
	else if (!rsxgl_traced_matrix_cache)
	{
		PS3_BOOT_TRACE("RSX renderer: shader/MVP draw-state cache active");
		rsxgl_traced_matrix_cache = 1;
	}
	if (rsxgl_lightmap_program_active && rsxgl_lightmap_scale_dirty)
	{
		rsxSetVertexProgramParameter(rsxgl_context, rsxgl_lightmap_vp,
			rsxgl_lightmap_scale_parameter, rsxgl_lightmap_scale);
		rsxgl_lightmap_scale_dirty = 0;
	}
	if (!rsxgl_traced_first_draw)
	{
		PS3_BOOT_TRACE("RSX renderer: first vertex program configured");
	}
	if (!rsxgl_programs_bound)
	{
		rsxLoadFragmentProgramLocation(rsxgl_context, fragment_program,
			fragment_offset, GCM_LOCATION_RSX);
		if (rsxgl_filter_program_active)
		{
			rsxSetFragmentProgramParameter(rsxgl_context, rsxgl_filter_fp,
				rsxgl_filter_params0_parameter, rsxgl_filter_params0,
				rsxgl_filter_fp_offset, GCM_LOCATION_RSX);
			rsxSetFragmentProgramParameter(rsxgl_context, rsxgl_filter_fp,
				rsxgl_filter_params1_parameter, rsxgl_filter_params1,
				rsxgl_filter_fp_offset, GCM_LOCATION_RSX);
		}
		rsxgl_programs_bound = 1;
	}
	if (!rsxgl_traced_first_draw)
	{
		PS3_BOOT_TRACE("RSX renderer: first fragment program configured");
	}
	if (rsxgl_filter_program_active)
	{
		/* The mask is procedural and deliberately has no texture bandwidth. */
	}
	else if (rsxgl_lightmap_program_active)
	{
		rsxgl_apply_lightmap_textures();
	}
	else
	{
		rsxgl_apply_texture();
	}
	if (!rsxgl_traced_first_draw)
	{
		PS3_BOOT_TRACE("RSX renderer: first texture configured");
	}
	return 1;
}

/* Console and menu composition commonly submits many adjacent inline runs
 * without changing shader, matrices, texture, or texture contents. Avoid
 * rebuilding the full preparation stack in that exact state. Every mutation
 * which could invalidate the shortcut already clears one of these cached
 * invariants; the general path remains the recovery and transition path. */
static int
rsxgl_prepare_inline_2d_state(void)
{
	if (rsxgl_initialized && rsxgl_frame_ready &&
		!rsxgl_lightmap_program_active && !rsxgl_filter_program_active &&
		rsxgl_programs_bound && rsxgl_mvp_valid &&
		rsxgl_uploaded_matrix_revision == rsxgl_matrix_revision &&
		!rsxgl_sampler_dirty && !rsxgl_texture_cache_dirty)
	{
		if (rsxgl_texture_enabled)
		{
			rsxgl_note_texture_sample(rsxgl_bound_texture);
		}
		if (rsxgl_stats_enabled)
		{
			rsxgl_frame_ui_state_hits++;
		}
		if (!rsxgl_traced_inline_2d_state_cache)
		{
			PS3_RUNTIME_TRACE(
				"RSX renderer: inline 2D prepared-state fast path active");
			rsxgl_traced_inline_2d_state_cache = 1;
		}
		return 1;
	}
	/* A changed program, matrix, or sampler cannot be emitted inside an open
	 * Begin/End stream. This is a defensive close; normal mutators already use
	 * the shared batch boundary before making the predicate false. */
	rsxgl_end_inline_2d_batch();
	return rsxgl_prepare_draw_state();
}

static int
rsxgl_prepare_vertices(rsxgl_vertex_t *gpu_vertices, int count)
{
	u32 offset;

	if (!gpu_vertices || count <= 0 || !rsxgl_prepare_draw_state())
	{
		return 0;
	}

	if (!rsxgl_arena_offset(gpu_vertices, rsxgl_vertex_arena,
		rsxgl_vertex_arena_size, rsxgl_vertex_arena_offset, &offset))
	{
		PS3_BOOT_TRACE("RSX renderer: vertex arena offset lookup failed");
		return 0;
	}
	rsxgl_bind_vertex_arrays(offset, rsxgl_stream_location);
	if (!rsxgl_traced_first_draw)
	{
		PS3_BOOT_TRACE("RSX renderer: first vertex arrays bound");
	}
	return 1;
}

static int
rsxgl_stage_ui_vertices(rsxgl_vertex_t *cpu_vertices, int count)
{
	size_t bytes;
	size_t destination_used;
	u32 source_offset;
	u32 destination_offset;

	if (rsxgl_ui_stream_direct_local ||
		!rsxgl_2d_mode || !rsxgl_ui_stream_selected ||
		!rsxgl_ui_stream_main_mapped || !rsxgl_ui_local_vertex_arena ||
		count <= 0)
	{
		return 0;
	}
	bytes = (size_t)count * sizeof(*cpu_vertices);
	if (!bytes || bytes > RSXGL_UI_VERTEX_ARENA_SIZE || bytes > UINT32_MAX)
	{
		return 0;
	}
	if (!rsxgl_arena_offset(cpu_vertices, rsxgl_ui_vertex_arena,
		rsxgl_ui_vertex_arena_size, rsxgl_ui_vertex_arena_offset,
		&source_offset))
	{
		PS3_RUNTIME_TRACE("RSX renderer: UI staging source offset lookup failed");
		return 0;
	}

	destination_used = (rsxgl_ui_local_vertex_arena_used + 127) &
		~((size_t)127);
	if (destination_used + bytes > rsxgl_ui_local_vertex_arena_size)
	{
		/* BeginFrame reserves an eight-MiB worst-case budget in the rotating
		 * destination, so this is exceptional. Complete earlier UI draws before
		 * recycling the local destination instead of fetching direct from XDR. */
		rsxgl_finish();
		destination_used = 0;
	}
	destination_offset = rsxgl_ui_local_vertex_arena_offset +
		(u32)destination_used;

	/* The transfer and following draw share one RSX command stream. Evict the
	 * completed CPU range, enqueue its copy to local video memory, invalidate
	 * any prior vertex-cache generation, then bind only the local destination. */
	rsxgl_flush_ppu_cache_range(cpu_vertices, bytes);
	rsxSetTransferData(rsxgl_context, GCM_TRANSFER_MAIN_TO_LOCAL,
		destination_offset, (u32)bytes, source_offset, (u32)bytes,
		(u32)bytes, 1);
	rsxInvalidateVertexCache(rsxgl_context);
	rsxgl_bind_vertex_arrays(destination_offset, GCM_LOCATION_RSX);
	rsxgl_ui_local_vertex_arena_used = destination_used + bytes;
	if (!rsxgl_traced_ui_local_stage)
	{
		PS3_RUNTIME_TRACE("RSX renderer: GPU-local staged 2D vertex stream active");
		rsxgl_traced_ui_local_stage = 1;
	}
	return 1;
}

static void
rsxgl_note_gpu_draw_submitted(void)
{
	rsxgl_frame_gpu_draws++;
	if (!rsxgl_traced_first_draw)
	{
		PS3_BOOT_TRACE("RSX renderer: first GPU draw submitted");
		rsxgl_traced_first_draw = 1;
	}
}

static int
rsxgl_reserve_command_words(u32 words)
{
	if (!rsxgl_context || !words)
	{
		return 0;
	}
	if (rsxgl_context->current + words > rsxgl_context->end &&
		rsxContextCallback(rsxgl_context, words) != 0)
	{
		PS3_RUNTIME_TRACE("RSX renderer: inline 2D command reservation failed");
		return 0;
	}
	return rsxgl_context->current + words <= rsxgl_context->end;
}

/* Direct 2D entry points bypass the generic glDrawArrays guard. Keep them
 * strictly inside the acquired-frame lifetime so a loading/menu callback can
 * never append glyphs to the surface which has already been queued for scanout. */
static int
rsxgl_2d_submission_ready(void)
{
	if (rsxgl_initialized && rsxgl_frame_active && rsxgl_frame_ready)
	{
		return 1;
	}
	if (rsxgl_initialized && !rsxgl_traced_out_of_frame_2d)
	{
		PS3_RUNTIME_TRACE(
			"RSX renderer: rejected 2D submission outside acquired frame");
		rsxgl_traced_out_of_frame_2d = 1;
	}
	return 0;
}

static void
rsxgl_reset_ui_string_signatures(void)
{
	if (rsxgl_stats_enabled)
	{
		memset(rsxgl_ui_string_signature_valid, 0,
			sizeof(rsxgl_ui_string_signature_valid));
	}
}

static uint64_t
rsxgl_hash_ui_word(uint64_t hash, uint32_t value)
{
	int byte_index;
	for (byte_index = 0; byte_index < 4; byte_index++)
	{
		hash ^= (unsigned char)(value >> (byte_index * 8));
		hash *= UINT64_C(1099511628211);
	}
	return hash;
}

/* Record exact same-eye string calls only while profiling is requested. This
 * is diagnostic rather than a heuristic draw suppression: a physical trace
 * can now distinguish duplicate engine submission from stale presentation
 * without changing legitimate overdraw or menu behavior. */
static void
rsxgl_note_ui_string_signature(float x, float y,
	const unsigned char *text, int count, float scale, int xor_mask,
	int atlas_width, int atlas_height)
{
	uint64_t hash = UINT64_C(1469598103934665603);
	uint32_t x_bits;
	uint32_t y_bits;
	uint32_t scale_bits;
	unsigned int probe;
	int i;

	if (!rsxgl_stats_enabled || !text || count <= 0)
	{
		return;
	}
	memcpy(&x_bits, &x, sizeof(x_bits));
	memcpy(&y_bits, &y, sizeof(y_bits));
	memcpy(&scale_bits, &scale, sizeof(scale_bits));
	hash = rsxgl_hash_ui_word(hash, x_bits);
	hash = rsxgl_hash_ui_word(hash, y_bits);
	hash = rsxgl_hash_ui_word(hash, scale_bits);
	hash = rsxgl_hash_ui_word(hash, (uint32_t)count);
	hash = rsxgl_hash_ui_word(hash, (uint32_t)xor_mask);
	hash = rsxgl_hash_ui_word(hash, (uint32_t)atlas_width);
	hash = rsxgl_hash_ui_word(hash, (uint32_t)atlas_height);
	hash = rsxgl_hash_ui_word(hash, (uint32_t)rsxgl_bound_texture);
	for (i = 0; i < count; i++)
	{
		hash ^= text[i];
		hash *= UINT64_C(1099511628211);
	}

	probe = (unsigned int)hash & (RSXGL_UI_SIGNATURE_BUCKETS - 1);
	for (i = 0; i < RSXGL_UI_SIGNATURE_BUCKETS; i++)
	{
		unsigned int bucket = (probe + (unsigned int)i) &
			(RSXGL_UI_SIGNATURE_BUCKETS - 1);
		if (!rsxgl_ui_string_signature_valid[bucket])
		{
			rsxgl_ui_string_signature_valid[bucket] = 1;
			rsxgl_ui_string_signatures[bucket] = hash;
			return;
		}
		if (rsxgl_ui_string_signatures[bucket] == hash)
		{
			rsxgl_frame_ui_duplicate_strings++;
			if (!rsxgl_traced_ui_duplicate_string)
			{
				PS3_RUNTIME_TRACE(
					"RSX stats: same-eye duplicate UI string signature observed");
				rsxgl_traced_ui_duplicate_string = 1;
			}
			return;
		}
	}
}

/* Attribute zero (POSITION) commits an immediate vertex on NV4x. Keep it
 * last so each vertex observes the UV written immediately before it and the
 * string-wide COLOR0 value. The aligned scratch arrays also satisfy librsx's
 * paired-load implementation on the PPU. */
static inline void
rsxgl_emit_inline_2d_vertex(float x, float y, float s, float t,
	float position[4], float texcoord[4])
{
	if (rsxgl_direct_2d_clip)
	{
		x = x * rsxgl_direct_2d_scale_x - 1.0f;
		y = 1.0f - y * rsxgl_direct_2d_scale_y;
	}
	texcoord[0] = s;
	texcoord[1] = t;
	position[0] = x;
	position[1] = y;
	position[2] = 0.0f;
	rsxDrawVertex2fUnsafe(rsxgl_context, GCM_VERTEX_ATTRIB_TEX0, texcoord);
	rsxDrawVertex3fUnsafe(rsxgl_context, GCM_VERTEX_ATTRIB_POS, position);
}

static void
rsxgl_end_inline_2d_batch(void)
{
	if (!rsxgl_inline_2d_batch_active)
	{
		return;
	}
	/* Every append reserved these two words before it emitted any vertex data,
	 * so STOP is safe even when the command buffer is otherwise full. */
	if (rsxgl_context && rsxgl_inline_2d_batch_vertices > 0)
	{
		rsxDrawVertexEndUnsafe(rsxgl_context);
	}
	rsxgl_inline_2d_batch_active = 0;
	rsxgl_inline_2d_batch_vertices = 0;
	rsxgl_inline_2d_batch_color_valid = 0;
}

/* Reserve and open (or extend) one generic 2D triangle stream. No unrelated
 * RSX method may be emitted while it is active; every state and stream change
 * already routes through rsxgl_flush_triangle_batch(), which closes it. If a
 * command-buffer boundary is near, close first, let the normal callback move
 * to fresh space, and open a new bounded stream. */
static int
rsxgl_begin_inline_2d_batch(const float input_color[4], u32 vertex_words,
	unsigned int vertices)
{
	float color[4] __attribute__((aligned(16)));
	u32 required;
	int color_changed;
	int merged;

	if (!rsxgl_context || !input_color || !vertex_words || !vertices)
	{
		return 0;
	}
	memcpy(color, input_color, sizeof(color));
	color_changed = !rsxgl_inline_2d_batch_color_valid ||
		memcmp(rsxgl_inline_2d_batch_color, color, sizeof(color)) != 0;
	required = vertex_words + RSXGL_INLINE_END_WORDS +
		(rsxgl_inline_2d_batch_active ? 0 : RSXGL_INLINE_BEGIN_WORDS) +
		(color_changed ? RSXGL_INLINE_COLOR_WORDS : 0);

	if (rsxgl_inline_2d_batch_active &&
		(rsxgl_inline_2d_batch_vertices + vertices >
			RSXGL_IMMEDIATE_VERTICES ||
		rsxgl_context->current + required > rsxgl_context->end))
	{
		rsxgl_end_inline_2d_batch();
		color_changed = 1;
		required = RSXGL_INLINE_BEGIN_WORDS + RSXGL_INLINE_COLOR_WORDS +
			vertex_words + RSXGL_INLINE_END_WORDS;
	}
	if (!rsxgl_reserve_command_words(required))
	{
		return 0;
	}

	merged = rsxgl_inline_2d_batch_active;
	if (!rsxgl_inline_2d_batch_active)
	{
		rsxDrawVertexBeginUnsafe(rsxgl_context, GCM_TYPE_TRIANGLES);
		rsxgl_inline_2d_batch_active = 1;
		rsxgl_inline_2d_batch_color_valid = 0;
		color_changed = 1;
		rsxgl_frame_ui_batch_starts++;
		rsxgl_note_gpu_draw_submitted();
	}
	else
	{
		rsxgl_frame_ui_batch_merges++;
	}
	if (color_changed)
	{
		rsxDrawVertex4fUnsafe(rsxgl_context, GCM_VERTEX_ATTRIB_COLOR0,
			color);
		memcpy(rsxgl_inline_2d_batch_color, color, sizeof(color));
		rsxgl_inline_2d_batch_color_valid = 1;
	}
	rsxgl_inline_2d_batch_vertices += vertices;
	if (merged && !rsxgl_traced_inline_2d_batch)
	{
		PS3_RUNTIME_TRACE(
			"RSX renderer: adjacent command-inline 2D draws share one primitive batch");
		rsxgl_traced_inline_2d_batch = 1;
	}
	return 1;
}

static int
rsxgl_append_inline_2d_quad(float x0, float y0, float x1, float y1,
	float s0, float t0, float s1, float t1, const float input_color[4])
{
	float position[4] __attribute__((aligned(16))) = {0, 0, 0, 1};
	float texcoord[4] __attribute__((aligned(16))) = {0, 0, 0, 0};
	u32 *vertex_start;

	if (!rsxgl_begin_inline_2d_batch(input_color,
		RSXGL_INLINE_TEXT_WORDS_PER_GLYPH, 6))
	{
		return 0;
	}
	vertex_start = rsxgl_context->current;
	rsxgl_emit_inline_2d_vertex(x0, y0, s0, t0, position, texcoord);
	rsxgl_emit_inline_2d_vertex(x1, y0, s1, t0, position, texcoord);
	rsxgl_emit_inline_2d_vertex(x1, y1, s1, t1, position, texcoord);
	rsxgl_emit_inline_2d_vertex(x0, y0, s0, t0, position, texcoord);
	rsxgl_emit_inline_2d_vertex(x1, y1, s1, t1, position, texcoord);
	rsxgl_emit_inline_2d_vertex(x0, y1, s0, t1, position, texcoord);
	if ((u32)(rsxgl_context->current - vertex_start) !=
		RSXGL_INLINE_TEXT_WORDS_PER_GLYPH &&
		!rsxgl_traced_inline_quad_word_mismatch)
	{
		PS3_RUNTIME_TRACE(
			"RSX renderer: batched inline 2D quad command-size invariant failed");
		rsxgl_traced_inline_quad_word_mismatch = 1;
	}
	return 1;
}

/* State and shader selection are owned by the caller. Presentation filters use
 * this isolated six-vertex sequence because their procedural fragment program
 * is always a hard boundary; ordinary UI uses the retained batch above. */
static int
rsxgl_submit_inline_2d_quad(float x0, float y0, float x1, float y1,
	float s0, float t0, float s1, float t1, const float input_color[4])
{
	float position[4] __attribute__((aligned(16))) = {0, 0, 0, 1};
	float texcoord[4] __attribute__((aligned(16))) = {0, 0, 0, 0};
	float color[4] __attribute__((aligned(16)));
	u32 *command_start;

	if (!rsxgl_reserve_command_words(RSXGL_INLINE_QUAD_WORDS))
	{
		return 0;
	}
	memcpy(color, input_color, sizeof(color));
	command_start = rsxgl_context->current;
	rsxDrawVertexBeginUnsafe(rsxgl_context, GCM_TYPE_TRIANGLES);
	rsxDrawVertex4fUnsafe(rsxgl_context, GCM_VERTEX_ATTRIB_COLOR0, color);
	rsxgl_emit_inline_2d_vertex(x0, y0, s0, t0, position, texcoord);
	rsxgl_emit_inline_2d_vertex(x1, y0, s1, t0, position, texcoord);
	rsxgl_emit_inline_2d_vertex(x1, y1, s1, t1, position, texcoord);
	rsxgl_emit_inline_2d_vertex(x0, y0, s0, t0, position, texcoord);
	rsxgl_emit_inline_2d_vertex(x1, y1, s1, t1, position, texcoord);
	rsxgl_emit_inline_2d_vertex(x0, y1, s0, t1, position, texcoord);
	rsxDrawVertexEndUnsafe(rsxgl_context);
	if ((u32)(rsxgl_context->current - command_start) !=
		RSXGL_INLINE_QUAD_WORDS && !rsxgl_traced_inline_quad_word_mismatch)
	{
		PS3_RUNTIME_TRACE(
			"RSX renderer: inline 2D quad command-size invariant failed");
		rsxgl_traced_inline_quad_word_mismatch = 1;
	}
	rsxgl_note_gpu_draw_submitted();
	return 1;
}

static void
rsxgl_publish_dynamic_vertices(void)
{
	/* Every draw needs an ordering barrier after its CPU writes. The complete
	 * vertex-cache invalidate is required only when an arena generation starts:
	 * addresses append monotonically until the next frame or fenced wrap, so no
	 * later submission in that generation can alias an earlier cached fetch. */
	__sync_synchronize();
	if (!rsxgl_vertex_cache_needs_invalidate && !rsxgl_2d_mode)
	{
		return;
	}

	rsxInvalidateVertexCache(rsxgl_context);
	rsxgl_vertex_cache_needs_invalidate = 0;
	if (!rsxgl_traced_vertex_cache_sync)
	{
		PS3_RUNTIME_TRACE("RSX renderer: dynamic vertex-cache synchronization active");
		rsxgl_traced_vertex_cache_sync = 1;
	}
}

static void
rsxgl_submit_vertices(GLenum mode, rsxgl_vertex_t *gpu_vertices, int count)
{
	if (!rsxgl_prepare_vertices(gpu_vertices, count))
	{
		return;
	}
	if (rsxgl_stage_ui_vertices(gpu_vertices, count))
	{
		rsxDrawVertexArray(rsxgl_context, rsxgl_primitive(mode), 0, count);
		rsxgl_note_gpu_draw_submitted();
		return;
	}
	if (rsxgl_stream_location == GCM_LOCATION_CELL)
	{
		/* The RSX I/O map does not make dirty PPU cache lines visible by itself.
		 * Publish the completed stream before the draw can consume it. */
		if (rsxgl_2d_mode && rsxgl_ui_stream_selected)
		{
			rsxgl_flush_ppu_cache_range(gpu_vertices,
				(size_t)count * sizeof(*gpu_vertices));
		}
		else
		{
			rsxgl_store_ppu_cache_range(gpu_vertices,
				(size_t)count * sizeof(*gpu_vertices));
		}
	}
	/* Physical RSX retains vertex fetches across recycled local-memory addresses;
	 * RPCS3 does not model that cache closely enough to expose stale glyphs. */
	rsxgl_publish_dynamic_vertices();
	rsxDrawVertexArray(rsxgl_context, rsxgl_primitive(mode), 0, count);
	rsxgl_note_gpu_draw_submitted();
}

/* PSL1GHT's hardware debug-font renderer deliberately feeds position, UV, and
 * color from three tightly packed arrays. Keep that proven layout for native
 * Quake II text as well: unlike world geometry, a console row repeats the same
 * interleaved 40-byte fetch hundreds of times and is the one path on physical
 * RSX which has continued to show coherent-but-wrong glyph geometry. */
static int
rsxgl_submit_planar_text_vertices(float *positions, float *texcoords,
	float *colors, int count)
{
	u32 position_offset;
	u32 texcoord_offset;
	u32 color_offset;

	if (!positions || !texcoords || !colors || count <= 0 ||
		!rsxgl_prepare_draw_state())
	{
		return 0;
	}
	if (!rsxgl_arena_offset(positions, rsxgl_vertex_arena,
		rsxgl_vertex_arena_size, rsxgl_vertex_arena_offset,
		&position_offset) ||
		!rsxgl_arena_offset(texcoords, rsxgl_vertex_arena,
		rsxgl_vertex_arena_size, rsxgl_vertex_arena_offset,
		&texcoord_offset) ||
		!rsxgl_arena_offset(colors, rsxgl_vertex_arena,
		rsxgl_vertex_arena_size, rsxgl_vertex_arena_offset,
		&color_offset))
	{
		PS3_RUNTIME_TRACE("RSX renderer: planar text offset lookup failed");
		return 0;
	}

	rsxBindVertexArrayAttrib(rsxgl_context, GCM_VERTEX_ATTRIB_POS, 0,
		position_offset, sizeof(float) * 3, 3, GCM_VERTEX_DATA_TYPE_F32,
		GCM_LOCATION_RSX);
	rsxBindVertexArrayAttrib(rsxgl_context, GCM_VERTEX_ATTRIB_TEX0, 0,
		texcoord_offset, sizeof(float) * 2, 2, GCM_VERTEX_DATA_TYPE_F32,
		GCM_LOCATION_RSX);
	rsxBindVertexArrayAttrib(rsxgl_context, GCM_VERTEX_ATTRIB_COLOR0, 0,
		color_offset, sizeof(float) * 4, 4, GCM_VERTEX_DATA_TYPE_F32,
		GCM_LOCATION_RSX);
	rsxgl_publish_dynamic_vertices();
	rsxDrawVertexArray(rsxgl_context, GCM_TYPE_TRIANGLES, 0, count);
	rsxgl_note_gpu_draw_submitted();
	return 1;
}

static void
rsxgl_submit_indexed_vertices(rsxgl_vertex_t *gpu_vertices, int vertex_count,
	u16 *gpu_indices, int index_count)
{
	u32 index_offset;

	if (!gpu_indices || index_count <= 0)
	{
		return;
	}
	if (!rsxgl_arena_offset(gpu_indices, rsxgl_index_arena,
		RSXGL_INDEX_ARENA_SIZE, rsxgl_index_arena_offset, &index_offset))
	{
		PS3_RUNTIME_TRACE("RSX renderer: index arena offset lookup failed");
		return;
	}
	if (!rsxgl_prepare_vertices(gpu_vertices, vertex_count))
	{
		return;
	}
	if (rsxgl_stream_location == GCM_LOCATION_CELL)
	{
		/* Publish both arrays before invalidating RSX's prior cached view. */
		rsxgl_store_ppu_cache_range(gpu_vertices,
			(size_t)vertex_count * sizeof(*gpu_vertices));
		rsxgl_store_ppu_cache_range(gpu_indices,
			(size_t)index_count * sizeof(*gpu_indices));
	}
	/* This is required for GCM_LOCATION_RSX too: the local-memory arena is
	 * rewritten by the PPU and recycled from offset zero after each flip. */
	rsxgl_publish_dynamic_vertices();
	rsxDrawIndexArray(rsxgl_context, GCM_TYPE_TRIANGLES, index_offset,
		index_count, GCM_INDEX_TYPE_16B, rsxgl_stream_location);
	rsxgl_note_gpu_draw_submitted();
}

static int
rsxgl_triangle_vertex_count(GLenum mode, int count)
{
	if (!rsxgl_triangle_batch_enabled || count < 3)
	{
		return 0;
	}

	switch (mode)
	{
		case GL_TRIANGLES:
			return count - (count % 3);
		case GL_TRIANGLE_FAN:
		case GL_TRIANGLE_STRIP:
			return (count - 2) * 3;
		case GL_QUADS:
			return (count / 4) * 6;
		default:
			return 0;
	}
}

static int
rsxgl_triangle_source_index(GLenum mode, int output_index)
{
	static const unsigned char quad_indices[6] = {0, 1, 2, 0, 2, 3};
	int triangle;
	int corner;

	if (mode == GL_TRIANGLES)
	{
		return output_index;
	}
	if (mode == GL_QUADS)
	{
		return (output_index / 6) * 4 + quad_indices[output_index % 6];
	}

	triangle = output_index / 3;
	corner = output_index % 3;
	if (mode == GL_TRIANGLE_FAN)
	{
		return corner == 0 ? 0 : triangle + corner;
	}

	/* Match the alternating winding defined by GL_TRIANGLE_STRIP. */
	if ((triangle & 1) == 0)
	{
		return triangle + corner;
	}
	return corner == 0 ? triangle + 1 :
		(corner == 1 ? triangle : triangle + 2);
}

static int
rsxgl_triangle_source_vertex_count(GLenum mode, int count)
{
	if (count < 3)
	{
		return 0;
	}
	switch (mode)
	{
		case GL_TRIANGLES:
			return count - (count % 3);
		case GL_TRIANGLE_FAN:
		case GL_TRIANGLE_STRIP:
			return count;
		case GL_QUADS:
			return (count / 4) * 4;
		default:
			return 0;
	}
}

static int
rsxgl_should_index_triangle_batch(GLenum mode, int source_vertices)
{
	/* A triangle list already streams each vertex exactly once, so an index
	 * buffer would add bandwidth without reducing vertex traffic. Fans, strips,
	 * and quads reuse vertices and benefit directly from 16-bit indices. */
	return rsxgl_indexed_batch_enabled && !rsxgl_2d_mode &&
		mode != GL_TRIANGLES &&
		source_vertices > 0 &&
		source_vertices <= RSXGL_INDEXED_BATCH_MAX_VERTICES;
}

static rsxgl_vertex_t *
rsxgl_reserve_triangle_batch(int count)
{
	size_t bytes;
	rsxgl_vertex_t *result;

	if (!rsxgl_initialized || !rsxgl_frame_ready || count <= 0)
	{
		return NULL;
	}
	if (rsxgl_static_batch_index_count > 0)
	{
		rsxgl_flush_triangle_batch();
	}
	/* Expanded and indexed geometry cannot share one hardware submission. */
	if (rsxgl_triangle_batch_index_count > 0)
	{
		rsxgl_flush_triangle_batch();
	}
	bytes = (size_t)count * sizeof(rsxgl_vertex_t);
	if (bytes > rsxgl_vertex_arena_size)
	{
		return NULL;
	}

	if (rsxgl_triangle_batch_vertices == 0)
	{
		rsxgl_vertex_arena_used = (rsxgl_vertex_arena_used + 127) &
			~((size_t)127);
	}
	if (rsxgl_vertex_arena_used + bytes > rsxgl_vertex_arena_size)
	{
		rsxgl_flush_triangle_batch();
		/* The previous batch is now queued. Fence before reusing the arena from
		 * offset zero; ordinary frames reset it after the display flip instead. */
		if (rsxgl_vertex_arena_used + bytes > rsxgl_vertex_arena_size)
		{
			rsxgl_finish();
			rsxgl_vertex_arena_used = 0;
			rsxgl_vertex_cache_needs_invalidate = 1;
		}
	}

	result = (rsxgl_vertex_t *)((uint8_t *)rsxgl_vertex_arena +
		rsxgl_vertex_arena_used);
	if (rsxgl_triangle_batch_vertices == 0)
	{
		rsxgl_triangle_batch = result;
	}
	rsxgl_vertex_arena_used += bytes;
	rsxgl_triangle_batch_vertices += count;
	return result;
}

static rsxgl_vertex_t *
rsxgl_reserve_indexed_triangle_batch(int vertex_count, int index_count,
	u16 **indices, int *base_vertex)
{
	size_t vertex_bytes;
	size_t index_bytes;
	rsxgl_vertex_t *vertices;
	u16 *reserved_indices;

	if (indices) *indices = NULL;
	if (base_vertex) *base_vertex = 0;
	if (!rsxgl_initialized || !rsxgl_frame_ready || vertex_count <= 0 ||
		index_count <= 0 ||
		vertex_count > RSXGL_INDEXED_BATCH_MAX_VERTICES)
	{
		return NULL;
	}
	if (rsxgl_static_batch_index_count > 0)
	{
		rsxgl_flush_triangle_batch();
	}
	vertex_bytes = (size_t)vertex_count * sizeof(rsxgl_vertex_t);
	index_bytes = (size_t)index_count * sizeof(u16);
	if (vertex_bytes > rsxgl_vertex_arena_size ||
		index_bytes > RSXGL_INDEX_ARENA_SIZE)
	{
		return NULL;
	}

	/* Keep triangle lists on the simpler non-indexed stream. */
	if (rsxgl_triangle_batch_vertices > 0 &&
		rsxgl_triangle_batch_index_count == 0)
	{
		rsxgl_flush_triangle_batch();
	}
	/* A 16-bit index addresses at most 65,535 vertices from the batch base. */
	if (rsxgl_triangle_batch_vertices + vertex_count >
		RSXGL_INDEXED_BATCH_MAX_VERTICES)
	{
		rsxgl_flush_triangle_batch();
	}
	if (rsxgl_triangle_batch_vertices == 0)
	{
		rsxgl_vertex_arena_used = (rsxgl_vertex_arena_used + 127) &
			~((size_t)127);
		rsxgl_index_arena_used = (rsxgl_index_arena_used + 127) &
			~((size_t)127);
	}

	if (rsxgl_vertex_arena_used + vertex_bytes > rsxgl_vertex_arena_size ||
		rsxgl_index_arena_used + index_bytes > RSXGL_INDEX_ARENA_SIZE)
	{
		rsxgl_flush_triangle_batch();
		if (rsxgl_vertex_arena_used + vertex_bytes > rsxgl_vertex_arena_size ||
			rsxgl_index_arena_used + index_bytes > RSXGL_INDEX_ARENA_SIZE)
		{
			/* Both arenas may contain queued submissions. Fence before reusing
			 * either one from offset zero. */
			rsxgl_finish();
			rsxgl_vertex_arena_used = 0;
			rsxgl_index_arena_used = 0;
			rsxgl_vertex_cache_needs_invalidate = 1;
		}
	}

	vertices = (rsxgl_vertex_t *)((uint8_t *)rsxgl_vertex_arena +
		rsxgl_vertex_arena_used);
	reserved_indices = (u16 *)((uint8_t *)rsxgl_index_arena +
		rsxgl_index_arena_used);
	if (rsxgl_triangle_batch_vertices == 0)
	{
		rsxgl_triangle_batch = vertices;
		rsxgl_triangle_batch_indices = reserved_indices;
	}
	if (base_vertex) *base_vertex = rsxgl_triangle_batch_vertices;
	if (indices) *indices = reserved_indices;
	rsxgl_vertex_arena_used += vertex_bytes;
	rsxgl_index_arena_used += index_bytes;
	rsxgl_triangle_batch_vertices += vertex_count;
	rsxgl_triangle_batch_index_count += index_count;
	return vertices;
}

static void
rsxgl_note_triangle_draw(int indexed)
{
	rsxgl_triangle_batch_draws++;
	if (indexed && !rsxgl_traced_indexed_triangle_batch)
	{
		PS3_RUNTIME_TRACE("RSX renderer: indexed triangle batching active");
		rsxgl_traced_indexed_triangle_batch = 1;
	}
	if (rsxgl_triangle_batch_draws > 1 && !rsxgl_traced_triangle_batch)
	{
		PS3_RUNTIME_TRACE("RSX renderer: compatible triangle batching active");
		rsxgl_traced_triangle_batch = 1;
	}
}

static void
rsxgl_flush_static_triangle_batch(void)
{
	u16 *indices = rsxgl_static_batch_indices;
	int index_count = rsxgl_static_batch_index_count;
	u32 vertex_offset = rsxgl_static_batch_vertex_offset;
	u32 index_offset;

	if (!indices || index_count <= 0)
	{
		return;
	}
	/* Clear ownership before state preparation so future recovery changes cannot
	 * resubmit this persistent batch recursively. */
	rsxgl_static_batch_indices = NULL;
	rsxgl_static_batch_index_count = 0;
	rsxgl_static_batch_draws = 0;
	if (!rsxgl_arena_offset(indices, rsxgl_index_arena,
		RSXGL_INDEX_ARENA_SIZE, rsxgl_index_arena_offset, &index_offset))
	{
		PS3_RUNTIME_TRACE("RSX renderer: static BSP index offset lookup failed");
		return;
	}
	if (!rsxgl_prepare_draw_state())
	{
		return;
	}
	if (rsxgl_world_stream_location == GCM_LOCATION_CELL)
	{
		rsxgl_store_ppu_cache_range(indices,
			(size_t)index_count * sizeof(*indices));
	}
	rsxgl_publish_dynamic_vertices();
	rsxgl_bind_vertex_arrays(vertex_offset, GCM_LOCATION_RSX);
	rsxDrawIndexArray(rsxgl_context, GCM_TYPE_TRIANGLES, index_offset,
		index_count, GCM_INDEX_TYPE_16B, rsxgl_world_stream_location);
	rsxgl_frame_batch_flushes++;
	rsxgl_note_gpu_draw_submitted();
}

static void
rsxgl_flush_addressable_triangle_batch(void)
{
	rsxgl_vertex_t *vertices = rsxgl_triangle_batch;
	u16 *indices = rsxgl_triangle_batch_indices;
	int count = rsxgl_triangle_batch_vertices;
	int index_count = rsxgl_triangle_batch_index_count;

	if (rsxgl_static_batch_index_count > 0)
	{
		/* Static index submission emits ordinary draw methods and therefore
		 * cannot occur inside a retained immediate-mode UI stream. */
		rsxgl_end_inline_2d_batch();
		rsxgl_flush_static_triangle_batch();
	}
	if (!vertices || count <= 0)
	{
		return;
	}
	rsxgl_frame_batch_flushes++;
	/* Clear first so a failure or a future fence inside submission cannot
	 * recursively resubmit the same geometry. */
	rsxgl_triangle_batch = NULL;
	rsxgl_triangle_batch_indices = NULL;
	rsxgl_triangle_batch_vertices = 0;
	rsxgl_triangle_batch_index_count = 0;
	rsxgl_triangle_batch_draws = 0;
	if (indices && index_count > 0)
	{
		rsxgl_submit_indexed_vertices(vertices, count, indices, index_count);
	}
	else
	{
		rsxgl_submit_vertices(GL_TRIANGLES, vertices, count);
	}
	if (rsxgl_vertex_arena_used < rsxgl_vertex_arena_size)
	{
		size_t aligned = (rsxgl_vertex_arena_used + 127) &
			~((size_t)127);
		rsxgl_vertex_arena_used = aligned <= rsxgl_vertex_arena_size ?
			aligned : rsxgl_vertex_arena_size;
	}
	if (rsxgl_index_arena_used < RSXGL_INDEX_ARENA_SIZE)
	{
		size_t aligned = (rsxgl_index_arena_used + 127) &
			~((size_t)127);
		rsxgl_index_arena_used = aligned <= RSXGL_INDEX_ARENA_SIZE ?
			aligned : RSXGL_INDEX_ARENA_SIZE;
	}
}

/* This is the universal renderer ordering boundary. State setters, matrix and
 * viewport changes, texture transitions, stream switches, clears, filters,
 * frame/eye changes, and shutdown all route through it. */
static void
rsxgl_flush_triangle_batch(void)
{
	rsxgl_end_inline_2d_batch();
	rsxgl_flush_addressable_triangle_batch();
}

static void
rsxgl_save_active_vertex_cursor(void)
{
	if (rsxgl_ui_stream_selected)
	{
		if (rsxgl_ui_stream_direct_local)
		{
			rsxgl_ui_local_vertex_arena_used = rsxgl_vertex_arena_used;
		}
		else
		{
			rsxgl_ui_vertex_arena_used = rsxgl_vertex_arena_used;
		}
	}
	else
	{
		rsxgl_world_vertex_arena_used = rsxgl_vertex_arena_used;
	}
}

static void
rsxgl_select_vertex_stream(int ui)
{
	int select_ui = ui && (rsxgl_ui_stream_direct_local ?
		rsxgl_ui_local_vertex_arena != NULL :
		rsxgl_ui_stream_main_mapped);

	if (select_ui == rsxgl_ui_stream_selected)
	{
		return;
	}

	/* A draw cannot span address spaces. Preserve the used position in each
	 * arena so the 2D -> world -> 2D transitions made by stereo and loading
	 * screens remain ordered without recycling queued data. */
	rsxgl_flush_triangle_batch();
	rsxgl_save_active_vertex_cursor();

	if (select_ui)
	{
		if (rsxgl_ui_stream_direct_local)
		{
			rsxgl_vertex_arena = rsxgl_ui_local_vertex_arena;
			rsxgl_vertex_arena_offset = rsxgl_ui_local_vertex_arena_offset;
			rsxgl_vertex_arena_size = rsxgl_ui_local_vertex_arena_size;
			rsxgl_vertex_arena_used = rsxgl_ui_local_vertex_arena_used;
			rsxgl_stream_location = GCM_LOCATION_RSX;
		}
		else
		{
			rsxgl_vertex_arena = rsxgl_ui_vertex_arena;
			rsxgl_vertex_arena_offset = rsxgl_ui_vertex_arena_offset;
			rsxgl_vertex_arena_size = rsxgl_ui_vertex_arena_size;
			rsxgl_vertex_arena_used = rsxgl_ui_vertex_arena_used;
			rsxgl_stream_location = GCM_LOCATION_CELL;
		}
	}
	else
	{
		rsxgl_vertex_arena = rsxgl_world_vertex_arena;
		rsxgl_vertex_arena_offset = rsxgl_world_vertex_arena_offset;
		rsxgl_vertex_arena_size = RSXGL_VERTEX_ARENA_SIZE;
		rsxgl_vertex_arena_used = rsxgl_world_vertex_arena_used;
		rsxgl_stream_location = rsxgl_world_stream_location;
	}
	rsxgl_ui_stream_selected = select_ui;
	rsxgl_vertex_cache_needs_invalidate = 1;
}

static void
rsxgl_draw_vertices(GLenum mode, const rsxgl_vertex_t *vertices, int count)
{
	rsxgl_vertex_t *gpu_vertices;
	u16 *gpu_indices;
	int triangle_vertices;
	int source_vertices;
	int base_vertex;
	int i;
	if (!vertices || count <= 0) return;
	rsxgl_set_lightmap_program(0);
	triangle_vertices = rsxgl_triangle_vertex_count(mode, count);
	if (triangle_vertices > 0)
	{
		source_vertices = rsxgl_triangle_source_vertex_count(mode, count);
		if (rsxgl_should_index_triangle_batch(mode, source_vertices))
		{
			gpu_vertices = rsxgl_reserve_indexed_triangle_batch(
				source_vertices, triangle_vertices, &gpu_indices, &base_vertex);
			if (!gpu_vertices) return;
			memcpy(gpu_vertices, vertices,
				source_vertices * sizeof(*vertices));
			for (i = 0; i < triangle_vertices; i++)
			{
				gpu_indices[i] = (u16)(base_vertex +
					rsxgl_triangle_source_index(mode, i));
			}
			rsxgl_note_api_draw(source_vertices, triangle_vertices, 1);
			rsxgl_note_triangle_draw(1);
			return;
		}
		gpu_vertices = rsxgl_reserve_triangle_batch(triangle_vertices);
		if (!gpu_vertices) return;
		for (i = 0; i < triangle_vertices; i++)
		{
			gpu_vertices[i] = vertices[
				rsxgl_triangle_source_index(mode, i)];
		}
		rsxgl_note_api_draw(triangle_vertices, 0, 1);
		rsxgl_note_triangle_draw(0);
		return;
	}
	gpu_vertices = rsxgl_allocate_vertices(count);
	if (!gpu_vertices) return;
	memcpy(gpu_vertices, vertices, count * sizeof(*vertices));
	rsxgl_note_api_draw(count, 0, 0);
	rsxgl_submit_vertices(mode, gpu_vertices, count);
}

static inline __attribute__((always_inline)) void
rsxgl_resolve_vertex_color(const float color[4], float resolved[4])
{
	resolved[0] = color[0];
	resolved[1] = color[1];
	resolved[2] = color[2];
	resolved[3] = color[3];

	/* Texture-environment changes are batch boundaries, so finalize effective
	 * color during the initial pack rather than rewriting the whole batch. */
	if (rsxgl_texture_enabled && rsxgl_texture_env == GL_REPLACE)
	{
		resolved[0] = resolved[1] = resolved[2] = resolved[3] = 1.0f;
	}
	else if (rsxgl_texture_enabled &&
		rsxgl_texture_env == RSXGL_TEXTURE_ENV_COMBINE &&
		rsxgl_rgb_scale != 1.0f)
	{
		resolved[0] *= rsxgl_rgb_scale;
		resolved[1] *= rsxgl_rgb_scale;
		resolved[2] *= rsxgl_rgb_scale;
	}
}

static void
rsxgl_store_resolved_vertex(rsxgl_vertex_t *destination,
	float x, float y, float z, float s, float t, const float color[4])
{
	/* RSX local memory is CPU-visible, but use explicit 32-bit volatile writes
	 * so the compiler cannot turn this stream into alignment-sensitive paired
	 * integer stores. */
	volatile float *output = (volatile float *)destination;
	if (rsxgl_direct_2d_clip)
	{
		x = x * rsxgl_direct_2d_scale_x - 1.0f;
		y = 1.0f - y * rsxgl_direct_2d_scale_y;
		z = 0.0f;
	}
	output[0] = x;
	output[1] = y;
	output[2] = z;
	output[3] = 1.0f;
	output[4] = s;
	output[5] = t;
	output[6] = color[0];
	output[7] = color[1];
	output[8] = color[2];
	output[9] = color[3];
}

static void
rsxgl_store_planar_text_vertex(float *positions, float *texcoords,
	float *colors, int index, float x, float y, float s, float t,
	const float color[4])
{
	volatile float *position = (volatile float *)&positions[index * 3];
	volatile float *texcoord = (volatile float *)&texcoords[index * 2];
	volatile float *vertex_color = (volatile float *)&colors[index * 4];

	if (rsxgl_direct_2d_clip)
	{
		x = x * rsxgl_direct_2d_scale_x - 1.0f;
		y = 1.0f - y * rsxgl_direct_2d_scale_y;
	}
	position[0] = x;
	position[1] = y;
	position[2] = 0.0f;
	texcoord[0] = s;
	texcoord[1] = t;
	vertex_color[0] = color[0];
	vertex_color[1] = color[1];
	vertex_color[2] = color[2];
	vertex_color[3] = color[3];
}

static void
rsxgl_store_lightmapped_vertex(rsxgl_vertex_t *destination,
	float x, float y, float z, float s, float t, float ls, float lt,
	const float color[4])
{
	volatile float *output = (volatile float *)destination;
	output[0] = x;
	output[1] = y;
	output[2] = z;
	output[3] = 1.0f;
	output[4] = s;
	output[5] = t;
	/* Combined world draws reuse COLOR0 for the second UV pair. This
	 * keeps the proven 40-byte v1.47 vertex layout unchanged for console/menu
	 * geometry while still feeding both samplers in the lightmap program. The
	 * overbright scale is a shader uniform, so persistent vertices remain valid
	 * when the setting changes. */
	output[6] = ls;
	output[7] = lt;
	output[8] = color[0];
	output[9] = 1.0f;
}

void
RSXGL_ResetStaticWorldGeometry(void)
{
	if (!rsxgl_initialized)
	{
		return;
	}
	/* A map transition is rare and may free the source BSP immediately. Finish
	 * only when the prior generation actually populated persistent geometry,
	 * then reuse the fixed local arena without per-map allocations. */
	if (rsxgl_static_vertex_arena_used)
	{
		rsxgl_flush_triangle_batch();
		rsxgl_finish();
	}
	rsxgl_static_vertex_generation++;
	if (!rsxgl_static_vertex_generation)
	{
		rsxgl_static_vertex_generation = 1;
	}
	rsxgl_static_vertex_arena_used = 0;
	rsxgl_static_vertex_page_offset = 0;
	rsxgl_static_vertex_page_used = 0;
	rsxgl_static_registered_vertices = 0;
	rsxgl_static_geometry_dirty = 0;
	rsxgl_static_batch_indices = NULL;
	rsxgl_static_batch_vertex_offset = 0;
	rsxgl_static_batch_index_count = 0;
	rsxgl_static_batch_draws = 0;
}

int
RSXGL_RegisterStaticLightmappedFan(const float *positions,
	int position_stride_floats, const float *texcoords,
	int texcoord_stride_floats, const float *lightcoords,
	int lightcoord_stride_floats, int count, unsigned int *generation,
	unsigned int *vertex_offset, unsigned short *first_vertex)
{
	static const float white[4] = {1, 1, 1, 1};
	rsxgl_vertex_t *destination;
	size_t bytes;
	int i;

	if (generation) *generation = 0;
	if (vertex_offset) *vertex_offset = 0;
	if (first_vertex) *first_vertex = 0;
	if (!rsxgl_initialized || !rsxgl_static_vertex_arena ||
		!rsxgl_cvar_static_world || rsxgl_cvar_static_world->value == 0.0f ||
		!positions || !texcoords || !lightcoords || !generation ||
		!vertex_offset || !first_vertex || position_stride_floats < 3 ||
		texcoord_stride_floats < 2 || lightcoord_stride_floats < 2 ||
		count < 3 || count > RSXGL_STATIC_VERTEX_PAGE_VERTICES)
	{
		return 0;
	}

	bytes = (size_t)count * sizeof(rsxgl_vertex_t);
	if (rsxgl_static_vertex_page_used + (unsigned int)count >
		RSXGL_STATIC_VERTEX_PAGE_VERTICES)
	{
		rsxgl_static_vertex_arena_used =
			(rsxgl_static_vertex_arena_used + 127) & ~((size_t)127);
		rsxgl_static_vertex_page_offset = rsxgl_static_vertex_arena_used;
		rsxgl_static_vertex_page_used = 0;
	}
	if (rsxgl_static_vertex_arena_used + bytes >
		RSXGL_STATIC_VERTEX_ARENA_SIZE)
	{
		return 0;
	}

	destination = (rsxgl_vertex_t *)((uint8_t *)rsxgl_static_vertex_arena +
		rsxgl_static_vertex_arena_used);
	*generation = rsxgl_static_vertex_generation;
	*vertex_offset = rsxgl_static_vertex_arena_offset +
		(u32)rsxgl_static_vertex_page_offset;
	*first_vertex = (unsigned short)rsxgl_static_vertex_page_used;
	for (i = 0; i < count; i++)
	{
		const float *position = positions + i * position_stride_floats;
		const float *texcoord = texcoords + i * texcoord_stride_floats;
		const float *lightcoord = lightcoords + i * lightcoord_stride_floats;
		rsxgl_store_lightmapped_vertex(&destination[i],
			position[0], position[1], position[2], texcoord[0], texcoord[1],
			lightcoord[0], lightcoord[1], white);
	}
	rsxgl_static_vertex_arena_used += bytes;
	rsxgl_static_vertex_page_used += (unsigned int)count;
	rsxgl_static_registered_vertices += (unsigned int)count;
	rsxgl_static_geometry_dirty = 1;
	return 1;
}

void
RSXGL_FinalizeStaticWorldGeometry(void)
{
	char line[160];

	if (!rsxgl_initialized || !rsxgl_static_geometry_dirty)
	{
		return;
	}
	/* Registration writes through the local aperture before any draw can use
	 * the arena. Publish once and invalidate one prior vertex generation;
	 * displayed frames thereafter perform no persistent-vertex writes. */
	__sync_synchronize();
	rsxInvalidateVertexCache(rsxgl_context);
	rsxgl_static_geometry_dirty = 0;
	Com_sprintf(line, sizeof(line),
		"RSX renderer: static BSP arena registered %u vertices (%u KiB)",
		rsxgl_static_registered_vertices,
		(unsigned int)((rsxgl_static_vertex_arena_used + 1023) / 1024));
	PS3_RUNTIME_TRACE(line);
}

static void
rsxgl_store_vertex(rsxgl_vertex_t *destination, float x, float y, float z,
	float s, float t, const float color[4])
{
	float resolved[4];

	rsxgl_resolve_vertex_color(color, resolved);
	rsxgl_store_resolved_vertex(destination, x, y, z, s, t, resolved);
}

/* Console/menu pictures, backgrounds, fills, fades, and cinematic quads share
 * the same command-inline lifetime as glyphs. Keeping these rectangles out of
 * the rotating addressable 2D arena prevents a safe text row from being
 * composited over a stale window or background generation. */
void
RSXGL_DrawTexturedQuad2D(float x0, float y0, float x1, float y1,
	float s0, float t0, float s1, float t1)
{
	float color[4];

	if (!rsxgl_2d_submission_ready())
	{
		return;
	}

	/* GL clips these later, but rejecting a completely invisible UI rectangle
	 * here avoids writing, publishing, copying, and fetching six vertices. */
	if ((x0 <= x1 ? x1 : x0) <= 0.0f ||
		(y0 <= y1 ? y1 : y0) <= 0.0f ||
		(x0 <= x1 ? x0 : x1) >= (float)rsxgl_virtual_width ||
		(y0 <= y1 ? y0 : y1) >= (float)rsxgl_virtual_height)
	{
		return;
	}

	rsxgl_set_lightmap_program(0);
	/* Preserve ordering against addressable geometry, but keep an already-open
	 * command-inline UI stream alive when all renderer state is unchanged. */
	rsxgl_flush_addressable_triangle_batch();
	if (!rsxgl_prepare_inline_2d_state())
	{
		return;
	}

	rsxgl_resolve_vertex_color(rsxgl_current_color, color);
	if (!rsxgl_append_inline_2d_quad(x0, y0, x1, y1,
		s0, t0, s1, t1, color))
	{
		return;
	}
	rsxgl_note_api_draw(6, 0, 1);
	if (!rsxgl_traced_inline_quad)
	{
		PS3_RUNTIME_TRACE(
			"RSX renderer: command-buffer inline 2D quads active");
		rsxgl_traced_inline_quad = 1;
	}
}

int
RSXGL_TiledTargetsActive(void)
{
	/* Tile bindings are removed again when setup falls back, so this reports
	 * the effective renderer layout rather than merely the requested cvar. */
	return rsxgl_initialized && rsxgl_bound_tile_count > 0;
}

int
RSXGL_ResolveLinearSurface(uint32_t source_offset, uint32_t source_pitch,
	int source_width, int source_height, int source_x, int source_y,
	int source_width_rect, int source_height_rect, float destination_x,
	float destination_y, float destination_width, float destination_height)
{
	const float white[4] = {1.0f, 1.0f, 1.0f, 1.0f};
	gcmTexture texture;
	float s0;
	float t0;
	float s1;
	float t1;
	int restore_alpha;
	int restore_scissor;
	int restore_texture;
	int submitted = 0;
	u8 unit;

	if (!rsxgl_initialized || !rsxgl_frame_ready || !source_pitch ||
		source_width <= 0 || source_height <= 0 ||
		source_width > UINT16_MAX || source_height > UINT16_MAX ||
		source_x < 0 || source_y < 0 || source_width_rect < 1 ||
		source_height_rect < 1 ||
		source_x + source_width_rect > source_width ||
		source_y + source_height_rect > source_height ||
		destination_width <= 0.0f || destination_height <= 0.0f ||
		!rsxgl_texture_parameter)
	{
		return 0;
	}

	/* R_SetGL2D has already ended the world and selected the ordinary fixed
	 * program. Reassert only the states required by this opaque resolve, then
	 * restore the UI-facing alpha/texture/scissor choices below. */
	restore_alpha = rsxgl_alpha_test_enabled;
	restore_scissor = rsxgl_scissor_enabled;
	restore_texture = rsxgl_texture_enabled;
	rsxgl_set_lightmap_program(0);
	rsxgl_set_filter_program(0);
	glViewport(0, 0, rsxgl_virtual_width, rsxgl_virtual_height);
	RSXGL_Set2DProjection(rsxgl_virtual_width, rsxgl_virtual_height);
	glDisable(GL_SCISSOR_TEST);
	glDisable(GL_DEPTH_TEST);
	glDisable(GL_CULL_FACE);
	glDisable(GL_BLEND);
	glDisable(GL_ALPHA_TEST);
	glEnable(GL_TEXTURE_2D);
	glColor4f(1.0f, 1.0f, 1.0f, 1.0f);

	/* Prepare the ordinary program and identity MVP before replacing only its
	 * sampler descriptor. A subsequent UI draw sees sampler_dirty and restores
	 * Quake's bound atlas or picture; no external texture enters that table. */
	if (!rsxgl_prepare_draw_state())
	{
		goto restore_state;
	}
	memset(&texture, 0, sizeof(texture));
	texture.format = GCM_TEXTURE_FORMAT_A8R8G8B8 | GCM_TEXTURE_FORMAT_LIN;
	texture.mipmap = 1;
	texture.dimension = GCM_TEXTURE_DIMS_2D;
	texture.width = (u16)source_width;
	texture.height = (u16)source_height;
	texture.depth = 1;
	texture.location = GCM_LOCATION_RSX;
	texture.pitch = source_pitch;
	texture.offset = source_offset;
	texture.remap =
		(GCM_TEXTURE_REMAP_TYPE_REMAP << GCM_TEXTURE_REMAP_TYPE_B_SHIFT) |
		(GCM_TEXTURE_REMAP_TYPE_REMAP << GCM_TEXTURE_REMAP_TYPE_G_SHIFT) |
		(GCM_TEXTURE_REMAP_TYPE_REMAP << GCM_TEXTURE_REMAP_TYPE_R_SHIFT) |
		(GCM_TEXTURE_REMAP_TYPE_REMAP << GCM_TEXTURE_REMAP_TYPE_A_SHIFT) |
		(GCM_TEXTURE_REMAP_COLOR_B << GCM_TEXTURE_REMAP_COLOR_B_SHIFT) |
		(GCM_TEXTURE_REMAP_COLOR_G << GCM_TEXTURE_REMAP_COLOR_G_SHIFT) |
		(GCM_TEXTURE_REMAP_COLOR_R << GCM_TEXTURE_REMAP_COLOR_R_SHIFT) |
		(GCM_TEXTURE_REMAP_COLOR_A << GCM_TEXTURE_REMAP_COLOR_A_SHIFT);
	unit = rsxgl_texture_parameter->index;
	/* The producer and consumer are ordered in one RSX FIFO. Invalidating the
	 * texture cache is the render-target-to-texture transition; unlike a PPU
	 * readback, it does not require stalling for GPU completion. */
	rsxInvalidateTextureCache(rsxgl_context, GCM_INVALIDATE_TEXTURE);
	rsxgl_texture_cache_dirty = 0;
	rsxLoadTexture(rsxgl_context, unit, &texture);
	rsxTextureControl(rsxgl_context, unit, GCM_TRUE, 0, 0,
		GCM_TEXTURE_MAX_ANISO_1);
	rsxTextureFilter(rsxgl_context, unit, 0, GCM_TEXTURE_LINEAR,
		GCM_TEXTURE_LINEAR, GCM_TEXTURE_CONVOLUTION_QUINCUNX);
	rsxTextureWrapMode(rsxgl_context, unit, GCM_TEXTURE_CLAMP_TO_EDGE,
		GCM_TEXTURE_CLAMP_TO_EDGE, GCM_TEXTURE_CLAMP_TO_EDGE, 0,
		GCM_TEXTURE_ZFUNC_LESS, 0);

	/* Stay half a texel inside the shaded rectangle. The surrounding private
	 * surface is intentionally uncleared, so sampling its border would produce
	 * a stale one-pixel seam under linear enlargement. */
	s0 = ((float)source_x + 0.5f) / (float)source_width;
	t0 = ((float)source_y + 0.5f) / (float)source_height;
	s1 = ((float)(source_x + source_width_rect) - 0.5f) /
		(float)source_width;
	t1 = ((float)(source_y + source_height_rect) - 0.5f) /
		(float)source_height;
	if (rsxgl_submit_inline_2d_quad(destination_x, destination_y,
		destination_x + destination_width,
		destination_y + destination_height, s0, t0, s1, t1, white))
	{
		rsxgl_note_api_draw(6, 0, 1);
		submitted = 1;
	}
	rsxgl_sampler_dirty = 1;

restore_state:
	if (!restore_texture) glDisable(GL_TEXTURE_2D);
	if (restore_alpha) glEnable(GL_ALPHA_TEST);
	if (restore_scissor) glEnable(GL_SCISSOR_TEST);
	return submitted;
}

void
RSXGL_ApplyDisplayFilter(void)
{
	const float white[4] = {1.0f, 1.0f, 1.0f, 1.0f};
	float strength;
	float scanline = 0.0f;
	float grille = 0.0f;
	float vignette = 0.0f;
	int mode;

	if (!rsxgl_initialized || !rsxgl_frame_ready || !rsxgl_cvar_filter)
	{
		return;
	}
	mode = (int)rsxgl_cvar_filter->value;
	if (mode <= 0 || mode > 5)
	{
		return;
	}
	strength = rsxgl_cvar_filter_strength ?
		rsxgl_cvar_filter_strength->value : 1.0f;
	if (strength < 0.0f) strength = 0.0f;
	if (strength > 2.0f) strength = 2.0f;
	if (strength == 0.0f)
	{
		return;
	}

	switch (mode)
	{
		case 1:
			scanline = 0.18f * strength;
			break;
		case 2:
			grille = 0.12f * strength;
			break;
		case 3:
			vignette = 0.30f * strength;
			break;
		case 4:
			scanline = 0.12f * strength;
			grille = 0.06f * strength;
			vignette = 0.22f * strength;
			break;
		case 5:
			scanline = 0.25f * strength;
			grille = 0.13f * strength;
			vignette = 0.42f * strength;
			break;
	}
	if (scanline > 0.85f) scanline = 0.85f;
	if (grille > 0.75f) grille = 0.75f;
	if (vignette > 0.85f) vignette = 0.85f;

	rsxgl_filter_params0[0] = (float)rsxgl_output_width;
	rsxgl_filter_params0[1] = (float)rsxgl_target_height;
	rsxgl_filter_params0[2] = scanline;
	rsxgl_filter_params0[3] = grille;
	rsxgl_filter_params1[0] = vignette;
	rsxgl_filter_params1[1] = 0.0f;
	rsxgl_filter_params1[2] = 0.0f;
	rsxgl_filter_params1[3] = 0.0f;

	/* Resolve the complete scene, HUD, console, and menu through one native
	 * eye-sized quad. No offscreen surface or texture sample is involved. */
	glViewport(0, 0, rsxgl_virtual_width, rsxgl_virtual_height);
	RSXGL_Set2DProjection(rsxgl_virtual_width, rsxgl_virtual_height);
	/* The cached projection may be a complete no-op. Close v1.96's retained UI
	 * primitive explicitly before any filter state or program method. */
	rsxgl_flush_triangle_batch();
	glDisable(GL_SCISSOR_TEST);
	glDisable(GL_DEPTH_TEST);
	glDisable(GL_CULL_FACE);
	glDisable(GL_ALPHA_TEST);
	glEnable(GL_BLEND);
	glBlendFunc(GL_DST_COLOR, GL_ZERO);

	rsxgl_set_filter_program(1);
	if (rsxgl_prepare_draw_state() &&
		rsxgl_submit_inline_2d_quad(0.0f, 0.0f,
			(float)rsxgl_virtual_width, (float)rsxgl_virtual_height,
			0.0f, 0.0f, 1.0f, 1.0f, white))
	{
		rsxgl_note_api_draw(6, 0, 1);
	}
	rsxgl_set_filter_program(0);

	/* Leave the compatibility state expected by the next eye/frame. */
	glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
	glDisable(GL_BLEND);
	glEnable(GL_ALPHA_TEST);
	glColor4f(1.0f, 1.0f, 1.0f, 1.0f);
	if (!rsxgl_traced_display_filter)
	{
		PS3_RUNTIME_TRACE("RSX renderer: native per-eye display filter active");
		rsxgl_traced_display_filter = 1;
	}
}

/* Submit a complete console/menu string through the flip-fenced rotating UI
 * vertex ring. Earlier command-inline text avoided address reuse, but a full
 * console redraw emits tens of thousands of immediate method words and real
 * RSX hardware can replay/corrupt that long Begin/End stream. One contiguous
 * addressable triangle list per string keeps the geometry immutable until the
 * display flip fence, uses fresh GPU-local addresses across frames, and leaves
 * the command buffer small. */
void
RSXGL_DrawTexturedGlyphs2D(float x, float y, const unsigned char *text,
	int count, float scale, int xor_mask, int atlas_width, int atlas_height)
{
	float color[4];
	float glyph_size;
	float inset_s;
	float inset_t;
	const float cell = 1.0f / 16.0f;
	rsxgl_vertex_t *vertices;
	float *positions = NULL;
	float *texcoords = NULL;
	float *colors = NULL;
	size_t position_bytes;
	size_t texcoord_bytes;
	size_t color_bytes;
	size_t texcoord_start;
	size_t color_start;
	size_t planar_bytes;
	int planar_text;
	int visible = 0;
	int vertex_count;
	int cursor;
	int emitted = 0;

	if (!rsxgl_2d_submission_ready() || !text || count <= 0 ||
		count > RSXGL_IMMEDIATE_VERTICES ||
		scale <= 0.0f || atlas_width <= 0 || atlas_height <= 0)
	{
		return;
	}

	glyph_size = 8.0f * scale;
	if (y + glyph_size <= 0.0f || y >= (float)rsxgl_virtual_height ||
		x >= (float)rsxgl_virtual_width || x + count * glyph_size <= 0.0f)
	{
		return;
	}

	xor_mask &= 255;
	inset_s = 0.5f / atlas_width;
	inset_t = 0.5f / atlas_height;

	/* Count first so spaces and fully clipped glyphs consume neither local
	 * memory nor RSX vertices. The public count is already bounded above. */
	for (cursor = 0; cursor < count; cursor++)
	{
		int glyph = text[cursor] ^ xor_mask;
		float glyph_x = x + cursor * glyph_size;
		if ((glyph & 127) != 32 && glyph_x + glyph_size > 0.0f &&
			glyph_x < (float)rsxgl_virtual_width)
		{
			visible++;
		}
	}
	if (!visible)
	{
		return;
	}

	vertex_count = visible * 6;
	rsxgl_set_lightmap_program(0);
	/* Close any preceding inline picture/background and every pending array
	 * batch before selecting immutable storage for this complete string. */
	rsxgl_flush_triangle_batch();
	planar_text = rsxgl_ui_stream_direct_local && rsxgl_ui_stream_selected &&
		rsxgl_stream_location == GCM_LOCATION_RSX;
	vertices = NULL;
	if (planar_text)
	{
		position_bytes = (size_t)vertex_count * sizeof(float) * 3;
		texcoord_bytes = (size_t)vertex_count * sizeof(float) * 2;
		color_bytes = (size_t)vertex_count * sizeof(float) * 4;
		texcoord_start = (position_bytes + 127) & ~((size_t)127);
		color_start = (texcoord_start + texcoord_bytes + 127) &
			~((size_t)127);
		planar_bytes = (color_start + color_bytes + 127) & ~((size_t)127);
		vertices = rsxgl_allocate_vertices((int)((planar_bytes +
			sizeof(*vertices) - 1) / sizeof(*vertices)));
		if (vertices)
		{
			positions = (float *)vertices;
			texcoords = (float *)((uint8_t *)vertices + texcoord_start);
			colors = (float *)((uint8_t *)vertices + color_start);
		}
	}
	else
	{
		vertices = rsxgl_allocate_vertices(vertex_count);
	}
	if (!vertices)
	{
		return;
	}
	rsxgl_resolve_vertex_color(rsxgl_current_color, color);

	for (cursor = 0; cursor < count; cursor++)
	{
		int glyph = text[cursor] ^ xor_mask;
		float x0 = x + cursor * glyph_size;
		float x1;
		float y1;
		float s0;
		float s1;
		float t0;
		float t1;
		int base_vertex;

		if ((glyph & 127) == 32 || x0 + glyph_size <= 0.0f ||
			x0 >= (float)rsxgl_virtual_width)
		{
			continue;
		}
		x1 = x0 + glyph_size;
		y1 = y + glyph_size;
		s0 = (glyph & 15) * cell + inset_s;
		s1 = (glyph & 15) * cell + cell - inset_s;
		t0 = (glyph >> 4) * cell + inset_t;
		t1 = (glyph >> 4) * cell + cell - inset_t;
		base_vertex = emitted * 6;
		if (planar_text)
		{
			rsxgl_store_planar_text_vertex(positions, texcoords, colors,
				base_vertex + 0, x0, y, s0, t0, color);
			rsxgl_store_planar_text_vertex(positions, texcoords, colors,
				base_vertex + 1, x1, y, s1, t0, color);
			rsxgl_store_planar_text_vertex(positions, texcoords, colors,
				base_vertex + 2, x1, y1, s1, t1, color);
			rsxgl_store_planar_text_vertex(positions, texcoords, colors,
				base_vertex + 3, x0, y, s0, t0, color);
			rsxgl_store_planar_text_vertex(positions, texcoords, colors,
				base_vertex + 4, x1, y1, s1, t1, color);
			rsxgl_store_planar_text_vertex(positions, texcoords, colors,
				base_vertex + 5, x0, y1, s0, t1, color);
		}
		else
		{
			rsxgl_vertex_t *quad = &vertices[base_vertex];
			rsxgl_store_resolved_vertex(&quad[0], x0, y, 0.0f, s0, t0, color);
			rsxgl_store_resolved_vertex(&quad[1], x1, y, 0.0f, s1, t0, color);
			rsxgl_store_resolved_vertex(&quad[2], x1, y1, 0.0f, s1, t1, color);
			rsxgl_store_resolved_vertex(&quad[3], x0, y, 0.0f, s0, t0, color);
			rsxgl_store_resolved_vertex(&quad[4], x1, y1, 0.0f, s1, t1, color);
			rsxgl_store_resolved_vertex(&quad[5], x0, y1, 0.0f, s0, t1, color);
		}
		emitted++;
	}

	rsxgl_note_api_draw(vertex_count, 0, 1);
	if (planar_text)
	{
		rsxgl_submit_planar_text_vertices(positions, texcoords, colors,
			vertex_count);
	}
	else
	{
		rsxgl_submit_vertices(GL_TRIANGLES, vertices, vertex_count);
	}
	rsxgl_note_ui_string_signature(x, y, text, count, scale,
		xor_mask, atlas_width, atlas_height);
	if (rsxgl_frame_ready)
	{
		rsxgl_frame_ui_strings++;
		rsxgl_frame_ui_glyphs += emitted;
	}
	if (!rsxgl_traced_text_geometry && count >= 4)
	{
		char line[256];
		Com_sprintf(line, sizeof(line),
			"RSX text geometry: xy %.2f %.2f count %d visible %d scale %.3f glyph %.3f virtual %dx%d viewport %d,%d %dx%d proj %.7f %.7f %.3f %.3f",
			x, y, count, visible, scale, glyph_size,
			rsxgl_virtual_width, rsxgl_virtual_height,
			rsxgl_viewport_x, rsxgl_viewport_y,
			rsxgl_viewport_width, rsxgl_viewport_height,
			rsxgl_projection[rsxgl_projection_top][0],
			rsxgl_projection[rsxgl_projection_top][5],
			rsxgl_projection[rsxgl_projection_top][12],
			rsxgl_projection[rsxgl_projection_top][13]);
		PS3_RUNTIME_TRACE(line);
		rsxgl_traced_text_geometry = 1;
	}
	if (planar_text && !rsxgl_traced_planar_text)
	{
		PS3_RUNTIME_TRACE(
			"RSX renderer: tightly packed planar GPU-local text stream active");
		rsxgl_traced_planar_text = 1;
	}
	if (!planar_text && !rsxgl_traced_inline_text)
	{
		PS3_RUNTIME_TRACE(
			"RSX renderer: interleaved addressable text fallback active");
		rsxgl_traced_inline_text = 1;
	}
}

void
RSXGL_DrawTexturedGlyph2D(float x, float y, unsigned int glyph, float scale,
	int atlas_width, int atlas_height)
{
	const unsigned char value = (unsigned char)(glyph & 255);
	RSXGL_DrawTexturedGlyphs2D(x, y, &value, 1, scale, 0,
		atlas_width, atlas_height);
}

/* Typed native path for BSP, lightmap, and warp fans. These account for most
 * GL1 calls in a normal frame. Their source format is already known to be
 * aligned float3 positions plus float2 texture coordinates, so avoid repeated
 * client-state toggles, byte-stride validation, and generic array dispatch for
 * every visible surface. Geometry and recovery behavior remain identical to
 * glDrawArrays(GL_TRIANGLE_FAN): indexed batching is used when enabled,
 * expanded batching otherwise, and the original hardware fan when batching is
 * disabled. */
void
RSXGL_DrawTexturedFan(const float *positions, int position_stride_floats,
	const float *texcoords, int texcoord_stride_floats, int count)
{
	rsxgl_vertex_t *vertices;
	u16 *indices = NULL;
	int output_count;
	int base_vertex = 0;
	int batching;
	int indexed;
	int pack_count;
	int i;
	float color[4];

	if (!positions || !texcoords || position_stride_floats < 3 ||
		texcoord_stride_floats < 2 || count < 3)
	{
		return;
	}
	rsxgl_set_lightmap_program(0);

	/* This entry point only accepts a valid triangle fan. Specialize the two
	 * generic primitive decisions here: it is called for every visible BSP,
	 * lightmap, water, and sky polygon, so even one switch/function call per fan
	 * is material PPU work. */
	batching = rsxgl_triangle_batch_enabled;
	output_count = batching ? (count - 2) * 3 : count;
	indexed = batching && rsxgl_indexed_batch_enabled && !rsxgl_2d_mode &&
		count <= RSXGL_INDEXED_BATCH_MAX_VERTICES;
	pack_count = indexed ? count : output_count;
	vertices = indexed ? rsxgl_reserve_indexed_triangle_batch(
		count, output_count, &indices, &base_vertex) :
		(batching ? rsxgl_reserve_triangle_batch(output_count) :
			rsxgl_allocate_vertices(output_count));
	if (!vertices)
	{
		return;
	}

	rsxgl_resolve_vertex_color(rsxgl_current_color, color);
	for (i = 0; i < pack_count; i++)
	{
		int source_index = batching && !indexed ?
			rsxgl_triangle_source_index(GL_TRIANGLE_FAN, i) : i;
		const float *position = positions +
			source_index * position_stride_floats;
		const float *texcoord = texcoords +
			source_index * texcoord_stride_floats;
		rsxgl_store_resolved_vertex(&vertices[i], position[0], position[1],
			position[2], texcoord[0], texcoord[1], color);
	}

	if (indexed)
	{
		int triangle;
		int output = 0;

		/* A fan's triangles are (0, n+1, n+2). Emit them sequentially
		 * instead of calling the generic mode decoder for every index. */
		for (triangle = 0; triangle < count - 2; triangle++)
		{
			indices[output++] = (u16)base_vertex;
			indices[output++] = (u16)(base_vertex + triangle + 1);
			indices[output++] = (u16)(base_vertex + triangle + 2);
		}
		rsxgl_note_api_draw(count, output_count, 1);
	}
	else
	{
		rsxgl_note_api_draw(output_count, 0, batching);
	}
	if (batching)
	{
		rsxgl_note_triangle_draw(indexed);
	}
	else
	{
		rsxgl_submit_vertices(GL_TRIANGLE_FAN, vertices, count);
	}

	if (rsxgl_frame_ready)
	{
		rsxgl_frame_fast_array_vertices += pack_count;
	}
}

/* Native GL1 particles are already expanded into a triangle list. Their
 * source layout is fixed: three float3 positions and float2 coordinates plus
 * one shared float4 color per particle. Bypass the retained client-array
 * state, generic stride/type validation, source-index dispatch, and redundant
 * per-corner color resolution while preserving the same final 40-byte vertex
 * layout, batching boundaries, matrix, texture, blend, and depth state. */
void
RSXGL_DrawParticleTriangles(const float *positions, const float *texcoords,
	const float *colors, int particle_count)
{
	rsxgl_vertex_t *vertices;
	int vertex_count;
	int batching;
	int particle;

	if (!positions || !texcoords || !colors || particle_count <= 0 ||
		particle_count > INT_MAX / 3)
	{
		return;
	}
	vertex_count = particle_count * 3;
	rsxgl_set_lightmap_program(0);
	batching = rsxgl_triangle_batch_enabled;
	vertices = batching ? rsxgl_reserve_triangle_batch(vertex_count) :
		rsxgl_allocate_vertices(vertex_count);
	if (!vertices)
	{
		return;
	}

	for (particle = 0; particle < particle_count; particle++)
	{
		const float *particle_positions = positions + particle * 9;
		const float *particle_texcoords = texcoords + particle * 6;
		const float *source_color = colors + particle * 4;
		float color[4];
		int corner;

		rsxgl_resolve_vertex_color(source_color, color);
		for (corner = 0; corner < 3; corner++)
		{
			const float *position = particle_positions + corner * 3;
			const float *texcoord = particle_texcoords + corner * 2;
			rsxgl_store_resolved_vertex(&vertices[particle * 3 + corner],
				position[0], position[1], position[2],
				texcoord[0], texcoord[1], color);
		}
	}

	rsxgl_note_api_draw(vertex_count, 0, batching);
	if (batching)
	{
		rsxgl_note_triangle_draw(0);
	}
	else
	{
		rsxgl_submit_vertices(GL_TRIANGLES, vertices, vertex_count);
	}
	if (rsxgl_frame_ready)
	{
		rsxgl_frame_fast_array_vertices += vertex_count;
	}
	if (!rsxgl_traced_typed_particles)
	{
		PS3_RUNTIME_TRACE("RSX renderer: typed particle submission active");
		rsxgl_traced_typed_particles = 1;
	}
}

/* Native GL1 alias models arrive as fixed float3/float2/float4 fans and
 * strips. They are among the most fragmented primitive streams in a Quake II
 * frame, especially for enemies, pickups, the weapon model, and the paired
 * stereo eye. Pack that known layout directly instead of repeatedly mutating
 * retained client-array state and rediscovering its type, stride, alignment,
 * and enabled fields in generic glDrawArrays. The shadow variant deliberately
 * shares this path with current texture coordinates and color, matching a
 * vertex-only client array while avoiding another generic dispatch. */
static void
rsxgl_draw_alias_primitive(GLenum mode, const float *positions,
	const float *texcoords, const float *colors, int count)
{
	rsxgl_vertex_t *vertices;
	u16 *indices = NULL;
	int output_count;
	int pack_count;
	int base_vertex = 0;
	int batching;
	int indexed;
	int i;
	float current_resolved[4] = {1.0f, 1.0f, 1.0f, 1.0f};

	if (!positions || count < 3 ||
		(mode != GL_TRIANGLE_FAN && mode != GL_TRIANGLE_STRIP))
	{
		return;
	}
	rsxgl_set_lightmap_program(0);
	batching = rsxgl_triangle_batch_enabled;
	output_count = batching ? (count - 2) * 3 : count;
	indexed = batching && rsxgl_indexed_batch_enabled && !rsxgl_2d_mode &&
		count <= RSXGL_INDEXED_BATCH_MAX_VERTICES;
	pack_count = indexed ? count : output_count;
	vertices = indexed ? rsxgl_reserve_indexed_triangle_batch(
		count, output_count, &indices, &base_vertex) :
		(batching ? rsxgl_reserve_triangle_batch(output_count) :
			rsxgl_allocate_vertices(output_count));
	if (!vertices)
	{
		return;
	}

	if (!colors)
	{
		rsxgl_resolve_vertex_color(rsxgl_current_color, current_resolved);
	}
	for (i = 0; i < pack_count; i++)
	{
		int source_index = batching && !indexed ?
			rsxgl_triangle_source_index(mode, i) : i;
		const float *position = positions + source_index * 3;
		/* Colored shells intentionally leave the GL command texture coordinates
		 * unwritten while texturing is disabled. Never read those bytes: the
		 * current coordinate is the exact effective fixed-function input. */
		const float *texcoord = texcoords && rsxgl_texture_enabled ?
			texcoords + source_index * 2 : rsxgl_current_texcoord;

		if (colors)
		{
			rsxgl_store_vertex(&vertices[i], position[0], position[1],
				position[2], texcoord[0], texcoord[1],
				colors + source_index * 4);
		}
		else
		{
			rsxgl_store_resolved_vertex(&vertices[i], position[0], position[1],
				position[2], texcoord[0], texcoord[1], current_resolved);
		}
	}

	if (indexed)
	{
		for (i = 0; i < output_count; i++)
		{
			indices[i] = (u16)(base_vertex +
				rsxgl_triangle_source_index(mode, i));
		}
		rsxgl_note_api_draw(count, output_count, 1);
	}
	else
	{
		rsxgl_note_api_draw(output_count, 0, batching);
	}
	if (batching)
	{
		rsxgl_note_triangle_draw(indexed);
	}
	else
	{
		rsxgl_submit_vertices(mode, vertices, count);
	}
	if (rsxgl_frame_ready)
	{
		rsxgl_frame_fast_array_vertices += pack_count;
	}
	if (!rsxgl_traced_typed_alias)
	{
		PS3_RUNTIME_TRACE("RSX renderer: typed alias-model submission active");
		rsxgl_traced_typed_alias = 1;
	}
}

void
RSXGL_DrawAliasPrimitive(GLenum mode, const float *positions,
	const float *texcoords, const float *colors, int count)
{
	if (!texcoords || !colors)
	{
		return;
	}
	rsxgl_draw_alias_primitive(mode, positions, texcoords, colors, count);
}

void
RSXGL_DrawAliasShadowPrimitive(GLenum mode, const float *positions, int count)
{
	rsxgl_draw_alias_primitive(mode, positions, NULL, NULL, count);
}

/* Consume the model loader's immutable corner/index records directly. GL1
 * supplies one interpolated position and lit color per source MD2 vertex;
 * this routine performs the sole per-corner walk while packing the final RSX
 * stream. Fans/strips, indexed batching, alternating strip winding, disabled
 * shell texturing, and the current-color shadow path match
 * rsxgl_draw_alias_primitive(). */
int
RSXGL_DrawPreparedAlias(const float *positions, int position_count,
	int position_stride_floats, const float *colors,
	int color_stride_floats, const rsxgl_alias_draw_t *draws, int draw_count,
	const rsxgl_alias_ref_t *refs, int ref_count,
	const uint16_t *prepared_indices, int prepared_index_count)
{
	float current_resolved[4] = {1.0f, 1.0f, 1.0f, 1.0f};
	int batching;
	int submitted_refs = 0;
	int draw_index;

	if (!positions || position_count <= 0 || position_stride_floats < 3 ||
		(colors && color_stride_floats < 4) || !draws || draw_count <= 0 ||
		!refs || ref_count <= 0)
	{
		return 0;
	}

	rsxgl_set_lightmap_program(0);
	batching = rsxgl_triangle_batch_enabled;
	if (!colors)
	{
		rsxgl_resolve_vertex_color(rsxgl_current_color, current_resolved);
	}

	/* The loader has already flattened every immutable fan/strip into one
	 * 16-bit triangle list. Reserve the complete entity once, pack every UV
	 * reference once, and only add the current batch base to its stored indices.
	 * This removes per-primitive reservation and topology construction from both
	 * animated models and projected shadows. Large custom models which cannot use
	 * one 16-bit reference domain continue through the verified loop below. */
	if (batching && rsxgl_indexed_batch_enabled && !rsxgl_2d_mode &&
		prepared_indices && prepared_index_count > 0 &&
		ref_count <= RSXGL_INDEXED_BATCH_MAX_VERTICES)
	{
		rsxgl_vertex_t *vertices;
		u16 *indices = NULL;
		int base_vertex = 0;
		int i;

		vertices = rsxgl_reserve_indexed_triangle_batch(ref_count,
			prepared_index_count, &indices, &base_vertex);
		if (vertices)
		{
			for (i = 0; i < ref_count; i++)
			{
				const rsxgl_alias_ref_t *ref = &refs[i];
				int source_vertex = ref->index_xyz;
				const float *position;
				const float *texcoord;

				if (source_vertex < 0 || source_vertex >= position_count)
				{
					source_vertex = 0;
				}
				position = positions + source_vertex * position_stride_floats;
				texcoord = rsxgl_texture_enabled ? &ref->s :
					rsxgl_current_texcoord;
				if (colors)
				{
					rsxgl_store_vertex(&vertices[i], position[0], position[1],
						position[2], texcoord[0], texcoord[1],
						colors + source_vertex * color_stride_floats);
				}
				else
				{
					rsxgl_store_resolved_vertex(&vertices[i], position[0],
						position[1], position[2], texcoord[0], texcoord[1],
						current_resolved);
				}
			}
			for (i = 0; i < prepared_index_count; i++)
			{
				unsigned int relative = prepared_indices[i];

				/* The loader creates these indices after validating the complete
				 * command stream. Retain a bounded corruption fallback here too. */
				if (relative >= (unsigned int)ref_count)
				{
					relative = 0;
				}
				indices[i] = (u16)(base_vertex + relative);
			}

			rsxgl_note_api_draw(ref_count, prepared_index_count, 1);
			rsxgl_note_triangle_draw(1);
			if (rsxgl_frame_ready)
			{
				rsxgl_frame_fast_array_vertices += ref_count;
				rsxgl_frame_alias_sources += position_count;
				rsxgl_frame_alias_refs += ref_count;
			}
			if (!rsxgl_traced_flattened_alias)
			{
				PS3_RUNTIME_TRACE(
					"RSX renderer: load-time flattened alias indices active");
				rsxgl_traced_flattened_alias = 1;
			}
			if (!rsxgl_traced_typed_alias)
			{
				PS3_RUNTIME_TRACE(
					"RSX renderer: direct prepared alias-model submission active");
				rsxgl_traced_typed_alias = 1;
			}
			return ref_count;
		}
	}

	for (draw_index = 0; draw_index < draw_count; draw_index++)
	{
		const rsxgl_alias_draw_t *draw = &draws[draw_index];
		const rsxgl_alias_ref_t *draw_refs;
		rsxgl_vertex_t *vertices;
		u16 *indices = NULL;
		int output_count;
		int pack_count;
		int base_vertex = 0;
		int indexed;
		int i;

		if (draw->count < 3 ||
			(draw->type != GL_TRIANGLE_FAN &&
			 draw->type != GL_TRIANGLE_STRIP) ||
			draw->first_ref > (uint32_t)ref_count ||
			draw->count > ref_count - (int)draw->first_ref)
		{
			continue;
		}
		draw_refs = refs + draw->first_ref;
		output_count = batching ? (draw->count - 2) * 3 : draw->count;
		indexed = batching && rsxgl_indexed_batch_enabled && !rsxgl_2d_mode &&
			draw->count <= RSXGL_INDEXED_BATCH_MAX_VERTICES;
		pack_count = indexed ? draw->count : output_count;
		vertices = indexed ? rsxgl_reserve_indexed_triangle_batch(
			draw->count, output_count, &indices, &base_vertex) :
			(batching ? rsxgl_reserve_triangle_batch(output_count) :
				rsxgl_allocate_vertices(output_count));
		if (!vertices)
		{
			continue;
		}

		for (i = 0; i < pack_count; i++)
		{
			int source_corner = batching && !indexed ?
				rsxgl_triangle_source_index((GLenum)draw->type, i) : i;
			const rsxgl_alias_ref_t *ref = &draw_refs[source_corner];
			int source_vertex = ref->index_xyz;
			const float *position;
			const float *texcoord;

			/* The model loader validates every source index before publishing the
			 * topology. Retain a defensive clamp for an asynchronously corrupted
			 * record without permitting an out-of-range PPU read. */
			if (source_vertex < 0 || source_vertex >= position_count)
			{
				source_vertex = 0;
			}
			position = positions + source_vertex * position_stride_floats;
			texcoord = rsxgl_texture_enabled ? &ref->s :
				rsxgl_current_texcoord;
			if (colors)
			{
				rsxgl_store_vertex(&vertices[i], position[0], position[1],
					position[2], texcoord[0], texcoord[1],
					colors + source_vertex * color_stride_floats);
			}
			else
			{
				rsxgl_store_resolved_vertex(&vertices[i], position[0],
					position[1], position[2], texcoord[0], texcoord[1],
					current_resolved);
			}
		}

		if (indexed)
		{
			for (i = 0; i < output_count; i++)
			{
				indices[i] = (u16)(base_vertex +
					rsxgl_triangle_source_index((GLenum)draw->type, i));
			}
			rsxgl_note_api_draw(draw->count, output_count, 1);
		}
		else
		{
			rsxgl_note_api_draw(output_count, 0, batching);
		}
		if (batching)
		{
			rsxgl_note_triangle_draw(indexed);
		}
		else
		{
			rsxgl_submit_vertices((GLenum)draw->type, vertices,
				draw->count);
		}
		if (rsxgl_frame_ready)
		{
			rsxgl_frame_fast_array_vertices += pack_count;
		}
		submitted_refs += draw->count;
	}

	if (rsxgl_frame_ready && submitted_refs > 0)
	{
		rsxgl_frame_alias_sources += position_count;
		rsxgl_frame_alias_refs += submitted_refs;
	}
	if (!rsxgl_traced_typed_alias && submitted_refs > 0)
	{
		PS3_RUNTIME_TRACE(
			"RSX renderer: direct prepared alias-model submission active");
		rsxgl_traced_typed_alias = 1;
	}
	return submitted_refs;
}

int
RSXGL_CombinedLightmapsEnabled(void)
{
	return rsxgl_initialized && rsxgl_cvar_combined_lightmaps &&
		rsxgl_cvar_combined_lightmaps->value != 0.0f;
}

int
RSXGL_BeginLightmappedBatch(GLuint texture, GLuint lightmap,
	float light_scale)
{
	if (!RSXGL_CombinedLightmapsEnabled() ||
		texture >= RSXGL_MAX_TEXTURES || lightmap >= RSXGL_MAX_TEXTURES)
	{
		rsxgl_lightmap_batch_active = 0;
		return 0;
	}

	rsxgl_set_lightmap_program(1);
	if (rsxgl_lightmap_base_texture != texture ||
		rsxgl_lightmap_texture != lightmap)
	{
		rsxgl_flush_triangle_batch();
		if (rsxgl_lightmap_base_texture != texture)
		{
			rsxgl_lightmap_base_texture = texture;
			rsxgl_lightmap_base_sampler_dirty = 1;
		}
		if (rsxgl_lightmap_texture != lightmap)
		{
			rsxgl_lightmap_texture = lightmap;
			rsxgl_lightmap_page_sampler_dirty = 1;
		}
	}

	if (light_scale <= 0.0f)
	{
		light_scale = 1.0f;
	}
	if (rsxgl_lightmap_scale[0] != light_scale)
	{
		rsxgl_lightmap_scale[0] = light_scale;
		rsxgl_lightmap_scale[1] = light_scale;
		rsxgl_lightmap_scale[2] = light_scale;
		rsxgl_lightmap_scale[3] = 1.0f;
		rsxgl_lightmap_scale_dirty = 1;
	}
	rsxgl_lightmap_batch_color[0] = light_scale;
	rsxgl_lightmap_batch_color[1] = light_scale;
	rsxgl_lightmap_batch_color[2] = light_scale;
	rsxgl_lightmap_batch_color[3] = 1.0f;
	rsxgl_lightmap_batch_streamed = rsxgl_triangle_batch_enabled;
	rsxgl_lightmap_batch_indexed = rsxgl_lightmap_batch_streamed &&
		rsxgl_indexed_batch_enabled && !rsxgl_2d_mode;
	rsxgl_lightmap_batch_active = 1;
	return 1;
}

int
RSXGL_DrawStaticLightmappedFan(unsigned int generation,
	unsigned int vertex_offset, unsigned short first_vertex, int count,
	const unsigned short *prepared_indices, int prepared_index_count)
{
	u16 *indices;
	size_t index_bytes;
	size_t aligned;
	int index_count;
	int triangle;
	int output = 0;

	if (!rsxgl_initialized || !rsxgl_frame_ready ||
		!rsxgl_lightmap_batch_active || !rsxgl_static_vertex_arena ||
		!rsxgl_cvar_static_world || rsxgl_cvar_static_world->value == 0.0f ||
		generation != rsxgl_static_vertex_generation || count < 3 ||
		(unsigned int)first_vertex + (unsigned int)count >
			RSXGL_STATIC_VERTEX_PAGE_VERTICES)
	{
		return 0;
	}
	index_count = (count - 2) * 3;
	index_bytes = (size_t)index_count * sizeof(u16);
	if (!index_bytes || index_bytes > RSXGL_INDEX_ARENA_SIZE)
	{
		return 0;
	}

	/* Dynamic and persistent vertices use different array bases and cannot own
	 * one hardware batch. Program/texture state transitions already call the
	 * common flush; enforce the same invariant when a fallback surface precedes
	 * a static fan inside one material chain. */
	if (rsxgl_triangle_batch_vertices > 0)
	{
		rsxgl_flush_triangle_batch();
	}
	if (rsxgl_static_batch_index_count > 0 &&
		rsxgl_static_batch_vertex_offset != vertex_offset)
	{
		rsxgl_flush_triangle_batch();
	}
	if (!rsxgl_static_batch_index_count)
	{
		aligned = (rsxgl_index_arena_used + 127) & ~((size_t)127);
		rsxgl_index_arena_used = aligned <= RSXGL_INDEX_ARENA_SIZE ?
			aligned : RSXGL_INDEX_ARENA_SIZE;
	}
	if (rsxgl_index_arena_used + index_bytes > RSXGL_INDEX_ARENA_SIZE)
	{
		rsxgl_flush_triangle_batch();
		if (rsxgl_index_arena_used + index_bytes > RSXGL_INDEX_ARENA_SIZE)
		{
			rsxgl_finish();
			rsxgl_index_arena_used = 0;
			rsxgl_vertex_cache_needs_invalidate = 1;
		}
	}

	indices = (u16 *)((uint8_t *)rsxgl_index_arena +
		rsxgl_index_arena_used);
	if (!rsxgl_static_batch_index_count)
	{
		rsxgl_static_batch_indices = indices;
		rsxgl_static_batch_vertex_offset = vertex_offset;
	}
	if (prepared_indices && prepared_index_count == index_count)
	{
		memcpy(indices, prepared_indices, index_bytes);
		output = index_count;
		if (!rsxgl_traced_static_indices)
		{
			PS3_RUNTIME_TRACE(
				"RSX renderer: load-time prepared BSP fan indices active");
			rsxgl_traced_static_indices = 1;
		}
	}
	else
	{
		for (triangle = 0; triangle < count - 2; triangle++)
		{
			indices[output++] = first_vertex;
			indices[output++] = (u16)(first_vertex + triangle + 1);
			indices[output++] = (u16)(first_vertex + triangle + 2);
		}
	}
	rsxgl_index_arena_used += index_bytes;
	rsxgl_static_batch_index_count += index_count;
	rsxgl_static_batch_draws++;
	rsxgl_note_api_draw(count, index_count, 1);
	if (rsxgl_frame_ready)
	{
		rsxgl_frame_combined_lightmap_draws++;
		rsxgl_frame_combined_lightmap_vertices += (unsigned int)count;
		rsxgl_frame_static_lightmap_vertices += (unsigned int)count;
	}
	if (!rsxgl_traced_static_world)
	{
		PS3_RUNTIME_TRACE("RSX renderer: persistent local BSP vertex stream active");
		rsxgl_traced_static_world = 1;
	}
	return 1;
}

void
RSXGL_DrawLightmappedFanPrepared(const float *positions,
	int position_stride_floats, const float *texcoords,
	int texcoord_stride_floats, const float *lightcoords,
	int lightcoord_stride_floats, int count)
{
	rsxgl_vertex_t *vertices;
	u16 *indices = NULL;
	int output_count;
	int base_vertex = 0;
	int indexed;
	int pack_count;
	int i;

	if (!rsxgl_lightmap_batch_active || !positions || !texcoords ||
		!lightcoords || position_stride_floats < 3 ||
		texcoord_stride_floats < 2 || lightcoord_stride_floats < 2 ||
		count < 3)
	{
		return;
	}

	output_count = rsxgl_lightmap_batch_streamed ? (count - 2) * 3 : count;
	indexed = rsxgl_lightmap_batch_indexed &&
		count <= RSXGL_INDEXED_BATCH_MAX_VERTICES;
	pack_count = indexed ? count : output_count;
	vertices = indexed ? rsxgl_reserve_indexed_triangle_batch(
		count, output_count, &indices, &base_vertex) :
		(rsxgl_lightmap_batch_streamed ?
			rsxgl_reserve_triangle_batch(output_count) :
			rsxgl_allocate_vertices(output_count));
	if (!vertices)
	{
		return;
	}

	for (i = 0; i < pack_count; i++)
	{
		int source_index = rsxgl_lightmap_batch_streamed && !indexed ?
			rsxgl_triangle_source_index(GL_TRIANGLE_FAN, i) : i;
		const float *position = positions +
			source_index * position_stride_floats;
		const float *texcoord = texcoords +
			source_index * texcoord_stride_floats;
		const float *lightcoord = lightcoords +
			source_index * lightcoord_stride_floats;
		rsxgl_store_lightmapped_vertex(&vertices[i], position[0], position[1],
			position[2], texcoord[0], texcoord[1],
			lightcoord[0], lightcoord[1], rsxgl_lightmap_batch_color);
	}

	if (indexed)
	{
		int triangle;
		int output = 0;
		for (triangle = 0; triangle < count - 2; triangle++)
		{
			indices[output++] = (u16)base_vertex;
			indices[output++] = (u16)(base_vertex + triangle + 1);
			indices[output++] = (u16)(base_vertex + triangle + 2);
		}
		rsxgl_note_api_draw(count, output_count, 1);
	}
	else
	{
		rsxgl_note_api_draw(output_count, 0,
			rsxgl_lightmap_batch_streamed);
	}
	if (rsxgl_lightmap_batch_streamed)
	{
		rsxgl_note_triangle_draw(indexed);
	}
	else
	{
		rsxgl_submit_vertices(GL_TRIANGLE_FAN, vertices, count);
	}

	if (rsxgl_frame_ready)
	{
		rsxgl_frame_fast_array_vertices += pack_count;
		rsxgl_frame_combined_lightmap_draws++;
		rsxgl_frame_combined_lightmap_vertices += pack_count;
	}
	if (!rsxgl_traced_combined_lightmaps)
	{
		PS3_RUNTIME_TRACE("RSX renderer: single-pass base/lightmap modulation active");
		rsxgl_traced_combined_lightmaps = 1;
	}
}

void
RSXGL_EndLightmappedBatch(void)
{
	rsxgl_lightmap_batch_active = 0;
}

void
RSXGL_DrawLightmappedFan(GLuint texture, GLuint lightmap,
	const float *positions, int position_stride_floats,
	const float *texcoords, int texcoord_stride_floats,
	const float *lightcoords, int lightcoord_stride_floats,
	int count, float light_scale)
{
	if (!RSXGL_BeginLightmappedBatch(texture, lightmap, light_scale))
	{
		return;
	}
	RSXGL_DrawLightmappedFanPrepared(positions, position_stride_floats,
		texcoords, texcoord_stride_floats, lightcoords,
		lightcoord_stride_floats, count);
	RSXGL_EndLightmappedBatch();
}

int
RSXGL_Init(gcmContextData *context, int virtual_width, int virtual_height)
{
	videoState state;
	videoResolution resolution;
	u32 vp_size;
	u32 fp_size;
	size_t color_bytes;
	size_t depth_bytes;
	int allocation_alignment;
	int color_allocation_height;
	int depth_allocation_height;
	int depth_height_alignment;
	int i;

	if (!context || virtual_width <= 0 || virtual_height <= 0)
	{
		return 0;
	}

	/* The backend registers these first, so Cvar_Get normally only returns the
	 * existing objects. Keeping the defaults here also makes the compatibility
	 * layer self-contained if it is initialized by a future backend. */
	rsxgl_cvar_hw_cull_mode = Cvar_Get("ps3_rsx_hw_cull_mode", "1", 0);
	rsxgl_cvar_stats = Cvar_Get("ps3_rsx_stats", "0", CVAR_ARCHIVE);
	rsxgl_cvar_tiled_targets = Cvar_Get("ps3_rsx_tiled_targets", "0", 0);
	rsxgl_cvar_ztrick = Cvar_Get("gl1_ztrick", "0", 0);
	rsxgl_cvar_stream_main_memory =
		Cvar_Get("ps3_rsx_stream_main_memory", "0", 0);
	rsxgl_cvar_ui_main_memory =
		Cvar_Get("ps3_rsx_ui_main_memory", "1", 0);
	rsxgl_cvar_ui_vertex_ring =
		Cvar_Get("ps3_rsx_ui_vertex_ring", "1", 0);
	rsxgl_cvar_ui_local_stage =
		Cvar_Get("ps3_rsx_ui_local_stage", "2", 0);
	rsxgl_cvar_batch = Cvar_Get("ps3_rsx_batch", "1", CVAR_ARCHIVE);
	rsxgl_cvar_indexed_batch =
		Cvar_Get("ps3_rsx_indexed_batch", "1", CVAR_ARCHIVE);
	rsxgl_cvar_state_filter =
		Cvar_Get("ps3_rsx_state_filter", "1", CVAR_ARCHIVE);
	rsxgl_cvar_fast_arrays =
		Cvar_Get("ps3_rsx_fast_arrays", "1", CVAR_ARCHIVE);
	rsxgl_cvar_stereo_enable =
		Cvar_Get("ps3_stereo_enable", "0", CVAR_ARCHIVE);
	rsxgl_cvar_stereo_output =
		Cvar_Get("ps3_stereo_output", "0", CVAR_ARCHIVE);
	rsxgl_cvar_stereo_screenshot_eye =
		Cvar_Get("ps3_rsx_stereo_screenshot_eye", "0", 0);
	rsxgl_cvar_texture_rename =
		Cvar_Get("ps3_rsx_texture_rename", "1", CVAR_ARCHIVE);
	rsxgl_cvar_texture_frame_reuse =
		Cvar_Get("ps3_rsx_texture_frame_reuse", "1", CVAR_ARCHIVE);
	rsxgl_cvar_combined_lightmaps =
		Cvar_Get("ps3_rsx_combined_lightmaps", "1", CVAR_ARCHIVE);
	rsxgl_cvar_static_world =
		Cvar_Get("ps3_rsx_static_world", "1", CVAR_ARCHIVE);
	rsxgl_cvar_filter =
		Cvar_Get("ps3_rsx_filter", "0", CVAR_ARCHIVE);
	rsxgl_cvar_filter_strength =
		Cvar_Get("ps3_rsx_filter_strength", "1.0", CVAR_ARCHIVE);
	rsxgl_cvar_performance_profile =
		Cvar_Get("ps3_rsx_performance_profile", "0", CVAR_ARCHIVE);
	rsxgl_cvar_fps_target =
		Cvar_Get("ps3_rsx_fps_target", "0", CVAR_ARCHIVE);
	if (videoGetState(VIDEO_PRIMARY, 0, &state) != 0 ||
		videoGetResolution(state.displayMode.resolution, &resolution) != 0)
	{
		return 0;
	}

	rsxgl_context = context;
	rsxgl_virtual_width = virtual_width;
	rsxgl_virtual_height = virtual_height;
	rsxgl_output_width = resolution.width;
	rsxgl_output_height = resolution.height;
	rsxgl_target_y = 0;
	rsxgl_target_height = rsxgl_output_height;
	rsxgl_frame_packed_output =
		(rsxgl_output_width == 1280 &&
		rsxgl_output_height >= RSXGL_FRAME_PACKED_EYE_HEIGHT * 2 + 30);
	rsxgl_top_bottom_output = !rsxgl_frame_packed_output &&
		rsxgl_output_width == 1280 && rsxgl_output_height == 720 &&
		rsxgl_cvar_stereo_enable->value > 0.0f &&
		rsxgl_cvar_stereo_output->value >= 1.0f;
	if (rsxgl_top_bottom_output)
	{
		PS3_RUNTIME_TRACE("RSX stereo: standard 720p top-and-bottom compatibility output active");
	}
	rsxgl_tiled_targets_requested =
		rsxgl_cvar_tiled_targets->value >= 1.0f;
	rsxgl_zcull_requested =
		rsxgl_cvar_tiled_targets->value >= 2.0f;
	if (rsxgl_tiled_targets_requested &&
		rsxgl_cvar_ztrick->value != 0.0f)
	{
		/* Z-trick alternates LEQUAL/GEQUAL every frame, but one hardware
		 * Z-cull region has a fixed direction. Keep the linear path rather
		 * than permit incorrect early rejection under a custom config. */
		PS3_RUNTIME_TRACE("RSX renderer: tiled targets require gl1_ztrick 0; using linear fallback");
		rsxgl_tiled_targets_requested = 0;
		rsxgl_zcull_requested = 0;
	}
	rsxgl_bound_tile_count = 0;
	rsxgl_zcull_bound = 0;
	color_allocation_height = rsxgl_output_height;
	depth_allocation_height = rsxgl_output_height;
	allocation_alignment = 64;
	if (rsxgl_tiled_targets_requested)
	{
		rsxgl_color_pitch = gcmGetTiledPitchSize(rsxgl_output_width * 4);
		rsxgl_depth_pitch = gcmGetTiledPitchSize(rsxgl_output_width * 4);
		color_allocation_height = (rsxgl_output_height +
			GCM_TILE_LOCAL_ALIGN_HEIGHT - 1) &
			~(GCM_TILE_LOCAL_ALIGN_HEIGHT - 1);
		/* The Z-cull registration is rounded to 64 rows, not the tile allocator's
		 * 32. Allocating only the tile extent made 480p and 720p mono Z-cull
		 * address beyond their depth surfaces while frame-packed 1470 happened
		 * to align correctly. Color remains on its smaller valid extent. */
		depth_height_alignment = rsxgl_zcull_requested ?
			GCM_ZCULL_ALIGN_HEIGHT : GCM_TILE_LOCAL_ALIGN_HEIGHT;
		depth_allocation_height = (rsxgl_output_height +
			depth_height_alignment - 1) & ~(depth_height_alignment - 1);
		allocation_alignment = GCM_TILE_ALIGN_SIZE;
	}
	else
	{
		rsxgl_color_pitch = rsxgl_output_width * 4;
		rsxgl_depth_pitch = rsxgl_output_width * 4;
	}
	color_bytes = (size_t)rsxgl_color_pitch * color_allocation_height;
	depth_bytes = (size_t)rsxgl_depth_pitch * depth_allocation_height;
	if (rsxgl_tiled_targets_requested)
	{
		color_bytes = rsxgl_align_size(color_bytes, GCM_TILE_ALIGN_OFFSET);
		depth_bytes = rsxgl_align_size(depth_bytes, GCM_TILE_ALIGN_OFFSET);
	}
	if (!color_bytes || !depth_bytes || color_bytes > UINT32_MAX ||
		depth_bytes > UINT32_MAX)
	{
		goto fail;
	}
	rsxgl_color_allocation_size = color_bytes;
	rsxgl_depth_allocation_size = depth_bytes;

	for (i = 0; i < RSXGL_DISPLAY_BUFFERS; i++)
	{
		rsxgl_color[i] = rsxMemalign(allocation_alignment,
			rsxgl_color_allocation_size);
		if (!rsxgl_color[i]) goto fail;
		memset(rsxgl_color[i], 0, rsxgl_color_allocation_size);
		if (rsxAddressToOffset(rsxgl_color[i],
			&rsxgl_color_offset[i]) != 0) goto fail;
		if (gcmSetDisplayBuffer(i, rsxgl_color_offset[i], rsxgl_color_pitch,
			rsxgl_output_width, rsxgl_output_height) != 0) goto fail;
	}
	rsxgl_depth = rsxMemalign(allocation_alignment,
		rsxgl_depth_allocation_size);
	if (!rsxgl_depth) goto fail;
	memset(rsxgl_depth, 0xff, rsxgl_depth_allocation_size);
	if (rsxAddressToOffset(rsxgl_depth, &rsxgl_depth_offset) != 0) goto fail;
	if (rsxgl_tiled_targets_requested)
	{
		if (rsxgl_configure_tiled_targets())
		{
			PS3_RUNTIME_TRACE(rsxgl_zcull_requested ?
				"RSX renderer: tiled targets and Z-cull active" :
				"RSX renderer: tiled targets active without Z-cull");
		}
		else
		{
			PS3_RUNTIME_TRACE("RSX renderer: tiled-target setup failed; using linear fallback");
		}
	}
	PS3_BOOT_TRACE("RSX renderer: color/depth targets allocated");

	/* RSX-local render targets are write-combined from the PPU side and cannot
	 * be used as a coherent CPU readback source. Keep one mapped XDR staging
	 * surface so glReadPixels can DMA the completed render target to main memory
	 * before scaling it into the engine's screenshot buffer. */
	rsxgl_readback_main_allocation = NULL;
	rsxgl_readback_main_offset = 0;
	rsxgl_readback_mapping_size = rsxgl_align_size(
		(size_t)rsxgl_color_pitch * rsxgl_output_height,
		RSXGL_MAIN_MEMORY_ALIGNMENT);
	rsxgl_readback_main_mapped = 0;
	if (rsxgl_readback_mapping_size)
	{
		rsxgl_readback_main_allocation = memalign(RSXGL_MAIN_MEMORY_ALIGNMENT,
			rsxgl_readback_mapping_size);
		if (rsxgl_readback_main_allocation &&
			gcmMapMainMemory(rsxgl_readback_main_allocation,
				(u32)rsxgl_readback_mapping_size,
				&rsxgl_readback_main_offset) == 0)
		{
			rsxgl_readback_main_mapped = 1;
			PS3_RUNTIME_TRACE("RSX renderer: main-memory screenshot readback mapped");
		}
		else
		{
			if (rsxgl_readback_main_allocation)
			{
				free(rsxgl_readback_main_allocation);
				rsxgl_readback_main_allocation = NULL;
			}
			PS3_RUNTIME_TRACE("RSX renderer: screenshot readback mapping unavailable");
		}
	}

	rsxgl_stream_location = GCM_LOCATION_RSX;
	rsxgl_vertex_arena_offset = 0;
	rsxgl_index_arena_offset = 0;
	rsxgl_stream_main_allocation = NULL;
	rsxgl_stream_main_mapped = 0;
	if (rsxgl_cvar_stream_main_memory->value != 0.0f)
	{
		u32 main_offset = 0;
		rsxgl_stream_main_allocation = memalign(RSXGL_MAIN_MEMORY_ALIGNMENT,
			RSXGL_STREAM_ARENA_SIZE);
		if (rsxgl_stream_main_allocation &&
			gcmMapMainMemory(rsxgl_stream_main_allocation,
				RSXGL_STREAM_ARENA_SIZE, &main_offset) == 0)
		{
			rsxgl_stream_main_mapped = 1;
			rsxgl_stream_location = GCM_LOCATION_CELL;
			rsxgl_vertex_arena = (rsxgl_vertex_t *)
				rsxgl_stream_main_allocation;
			rsxgl_index_arena = (u16 *)((uint8_t *)
				rsxgl_stream_main_allocation + RSXGL_VERTEX_ARENA_SIZE);
			rsxgl_vertex_arena_offset = main_offset;
			rsxgl_index_arena_offset = main_offset + RSXGL_VERTEX_ARENA_SIZE;
			PS3_RUNTIME_TRACE("RSX renderer: main-memory vertex/index streaming active");
		}
		else
		{
			if (rsxgl_stream_main_allocation)
			{
				free(rsxgl_stream_main_allocation);
				rsxgl_stream_main_allocation = NULL;
			}
			PS3_RUNTIME_TRACE("RSX renderer: main-memory stream mapping failed; using local memory");
		}
	}
	if (!rsxgl_stream_main_mapped)
	{
		rsxgl_vertex_arena = rsxMemalign(128, RSXGL_VERTEX_ARENA_SIZE);
		if (!rsxgl_vertex_arena ||
			rsxAddressToOffset(rsxgl_vertex_arena,
				&rsxgl_vertex_arena_offset) != 0) goto fail;
		rsxgl_index_arena = rsxMemalign(128, RSXGL_INDEX_ARENA_SIZE);
		if (!rsxgl_index_arena ||
			rsxAddressToOffset(rsxgl_index_arena,
				&rsxgl_index_arena_offset) != 0) goto fail;
	}
	rsxgl_world_vertex_arena = rsxgl_vertex_arena;
	rsxgl_world_vertex_arena_offset = rsxgl_vertex_arena_offset;
	rsxgl_world_stream_location = rsxgl_stream_location;
	rsxgl_world_vertex_arena_used = 0;
	rsxgl_static_vertex_arena = NULL;
	rsxgl_static_vertex_arena_offset = 0;
	rsxgl_static_vertex_arena_used = 0;
	rsxgl_static_vertex_page_offset = 0;
	rsxgl_static_vertex_page_used = 0;
	rsxgl_static_registered_vertices = 0;
	rsxgl_static_geometry_dirty = 0;
	/* A renderer restart can retain the CPU-side BSP model while replacing every
	 * RSX allocation. Reject those old polygon handles even when the map name is
	 * unchanged; they safely use the established dynamic stream until rebuilt. */
	rsxgl_static_vertex_generation++;
	if (!rsxgl_static_vertex_generation)
	{
		rsxgl_static_vertex_generation = 1;
	}
	if (rsxgl_cvar_static_world->value != 0.0f)
	{
		rsxgl_static_vertex_arena = rsxMemalign(128,
			RSXGL_STATIC_VERTEX_ARENA_SIZE);
		if (!rsxgl_static_vertex_arena ||
			rsxAddressToOffset(rsxgl_static_vertex_arena,
				&rsxgl_static_vertex_arena_offset) != 0)
		{
			if (rsxgl_static_vertex_arena)
			{
				rsxFree(rsxgl_static_vertex_arena);
				rsxgl_static_vertex_arena = NULL;
			}
			PS3_RUNTIME_TRACE("RSX renderer: static BSP arena unavailable; using dynamic stream");
		}
	}
	rsxgl_ui_stream_main_allocation = NULL;
	rsxgl_ui_vertex_arena = NULL;
	rsxgl_ui_vertex_arena_offset = 0;
	rsxgl_ui_vertex_arena_used = 0;
	rsxgl_ui_vertex_arena_size = RSXGL_UI_VERTEX_ARENA_SIZE;
	rsxgl_ui_stream_main_mapped = 0;
	rsxgl_ui_stream_selected = 0;
	rsxgl_ui_local_vertex_arena = NULL;
	rsxgl_ui_local_vertex_arena_offset = 0;
	rsxgl_ui_local_vertex_arena_used = 0;
	rsxgl_ui_local_vertex_arena_size = RSXGL_UI_VERTEX_ARENA_SIZE;
	rsxgl_ui_stream_direct_local = 0;
	if (rsxgl_cvar_ui_main_memory->value != 0.0f)
	{
		size_t requested_size = rsxgl_cvar_ui_vertex_ring->value != 0.0f ?
			RSXGL_UI_VERTEX_RING_SIZE : RSXGL_UI_VERTEX_ARENA_SIZE;
		rsxgl_ui_stream_main_allocation =
			memalign(RSXGL_MAIN_MEMORY_ALIGNMENT,
				requested_size);
		if (rsxgl_ui_stream_main_allocation &&
			gcmMapMainMemory(rsxgl_ui_stream_main_allocation,
				requested_size,
				&rsxgl_ui_vertex_arena_offset) == 0)
		{
			rsxgl_ui_stream_main_mapped = 1;
			rsxgl_ui_vertex_arena_size = requested_size;
			rsxgl_ui_vertex_arena = (rsxgl_vertex_t *)
				rsxgl_ui_stream_main_allocation;
			PS3_RUNTIME_TRACE(requested_size > RSXGL_UI_VERTEX_ARENA_SIZE ?
				"RSX renderer: cache-coherent rotating main-memory 2D stream active" :
				"RSX renderer: cache-coherent main-memory 2D stream active");
		}
		else
		{
			if (rsxgl_ui_stream_main_allocation)
			{
				free(rsxgl_ui_stream_main_allocation);
				rsxgl_ui_stream_main_allocation = NULL;
			}
			/* A 16 MiB rotating stream is preferred, but do not lose the proven
			 * 8 MiB coherent path if memory pressure prevents the larger map. */
			if (requested_size > RSXGL_UI_VERTEX_ARENA_SIZE)
			{
				rsxgl_ui_stream_main_allocation =
					memalign(RSXGL_MAIN_MEMORY_ALIGNMENT,
						RSXGL_UI_VERTEX_ARENA_SIZE);
				if (rsxgl_ui_stream_main_allocation &&
					gcmMapMainMemory(rsxgl_ui_stream_main_allocation,
						RSXGL_UI_VERTEX_ARENA_SIZE,
						&rsxgl_ui_vertex_arena_offset) == 0)
				{
					rsxgl_ui_stream_main_mapped = 1;
					rsxgl_ui_vertex_arena = (rsxgl_vertex_t *)
						rsxgl_ui_stream_main_allocation;
					PS3_RUNTIME_TRACE("RSX renderer: rotating 2D stream unavailable; using coherent single-frame stream");
				}
				else if (rsxgl_ui_stream_main_allocation)
				{
					free(rsxgl_ui_stream_main_allocation);
					rsxgl_ui_stream_main_allocation = NULL;
				}
			}
			if (!rsxgl_ui_stream_main_mapped)
			{
				PS3_RUNTIME_TRACE("RSX renderer: main-memory 2D stream unavailable; using primary stream");
			}
		}
	}
	if (rsxgl_ui_stream_main_mapped &&
		rsxgl_cvar_ui_local_stage->value != 0.0f)
	{
		rsxgl_ui_local_vertex_arena =
			rsxMemalign(128, RSXGL_UI_LOCAL_RING_SIZE);
		if (!rsxgl_ui_local_vertex_arena ||
			rsxAddressToOffset(rsxgl_ui_local_vertex_arena,
				&rsxgl_ui_local_vertex_arena_offset) != 0)
		{
			if (rsxgl_ui_local_vertex_arena)
			{
				rsxFree(rsxgl_ui_local_vertex_arena);
				rsxgl_ui_local_vertex_arena = NULL;
			}
			rsxgl_ui_local_vertex_arena_offset = 0;

			/* Prefer fresh destination addresses across frames, but retain the
			 * established staged path when local-memory pressure prevents the
			 * larger ring allocation. This fallback is still safer than direct
			 * cacheable-XDR vertex fetches on physical RSX. */
			rsxgl_ui_local_vertex_arena =
				rsxMemalign(128, RSXGL_UI_VERTEX_ARENA_SIZE);
			if (rsxgl_ui_local_vertex_arena &&
				rsxAddressToOffset(rsxgl_ui_local_vertex_arena,
					&rsxgl_ui_local_vertex_arena_offset) == 0)
			{
				rsxgl_ui_local_vertex_arena_size =
					RSXGL_UI_VERTEX_ARENA_SIZE;
				PS3_RUNTIME_TRACE("RSX renderer: rotating local 2D stage unavailable; using flip-fenced single-frame stage");
			}
			else
			{
				if (rsxgl_ui_local_vertex_arena)
				{
					rsxFree(rsxgl_ui_local_vertex_arena);
					rsxgl_ui_local_vertex_arena = NULL;
				}
				rsxgl_ui_local_vertex_arena_offset = 0;
				PS3_RUNTIME_TRACE("RSX renderer: GPU-local 2D staging unavailable; using direct XDR stream");
			}
		}
		else
		{
			rsxgl_ui_local_vertex_arena_size = RSXGL_UI_LOCAL_RING_SIZE;
			PS3_RUNTIME_TRACE("RSX renderer: rotating GPU-local 2D staging ring active");
		}
	}
	if (rsxgl_ui_local_vertex_arena &&
		rsxgl_ui_local_vertex_arena_size > RSXGL_UI_VERTEX_ARENA_SIZE &&
		rsxgl_cvar_ui_local_stage->value >= 2.0f)
	{
		/* Packing directly into a rotating local ring removes the asynchronous
		 * main-to-local copy from the physical text path. Each batch still gets
		 * the established vertex-cache invalidate, and the flip fence protects
		 * every address before the ring wraps. Value 1 retains DMA staging for
		 * hardware A/B tests; zero retains direct XDR. */
		rsxgl_ui_stream_direct_local = 1;
		PS3_RUNTIME_TRACE("RSX renderer: direct rotating GPU-local 2D stream active");
	}
	rsxgl_white_pixels = rsxMemalign(128, 128);
	if (!rsxgl_white_pixels) goto fail;
	*((u32 *)rsxgl_white_pixels) = 0xffffffff;
	if (rsxAddressToOffset(rsxgl_white_pixels,
		&rsxgl_white_offset) != 0) goto fail;

	rsxVertexProgramGetUCode(rsxgl_vp, &rsxgl_vp_ucode, &vp_size);
	rsxFragmentProgramGetUCode(rsxgl_fp, &rsxgl_fp_ucode, &fp_size);
	rsxgl_fp_buffer = rsxMemalign(64, fp_size);
	if (!rsxgl_fp_buffer) goto fail;
	memcpy(rsxgl_fp_buffer, rsxgl_fp_ucode, fp_size);
	if (rsxAddressToOffset(rsxgl_fp_buffer, &rsxgl_fp_offset) != 0) goto fail;
	rsxgl_mvp_parameter = rsxVertexProgramGetConst(rsxgl_vp, "mvpMatrix");
	rsxgl_texture_parameter = rsxFragmentProgramGetAttrib(rsxgl_fp, "texture0");
	if (!rsxgl_mvp_parameter || !rsxgl_texture_parameter) goto fail;
	rsxVertexProgramGetUCode(rsxgl_lightmap_vp,
		&rsxgl_lightmap_vp_ucode, &vp_size);
	rsxFragmentProgramGetUCode(rsxgl_lightmap_fp,
		&rsxgl_lightmap_fp_ucode, &fp_size);
	rsxgl_lightmap_fp_buffer = rsxMemalign(64, fp_size);
	if (!rsxgl_lightmap_fp_buffer) goto fail;
	memcpy(rsxgl_lightmap_fp_buffer, rsxgl_lightmap_fp_ucode, fp_size);
	if (rsxAddressToOffset(rsxgl_lightmap_fp_buffer,
		&rsxgl_lightmap_fp_offset) != 0) goto fail;
	rsxgl_lightmap_mvp_parameter =
		rsxVertexProgramGetConst(rsxgl_lightmap_vp, "mvpMatrix");
	rsxgl_lightmap_scale_parameter =
		rsxVertexProgramGetConst(rsxgl_lightmap_vp, "lightScale");
	rsxgl_lightmap_texture_parameter =
		rsxFragmentProgramGetAttrib(rsxgl_lightmap_fp, "texture0");
	rsxgl_lightmap_parameter =
		rsxFragmentProgramGetAttrib(rsxgl_lightmap_fp, "lightmap0");
	if (!rsxgl_lightmap_mvp_parameter || !rsxgl_lightmap_scale_parameter ||
		!rsxgl_lightmap_texture_parameter || !rsxgl_lightmap_parameter)
		goto fail;
	rsxFragmentProgramGetUCode(rsxgl_filter_fp,
		&rsxgl_filter_fp_ucode, &fp_size);
	rsxgl_filter_fp_buffer = rsxMemalign(64, fp_size);
	if (!rsxgl_filter_fp_buffer) goto fail;
	memcpy(rsxgl_filter_fp_buffer, rsxgl_filter_fp_ucode, fp_size);
	if (rsxAddressToOffset(rsxgl_filter_fp_buffer,
		&rsxgl_filter_fp_offset) != 0) goto fail;
	rsxgl_filter_params0_parameter =
		rsxFragmentProgramGetConst(rsxgl_filter_fp, "filterParams0");
	rsxgl_filter_params1_parameter =
		rsxFragmentProgramGetConst(rsxgl_filter_fp, "filterParams1");
	if (!rsxgl_filter_params0_parameter || !rsxgl_filter_params1_parameter)
		goto fail;
	PS3_BOOT_TRACE("RSX renderer: native shaders loaded");

	memset(&rsxgl_surface, 0, sizeof(rsxgl_surface));
	rsxgl_surface.colorFormat = GCM_SURFACE_X8R8G8B8;
	rsxgl_surface.colorTarget = GCM_SURFACE_TARGET_0;
	for (i = 0; i < GCM_MAX_MRT_COUNT; i++)
	{
		rsxgl_surface.colorLocation[i] = GCM_LOCATION_RSX;
		rsxgl_surface.colorOffset[i] = rsxgl_color_offset[0];
		rsxgl_surface.colorPitch[i] = (i == 0) ? rsxgl_color_pitch : 64;
	}
	rsxgl_surface.depthFormat = GCM_SURFACE_ZETA_Z24S8;
	rsxgl_surface.depthLocation = GCM_LOCATION_RSX;
	rsxgl_surface.depthOffset = rsxgl_depth_offset;
	rsxgl_surface.depthPitch = rsxgl_depth_pitch;
	rsxgl_surface.type = GCM_SURFACE_TYPE_LINEAR;
	rsxgl_surface.antiAlias = GCM_SURFACE_CENTER_1;
	rsxgl_surface.width = rsxgl_output_width;
	rsxgl_surface.height = rsxgl_output_height;

	rsxgl_identity(rsxgl_modelview[0]);
	rsxgl_identity(rsxgl_projection[0]);
	rsxgl_modelview_top = 0;
	rsxgl_projection_top = 0;
	rsxgl_matrix_mode = GL_MODELVIEW;
	memset(&rsxgl_vertex_array, 0, sizeof(rsxgl_vertex_array));
	memset(&rsxgl_texcoord_array, 0, sizeof(rsxgl_texcoord_array));
	memset(&rsxgl_color_array, 0, sizeof(rsxgl_color_array));
	rsxgl_current_color[0] = 1.0f;
	rsxgl_current_color[1] = 1.0f;
	rsxgl_current_color[2] = 1.0f;
	rsxgl_current_color[3] = 1.0f;
	rsxgl_current_texcoord[0] = 0.0f;
	rsxgl_current_texcoord[1] = 0.0f;
	rsxgl_bound_texture = 0;
	rsxgl_texture_enabled = 0;
	/* R_SetDefaultState() selects REPLACE. Starting there also keeps a
	 * renderer restart correct if GL1's texture-environment cache suppresses
	 * the otherwise redundant call. */
	rsxgl_texture_env = GL_REPLACE;
	rsxgl_inside_begin = 0;
	rsxgl_immediate_count = 0;
	rsxgl_depth_near = 0.0f;
	rsxgl_depth_far = 1.0f;
	rsxgl_draw_buffer = 0;
	rsxgl_viewport_x = 0;
	rsxgl_viewport_y = 0;
	rsxgl_viewport_width = virtual_width;
	rsxgl_viewport_height = virtual_height;
	rsxgl_scissor_x = 0;
	rsxgl_scissor_y = 0;
	rsxgl_scissor_width = virtual_width;
	rsxgl_scissor_height = virtual_height;
	rsxgl_scissor_enabled = 0;
	rsxgl_physical_viewport_valid = 0;
	rsxgl_physical_scissor_valid = 0;
	rsxgl_rgb_scale = 1.0f;
	rsxgl_retired_texture_count = 0;
	rsxgl_retired_texture_bytes = 0;
	rsxgl_frame_active = 0;
	rsxgl_frame_ready = 0;
	rsxgl_frame_serial = 0;
	rsxgl_traced_texture_rename = 0;
	rsxgl_traced_texture_frame_reuse = 0;
	rsxgl_traced_texture_rename_fallback = 0;
	rsxgl_traced_texture_retire_pressure = 0;
	rsxgl_traced_readback_dma = 0;
	rsxgl_traced_first_upload = 0;
	rsxgl_traced_first_finish = 0;
	rsxgl_traced_flip_timeout = 0;
	rsxgl_traced_first_mipmap = 0;
	rsxgl_traced_generated_mipmap = 0;
	rsxgl_traced_texture_redefine = 0;
	rsxgl_traced_stereo_readback = 0;
	rsxgl_traced_matrix_cache = 0;
	rsxgl_traced_sampler_cache = 0;
	rsxgl_traced_triangle_batch = 0;
	rsxgl_traced_indexed_triangle_batch = 0;
	rsxgl_traced_state_filter = 0;
	rsxgl_traced_physical_raster_cache = 0;
	rsxgl_traced_fast_arrays = 0;
	rsxgl_traced_typed_particles = 0;
	rsxgl_traced_typed_alias = 0;
	rsxgl_traced_flattened_alias = 0;
	rsxgl_traced_vertex_cache_sync = 0;
	rsxgl_traced_ui_ring_wrap = 0;
	rsxgl_traced_ui_local_ring_wrap = 0;
	rsxgl_traced_ui_local_stage = 0;
	rsxgl_traced_inline_text = 0;
	rsxgl_traced_planar_text = 0;
	rsxgl_traced_text_geometry = 0;
	rsxgl_traced_combined_lightmaps = 0;
	rsxgl_traced_static_world = 0;
	rsxgl_traced_static_indices = 0;
	rsxgl_traced_display_filter = 0;
	rsxgl_vertex_cache_needs_invalidate = 1;
	rsxgl_traced_first_draw = 0;
	rsxgl_traced_first_frame = 0;
	rsxgl_traced_first_clear = 0;
	rsxgl_traced_stereo_left = 0;
	rsxgl_traced_stereo_right = 0;
	rsxgl_traced_stereo_clear = 0;
	rsxgl_traced_acquired_surface_clear = 0;
	rsxgl_traced_out_of_frame_2d = 0;
	rsxgl_traced_inline_text_word_mismatch = 0;
	rsxgl_traced_inline_quad_word_mismatch = 0;
	rsxgl_traced_inline_quad = 0;
	rsxgl_traced_inline_2d_state_cache = 0;
	rsxgl_traced_inline_2d_batch = 0;
	rsxgl_traced_2d_projection_cache = 0;
	rsxgl_traced_ui_duplicate_string = 0;
	memset(rsxgl_ui_string_signature_valid, 0,
		sizeof(rsxgl_ui_string_signature_valid));
	rsxgl_inline_2d_batch_active = 0;
	rsxgl_inline_2d_batch_vertices = 0;
	rsxgl_inline_2d_batch_color_valid = 0;
	rsxgl_programs_bound = 0;
	rsxgl_mvp_valid = 0;
	rsxgl_matrix_revision = 1;
	rsxgl_uploaded_matrix_revision = 0;
	rsxgl_sampler_dirty = 1;
	rsxgl_texture_cache_dirty = 1;
	rsxgl_lightmap_program_active = 0;
	rsxgl_filter_program_active = 0;
	rsxgl_lightmap_base_texture = RSXGL_MAX_TEXTURES;
	rsxgl_lightmap_texture = RSXGL_MAX_TEXTURES;
	rsxgl_lightmap_base_sampler_dirty = 1;
	rsxgl_lightmap_page_sampler_dirty = 1;
	rsxgl_lightmap_batch_active = 0;
	rsxgl_lightmap_batch_streamed = 0;
	rsxgl_lightmap_batch_indexed = 0;
	rsxgl_lightmap_scale[0] = 1.0f;
	rsxgl_lightmap_scale[1] = 1.0f;
	rsxgl_lightmap_scale[2] = 1.0f;
	rsxgl_lightmap_scale[3] = 1.0f;
	rsxgl_lightmap_scale_dirty = 1;
	rsxgl_world_vertex_arena_used = 0;
	rsxgl_ui_vertex_arena_used = 0;
	rsxgl_ui_local_vertex_arena_used = 0;
	rsxgl_ui_stream_selected = 0;
	rsxgl_vertex_arena = rsxgl_world_vertex_arena;
	rsxgl_vertex_arena_offset = rsxgl_world_vertex_arena_offset;
	rsxgl_vertex_arena_size = RSXGL_VERTEX_ARENA_SIZE;
	rsxgl_stream_location = rsxgl_world_stream_location;
	rsxgl_vertex_arena_used = 0;
	rsxgl_index_arena_used = 0;
	rsxgl_vertex_cache_needs_invalidate = 1;
	rsxgl_2d_mode = 0;
	rsxgl_direct_2d_clip = 0;
	rsxgl_triangle_batch = NULL;
	rsxgl_triangle_batch_indices = NULL;
	rsxgl_triangle_batch_vertices = 0;
	rsxgl_triangle_batch_index_count = 0;
	rsxgl_triangle_batch_draws = 0;
	rsxgl_static_batch_indices = NULL;
	rsxgl_static_batch_vertex_offset = 0;
	rsxgl_static_batch_index_count = 0;
	rsxgl_static_batch_draws = 0;
	rsxgl_triangle_batch_enabled = rsxgl_cvar_batch->value != 0.0f;
	rsxgl_indexed_batch_enabled = rsxgl_cvar_indexed_batch->value != 0.0f;
	rsxgl_state_filter_enabled = rsxgl_cvar_state_filter->value != 0.0f;
	rsxgl_fast_arrays_enabled = rsxgl_cvar_fast_arrays->value != 0.0f;
	rsxgl_stats_enabled = 0;
	rsxgl_reset_stats_accumulator();
	rsxgl_stats_previous_begin_us = 0;
	rsxgl_frame_api_draws = 0;
	rsxgl_frame_gpu_draws = 0;
	rsxgl_frame_batched_draws = 0;
	rsxgl_frame_batch_flushes = 0;
	rsxgl_frame_vertices = 0;
	rsxgl_frame_indices = 0;
	rsxgl_frame_state_skips = 0;
	rsxgl_frame_fast_array_vertices = 0;
	rsxgl_frame_texture_updates = 0;
	rsxgl_frame_texture_inplace = 0;
	rsxgl_frame_texture_renames = 0;
	rsxgl_frame_texture_rename_kib = 0;
	rsxgl_frame_combined_lightmap_draws = 0;
	rsxgl_frame_combined_lightmap_vertices = 0;
	rsxgl_frame_static_lightmap_vertices = 0;
	rsxgl_frame_stereo_lightmap_reuse_pixels = 0;
	rsxgl_frame_finishes = 0;
	rsxgl_alpha_test_enabled = 0;
	rsxgl_blend_enabled = 0;
	rsxgl_cull_face_enabled = 0;
	rsxgl_hw_cull_mode = rsxgl_requested_hardware_cull_mode();
	rsxgl_depth_test_enabled = 0;
	rsxgl_stencil_test_enabled = 0;
	rsxgl_polygon_offset_fill_enabled = 0;
	rsxgl_alpha_func_valid = 0;
	rsxgl_blend_func_valid = 0;
	rsxgl_color_mask_state = GCM_COLOR_MASK_R | GCM_COLOR_MASK_G |
		GCM_COLOR_MASK_B | GCM_COLOR_MASK_A;
	rsxgl_cull_face_state = GL_FRONT;
	rsxgl_depth_func_state = GL_LEQUAL;
	rsxgl_depth_mask_state = GL_TRUE;
	rsxgl_point_size_valid = 0;
	rsxgl_front_polygon_mode_valid = 0;
	rsxgl_back_polygon_mode_valid = 0;
	rsxgl_polygon_offset_valid = 0;
	rsxgl_shade_model_valid = 0;
	rsxgl_stencil_func_valid = 0;
	rsxgl_stencil_mask_valid = 0;
	rsxgl_stencil_op_valid = 0;
	rsxgl_initialized = 1;
	RSXGL_SetVSync(1);
	rsxgl_set_surface(0);
	rsxSetColorMask(rsxgl_context, GCM_COLOR_MASK_R | GCM_COLOR_MASK_G |
		GCM_COLOR_MASK_B | GCM_COLOR_MASK_A);
	rsxSetColorMaskMrt(rsxgl_context, 0);
	rsxSetBlendEquation(rsxgl_context, GCM_FUNC_ADD, GCM_FUNC_ADD);
	rsxSetBlendEnable(rsxgl_context, GCM_FALSE);
	rsxSetAlphaTestEnable(rsxgl_context, GCM_FALSE);
	rsxSetDepthTestEnable(rsxgl_context, GCM_FALSE);
	rsxSetDepthWriteEnable(rsxgl_context, GCM_TRUE);
	rsxSetDepthFunc(rsxgl_context, GCM_LEQUAL);
	rsxgl_apply_hardware_cull_state();
	rsxgl_trace_hardware_cull_mode();
	rsxSetStencilTestEnable(rsxgl_context, GCM_FALSE);
	rsxSetZMinMaxControl(rsxgl_context, GCM_FALSE, GCM_TRUE, GCM_FALSE);
	for (i = 0; i < 8; i++)
	{
		rsxSetViewportClip(rsxgl_context, i, rsxgl_output_width,
			rsxgl_output_height);
	}
	rsxgl_apply_viewport(0, 0, virtual_width, virtual_height);
	rsxgl_apply_scissor();
	PS3_BOOT_TRACE("RSX renderer: queueing initial display flip");
	if (gcmSetFlip(rsxgl_context, 0) != 0) goto fail;
	rsxFlushBuffer(rsxgl_context);
	PS3_BOOT_TRACE("RSX renderer: initial display flip flushed");
	gcmSetWaitFlip(rsxgl_context);
	PS3_BOOT_TRACE("RSX renderer: initial display flip wait queued");
	return 1;

fail:
	RSXGL_Shutdown();
	return 0;
}

void
RSXGL_Shutdown(void)
{
	int i;
	if (Sys_PS3_ExitRequested())
	{
		/* XMB can reclaim the display queue before the main thread reaches
		 * renderer shutdown. Do not wait for, unmap, or free allocations that
		 * may still be referenced by that queue; LV2 reclaims the process-owned
		 * resources immediately after Sys_Quit returns. This guard must live in
		 * the renderer, because RI_ShutdownContext runs before GLimp_Shutdown's
		 * corresponding host/context guard. */
		PS3_RUNTIME_TRACE("RSXGL_Shutdown: XMB exit leaves renderer resources to process teardown");
		rsxgl_inline_2d_batch_active = 0;
		rsxgl_inline_2d_batch_vertices = 0;
		rsxgl_inline_2d_batch_color_valid = 0;
		rsxgl_frame_active = 0;
		rsxgl_frame_ready = 0;
		rsxgl_initialized = 0;
		rsxgl_context = NULL;
		return;
	}
	if (rsxgl_context && rsxgl_initialized)
	{
		rsxgl_end_inline_2d_batch();
		rsxgl_finish();
	}
	rsxgl_disable_tiled_targets();
	rsxgl_release_retired_textures();
	for (i = 0; i < RSXGL_MAX_TEXTURES; i++)
	{
		if (rsxgl_textures[i].pixels)
		{
			rsxFree(rsxgl_textures[i].pixels);
		}
	}
	memset(rsxgl_textures, 0, sizeof(rsxgl_textures));
	if (rsxgl_fp_buffer) rsxFree(rsxgl_fp_buffer);
	if (rsxgl_lightmap_fp_buffer) rsxFree(rsxgl_lightmap_fp_buffer);
	if (rsxgl_filter_fp_buffer) rsxFree(rsxgl_filter_fp_buffer);
	if (rsxgl_white_pixels) rsxFree(rsxgl_white_pixels);
	if (rsxgl_ui_local_vertex_arena)
	{
		rsxFree(rsxgl_ui_local_vertex_arena);
	}
	if (rsxgl_static_vertex_arena)
	{
		rsxFree(rsxgl_static_vertex_arena);
	}
	if (rsxgl_ui_stream_main_allocation)
	{
		if (rsxgl_ui_stream_main_mapped &&
			gcmUnmapEaIoAddress(rsxgl_ui_stream_main_allocation) != 0)
		{
			PS3_RUNTIME_TRACE("RSX renderer: main-memory 2D stream unmap failed");
		}
		free(rsxgl_ui_stream_main_allocation);
	}
	if (rsxgl_stream_main_allocation)
	{
		if (rsxgl_stream_main_mapped &&
			gcmUnmapEaIoAddress(rsxgl_stream_main_allocation) != 0)
		{
			PS3_RUNTIME_TRACE("RSX renderer: main-memory stream unmap failed");
		}
		free(rsxgl_stream_main_allocation);
	}
	else
	{
		if (rsxgl_index_arena) rsxFree(rsxgl_index_arena);
		if (rsxgl_world_vertex_arena)
		{
			rsxFree(rsxgl_world_vertex_arena);
		}
		else if (rsxgl_vertex_arena)
		{
			rsxFree(rsxgl_vertex_arena);
		}
	}
	if (rsxgl_readback_main_allocation)
	{
		if (rsxgl_readback_main_mapped &&
			gcmUnmapEaIoAddress(rsxgl_readback_main_allocation) != 0)
		{
			PS3_RUNTIME_TRACE("RSX renderer: screenshot readback unmap failed");
		}
		free(rsxgl_readback_main_allocation);
	}
	if (rsxgl_depth) rsxFree(rsxgl_depth);
	for (i = 0; i < RSXGL_DISPLAY_BUFFERS; i++)
	{
		if (rsxgl_color[i]) rsxFree(rsxgl_color[i]);
	}
	memset(rsxgl_color, 0, sizeof(rsxgl_color));
	rsxgl_depth = NULL;
	rsxgl_color_allocation_size = 0;
	rsxgl_depth_allocation_size = 0;
	rsxgl_tiled_targets_requested = 0;
	rsxgl_zcull_requested = 0;
	rsxgl_fp_buffer = NULL;
	rsxgl_lightmap_fp_buffer = NULL;
	rsxgl_filter_fp_buffer = NULL;
	rsxgl_white_pixels = NULL;
	rsxgl_index_arena = NULL;
	rsxgl_vertex_arena = NULL;
	rsxgl_vertex_arena_size = RSXGL_VERTEX_ARENA_SIZE;
	rsxgl_vertex_arena_offset = 0;
	rsxgl_index_arena_offset = 0;
	rsxgl_stream_main_allocation = NULL;
	rsxgl_stream_main_mapped = 0;
	rsxgl_stream_location = GCM_LOCATION_RSX;
	rsxgl_world_vertex_arena = NULL;
	rsxgl_world_vertex_arena_offset = 0;
	rsxgl_world_stream_location = GCM_LOCATION_RSX;
	rsxgl_world_vertex_arena_used = 0;
	rsxgl_static_vertex_arena = NULL;
	rsxgl_static_vertex_arena_offset = 0;
	rsxgl_static_vertex_arena_used = 0;
	rsxgl_static_vertex_page_offset = 0;
	rsxgl_static_vertex_page_used = 0;
	rsxgl_static_registered_vertices = 0;
	rsxgl_static_geometry_dirty = 0;
	rsxgl_ui_stream_main_allocation = NULL;
	rsxgl_ui_vertex_arena = NULL;
	rsxgl_ui_vertex_arena_offset = 0;
	rsxgl_ui_vertex_arena_used = 0;
	rsxgl_ui_vertex_arena_size = RSXGL_UI_VERTEX_ARENA_SIZE;
	rsxgl_ui_stream_main_mapped = 0;
	rsxgl_ui_stream_selected = 0;
	rsxgl_ui_local_vertex_arena = NULL;
	rsxgl_ui_local_vertex_arena_offset = 0;
	rsxgl_ui_local_vertex_arena_used = 0;
	rsxgl_ui_local_vertex_arena_size = RSXGL_UI_VERTEX_ARENA_SIZE;
	rsxgl_ui_stream_direct_local = 0;
	rsxgl_readback_main_allocation = NULL;
	rsxgl_readback_main_offset = 0;
	rsxgl_readback_mapping_size = 0;
	rsxgl_readback_main_mapped = 0;
	rsxgl_triangle_batch = NULL;
	rsxgl_triangle_batch_indices = NULL;
	rsxgl_triangle_batch_vertices = 0;
	rsxgl_triangle_batch_index_count = 0;
	rsxgl_triangle_batch_draws = 0;
	rsxgl_static_batch_indices = NULL;
	rsxgl_static_batch_vertex_offset = 0;
	rsxgl_static_batch_index_count = 0;
	rsxgl_static_batch_draws = 0;
	rsxgl_inline_2d_batch_active = 0;
	rsxgl_inline_2d_batch_vertices = 0;
	rsxgl_inline_2d_batch_color_valid = 0;
	rsxgl_initialized = 0;
	rsxgl_context = NULL;
}

void
RSXGL_BeginFrame(void)
{
	int i;
	int requested_cull_mode;
	int timing_enabled;
	unsigned long long wait_start_us;
	unsigned long long wait_elapsed_us;
	unsigned long long render_start_us;
	/* RI_BeginFrame calls this for every eye. Acquiring the frame here must be
	 * independent of the user-configurable separation sign (and must still work
	 * when depth is exactly zero), so make acquisition idempotent per frame. */
	if (!rsxgl_initialized || rsxgl_frame_active || rsxgl_frame_ready) return;
	if (Sys_PS3_CheckCallbacks()) return;
	rsxgl_stereo_eye_pass = 0;
	timing_enabled = rsxgl_cvar_stats && rsxgl_cvar_stats->value != 0.0f;
	wait_start_us = timing_enabled ? (unsigned long long)Sys_Microseconds() : 0;
	if (!rsxgl_traced_first_frame)
	{
		PS3_BOOT_TRACE("RSX renderer: waiting for initial display flip");
	}
	if (!rsxgl_wait_for_flip())
	{
		rsxgl_frame_active = 0;
		rsxgl_frame_ready = 0;
		return;
	}
	wait_elapsed_us = timing_enabled ?
		(unsigned long long)Sys_Microseconds() - wait_start_us : 0;
	render_start_us = timing_enabled ?
		(unsigned long long)Sys_Microseconds() : 0;
	if (!rsxgl_traced_first_frame)
	{
		PS3_BOOT_TRACE("RSX renderer: initial display flip complete");
	}
	/* The flip is ordered after every draw in the previous frame, so texture
	 * allocations retired by copy-on-update are no longer referenced. */
	rsxgl_release_retired_textures();
	rsxgl_draw_buffer ^= 1;
	/* Do not carry the previous stereo eye's raster rectangle into acquisition.
	 * SetStereoEye immediately narrows it again for frame-packed output. */
	rsxgl_target_y = 0;
	rsxgl_target_height = rsxgl_output_height;
	rsxgl_set_surface(rsxgl_draw_buffer);
	rsxgl_clear_acquired_surface();
	requested_cull_mode = rsxgl_requested_hardware_cull_mode();
	if (requested_cull_mode != rsxgl_hw_cull_mode)
	{
		rsxgl_hw_cull_mode = requested_cull_mode;
		rsxgl_apply_hardware_cull_state();
		rsxgl_trace_hardware_cull_mode();
	}
	rsxgl_world_vertex_arena_used = 0;
	/* Save the prior frame's selected stream before applying its flip-fenced
	 * wrap. EndFrame normally did this already; keeping the acquisition-side
	 * save makes the lifecycle robust if a future path leaves UI selected. */
	if (rsxgl_ui_stream_selected)
	{
		rsxgl_save_active_vertex_cursor();
	}
	/* The preceding flip wait fences the prior frame. Preserve the local staging
	 * cursor while another complete worst-case frame fits, giving physical RSX
	 * fresh vertex-fetch addresses across ordinary frames. The former staging
	 * path reset this destination to zero every vblank and therefore recreated
	 * the same-address cache-retention failure that the rotating XDR source was
	 * designed to remove. A fenced wrap is safe; the 8 MiB fallback retains its
	 * established single-frame behavior. */
	if (rsxgl_ui_local_vertex_arena_size > RSXGL_UI_VERTEX_ARENA_SIZE &&
		rsxgl_ui_local_vertex_arena_used >
			rsxgl_ui_local_vertex_arena_size - RSXGL_UI_VERTEX_ARENA_SIZE)
	{
		rsxgl_ui_local_vertex_arena_used = 0;
		if (!rsxgl_traced_ui_local_ring_wrap)
		{
			PS3_RUNTIME_TRACE("RSX renderer: rotating local 2D stage completed a flip-fenced wrap");
			rsxgl_traced_ui_local_ring_wrap = 1;
		}
	}
	else if (rsxgl_ui_local_vertex_arena_size <=
		RSXGL_UI_VERTEX_ARENA_SIZE)
	{
		rsxgl_ui_local_vertex_arena_used = 0;
	}
	/* Preserve the XDR UI write cursor across completed frames so new text uses
	 * different RSX fetch addresses instead of rewriting offset zero every
	 * vblank. The preceding flip wait guarantees every older UI fetch is
	 * complete before a boundary wrap reuses the first half of the mapping. */
	if (rsxgl_ui_vertex_arena_size > RSXGL_UI_VERTEX_ARENA_SIZE &&
		rsxgl_ui_vertex_arena_used >
			rsxgl_ui_vertex_arena_size - RSXGL_UI_VERTEX_ARENA_SIZE)
	{
		rsxgl_ui_vertex_arena_used = 0;
		if (!rsxgl_traced_ui_ring_wrap)
		{
			PS3_RUNTIME_TRACE("RSX renderer: rotating 2D stream completed a fenced wrap");
			rsxgl_traced_ui_ring_wrap = 1;
		}
	}
	else if (rsxgl_ui_vertex_arena_size <= RSXGL_UI_VERTEX_ARENA_SIZE)
	{
		/* Preserve the established single-frame behavior in the low-memory
		 * fallback; only the larger mapping rotates between frames. */
		rsxgl_ui_vertex_arena_used = 0;
	}
	rsxgl_ui_stream_selected = 0;
	rsxgl_vertex_arena = rsxgl_world_vertex_arena;
	rsxgl_vertex_arena_offset = rsxgl_world_vertex_arena_offset;
	rsxgl_vertex_arena_size = RSXGL_VERTEX_ARENA_SIZE;
	rsxgl_stream_location = rsxgl_world_stream_location;
	rsxgl_vertex_arena_used = 0;
	rsxgl_index_arena_used = 0;
	rsxgl_vertex_cache_needs_invalidate = 1;
	rsxgl_2d_mode = 0;
	rsxgl_direct_2d_clip = 0;
	rsxgl_triangle_batch = NULL;
	rsxgl_triangle_batch_indices = NULL;
	rsxgl_triangle_batch_vertices = 0;
	rsxgl_triangle_batch_index_count = 0;
	rsxgl_triangle_batch_draws = 0;
	rsxgl_static_batch_indices = NULL;
	rsxgl_static_batch_vertex_offset = 0;
	rsxgl_static_batch_index_count = 0;
	rsxgl_static_batch_draws = 0;
	rsxgl_frame_serial++;
	if (!rsxgl_frame_serial)
	{
		rsxgl_frame_serial = 1;
		for (i = 0; i < RSXGL_MAX_TEXTURES; i++)
		{
			rsxgl_textures[i].last_sampled_pixels = NULL;
			rsxgl_textures[i].last_sampled_frame = 0;
		}
	}
	rsxgl_triangle_batch_enabled = rsxgl_cvar_batch->value != 0.0f;
	rsxgl_indexed_batch_enabled = rsxgl_cvar_indexed_batch->value != 0.0f;
	rsxgl_state_filter_enabled = rsxgl_cvar_state_filter->value != 0.0f;
	rsxgl_fast_arrays_enabled = rsxgl_cvar_fast_arrays->value != 0.0f;
	rsxgl_begin_frame_stats();
	if (rsxgl_stats_enabled)
	{
		rsxgl_frame_wait_us = wait_elapsed_us;
		rsxgl_frame_render_start_us = render_start_us;
		if (rsxgl_stats_previous_begin_us &&
			render_start_us >= rsxgl_stats_previous_begin_us)
		{
			rsxgl_frame_cadence_us = render_start_us -
				rsxgl_stats_previous_begin_us;
		}
		rsxgl_stats_previous_begin_us = render_start_us;
	}
	rsxgl_frame_active = 1;
	rsxgl_frame_ready = 1;
	if (!rsxgl_traced_first_frame)
	{
		PS3_BOOT_TRACE("RSX renderer: first native frame begun");
	}
}

void
RSXGL_SetStereoEye(float camera_separation)
{
	if (!rsxgl_initialized) return;
	rsxgl_flush_triangle_batch();
	if (!rsxgl_frame_ready) return;
	rsxgl_reset_ui_string_signatures();

	if (rsxgl_frame_packed_output || rsxgl_top_bottom_output)
	{
		const int eye_height = rsxgl_frame_packed_output ?
			RSXGL_FRAME_PACKED_EYE_HEIGHT : rsxgl_output_height / 2;
		rsxgl_target_height = eye_height;
		/* Frame packing is top-eye then bottom-eye. Do not infer eye identity
		 * from separation: its sign is user configurable and depth zero makes
		 * both separations numerically identical. */
		if (rsxgl_stereo_eye_pass == 0)
		{
			rsxgl_target_y = 0;
			if (!rsxgl_traced_stereo_left)
			{
				PS3_RUNTIME_TRACE("RSX stereo: first eye routed to top frame");
				rsxgl_traced_stereo_left = 1;
			}
		}
		else
		{
			rsxgl_target_y = rsxgl_output_height - eye_height;
			if (!rsxgl_traced_stereo_right)
			{
				PS3_RUNTIME_TRACE("RSX stereo: second eye routed to bottom frame");
				rsxgl_traced_stereo_right = 1;
			}
		}
		if (rsxgl_stereo_eye_pass < 2) rsxgl_stereo_eye_pass++;
	}
	else
	{
		rsxgl_target_y = 0;
		rsxgl_target_height = rsxgl_output_height;
	}

	/* Eye changes occur between GL1 passes without rebinding the render
	 * surface. Reapply both raster rectangles immediately so clears and any
	 * state emitted before the next glViewport stay inside the selected eye. */
	rsxgl_apply_viewport(rsxgl_viewport_x, rsxgl_viewport_y,
		rsxgl_viewport_width, rsxgl_viewport_height);
	rsxgl_apply_scissor();
	(void)camera_separation;
}

void
RSXGL_EndFrame(void)
{
	if (!rsxgl_initialized || !rsxgl_frame_ready) return;
	rsxgl_flush_triangle_batch();
	rsxgl_save_active_vertex_cursor();
	if (rsxgl_stats_enabled && rsxgl_frame_render_start_us)
	{
		rsxgl_frame_render_us = (unsigned long long)Sys_Microseconds() -
			rsxgl_frame_render_start_us;
	}
	rsxgl_end_frame_stats();
	rsxgl_frame_active = 0;
	rsxgl_frame_ready = 0;
	/* Exit Game can arrive after the main-loop poll while a gameplay frame is
	 * being built. Observe it before queuing another display flip; the next
	 * main-loop iteration will enter the non-blocking process-exit teardown. */
	if (Sys_PS3_CheckCallbacks())
	{
		PS3_RUNTIME_TRACE("RSX renderer: XMB exit intercepted before frame flip");
		return;
	}
	/* Exact 30 Hz pacing submits this frame now and holds only the flip command
	 * until the intermediate VBlank. This gives both PPU command generation and
	 * RSX execution the full two-refresh budget. Other caps return immediately. */
	(void)GLimp_WaitForScheduledFlipQueue();
	if (Sys_PS3_CheckCallbacks())
	{
		PS3_RUNTIME_TRACE("RSX renderer: XMB exit intercepted during flip pacing");
		return;
	}
	if (!rsxgl_traced_first_frame)
	{
		PS3_BOOT_TRACE("RSX renderer: queueing first rendered flip");
	}
	if (gcmSetFlip(rsxgl_context, rsxgl_draw_buffer) == 0)
	{
		if (!rsxgl_traced_first_frame)
		{
			PS3_BOOT_TRACE("RSX renderer: first rendered flip accepted");
		}
		rsxFlushBuffer(rsxgl_context);
		if (!rsxgl_traced_first_frame)
		{
			PS3_BOOT_TRACE("RSX renderer: first rendered command buffer flushed");
		}
		gcmSetWaitFlip(rsxgl_context);
		if (!rsxgl_traced_first_frame)
		{
			PS3_RUNTIME_TRACE("RSX renderer: first native frame queued");
			rsxgl_traced_first_frame = 1;
		}
	}
	else
	{
		/* Without a queued flip there is no fence for renamed textures. Finish
		 * the submitted command stream before allowing those allocations to be
		 * recycled on the next BeginFrame. */
		rsxgl_finish();
		rsxgl_release_retired_textures();
		PS3_BOOT_TRACE("RSX renderer: flip queue failed; forced GPU finish");
	}
}

void RSXGL_SetVSync(int enabled)
{
	rsxgl_vsync = enabled != 0;
	gcmSetFlipMode(rsxgl_vsync ? GCM_FLIP_VSYNC : GCM_FLIP_HSYNC);
}

int RSXGL_IsVSync(void) { return rsxgl_vsync; }
int RSXGL_GetOutputWidth(void) { return rsxgl_output_width; }
int RSXGL_GetOutputHeight(void) { return rsxgl_output_height; }

void APIENTRY glAlphaFunc(GLenum func, GLfloat ref)
{
	u32 ref_state = (u32)(ref * 255.0f);
	if (rsxgl_state_filter_enabled && rsxgl_alpha_func_valid &&
		rsxgl_alpha_func_state == func &&
		rsxgl_alpha_ref_state == ref_state)
	{
		rsxgl_note_state_skip();
		return;
	}
	rsxgl_flush_triangle_batch();
	if (rsxgl_context) rsxSetAlphaFunc(rsxgl_context, rsxgl_compare(func),
		ref_state);
	rsxgl_alpha_func_state = func;
	rsxgl_alpha_ref_state = ref_state;
	rsxgl_alpha_func_valid = 1;
}

void APIENTRY glBegin(GLenum mode)
{
	rsxgl_inside_begin = 1;
	rsxgl_immediate_mode = mode;
	rsxgl_immediate_count = 0;
}

void APIENTRY glBindTexture(GLenum target, GLuint texture)
{
	(void)target;
	if (rsxgl_bound_texture != texture)
	{
		rsxgl_flush_triangle_batch();
		rsxgl_bound_texture = texture;
		rsxgl_sampler_dirty = 1;
	}
	else
	{
		rsxgl_note_state_skip();
	}
}

void APIENTRY glBlendFunc(GLenum sfactor, GLenum dfactor)
{
	if (rsxgl_state_filter_enabled && rsxgl_blend_func_valid &&
		rsxgl_blend_src_state == sfactor &&
		rsxgl_blend_dst_state == dfactor)
	{
		rsxgl_note_state_skip();
		return;
	}
	rsxgl_flush_triangle_batch();
	if (rsxgl_context) rsxSetBlendFunc(rsxgl_context, sfactor, dfactor,
		sfactor, dfactor);
	rsxgl_blend_src_state = sfactor;
	rsxgl_blend_dst_state = dfactor;
	rsxgl_blend_func_valid = 1;
}

void APIENTRY glClear(GLbitfield mask)
{
	u32 clear = 0;
	if (!rsxgl_context) return;
	rsxgl_flush_triangle_batch();
	if (rsxgl_frame_active && !rsxgl_traced_first_clear)
	{
		PS3_BOOT_TRACE("RSX first clear: entered");
	}
	if (mask & GL_COLOR_BUFFER_BIT)
		clear |= GCM_CLEAR_R | GCM_CLEAR_G | GCM_CLEAR_B | GCM_CLEAR_A;
	if ((mask & GL_COLOR_BUFFER_BIT) &&
		(rsxgl_frame_packed_output || rsxgl_top_bottom_output) &&
		rsxgl_target_height < rsxgl_output_height &&
		!rsxgl_traced_stereo_clear)
	{
		PS3_RUNTIME_TRACE("RSX stereo: per-eye color clearing active");
		rsxgl_traced_stereo_clear = 1;
	}
	if (mask & GL_DEPTH_BUFFER_BIT) clear |= GCM_CLEAR_Z;
	if (mask & GL_STENCIL_BUFFER_BIT) clear |= GCM_CLEAR_S;
	rsxSetClearColor(rsxgl_context, rsxgl_clear_color);
	if (rsxgl_frame_active && !rsxgl_traced_first_clear)
	{
		PS3_BOOT_TRACE("RSX first clear: color state queued");
	}
	rsxSetClearDepthStencil(rsxgl_context,
		(0xffffff << 8) | (rsxgl_clear_stencil & 0xff));
	if (rsxgl_frame_active && !rsxgl_traced_first_clear)
	{
		PS3_BOOT_TRACE("RSX first clear: depth state queued");
	}
	rsxClearSurface(rsxgl_context, clear);
	if (rsxgl_frame_active && !rsxgl_traced_first_clear)
	{
		PS3_BOOT_TRACE("RSX first clear: surface command queued");
		rsxgl_traced_first_clear = 1;
	}
}

void APIENTRY glClearColor(GLfloat red, GLfloat green, GLfloat blue, GLfloat alpha)
{
	u32 r = (u32)(red * 255.0f) & 0xff;
	u32 g = (u32)(green * 255.0f) & 0xff;
	u32 b = (u32)(blue * 255.0f) & 0xff;
	u32 a = (u32)(alpha * 255.0f) & 0xff;
	rsxgl_clear_color = (a << 24) | (r << 16) | (g << 8) | b;
}

void APIENTRY glClearStencil(GLint s) { rsxgl_clear_stencil = s & 0xff; }

void APIENTRY glColor4f(GLfloat red, GLfloat green, GLfloat blue, GLfloat alpha)
{
	rsxgl_current_color[0] = red;
	rsxgl_current_color[1] = green;
	rsxgl_current_color[2] = blue;
	rsxgl_current_color[3] = alpha;
}

void APIENTRY glColorMask(GLboolean red, GLboolean green, GLboolean blue, GLboolean alpha)
{
	u32 mask = 0;
	if (red) mask |= GCM_COLOR_MASK_R;
	if (green) mask |= GCM_COLOR_MASK_G;
	if (blue) mask |= GCM_COLOR_MASK_B;
	if (alpha) mask |= GCM_COLOR_MASK_A;
	if (rsxgl_state_filter_enabled && rsxgl_color_mask_state == mask)
	{
		rsxgl_note_state_skip();
		return;
	}
	rsxgl_flush_triangle_batch();
	if (rsxgl_context) rsxSetColorMask(rsxgl_context, mask);
	rsxgl_color_mask_state = mask;
}

void APIENTRY glColorPointer(GLint size, GLenum type, GLsizei stride, const GLvoid *pointer)
{
	rsxgl_color_array.pointer = pointer;
	rsxgl_color_array.size = size;
	rsxgl_color_array.type = type;
	rsxgl_color_array.stride = stride;
}

void APIENTRY glCullFace(GLenum mode)
{
	GLenum state = mode == GL_FRONT ? GL_FRONT : GL_BACK;
	if (rsxgl_state_filter_enabled && rsxgl_cull_face_state == state)
	{
		rsxgl_note_state_skip();
		return;
	}
	rsxgl_flush_triangle_batch();
	rsxgl_cull_face_state = state;
	rsxgl_apply_hardware_cull_state();
}

void APIENTRY glDeleteTextures(GLsizei n, const GLuint *textures)
{
	int i;
	int needs_finish = 0;
	for (i = 0; i < n; i++)
	{
		if (textures[i] < RSXGL_MAX_TEXTURES &&
			rsxgl_textures[textures[i]].pixels)
		{
			needs_finish = 1;
			break;
		}
	}
	if (needs_finish) rsxgl_finish();
	for (i = 0; i < n; i++)
	{
		GLuint id = textures[i];
		if (id < RSXGL_MAX_TEXTURES && rsxgl_textures[id].pixels)
		{
			rsxFree(rsxgl_textures[id].pixels);
			memset(&rsxgl_textures[id], 0, sizeof(rsxgl_textures[id]));
			if (id == rsxgl_bound_texture) rsxgl_sampler_dirty = 1;
			rsxgl_dirty_lightmap_sampler_for_texture(id);
		}
	}
}

void APIENTRY glDepthFunc(GLenum func)
{
	if (rsxgl_state_filter_enabled && rsxgl_depth_func_state == func)
	{
		rsxgl_note_state_skip();
		return;
	}
	rsxgl_flush_triangle_batch();
	if (rsxgl_context) rsxSetDepthFunc(rsxgl_context, rsxgl_compare(func));
	rsxgl_depth_func_state = func;
}

void APIENTRY glDepthMask(GLboolean flag)
{
	flag = flag ? GL_TRUE : GL_FALSE;
	if (rsxgl_state_filter_enabled && rsxgl_depth_mask_state == flag)
	{
		rsxgl_note_state_skip();
		return;
	}
	rsxgl_flush_triangle_batch();
	if (rsxgl_context) rsxSetDepthWriteEnable(rsxgl_context, flag ? GCM_TRUE : GCM_FALSE);
	rsxgl_depth_mask_state = flag;
}

void APIENTRY glDepthRange(GLdouble near_value, GLdouble far_value)
{
	float near_float = (float)near_value;
	float far_float = (float)far_value;
	if (rsxgl_state_filter_enabled && rsxgl_depth_near == near_float &&
		rsxgl_depth_far == far_float)
	{
		rsxgl_note_state_skip();
		return;
	}
	rsxgl_flush_triangle_batch();
	rsxgl_depth_near = near_float;
	rsxgl_depth_far = far_float;
	if (rsxgl_context)
	{
		rsxgl_apply_viewport(rsxgl_viewport_x, rsxgl_viewport_y,
			rsxgl_viewport_width, rsxgl_viewport_height);
	}
}

static void
rsxgl_set_cap(GLenum cap, int enabled)
{
	int *state = NULL;

	if (!rsxgl_context) return;
	switch (cap)
	{
		case GL_ALPHA_TEST: state = &rsxgl_alpha_test_enabled; break;
		case GL_BLEND: state = &rsxgl_blend_enabled; break;
		case GL_CULL_FACE: state = &rsxgl_cull_face_enabled; break;
		case GL_DEPTH_TEST: state = &rsxgl_depth_test_enabled; break;
		case GL_STENCIL_TEST: state = &rsxgl_stencil_test_enabled; break;
		case GL_SCISSOR_TEST: state = &rsxgl_scissor_enabled; break;
		case GL_POLYGON_OFFSET_FILL:
			state = &rsxgl_polygon_offset_fill_enabled;
			break;
		case GL_TEXTURE_2D: state = &rsxgl_texture_enabled; break;
		default: return;
	}
	if (rsxgl_state_filter_enabled && *state == enabled)
	{
		rsxgl_note_state_skip();
		return;
	}

	rsxgl_flush_triangle_batch();
	*state = enabled;
	switch (cap)
	{
		case GL_ALPHA_TEST:
			rsxSetAlphaTestEnable(rsxgl_context,
				enabled ? GCM_TRUE : GCM_FALSE);
			break;
		case GL_BLEND:
			rsxSetBlendEnable(rsxgl_context,
				enabled ? GCM_TRUE : GCM_FALSE);
			break;
		case GL_CULL_FACE:
			rsxgl_apply_hardware_cull_state();
			break;
		case GL_DEPTH_TEST:
			rsxSetDepthTestEnable(rsxgl_context,
				enabled ? GCM_TRUE : GCM_FALSE);
			break;
		case GL_STENCIL_TEST:
			rsxSetStencilTestEnable(rsxgl_context,
				enabled ? GCM_TRUE : GCM_FALSE);
			break;
		case GL_SCISSOR_TEST:
			rsxgl_apply_scissor();
			break;
		case GL_POLYGON_OFFSET_FILL:
			rsxSetPolygonOffsetFillEnable(rsxgl_context,
				enabled ? GCM_TRUE : GCM_FALSE);
			break;
		case GL_TEXTURE_2D:
			rsxgl_sampler_dirty = 1;
			break;
		default: break;
	}
}

void APIENTRY glDisable(GLenum cap)
{
	rsxgl_set_cap(cap, 0);
}

void APIENTRY glDisableClientState(GLenum array)
{
	if (array == GL_VERTEX_ARRAY) rsxgl_vertex_array.enabled = GL_FALSE;
	else if (array == GL_TEXTURE_COORD_ARRAY) rsxgl_texcoord_array.enabled = GL_FALSE;
	else if (array == GL_COLOR_ARRAY) rsxgl_color_array.enabled = GL_FALSE;
}

static size_t
rsxgl_array_type_size(GLenum type)
{
	switch (type)
	{
		case GL_BYTE:
		case GL_UNSIGNED_BYTE: return 1;
		case GL_SHORT:
		case GL_UNSIGNED_SHORT: return 2;
		case GL_FLOAT: return 4;
		default: return 0;
	}
}

static int
rsxgl_read_array(const rsxgl_array_t *array, int index, float *values,
	int value_count, int normalized)
{
	size_t type_size;
	size_t stride;
	const uint8_t *element;
	int component;

	if (!array || !array->pointer || !values || index < 0 ||
		array->size <= 0 || value_count <= 0 || array->stride < 0)
	{
		return 0;
	}
	type_size = rsxgl_array_type_size(array->type);
	if (!type_size) return 0;
	stride = array->stride ? (size_t)array->stride :
		(size_t)array->size * type_size;
	element = array->pointer + (size_t)index * stride;

	for (component = 0; component < array->size && component < value_count;
		component++)
	{
		const uint8_t *source = element + (size_t)component * type_size;
		switch (array->type)
		{
			case GL_BYTE:
			{
				signed char value;
				memcpy(&value, source, sizeof(value));
				values[component] = normalized ?
					(value == -128 ? -1.0f : (float)value / 127.0f) :
					(float)value;
				break;
			}
			case GL_UNSIGNED_BYTE:
			{
				unsigned char value;
				memcpy(&value, source, sizeof(value));
				values[component] = normalized ?
					(float)value / 255.0f : (float)value;
				break;
			}
			case GL_SHORT:
			{
				short value;
				memcpy(&value, source, sizeof(value));
				values[component] = normalized ?
					(value == -32768 ? -1.0f : (float)value / 32767.0f) :
					(float)value;
				break;
			}
			case GL_UNSIGNED_SHORT:
			{
				unsigned short value;
				memcpy(&value, source, sizeof(value));
				values[component] = normalized ?
					(float)value / 65535.0f : (float)value;
				break;
			}
			case GL_FLOAT:
				memcpy(&values[component], source, sizeof(values[component]));
				break;
			default:
				return 0;
		}
	}
	return 1;
}

static int
rsxgl_float_array_aligned(const rsxgl_array_t *array)
{
	return (((uintptr_t)array->pointer & (sizeof(float) - 1)) == 0) &&
		(array->stride == 0 ||
		((unsigned int)array->stride & (sizeof(float) - 1)) == 0);
}

static int
rsxgl_can_fast_float_arrays(void)
{
	if (!rsxgl_fast_arrays_enabled || !rsxgl_vertex_array.pointer ||
		rsxgl_vertex_array.type != GL_FLOAT ||
		(rsxgl_vertex_array.size != 2 && rsxgl_vertex_array.size != 3) ||
		rsxgl_vertex_array.stride < 0 ||
		!rsxgl_float_array_aligned(&rsxgl_vertex_array))
	{
		return 0;
	}
	if (rsxgl_texcoord_array.enabled &&
		(!rsxgl_texcoord_array.pointer ||
		rsxgl_texcoord_array.type != GL_FLOAT ||
		rsxgl_texcoord_array.size != 2 ||
		rsxgl_texcoord_array.stride < 0 ||
		!rsxgl_float_array_aligned(&rsxgl_texcoord_array)))
	{
		return 0;
	}
	if (rsxgl_color_array.enabled &&
		(!rsxgl_color_array.pointer || rsxgl_color_array.type != GL_FLOAT ||
		rsxgl_color_array.size != 4 || rsxgl_color_array.stride < 0 ||
		!rsxgl_float_array_aligned(&rsxgl_color_array)))
	{
		return 0;
	}
	return 1;
}

static const float *
rsxgl_float_array_element(const rsxgl_array_t *array, int index)
{
	size_t stride = array->stride ? (size_t)array->stride :
		(size_t)array->size * sizeof(float);
	return (const float *)(array->pointer + (size_t)index * stride);
}

void APIENTRY glDrawArrays(GLenum mode, GLint first, GLsizei count)
{
	rsxgl_vertex_t *vertices;
	u16 *indices = NULL;
	int output_count;
	int source_count;
	int pack_count;
	int base_vertex = 0;
	int batching;
	int indexed;
	int fast_arrays;
	int i;
	if (!rsxgl_traced_first_draw)
	{
		PS3_BOOT_TRACE("RSX first DrawArrays: entered");
	}
	if (!rsxgl_vertex_array.enabled || count <= 0 || first < 0)
		return;
	rsxgl_set_lightmap_program(0);
	output_count = rsxgl_triangle_vertex_count(mode, count);
	batching = output_count > 0;
	source_count = batching ?
		rsxgl_triangle_source_vertex_count(mode, count) : count;
	indexed = batching &&
		rsxgl_should_index_triangle_batch(mode, source_count);
	if (!batching)
	{
		output_count = count;
	}
	pack_count = indexed ? source_count : output_count;
	vertices = indexed ? rsxgl_reserve_indexed_triangle_batch(
		source_count, output_count, &indices, &base_vertex) :
		(batching ? rsxgl_reserve_triangle_batch(output_count) :
			rsxgl_allocate_vertices(output_count));
	if (!vertices) return;
	if (!rsxgl_traced_first_draw)
	{
		PS3_BOOT_TRACE("RSX first DrawArrays: vertex allocation complete");
	}
	fast_arrays = rsxgl_can_fast_float_arrays();
	if (fast_arrays)
	{
		for (i = 0; i < pack_count; i++)
		{
			int source_index = batching && !indexed ?
				rsxgl_triangle_source_index(mode, i) : i;
			int array_index = first + source_index;
			const float *position = rsxgl_float_array_element(
				&rsxgl_vertex_array, array_index);
			const float *texcoord = rsxgl_texcoord_array.enabled ?
				rsxgl_float_array_element(&rsxgl_texcoord_array, array_index) :
				rsxgl_current_texcoord;
			const float *color = rsxgl_color_array.enabled ?
				rsxgl_float_array_element(&rsxgl_color_array, array_index) :
				rsxgl_current_color;
			rsxgl_store_vertex(&vertices[i], position[0], position[1],
				rsxgl_vertex_array.size == 3 ? position[2] : 0.0f,
				texcoord[0], texcoord[1], color);
		}
		if (rsxgl_frame_ready)
		{
			rsxgl_frame_fast_array_vertices += pack_count;
		}
		if (!rsxgl_traced_fast_arrays)
		{
			PS3_RUNTIME_TRACE("RSX renderer: specialized float-array packing active");
			rsxgl_traced_fast_arrays = 1;
		}
	}
	else
	{
		for (i = 0; i < pack_count; i++)
		{
			int source_index = batching && !indexed ?
				rsxgl_triangle_source_index(mode, i) : i;
			float position[3] = {0.0f, 0.0f, 0.0f};
			float texcoord[2] = {rsxgl_current_texcoord[0], rsxgl_current_texcoord[1]};
			float color[4] = {rsxgl_current_color[0], rsxgl_current_color[1],
				rsxgl_current_color[2], rsxgl_current_color[3]};
			if (!rsxgl_read_array(&rsxgl_vertex_array, first + source_index,
				position, 3, 0))
				goto invalid_array;
			if (rsxgl_texcoord_array.enabled &&
				!rsxgl_read_array(&rsxgl_texcoord_array, first + source_index,
					texcoord, 2, 0))
				goto invalid_array;
			if (rsxgl_color_array.enabled &&
				!rsxgl_read_array(&rsxgl_color_array, first + source_index,
					color, 4, 1))
				goto invalid_array;
			rsxgl_store_vertex(&vertices[i], position[0], position[1], position[2],
				texcoord[0], texcoord[1], color);
		}
	}
	if (!rsxgl_traced_first_draw)
	{
		PS3_BOOT_TRACE("RSX first DrawArrays: vertex packing complete");
	}
	if (batching)
	{
		if (indexed)
		{
			for (i = 0; i < output_count; i++)
			{
				indices[i] = (u16)(base_vertex +
					rsxgl_triangle_source_index(mode, i));
			}
			rsxgl_note_api_draw(source_count, output_count, 1);
		}
		else
		{
			rsxgl_note_api_draw(output_count, 0, 1);
		}
		rsxgl_note_triangle_draw(indexed);
	}
	else
	{
		rsxgl_note_api_draw(output_count, 0, 0);
		rsxgl_submit_vertices(mode, vertices, output_count);
	}
	return;

invalid_array:
	if (indexed)
	{
		/* Restore both arenas to the start of this reservation. Earlier indexed
		 * draws in the same batch remain contiguous and valid. */
		rsxgl_vertex_arena_used = (size_t)((uint8_t *)vertices -
			(uint8_t *)rsxgl_vertex_arena);
		rsxgl_index_arena_used = (size_t)((uint8_t *)indices -
			(uint8_t *)rsxgl_index_arena);
		rsxgl_triangle_batch_vertices -= source_count;
		rsxgl_triangle_batch_index_count -= output_count;
		if (rsxgl_triangle_batch_vertices == 0)
		{
			rsxgl_triangle_batch = NULL;
			rsxgl_triangle_batch_indices = NULL;
		}
	}
	else if (batching)
	{
		/* Drop only this reservation. Any earlier compatible draws remain
		 * contiguous and valid. */
		rsxgl_vertex_arena_used = (size_t)((uint8_t *)vertices -
			(uint8_t *)rsxgl_vertex_arena);
		rsxgl_triangle_batch_vertices -= output_count;
		if (rsxgl_triangle_batch_vertices == 0)
		{
			rsxgl_triangle_batch = NULL;
		}
	}
}

void APIENTRY glDrawBuffer(GLenum mode) { (void)mode; }

void APIENTRY glEnable(GLenum cap)
{
	rsxgl_set_cap(cap, 1);
}

void APIENTRY glEnableClientState(GLenum array)
{
	if (array == GL_VERTEX_ARRAY) rsxgl_vertex_array.enabled = GL_TRUE;
	else if (array == GL_TEXTURE_COORD_ARRAY) rsxgl_texcoord_array.enabled = GL_TRUE;
	else if (array == GL_COLOR_ARRAY) rsxgl_color_array.enabled = GL_TRUE;
}

void APIENTRY glEnd(void)
{
	if (!rsxgl_inside_begin) return;
	rsxgl_draw_vertices(rsxgl_immediate_mode, rsxgl_immediate,
		rsxgl_immediate_count);
	rsxgl_inside_begin = 0;
	rsxgl_immediate_count = 0;
}

void APIENTRY glFinish(void) { rsxgl_finish(); }

void APIENTRY glFrustum(GLdouble left, GLdouble right, GLdouble bottom, GLdouble top, GLdouble near_value, GLdouble far_value)
{
	float matrix[16];
	float *current = rsxgl_current_matrix();
	rsxgl_flush_triangle_batch();
	rsxgl_select_vertex_stream(0);
	rsxgl_2d_mode = 0;
	rsxgl_direct_2d_clip = 0;
	memset(matrix, 0, sizeof(matrix));
	matrix[0] = (2.0 * near_value) / (right - left);
	matrix[5] = (2.0 * near_value) / (top - bottom);
	matrix[8] = (right + left) / (right - left);
	matrix[9] = (top + bottom) / (top - bottom);
	matrix[10] = -(far_value + near_value) / (far_value - near_value);
	matrix[11] = -1.0f;
	matrix[14] = -(2.0 * far_value * near_value) / (far_value - near_value);
	rsxgl_multiply(current, current, matrix);
	rsxgl_touch_matrix();
}

void APIENTRY glGetFloatv(GLenum pname, GLfloat *params)
{
	if (!params) return;
	if (pname == GL_MODELVIEW_MATRIX)
		memcpy(params, rsxgl_modelview[rsxgl_modelview_top], sizeof(float) * 16);
	else if (pname == GL_MAX_TEXTURE_MAX_ANISOTROPY_EXT)
		*params = 16.0f;
}

const GLubyte *APIENTRY glGetString(GLenum name)
{
	switch (name)
	{
		case GL_VENDOR: return (const GLubyte *)"Sony/NVIDIA";
		case GL_RENDERER: return (const GLubyte *)"PlayStation 3 RSX (native PSL1GHT)";
		case GL_VERSION: return (const GLubyte *)"1.4 RSX compatibility";
		case GL_EXTENSIONS: return (const GLubyte *)
			"GL_ARB_texture_non_power_of_two GL_EXT_texture_filter_anisotropic";
		default: return (const GLubyte *)"";
	}
}

void APIENTRY glHint(GLenum target, GLenum mode) { (void)target; (void)mode; }
void APIENTRY glLoadIdentity(void)
{
	float identity[16];
	float *current = rsxgl_current_matrix();
	rsxgl_identity(identity);
	if (rsxgl_state_filter_enabled &&
		memcmp(current, identity, sizeof(identity)) == 0)
	{
		rsxgl_note_state_skip();
		return;
	}
	rsxgl_flush_triangle_batch();
	memcpy(current, identity, sizeof(identity));
	rsxgl_touch_matrix();
}
void APIENTRY glLoadMatrixf(const GLfloat *matrix)
{
	float *current;
	if (!matrix) return;
	current = rsxgl_current_matrix();
	if (rsxgl_state_filter_enabled &&
		memcmp(current, matrix, sizeof(float) * 16) == 0)
	{
		rsxgl_note_state_skip();
		return;
	}
	rsxgl_flush_triangle_batch();
	memcpy(current, matrix, sizeof(float) * 16);
	rsxgl_touch_matrix();
}
void APIENTRY glMatrixMode(GLenum mode) { rsxgl_matrix_mode = mode; }

void
RSXGL_Set2DProjection(int width, int height)
{
	float identity[16];
	float *current_projection;
	float *current_modelview;

	if (width <= 0 || height <= 0)
	{
		return;
	}
	/* Convert native UI coordinates to NDC at vertex emission and use an
	 * identity shader transform.  The RSX viewport's negative Y scale then maps
	 * clip +1 to the physical top and -1 to the bottom exactly once.  Unlike an
	 * uploaded ortho constant this cannot retain the boot-time canvas size or a
	 * world matrix across a frame-mode transition. */
	rsxgl_identity(identity);
	current_projection = rsxgl_projection[rsxgl_projection_top];
	current_modelview = rsxgl_modelview[rsxgl_modelview_top];

	/* Selecting UI first preserves the normal world->UI ordering and stream
	 * cursor. If the exact matrices are already resident, the complete repeated
	 * LoadIdentity/Ortho/LoadIdentity sequence becomes one filtered no-op. */
	rsxgl_select_vertex_stream(1);
	rsxgl_2d_mode = 1;
	rsxgl_direct_2d_clip = 1;
	rsxgl_direct_2d_scale_x = 2.0f / (float)width;
	rsxgl_direct_2d_scale_y = 2.0f / (float)height;
	rsxgl_matrix_mode = GL_MODELVIEW;
	if (rsxgl_state_filter_enabled &&
		memcmp(current_projection, identity, sizeof(identity)) == 0 &&
		memcmp(current_modelview, identity, sizeof(identity)) == 0)
	{
		if (rsxgl_stats_enabled)
		{
			rsxgl_frame_ortho_hits++;
		}
		rsxgl_note_state_skip();
		if (!rsxgl_traced_2d_projection_cache)
		{
			PS3_RUNTIME_TRACE(
				"RSX renderer: exact native 2D projection reuse active");
			rsxgl_traced_2d_projection_cache = 1;
		}
		return;
	}

	rsxgl_flush_triangle_batch();
	memcpy(current_projection, identity, sizeof(identity));
	memcpy(current_modelview, identity, sizeof(identity));
	rsxgl_touch_matrix();
	if (!rsxgl_traced_deterministic_2d)
	{
		PS3_RUNTIME_TRACE(
			"RSX renderer: deterministic non-indexed 2D stream active");
		rsxgl_traced_deterministic_2d = 1;
	}
}

void APIENTRY glOrtho(GLdouble left, GLdouble right, GLdouble bottom, GLdouble top, GLdouble near_value, GLdouble far_value)
{
	float matrix[16];
	float *current = rsxgl_current_matrix();
	rsxgl_flush_triangle_batch();
	rsxgl_select_vertex_stream(1);
	rsxgl_2d_mode = 1;
	rsxgl_direct_2d_clip = 0;
	if (!rsxgl_traced_deterministic_2d)
	{
		PS3_RUNTIME_TRACE("RSX renderer: deterministic non-indexed 2D stream active");
		rsxgl_traced_deterministic_2d = 1;
	}
	rsxgl_build_ortho(matrix, left, right, bottom, top,
		near_value, far_value);
	rsxgl_multiply(current, current, matrix);
	rsxgl_touch_matrix();
}

void APIENTRY glPixelStorei(GLenum pname, GLint param) { (void)pname; (void)param; }
void APIENTRY glPointSize(GLfloat size)
{
	if (rsxgl_state_filter_enabled && rsxgl_point_size_valid &&
		rsxgl_point_size_state == size)
	{
		rsxgl_note_state_skip();
		return;
	}
	rsxgl_flush_triangle_batch();
	if (rsxgl_context) rsxSetPointSize(rsxgl_context, size);
	rsxgl_point_size_state = size;
	rsxgl_point_size_valid = 1;
}

void APIENTRY glPolygonMode(GLenum face, GLenum mode)
{
	u32 rsx_mode = mode == GL_LINE ? GCM_POLYGON_MODE_LINE :
		(mode == GL_POINT ? GCM_POLYGON_MODE_POINT : GCM_POLYGON_MODE_FILL);
	int set_front = face == GL_FRONT || face == GL_FRONT_AND_BACK;
	int set_back = face == GL_BACK || face == GL_FRONT_AND_BACK;
	int front_changed = set_front && (!rsxgl_state_filter_enabled ||
		!rsxgl_front_polygon_mode_valid ||
		rsxgl_front_polygon_mode_state != mode);
	int back_changed = set_back && (!rsxgl_state_filter_enabled ||
		!rsxgl_back_polygon_mode_valid ||
		rsxgl_back_polygon_mode_state != mode);
	if (!rsxgl_context) return;
	if (!front_changed && !back_changed)
	{
		rsxgl_note_state_skip();
		return;
	}
	rsxgl_flush_triangle_batch();
	if (front_changed)
	{
		rsxSetFrontPolygonMode(rsxgl_context, rsx_mode);
		rsxgl_front_polygon_mode_state = mode;
		rsxgl_front_polygon_mode_valid = 1;
	}
	if (back_changed)
	{
		rsxSetBackPolygonMode(rsxgl_context, rsx_mode);
		rsxgl_back_polygon_mode_state = mode;
		rsxgl_back_polygon_mode_valid = 1;
	}
}

void APIENTRY glPolygonOffset(GLfloat factor, GLfloat units)
{
	if (rsxgl_state_filter_enabled && rsxgl_polygon_offset_valid &&
		rsxgl_polygon_offset_factor_state == factor &&
		rsxgl_polygon_offset_units_state == units)
	{
		rsxgl_note_state_skip();
		return;
	}
	rsxgl_flush_triangle_batch();
	if (rsxgl_context) rsxSetPolygonOffset(rsxgl_context, factor, units);
	rsxgl_polygon_offset_factor_state = factor;
	rsxgl_polygon_offset_units_state = units;
	rsxgl_polygon_offset_valid = 1;
}

void APIENTRY glPopMatrix(void)
{
	if (rsxgl_matrix_mode == GL_PROJECTION)
	{
		if (rsxgl_projection_top > 0)
		{
			rsxgl_flush_triangle_batch();
			rsxgl_projection_top--;
			rsxgl_touch_matrix();
		}
	}
	else if (rsxgl_modelview_top > 0)
	{
		rsxgl_flush_triangle_batch();
		rsxgl_modelview_top--;
		rsxgl_touch_matrix();
	}
}

void APIENTRY glPushMatrix(void)
{
	if (rsxgl_matrix_mode == GL_PROJECTION)
	{
		if (rsxgl_projection_top + 1 < RSXGL_MATRIX_DEPTH)
		{
			memcpy(rsxgl_projection[rsxgl_projection_top + 1],
				rsxgl_projection[rsxgl_projection_top], sizeof(float) * 16);
			rsxgl_projection_top++;
		}
	}
	else if (rsxgl_modelview_top + 1 < RSXGL_MATRIX_DEPTH)
	{
		memcpy(rsxgl_modelview[rsxgl_modelview_top + 1],
			rsxgl_modelview[rsxgl_modelview_top], sizeof(float) * 16);
		rsxgl_modelview_top++;
	}
}

void APIENTRY glReadPixels(GLint x, GLint y, GLsizei width, GLsizei height, GLenum format, GLenum type, GLvoid *pixels)
{
	uint8_t *destination = pixels;
	const uint8_t *readback;
	int components = format == GL_RGBA ? 4 : 3;
	int readback_target_y = rsxgl_target_y;
	int readback_height = rsxgl_output_height;
	u32 readback_source_offset;
	int row;
	int column;
	(void)type;
	if (!destination || !rsxgl_color[rsxgl_draw_buffer] ||
		width <= 0 || height <= 0) return;
	if (rsxgl_frame_packed_output || rsxgl_top_bottom_output)
	{
		/* Diagnostic override for comparing the two packed render regions with
		 * Quake's ordinary screenshot command. Zero retains the active-eye
		 * behavior; one selects the top eye and two the bottom eye. */
		int eye = rsxgl_cvar_stereo_screenshot_eye ?
			(int)rsxgl_cvar_stereo_screenshot_eye->value : 0;
		if (eye == 1)
			readback_target_y = 0;
		else if (eye == 2)
			readback_target_y = rsxgl_output_height -
				rsxgl_target_height;
		readback_height = rsxgl_target_height;
	}
	/* Keep the color allocation's tile base as the source offset and express
	 * eye/row selection through the blitter's source coordinates. Rebasing a
	 * tiled allocation by pitch treats its physical layout as linear and loses
	 * the tile-relative address needed by the RSX transfer engine. */
	readback_source_offset = rsxgl_color_offset[rsxgl_draw_buffer];
	if (rsxgl_readback_main_mapped)
	{
		/* The transfer is ordered after all queued draws. Discard any PPU cache
		 * lines for the destination first, then wait for the DMA before reading.
		 * Use the surface blit engine rather than the raw memory-to-memory engine:
		 * the source is an active color surface and requires RSX render-target
		 * cache/layout handling before it can be consumed by the PPU. */
		rsxgl_flush_triangle_batch();
		rsxgl_invalidate_ppu_cache_for_gpu_write(
			rsxgl_readback_main_allocation,
			(size_t)rsxgl_color_pitch * readback_height);
		if (!rsxgl_traced_readback_dma)
			PS3_RUNTIME_TRACE("RSX renderer: screenshot surface readback queued");
		/* An active render target can retain completed fragments in the 3D backend
		 * after ordinary command ordering. Drain that backend before the independent
		 * 2D transfer engine consumes the surface; the following finish still waits
		 * for the transfer itself before the PPU reads the XDR staging surface. */
		rsxSetWaitForIdle(rsxgl_context);
		rsxSetTransferImage(rsxgl_context, GCM_TRANSFER_LOCAL_TO_MAIN,
			rsxgl_readback_main_offset, rsxgl_color_pitch, 0, 0,
			readback_source_offset, rsxgl_color_pitch, 0, readback_target_y,
			rsxgl_output_width, readback_height, 4);
		rsxgl_finish();
		__sync_synchronize();
		readback = (const uint8_t *)rsxgl_readback_main_allocation;
		if (!rsxgl_traced_readback_dma)
		{
			PS3_RUNTIME_TRACE("RSX renderer: screenshot surface readback completed");
			rsxgl_traced_readback_dma = 1;
		}
	}
	else
	{
		/* Retain the old direct path as a best-effort fallback if the XDR map
		 * could not be reserved. It is not assumed to be coherent on hardware. */
		rsxgl_finish();
		readback = (const uint8_t *)rsxgl_color[rsxgl_draw_buffer] +
			(size_t)readback_target_y * rsxgl_color_pitch;
	}
	/* The selected packed eye is copied to row zero in the staging surface (or
	 * rebased to row zero in the direct fallback), so the sampling loop no
	 * longer crosses the HDMI blanking gap or a 1024-line blit boundary. */
	readback_target_y = 0;
	if ((rsxgl_frame_packed_output || rsxgl_top_bottom_output) &&
		!rsxgl_traced_stereo_readback)
	{
		PS3_BOOT_TRACE("RSX stereo: screenshot readback uses active eye");
		rsxgl_traced_stereo_readback = 1;
	}
	for (row = 0; row < height; row++)
	{
		int virtual_y = y + row;
		int source_y;
		if (virtual_y < 0) virtual_y = 0;
		if (virtual_y >= rsxgl_virtual_height)
			virtual_y = rsxgl_virtual_height - 1;
		source_y = readback_target_y + rsxgl_target_height - 1 -
			(virtual_y * rsxgl_target_height / rsxgl_virtual_height);
		const u32 *source = (const u32 *)(readback +
			source_y * rsxgl_color_pitch);
		for (column = 0; column < width; column++)
		{
			int virtual_x = x + column;
			int source_x;
			if (virtual_x < 0) virtual_x = 0;
			if (virtual_x >= rsxgl_virtual_width)
				virtual_x = rsxgl_virtual_width - 1;
			source_x = virtual_x * rsxgl_output_width / rsxgl_virtual_width;
			u32 color = source[source_x];
			*destination++ = (color >> 16) & 0xff;
			*destination++ = (color >> 8) & 0xff;
			*destination++ = color & 0xff;
			if (components == 4) *destination++ = (color >> 24) & 0xff;
		}
	}
}

void APIENTRY glRotatef(GLfloat angle, GLfloat x, GLfloat y, GLfloat z)
{
	float matrix[16];
	float length_squared;
	float radians;
	float c;
	float s;
	float one_minus_c;
	float *current;

	if (angle == 0.0f)
	{
		if (!rsxgl_traced_noop_transform)
		{
			PS3_RUNTIME_TRACE("RSX renderer: no-op transform filtering active");
			rsxgl_traced_noop_transform = 1;
		}
		return;
	}
	length_squared = x * x + y * y + z * z;
	if (length_squared == 0.0f) return;
	current = rsxgl_current_matrix();
	rsxgl_flush_triangle_batch();
	if (length_squared != 1.0f)
	{
		float length = sqrtf(length_squared);
		x /= length;
		y /= length;
		z /= length;
	}
	radians = angle * (float)M_PI / 180.0f;
	c = cosf(radians); s = sinf(radians); one_minus_c = 1.0f - c;
	rsxgl_identity(matrix);
	matrix[0] = x*x*one_minus_c + c;
	matrix[4] = x*y*one_minus_c - z*s;
	matrix[8] = x*z*one_minus_c + y*s;
	matrix[1] = y*x*one_minus_c + z*s;
	matrix[5] = y*y*one_minus_c + c;
	matrix[9] = y*z*one_minus_c - x*s;
	matrix[2] = z*x*one_minus_c - y*s;
	matrix[6] = z*y*one_minus_c + x*s;
	matrix[10] = z*z*one_minus_c + c;
	rsxgl_multiply(current, current, matrix);
	rsxgl_touch_matrix();
}

void APIENTRY glScalef(GLfloat x, GLfloat y, GLfloat z)
{
	float matrix[16];
	float *current;
	if (x == 1.0f && y == 1.0f && z == 1.0f)
	{
		if (!rsxgl_traced_noop_transform)
		{
			PS3_RUNTIME_TRACE("RSX renderer: no-op transform filtering active");
			rsxgl_traced_noop_transform = 1;
		}
		return;
	}
	current = rsxgl_current_matrix();
	rsxgl_flush_triangle_batch();
	rsxgl_identity(matrix);
	matrix[0] = x; matrix[5] = y; matrix[10] = z;
	rsxgl_multiply(current, current, matrix);
	rsxgl_touch_matrix();
}

void APIENTRY glScissor(GLint x, GLint y, GLsizei width, GLsizei height)
{
	if (rsxgl_state_filter_enabled &&
		rsxgl_scissor_x == x && rsxgl_scissor_y == y &&
		rsxgl_scissor_width == width && rsxgl_scissor_height == height)
	{
		rsxgl_note_state_skip();
		return;
	}
	if (rsxgl_scissor_enabled)
	{
		rsxgl_flush_triangle_batch();
	}
	rsxgl_scissor_x = x;
	rsxgl_scissor_y = y;
	rsxgl_scissor_width = width;
	rsxgl_scissor_height = height;
	if (rsxgl_context && rsxgl_scissor_enabled) rsxgl_apply_scissor();
}

void APIENTRY glShadeModel(GLenum mode)
{
	if (rsxgl_state_filter_enabled && rsxgl_shade_model_valid &&
		rsxgl_shade_model_state == mode)
	{
		rsxgl_note_state_skip();
		return;
	}
	rsxgl_flush_triangle_batch();
	if (rsxgl_context) rsxSetShadeModel(rsxgl_context,
		mode == GL_FLAT ? GCM_SHADE_MODEL_FLAT : GCM_SHADE_MODEL_SMOOTH);
	rsxgl_shade_model_state = mode;
	rsxgl_shade_model_valid = 1;
}

void APIENTRY glStencilFunc(GLenum func, GLint ref, GLuint mask)
{
	if (rsxgl_state_filter_enabled && rsxgl_stencil_func_valid &&
		rsxgl_stencil_func_state == func &&
		rsxgl_stencil_ref_state == ref &&
		rsxgl_stencil_func_mask_state == mask)
	{
		rsxgl_note_state_skip();
		return;
	}
	rsxgl_flush_triangle_batch();
	if (rsxgl_context) rsxSetStencilFunc(rsxgl_context, rsxgl_compare(func), ref, mask);
	rsxgl_stencil_func_state = func;
	rsxgl_stencil_ref_state = ref;
	rsxgl_stencil_func_mask_state = mask;
	rsxgl_stencil_func_valid = 1;
}
void APIENTRY glStencilMask(GLuint mask)
{
	if (rsxgl_state_filter_enabled && rsxgl_stencil_mask_valid &&
		rsxgl_stencil_write_mask_state == mask)
	{
		rsxgl_note_state_skip();
		return;
	}
	rsxgl_flush_triangle_batch();
	if (rsxgl_context) rsxSetStencilMask(rsxgl_context, mask);
	rsxgl_stencil_write_mask_state = mask;
	rsxgl_stencil_mask_valid = 1;
}
void APIENTRY glStencilOp(GLenum fail, GLenum zfail, GLenum zpass)
{
	if (rsxgl_state_filter_enabled && rsxgl_stencil_op_valid &&
		rsxgl_stencil_fail_state == fail &&
		rsxgl_stencil_zfail_state == zfail &&
		rsxgl_stencil_zpass_state == zpass)
	{
		rsxgl_note_state_skip();
		return;
	}
	rsxgl_flush_triangle_batch();
	if (rsxgl_context) rsxSetStencilOp(rsxgl_context, fail, zfail, zpass);
	rsxgl_stencil_fail_state = fail;
	rsxgl_stencil_zfail_state = zfail;
	rsxgl_stencil_zpass_state = zpass;
	rsxgl_stencil_op_valid = 1;
}

void APIENTRY glTexCoordPointer(GLint size, GLenum type, GLsizei stride, const GLvoid *pointer)
{
	rsxgl_texcoord_array.pointer = pointer;
	rsxgl_texcoord_array.size = size;
	rsxgl_texcoord_array.type = type;
	rsxgl_texcoord_array.stride = stride;
}
void APIENTRY glTexEnvf(GLenum target, GLenum pname, GLfloat param)
{
	(void)target;
	if (pname == GL_TEXTURE_ENV_MODE && rsxgl_texture_env != (GLint)param)
	{
		rsxgl_flush_triangle_batch();
		rsxgl_texture_env = param;
	}
	else if (pname == 0x8573 /* GL_RGB_SCALE_EXT */ &&
		rsxgl_rgb_scale != param)
	{
		rsxgl_flush_triangle_batch();
		rsxgl_rgb_scale = param;
	}
	else if (pname == GL_TEXTURE_ENV_MODE ||
		pname == 0x8573 /* GL_RGB_SCALE_EXT */)
	{
		rsxgl_note_state_skip();
	}
}
void APIENTRY glTexEnvi(GLenum target, GLenum pname, GLint param)
{
	(void)target;
	if (pname == GL_TEXTURE_ENV_MODE && rsxgl_texture_env != param)
	{
		rsxgl_flush_triangle_batch();
		rsxgl_texture_env = param;
	}
	else if (pname == 0x8573 /* GL_RGB_SCALE_EXT */ &&
		rsxgl_rgb_scale != (float)param)
	{
		rsxgl_flush_triangle_batch();
		rsxgl_rgb_scale = param;
	}
	else if (pname == GL_TEXTURE_ENV_MODE ||
		pname == 0x8573 /* GL_RGB_SCALE_EXT */)
	{
		rsxgl_note_state_skip();
	}
}

static void
rsxgl_copy_texture_pixels(rsxgl_texture_t *slot, int level,
	int xoffset, int yoffset,
	int width, int height, GLenum format, const GLvoid *pixels)
{
	const uint8_t *source = pixels;
	int source_components = format == GL_RGBA ? 4 : (format == GL_RGB ? 3 : 1);
	size_t level_offset;
	int level_width;
	int level_height;
	int y;
	int x;
	if (!pixels || !slot->pixels ||
		!rsxgl_mipmap_dimensions(slot, level, &level_width, &level_height) ||
		xoffset < 0 || yoffset < 0 ||
		xoffset + width > level_width || yoffset + height > level_height) return;
	level_offset = rsxgl_mipmap_offset(slot, level);
	for (y = 0; y < height; y++)
	{
		uint8_t *destination = (uint8_t *)slot->pixels + level_offset +
			(y + yoffset) * slot->pitch + xoffset * 4;
		for (x = 0; x < width; x++)
		{
			const uint8_t *pixel = source + (y * width + x) * source_components;
			uint8_t r = pixel[0];
			uint8_t g = source_components > 1 ? pixel[1] : pixel[0];
			uint8_t b = source_components > 1 ? pixel[2] : pixel[0];
			uint8_t a = source_components > 3 ? pixel[3] : 255;
			destination[x * 4 + 0] = a;
			destination[x * 4 + 1] = r;
			destination[x * 4 + 2] = g;
			destination[x * 4 + 3] = b;
		}
	}
}

static int
rsxgl_expand_mipmap_storage(rsxgl_texture_t *slot)
{
	void *replacement;
	void *old_pixels;
	u32 replacement_offset;
	int capacity;
	size_t size;
	if (!slot || !slot->pixels) return 0;
	capacity = rsxgl_full_mipmap_count(slot->width, slot->height);
	if (slot->mipmap_capacity >= capacity) return 1;
	size = rsxgl_mipmap_storage_size(slot->pitch, slot->height, capacity);
	if (!size) return 0;
	replacement = rsxMemalign(128, size);
	if (!replacement) return 0;
	if (rsxAddressToOffset(replacement, &replacement_offset) != 0)
	{
		rsxFree(replacement);
		return 0;
	}
	if (slot->ever_applied)
	{
		rsxgl_finish();
		rsxgl_release_retired_textures();
	}
	memset(replacement, 0xff, size);
	memcpy(replacement, slot->pixels, slot->allocation_size);
	old_pixels = slot->pixels;
	slot->pixels = replacement;
	slot->offset = replacement_offset;
	slot->allocation_size = size;
	slot->mipmap_capacity = capacity;
	slot->ever_applied = GL_FALSE;
	slot->last_sampled_pixels = NULL;
	slot->last_sampled_frame = 0;
	rsxFree(old_pixels);
	return 1;
}

static void
rsxgl_update_mipmap_count(rsxgl_texture_t *slot)
{
	int levels = 0;
	while (levels < slot->mipmap_capacity &&
		(slot->uploaded_mipmap_mask & (1u << levels))) levels++;
	slot->mipmap_levels = levels > 0 ? levels : 1;
}

static void
rsxgl_generate_mipmaps(rsxgl_texture_t *slot)
{
	int level;
	if (!slot || !slot->pixels) return;
	if (slot->ever_applied)
	{
		rsxgl_finish();
		rsxgl_release_retired_textures();
		slot->ever_applied = GL_FALSE;
		slot->last_sampled_pixels = NULL;
		slot->last_sampled_frame = 0;
	}
	if (!rsxgl_expand_mipmap_storage(slot)) return;
	for (level = 1; level < slot->mipmap_capacity; level++)
	{
		int source_width;
		int source_height;
		int width;
		int height;
		int x;
		int y;
		uint8_t *source;
		uint8_t *destination;
		rsxgl_mipmap_dimensions(slot, level - 1, &source_width, &source_height);
		rsxgl_mipmap_dimensions(slot, level, &width, &height);
		source = (uint8_t *)slot->pixels + rsxgl_mipmap_offset(slot, level - 1);
		destination = (uint8_t *)slot->pixels + rsxgl_mipmap_offset(slot, level);
		for (y = 0; y < height; y++)
		{
			for (x = 0; x < width; x++)
			{
				unsigned int sum[4] = {0, 0, 0, 0};
				unsigned int samples = 0;
				int source_x;
				int source_y;
				int component;
				for (source_y = y * 2;
					source_y < y * 2 + 2 && source_y < source_height;
					source_y++)
				{
					for (source_x = x * 2;
						source_x < x * 2 + 2 && source_x < source_width;
						source_x++)
					{
						const uint8_t *pixel = source +
							source_y * slot->pitch + source_x * 4;
						for (component = 0; component < 4; component++)
							sum[component] += pixel[component];
						samples++;
					}
				}
				for (component = 0; component < 4; component++)
					destination[y * slot->pitch + x * 4 + component] =
						(uint8_t)(sum[component] / samples);
			}
		}
	}
	slot->uploaded_mipmap_mask = (1u << slot->mipmap_capacity) - 1u;
	slot->mipmap_levels = slot->mipmap_capacity;
	rsxgl_texture_cache_dirty = 1;
	rsxgl_sampler_dirty = 1;
	rsxgl_dirty_lightmap_sampler_for_texture(
		(GLuint)(slot - rsxgl_textures));
	if (slot->mipmap_levels > 1 && !rsxgl_traced_generated_mipmap)
	{
		PS3_RUNTIME_TRACE("RSX renderer: CPU generated native mip chain");
		rsxgl_traced_generated_mipmap = 1;
	}
}

void APIENTRY glTexImage2D(GLenum target, GLint level, GLint internalformat,
	GLsizei width, GLsizei height, GLint border, GLenum format, GLenum type,
	const GLvoid *pixels)
{
	rsxgl_texture_t *slot;
	size_t size;
	(void)target; (void)internalformat; (void)border; (void)type;
	if (level < 0 || level >= RSXGL_MAX_MIP_LEVELS ||
		rsxgl_bound_texture >= RSXGL_MAX_TEXTURES ||
		width <= 0 || height <= 0 || width > 4096 || height > 4096) return;
	rsxgl_flush_triangle_batch();
	slot = &rsxgl_textures[rsxgl_bound_texture];
	rsxgl_initialize_texture_parameters(slot);
	/* Storage, offsets, mip counts, or pixels may change below. */
	rsxgl_sampler_dirty = 1;
	rsxgl_texture_cache_dirty = 1;
	rsxgl_dirty_lightmap_sampler_for_texture(rsxgl_bound_texture);
	if (level > 0)
	{
		int expected_width;
		int expected_height;
		if (!slot->pixels ||
			!rsxgl_mipmap_dimensions(slot, level, &expected_width, &expected_height) ||
			width != expected_width || height != expected_height ||
			!rsxgl_expand_mipmap_storage(slot)) return;
		if (slot->ever_applied)
		{
			rsxgl_finish();
			rsxgl_release_retired_textures();
			slot->ever_applied = GL_FALSE;
			slot->last_sampled_pixels = NULL;
			slot->last_sampled_frame = 0;
		}
		rsxgl_copy_texture_pixels(slot, level, 0, 0, width, height,
			format, pixels);
		slot->uploaded_mipmap_mask |= 1u << level;
		rsxgl_update_mipmap_count(slot);
		return;
	}
	/* Cinematics and scrap atlases redefine the same complete image repeatedly.
	 * Route those uploads through the flip-fenced copy-on-update path instead
	 * of synchronizing, freeing, and reallocating identical storage each time. */
	if (slot->pixels && pixels && slot->width == width && slot->height == height &&
		slot->mipmap_capacity == (slot->generate_mipmap ?
			rsxgl_full_mipmap_count(width, height) : 1))
	{
		if (!rsxgl_traced_texture_redefine)
		{
			PS3_BOOT_TRACE("RSX renderer: same-size texture redefine uses fenced update");
			rsxgl_traced_texture_redefine = 1;
		}
		glTexSubImage2D(target, 0, 0, 0, width, height, format, type, pixels);
		return;
	}
	if (slot->pixels)
	{
		rsxgl_finish();
		rsxgl_release_retired_textures();
		rsxFree(slot->pixels);
	}
	slot->pixels = NULL;
	slot->offset = 0;
	slot->width = width;
	slot->height = height;
	/* Linear RSX textures tolerate row padding; a 64-byte pitch keeps small
	 * Quake assets (8x8 notexture, tiny mip bases, etc.) hardware-aligned. */
	slot->pitch = (width * 4 + 63) & ~63;
	slot->mipmap_capacity = slot->generate_mipmap ?
		rsxgl_full_mipmap_count(width, height) : 1;
	slot->mipmap_levels = 1;
	slot->uploaded_mipmap_mask = 1u;
	slot->ever_applied = GL_FALSE;
	slot->last_sampled_pixels = NULL;
	slot->last_sampled_frame = 0;
	size = rsxgl_mipmap_storage_size(slot->pitch, height,
		slot->mipmap_capacity);
	slot->allocation_size = size;
	if (!size) return;
	slot->pixels = rsxMemalign(128, size);
	if (!slot->pixels) return;
	memset(slot->pixels, 0xff, size);
	if (rsxAddressToOffset(slot->pixels, &slot->offset) != 0)
	{
		rsxFree(slot->pixels);
		slot->pixels = NULL;
		slot->allocation_size = 0;
		return;
	}
	rsxgl_copy_texture_pixels(slot, 0, 0, 0, width, height, format, pixels);
	if (slot->generate_mipmap) rsxgl_generate_mipmaps(slot);
}

void APIENTRY glTexParameteri(GLenum target, GLenum pname, GLint param)
{
	rsxgl_texture_t *slot;
	GLboolean generate = GL_FALSE;
	(void)target;
	if (rsxgl_bound_texture >= RSXGL_MAX_TEXTURES) return;
	slot = &rsxgl_textures[rsxgl_bound_texture];
	rsxgl_initialize_texture_parameters(slot);
	if (pname == GL_TEXTURE_MIN_FILTER)
	{
		if (rsxgl_state_filter_enabled && slot->min_filter == param)
			goto unchanged;
	}
	else if (pname == GL_TEXTURE_MAG_FILTER)
	{
		if (rsxgl_state_filter_enabled && slot->mag_filter == param)
			goto unchanged;
	}
	else if (pname == GL_TEXTURE_MAX_ANISOTROPY_EXT)
	{
		if (param < 1) param = 1;
		if (param > 16) param = 16;
		if (rsxgl_state_filter_enabled && slot->max_anisotropy == param)
			goto unchanged;
	}
	else if (pname == GL_TEXTURE_WRAP_S)
	{
		if (rsxgl_state_filter_enabled && slot->wrap_s == param)
			goto unchanged;
	}
	else if (pname == GL_TEXTURE_WRAP_T)
	{
		if (rsxgl_state_filter_enabled && slot->wrap_t == param)
			goto unchanged;
	}
	else if (pname == GL_GENERATE_MIPMAP)
	{
		generate = param ? GL_TRUE : GL_FALSE;
		if (rsxgl_state_filter_enabled && slot->generate_mipmap == generate)
			goto unchanged;
	}
	else return;

	rsxgl_flush_triangle_batch();
	if (pname == GL_TEXTURE_MIN_FILTER) slot->min_filter = param;
	else if (pname == GL_TEXTURE_MAG_FILTER) slot->mag_filter = param;
	else if (pname == GL_TEXTURE_MAX_ANISOTROPY_EXT)
		slot->max_anisotropy = param;
	else if (pname == GL_TEXTURE_WRAP_S) slot->wrap_s = param;
	else if (pname == GL_TEXTURE_WRAP_T) slot->wrap_t = param;
	else if (pname == GL_GENERATE_MIPMAP)
	{
		slot->generate_mipmap = generate;
		if (generate && slot->pixels) rsxgl_generate_mipmaps(slot);
	}
	rsxgl_sampler_dirty = 1;
	rsxgl_dirty_lightmap_sampler_for_texture(rsxgl_bound_texture);
	return;

unchanged:
	rsxgl_note_state_skip();
}

void APIENTRY glTexSubImage2D(GLenum target, GLint level, GLint xoffset,
	GLint yoffset, GLsizei width, GLsizei height, GLenum format, GLenum type,
	const GLvoid *pixels)
{
	rsxgl_texture_t *slot;
	void *replacement;
	size_t size;
	int rename_enabled;
	int rename_requested;
	int frame_reuse_enabled;
	int sampled_current_allocation;
	(void)target; (void)type;
	if (level != 0 || rsxgl_bound_texture >= RSXGL_MAX_TEXTURES) return;
	rsxgl_flush_triangle_batch();
	slot = &rsxgl_textures[rsxgl_bound_texture];
	if (!slot->pixels || xoffset < 0 || yoffset < 0 ||
		xoffset + width > slot->width || yoffset + height > slot->height) return;
	rsxgl_texture_cache_dirty = 1;
	/* Draw commands contain the texture's current RSX offset. BeginFrame has
	 * already waited for the preceding flip, so an allocation that has not yet
	 * been sampled in this frame is idle and may be updated in place. After its
	 * first sample, update a fresh allocation and retain the old one until the
	 * next flip proves those draws complete. Uploads outside a frame retain the
	 * conservative synchronized path because they have no frame fence. */
	size = slot->allocation_size;
	rename_enabled = rsxgl_cvar_texture_rename &&
		rsxgl_cvar_texture_rename->value != 0.0f;
	frame_reuse_enabled = rsxgl_cvar_texture_frame_reuse &&
		rsxgl_cvar_texture_frame_reuse->value != 0.0f;
	sampled_current_allocation = rsxgl_frame_active &&
		slot->last_sampled_frame == rsxgl_frame_serial &&
		slot->last_sampled_pixels == slot->pixels;
	rename_requested = rsxgl_frame_active && rename_enabled &&
		(!frame_reuse_enabled || sampled_current_allocation);
	if (rsxgl_frame_ready)
	{
		rsxgl_frame_texture_updates++;
	}
	if (!rsxgl_traced_first_upload)
	{
		PS3_BOOT_TRACE(rsxgl_frame_active && frame_reuse_enabled &&
			!sampled_current_allocation ?
			"RSX renderer: first texture subimage entered (pre-sample in-place update)" :
			(rename_requested ?
			"RSX renderer: first texture subimage entered (rename requested)" :
			(rename_enabled ?
				"RSX renderer: first texture subimage entered (outside-frame synchronization)" :
				"RSX renderer: first texture subimage entered (rename disabled)")));
	}
	if (rsxgl_frame_active && frame_reuse_enabled &&
		!sampled_current_allocation)
	{
		rsxgl_copy_texture_pixels(slot, 0, xoffset, yoffset, width, height,
			format, pixels);
		if (slot->generate_mipmap) rsxgl_generate_mipmaps(slot);
		if (rsxgl_frame_ready)
		{
			rsxgl_frame_texture_inplace++;
		}
		if (!rsxgl_traced_first_upload)
		{
			PS3_BOOT_TRACE("RSX renderer: first pre-sample texture subimage complete");
			rsxgl_traced_first_upload = 1;
		}
		if (!rsxgl_traced_texture_frame_reuse)
		{
			PS3_RUNTIME_TRACE("RSX renderer: pre-sample dynamic texture reuse active");
			rsxgl_traced_texture_frame_reuse = 1;
		}
		return;
	}
	replacement = rename_requested ? rsxMemalign(128, size) : NULL;
	if (replacement)
	{
		void *old_pixels = slot->pixels;
		u32 replacement_offset;
		memcpy(replacement, old_pixels, size);
		if (rsxAddressToOffset(replacement, &replacement_offset) != 0)
		{
			rsxFree(replacement);
			replacement = NULL;
		}
		else
		{
			slot->pixels = replacement;
			slot->offset = replacement_offset;
			rsxgl_sampler_dirty = 1;
			rsxgl_dirty_lightmap_sampler_for_texture(rsxgl_bound_texture);
			rsxgl_copy_texture_pixels(slot, 0, xoffset, yoffset, width, height,
				format, pixels);
			if (slot->generate_mipmap) rsxgl_generate_mipmaps(slot);
			rsxgl_retire_texture(old_pixels, size);
			if (rsxgl_frame_ready)
			{
				rsxgl_frame_texture_renames++;
				rsxgl_frame_texture_rename_kib +=
					(unsigned int)((size + 1023) / 1024);
			}
			if (!rsxgl_traced_first_upload)
			{
				PS3_BOOT_TRACE("RSX renderer: first renamed texture subimage complete");
				rsxgl_traced_first_upload = 1;
			}
			if (!rsxgl_traced_texture_rename)
			{
				PS3_RUNTIME_TRACE("RSX renderer: dynamic texture renaming active");
				rsxgl_traced_texture_rename = 1;
			}
			return;
		}
	}
	if (rename_requested && !rsxgl_traced_texture_rename_fallback)
	{
		PS3_BOOT_TRACE("RSX renderer: texture rename allocation failed; synchronized fallback");
		rsxgl_traced_texture_rename_fallback = 1;
	}

	rsxgl_finish();
	rsxgl_release_retired_textures();
	slot->last_sampled_pixels = NULL;
	slot->last_sampled_frame = 0;
	rsxgl_copy_texture_pixels(slot, 0, xoffset, yoffset, width, height,
		format, pixels);
	if (slot->generate_mipmap) rsxgl_generate_mipmaps(slot);
	if (!rsxgl_traced_first_upload)
	{
		PS3_BOOT_TRACE("RSX renderer: first synchronized texture subimage complete");
		rsxgl_traced_first_upload = 1;
	}
}

void APIENTRY glTranslatef(GLfloat x, GLfloat y, GLfloat z)
{
	float matrix[16];
	float *current;
	if (x == 0.0f && y == 0.0f && z == 0.0f)
	{
		if (!rsxgl_traced_noop_transform)
		{
			PS3_RUNTIME_TRACE("RSX renderer: no-op transform filtering active");
			rsxgl_traced_noop_transform = 1;
		}
		return;
	}
	current = rsxgl_current_matrix();
	rsxgl_flush_triangle_batch();
	rsxgl_identity(matrix);
	matrix[12] = x; matrix[13] = y; matrix[14] = z;
	rsxgl_multiply(current, current, matrix);
	rsxgl_touch_matrix();
}

static void
rsxgl_emit_vertex(float x, float y, float z)
{
	rsxgl_vertex_t *vertex;
	if (!rsxgl_inside_begin || rsxgl_immediate_count >= RSXGL_IMMEDIATE_VERTICES) return;
	vertex = &rsxgl_immediate[rsxgl_immediate_count++];
	rsxgl_store_vertex(vertex, x, y, z, rsxgl_current_texcoord[0],
		rsxgl_current_texcoord[1], rsxgl_current_color);
}

void APIENTRY glVertex2f(GLfloat x, GLfloat y) { rsxgl_emit_vertex(x, y, 0.0f); }
void APIENTRY glVertex2i(GLint x, GLint y) { rsxgl_emit_vertex(x, y, 0.0f); }
void APIENTRY glVertex3fv(const GLfloat *v) { if (v) rsxgl_emit_vertex(v[0], v[1], v[2]); }

void APIENTRY glVertexPointer(GLint size, GLenum type, GLsizei stride, const GLvoid *pointer)
{
	rsxgl_vertex_array.pointer = pointer;
	rsxgl_vertex_array.size = size;
	rsxgl_vertex_array.type = type;
	rsxgl_vertex_array.stride = stride;
}

void APIENTRY glViewport(GLint x, GLint y, GLsizei width, GLsizei height)
{
	if (rsxgl_state_filter_enabled &&
		rsxgl_viewport_x == x && rsxgl_viewport_y == y &&
		rsxgl_viewport_width == width && rsxgl_viewport_height == height)
	{
		rsxgl_note_state_skip();
		return;
	}
	rsxgl_flush_triangle_batch();
	rsxgl_viewport_x = x;
	rsxgl_viewport_y = y;
	rsxgl_viewport_width = width;
	rsxgl_viewport_height = height;
	if (rsxgl_context) rsxgl_apply_viewport(x, y, width, height);
}
