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
 * MD2 file format
 *
 * =======================================================================
 */

#include "header/local.h"

#ifdef PS3_NATIVE_RSX
static size_t
PS3_AliasHunkAlignedSize(size_t size)
{
	return (size + 31u) & ~(size_t)31u;
}

/* Count and validate the immutable MD2 GL command topology before opening the
 * model hunk. This gives Hunk_Begin() an exact upper bound for the native
 * records which are appended after the byte-swapped file image. */
static void
PS3_CountAliasTopology(model_t *mod, const dmdl_t *pinmodel, int modfilelen,
	int ofs_end, int *num_draws, int *num_refs, int *num_indices)
{
	const int *cursor;
	const int *end;
	int glcmd_words = LittleLong(pinmodel->num_glcmds);
	int glcmd_offset = LittleLong(pinmodel->ofs_glcmds);
	int draws = 0;
	int refs = 0;
	int indices = 0;
	qboolean terminated = false;

	if (glcmd_words <= 0 || glcmd_offset < 0 ||
		glcmd_offset > ofs_end || glcmd_offset > modfilelen ||
		(size_t)glcmd_words * sizeof(int) >
			(size_t)(ofs_end - glcmd_offset) ||
		(size_t)glcmd_words * sizeof(int) >
			(size_t)(modfilelen - glcmd_offset))
	{
		ri.Sys_Error(ERR_DROP, "model %s has an invalid GL command range",
			mod->name);
	}

	cursor = (const int *)((const byte *)pinmodel + glcmd_offset);
	end = cursor + glcmd_words;
	while (cursor < end)
	{
		int count = LittleLong(*cursor++);

		if (!count)
		{
			terminated = true;
			break;
		}
		if (count == (-2147483647 - 1))
		{
			ri.Sys_Error(ERR_DROP,
				"model %s has an invalid GL primitive count", mod->name);
		}
		if (count < 0)
		{
			count = -count;
		}
		if (count > MAX_VERTS || count > (end - cursor) / 3)
		{
			ri.Sys_Error(ERR_DROP,
				"model %s has an invalid GL primitive", mod->name);
		}

		if (draws == 0x7fffffff || refs > 0x7fffffff - count ||
			(count >= 3 && indices > 0x7fffffff - (count - 2) * 3))
		{
			ri.Sys_Error(ERR_DROP,
				"model %s has excessive native topology", mod->name);
		}
		draws++;
		refs += count;
		if (count >= 3)
		{
			indices += (count - 2) * 3;
		}
		cursor += 3 * count;
	}

	if (!terminated)
	{
		ri.Sys_Error(ERR_DROP, "model %s has no GL command terminator",
			mod->name);
	}
	*num_draws = draws;
	*num_refs = refs;
	/* One flattened RSX batch uses 16-bit references. Oversized custom data
	 * remains valid and takes the established per-primitive path. */
	*num_indices = refs <= 65535 ? indices : 0;
}

