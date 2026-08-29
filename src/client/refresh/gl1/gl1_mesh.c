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
 * Mesh handling
 *
 * =======================================================================
 */

#include <stddef.h>

#include "header/local.h"

#define NUMVERTEXNORMALS 162
#define SHADEDOT_QUANT 16

float r_avertexnormals[NUMVERTEXNORMALS][3] = {
#include "../constants/anorms.h"
};

/* precalculated dot products for quantized angles */
float r_avertexnormal_dots[SHADEDOT_QUANT][256] = {
#include "../constants/anormtab.h"
};

typedef float vec4_t[4];
static vec4_t s_lerped[MAX_VERTS];
vec3_t shadevector;
float shadelight[3];
float *shadedots = r_avertexnormal_dots[0];
extern vec3_t lightspot;
#ifdef PS3_NATIVE_RSX
#define PS3_ALIAS_STEREO_CACHE_BYTES (1024u * 1024u)
#define PS3_ALIAS_STEREO_CACHE_VERTS \
	(PS3_ALIAS_STEREO_CACHE_BYTES / sizeof(vec4_t))
#define PS3_ALIAS_STEREO_CACHE_ALIGN 16u

typedef struct
{
	unsigned int generation;
	const entity_t *entity;
	const dmdl_t *model;
	vec4_t *lerped;
	int num_lerped;
	GLfloat *colors;
	int num_colors;
	vec4_t *shadow_vertices;
	int num_shadow_vertices;
	qboolean lighting_valid;
	vec3_t cached_shadelight;
	vec3_t cached_shadevector;
	vec3_t cached_lightspot;
	int shadedot_index;
	float lightlevel;
} ps3_alias_stereo_entry_t;

/* Model-space interpolation, entity lighting, one color per source vertex,
 * and one projected position per shadow source vertex do not depend on the
 * stereo eye. Keep a bounded one-visual-frame arena so the second eye retains
 * those unique results without caching expanded strip/fan corners. */
static ps3_alias_stereo_entry_t ps3_alias_stereo_entries[MAX_ENTITIES];
static vec4_t ps3_alias_stereo_arena[PS3_ALIAS_STEREO_CACHE_VERTS];
static GLfloat ps3_alias_source_clr[4 * MAX_VERTS];
static vec4_t ps3_alias_shadow_vertices[MAX_VERTS];
static unsigned int ps3_alias_stereo_generation;
static size_t ps3_alias_stereo_bytes_used;
static qboolean ps3_alias_stereo_active;
static qboolean ps3_alias_stereo_second_eye;
static qboolean ps3_traced_stereo_alias_lighting;
static qboolean ps3_traced_stereo_alias_lerp;
static qboolean ps3_traced_stereo_alias_colors;
static qboolean ps3_traced_stereo_alias_shadow_vertices;
static qboolean ps3_traced_projected_shadow;
static qboolean ps3_traced_alias_obb_cull;
static qboolean ps3_traced_prepared_alias_topology;

static size_t
R_PS3_AliasArenaAlign(size_t value)
{
	return (value + PS3_ALIAS_STEREO_CACHE_ALIGN - 1) &
		~(size_t)(PS3_ALIAS_STEREO_CACHE_ALIGN - 1);
}

static void *
R_PS3_AliasArenaAlloc(size_t bytes)
{
	size_t offset = R_PS3_AliasArenaAlign(ps3_alias_stereo_bytes_used);

	if (offset > PS3_ALIAS_STEREO_CACHE_BYTES ||
		bytes > PS3_ALIAS_STEREO_CACHE_BYTES - offset)
	{
		return NULL;
	}
	ps3_alias_stereo_bytes_used = offset + bytes;
	return (byte *)ps3_alias_stereo_arena + offset;
}

void
R_PS3_BeginAliasStereoEye(float camera_separation, qboolean second_eye)
{
	if (camera_separation == 0.0f)
	{
		ps3_alias_stereo_active = false;
		ps3_alias_stereo_second_eye = false;
		return;
	}

	ps3_alias_stereo_active = true;
	ps3_alias_stereo_second_eye = second_eye;
	if (!second_eye)
	{
		ps3_alias_stereo_generation++;
		if (!ps3_alias_stereo_generation)
		{
			memset(ps3_alias_stereo_entries, 0,
				sizeof(ps3_alias_stereo_entries));
			ps3_alias_stereo_generation = 1;
		}
		ps3_alias_stereo_bytes_used = 0;
	}
}

void
R_PS3_EndAliasStereoPair(void)
{
	ps3_alias_stereo_active = false;
	ps3_alias_stereo_second_eye = false;
}

static int
R_PS3_AliasEntityIndex(const entity_t *currententity)
{
	ptrdiff_t index;

	if (!r_newrefdef.entities || r_newrefdef.num_entities <= 0)
	{
		return -1;
	}
	index = currententity - r_newrefdef.entities;
	if (index < 0 || index >= r_newrefdef.num_entities ||
		index >= MAX_ENTITIES)
	{
		return -1;
	}
	return (int)index;
}

static ps3_alias_stereo_entry_t *
R_PS3_AliasStereoEntry(entity_t *currententity, dmdl_t *paliashdr,
	qboolean create)
{
	ps3_alias_stereo_entry_t *entry;
	int index;

	if (!ps3_alias_stereo_active)
	{
		return NULL;
	}
	index = R_PS3_AliasEntityIndex(currententity);
	if (index < 0)
	{
		return NULL;
	}
	entry = &ps3_alias_stereo_entries[index];
	if (entry->generation == ps3_alias_stereo_generation &&
		entry->entity == currententity && entry->model == paliashdr)
	{
		return entry;
	}
	if (!create || ps3_alias_stereo_second_eye)
	{
		return NULL;
	}

	memset(entry, 0, sizeof(*entry));
	entry->generation = ps3_alias_stereo_generation;
	entry->entity = currententity;
	entry->model = paliashdr;
	return entry;
}

