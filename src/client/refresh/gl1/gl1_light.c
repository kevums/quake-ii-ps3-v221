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
 * Lightmaps and dynamic lighting
 *
 * =======================================================================
 */

#include "header/local.h"

#ifdef PS3_NATIVE_RSX
#include <stdint.h>
#endif

#define DLIGHT_CUTOFF 64

int r_dlightframecount;
vec3_t pointcolor;
cplane_t *lightplane; /* used as shadow plane */
vec3_t lightspot;
static float s_blocklights[34 * 34 * 3];

#ifdef PS3_NATIVE_RSX
typedef struct
{
	int registration_sequence;
	vec3_t origin;
	msurface_t *surface;
	byte *sample;
	vec3_t spot;
	cplane_t *plane;
	qboolean final_color_valid;
	vec3_t final_origin;
	vec3_t final_color;
	float final_modulate;
	unsigned int final_dlight_revision;
	int final_style_count;
	float final_style_rgb[MAXLIGHTMAPS][3];
} ps3_lightpoint_cache_t;

/* The renderer's entity array has stable slots within a frame sequence. Cache
 * only the BSP hit for an unchanged origin; lightstyles, r_modulate, and
 * dynamic lights are deliberately evaluated again on every use. */
static ps3_lightpoint_cache_t ps3_lightpoint_cache[MAX_ENTITIES];
static qboolean ps3_traced_lightpoint_cache;
static qboolean ps3_traced_final_light_cache;
static msurface_t *ps3_lightpoint_surface;
static byte *ps3_lightpoint_sample;
static dlight_t ps3_last_dlights[MAX_DLIGHTS];
static int ps3_last_dlight_count = -1;
static int ps3_last_dlight_frame = -1;
static int ps3_last_dlight_registration = -1;
static unsigned int ps3_dlight_revision = 1;

/* Compare the complete dynamic-light input once per renderer pass, not once
 * per entity. dlight_t contains only seven floats, so the byte comparison is
 * exact and padding-free; any bit-level input change advances the revision.
 * The cached final entity light can then reject changed dynamic lighting with
 * one integer comparison while avoiding up to 32 vector lengths per model. */
static unsigned int
R_PS3_DlightRevision(void)
{
	int count = r_newrefdef.num_dlights;
	int registration = r_worldmodel ?
		r_worldmodel->registration_sequence : 0;
	qboolean changed;

	if (count < 0) count = 0;
	if (count > MAX_DLIGHTS) count = MAX_DLIGHTS;
	if (ps3_last_dlight_frame == r_framecount &&
		ps3_last_dlight_registration == registration)
	{
		return ps3_dlight_revision;
	}

	changed = count != ps3_last_dlight_count;
	if (!changed && count > 0)
	{
		changed = memcmp(ps3_last_dlights, r_newrefdef.dlights,
			(size_t)count * sizeof(dlight_t)) != 0;
	}
	if (changed)
	{
		if (count > 0)
		{
			memcpy(ps3_last_dlights, r_newrefdef.dlights,
				(size_t)count * sizeof(dlight_t));
		}
		ps3_last_dlight_count = count;
		ps3_dlight_revision++;
		if (!ps3_dlight_revision)
		{
			ps3_dlight_revision = 1;
		}
	}
	ps3_last_dlight_frame = r_framecount;
	ps3_last_dlight_registration = registration;
	return ps3_dlight_revision;
}

static qboolean
R_PS3_FinalLightIsCurrent(const ps3_lightpoint_cache_t *entry,
	const entity_t *currententity, unsigned int dlight_revision)
{
	int maps;

	if (!entry || !entry->final_color_valid || !entry->surface ||
		entry->final_modulate != r_modulate->value ||
		entry->final_dlight_revision != dlight_revision ||
		entry->final_origin[0] != currententity->origin[0] ||
		entry->final_origin[1] != currententity->origin[1] ||
		entry->final_origin[2] != currententity->origin[2])
	{
		return false;
	}

	for (maps = 0; maps < MAXLIGHTMAPS &&
		entry->surface->styles[maps] != 255; maps++)
	{
		const float *rgb = r_newrefdef.lightstyles[
			entry->surface->styles[maps]].rgb;
		if (maps >= entry->final_style_count ||
			entry->final_style_rgb[maps][0] != rgb[0] ||
			entry->final_style_rgb[maps][1] != rgb[1] ||
			entry->final_style_rgb[maps][2] != rgb[2])
		{
			return false;
		}
	}
	return maps == entry->final_style_count;
}