static void
PS3_PrepareAliasTopology(model_t *mod, dmdl_t *pheader)
{
	const int *order = (const int *)((const byte *)pheader +
		pheader->ofs_glcmds);
	ps3_alias_draw_t *draw;
	ps3_alias_ref_t *ref;
	unsigned short *index;
	int draw_index;
	int ref_index = 0;

	if (mod->ps3_alias_num_draws <= 0 || mod->ps3_alias_num_refs <= 0)
	{
		return;
	}

	mod->ps3_alias_draws = Hunk_Alloc(mod->ps3_alias_num_draws *
		sizeof(*mod->ps3_alias_draws));
	mod->ps3_alias_refs = Hunk_Alloc(mod->ps3_alias_num_refs *
		sizeof(*mod->ps3_alias_refs));
	mod->ps3_alias_indices = NULL;
	if (mod->ps3_alias_num_indices > 0)
	{
		mod->ps3_alias_indices = Hunk_Alloc(mod->ps3_alias_num_indices *
			sizeof(*mod->ps3_alias_indices));
	}
	draw = mod->ps3_alias_draws;
	ref = mod->ps3_alias_refs;
	index = mod->ps3_alias_indices;

	for (draw_index = 0; draw_index < mod->ps3_alias_num_draws;
		draw_index++, draw++)
	{
		int count = *order++;
		int i;

		draw->type = count < 0 ? GL_TRIANGLE_FAN : GL_TRIANGLE_STRIP;
		if (count < 0)
		{
			count = -count;
		}
		draw->first_ref = ref_index;
		draw->count = (unsigned short)count;

		for (i = 0; i < count; i++, order += 3, ref++, ref_index++)
		{
			int index_xyz = order[2];

			if (index_xyz < 0 || index_xyz >= pheader->num_xyz)
			{
				ri.Sys_Error(ERR_DROP,
					"model %s has an invalid GL vertex index", mod->name);
			}
			memcpy(&ref->s, order, sizeof(ref->s));
			memcpy(&ref->t, order + 1, sizeof(ref->t));
			ref->index_xyz = (unsigned short)index_xyz;
			ref->reserved = 0;
		}

		/* Fans and strips are immutable. Store their exact triangle winding once
		 * so visible entities and their shadows only add the current batch base. */
		if (index && count >= 3)
		{
			int triangle;

			for (triangle = 0; triangle < count - 2; triangle++)
			{
				if (draw->type == GL_TRIANGLE_FAN)
				{
					*index++ = (unsigned short)draw->first_ref;
					*index++ = (unsigned short)(draw->first_ref + triangle + 1);
					*index++ = (unsigned short)(draw->first_ref + triangle + 2);
				}
				else if ((triangle & 1) == 0)
				{
					*index++ = (unsigned short)(draw->first_ref + triangle);
					*index++ = (unsigned short)(draw->first_ref + triangle + 1);
					*index++ = (unsigned short)(draw->first_ref + triangle + 2);
				}
				else
				{
					*index++ = (unsigned short)(draw->first_ref + triangle + 1);
					*index++ = (unsigned short)(draw->first_ref + triangle);
					*index++ = (unsigned short)(draw->first_ref + triangle + 2);
				}
			}
		}
	}
}
#endif

