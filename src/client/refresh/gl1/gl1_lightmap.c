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
 * Lightmap handling
 *
 * =======================================================================
 */

#include "header/local.h"

#ifdef __PSL1GHT__
/* Avoid a 64 KiB local array during map lightmap registration. */
static unsigned ps3_empty_lightmap[128 * 128];
#endif

extern gllightmapstate_t gl_lms;

void R_SetCacheState(msurface_t *surf);
void R_BuildLightMap(msurface_t *surf, byte *dest, int stride);

int
LM_DynamicTextureIndex(void)
{
#ifdef PS3_NATIVE_RSX
	static qboolean traced_stereo_atlas;

	if (r_ps3_second_stereo_eye && R_PS3_StereoLightmapAtlasEnabled())
	{
		if (!traced_stereo_atlas)
		{
			PS3_RUNTIME_TRACE(
				"RSX renderer: independent second-eye dynamic lightmap atlas active");
			traced_stereo_atlas = true;
		}
		return PS3_STEREO_DYNAMIC_LIGHTMAP;
	}
#endif
	return 0;
}

void
LM_InitBlock(void)
{
	memset(gl_lms.allocated, 0, sizeof(gl_lms.allocated));
}

void
LM_UploadBlock(qboolean dynamic)
{
	int texture;
	int height = 0;

	if (dynamic)
	{
		texture = LM_DynamicTextureIndex();
	}
	else
	{
		texture = gl_lms.current_lightmap_texture;
	}

	if (dynamic)
	{
		int i;
		/* R_BlendLightmaps selected this eye's dynamic atlas before packing,
		 * and every intervening draw samples that same atlas. Avoid repeating
		 * the cached bind and two immutable filter calls for every upload. */

		for (i = 0; i < BLOCK_WIDTH; i++)
		{
			if (gl_lms.allocated[i] > height)
			{
				height = gl_lms.allocated[i];
			}
		}

		glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, BLOCK_WIDTH,
				height, GL_LIGHTMAP_FORMAT, GL_UNSIGNED_BYTE,
				gl_lms.lightmap_buffer);
	}
	else
	{
		R_Bind(gl_state.lightmap_textures + texture);
		glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
		glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
		gl_lms.internal_format = GL_LIGHTMAP_FORMAT;
		glTexImage2D(GL_TEXTURE_2D, 0, gl_lms.internal_format,
				BLOCK_WIDTH, BLOCK_HEIGHT, 0, GL_LIGHTMAP_FORMAT,
				GL_UNSIGNED_BYTE, gl_lms.lightmap_buffer);

		if (++gl_lms.current_lightmap_texture == MAX_LIGHTMAPS)
		{
			ri.Sys_Error(ERR_DROP,
					"LM_UploadBlock() - MAX_LIGHTMAPS exceeded\n");
		}
	}
}

/*
 * returns a texture number and the position inside it
 */
qboolean
LM_AllocBlock(int w, int h, int *x, int *y)
{
	int i, j;
	int best, best2;

	best = BLOCK_HEIGHT;

	for (i = 0; i < BLOCK_WIDTH - w; i++)
	{
		best2 = 0;

		for (j = 0; j < w; j++)
		{
			if (gl_lms.allocated[i + j] >= best)
			{
				break;
			}

			if (gl_lms.allocated[i + j] > best2)
			{
				best2 = gl_lms.allocated[i + j];
			}
		}

		if (j == w)
		{
			/* this is a valid spot */
			*x = i;
			*y = best = best2;
		}
	}

	if (best + h > BLOCK_HEIGHT)
	{
		return false;
	}

	for (i = 0; i < w; i++)
	{
		gl_lms.allocated[*x + i] = best + h;
	}

	return true;
}