static void
R_PS3_StoreFinalLight(ps3_lightpoint_cache_t *entry,
	const entity_t *currententity, const vec3_t color,
	unsigned int dlight_revision)
{
	int maps;

	if (!entry || !entry->surface || !entry->sample)
	{
		return;
	}
	VectorCopy(currententity->origin, entry->final_origin);
	VectorCopy(color, entry->final_color);
	entry->final_modulate = r_modulate->value;
	entry->final_dlight_revision = dlight_revision;
	for (maps = 0; maps < MAXLIGHTMAPS &&
		entry->surface->styles[maps] != 255; maps++)
	{
		const float *rgb = r_newrefdef.lightstyles[
			entry->surface->styles[maps]].rgb;
		entry->final_style_rgb[maps][0] = rgb[0];
		entry->final_style_rgb[maps][1] = rgb[1];
		entry->final_style_rgb[maps][2] = rgb[2];
	}
	entry->final_style_count = maps;
	entry->final_color_valid = true;
}

static ps3_lightpoint_cache_t *
R_PS3_LightPointCacheEntry(entity_t *currententity, const vec3_t origin)
{
	uintptr_t address;
	uintptr_t base;
	uintptr_t span;
	uintptr_t offset;
	ps3_lightpoint_cache_t *entry;

	if (!currententity || !r_newrefdef.entities ||
		r_newrefdef.num_entities <= 0 || !r_worldmodel)
	{
		return NULL;
	}
	address = (uintptr_t)currententity;
	base = (uintptr_t)r_newrefdef.entities;
	span = (uintptr_t)r_newrefdef.num_entities * sizeof(entity_t);
	if (address < base || address >= base + span)
	{
		return NULL;
	}
	offset = address - base;
	if (offset % sizeof(entity_t) != 0 ||
		offset / sizeof(entity_t) >= MAX_ENTITIES)
	{
		return NULL;
	}
	entry = &ps3_lightpoint_cache[offset / sizeof(entity_t)];
	if (entry->registration_sequence !=
		r_worldmodel->registration_sequence ||
		entry->origin[0] != origin[0] ||
		entry->origin[1] != origin[1] ||
		entry->origin[2] != origin[2])
	{
		entry->surface = NULL;
		entry->sample = NULL;
		entry->final_color_valid = false;
	}
	return entry;
}
#endif

static void
R_AccumulateLightPoint(msurface_t *surf, byte *lightmap)
{
	int maps;

	VectorCopy(vec3_origin, pointcolor);
	for (maps = 0; maps < MAXLIGHTMAPS && surf->styles[maps] != 255;
		 maps++)
	{
		const float *rgb =
			r_newrefdef.lightstyles[surf->styles[maps]].rgb;
		int j;

		for (j = 0; j < 3; j++)
		{
			float scale = rgb[j] * r_modulate->value;
			pointcolor[j] += lightmap[j] * scale * (1.0 / 255);
		}
		lightmap += 3 * ((surf->extents[0] >> 4) + 1) *
			((surf->extents[1] >> 4) + 1);
	}
}

void
R_RenderDlight(dlight_t *light)
{
	int i, j;
	float a;
	float rad;

	rad = light->intensity * 0.35;

	GLfloat vtx[3*18];
	GLfloat clr[4*18];

	unsigned int index_vtx = 3;
	unsigned int index_clr = 0;

	glEnableClientState( GL_VERTEX_ARRAY );
	glEnableClientState( GL_COLOR_ARRAY );

	clr[index_clr++] = light->color [ 0 ] * 0.2;
	clr[index_clr++] = light->color [ 1 ] * 0.2;
	clr[index_clr++] = light->color [ 2 ] * 0.2;
	clr[index_clr++] = 1;

	for ( i = 0; i < 3; i++ )
	{
		vtx [ i ] = light->origin [ i ] - vpn [ i ] * rad;
	}

	for ( i = 16; i >= 0; i-- )
	{
		clr[index_clr++] = 0;
		clr[index_clr++] = 0;
		clr[index_clr++] = 0;
		clr[index_clr++] = 1;

		a = i / 16.0 * M_PI * 2;

		for ( j = 0; j < 3; j++ )
		{
			vtx[index_vtx++] = light->origin [ j ] + vright [ j ] * cos( a ) * rad
				+ vup [ j ] * sin( a ) * rad;
		}
	}

	glVertexPointer( 3, GL_FLOAT, 0, vtx );
	glColorPointer( 4, GL_FLOAT, 0, clr );
	glDrawArrays( GL_TRIANGLE_FAN, 0, 18 );

	glDisableClientState( GL_VERTEX_ARRAY );
	glDisableClientState( GL_COLOR_ARRAY );
}