static vec4_t *
R_PS3_AliasLerpBuffer(entity_t *currententity, dmdl_t *paliashdr,
	int numverts, qboolean *reused)
{
	ps3_alias_stereo_entry_t *entry;

	*reused = false;
	entry = R_PS3_AliasStereoEntry(currententity, paliashdr,
		!ps3_alias_stereo_second_eye);
	if (!entry)
	{
		return s_lerped;
	}
	if (ps3_alias_stereo_second_eye)
	{
		if (entry->lerped && entry->num_lerped == numverts)
		{
			*reused = true;
			if (!ps3_traced_stereo_alias_lerp)
			{
				PS3_RUNTIME_TRACE(
					"RSX renderer: stereo alias interpolation reused on second eye");
				ps3_traced_stereo_alias_lerp = true;
			}
			return entry->lerped;
		}
		return s_lerped;
	}

	if (numverts > 0)
	{
		entry->lerped = R_PS3_AliasArenaAlloc(
			(size_t)numverts * sizeof(vec4_t));
		if (entry->lerped)
		{
			entry->num_lerped = numverts;
			return entry->lerped;
		}
	}
	return s_lerped;
}

static GLfloat *
R_PS3_AliasColorBuffer(entity_t *currententity, dmdl_t *paliashdr,
	int numverts, qboolean *reused)
{
	ps3_alias_stereo_entry_t *entry;

	*reused = false;
	entry = R_PS3_AliasStereoEntry(currententity, paliashdr,
		!ps3_alias_stereo_second_eye);
	if (!entry)
	{
		return ps3_alias_source_clr;
	}
	if (ps3_alias_stereo_second_eye)
	{
		if (entry->colors && entry->num_colors == numverts)
		{
			*reused = true;
			if (!ps3_traced_stereo_alias_colors)
			{
				PS3_RUNTIME_TRACE(
					"RSX renderer: stereo alias source colors reused on second eye");
				ps3_traced_stereo_alias_colors = true;
			}
			return entry->colors;
		}
		return ps3_alias_source_clr;
	}

	entry->colors = R_PS3_AliasArenaAlloc(
		(size_t)numverts * 4 * sizeof(GLfloat));
	if (entry->colors)
	{
		entry->num_colors = numverts;
		return entry->colors;
	}
	return ps3_alias_source_clr;
}

static vec4_t *
R_PS3_AliasShadowBuffer(entity_t *currententity, dmdl_t *paliashdr,
	int numverts, qboolean *reused)
{
	ps3_alias_stereo_entry_t *entry;

	*reused = false;
	entry = R_PS3_AliasStereoEntry(currententity, paliashdr,
		!ps3_alias_stereo_second_eye);
	/* Preserve the previous paired-eye proof: only a retained BSP sample
	 * guarantees that lightspot and therefore the shadow projection are
	 * identical. Shell/fullbright/no-lightdata fallback state stays local to
	 * each eye. */
	if (!entry || !entry->lighting_valid)
	{
		return ps3_alias_shadow_vertices;
	}
	if (ps3_alias_stereo_second_eye)
	{
		if (entry->shadow_vertices && entry->num_shadow_vertices == numverts)
		{
			*reused = true;
			if (!ps3_traced_stereo_alias_shadow_vertices)
			{
				PS3_RUNTIME_TRACE(
					"RSX renderer: stereo alias projected source vertices reused on second eye");
				ps3_traced_stereo_alias_shadow_vertices = true;
			}
			return entry->shadow_vertices;
		}
		return ps3_alias_shadow_vertices;
	}

	entry->shadow_vertices = R_PS3_AliasArenaAlloc(
		(size_t)numverts * sizeof(vec4_t));
	if (entry->shadow_vertices)
	{
		entry->num_shadow_vertices = numverts;
		return entry->shadow_vertices;
	}
	return ps3_alias_shadow_vertices;
}

static qboolean
R_PS3_RestoreAliasLighting(entity_t *currententity, dmdl_t *paliashdr)
{
	ps3_alias_stereo_entry_t *entry;

	if (!ps3_alias_stereo_second_eye)
	{
		return false;
	}
	entry = R_PS3_AliasStereoEntry(currententity, paliashdr, false);
	if (!entry || !entry->lighting_valid)
	{
		return false;
	}

	VectorCopy(entry->cached_shadelight, shadelight);
	VectorCopy(entry->cached_shadevector, shadevector);
	VectorCopy(entry->cached_lightspot, lightspot);
	shadedots = r_avertexnormal_dots[entry->shadedot_index];
	if (currententity->flags & RF_WEAPONMODEL)
	{
		r_lightlevel->value = entry->lightlevel;
	}
	if (!ps3_traced_stereo_alias_lighting)
	{
		PS3_RUNTIME_TRACE(
			"RSX renderer: stereo alias lighting reused on second eye");
		ps3_traced_stereo_alias_lighting = true;
	}
	return true;
}

static void
R_PS3_StoreAliasLighting(entity_t *currententity, dmdl_t *paliashdr,
	int shadedot_index)
{
	ps3_alias_stereo_entry_t *entry;

	if (ps3_alias_stereo_second_eye)
	{
		return;
	}
	entry = R_PS3_AliasStereoEntry(currententity, paliashdr, true);
	if (!entry)
	{
		return;
	}
	VectorCopy(shadelight, entry->cached_shadelight);
	VectorCopy(shadevector, entry->cached_shadevector);
	VectorCopy(lightspot, entry->cached_lightspot);
	entry->shadedot_index = shadedot_index;
	entry->lightlevel = r_lightlevel->value;
	entry->lighting_valid = true;
}
#endif

#if defined(__PSL1GHT__) && !defined(PS3_NATIVE_RSX)
/* The MD2 loader rejects models above MAX_VERTS. Alias rendering and its
 * projected-shadow pass are sequential, so they can share one fixed vertex
 * workspace and avoid three malloc/free pairs per visible entity per frame. */
static GLfloat ps3_alias_vtx[3 * MAX_VERTS];
static GLfloat ps3_alias_tex[2 * MAX_VERTS];
static GLfloat ps3_alias_clr[4 * MAX_VERTS];
#endif