void
LM_BuildPolygonFromSurface(model_t *currentmodel, msurface_t *fa)
{
	int i, lindex, lnumverts;
	medge_t *pedges, *r_pedge;
	float *vec;
	float s, t;
	glpoly_t *poly;
	vec3_t total;
	size_t poly_size;

	/* reconstruct the polygon */
	pedges = currentmodel->edges;
	lnumverts = fa->numedges;

	VectorClear(total);

	/* draw texture */
	poly_size = sizeof(glpoly_t) +
		(lnumverts - 4) * VERTEXSIZE * sizeof(float);
	#ifdef PS3_NATIVE_RSX
	/* Append one immutable triangle list after the variable vertex payload.
	 * Hunk_Alloc keeps it alive for exactly the same map generation. */
	if (lnumverts >= 3)
		poly_size += (lnumverts - 2) * 3 * sizeof(unsigned short);
	#endif
	poly = Hunk_Alloc(poly_size);
	poly->next = fa->polys;
	poly->chain = NULL;
	poly->flags = fa->flags;
	fa->polys = poly;
	poly->numverts = lnumverts;
	#ifdef PS3_NATIVE_RSX
	poly->ps3_rsx_static_indices = NULL;
	fa->ps3_combined_geometry_eligible =
		fa->lightmaptexturenum > 0 &&
		fa->lightmaptexturenum < MAX_LIGHTMAPS &&
		!(fa->flags & SURF_DRAWTURB) &&
		!(fa->texinfo->flags & SURF_FLOWING);
	#endif

	for (i = 0; i < lnumverts; i++)
	{
		lindex = currentmodel->surfedges[fa->firstedge + i];

		if (lindex > 0)
		{
			r_pedge = &pedges[lindex];
			vec = currentmodel->vertexes[r_pedge->v[0]].position;
		}
		else
		{
			r_pedge = &pedges[-lindex];
			vec = currentmodel->vertexes[r_pedge->v[1]].position;
		}

		s = DotProduct(vec, fa->texinfo->vecs[0]) + fa->texinfo->vecs[0][3];
		s /= fa->texinfo->image->width;

		t = DotProduct(vec, fa->texinfo->vecs[1]) + fa->texinfo->vecs[1][3];
		t /= fa->texinfo->image->height;

		VectorAdd(total, vec, total);
		VectorCopy(vec, poly->verts[i]);
		poly->verts[i][3] = s;
		poly->verts[i][4] = t;

		/* lightmap texture coordinates */
		s = DotProduct(vec, fa->texinfo->vecs[0]) + fa->texinfo->vecs[0][3];
		s -= fa->texturemins[0];
		s += fa->light_s * 16;
		s += 8;
		s /= BLOCK_WIDTH * 16; /* fa->texinfo->texture->width; */

		t = DotProduct(vec, fa->texinfo->vecs[1]) + fa->texinfo->vecs[1][3];
		t -= fa->texturemins[1];
		t += fa->light_t * 16;
		t += 8;
		t /= BLOCK_HEIGHT * 16; /* fa->texinfo->texture->height; */

		poly->verts[i][5] = s;
		poly->verts[i][6] = t;
	}

#ifdef PS3_NATIVE_RSX
	/* Only the opaque, non-flowing, statically shaped lightmap subset can use
	 * the persistent local vertex arena. Runtime lightstyle/dlight changes still
	 * decide whether a registered fan is drawn there or through the established
	 * dynamic-lightmap fallback. */
	if (fa->lightmaptexturenum > 0 &&
		!(fa->texinfo->flags &
			(SURF_SKY | SURF_TRANS33 | SURF_TRANS66 | SURF_WARP | SURF_FLOWING)) &&
		fa->texinfo->image && !fa->texinfo->image->has_alpha)
	{
		if (RSXGL_RegisterStaticLightmappedFan(poly->verts[0], VERTEXSIZE,
			poly->verts[0] + 3, VERTEXSIZE, poly->verts[0] + 5,
			VERTEXSIZE, poly->numverts, &poly->ps3_rsx_static_generation,
			&poly->ps3_rsx_static_vertex_offset,
			&poly->ps3_rsx_static_first_vertex))
		{
			int triangle;
			unsigned short *indices = (unsigned short *)((byte *)poly +
				sizeof(glpoly_t) +
				(lnumverts - 4) * VERTEXSIZE * sizeof(float));

			poly->ps3_rsx_static_indices = indices;
			for (triangle = 0; triangle < lnumverts - 2; triangle++)
			{
				*indices++ = poly->ps3_rsx_static_first_vertex;
				*indices++ = (unsigned short)(
					poly->ps3_rsx_static_first_vertex + triangle + 1);
				*indices++ = (unsigned short)(
					poly->ps3_rsx_static_first_vertex + triangle + 2);
			}
		}
	}
#endif
}

