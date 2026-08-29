/*
 * Copyright (C) 1997-2001 Id Software, Inc.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or (at
 * your option) any later version.
 *
 * This program is distributed in the hope that it will be useful, but
 * WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.
 *
 * See the GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 59 Temple Place - Suite 330, Boston, MA
 * 02111-1307, USA.
 *
 * =======================================================================
 *
 * Surface generation and drawing
 *
 * =======================================================================
 */

#include <assert.h>
#include <stddef.h>
#include <stdint.h>

#include "header/local.h"

msurface_t *r_alpha_surfaces;
gllightmapstate_t gl_lms;

#ifdef __PSL1GHT__
/* BSP face and subdivided warp polygons are capped below 64 vertices by the
 * loaders. Flowing and offset lightmap passes can therefore share this small
 * scratch array instead of touching the heap for each visible chain. */
static GLfloat ps3_surface_texcoords[2 * 64];
#endif

#ifdef PS3_NATIVE_RSX
#define PS3_VISIBLE_IMAGE_WORD_BITS 64
#define PS3_VISIBLE_IMAGE_WORDS \
	((MAX_GLTEXTURES + PS3_VISIBLE_IMAGE_WORD_BITS - 1) / \
		PS3_VISIBLE_IMAGE_WORD_BITS)
#define PS3_LIGHTMAP_PAGE_WORDS \
	((MAX_LIGHTMAPS + PS3_VISIBLE_IMAGE_WORD_BITS - 1) / \
		PS3_VISIBLE_IMAGE_WORD_BITS)
#define PS3_STEREO_LIGHTMAP_CACHE_SIZE (512 * 1024)

/* World traversal already knows the exact images that receive an opaque
 * texture chain. Record that sparse set as it is built, then consume it in
 * ascending gltextures[] order. This preserves the old material ordering while
 * avoiding a scan of every one of the 1,024 registered-image slots per eye. */
static uint64_t ps3_visible_image_words[PS3_VISIBLE_IMAGE_WORDS];
static unsigned int ps3_visible_image_word_count;
/* Heads are consumed and cleared for every occupied page before the next base
 * texture. Persistent zero-on-consume storage avoids clearing a 1 KiB automatic
 * table on every mono pass (and twice per stereoscopic frame). */
static msurface_t *ps3_combined_lightmap_heads[MAX_LIGHTMAPS];
/* A render error can abort this routine before its normal zero-on-consume
 * cleanup. Recover once on the next pass instead of carrying stale surface
 * links across frames; the normal path still avoids the table-sized clear. */
static qboolean ps3_combined_lightmap_heads_dirty;
/* The compatibility lightmap pass formerly cleared every head and scanned all
 * 128 pages before every world or brush-model draw. Track only pages which
 * actually receive a surface. Their heads remain valid through show-triangle
 * diagnostics and are cleared sparsely at the beginning of the next pass. */
static uint64_t ps3_lightmap_page_words[PS3_LIGHTMAP_PAGE_WORDS];
static qboolean ps3_lightmap_page_heads_dirty;
static qboolean ps3_traced_sparse_lightmap_pages;
static qboolean ps3_traced_empty_lightmap_pass;
static qboolean ps3_traced_sparse_texture_chains;
static qboolean ps3_traced_prepared_surface_facts;
static qboolean ps3_traced_hierarchical_world_clip;
static qboolean ps3_world_clip_stats;
static unsigned int ps3_world_clip_tests;
static unsigned int ps3_world_clip_skips;
/* Dynamic world-light texels are eye-independent, but each eye still needs its
 * own atlas allocation and upload because intervening brush models can replace
 * texture zero. Retain compact first-eye tiles and repack them into the normal
 * second-eye atlas; overflow and surfaces visible in only one eye build through
 * the original path. This is bounded PPU memory and performs no RSX allocation. */
static byte ps3_stereo_lightmap_cache[PS3_STEREO_LIGHTMAP_CACHE_SIZE]
	__attribute__((aligned(128)));
static size_t ps3_stereo_lightmap_cache_used;
static unsigned int ps3_stereo_lightmap_generation;
static qboolean ps3_traced_stereo_lightmap_reuse;

static image_t *R_TextureAnimation(entity_t *currententity, mtexinfo_t *tex);

static image_t *
R_PS3_WorldTextureAnimation(entity_t *currententity, mtexinfo_t *tex)
{
	image_t *image;
	static qboolean traced_animation_cache;

	if (!tex->next || tex->ps3_cached_registration < 0)
	{
		return R_TextureAnimation(currententity, tex);
	}
	if (tex->ps3_cached_image &&
		tex->ps3_cached_frame == currententity->frame &&
		tex->ps3_cached_registration == registration_sequence)
	{
		if (!traced_animation_cache)
		{
			PS3_RUNTIME_TRACE(
				"RSX renderer: opaque animated-texture resolution cache active");
			traced_animation_cache = true;
		}
		return tex->ps3_cached_image;
	}

	image = R_TextureAnimation(currententity, tex);
	tex->ps3_cached_image = image;
	tex->ps3_cached_frame = currententity->frame;
	tex->ps3_cached_registration = registration_sequence;
	return image;
}

static void
R_PS3_MarkVisibleImage(image_t *image)
{
	ptrdiff_t index;
	unsigned int word;

	if (!image)
	{
		return;
	}
	index = image - gltextures;
	if (index < 0 || index >= MAX_GLTEXTURES)
	{
		return;
	}
	word = (unsigned int)index / PS3_VISIBLE_IMAGE_WORD_BITS;
	ps3_visible_image_words[word] |=
		UINT64_C(1) << ((unsigned int)index &
			(PS3_VISIBLE_IMAGE_WORD_BITS - 1));
	if (word + 1 > ps3_visible_image_word_count)
	{
		ps3_visible_image_word_count = word + 1;
	}
}

static int
R_PS3_NextVisibleImage(unsigned int *word_cursor, unsigned int word_limit,
	uint64_t *word_bits)
{
	unsigned int bit;

	while (!*word_bits)
	{
		if (*word_cursor >= word_limit)
		{
			return -1;
		}
		*word_bits = ps3_visible_image_words[*word_cursor];
		ps3_visible_image_words[*word_cursor] = 0;
		(*word_cursor)++;
	}

	bit = (unsigned int)__builtin_ctzll(*word_bits);
	*word_bits &= *word_bits - 1;
	return (int)((*word_cursor - 1) * PS3_VISIBLE_IMAGE_WORD_BITS + bit);
}

static void
R_PS3_BeginLightmapSurfacePass(void)
{
	unsigned int word;

	if (ps3_lightmap_page_heads_dirty)
	{
		for (word = 0; word < PS3_LIGHTMAP_PAGE_WORDS; word++)
		{
			uint64_t pages = ps3_lightmap_page_words[word];
			while (pages)
			{
				unsigned int bit = (unsigned int)__builtin_ctzll(pages);
				unsigned int page = word * PS3_VISIBLE_IMAGE_WORD_BITS + bit;
				if (page < MAX_LIGHTMAPS)
				{
					gl_lms.lightmap_surfaces[page] = NULL;
				}
				pages &= pages - 1;
			}
			ps3_lightmap_page_words[word] = 0;
		}
		ps3_lightmap_page_heads_dirty = false;
	}
}

