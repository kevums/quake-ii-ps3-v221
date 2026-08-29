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
 * Drawing of all images that are not textures
 *
 * =======================================================================
 */

#include "header/local.h"

image_t *draw_chars;

extern qboolean scrap_dirty;
void Scrap_Upload(void);

extern unsigned r_rawpalette[256];

#ifdef __PSL1GHT__
/* Cinematic conversion used to reserve over 300 KiB on the PPU stack. */
static unsigned ps3_raw_image32[320 * 240];
static unsigned char ps3_raw_image8[256 * 256];
#endif

#if defined(PS3_NATIVE_RSX) && defined(PS3_FORCE_FONT_SAMPLER_PER_DRAW)
static void
PS3_ForceFontSamplerNearest(void)
{
	/* Diagnostic fallback for hardware which appears to lose the atlas sampler
	 * after initialization or a later texture-mode transition. Native RSX state
	 * filtering makes already-current parameters command-buffer no-ops. */
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
}
#endif

void
Draw_InitLocal(void)
{
	/* load console characters */
	draw_chars = R_FindImage("pics/conchars.pcx", it_pic);
	if (!draw_chars)
	{
		ri.Sys_Error(ERR_FATAL, "Couldn't load pics/conchars.pcx");
	}

	/* conchars is a packed 16x16 atlas with no gutters. Linear sampling blends
	 * neighboring glyph cells and is especially destructive at the PS3's 2x/3x
	 * console scales, where it looks like stale or doubled lettering. This font
	 * is pixel art and must remain nearest-filtered regardless of the selected
	 * world/2D texture mode. */
	R_Bind(draw_chars->texnum);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
}

/*
 * Draws one 8*8 graphics character with 0 being transparent.
 * It can be clipped to the top of the screen to allow the console to be
 * smoothly scrolled off.
 */
void
RDraw_CharScaled(int x, int y, int num, float scale)
{
#ifndef PS3_NATIVE_RSX
	int row, col;
	float frow, fcol, size, scaledSize;
	float insetS, insetT;
#endif

	num &= 255;

	if ((num & 127) == 32)
	{
		return; /* space */
	}

	if (y <= -8)
	{
		return; /* totally off screen */
	}

#ifdef PS3_NATIVE_RSX
	R_Bind(draw_chars->texnum);
	#ifdef PS3_FORCE_FONT_SAMPLER_PER_DRAW
	PS3_ForceFontSamplerNearest();
	#endif
	RSXGL_DrawTexturedGlyph2D(x, y, (unsigned int)num, scale,
		draw_chars->upload_width, draw_chars->upload_height);
	return;
#else
	row = num >> 4;
	col = num & 15;

	frow = row * 0.0625;
	fcol = col * 0.0625;
	size = 0.0625;
	/* Sample within the selected 8x8 atlas cell. Exact cell-edge UVs can select
	 * an adjacent glyph under RSX's fixed-point rasterization even with nearest
	 * filtering, which presents as a faint doubled edge at large output modes. */
	insetS = draw_chars->upload_width > 0 ?
		0.5f / draw_chars->upload_width : 0.0f;
	insetT = draw_chars->upload_height > 0 ?
		0.5f / draw_chars->upload_height : 0.0f;

	scaledSize = 8*scale;

	R_Bind(draw_chars->texnum);

	GLfloat vtx[] = {
		x, y,
		x + scaledSize, y,
		x + scaledSize, y + scaledSize,
		x, y + scaledSize
	};

	GLfloat tex[] = {
		fcol + insetS, frow + insetT,
		fcol + size - insetS, frow + insetT,
		fcol + size - insetS, frow + size - insetT,
		fcol + insetS, frow + size - insetT
	};

	glEnableClientState( GL_VERTEX_ARRAY );
	glEnableClientState( GL_TEXTURE_COORD_ARRAY );

	glVertexPointer( 2, GL_FLOAT, 0, vtx );
	glTexCoordPointer( 2, GL_FLOAT, 0, tex );
	glDrawArrays( GL_TRIANGLE_FAN, 0, 4 );

	glDisableClientState( GL_VERTEX_ARRAY );
	glDisableClientState( GL_TEXTURE_COORD_ARRAY );
#endif
}