void
LM_CreateSurfaceLightmap(msurface_t *surf)
{
	int smax, tmax;
	byte *base;

	if (surf->flags & (SURF_DRAWSKY | SURF_DRAWTURB))
	{
		return;
	}

	smax = (surf->extents[0] >> 4) + 1;
	tmax = (surf->extents[1] >> 4) + 1;

	if (!LM_AllocBlock(smax, tmax, &surf->light_s, &surf->light_t))
	{
		LM_UploadBlock(false);
		LM_InitBlock();

		if (!LM_AllocBlock(smax, tmax, &surf->light_s, &surf->light_t))
		{
			ri.Sys_Error(ERR_FATAL, "Consecutive calls to LM_AllocBlock(%d,%d) failed\n",
					smax, tmax);
		}
	}

	surf->lightmaptexturenum = gl_lms.current_lightmap_texture;

	base = gl_lms.lightmap_buffer;
	base += (surf->light_t * BLOCK_WIDTH + surf->light_s) * LIGHTMAP_BYTES;

	R_SetCacheState(surf);
	R_BuildLightMap(surf, base, BLOCK_WIDTH * LIGHTMAP_BYTES);
}

void
LM_BeginBuildingLightmaps(model_t *m)
{
	static lightstyle_t lightstyles[MAX_LIGHTSTYLES];
	int i;
	#ifdef __PSL1GHT__
	unsigned *dummy = ps3_empty_lightmap;
	memset(dummy, 0, sizeof(ps3_empty_lightmap));
	#else
	unsigned dummy[128 * 128] = {0};
	#endif

	memset(gl_lms.allocated, 0, sizeof(gl_lms.allocated));

	r_framecount = 1; /* no dlightcache */

	/* setup the base lightstyles so the lightmaps
	   won't have to be regenerated the first time
	   they're seen */
	for (i = 0; i < MAX_LIGHTSTYLES; i++)
	{
		lightstyles[i].rgb[0] = 1;
		lightstyles[i].rgb[1] = 1;
		lightstyles[i].rgb[2] = 1;
		lightstyles[i].white = 3;
	}

	r_newrefdef.lightstyles = lightstyles;

	if (!gl_state.lightmap_textures)
	{
		gl_state.lightmap_textures = TEXNUM_LIGHTMAPS;
	}

	gl_lms.current_lightmap_texture = 1;
	gl_lms.internal_format = GL_LIGHTMAP_FORMAT;

	/* initialize the dynamic lightmap texture */
	R_Bind(gl_state.lightmap_textures + 0);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
	glTexImage2D(GL_TEXTURE_2D, 0, gl_lms.internal_format,
			BLOCK_WIDTH, BLOCK_HEIGHT, 0, GL_LIGHTMAP_FORMAT,
			GL_UNSIGNED_BYTE, dummy);

	#ifdef PS3_NATIVE_RSX
	/* A separate 64 KiB atlas keeps eye two's first upload on the existing
	 * pre-sample in-place path. Sharing texture zero would force a rename and a
	 * complete old-allocation copy after eye one had already sampled it. */
	R_Bind(gl_state.lightmap_textures + PS3_STEREO_DYNAMIC_LIGHTMAP);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
	glTexImage2D(GL_TEXTURE_2D, 0, gl_lms.internal_format,
			BLOCK_WIDTH, BLOCK_HEIGHT, 0, GL_LIGHTMAP_FORMAT,
			GL_UNSIGNED_BYTE, dummy);
	#endif
}

void
LM_EndBuildingLightmaps(void)
{
	LM_UploadBlock(false);
}