void
LoadMD2(model_t *mod, void *buffer, int modfilelen)
{
	int i, j;
	dmdl_t *pinmodel, *pheader;
	dstvert_t *pinst, *poutst;
	dtriangle_t *pintri, *pouttri;
	daliasframe_t *pinframe, *poutframe;
	int *pincmd, *poutcmd;
	int version;
	int ofs_end;
	#ifdef PS3_NATIVE_RSX
	size_t hunk_size;
	#endif

	pinmodel = (dmdl_t *)buffer;

	version = LittleLong(pinmodel->version);

	if (version != ALIAS_VERSION)
	{
		ri.Sys_Error(ERR_DROP, "%s has wrong version number (%i should be %i)",
				mod->name, version, ALIAS_VERSION);
	}

	ofs_end = LittleLong(pinmodel->ofs_end);
	if (ofs_end < 0 || ofs_end > modfilelen)
		ri.Sys_Error (ERR_DROP, "model %s file size(%d) too small, should be %d", mod->name,
				   modfilelen, ofs_end);

	#ifdef PS3_NATIVE_RSX
	PS3_CountAliasTopology(mod, pinmodel, modfilelen, ofs_end,
		&mod->ps3_alias_num_draws, &mod->ps3_alias_num_refs,
		&mod->ps3_alias_num_indices);
	hunk_size = PS3_AliasHunkAlignedSize((size_t)ofs_end) +
		PS3_AliasHunkAlignedSize((size_t)mod->ps3_alias_num_draws *
			sizeof(ps3_alias_draw_t)) +
		PS3_AliasHunkAlignedSize((size_t)mod->ps3_alias_num_refs *
			sizeof(ps3_alias_ref_t)) +
		PS3_AliasHunkAlignedSize((size_t)mod->ps3_alias_num_indices *
			sizeof(*mod->ps3_alias_indices));
	if (hunk_size > 0x7fffffffu)
	{
		ri.Sys_Error(ERR_DROP, "model %s native topology is too large",
			mod->name);
	}
	mod->extradata = Hunk_Begin((int)hunk_size);
	#else
	mod->extradata = Hunk_Begin(modfilelen);
	#endif
	pheader = Hunk_Alloc(ofs_end);

	/* byte swap the header fields and sanity check */
	for (i = 0; i < sizeof(dmdl_t) / 4; i++)
	{
		((int *)pheader)[i] = LittleLong(((int *)buffer)[i]);
	}

	if (pheader->skinheight > MAX_LBM_HEIGHT)
	{
		ri.Sys_Error(ERR_DROP, "model %s has a skin taller than %d", mod->name,
				MAX_LBM_HEIGHT);
	}

	if (pheader->num_xyz <= 0)
	{
		ri.Sys_Error(ERR_DROP, "model %s has no vertices", mod->name);
	}

	if (pheader->num_xyz > MAX_VERTS)
	{
		ri.Sys_Error(ERR_DROP, "model %s has too many vertices", mod->name);
	}

	if (pheader->num_st <= 0)
	{
		ri.Sys_Error(ERR_DROP, "model %s has no st vertices", mod->name);
	}

	if (pheader->num_tris <= 0)
	{
		ri.Sys_Error(ERR_DROP, "model %s has no triangles", mod->name);
	}

	if (pheader->num_frames <= 0)
	{
		ri.Sys_Error(ERR_DROP, "model %s has no frames", mod->name);
	}

	/* load base s and t vertices (not used in gl version) */
	pinst = (dstvert_t *)((byte *)pinmodel + pheader->ofs_st);
	poutst = (dstvert_t *)((byte *)pheader + pheader->ofs_st);

	for (i = 0; i < pheader->num_st; i++)
	{
		poutst[i].s = LittleShort(pinst[i].s);
		poutst[i].t = LittleShort(pinst[i].t);
	}

	/* load triangle lists */
	pintri = (dtriangle_t *)((byte *)pinmodel + pheader->ofs_tris);
	pouttri = (dtriangle_t *)((byte *)pheader + pheader->ofs_tris);

	for (i = 0; i < pheader->num_tris; i++)
	{
		for (j = 0; j < 3; j++)
		{
			pouttri[i].index_xyz[j] = LittleShort(pintri[i].index_xyz[j]);
			pouttri[i].index_st[j] = LittleShort(pintri[i].index_st[j]);
		}
	}

	/* load the frames */
	for (i = 0; i < pheader->num_frames; i++)
	{
		pinframe = (daliasframe_t *)((byte *)pinmodel
				+ pheader->ofs_frames + i * pheader->framesize);
		poutframe = (daliasframe_t *)((byte *)pheader
				+ pheader->ofs_frames + i * pheader->framesize);

		memcpy(poutframe->name, pinframe->name, sizeof(poutframe->name));

		for (j = 0; j < 3; j++)
		{
			poutframe->scale[j] = LittleFloat(pinframe->scale[j]);
			poutframe->translate[j] = LittleFloat(pinframe->translate[j]);
		}

		/* verts are all 8 bit, so no swapping needed */
		memcpy(poutframe->verts, pinframe->verts,
				pheader->num_xyz * sizeof(dtrivertx_t));
	}

	mod->type = mod_alias;

	/* load the glcmds */
	pincmd = (int *)((byte *)pinmodel + pheader->ofs_glcmds);
	poutcmd = (int *)((byte *)pheader + pheader->ofs_glcmds);

	for (i = 0; i < pheader->num_glcmds; i++)
	{
		poutcmd[i] = LittleLong(pincmd[i]);
	}
	#ifdef PS3_NATIVE_RSX
	PS3_PrepareAliasTopology(mod, pheader);
	#endif

	if (poutcmd[pheader->num_glcmds-1] != 0)
	{
		R_Printf(PRINT_ALL, "%s: Entity %s has possible last element issues with %d verts.\n",
			__func__,
			mod->name,
			poutcmd[pheader->num_glcmds-1]);
	}

	/* register all skins */
	memcpy((char *)pheader + pheader->ofs_skins,
			(char *)pinmodel + pheader->ofs_skins,
			pheader->num_skins * MAX_SKINNAME);

	for (i = 0; i < pheader->num_skins; i++)
	{
		mod->skins[i] = R_FindImage(
				(char *)pheader + pheader->ofs_skins + i * MAX_SKINNAME,
				it_skin);
	}

	mod->mins[0] = -32;
	mod->mins[1] = -32;
	mod->mins[2] = -32;
	mod->maxs[0] = 32;
	mod->maxs[1] = 32;
	mod->maxs[2] = 32;
}