static inline void
R_PS3_ChainLightmapSurface(msurface_t *surface, unsigned int page)
{
	if (!surface || page >= MAX_LIGHTMAPS)
	{
		return;
	}
	surface->lightmapchain = gl_lms.lightmap_surfaces[page];
	gl_lms.lightmap_surfaces[page] = surface;
	ps3_lightmap_page_words[page / PS3_VISIBLE_IMAGE_WORD_BITS] |=
		UINT64_C(1) << (page & (PS3_VISIBLE_IMAGE_WORD_BITS - 1));
	ps3_lightmap_page_heads_dirty = true;
}

static qboolean
R_PS3_HasLightmapSurfacePages(void)
{
	unsigned int word;
	for (word = 0; word < PS3_LIGHTMAP_PAGE_WORDS; word++)
	{
		if (ps3_lightmap_page_words[word])
		{
			return true;
		}
	}
	return false;
}

static inline void
R_PS3_ChainWorldSurface(entity_t *currententity, msurface_t *surface)
{
	image_t *image;

	if (surface->texinfo->flags & SURF_SKY)
	{
		R_AddSkySurface(surface);
		return;
	}

	image = R_PS3_WorldTextureAnimation(currententity, surface->texinfo);
	if (surface->texinfo->flags & (SURF_TRANS33 | SURF_TRANS66))
	{
		surface->texturechain = r_alpha_surfaces;
		r_alpha_surfaces = surface;
		surface->texinfo->image = image;
		return;
	}

	if (!image->texturechain)
	{
		R_PS3_MarkVisibleImage(image);
	}
	surface->texturechain = image->texturechain;
	image->texturechain = surface;
}

/* Test only frustum planes which the parent BSP box crossed. A child of a box
 * wholly in front of one plane cannot leave that half-space, so clear that bit
 * for its complete subtree. The positive support corner preserves the exact
 * old outside decision; the negative support corner is used only to prove that
 * descendants no longer need this plane. */
static qboolean
R_PS3_ClipWorldBox(const vec3_t mins, const vec3_t maxs,
	unsigned int clipflags, unsigned int *child_clipflags)
{
	unsigned int original_flags = clipflags;
	int i;
	if (ps3_world_clip_stats)
	{
		ps3_world_clip_skips += 4u -
			(unsigned int)__builtin_popcount(clipflags & 15u);
	}

	for (i = 0; i < 4; i++)
	{
		const cplane_t *plane;
		float maximum;
		float minimum;
		unsigned int bit = 1u << i;

		if (!(clipflags & bit))
		{
			continue;
		}
		if (ps3_world_clip_stats)
		{
			ps3_world_clip_tests++;
		}
		plane = &frustum[i];
		switch (plane->signbits & 7)
		{
			case 0:
				maximum = plane->normal[0] * maxs[0] +
					plane->normal[1] * maxs[1] +
					plane->normal[2] * maxs[2];
				minimum = plane->normal[0] * mins[0] +
					plane->normal[1] * mins[1] +
					plane->normal[2] * mins[2];
				break;
			case 1:
				maximum = plane->normal[0] * mins[0] +
					plane->normal[1] * maxs[1] +
					plane->normal[2] * maxs[2];
				minimum = plane->normal[0] * maxs[0] +
					plane->normal[1] * mins[1] +
					plane->normal[2] * mins[2];
				break;
			case 2:
				maximum = plane->normal[0] * maxs[0] +
					plane->normal[1] * mins[1] +
					plane->normal[2] * maxs[2];
				minimum = plane->normal[0] * mins[0] +
					plane->normal[1] * maxs[1] +
					plane->normal[2] * mins[2];
				break;
			case 3:
				maximum = plane->normal[0] * mins[0] +
					plane->normal[1] * mins[1] +
					plane->normal[2] * maxs[2];
				minimum = plane->normal[0] * maxs[0] +
					plane->normal[1] * maxs[1] +
					plane->normal[2] * mins[2];
				break;
			case 4:
				maximum = plane->normal[0] * maxs[0] +
					plane->normal[1] * maxs[1] +
					plane->normal[2] * mins[2];
				minimum = plane->normal[0] * mins[0] +
					plane->normal[1] * mins[1] +
					plane->normal[2] * maxs[2];
				break;
			case 5:
				maximum = plane->normal[0] * mins[0] +
					plane->normal[1] * maxs[1] +
					plane->normal[2] * mins[2];
				minimum = plane->normal[0] * maxs[0] +
					plane->normal[1] * mins[1] +
					plane->normal[2] * maxs[2];
				break;
			case 6:
				maximum = plane->normal[0] * maxs[0] +
					plane->normal[1] * mins[1] +
					plane->normal[2] * mins[2];
				minimum = plane->normal[0] * mins[0] +
					plane->normal[1] * maxs[1] +
					plane->normal[2] * maxs[2];
				break;
			default:
				maximum = plane->normal[0] * mins[0] +
					plane->normal[1] * mins[1] +
					plane->normal[2] * mins[2];
				minimum = plane->normal[0] * maxs[0] +
					plane->normal[1] * maxs[1] +
					plane->normal[2] * maxs[2];
				break;
		}

		if (maximum < plane->dist)
		{
			return false;
		}
		if (minimum >= plane->dist)
		{
			clipflags &= ~bit;
		}
	}

	*child_clipflags = clipflags;
	if (clipflags != original_flags &&
		!ps3_traced_hierarchical_world_clip)
	{
		PS3_RUNTIME_TRACE(
			"GL1 renderer: hierarchical BSP frustum plane masking active");
		ps3_traced_hierarchical_world_clip = true;
	}
	return true;
}
#endif

int c_visible_lightmaps;
int c_visible_textures;
static vec3_t modelorg; /* relative to viewpoint */

void LM_InitBlock(void);
void LM_UploadBlock(qboolean dynamic);
qboolean LM_AllocBlock(int w, int h, int *x, int *y);
int LM_DynamicTextureIndex(void);

void R_SetCacheState(msurface_t *surf);
void R_BuildLightMap(msurface_t *surf, byte *dest, int stride);

#ifdef PS3_NATIVE_RSX
static void
R_PS3_CopyLightmapRows(byte *destination, int destination_stride,
	const byte *source, int source_stride, int row_bytes, int rows)
{
	int row;

	for (row = 0; row < rows; row++)
	{
		memcpy(destination, source, (size_t)row_bytes);
		destination += destination_stride;
		source += source_stride;
	}
}

static void
R_PS3_BuildDynamicLightmap(const model_t *currentmodel, msurface_t *surface,
	byte *destination, int stride, int smax, int tmax)
{
	size_t bytes = (size_t)smax * (size_t)tmax * LIGHTMAP_BYTES;
	size_t offset;
	int row_bytes = smax * LIGHTMAP_BYTES;
	qboolean world = currentmodel == r_worldmodel;

	if (world && r_ps3_second_stereo_eye &&
		ps3_stereo_lightmap_generation ==
			(unsigned int)r_dlightframecount &&
		surface->ps3_stereo_lightmap_generation ==
			ps3_stereo_lightmap_generation &&
		(size_t)surface->ps3_stereo_lightmap_offset + bytes <=
			ps3_stereo_lightmap_cache_used)
	{
		R_PS3_CopyLightmapRows(destination, stride,
			ps3_stereo_lightmap_cache +
				surface->ps3_stereo_lightmap_offset,
			row_bytes, row_bytes, tmax);
		if (!ps3_traced_stereo_lightmap_reuse)
		{
			PS3_RUNTIME_TRACE(
				"RSX renderer: stereo dynamic-lightmap texels reused on second eye");
			ps3_traced_stereo_lightmap_reuse = true;
		}
		RSXGL_RecordStereoLightmapReuse((unsigned int)(smax * tmax));
		return;
	}

	R_BuildLightMap(surface, destination, stride);
	if (!world || r_ps3_second_stereo_eye ||
		ps3_stereo_lightmap_generation !=
			(unsigned int)r_dlightframecount)
	{
		return;
	}

	offset = (ps3_stereo_lightmap_cache_used + 15) & ~((size_t)15);
	if (offset + bytes > PS3_STEREO_LIGHTMAP_CACHE_SIZE)
	{
		surface->ps3_stereo_lightmap_generation = 0;
		return;
	}
	R_PS3_CopyLightmapRows(ps3_stereo_lightmap_cache + offset, row_bytes,
		destination, stride, row_bytes, tmax);
	surface->ps3_stereo_lightmap_generation =
		ps3_stereo_lightmap_generation;
	surface->ps3_stereo_lightmap_offset = (unsigned int)offset;
	ps3_stereo_lightmap_cache_used = offset + bytes;
}
#endif