/*
 * Draws a complete fixed-width console-font run.  On PS3 this is deliberately
 * one atomic native submission: the RSX never sees a partially published row,
 * and the PPU no longer repeats the bind/state/reservation path for every
 * glyph.  Other GL1 targets retain the established single-glyph behavior.
 */
void
RDraw_GlyphStringScaled(int x, int y, const char *text, int count,
		float scale, int xor_mask)
{
#ifndef PS3_NATIVE_RSX
	int i;
#endif

	if (!text || count <= 0 || scale <= 0.0f || y + 8.0f * scale <= 0.0f)
	{
		return;
	}

#ifdef PS3_NATIVE_RSX
	R_Bind(draw_chars->texnum);
	#ifdef PS3_FORCE_FONT_SAMPLER_PER_DRAW
	PS3_ForceFontSamplerNearest();
	#endif
	RSXGL_DrawTexturedGlyphs2D(x, y, (const unsigned char *)text, count,
		scale, xor_mask, draw_chars->upload_width,
		draw_chars->upload_height);
#else
	for (i = 0; i < count; i++)
	{
		RDraw_CharScaled(x + (int)(i * 8 * scale), y,
			((unsigned char)text[i]) ^ xor_mask, scale);
	}
#endif
}

image_t *
RDraw_FindPic(char *name)
{
	image_t *gl;
	char fullname[MAX_QPATH];

	if ((name[0] != '/') && (name[0] != '\\'))
	{
		Com_sprintf(fullname, sizeof(fullname), "pics/%s.pcx", name);
		gl = R_FindImage(fullname, it_pic);
	}
	else
	{
		gl = R_FindImage(name + 1, it_pic);
	}

	return gl;
}

void
RDraw_GetPicSize(int *w, int *h, char *pic)
{
	image_t *gl;

	gl = RDraw_FindPic(pic);

	if (!gl)
	{
		*w = *h = -1;
		return;
	}

	*w = gl->width;
	*h = gl->height;
}

void
RDraw_StretchPic(int x, int y, int w, int h, char *pic)
{
	image_t *gl;

	gl = RDraw_FindPic(pic);

	if (!gl)
	{
		R_Printf(PRINT_ALL, "Can't find pic: %s\n", pic);
		return;
	}

	if (scrap_dirty)
	{
		Scrap_Upload();
	}

	R_Bind(gl->texnum);

#ifdef PS3_NATIVE_RSX
	RSXGL_DrawTexturedQuad2D(x, y, x + w, y + h,
		gl->sl, gl->tl, gl->sh, gl->th);
#else

	GLfloat vtx[] = {
		x, y,
		x + w, y,
		x + w, y + h,
		x, y + h
	};

	GLfloat tex[] = {
		gl->sl, gl->tl,
		gl->sh, gl->tl,
		gl->sh, gl->th,
		gl->sl, gl->th
	};

	glEnableClientState( GL_VERTEX_ARRAY );
	glEnableClientState( GL_TEXTURE_COORD_ARRAY );

	glVertexPointer( 2, GL_FLOAT, 0, vtx );
	glTexCoordPointer( 2, GL_FLOAT, 0, tex );
	glDrawArrays( GL_TRIANGLE_FAN, 0, 4 );

	glDisableClientState( GL_VERTEX_ARRAY );
	glDisableClientState( GL_TEXTURE_COORD_ARRAY );
#endif
}