void
R_RenderDlights(void)
{
	int i;
	dlight_t *l;

	if (!gl1_flashblend->value)
	{
		return;
	}

	glDepthMask(0);
	glDisable(GL_TEXTURE_2D);
	glShadeModel(GL_SMOOTH);
	glEnable(GL_BLEND);
	glBlendFunc(GL_ONE, GL_ONE);

	l = r_newrefdef.dlights;

	for (i = 0; i < r_newrefdef.num_dlights; i++, l++)
	{
		R_RenderDlight(l);
	}

	glColor4f(1, 1, 1, 1);
	glDisable(GL_BLEND);
	glEnable(GL_TEXTURE_2D);
	glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
	glDepthMask(1);
}

void
R_MarkLights(dlight_t *light, int bit, mnode_t *node)
{
	cplane_t *splitplane;
	float dist;
	msurface_t *surf;
	int i;
	int sidebit;

	if (node->contents != -1)
	{
		return;
	}

	splitplane = node->plane;
	dist = DotProduct(light->origin, splitplane->normal) - splitplane->dist;

	if (dist > light->intensity - DLIGHT_CUTOFF)
	{
		R_MarkLights(light, bit, node->children[0]);
		return;
	}

	if (dist < -light->intensity + DLIGHT_CUTOFF)
	{
		R_MarkLights(light, bit, node->children[1]);
		return;
	}

	/* mark the polygons */
	surf = r_worldmodel->surfaces + node->firstsurface;

	for (i = 0; i < node->numsurfaces; i++, surf++)
	{
		dist = DotProduct(light->origin, surf->plane->normal) - surf->plane->dist;

		if (dist >= 0)
		{
			sidebit = 0;
		}
		else
		{
			sidebit = SURF_PLANEBACK;
		}

		if ((surf->flags & SURF_PLANEBACK) != sidebit)
		{
			continue;
		}

		if (surf->dlightframe != r_dlightframecount)
		{
			surf->dlightbits = 0;
			surf->dlightframe = r_dlightframecount;
		}

		surf->dlightbits |= bit;
	}

	R_MarkLights(light, bit, node->children[0]);
	R_MarkLights(light, bit, node->children[1]);
}

void
R_PushDlights(void)
{
	int i;
	dlight_t *l;

	/* R_SetupFrame() has not advanced the visibility generation yet. Keep a
	 * separate light generation so a native stereo pair can share the marks
	 * while each eye still receives its own surface-visibility generation. */
	r_dlightframecount = r_framecount + 1;

	if (gl1_flashblend->value)
	{
		return;
	}

	l = r_newrefdef.dlights;

	for (i = 0; i < r_newrefdef.num_dlights; i++, l++)
	{
		R_MarkLights(l, 1 << i, r_worldmodel->nodes);
	}
}