/*
 * Returns the proper texture for a given time and base texture
 */
static image_t *
R_TextureAnimation(entity_t *currententity, mtexinfo_t *tex)
{
	int c;

	if (!tex->next)
	{
		return tex->image;
	}

	c = currententity->frame % tex->numframes;

	while (c)
	{
		tex = tex->next;
		c--;
	}

	return tex->image;
}

static void
R_DrawGLPoly(glpoly_t *p)
{
	float *v;

	v = p->verts[0];

#ifdef PS3_NATIVE_RSX
	RSXGL_DrawTexturedFan(v, VERTEXSIZE, v + 3, VERTEXSIZE,
		p->numverts);
#else
    glEnableClientState( GL_VERTEX_ARRAY );
    glEnableClientState( GL_TEXTURE_COORD_ARRAY );

    glVertexPointer( 3, GL_FLOAT, VERTEXSIZE*sizeof(GLfloat), v );
    glTexCoordPointer( 2, GL_FLOAT, VERTEXSIZE*sizeof(GLfloat), v+3 );
    glDrawArrays( GL_TRIANGLE_FAN, 0, p->numverts );

    glDisableClientState( GL_VERTEX_ARRAY );
    glDisableClientState( GL_TEXTURE_COORD_ARRAY );
#endif
}

static void
R_DrawGLFlowingPoly(msurface_t *fa)
{
	int i;
	float *v;
	glpoly_t *p;
	float scroll;

	p = fa->polys;

	scroll = -64 * ((r_newrefdef.time / 40.0) - (int)(r_newrefdef.time / 40.0));

	if (scroll == 0.0)
	{
		scroll = -64.0;
	}

    #ifdef __PSL1GHT__
    GLfloat *tex = ps3_surface_texcoords;
    #else
    YQ2_VLA(GLfloat, tex, 2*p->numverts);
    #endif
    unsigned int index_tex = 0;

    v = p->verts [ 0 ];

	for ( i = 0; i < p->numverts; i++, v += VERTEXSIZE )
    {
        tex[index_tex++] = v [ 3 ] + scroll;
        tex[index_tex++] = v [ 4 ];
    }
    v = p->verts [ 0 ];

#ifdef PS3_NATIVE_RSX
	RSXGL_DrawTexturedFan(v, VERTEXSIZE, tex, 2, p->numverts);
#else
    glEnableClientState( GL_VERTEX_ARRAY );
    glEnableClientState( GL_TEXTURE_COORD_ARRAY );

    glVertexPointer( 3, GL_FLOAT, VERTEXSIZE*sizeof(GLfloat), v );
    glTexCoordPointer( 2, GL_FLOAT, 0, tex );
    glDrawArrays( GL_TRIANGLE_FAN, 0, p->numverts );

    glDisableClientState( GL_VERTEX_ARRAY );
    glDisableClientState( GL_TEXTURE_COORD_ARRAY );
#endif
	
	#ifndef __PSL1GHT__
	YQ2_VLAFREE(tex);
	#endif
}

static void
R_DrawTriangleOutlinesPage(int page)
{
	int j;
	glpoly_t *p;
	msurface_t *surf;

	for (surf = gl_lms.lightmap_surfaces[page];
		 surf != 0;
		 surf = surf->lightmapchain)
	{
		p = surf->polys;

		for ( ; p; p = p->chain)
		{
			for (j = 2; j < p->numverts; j++)
			{
				GLfloat vtx[12];
				unsigned int k;

				for (k = 0; k < 3; k++)
				{
					vtx[0 + k] = p->verts[0][k];
					vtx[3 + k] = p->verts[j - 1][k];
					vtx[6 + k] = p->verts[j][k];
					vtx[9 + k] = p->verts[0][k];
				}

				glEnableClientState(GL_VERTEX_ARRAY);
				glVertexPointer(3, GL_FLOAT, 0, vtx);
				glDrawArrays(GL_LINE_STRIP, 0, 4);
				glDisableClientState(GL_VERTEX_ARRAY);
			}
		}
	}
}

static void
R_DrawTriangleOutlines(void)
{
#ifdef PS3_NATIVE_RSX
	unsigned int word;
#else
	int i;
#endif

	if (!gl_showtris->value)
	{
		return;
	}

	glDisable(GL_TEXTURE_2D);
	glDisable(GL_DEPTH_TEST);
	glColor4f(1, 1, 1, 1);

	#ifdef PS3_NATIVE_RSX
	for (word = 0; word < PS3_LIGHTMAP_PAGE_WORDS; word++)
	{
		uint64_t pages = ps3_lightmap_page_words[word];
		while (pages)
		{
			unsigned int bit = (unsigned int)__builtin_ctzll(pages);
			unsigned int page = word * PS3_VISIBLE_IMAGE_WORD_BITS + bit;
			if (page < MAX_LIGHTMAPS)
			{
				R_DrawTriangleOutlinesPage((int)page);
			}
			pages &= pages - 1;
		}
	}
	#else
	for (i = 0; i < MAX_LIGHTMAPS; i++)
	{
		R_DrawTriangleOutlinesPage(i);
	}
	#endif

	glEnable(GL_DEPTH_TEST);
	glEnable(GL_TEXTURE_2D);
}