void
RDraw_PicScaled(int x, int y, char *pic, float factor)
{
	image_t *gl;

	gl = RDraw_FindPic(pic);

	if (!gl)
	{
		R_Printf(PRINT_ALL, "Can't find pic: %s\n", pic);
		return;
	}

	if (scrap_dirty)
	{
		Scrap_Upload();
	}

	R_Bind(gl->texnum);

#ifdef PS3_NATIVE_RSX
	RSXGL_DrawTexturedQuad2D(x, y, x + gl->width * factor,
		y + gl->height * factor, gl->sl, gl->tl, gl->sh, gl->th);
#else

	GLfloat vtx[] = {
		x, y,
		x + gl->width * factor, y,
		x + gl->width * factor, y + gl->height * factor,
		x, y + gl->height * factor
	};

	GLfloat tex[] = {
		gl->sl, gl->tl,
		gl->sh, gl->tl,
		gl->sh, gl->th,
		gl->sl, gl->th
	};

	glEnableClientState( GL_VERTEX_ARRAY );
	glEnableClientState( GL_TEXTURE_COORD_ARRAY );

	glVertexPointer( 2, GL_FLOAT, 0, vtx );
	glTexCoordPointer( 2, GL_FLOAT, 0, tex );
	glDrawArrays( GL_TRIANGLE_FAN, 0, 4 );

	glDisableClientState( GL_VERTEX_ARRAY );
	glDisableClientState( GL_TEXTURE_COORD_ARRAY );
#endif
}

/*
 * This repeats a 64*64 tile graphic to fill
 * the screen around a sized down
 * refresh window.
 */
void
RDraw_TileClear(int x, int y, int w, int h, char *pic)
{
	image_t *image;

	image = RDraw_FindPic(pic);

	if (!image)
	{
		R_Printf(PRINT_ALL, "Can't find pic: %s\n", pic);
		return;
	}

	R_Bind(image->texnum);

#ifdef PS3_NATIVE_RSX
	RSXGL_DrawTexturedQuad2D(x, y, x + w, y + h,
		x / 64.0f, y / 64.0f,
		(x + w) / 64.0f, (y + h) / 64.0f);
#else

	GLfloat vtx[] = {
		x, y,
		x + w, y,
		x + w, y + h,
		x, y + h
	};

	GLfloat tex[] = {
		x / 64.0, y / 64.0,
		( x + w ) / 64.0, y / 64.0,
		( x + w ) / 64.0, ( y + h ) / 64.0,
		x / 64.0, ( y + h ) / 64.0
	};

	glEnableClientState( GL_VERTEX_ARRAY );
	glEnableClientState( GL_TEXTURE_COORD_ARRAY );

	glVertexPointer( 2, GL_FLOAT, 0, vtx );
	glTexCoordPointer( 2, GL_FLOAT, 0, tex );
	glDrawArrays( GL_TRIANGLE_FAN, 0, 4 );

	glDisableClientState( GL_VERTEX_ARRAY );
	glDisableClientState( GL_TEXTURE_COORD_ARRAY );
#endif
}

/*
 * Fills a box of pixels with a single color
 */
void
RDraw_Fill(int x, int y, int w, int h, int c)
{
	union
	{
		unsigned c;
		byte v[4];
	} color;

	if ((unsigned)c > 255)
	{
		ri.Sys_Error(ERR_FATAL, "Draw_Fill: bad color");
	}

	glDisable(GL_TEXTURE_2D);

	color.c = d_8to24table[c];
	glColor4f(color.v [ 0 ] / 255.0, color.v [ 1 ] / 255.0,
			   color.v [ 2 ] / 255.0, 1);

#ifdef PS3_NATIVE_RSX
	RSXGL_DrawTexturedQuad2D(x, y, x + w, y + h,
		0.0f, 0.0f, 1.0f, 1.0f);
#else

	GLfloat vtx[] = {
		x, y,
		x + w, y,
		x + w, y + h,
		x, y + h
	};

	glEnableClientState( GL_VERTEX_ARRAY );

	glVertexPointer( 2, GL_FLOAT, 0, vtx );
	glDrawArrays( GL_TRIANGLE_FAN, 0, 4 );

	glDisableClientState( GL_VERTEX_ARRAY );
#endif

	glColor4f( 1, 1, 1, 1 );
	glEnable(GL_TEXTURE_2D);
}