int
R_RecursiveLightPoint(mnode_t *node, vec3_t start, vec3_t end)
{
	float front, back, frac;
	int side;
	cplane_t *plane;
	vec3_t mid;
	msurface_t *surf;
	int s, t, ds, dt;
	int i;
	mtexinfo_t *tex;
	byte *lightmap;
	int r;

	if (node->contents != -1)
	{
		return -1;     /* didn't hit anything */
	}

	/* calculate mid point */
	plane = node->plane;
	front = DotProduct(start, plane->normal) - plane->dist;
	back = DotProduct(end, plane->normal) - plane->dist;
	side = front < 0;

	if ((back < 0) == side)
	{
		return R_RecursiveLightPoint(node->children[side], start, end);
	}

	frac = front / (front - back);
	mid[0] = start[0] + (end[0] - start[0]) * frac;
	mid[1] = start[1] + (end[1] - start[1]) * frac;
	mid[2] = start[2] + (end[2] - start[2]) * frac;

	/* go down front side */
	r = R_RecursiveLightPoint(node->children[side], start, mid);

	if (r >= 0)
	{
		return r;     /* hit something */
	}

	if ((back < 0) == side)
	{
		return -1;     /* didn't hit anuthing */
	}

	/* check for impact on this node */
	VectorCopy(mid, lightspot);
	lightplane = plane;

	surf = r_worldmodel->surfaces + node->firstsurface;

	for (i = 0; i < node->numsurfaces; i++, surf++)
	{
		if (surf->flags & (SURF_DRAWTURB | SURF_DRAWSKY))
		{
			continue; /* no lightmaps */
		}

		tex = surf->texinfo;

		s = DotProduct(mid, tex->vecs[0]) + tex->vecs[0][3];
		t = DotProduct(mid, tex->vecs[1]) + tex->vecs[1][3];

		if ((s < surf->texturemins[0]) ||
			(t < surf->texturemins[1]))
		{
			continue;
		}

		ds = s - surf->texturemins[0];
		dt = t - surf->texturemins[1];

		if ((ds > surf->extents[0]) || (dt > surf->extents[1]))
		{
			continue;
		}

		if (!surf->samples)
		{
			return 0;
		}

		ds >>= 4;
		dt >>= 4;

		lightmap = surf->samples +
			3 * (dt * ((surf->extents[0] >> 4) + 1) + ds);
		#ifdef PS3_NATIVE_RSX
		ps3_lightpoint_surface = surf;
		ps3_lightpoint_sample = lightmap;
		#endif
		R_AccumulateLightPoint(surf, lightmap);

		return 1;
	}

	/* go down back side */
	return R_RecursiveLightPoint(node->children[!side], mid, end);
}

void
R_LightPoint(entity_t *currententity, vec3_t p, vec3_t color)
{
	vec3_t end;
	float r;
	int lnum;
	dlight_t *dl;
	vec3_t dist;
	float add;
	#ifdef PS3_NATIVE_RSX
	ps3_lightpoint_cache_t *cache_entry = NULL;
	qboolean cached_hit = false;
	unsigned int dlight_revision;
	#endif

	if (!r_worldmodel->lightdata || !currententity)
	{
		color[0] = color[1] = color[2] = 1.0;
		return;
	}

	end[0] = p[0];
	end[1] = p[1];
	end[2] = p[2] - 2048;

	#ifdef PS3_NATIVE_RSX
	ps3_lightpoint_surface = NULL;
	ps3_lightpoint_sample = NULL;
	cache_entry = R_PS3_LightPointCacheEntry(currententity, p);
	dlight_revision = R_PS3_DlightRevision();
	if (cache_entry && cache_entry->surface && cache_entry->sample &&
		R_PS3_FinalLightIsCurrent(cache_entry, currententity,
			dlight_revision))
	{
		VectorCopy(cache_entry->spot, lightspot);
		lightplane = cache_entry->plane;
		VectorCopy(cache_entry->final_color, color);
		RSXGL_RecordFinalLightCacheHit();
		if (!ps3_traced_final_light_cache)
		{
			PS3_RUNTIME_TRACE(
				"GL1 renderer: stationary final entity-light cache active");
			ps3_traced_final_light_cache = true;
		}
		return;
	}
	if (cache_entry && cache_entry->surface && cache_entry->sample)
	{
		R_AccumulateLightPoint(cache_entry->surface, cache_entry->sample);
		VectorCopy(cache_entry->spot, lightspot);
		lightplane = cache_entry->plane;
		r = 1;
		cached_hit = true;
		if (!ps3_traced_lightpoint_cache)
		{
			PS3_RUNTIME_TRACE(
				"GL1 renderer: stationary BSP light-point hit cache active");
			ps3_traced_lightpoint_cache = true;
		}
	}
	else
	#endif
	{
		r = R_RecursiveLightPoint(r_worldmodel->nodes, p, end);
	}

	#ifdef PS3_NATIVE_RSX
	if (!cached_hit && r == 1 && cache_entry && ps3_lightpoint_surface &&
		ps3_lightpoint_sample)
	{
		cache_entry->registration_sequence =
			r_worldmodel->registration_sequence;
		VectorCopy(p, cache_entry->origin);
		cache_entry->surface = ps3_lightpoint_surface;
		cache_entry->sample = ps3_lightpoint_sample;
		VectorCopy(lightspot, cache_entry->spot);
		cache_entry->plane = lightplane;
	}
	#endif

	if (r == -1)
	{
		VectorCopy(vec3_origin, color);
	}
	else
	{
		VectorCopy(pointcolor, color);
	}

	/* add dynamic lights */
	dl = r_newrefdef.dlights;

	for (lnum = 0; lnum < r_newrefdef.num_dlights; lnum++, dl++)
	{
		VectorSubtract(currententity->origin,
				dl->origin, dist);
		add = dl->intensity - VectorLength(dist);
		add *= (1.0 / 256);

		if (add > 0)
		{
			VectorMA(color, add, dl->color, color);
		}
	}

	VectorScale(color, r_modulate->value, color);
	#ifdef PS3_NATIVE_RSX
	if (r == 1 && cache_entry)
	{
		R_PS3_StoreFinalLight(cache_entry, currententity, color,
			dlight_revision);
	}
	#endif
}