static void
R_BeginAliasPrimitives(void)
{
	#ifndef PS3_NATIVE_RSX
	glEnableClientState(GL_VERTEX_ARRAY);
	glEnableClientState(GL_TEXTURE_COORD_ARRAY);
	glEnableClientState(GL_COLOR_ARRAY);
	#endif
}

#ifndef PS3_NATIVE_RSX
static void
R_DrawAliasPrimitive(GLenum type, unsigned short total,
	const GLfloat *vtx, const GLfloat *tex, const GLfloat *clr)
{
	glVertexPointer(3, GL_FLOAT, 0, vtx);
	glTexCoordPointer(2, GL_FLOAT, 0, tex);
	glColorPointer(4, GL_FLOAT, 0, clr);
	glDrawArrays(type, 0, total);
}
#endif

static void
R_EndAliasPrimitives(void)
{
	#ifndef PS3_NATIVE_RSX
	glDisableClientState(GL_VERTEX_ARRAY);
	glDisableClientState(GL_TEXTURE_COORD_ARRAY);
	glDisableClientState(GL_COLOR_ARRAY);
	#endif
}

static void
R_LerpVerts(entity_t *currententity, int nverts, dtrivertx_t *v, dtrivertx_t *ov,
		dtrivertx_t *verts, float *lerp, float move[3],
		float frontv[3], float backv[3])
{
	int i;

	if (currententity->flags &
		(RF_SHELL_RED | RF_SHELL_GREEN |
		 RF_SHELL_BLUE | RF_SHELL_DOUBLE |
		 RF_SHELL_HALF_DAM))
	{
		for (i = 0; i < nverts; i++, v++, ov++, lerp += 4)
		{
			float *normal = r_avertexnormals[verts[i].lightnormalindex];

			lerp[0] = move[0] + ov->v[0] * backv[0] + v->v[0] * frontv[0] +
					  normal[0] * POWERSUIT_SCALE;
			lerp[1] = move[1] + ov->v[1] * backv[1] + v->v[1] * frontv[1] +
					  normal[1] * POWERSUIT_SCALE;
			lerp[2] = move[2] + ov->v[2] * backv[2] + v->v[2] * frontv[2] +
					  normal[2] * POWERSUIT_SCALE;
		}
	}
	else
	{
		for (i = 0; i < nverts; i++, v++, ov++, lerp += 4)
		{
			lerp[0] = move[0] + ov->v[0] * backv[0] + v->v[0] * frontv[0];
			lerp[1] = move[1] + ov->v[1] * backv[1] + v->v[1] * frontv[1];
			lerp[2] = move[2] + ov->v[2] * backv[2] + v->v[2] * frontv[2];
		}
	}
}

/*
 * Expand one MD2 frame without evaluating the inactive half of the usual
 * two-frame blend. This covers non-animated props, identical animation
 * frames, and the exact-current-frame endpoint. The shell displacement still
 * uses the current frame normal, matching R_LerpVerts().
 */
static void
R_LerpVertsCurrent(entity_t *currententity, int nverts, dtrivertx_t *v,
		float *lerp, const float move[3], const float scale[3])
{
	int i;

	if (currententity->flags &
		(RF_SHELL_RED | RF_SHELL_GREEN |
		 RF_SHELL_BLUE | RF_SHELL_DOUBLE |
		 RF_SHELL_HALF_DAM))
	{
		for (i = 0; i < nverts; i++, v++, lerp += 4)
		{
			float *normal = r_avertexnormals[v->lightnormalindex];

			lerp[0] = move[0] + v->v[0] * scale[0] +
				normal[0] * POWERSUIT_SCALE;
			lerp[1] = move[1] + v->v[1] * scale[1] +
				normal[1] * POWERSUIT_SCALE;
			lerp[2] = move[2] + v->v[2] * scale[2] +
				normal[2] * POWERSUIT_SCALE;
		}
	}
	else
	{
		for (i = 0; i < nverts; i++, v++, lerp += 4)
		{
			lerp[0] = move[0] + v->v[0] * scale[0];
			lerp[1] = move[1] + v->v[1] * scale[1];
			lerp[2] = move[2] + v->v[2] * scale[2];
		}
	}
}

/*
 * Interpolates between two frames and origins
 */