static void
R_DrawGLPolyChain(glpoly_t *p, float soffset, float toffset)
{
	if ((soffset == 0) && (toffset == 0))
	{
		for ( ; p != 0; p = p->chain)
		{
			float *v;

			v = p->verts[0];

#ifdef PS3_NATIVE_RSX
			RSXGL_DrawTexturedFan(v, VERTEXSIZE, v + 5, VERTEXSIZE,
				p->numverts);
#else
            glEnableClientState( GL_VERTEX_ARRAY );
            glEnableClientState( GL_TEXTURE_COORD_ARRAY );

            glVertexPointer( 3, GL_FLOAT, VERTEXSIZE*sizeof(GLfloat), v );
            glTexCoordPointer( 2, GL_FLOAT, VERTEXSIZE*sizeof(GLfloat), v+5 );
            glDrawArrays( GL_TRIANGLE_FAN, 0, p->numverts );

            glDisableClientState( GL_VERTEX_ARRAY );
            glDisableClientState( GL_TEXTURE_COORD_ARRAY );
#endif
		}
	}
	else
	{
		// workaround for lack of VLAs (=> our workaround uses alloca() which is bad in loops)
#if defined(__PSL1GHT__)
		GLfloat *tex = ps3_surface_texcoords;
#elif defined(_MSC_VER)
		int maxNumVerts = 0;
		for (glpoly_t* tmp = p; tmp; tmp = tmp->chain)
		{
			if ( tmp->numverts > maxNumVerts )
				maxNumVerts = tmp->numverts;
		}

		YQ2_VLA( GLfloat, tex, 2 * maxNumVerts );
#endif

		for ( ; p != 0; p = p->chain)
		{
			float *v;
			int j;

			v = p->verts[0];
#if !defined(_MSC_VER) && !defined(__PSL1GHT__) // desktop compilers use a scoped VLA
            YQ2_VLA(GLfloat, tex, 2*p->numverts);
#endif

            unsigned int index_tex = 0;

			for ( j = 0; j < p->numverts; j++, v += VERTEXSIZE )
			{
			    tex[index_tex++] = v [ 5 ] - soffset;
			    tex[index_tex++] = v [ 6 ] - toffset;
			}

			v = p->verts [ 0 ];

#ifdef PS3_NATIVE_RSX
			RSXGL_DrawTexturedFan(v, VERTEXSIZE, tex, 2,
				p->numverts);
#else
            glEnableClientState( GL_VERTEX_ARRAY );
            glEnableClientState( GL_TEXTURE_COORD_ARRAY );

            glVertexPointer( 3, GL_FLOAT, VERTEXSIZE*sizeof(GLfloat), v );
            glTexCoordPointer( 2, GL_FLOAT, 0, tex );
            glDrawArrays( GL_TRIANGLE_FAN, 0, p->numverts );

            glDisableClientState( GL_VERTEX_ARRAY );
            glDisableClientState( GL_TEXTURE_COORD_ARRAY );
#endif
		}

		#ifndef __PSL1GHT__
		YQ2_VLAFREE( tex );
		#endif
	}
}

/*
 * This routine takes all the given light mapped surfaces
 * in the world and blends them into the framebuffer.
 */
static void
R_DrawStaticLightmapPage(const model_t *currentmodel, int page)
{
	msurface_t *surf;

	if (!gl_lms.lightmap_surfaces[page])
	{
		return;
	}
	if (currentmodel == r_worldmodel)
	{
		c_visible_lightmaps++;
	}

	R_Bind(gl_state.lightmap_textures + page);
	for (surf = gl_lms.lightmap_surfaces[page];
		 surf != 0;
		 surf = surf->lightmapchain)
	{
		if (surf->polys)
		{
			/* Apply overbright bits to the static lightmaps. */
			if (gl1_overbrightbits->value)
			{
				R_TexEnv(GL_COMBINE_EXT);
				glTexEnvi(GL_TEXTURE_ENV, GL_RGB_SCALE_EXT,
					gl1_overbrightbits->value);
			}

			R_DrawGLPolyChain(surf->polys, 0, 0);
		}
	}
}

static void
R_BlendLightmaps(const model_t *currentmodel)
{
	#ifndef PS3_NATIVE_RSX
	int i;
	#endif
	msurface_t *surf, *newdrawsurf = 0;

#ifdef PS3_NATIVE_RSX
	qboolean ps3_cache_stereo = currentmodel == r_worldmodel &&
		R_PS3_StereoLightmapReuseEnabled();
	if (currentmodel == r_worldmodel && !r_ps3_second_stereo_eye)
	{
		/* R_PushDlights chooses one generation for the complete visual pair.
		 * Reset before all early exits so stale tiles can never cross frames. */
		ps3_stereo_lightmap_cache_used = 0;
		ps3_stereo_lightmap_generation = ps3_cache_stereo ?
			(unsigned int)r_dlightframecount : 0;
	}
#endif
	if (currentmodel == r_worldmodel)
	{
		c_visible_lightmaps = 0;
	}

	/* don't bother if we're set to fullbright */
	if (r_fullbright->value)
	{
		return;
	}

	if (!r_worldmodel->lightdata)
	{
		return;
	}

	#ifdef PS3_NATIVE_RSX
	/* A fully single-pass frame has no compatibility lightmap chains. Avoid
	 * changing depth/blend/texture state or touching the dynamic atlas at all. */
	if (!R_PS3_HasLightmapSurfacePages())
	{
		if (!ps3_traced_empty_lightmap_pass)
		{
			PS3_RUNTIME_TRACE(
				"RSX renderer: empty compatibility lightmap pass eliminated");
			ps3_traced_empty_lightmap_pass = true;
		}
		return;
	}
	if (!ps3_traced_sparse_lightmap_pages)
	{
		PS3_RUNTIME_TRACE(
			"RSX renderer: sparse compatibility lightmap page traversal active");
		ps3_traced_sparse_lightmap_pages = true;
	}
	#endif

	/* don't bother writing Z */
	glDepthMask(0);

	/* set the appropriate blending mode unless
	   we're only looking at the lightmaps. */
	if (!gl_lightmap->value)
	{
		glEnable(GL_BLEND);

		if (gl1_saturatelighting->value)
		{
			glBlendFunc(GL_ONE, GL_ONE);
		}
		else
		{
			glBlendFunc(GL_ZERO, GL_SRC_COLOR);
		}
	}

	/* render static lightmaps first */
	#ifdef PS3_NATIVE_RSX
	{
		unsigned int word;
		for (word = 0; word < PS3_LIGHTMAP_PAGE_WORDS; word++)
		{
			uint64_t pages = ps3_lightmap_page_words[word];
			if (word == 0)
			{
				pages &= ~UINT64_C(1); /* page zero is the dynamic chain */
			}
			while (pages)
			{
				unsigned int bit = (unsigned int)__builtin_ctzll(pages);
				unsigned int page = word * PS3_VISIBLE_IMAGE_WORD_BITS + bit;
				if (page < MAX_LIGHTMAPS)
				{
					R_DrawStaticLightmapPage(currentmodel, (int)page);
				}
				pages &= pages - 1;
			}
		}
	}
	#else
	for (i = 1; i < MAX_LIGHTMAPS; i++)
	{
		R_DrawStaticLightmapPage(currentmodel, i);
	}
	#endif

	/* render dynamic lightmaps */
	if (gl1_dynamic->value && gl_lms.lightmap_surfaces[0])
	{
		LM_InitBlock();

		R_Bind(gl_state.lightmap_textures + LM_DynamicTextureIndex());

		if (currentmodel == r_worldmodel)
		{
			c_visible_lightmaps++;
		}

		newdrawsurf = gl_lms.lightmap_surfaces[0];

		for (surf = gl_lms.lightmap_surfaces[0];
			 surf != 0;
			 surf = surf->lightmapchain)
		{
			int smax, tmax;
			byte *base;

			smax = (surf->extents[0] >> 4) + 1;
			tmax = (surf->extents[1] >> 4) + 1;

			if (LM_AllocBlock(smax, tmax, &surf->dlight_s, &surf->dlight_t))
			{
				base = gl_lms.lightmap_buffer;
				base += (surf->dlight_t * BLOCK_WIDTH +
						surf->dlight_s) * LIGHTMAP_BYTES;

				#ifdef PS3_NATIVE_RSX
				if (ps3_cache_stereo)
				{
					R_PS3_BuildDynamicLightmap(currentmodel, surf, base,
						BLOCK_WIDTH * LIGHTMAP_BYTES, smax, tmax);
				}
				else
				{
					R_BuildLightMap(surf, base,
						BLOCK_WIDTH * LIGHTMAP_BYTES);
				}
				#else
				R_BuildLightMap(surf, base, BLOCK_WIDTH * LIGHTMAP_BYTES);
				#endif
			}
			else
			{
				msurface_t *drawsurf;

				/* upload what we have so far */
				LM_UploadBlock(true);

				/* draw all surfaces that use this lightmap */
				for (drawsurf = newdrawsurf;
					 drawsurf != surf;
					 drawsurf = drawsurf->lightmapchain)
				{
					if (drawsurf->polys)
					{
						// Apply overbright bits to the dynamic lightmaps
						if (gl1_overbrightbits->value)
						{
							R_TexEnv(GL_COMBINE_EXT);
							glTexEnvi(GL_TEXTURE_ENV, GL_RGB_SCALE_EXT, gl1_overbrightbits->value);
						}

						R_DrawGLPolyChain(drawsurf->polys,
								(drawsurf->light_s - drawsurf->dlight_s) * (1.0 / 128.0),
								(drawsurf->light_t - drawsurf->dlight_t) * (1.0 / 128.0));
					}
				}

				newdrawsurf = drawsurf;

				/* clear the block */
				LM_InitBlock();

				/* try uploading the block now */
				if (!LM_AllocBlock(smax, tmax, &surf->dlight_s, &surf->dlight_t))
				{
					ri.Sys_Error(ERR_FATAL,
							"Consecutive calls to LM_AllocBlock(%d,%d) failed (dynamic)\n",
							smax, tmax);
				}

				base = gl_lms.lightmap_buffer;
				base += (surf->dlight_t * BLOCK_WIDTH +
						surf->dlight_s) * LIGHTMAP_BYTES;

				#ifdef PS3_NATIVE_RSX
				if (ps3_cache_stereo)
				{
					R_PS3_BuildDynamicLightmap(currentmodel, surf, base,
						BLOCK_WIDTH * LIGHTMAP_BYTES, smax, tmax);
				}
				else
				{
					R_BuildLightMap(surf, base,
						BLOCK_WIDTH * LIGHTMAP_BYTES);
				}
				#else
				R_BuildLightMap(surf, base, BLOCK_WIDTH * LIGHTMAP_BYTES);
				#endif
			}
		}

		/* draw remainder of dynamic lightmaps that haven't been uploaded yet */
		if (newdrawsurf)
		{
			LM_UploadBlock(true);
		}

		for (surf = newdrawsurf; surf != 0; surf = surf->lightmapchain)
		{
			if (surf->polys)
			{
				// Apply overbright bits to the remainder lightmaps
				if (gl1_overbrightbits->value)
				{
					R_TexEnv(GL_COMBINE_EXT);
					glTexEnvi(GL_TEXTURE_ENV, GL_RGB_SCALE_EXT, gl1_overbrightbits->value);
				}

				R_DrawGLPolyChain(surf->polys,
						(surf->light_s - surf->dlight_s) * (1.0 / 128.0),
						(surf->light_t - surf->dlight_t) * (1.0 / 128.0));
			}
		}
	}

	/* restore state */
	glDisable(GL_BLEND);
	glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
	glDepthMask(1);
}