void
R_AddDynamicLights(msurface_t *surf)
{
	int lnum;
	int sd, td;
	float fdist, frad, fminlight;
	vec3_t impact, local;
	int s, t;
	int i;
	int smax, tmax;
	mtexinfo_t *tex;
	dlight_t *dl;
	float *pfBL;
	float fsacc, ftacc;

	smax = (surf->extents[0] >> 4) + 1;
	tmax = (surf->extents[1] >> 4) + 1;
	tex = surf->texinfo;

	for (lnum = 0; lnum < r_newrefdef.num_dlights; lnum++)
	{
		if (!(surf->dlightbits & (1 << lnum)))
		{
			continue; /* not lit by this light */
		}

		dl = &r_newrefdef.dlights[lnum];
		frad = dl->intensity;
		fdist = DotProduct(dl->origin, surf->plane->normal) -
				surf->plane->dist;
		frad -= fabs(fdist);

		/* rad is now the highest intensity on the plane */
		fminlight = DLIGHT_CUTOFF;

		if (frad < fminlight)
		{
			continue;
		}

		fminlight = frad - fminlight;

		for (i = 0; i < 3; i++)
		{
			impact[i] = dl->origin[i] -
						surf->plane->normal[i] * fdist;
		}

		local[0] = DotProduct(impact,
				   tex->vecs[0]) + tex->vecs[0][3] - surf->texturemins[0];
		local[1] = DotProduct(impact,
				   tex->vecs[1]) + tex->vecs[1][3] - surf->texturemins[1];

		pfBL = s_blocklights;

		for (t = 0, ftacc = 0; t < tmax; t++, ftacc += 16)
		{
			td = local[1] - ftacc;

			if (td < 0)
			{
				td = -td;
			}

			for (s = 0, fsacc = 0; s < smax; s++, fsacc += 16, pfBL += 3)
			{
				sd = Q_ftol(local[0] - fsacc);

				if (sd < 0)
				{
					sd = -sd;
				}

				if (sd > td)
				{
					fdist = sd + (td >> 1);
				}
				else
				{
					fdist = td + (sd >> 1);
				}

				if (fdist < fminlight)
				{
					pfBL[0] += (frad - fdist) * dl->color[0];
					pfBL[1] += (frad - fdist) * dl->color[1];
					pfBL[2] += (frad - fdist) * dl->color[2];
				}
			}
		}
	}
}

void
R_SetCacheState(msurface_t *surf)
{
	int maps;

	for (maps = 0; maps < MAXLIGHTMAPS && surf->styles[maps] != 255;
		 maps++)
	{
		surf->cached_light[maps] =
			r_newrefdef.lightstyles[surf->styles[maps]].white;
	}
}

/*
 * Combine and scale multiple lightmaps into the floating format in blocklights
 */