static vec4_t *
R_DrawAliasFrameLerp(entity_t *currententity, const model_t *currentmodel,
	dmdl_t *paliashdr, float backlerp)
{
	daliasframe_t *frame, *oldframe;
	dtrivertx_t *v, *ov, *verts;
	#ifndef PS3_NATIVE_RSX
	unsigned short total;
	GLenum type;
	float l;
	int *order;
	int count;
	int index_xyz;
	#endif
	float frontlerp;
	float alpha;
	vec3_t move, delta, vectors[3];
	vec3_t frontv, backv;
	int i;
	vec4_t *lerped_vertices = s_lerped;
	qboolean reused_lerp = false;

	frame = (daliasframe_t *)((byte *)paliashdr + paliashdr->ofs_frames
							  + currententity->frame * paliashdr->framesize);
	verts = v = frame->verts;

	#ifndef PS3_NATIVE_RSX
	order = (int *)((byte *)paliashdr + paliashdr->ofs_glcmds);
	#endif

	if (currententity->flags & RF_TRANSLUCENT)
	{
		alpha = currententity->alpha;
	}
	else
	{
		alpha = 1.0;
	}

	if (currententity->flags &
		(RF_SHELL_RED | RF_SHELL_GREEN | RF_SHELL_BLUE | RF_SHELL_DOUBLE |
		 RF_SHELL_HALF_DAM))
	{
		glDisable(GL_TEXTURE_2D);
	}

	#ifdef PS3_NATIVE_RSX
	lerped_vertices = R_PS3_AliasLerpBuffer(currententity, paliashdr,
		paliashdr->num_xyz, &reused_lerp);
	#endif
	if (!reused_lerp)
	{
		/* At the exact current-frame endpoint, neither the old frame nor the
		 * old origin contributes. This is also the path used when model lerping
		 * is disabled. */
		if (backlerp == 0.0f)
		{
			R_LerpVertsCurrent(currententity, paliashdr->num_xyz, v,
				lerped_vertices[0], frame->translate, frame->scale);
		}
		else
		{
			qboolean stationary_origin =
				currententity->oldorigin[0] == currententity->origin[0] &&
				currententity->oldorigin[1] == currententity->origin[1] &&
				currententity->oldorigin[2] == currententity->origin[2];

			oldframe = (daliasframe_t *)((byte *)paliashdr +
				paliashdr->ofs_frames +
				currententity->oldframe * paliashdr->framesize);
			ov = oldframe->verts;

			/* A repeated frame needs only one source vertex. Preserve the
			 * origin interpolation used by RF_FRAMELERP entities when they move. */
			if (oldframe == frame)
			{
				if (stationary_origin)
				{
					VectorCopy(frame->translate, move);
				}
				else
				{
					VectorSubtract(currententity->oldorigin,
						currententity->origin, delta);
					AngleVectors(currententity->angles, vectors[0],
						vectors[1], vectors[2]);
					move[0] = frame->translate[0] + backlerp *
						DotProduct(delta, vectors[0]);
					move[1] = frame->translate[1] - backlerp *
						DotProduct(delta, vectors[1]);
					move[2] = frame->translate[2] + backlerp *
						DotProduct(delta, vectors[2]);
				}

				R_LerpVertsCurrent(currententity, paliashdr->num_xyz, v,
					lerped_vertices[0], move, frame->scale);
			}
			else
			{
				frontlerp = 1.0f - backlerp;

				/* Most regular entities already have an interpolated origin and
				 * therefore an identical oldorigin. Avoid building an angle basis
				 * merely to transform a zero delta. */
				if (stationary_origin)
				{
					VectorCopy(oldframe->translate, move);
				}
				else
				{
					VectorSubtract(currententity->oldorigin,
						currententity->origin, delta);
					AngleVectors(currententity->angles, vectors[0],
						vectors[1], vectors[2]);

					move[0] = DotProduct(delta, vectors[0]); /* forward */
					move[1] = -DotProduct(delta, vectors[1]); /* left */
					move[2] = DotProduct(delta, vectors[2]); /* up */
					VectorAdd(move, oldframe->translate, move);
				}

				for (i = 0; i < 3; i++)
				{
					move[i] = backlerp * move[i] +
						frontlerp * frame->translate[i];
					frontv[i] = frontlerp * frame->scale[i];
					backv[i] = backlerp * oldframe->scale[i];
				}

				R_LerpVerts(currententity, paliashdr->num_xyz, v, ov, verts,
					lerped_vertices[0], move, frontv, backv);
			}
		}
	}

	R_BeginAliasPrimitives();

	#ifdef PS3_NATIVE_RSX
	{
		GLfloat *source_colors;
		qboolean reused_colors;

		source_colors = R_PS3_AliasColorBuffer(currententity, paliashdr,
			paliashdr->num_xyz, &reused_colors);
		if (!reused_colors)
		{
			if (currententity->flags &
				(RF_SHELL_RED | RF_SHELL_GREEN | RF_SHELL_BLUE))
			{
				for (i = 0; i < paliashdr->num_xyz; i++)
				{
					source_colors[4 * i + 0] = shadelight[0];
					source_colors[4 * i + 1] = shadelight[1];
					source_colors[4 * i + 2] = shadelight[2];
					source_colors[4 * i + 3] = alpha;
				}
			}
			else
			{
				for (i = 0; i < paliashdr->num_xyz; i++)
				{
					float source_light =
						shadedots[verts[i].lightnormalindex];

					source_colors[4 * i + 0] =
						source_light * shadelight[0];
					source_colors[4 * i + 1] =
						source_light * shadelight[1];
					source_colors[4 * i + 2] =
						source_light * shadelight[2];
					source_colors[4 * i + 3] = alpha;
				}
			}
		}

		RSXGL_DrawPreparedAlias(lerped_vertices[0], paliashdr->num_xyz, 4,
			source_colors, 4, currentmodel->ps3_alias_draws,
			currentmodel->ps3_alias_num_draws,
			currentmodel->ps3_alias_refs,
			currentmodel->ps3_alias_num_refs,
			currentmodel->ps3_alias_indices,
			currentmodel->ps3_alias_num_indices);
		if (!ps3_traced_prepared_alias_topology)
		{
			PS3_RUNTIME_TRACE(
				"RSX renderer: prepared alias-model topology active");
			ps3_traced_prepared_alias_topology = true;
		}
	}
	#else
#if defined(__PSL1GHT__)
	GLfloat *vtx = ps3_alias_vtx;
	GLfloat *tex = ps3_alias_tex;
	GLfloat *clr = ps3_alias_clr;
#elif defined(_MSC_VER)
	int maxCount = 0;
	const int* tmpOrder = order;
	while (1)
	{
		int c = *tmpOrder++;
		if (!c)
			break;
		if ( c < 0 )
			c = -c;
		if ( c > maxCount )
			maxCount = c;

		tmpOrder += 3 * c;
	}

	YQ2_VLA( GLfloat, vtx, 3 * maxCount );
	YQ2_VLA( GLfloat, tex, 2 * maxCount );
	YQ2_VLA( GLfloat, clr, 4 * maxCount );
#endif

		while (1)
		{
			/* get the vertex count and primitive type */
			count = *order++;

			if (!count)
			{
				break; /* done */
			}

			if (count < 0)
			{
				count = -count;

                type = GL_TRIANGLE_FAN;
			}
			else
			{
                type = GL_TRIANGLE_STRIP;
			}

			total = count;

#if !defined(_MSC_VER) && !defined(__PSL1GHT__) // desktop compilers use a scoped VLA
			YQ2_VLA(GLfloat, vtx, 3*total);
			YQ2_VLA(GLfloat, tex, 2*total);
			YQ2_VLA(GLfloat, clr, 4*total);
#endif
			unsigned int index_vtx = 0;
			unsigned int index_tex = 0;
			unsigned int index_clr = 0;
			if (currententity->flags &
				(RF_SHELL_RED | RF_SHELL_GREEN | RF_SHELL_BLUE))
			{
				do
				{
					index_xyz = order[2];
					order += 3;

					clr[index_clr++] = shadelight[0];
					clr[index_clr++] = shadelight[1];
					clr[index_clr++] = shadelight[2];
					clr[index_clr++] = alpha;

					vtx[index_vtx++] = lerped_vertices[index_xyz][0];
					vtx[index_vtx++] = lerped_vertices[index_xyz][1];
					vtx[index_vtx++] = lerped_vertices[index_xyz][2];
				}
				while (--count);
			}
			else
			{
				do
				{
					/* texture coordinates come from the draw list */
					tex[index_tex++] = ((float *) order)[0];
					tex[index_tex++] = ((float *) order)[1];

					index_xyz = order[2];
					order += 3;

					/* normals and vertexes come from the frame list */
					l = shadedots[verts[index_xyz].lightnormalindex];

					clr[index_clr++] = l * shadelight[0];
					clr[index_clr++] = l * shadelight[1];
					clr[index_clr++] = l * shadelight[2];
					clr[index_clr++] = alpha;

					vtx[index_vtx++] = lerped_vertices[index_xyz][0];
					vtx[index_vtx++] = lerped_vertices[index_xyz][1];
					vtx[index_vtx++] = lerped_vertices[index_xyz][2];
				}
				while (--count);
			}

			R_DrawAliasPrimitive(type, total, vtx, tex, clr);
		}

	#ifndef __PSL1GHT__
		YQ2_VLAFREE( vtx );
		YQ2_VLAFREE( tex );
		YQ2_VLAFREE( clr )
	#endif
	#endif

	R_EndAliasPrimitives();

	if (currententity->flags &
		(RF_SHELL_RED | RF_SHELL_GREEN | RF_SHELL_BLUE |
		 RF_SHELL_DOUBLE | RF_SHELL_HALF_DAM))
	{
		glEnable(GL_TEXTURE_2D);
	}
	return lerped_vertices;
}