static void
R_ClassifySurfaceLightmap(msurface_t *fa, qboolean *is_dynamic,
	int *style_index)
{
	int maps;
	qboolean changed = false;

	#ifdef PS3_NATIVE_RSX
	/* The style terminator is immutable BSP data and was counted while loading
	 * the face. Keep only the live lightstyle comparison in the frame loop. */
	for (maps = 0; maps < fa->ps3_lightstyle_count; maps++)
	#else
	for (maps = 0; maps < MAXLIGHTMAPS && fa->styles[maps] != 255; maps++)
	#endif
	{
		if (r_newrefdef.lightstyles[fa->styles[maps]].white !=
			fa->cached_light[maps])
		{
			changed = true;
			break;
		}
	}

	if (style_index)
	{
		*style_index = maps;
	}
	if (is_dynamic)
	{
		*is_dynamic = (changed || fa->dlightframe == r_dlightframecount) &&
			gl1_dynamic->value &&
			!(fa->texinfo->flags &
				(SURF_SKY | SURF_TRANS33 | SURF_TRANS66 | SURF_WARP));
	}
}

#ifdef PS3_NATIVE_RSX
static qboolean
R_CanUseCombinedLightmaps(void)
{
	/* These switches and the world light-data pointer are invariant for a full
	 * texture-chain pass. Test them once outside the per-surface loop. */
	return !r_fullbright->value && r_worldmodel && r_worldmodel->lightdata &&
		!gl_lightmap->value && !gl1_saturatelighting->value;
}

static qboolean
R_CanDrawCombinedLightmapEnabled(entity_t *currententity, msurface_t *fa,
	image_t *image, qboolean is_dynamic)
{
	return !is_dynamic &&
		currententity && !(currententity->flags & RF_TRANSLUCENT) &&
		image && !image->has_alpha && fa->ps3_combined_geometry_eligible;
}

static void
R_DrawCombinedLightmapPrepared(msurface_t *fa)
{
	glpoly_t *p = fa->polys;
	float *v = p->verts[0];
	if (p->ps3_rsx_static_generation &&
		RSXGL_DrawStaticLightmappedFan(p->ps3_rsx_static_generation,
			p->ps3_rsx_static_vertex_offset,
			p->ps3_rsx_static_first_vertex, p->numverts,
			p->ps3_rsx_static_indices, (p->numverts - 2) * 3))
	{
		return;
	}
	RSXGL_DrawLightmappedFanPrepared(
		v, VERTEXSIZE, v + 3, VERTEXSIZE, v + 5, VERTEXSIZE,
		p->numverts);
}

static void
R_DrawCombinedLightmap(msurface_t *fa, image_t *image)
{
	float light_scale = gl1_overbrightbits->value ?
		gl1_overbrightbits->value : 1.0f;

	/* Inline brush models enter here without the world chain's prepared base
	 * binding. Keep GL1's texture mirror and the native sampler in agreement. */
	R_Bind(image->texnum);
	RSXGL_DrawLightmappedFan(image->texnum,
		gl_state.lightmap_textures + fa->lightmaptexturenum,
		fa->polys->verts[0], VERTEXSIZE,
		fa->polys->verts[0] + 3, VERTEXSIZE,
		fa->polys->verts[0] + 5, VERTEXSIZE,
		fa->polys->numverts, light_scale);
}
#endif