void
RDraw_FadeScreen(void)
{
	#ifdef PS3_NATIVE_RSX
	static qboolean traced_first_fade;
	if (!traced_first_fade)
	{
		PS3_BOOT_TRACE("GL1 first fade: entered");
	}
	#endif
	glEnable(GL_BLEND);
	glDisable(GL_TEXTURE_2D);
	glColor4f(0, 0, 0, 0.8);

#ifdef PS3_NATIVE_RSX
	RSXGL_DrawTexturedQuad2D(0, 0, vid.width, vid.height,
		0.0f, 0.0f, 1.0f, 1.0f);
	if (!traced_first_fade)
	{
		PS3_BOOT_TRACE("GL1 first fade: native quad submitted");
		traced_first_fade = true;
	}
#else

	GLfloat vtx[] = {
		0, 0,
		vid.width, 0,
		vid.width, vid.height,
		0, vid.height
	};

	glEnableClientState( GL_VERTEX_ARRAY );

	glVertexPointer( 2, GL_FLOAT, 0, vtx );
	#ifdef PS3_NATIVE_RSX
	if (!traced_first_fade)
	{
		PS3_BOOT_TRACE("GL1 first fade: before DrawArrays");
	}
	#endif
	glDrawArrays( GL_TRIANGLE_FAN, 0, 4 );
	#ifdef PS3_NATIVE_RSX
	if (!traced_first_fade)
	{
		PS3_BOOT_TRACE("GL1 first fade: DrawArrays complete");
		traced_first_fade = true;
	}
	#endif

	glDisableClientState( GL_VERTEX_ARRAY );
#endif

	glColor4f(1, 1, 1, 1);
	glEnable(GL_TEXTURE_2D);
	glDisable(GL_BLEND);
}