#ifndef PS3_NATIVE_RSX
static void
R_DrawAliasShadowPrimitive(GLenum type, unsigned short total,
	const GLfloat *vtx)
{
	glVertexPointer(3, GL_FLOAT, 0, vtx);
	glDrawArrays(type, 0, total);
}
#endif

static int
R_DrawAliasShadow(entity_t *currententity, const model_t *currentmodel,
	dmdl_t *paliashdr, int posenum, const vec4_t *lerped_vertices)
{
	#ifndef PS3_NATIVE_RSX
	unsigned short total;
	GLenum type;
	int *order;
	vec3_t point;
	int count;
	#endif
	float height = 0, lheight;
	int submitted_vertices = 0;
	#ifdef PS3_NATIVE_RSX
	vec4_t *projected_vertices;
	qboolean reused_shadow_vertices;
	int i;
	#endif

	/* stencilbuffer shadows */
	if (gl_state.stencil && gl1_stencilshadow->value)
	{
		glEnable(GL_STENCIL_TEST);
		glStencilFunc(GL_EQUAL, 1, 2);
		glStencilOp(GL_KEEP, GL_KEEP, GL_INCR);
	}
	#ifndef PS3_NATIVE_RSX
	glEnableClientState(GL_VERTEX_ARRAY);
	#endif

	lheight = currententity->origin[2] - lightspot[2];
	#ifndef PS3_NATIVE_RSX
	order = (int *)((byte *)paliashdr + paliashdr->ofs_glcmds);
	#endif
	height = -lheight + 0.1f;

	#ifdef PS3_NATIVE_RSX
	projected_vertices = R_PS3_AliasShadowBuffer(currententity, paliashdr,
		paliashdr->num_xyz, &reused_shadow_vertices);
	if (!reused_shadow_vertices)
	{
		for (i = 0; i < paliashdr->num_xyz; i++)
		{
			float source_height = lerped_vertices[i][2] + lheight;

			projected_vertices[i][0] = lerped_vertices[i][0] -
				shadevector[0] * source_height;
			projected_vertices[i][1] = lerped_vertices[i][1] -
				shadevector[1] * source_height;
			projected_vertices[i][2] = height;
		}
	}
	submitted_vertices = RSXGL_DrawPreparedAlias(projected_vertices[0],
		paliashdr->num_xyz, 4, NULL, 0,
		currentmodel->ps3_alias_draws, currentmodel->ps3_alias_num_draws,
		currentmodel->ps3_alias_refs, currentmodel->ps3_alias_num_refs,
		currentmodel->ps3_alias_indices, currentmodel->ps3_alias_num_indices);
	#else
#if defined(__PSL1GHT__)
	GLfloat *vtx = ps3_alias_vtx;
#elif defined(_MSC_VER)
	int maxCount = 0;
	const int* tmpOrder = order;
	while (1)
	{
		int c = *tmpOrder++;
		if (!c)
			break;
		if (c < 0)
			c = -c;
		if (c > maxCount)
			maxCount = c;

		tmpOrder += 3 * c;
	}

	YQ2_VLA(GLfloat, vtx, 3 * maxCount);
#endif

	while (1)
	{
		/* get the vertex count and primitive type */
		count = *order++;

		if (!count)
		{
			break; /* done */
		}

		if (count < 0)
		{
			count = -count;

            type = GL_TRIANGLE_FAN;
		}
		else
		{
            type = GL_TRIANGLE_STRIP;
		}

        total = count;

#if !defined(_MSC_VER) && !defined(__PSL1GHT__) // desktop compilers use a scoped VLA
        YQ2_VLA(GLfloat, vtx, 3*total);
#endif
		unsigned int index_vtx = 0;
		do
		{
			/* normals and vertexes come from the frame list */
			memcpy(point, lerped_vertices[order[2]], sizeof(point));

			point[0] -= shadevector[0] * (point[2] + lheight);
			point[1] -= shadevector[1] * (point[2] + lheight);
			point[2] = height;

            vtx[index_vtx++] = point [ 0 ];
            vtx[index_vtx++] = point [ 1 ];
            vtx[index_vtx++] = point [ 2 ];

			order += 3;
		}
		while (--count);

		R_DrawAliasShadowPrimitive(type, total, vtx);
		submitted_vertices += total;
	}
	glDisableClientState(GL_VERTEX_ARRAY);
	#ifndef __PSL1GHT__
	YQ2_VLAFREE(vtx);
	#endif
	#endif

	/* stencilbuffer shadows */
	if (gl_state.stencil && gl1_stencilshadow->value)
	{
		glDisable(GL_STENCIL_TEST);
	}

	(void)posenum;
	return submitted_vertices;
}