static void
R_RenderBrushPolyPrepared(msurface_t *fa, image_t *image,
	qboolean is_dynamic, int maps)
{
	c_brush_polys++;

	if (fa->flags & SURF_DRAWTURB)
	{
		R_Bind(image->texnum);

		/* This is a hack ontop of a hack. Warping surfaces like those generated
		   by R_EmitWaterPolys() don't have a lightmap. Original Quake II therefore
		   negated the global intensity on those surfaces, because otherwise they
		   would show up much too bright. When we implemented overbright bits this
		   hack modified the global GL state in an incompatible way. So implement
		   a new hack, based on overbright bits... Depending on the value set to
		   gl1_overbrightbits the result is different:

		    0: Old behaviour.
		    1: No overbright bits on the global scene but correct lightning on
		       warping surfaces.
		    2: Overbright bits on the global scene but not on warping surfaces.
		        They oversaturate otherwise. */
		if (gl1_overbrightbits->value)
		{
			R_TexEnv(GL_COMBINE_EXT);
			glTexEnvi(GL_TEXTURE_ENV, GL_RGB_SCALE_EXT, 1);
		}
		else
		{
			R_TexEnv(GL_MODULATE);
			glColor4f(gl_state.inverse_intensity, gl_state.inverse_intensity,
					  gl_state.inverse_intensity, 1.0f);
		}

		R_EmitWaterPolys(fa);
		R_TexEnv(GL_REPLACE);

		return;
	}
	else
	{
		R_Bind(image->texnum);

		R_TexEnv(GL_REPLACE);
	}

	if (fa->texinfo->flags & SURF_FLOWING)
	{
		R_DrawGLFlowingPoly(fa);
	}
	else
	{
		R_DrawGLPoly(fa->polys);
	}

	if (is_dynamic)
	{
		if (maps < MAXLIGHTMAPS &&
			((fa->styles[maps] >= 32) ||
			 (fa->styles[maps] == 0)) &&
			  (fa->dlightframe != r_dlightframecount))
		{
			unsigned temp[34 * 34];
			int smax, tmax;

			smax = (fa->extents[0] >> 4) + 1;
			tmax = (fa->extents[1] >> 4) + 1;

			R_BuildLightMap(fa, (void *)temp, smax * 4);
			R_SetCacheState(fa);

			R_Bind(gl_state.lightmap_textures + fa->lightmaptexturenum);

			glTexSubImage2D(GL_TEXTURE_2D, 0, fa->light_s, fa->light_t,
					smax, tmax, GL_LIGHTMAP_FORMAT, GL_UNSIGNED_BYTE, temp);

			#ifdef PS3_NATIVE_RSX
			R_PS3_ChainLightmapSurface(fa,
				(unsigned int)fa->lightmaptexturenum);
			#else
			fa->lightmapchain = gl_lms.lightmap_surfaces[fa->lightmaptexturenum];
			gl_lms.lightmap_surfaces[fa->lightmaptexturenum] = fa;
			#endif
		}
		else
		{
			#ifdef PS3_NATIVE_RSX
			R_PS3_ChainLightmapSurface(fa, 0);
			#else
			fa->lightmapchain = gl_lms.lightmap_surfaces[0];
			gl_lms.lightmap_surfaces[0] = fa;
			#endif
		}
	}
	else
	{
		#ifdef PS3_NATIVE_RSX
		R_PS3_ChainLightmapSurface(fa,
			(unsigned int)fa->lightmaptexturenum);
		#else
		fa->lightmapchain = gl_lms.lightmap_surfaces[fa->lightmaptexturenum];
		gl_lms.lightmap_surfaces[fa->lightmaptexturenum] = fa;
		#endif
	}
}

static void
R_RenderBrushPoly(entity_t *currententity, msurface_t *fa)
{
	int maps;
	image_t *image;
	qboolean is_dynamic = false;

	image = R_TextureAnimation(currententity, fa->texinfo);
	R_ClassifySurfaceLightmap(fa, &is_dynamic, &maps);

#ifdef PS3_NATIVE_RSX
	if (RSXGL_CombinedLightmapsEnabled() && R_CanUseCombinedLightmaps() &&
		R_CanDrawCombinedLightmapEnabled(currententity, fa, image, is_dynamic))
	{
		c_brush_polys++;
		R_DrawCombinedLightmap(fa, image);
		return;
	}
#endif

	R_RenderBrushPolyPrepared(fa, image, is_dynamic, maps);
}

/*
 * Draw water surfaces and windows.
 * The BSP tree is waled front to back, so unwinding the chain
 * of alpha_surfaces will draw back to front, giving proper ordering.
 */
void
R_DrawAlphaSurfaces(void)
{
	msurface_t *s;
	float intens;

	/* go back to the world matrix */
	glLoadMatrixf(r_world_matrix);

	glEnable(GL_BLEND);
	R_TexEnv(GL_MODULATE);

	/* the textures are prescaled up for a better
	   lighting range, so scale it back down */
	intens = gl_state.inverse_intensity;

	for (s = r_alpha_surfaces; s; s = s->texturechain)
	{
		R_Bind(s->texinfo->image->texnum);
		c_brush_polys++;

		if (s->texinfo->flags & SURF_TRANS33)
		{
			glColor4f(intens, intens, intens, 0.33);
		}
		else if (s->texinfo->flags & SURF_TRANS66)
		{
			glColor4f(intens, intens, intens, 0.66);
		}
		else
		{
			glColor4f(intens, intens, intens, 1);
		}

		if (s->flags & SURF_DRAWTURB)
		{
			R_EmitWaterPolys(s);
		}
		else if (s->texinfo->flags & SURF_FLOWING)
		{
			R_DrawGLFlowingPoly(s);
		}
		else
		{
			R_DrawGLPoly(s->polys);
		}
	}

	R_TexEnv(GL_REPLACE);
	glColor4f(1, 1, 1, 1);
	glDisable(GL_BLEND);

	r_alpha_surfaces = NULL;
}