void
RDraw_StretchRaw(int x, int y, int w, int h, int cols, int rows, byte *data)
{
	GLfloat tex[8];
	byte *source;
	float hscale = 1.0f;
	int frac, fracstep;
	int i, j, trows;
	int row;

	R_Bind(0);

	if(gl_config.npottextures || rows <= 256)
	{
		// X, X
		tex[0] = 0;
		tex[1] = 0;

		// X, Y
		tex[2] = 1;
		tex[3] = 0;

		// Y, X
		tex[4] = 1;
		tex[5] = 1;

		// Y, Y
		tex[6] = 0;
		tex[7] = 1;
	}
	else
	{
		// Scale params
		hscale = rows / 256.0;
		trows = 256;

		// X, X
		tex[0] = 1.0 / 512.0;
		tex[1] = 1.0 / 512.0;

		// X, Y
		tex[2] = 511.0 / 512.0;
		tex[3] = 1.0 / 512.0;

		// Y, X
		tex[4] = 511.0 / 512.0;
		tex[5] = rows * hscale / 256 - 1.0 / 512.0;

		// Y, Y
		tex[6] = 1.0 / 512.0;
		tex[7] = rows * hscale / 256 - 1.0 / 512.0;
	}

	#ifndef PS3_NATIVE_RSX
	GLfloat vtx[] = {
			x, y,
			x + w, y,
			x + w, y + h,
			x, y + h
	};
	#endif

	if (!gl_config.palettedtexture)
	{
		#ifdef __PSL1GHT__
		unsigned *image32 = ps3_raw_image32;
		#else
		unsigned image32[320*240]; /* was 256 * 256, but we want a bit more space */
		#endif

		/* .. because now if non-power-of-2 textures are supported, we just load
		 * the data into a texture in the original format, without skipping any
		 * pixels to fit into a 256x256 texture.
		 * This causes text in videos (which are 320x240) to not look broken anymore.
		 */
		if(gl_config.npottextures || rows <= 256)
		{
			unsigned* img = image32;

			if(cols*rows > 320*240)
			{
				/* in case there is a bigger video after all,
				 * malloc enough space to hold the frame */
				img = (unsigned*)malloc(cols*rows*4);
			}

			for(i=0; i<rows; ++i)
			{
				int rowOffset = i*cols;
				for(j=0; j<cols; ++j)
				{
					byte palIdx = data[rowOffset+j];
					img[rowOffset+j] = r_rawpalette[palIdx];
				}
			}

			glTexImage2D(GL_TEXTURE_2D, 0, gl_tex_solid_format,
								cols, rows, 0, GL_RGBA, GL_UNSIGNED_BYTE,
								img);

			if(img != image32)
			{
				free(img);
			}
		}
		else
		{
			#ifdef __PSL1GHT__
			unsigned int *image32 = ps3_raw_image32;
			#else
			unsigned int image32[320*240];
			#endif
			unsigned *dest;

			for (i = 0; i < trows; i++)
			{
				row = (int)(i * hscale);

				if (row > rows)
				{
					break;
				}

				source = data + cols * row;
				dest = &image32[i * 256];
				fracstep = cols * 0x10000 / 256;
				frac = fracstep >> 1;

				for (j = 0; j < 256; j++)
				{
					dest[j] = r_rawpalette[source[frac >> 16]];
					frac += fracstep;
				}
			}

			glTexImage2D(GL_TEXTURE_2D, 0, gl_tex_solid_format,
					256, 256, 0, GL_RGBA, GL_UNSIGNED_BYTE,
					image32);
		}
	}
	else
	{
		#ifdef __PSL1GHT__
		unsigned char *image8 = ps3_raw_image8;
		#else
		unsigned char image8[256 * 256];
		#endif
		unsigned char *dest;

		for (i = 0; i < trows; i++)
		{
			row = (int)(i * hscale);

			if (row > rows)
			{
				break;
			}

			source = data + cols * row;
			dest = &image8[i * 256];
			fracstep = cols * 0x10000 / 256;
			frac = fracstep >> 1;

			for (j = 0; j < 256; j++)
			{
				dest[j] = source[frac >> 16];
				frac += fracstep;
			}
		}

		glTexImage2D(GL_TEXTURE_2D, 0, GL_COLOR_INDEX8_EXT, 256, 256,
				0, GL_COLOR_INDEX, GL_UNSIGNED_BYTE, image8);
	}

	// Note: gl_filter_min could be GL_*_MIPMAP_* so we can't use it for min filter here (=> no mipmaps)
	//       but gl_filter_max (either GL_LINEAR or GL_NEAREST) should do the trick.
	GLint filter = (r_videos_unfiltered->value == 0) ? gl_filter_max : GL_NEAREST;
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, filter);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, filter);

#ifdef PS3_NATIVE_RSX
	RSXGL_DrawTexturedQuad2D(x, y, x + w, y + h,
		tex[0], tex[1], tex[4], tex[5]);
#else
	glEnableClientState( GL_VERTEX_ARRAY );
	glEnableClientState( GL_TEXTURE_COORD_ARRAY );

	glVertexPointer( 2, GL_FLOAT, 0, vtx );
	glTexCoordPointer( 2, GL_FLOAT, 0, tex );
	glDrawArrays( GL_TRIANGLE_FAN, 0, 4 );

	glDisableClientState( GL_VERTEX_ARRAY );
	glDisableClientState( GL_TEXTURE_COORD_ARRAY );
#endif
}

int
Draw_GetPalette(void)
{
	int i;
	int r, g, b;
	unsigned v;
	byte *pic, *pal;
	int width, height;

	/* get the palette */
	LoadPCX("pics/colormap.pcx", &pic, &pal, &width, &height);

	if (!pal)
	{
		ri.Sys_Error(ERR_FATAL, "Couldn't load pics/colormap.pcx");
	}

	for (i = 0; i < 256; i++)
	{
		r = pal[i * 3 + 0];
		g = pal[i * 3 + 1];
		b = pal[i * 3 + 2];

		v = (255 << 24) + (r << 0) + (g << 8) + (b << 16);
		d_8to24table[i] = LittleLong(v);
	}

	d_8to24table[255] &= LittleLong(0xffffff); /* 255 is transparent */

	free(pic);
	free(pal);

	return 0;
}