static qboolean
R_CullAliasModel(const model_t *currentmodel, vec3_t bbox[8], entity_t *e)
{
	int i;
	vec3_t mins, maxs;
	dmdl_t *paliashdr;
	vec3_t vectors[3];
	vec3_t thismins, oldmins, thismaxs, oldmaxs;
	daliasframe_t *pframe, *poldframe;
	vec3_t angles;
	vec3_t center, extents, worldcenter;
	vec3_t axes[3];

	paliashdr = (dmdl_t *)currentmodel->extradata;

	if ((e->frame >= paliashdr->num_frames) || (e->frame < 0))
	{
		R_Printf(PRINT_DEVELOPER, "R_CullAliasModel %s: no such frame %d\n",
				currentmodel->name, e->frame);
		e->frame = 0;
	}

	if ((e->oldframe >= paliashdr->num_frames) || (e->oldframe < 0))
	{
		R_Printf(PRINT_DEVELOPER, "R_CullAliasModel %s: no such oldframe %d\n",
				currentmodel->name, e->oldframe);
		e->oldframe = 0;
	}

	pframe = (daliasframe_t *)((byte *)paliashdr + paliashdr->ofs_frames +
			e->frame * paliashdr->framesize);

	poldframe = (daliasframe_t *)((byte *)paliashdr + paliashdr->ofs_frames +
			e->oldframe * paliashdr->framesize);

	/* compute axially aligned mins and maxs */
	if (pframe == poldframe)
	{
		for (i = 0; i < 3; i++)
		{
			mins[i] = pframe->translate[i];
			maxs[i] = mins[i] + pframe->scale[i] * 255;
		}
	}
	else
	{
		for (i = 0; i < 3; i++)
		{
			thismins[i] = pframe->translate[i];
			thismaxs[i] = thismins[i] + pframe->scale[i] * 255;

			oldmins[i] = poldframe->translate[i];
			oldmaxs[i] = oldmins[i] + poldframe->scale[i] * 255;

			if (thismins[i] < oldmins[i])
			{
				mins[i] = thismins[i];
			}
			else
			{
				mins[i] = oldmins[i];
			}

			if (thismaxs[i] > oldmaxs[i])
			{
				maxs[i] = thismaxs[i];
			}
			else
			{
				maxs[i] = oldmaxs[i];
			}
		}
	}

	for (i = 0; i < 3; i++)
	{
		center[i] = (mins[i] + maxs[i]) * 0.5f;
		extents[i] = (maxs[i] - mins[i]) * 0.5f;
	}

	/* Convert Quake's local-to-world rows into three world-space local axes.
	 * Zero-angle models are common and need no trigonometric basis at all. */
	if (e->angles[0] == 0.0f && e->angles[1] == 0.0f &&
		e->angles[2] == 0.0f)
	{
		VectorClear(axes[0]);
		VectorClear(axes[1]);
		VectorClear(axes[2]);
		axes[0][0] = 1.0f;
		axes[1][1] = 1.0f;
		axes[2][2] = 1.0f;
	}
	else
	{
		VectorCopy(e->angles, angles);
		angles[YAW] = -angles[YAW];
		AngleVectors(angles, vectors[0], vectors[1], vectors[2]);
		for (i = 0; i < 3; i++)
		{
			axes[i][0] = vectors[0][i];
			axes[i][1] = -vectors[1][i];
			axes[i][2] = vectors[2][i];
		}
	}

	for (i = 0; i < 3; i++)
	{
		worldcenter[i] = e->origin[i] + axes[0][i] * center[0] +
			axes[1][i] * center[1] + axes[2][i] * center[2];
	}

	/* An oriented box lies wholly behind a plane when its center distance plus
	 * the projection radius of its three half-axes is negative. This is the
	 * same result as testing all eight corners, with four center dots and twelve
	 * axis dots instead of thirty-two corner dots after corner construction. */
	#ifdef PS3_NATIVE_RSX
	if (!ps3_traced_alias_obb_cull)
	{
		PS3_RUNTIME_TRACE("GL1 renderer: oriented alias bounds culling active");
		ps3_traced_alias_obb_cull = true;
	}
	#endif
	for (i = 0; i < 4; i++)
	{
		float radius = extents[0] *
			fabsf(DotProduct(frustum[i].normal, axes[0])) +
			extents[1] * fabsf(DotProduct(frustum[i].normal, axes[1])) +
			extents[2] * fabsf(DotProduct(frustum[i].normal, axes[2]));
		float distance = DotProduct(frustum[i].normal, worldcenter) -
			frustum[i].dist;

		/* Remain slightly conservative at a floating-point plane boundary: an
		 * extra edge model is harmless, while a false rejection is visible. */
		if (distance + radius < -0.01f)
		{
			return true;
		}
	}

	/* Only the developer visualization consumes the corners. Normal rendering
	 * avoids constructing and transforming all eight of them. */
	if (gl_showbbox->value)
	{
		int corner;
		for (corner = 0; corner < 8; corner++)
		{
			vec3_t local;
			int component;

			local[0] = (corner & 1) ? mins[0] : maxs[0];
			local[1] = (corner & 2) ? mins[1] : maxs[1];
			local[2] = (corner & 4) ? mins[2] : maxs[2];
			for (component = 0; component < 3; component++)
			{
				bbox[corner][component] = e->origin[component] +
					axes[0][component] * local[0] +
					axes[1][component] * local[1] +
					axes[2][component] * local[2];
			}
		}
	}

	return false;
}