static void
R_DrawTextureChains(entity_t *currententity)
{
	int i;
	msurface_t *s;
	image_t *image;
#ifdef PS3_NATIVE_RSX
	unsigned int visible_word = 0;
	unsigned int visible_word_limit = ps3_visible_image_word_count;
	uint64_t visible_bits = 0;
	qboolean combined_enabled = RSXGL_CombinedLightmapsEnabled() &&
		R_CanUseCombinedLightmaps();
	if (ps3_combined_lightmap_heads_dirty)
	{
		memset(ps3_combined_lightmap_heads, 0,
			sizeof(ps3_combined_lightmap_heads));
		ps3_combined_lightmap_heads_dirty = false;
	}
	ps3_visible_image_word_count = 0;
	if (!ps3_traced_prepared_surface_facts)
	{
		PS3_RUNTIME_TRACE(
			"RSX renderer: load-time BSP material classification active");
		ps3_traced_prepared_surface_facts = true;
	}
	if (visible_word_limit && !ps3_traced_sparse_texture_chains)
	{
		PS3_RUNTIME_TRACE("RSX renderer: sparse visible-texture chain traversal active");
		ps3_traced_sparse_texture_chains = true;
	}
#endif

	c_visible_textures = 0;

#ifdef PS3_NATIVE_RSX
	for (;;)
	{
		i = R_PS3_NextVisibleImage(&visible_word, visible_word_limit,
			&visible_bits);
		if (i < 0)
		{
			break;
		}
		image = &gltextures[i];
#else
	for (i = 0, image = gltextures; i < numgltextures; i++, image++)
	{
#endif
		if (!image->registration_sequence)
		{
			continue;
		}

		s = image->texturechain;

		if (!s)
		{
			continue;
		}

		c_visible_textures++;

#ifdef PS3_NATIVE_RSX
		if (combined_enabled)
		{
			/* Opaque BSP surfaces have no order dependency. Bucket the new
			 * single-pass subset by lightmap page inside this already
			 * base-texture-sorted chain, so one material pair becomes one native
			 * batch instead of flushing whenever the BSP order alternates pages. */
			int lightmap;
			uint64_t occupied_lightmaps[2] = {0, 0};

			for ( ; s; s = s->texturechain)
			{
				qboolean is_dynamic = false;
				int maps;
				/* Recursive world traversal already resolved animation before
				 * placing this surface on image->texturechain. Reuse both that
				 * image and this classification in the fallback rather than
				 * repeating them inside R_RenderBrushPoly. */
				R_ClassifySurfaceLightmap(s, &is_dynamic, &maps);
				if (R_CanDrawCombinedLightmapEnabled(currententity, s, image,
					is_dynamic))
				{
					lightmap = s->lightmaptexturenum;
					ps3_combined_lightmap_heads_dirty = true;
					s->lightmapchain = ps3_combined_lightmap_heads[lightmap];
					ps3_combined_lightmap_heads[lightmap] = s;
					occupied_lightmaps[lightmap >> 6] |=
						UINT64_C(1) << (lightmap & 63);
				}
				else
				{
					R_RenderBrushPolyPrepared(s, image, is_dynamic, maps);
				}
			}

			if (occupied_lightmaps[0] || occupied_lightmaps[1])
			{
				float light_scale = gl1_overbrightbits->value ?
					gl1_overbrightbits->value : 1.0f;
				int lightmap_word;
				/* All prepared surfaces in this chain use image->texnum. Bind it
				 * once instead of re-entering the out-of-line GL wrapper per fan. */
				R_Bind(image->texnum);
				for (lightmap_word = 0; lightmap_word < 2; lightmap_word++)
				{
					uint64_t pages = occupied_lightmaps[lightmap_word];
					while (pages)
					{
						qboolean batch_ready;
						lightmap = (lightmap_word << 6) +
							(int)__builtin_ctzll(pages);
						pages &= pages - 1;
						batch_ready = RSXGL_BeginLightmappedBatch(
							image->texnum,
							gl_state.lightmap_textures + lightmap,
							light_scale);
						for (s = ps3_combined_lightmap_heads[lightmap]; s;
							s = s->lightmapchain)
						{
							c_brush_polys++;
							if (batch_ready)
							{
								R_DrawCombinedLightmapPrepared(s);
							}
						}
						if (batch_ready) RSXGL_EndLightmappedBatch();
						ps3_combined_lightmap_heads[lightmap] = NULL;
					}
				}
				ps3_combined_lightmap_heads_dirty = false;
			}
		}
		else
#endif
		{
		for ( ; s; s = s->texturechain)
		{
			R_RenderBrushPoly(currententity, s);
		}
		}

		image->texturechain = NULL;
	}

	R_TexEnv(GL_REPLACE);
}

static void
R_DrawInlineBModel(entity_t *currententity, const model_t *currentmodel)
{
	int i, k;
	cplane_t *pplane;
	float dot;
	msurface_t *psurf;
	dlight_t *lt;

	/* calculate dynamic lighting for bmodel */
	if (!gl1_flashblend->value
	#ifdef PS3_NATIVE_RSX
		&& !r_ps3_second_stereo_eye
	#endif
	)
	{
		lt = r_newrefdef.dlights;

		for (k = 0; k < r_newrefdef.num_dlights; k++, lt++)
		{
			R_MarkLights(lt, 1 << k, currentmodel->nodes + currentmodel->firstnode);
		}
	}

	psurf = &currentmodel->surfaces[currentmodel->firstmodelsurface];

	if (currententity->flags & RF_TRANSLUCENT)
	{
		glEnable(GL_BLEND);
		glColor4f(1, 1, 1, 0.25);
		R_TexEnv(GL_MODULATE);
	}

	/* draw texture */
	for (i = 0; i < currentmodel->nummodelsurfaces; i++, psurf++)
	{
		/* find which side of the node we are on */
		pplane = psurf->plane;

		dot = DotProduct(modelorg, pplane->normal) - pplane->dist;

		/* draw the polygon */
		if (((psurf->flags & SURF_PLANEBACK) && (dot < -BACKFACE_EPSILON)) ||
			(!(psurf->flags & SURF_PLANEBACK) && (dot > BACKFACE_EPSILON)))
		{
			if (psurf->texinfo->flags & (SURF_TRANS33 | SURF_TRANS66))
			{
				/* add to the translucent chain */
				psurf->texturechain = r_alpha_surfaces;
				r_alpha_surfaces = psurf;
			}
			else
			{
				R_RenderBrushPoly(currententity, psurf);
			}
		}
	}

	if (!(currententity->flags & RF_TRANSLUCENT))
	{

		R_BlendLightmaps(currentmodel);
	}
	else
	{
		glDisable(GL_BLEND);
		glColor4f(1, 1, 1, 1);
		R_TexEnv(GL_REPLACE);
	}
}

void
R_DrawBrushModel(entity_t *currententity, const model_t *currentmodel)
{
	vec3_t mins, maxs;
	int i;
	qboolean rotated;

	if (currentmodel->nummodelsurfaces == 0)
	{
		return;
	}

	gl_state.currenttextures[0] = gl_state.currenttextures[1] = -1;

	if (currententity->angles[0] || currententity->angles[1] || currententity->angles[2])
	{
		rotated = true;

		for (i = 0; i < 3; i++)
		{
			mins[i] = currententity->origin[i] - currentmodel->radius;
			maxs[i] = currententity->origin[i] + currentmodel->radius;
		}
	}
	else
	{
		rotated = false;
		VectorAdd(currententity->origin, currentmodel->mins, mins);
		VectorAdd(currententity->origin, currentmodel->maxs, maxs);
	}

	if (R_CullBox(mins, maxs))
	{
		return;
	}

	if (gl_zfix->value)
	{
		glEnable(GL_POLYGON_OFFSET_FILL);
	}

	glColor4f(1, 1, 1, 1);
	#ifdef PS3_NATIVE_RSX
	R_PS3_BeginLightmapSurfacePass();
	#else
	memset(gl_lms.lightmap_surfaces, 0, sizeof(gl_lms.lightmap_surfaces));
	#endif

	VectorSubtract(r_newrefdef.vieworg, currententity->origin, modelorg);

	if (rotated)
	{
		vec3_t temp;
		vec3_t forward, right, up;

		VectorCopy(modelorg, temp);
		AngleVectors(currententity->angles, forward, right, up);
		modelorg[0] = DotProduct(temp, forward);
		modelorg[1] = -DotProduct(temp, right);
		modelorg[2] = DotProduct(temp, up);
	}

	glPushMatrix();
	currententity->angles[0] = -currententity->angles[0];
	currententity->angles[2] = -currententity->angles[2];
	R_RotateForEntity(currententity);
	currententity->angles[0] = -currententity->angles[0];
	currententity->angles[2] = -currententity->angles[2];

	R_TexEnv(GL_REPLACE);

	if (gl_lightmap->value)
	{
		R_TexEnv(GL_REPLACE);
	}
	else
	{
		R_TexEnv(GL_MODULATE);
	}

	R_DrawInlineBModel(currententity, currentmodel);

	glPopMatrix();

	if (gl_zfix->value)
	{
		glDisable(GL_POLYGON_OFFSET_FILL);
	}
}

static void
R_RecursiveWorldNode(entity_t *currententity, mnode_t *node,
	unsigned int clipflags)
{
	int c, side, sidebit;
	cplane_t *plane;
	msurface_t *surf, **mark;
	mleaf_t *pleaf;
	float dot;
	#ifndef PS3_NATIVE_RSX
	image_t *image;
	#endif

	if (node->contents == CONTENTS_SOLID)
	{
		return; /* solid */
	}

	if (node->visframe != r_visframecount)
	{
		return;
	}

	#ifdef PS3_NATIVE_RSX
	/* Once an ancestor proves its complete box is inside every frustum plane,
	 * all descendants inherit a zero mask. Avoid entering the support-corner
	 * function at all for that enclosed subtree. Statistics preserve the four
	 * inherited skips which the former zero-mask call would have recorded. */
	if (!clipflags)
	{
		if (ps3_world_clip_stats)
		{
			ps3_world_clip_skips += 4;
		}
	}
	else if (!R_PS3_ClipWorldBox(node->minmaxs, node->minmaxs + 3,
		clipflags, &clipflags))
	#else
	if (R_CullBox(node->minmaxs, node->minmaxs + 3))
	#endif
	{
		return;
	}

	/* if a leaf node, draw stuff */
	if (node->contents != -1)
	{
		pleaf = (mleaf_t *)node;

		/* check for door connected areas */
		if (r_newrefdef.areabits)
		{
			if (!(r_newrefdef.areabits[pleaf->area >> 3] & (1 << (pleaf->area & 7))))
			{
				return; /* not visible */
			}
		}

		mark = pleaf->firstmarksurface;
		c = pleaf->nummarksurfaces;

		if (c)
		{
			do
			{
				(*mark)->visframe = r_framecount;
				mark++;
			}
			while (--c);
		}

		return;
	}

	/* node is just a decision point, so go down the apropriate
	   sides find which side of the node we are on */
	plane = node->plane;

	switch (plane->type)
	{
		case PLANE_X:
			dot = modelorg[0] - plane->dist;
			break;
		case PLANE_Y:
			dot = modelorg[1] - plane->dist;
			break;
		case PLANE_Z:
			dot = modelorg[2] - plane->dist;
			break;
		default:
			dot = DotProduct(modelorg, plane->normal) - plane->dist;
			break;
	}

	if (dot >= 0)
	{
		side = 0;
		sidebit = 0;
	}
	else
	{
		side = 1;
		sidebit = SURF_PLANEBACK;
	}

	/* recurse down the children, front side first */
	R_RecursiveWorldNode(currententity, node->children[side], clipflags);

	/* draw stuff */
	for (c = node->numsurfaces,
		 surf = r_worldmodel->surfaces + node->firstsurface;
		 c; c--, surf++)
	{
		if (surf->visframe != r_framecount)
		{
			continue;
		}

		if ((surf->flags & SURF_PLANEBACK) != sidebit)
		{
			continue; /* wrong side */
		}

		#ifdef PS3_NATIVE_RSX
		R_PS3_ChainWorldSurface(currententity, surf);
		#else
		if (surf->texinfo->flags & SURF_SKY)
		{
			/* just adds to visible sky bounds */
			R_AddSkySurface(surf);
		}
		else if (surf->texinfo->flags & (SURF_TRANS33 | SURF_TRANS66))
		{
			/* add to the translucent chain */
			surf->texturechain = r_alpha_surfaces;
			r_alpha_surfaces = surf;
			r_alpha_surfaces->texinfo->image = R_TextureAnimation(currententity, surf->texinfo);
		}
		else
		{
			/* the polygon is visible, so add it to the texture sorted chain */
			image = R_TextureAnimation(currententity, surf->texinfo);
			surf->texturechain = image->texturechain;
			image->texturechain = surf;
		}
		#endif
	}

	/* recurse down the back side */
	R_RecursiveWorldNode(currententity, node->children[!side], clipflags);
}

void
R_DrawWorld(void)
{
	entity_t ent;
	const model_t *currentmodel;
	if (!r_drawworld->value)
	{
		return;
	}

	if (r_newrefdef.rdflags & RDF_NOWORLDMODEL)
	{
		return;
	}

	currentmodel = r_worldmodel;

	VectorCopy(r_newrefdef.vieworg, modelorg);

	/* auto cycle the world frame for texture animation */
	memset(&ent, 0, sizeof(ent));
	ent.frame = (int)(r_newrefdef.time * 2);

	gl_state.currenttextures[0] = gl_state.currenttextures[1] = -1;

	glColor4f(1, 1, 1, 1);
	#ifdef PS3_NATIVE_RSX
	R_PS3_BeginLightmapSurfacePass();
	#else
	memset(gl_lms.lightmap_surfaces, 0, sizeof(gl_lms.lightmap_surfaces));
	#endif

	R_ClearSkyBox();
	#ifdef PS3_NATIVE_RSX
	ps3_world_clip_stats = RSXGL_StatsEnabled() && gl_cull->value;
	ps3_world_clip_tests = 0;
	ps3_world_clip_skips = 0;
	#endif
	R_RecursiveWorldNode(&ent, r_worldmodel->nodes,
		gl_cull->value ? 15u : 0u);
	#ifdef PS3_NATIVE_RSX
	if (ps3_world_clip_stats)
	{
		RSXGL_RecordWorldClip(ps3_world_clip_tests,
			ps3_world_clip_skips);
	}
	#endif
	R_DrawTextureChains(&ent);
	R_BlendLightmaps(currentmodel);
	R_DrawSkyBox();
	R_DrawTriangleOutlines();
}

/*
 * Mark the leaves and nodes that are
 * in the PVS for the current cluster
 */
void
R_MarkLeaves(void)
{
	const byte *vis;
	YQ2_ALIGNAS_TYPE(int) byte fatvis[MAX_MAP_LEAFS / 8];
	mnode_t *node;
	int i, c;
	mleaf_t *leaf;
	int cluster;

	if ((r_oldviewcluster == r_viewcluster) &&
		(r_oldviewcluster2 == r_viewcluster2) &&
		!r_novis->value &&
		(r_viewcluster != -1))
	{
		return;
	}

	/* development aid to let you run around
	   and see exactly where the pvs ends */
	if (r_lockpvs->value)
	{
		return;
	}

	r_visframecount++;
	r_oldviewcluster = r_viewcluster;
	r_oldviewcluster2 = r_viewcluster2;

	if (r_novis->value || (r_viewcluster == -1) || !r_worldmodel->vis)
	{
		/* mark everything */
		for (i = 0; i < r_worldmodel->numleafs; i++)
		{
			r_worldmodel->leafs[i].visframe = r_visframecount;
		}

		for (i = 0; i < r_worldmodel->numnodes; i++)
		{
			r_worldmodel->nodes[i].visframe = r_visframecount;
		}

		return;
	}

	vis = Mod_ClusterPVS(r_viewcluster, r_worldmodel);

	/* may have to combine two clusters because of solid water boundaries */
	if (r_viewcluster2 != r_viewcluster)
	{
		memcpy(fatvis, vis, (r_worldmodel->numleafs + 7) / 8);
		vis = Mod_ClusterPVS(r_viewcluster2, r_worldmodel);
		c = (r_worldmodel->numleafs + 31) / 32;

		for (i = 0; i < c; i++)
		{
			((int *)fatvis)[i] |= ((int *)vis)[i];
		}

		vis = fatvis;
	}

	for (i = 0, leaf = r_worldmodel->leafs;
		 i < r_worldmodel->numleafs;
		 i++, leaf++)
	{
		cluster = leaf->cluster;

		if (cluster == -1)
		{
			continue;
		}

		if (vis[cluster >> 3] & (1 << (cluster & 7)))
		{
			node = (mnode_t *)leaf;

			do
			{
				if (node->visframe == r_visframecount)
				{
					break;
				}

				node->visframe = r_visframecount;
				node = node->parent;
			}
			while (node);
		}
	}
}