void
R_BuildLightMap(msurface_t *surf, byte *dest, int stride)
{
	int smax, tmax;
	int r, g, b, a, max;
	int i, j, size;
	byte *lightmap;
	float scale[4];
	int nummaps;
	float *bl;

	if (surf->texinfo->flags &
		(SURF_SKY | SURF_TRANS33 | SURF_TRANS66 | SURF_WARP))
	{
		ri.Sys_Error(ERR_DROP, "R_BuildLightMap called for non-lit surface");
	}

	smax = (surf->extents[0] >> 4) + 1;
	tmax = (surf->extents[1] >> 4) + 1;
	size = smax * tmax;

	if (size > (sizeof(s_blocklights) >> 4))
	{
		ri.Sys_Error(ERR_DROP, "Bad s_blocklights size");
	}

	/* set to full bright if no light data */
	if (!surf->samples)
	{
		for (i = 0; i < size * 3; i++)
		{
			s_blocklights[i] = 255;
		}

		goto store;
	}

	/* count the # of maps */
	for (nummaps = 0; nummaps < MAXLIGHTMAPS && surf->styles[nummaps] != 255;
		 nummaps++)
	{
	}

	lightmap = surf->samples;

	/* add all the lightmaps */
	if (nummaps == 1)
	{
		int maps;

		for (maps = 0; maps < MAXLIGHTMAPS && surf->styles[maps] != 255; maps++)
		{
			bl = s_blocklights;

			for (i = 0; i < 3; i++)
			{
				scale[i] = r_modulate->value *
						   r_newrefdef.lightstyles[surf->styles[maps]].rgb[i];
			}

			if ((scale[0] == 1.0F) &&
				(scale[1] == 1.0F) &&
				(scale[2] == 1.0F))
			{
				for (i = 0; i < size; i++, bl += 3)
				{
					bl[0] = lightmap[i * 3 + 0];
					bl[1] = lightmap[i * 3 + 1];
					bl[2] = lightmap[i * 3 + 2];
				}
			}
			else
			{
				for (i = 0; i < size; i++, bl += 3)
				{
					bl[0] = lightmap[i * 3 + 0] * scale[0];
					bl[1] = lightmap[i * 3 + 1] * scale[1];
					bl[2] = lightmap[i * 3 + 2] * scale[2];
				}
			}

			lightmap += size * 3; /* skip to next lightmap */
		}
	}
	else
	{
		int maps;

		memset(s_blocklights, 0, sizeof(s_blocklights[0]) * size * 3);

		for (maps = 0; maps < MAXLIGHTMAPS && surf->styles[maps] != 255; maps++)
		{
			bl = s_blocklights;

			for (i = 0; i < 3; i++)
			{
				scale[i] = r_modulate->value *
						   r_newrefdef.lightstyles[surf->styles[maps]].rgb[i];
			}

			if ((scale[0] == 1.0F) &&
				(scale[1] == 1.0F) &&
				(scale[2] == 1.0F))
			{
				for (i = 0; i < size; i++, bl += 3)
				{
					bl[0] += lightmap[i * 3 + 0];
					bl[1] += lightmap[i * 3 + 1];
					bl[2] += lightmap[i * 3 + 2];
				}
			}
			else
			{
				for (i = 0; i < size; i++, bl += 3)
				{
					bl[0] += lightmap[i * 3 + 0] * scale[0];
					bl[1] += lightmap[i * 3 + 1] * scale[1];
					bl[2] += lightmap[i * 3 + 2] * scale[2];
				}
			}

			lightmap += size * 3; /* skip to next lightmap */
		}
	}

	/* add all the dynamic lights */
	if (surf->dlightframe == r_dlightframecount)
	{
		R_AddDynamicLights(surf);
	}

store:

	stride -= (smax << 2);
	bl = s_blocklights;

	for (i = 0; i < tmax; i++, dest += stride)
	{
		for (j = 0; j < smax; j++)
		{
			r = Q_ftol(bl[0]);
			g = Q_ftol(bl[1]);
			b = Q_ftol(bl[2]);

			/* catch negative lights */
			if (r < 0)
			{
				r = 0;
			}

			if (g < 0)
			{
				g = 0;
			}

			if (b < 0)
			{
				b = 0;
			}

			/* determine the brightest of the three color components */
			if (r > g)
			{
				max = r;
			}
			else
			{
				max = g;
			}

			if (b > max)
			{
				max = b;
			}

			/* alpha is ONLY used for the mono lightmap case. For this
			   reason we set it to the brightest of the color components
			   so that things don't get too dim. */
			a = max;

			/* rescale all the color components if the
			   intensity of the greatest channel exceeds
			   1.0 */
			if (max > 255)
			{
				float t = 255.0F / max;

				r = r * t;
				g = g * t;
				b = b * t;
				a = a * t;
			}

			dest[0] = r;
			dest[1] = g;
			dest[2] = b;
			dest[3] = a;

			bl += 3;
			dest += 4;
		}
	}
}