void
R_DrawAliasModel(entity_t *currententity, const model_t *currentmodel)
{
	int i;
	int shadedot_index;
	dmdl_t *paliashdr;
	float an;
	vec3_t bbox[8];
	image_t *skin;
	vec4_t *lerped_vertices;
	#ifdef PS3_NATIVE_RSX
	qboolean ps3_sampled_lightpoint = false;
	#endif

	if (!(currententity->flags & RF_WEAPONMODEL))
	{
		if (R_CullAliasModel(currentmodel, bbox, currententity))
		{
			return;
		}
	}

	if (currententity->flags & RF_WEAPONMODEL)
	{
		if (gl_lefthand->value == 2)
		{
			return;
		}
	}

	paliashdr = (dmdl_t *)currentmodel->extradata;

	#ifdef PS3_NATIVE_RSX
	if (!R_PS3_RestoreAliasLighting(currententity, paliashdr))
	{
	#endif
	/* get lighting information */
	if (currententity->flags &
		(RF_SHELL_HALF_DAM | RF_SHELL_GREEN | RF_SHELL_RED |
		 RF_SHELL_BLUE | RF_SHELL_DOUBLE))
	{
		VectorClear(shadelight);

		if (currententity->flags & RF_SHELL_HALF_DAM)
		{
			shadelight[0] = 0.56;
			shadelight[1] = 0.59;
			shadelight[2] = 0.45;
		}

		if (currententity->flags & RF_SHELL_DOUBLE)
		{
			shadelight[0] = 0.9;
			shadelight[1] = 0.7;
		}

		if (currententity->flags & RF_SHELL_RED)
		{
			shadelight[0] = 1.0;
		}

		if (currententity->flags & RF_SHELL_GREEN)
		{
			shadelight[1] = 1.0;
		}

		if (currententity->flags & RF_SHELL_BLUE)
		{
			shadelight[2] = 1.0;
		}
	}
	else if (currententity->flags & RF_FULLBRIGHT)
	{
		for (i = 0; i < 3; i++)
		{
			shadelight[i] = 1.0;
		}
	}
	else
	{
		R_LightPoint(currententity, currententity->origin, shadelight);
		#ifdef PS3_NATIVE_RSX
		/* R_LightPoint intentionally leaves lightspot untouched when a map has
		 * no light data. Do not cache that order-dependent fallback state. */
		ps3_sampled_lightpoint =
			r_worldmodel && r_worldmodel->lightdata;
		#endif

		/* player lighting hack for communication back to server */
		if (currententity->flags & RF_WEAPONMODEL)
		{
			/* pick the greatest component, which should be
			   the same as the mono value returned by software */
			if (shadelight[0] > shadelight[1])
			{
				if (shadelight[0] > shadelight[2])
				{
					r_lightlevel->value = 150 * shadelight[0];
				}
				else
				{
					r_lightlevel->value = 150 * shadelight[2];
				}
			}
			else
			{
				if (shadelight[1] > shadelight[2])
				{
					r_lightlevel->value = 150 * shadelight[1];
				}
				else
				{
					r_lightlevel->value = 150 * shadelight[2];
				}
			}
		}
	}

	if (currententity->flags & RF_MINLIGHT)
	{
		for (i = 0; i < 3; i++)
		{
			if (shadelight[i] > 0.1)
			{
				break;
			}
		}

		if (i == 3)
		{
			shadelight[0] = 0.1;
			shadelight[1] = 0.1;
			shadelight[2] = 0.1;
		}
	}

	if (currententity->flags & RF_GLOW)
	{
		/* bonus items will pulse with time */
		float scale;
		float min;

		scale = 0.1 * sin(r_newrefdef.time * 7);

		for (i = 0; i < 3; i++)
		{
			min = shadelight[i] * 0.8;
			shadelight[i] += scale;

			if (shadelight[i] < min)
			{
				shadelight[i] = min;
			}
		}
	}


    // Apply gl1_overbrightbits to the mesh. If we don't do this they will appear slightly dimmer relative to walls.
    if (gl1_overbrightbits->value)
    {
        for (i = 0; i < 3; ++i)
        {
            shadelight[i] *= gl1_overbrightbits->value;
        }
    }



	/* ir goggles color override */
	if (r_newrefdef.rdflags & RDF_IRGOGGLES && currententity->flags &
		RF_IR_VISIBLE)
	{
		shadelight[0] = 1.0;
		shadelight[1] = 0.0;
		shadelight[2] = 0.0;
	}

	shadedot_index = ((int)(currententity->angles[1] *
		(SHADEDOT_QUANT / 360.0))) & (SHADEDOT_QUANT - 1);
	shadedots = r_avertexnormal_dots[shadedot_index];

	an = currententity->angles[1] / 180 * M_PI;
	shadevector[0] = cos(-an);
	shadevector[1] = sin(-an);
	shadevector[2] = 1;
	VectorNormalize(shadevector);
	#ifdef PS3_NATIVE_RSX
	if (ps3_sampled_lightpoint)
	{
		R_PS3_StoreAliasLighting(currententity, paliashdr,
			shadedot_index);
	}
	}
	#endif

	/* locate the proper data */
	c_alias_polys += paliashdr->num_tris;

	/* draw all the triangles */
	if (currententity->flags & RF_DEPTHHACK)
	{
		/* hack the depth range to prevent view model from poking into walls */
		glDepthRange(gldepthmin, gldepthmin + 0.3 * (gldepthmax - gldepthmin));
	}

	if (currententity->flags & RF_WEAPONMODEL)
	{
		extern void R_MYgluPerspective(GLdouble fovy, GLdouble aspect, GLdouble zNear, GLdouble zFar);

		glMatrixMode(GL_PROJECTION);
		glPushMatrix();
		glLoadIdentity();

		if (gl_lefthand->value == 1.0F)
		{
			glScalef(-1, 1, 1);
		}

		float dist = (r_farsee->value == 0) ? 4096.0f : 8192.0f;

		if (r_gunfov->value < 0)
		{
			R_MYgluPerspective(r_newrefdef.fov_y, (float)r_newrefdef.width / r_newrefdef.height, 4, dist);
		}
		else
		{
			R_MYgluPerspective(r_gunfov->value, (float)r_newrefdef.width / r_newrefdef.height, 4, dist);
		}

		glMatrixMode(GL_MODELVIEW);

		if (gl_lefthand->value == 1.0F)
		{
			glCullFace(GL_BACK);
		}
	}

	glPushMatrix();
	currententity->angles[PITCH] = -currententity->angles[PITCH];
	R_RotateForEntity(currententity);
	currententity->angles[PITCH] = -currententity->angles[PITCH];

	/* select skin */
	if (currententity->skin)
	{
		skin = currententity->skin; /* custom player skin */
	}
	else
	{
		if (currententity->skinnum >= MAX_MD2SKINS)
		{
			skin = currentmodel->skins[0];
		}
		else
		{
			skin = currentmodel->skins[currententity->skinnum];

			if (!skin)
			{
				skin = currentmodel->skins[0];
			}
		}
	}

	if (!skin)
	{
		skin = r_notexture; /* fallback... */
	}

	R_Bind(skin->texnum);

	/* draw it */
	glShadeModel(GL_SMOOTH);

	R_TexEnv(GL_MODULATE);

	if (currententity->flags & RF_TRANSLUCENT)
	{
		glEnable(GL_BLEND);
	}

	if ((currententity->frame >= paliashdr->num_frames) ||
		(currententity->frame < 0))
	{
		R_Printf(PRINT_DEVELOPER, "R_DrawAliasModel %s: no such frame %d\n",
				currentmodel->name, currententity->frame);
		currententity->frame = 0;
		currententity->oldframe = 0;
	}

	if ((currententity->oldframe >= paliashdr->num_frames) ||
		(currententity->oldframe < 0))
	{
		R_Printf(PRINT_DEVELOPER, "R_DrawAliasModel %s: no such oldframe %d\n",
				currentmodel->name, currententity->oldframe);
		currententity->frame = 0;
		currententity->oldframe = 0;
	}

	if (!r_lerpmodels->value)
	{
		currententity->backlerp = 0;
	}

	lerped_vertices = R_DrawAliasFrameLerp(currententity, currentmodel,
		paliashdr,
		currententity->backlerp);

	R_TexEnv(GL_REPLACE);
	glShadeModel(GL_FLAT);

	glPopMatrix();

	if (gl_showbbox->value)
	{
		glDisable(GL_CULL_FACE);
		glPolygonMode(GL_FRONT_AND_BACK, GL_LINE);
		glDisable(GL_TEXTURE_2D);
		glBegin(GL_TRIANGLE_STRIP);

		for (i = 0; i < 8; i++)
		{
			glVertex3fv(bbox[i]);
		}

		glEnd();
		glEnable(GL_TEXTURE_2D);
		glPolygonMode(GL_FRONT_AND_BACK, GL_FILL);
		glEnable(GL_CULL_FACE);
	}

	if (currententity->flags & RF_WEAPONMODEL)
	{
		glMatrixMode(GL_PROJECTION);
		glPopMatrix();
		glMatrixMode(GL_MODELVIEW);
		if (gl_lefthand->value == 1.0F)
			glCullFace(GL_FRONT);
	}

	if (currententity->flags & RF_TRANSLUCENT)
	{
		glDisable(GL_BLEND);
	}

	if (currententity->flags & RF_DEPTHHACK)
	{
		glDepthRange(gldepthmin, gldepthmax);
	}

	if (gl_shadows->value &&
		!(currententity->flags & (RF_TRANSLUCENT | RF_WEAPONMODEL | RF_NOSHADOW)))
	{
		int shadow_vertices;
		glPushMatrix();

		/* don't rotate shadows on ungodly axes */
		glTranslatef(currententity->origin[0], currententity->origin[1], currententity->origin[2]);
		glRotatef(currententity->angles[1], 0, 0, 1);

		/* Projected shadows are coplanar, untextured translucent geometry. State
		 * every dependency explicitly so a native fixed-state cache cannot inherit
		 * model or alpha-test state and keep the pass from changing the world Z
		 * buffer. R_DrawAliasShadow already lifts the projection 0.1 world units;
		 * adding native RSX polygon offset produced long black streaks on nearby
		 * receivers instead of improving that separation. */
		glDisable(GL_TEXTURE_2D);
		glDisable(GL_ALPHA_TEST);
		glDisable(GL_CULL_FACE);
		glEnable(GL_BLEND);
		glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
		glDepthMask(GL_FALSE);
		glColor4f(0, 0, 0, 0.5f);
		shadow_vertices = R_DrawAliasShadow(currententity, currentmodel,
			paliashdr,
			currententity->frame, lerped_vertices);
		glDepthMask(GL_TRUE);
		glEnable(GL_TEXTURE_2D);
		glDisable(GL_BLEND);
		if (gl_cull->value)
		{
			glEnable(GL_CULL_FACE);
		}
		#ifdef PS3_NATIVE_RSX
		if (!ps3_traced_projected_shadow && shadow_vertices > 0)
		{
			char trace_line[96];
			Com_sprintf(trace_line, sizeof(trace_line),
				"GL1 renderer: projected shadow submitted (%d vertices)",
				shadow_vertices);
			PS3_RUNTIME_TRACE(trace_line);
			ps3_traced_projected_shadow = true;
		}
		#endif
		glPopMatrix();
	}

	glColor4f(1, 1, 1, 1);
}
