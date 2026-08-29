/*
 * Minimal OpenGL 1.x surface implemented by the PS3 RSX renderer.
 *
 * This is intentionally limited to the calls used by Yamagi's GL1 scene
 * frontend.  It is not a general OpenGL implementation: the public names let
 * the mature Quake II renderer feed native RSX shaders and command buffers.
 */
#ifndef YQ2_PS3_RSX_GL_H
#define YQ2_PS3_RSX_GL_H

#include <stddef.h>
#include <stdint.h>

#include <ppu-types.h>
#include <rsx/rsx.h>
#include <sysutil/video.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef unsigned int GLenum;
typedef unsigned char GLboolean;
typedef unsigned int GLbitfield;
typedef void GLvoid;
typedef signed char GLbyte;
typedef short GLshort;
typedef int GLint;
typedef int GLsizei;
typedef unsigned char GLubyte;
typedef unsigned short GLushort;
typedef unsigned int GLuint;
typedef float GLfloat;
typedef double GLdouble;

/* Immutable MD2 draw topology prepared by the GL1 model loader. The RSX
 * backend consumes these records directly so it can pack final vertices once
 * instead of receiving three temporary per-corner float arrays. */
typedef struct
{
	uint32_t first_ref;
	uint16_t count;
	uint16_t type;
} rsxgl_alias_draw_t;

typedef struct
{
	float s;
	float t;
	uint16_t index_xyz;
	uint16_t reserved;
} rsxgl_alias_ref_t;

#ifndef APIENTRY
#define APIENTRY
#endif

#define GL_FALSE                         0
#define GL_TRUE                          1
#define GL_POINTS                        0x0000
#define GL_LINES                         0x0001
#define GL_LINE_STRIP                    0x0003
#define GL_TRIANGLES                     0x0004
#define GL_TRIANGLE_STRIP                0x0005
#define GL_TRIANGLE_FAN                  0x0006
#define GL_QUADS                         0x0007
#define GL_NEVER                         0x0200
#define GL_LESS                          0x0201
#define GL_EQUAL                         0x0202
#define GL_LEQUAL                        0x0203
#define GL_GREATER                       0x0204
#define GL_GEQUAL                        0x0206
#define GL_SRC_COLOR                     0x0300
#define GL_SRC_ALPHA                     0x0302
#define GL_ONE_MINUS_SRC_ALPHA           0x0303
#define GL_DST_COLOR                     0x0306
#define GL_FRONT                         0x0404
#define GL_BACK                          0x0405
#define GL_FRONT_AND_BACK                0x0408
#define GL_VENDOR                        0x1F00
#define GL_RENDERER                      0x1F01
#define GL_VERSION                       0x1F02
#define GL_EXTENSIONS                    0x1F03
#define GL_CW                            0x0900
#define GL_CCW                           0x0901
#define GL_POINT                         0x1B00
#define GL_LINE                          0x1B01
#define GL_FILL                          0x1B02
#define GL_KEEP                          0x1E00
#define GL_REPLACE                       0x1E01
#define GL_INCR                          0x1E02
#define GL_INVERT                        0x150A
#define GL_BYTE                          0x1400
#define GL_UNSIGNED_BYTE                 0x1401
#define GL_SHORT                         0x1402
#define GL_UNSIGNED_SHORT                0x1403
#define GL_FLOAT                         0x1406
#define GL_ZERO                          0
#define GL_ONE                           1
#define GL_DEPTH_BUFFER_BIT              0x00000100
#define GL_STENCIL_BUFFER_BIT            0x00000400
#define GL_COLOR_BUFFER_BIT              0x00004000
#define GL_POINT_SMOOTH                  0x0B10
#define GL_CULL_FACE                     0x0B44
#define GL_DEPTH_TEST                    0x0B71
#define GL_STENCIL_TEST                  0x0B90
#define GL_SCISSOR_TEST                  0x0C11
#define GL_ALPHA_TEST                    0x0BC0
#define GL_BLEND                         0x0BE2
#define GL_TEXTURE_2D                    0x0DE1
#define GL_MODELVIEW_MATRIX              0x0BA6
#define GL_PACK_ALIGNMENT                0x0D05
#define GL_TEXTURE_ENV                   0x2300
#define GL_TEXTURE_ENV_MODE              0x2200
#define GL_TEXTURE_MAG_FILTER            0x2800
#define GL_TEXTURE_MIN_FILTER            0x2801
#define GL_TEXTURE_WRAP_S                0x2802
#define GL_TEXTURE_WRAP_T                0x2803
#define GL_NEAREST                       0x2600
#define GL_LINEAR                        0x2601
#define GL_NEAREST_MIPMAP_NEAREST        0x2700
#define GL_LINEAR_MIPMAP_NEAREST         0x2701
#define GL_NEAREST_MIPMAP_LINEAR         0x2702
#define GL_LINEAR_MIPMAP_LINEAR          0x2703
#define GL_REPEAT                        0x2901
#define GL_MODULATE                      0x2100
#define GL_RGB                           0x1907
#define GL_RGBA                          0x1908
#define GL_COLOR_INDEX                   0x1900
#define GL_R3_G3_B2                      0x2A10
#define GL_RGB4                          0x804F
#define GL_RGB5                          0x8050
#define GL_RGB8                          0x8051
#define GL_RGBA2                         0x8055
#define GL_RGBA4                         0x8056
#define GL_RGB5_A1                       0x8057
#define GL_RGBA8                         0x8058
#define GL_FLAT                          0x1D00
#define GL_SMOOTH                        0x1D01
#define GL_MODELVIEW                     0x1700
#define GL_PROJECTION                    0x1701
#define GL_VERTEX_ARRAY                  0x8074
#define GL_COLOR_ARRAY                   0x8076
#define GL_TEXTURE_COORD_ARRAY           0x8078
#define GL_POLYGON_OFFSET_FILL           0x8037
#define GL_GENERATE_MIPMAP               0x8191
#define GL_NICEST                        0x1102
#define GL_MAX_TEXTURE_MAX_ANISOTROPY_EXT 0x84FF
#define GL_TEXTURE_MAX_ANISOTROPY_EXT    0x84FE

int RSXGL_Init(gcmContextData *context, int virtual_width, int virtual_height);
void RSXGL_Shutdown(void);
void RSXGL_BeginFrame(void);
void RSXGL_SetStereoEye(float camera_separation);
void RSXGL_ApplyDisplayFilter(void);
void RSXGL_EndFrame(void);
enum rsxgl_render_stage
{
	RSXGL_RENDER_STAGE_DLIGHTS = 0,
	RSXGL_RENDER_STAGE_SETUP,
	RSXGL_RENDER_STAGE_WORLD,
	RSXGL_RENDER_STAGE_ENTITIES,
	RSXGL_RENDER_STAGE_EFFECTS,
	RSXGL_RENDER_STAGE_ALPHA,
	RSXGL_RENDER_STAGE_COUNT
};
int RSXGL_StatsEnabled(void);
void RSXGL_RecordRenderStage(enum rsxgl_render_stage stage,
	unsigned long long elapsed_us);
void RSXGL_RecordStereoLightmapReuse(unsigned int pixels);
void RSXGL_RecordFinalLightCacheHit(void);
void RSXGL_RecordWorldClip(unsigned int plane_tests,
	unsigned int inherited_plane_skips);
void RSXGL_SetVSync(int enabled);
int RSXGL_IsVSync(void);
int RSXGL_GetOutputWidth(void);
int RSXGL_GetOutputHeight(void);
int RSXGL_TiledTargetsActive(void);
int RSXGL_ResolveLinearSurface(uint32_t source_offset,
	uint32_t source_pitch, int source_width, int source_height,
	int source_x, int source_y, int source_width_rect,
	int source_height_rect, float destination_x, float destination_y,
	float destination_width, float destination_height);
void RSXGL_Set2DProjection(int width, int height);
void RSXGL_DrawTexturedQuad2D(float x0, float y0, float x1, float y1,
	float s0, float t0, float s1, float t1);
void RSXGL_DrawTexturedGlyph2D(float x, float y, unsigned int glyph,
	float scale, int atlas_width, int atlas_height);
void RSXGL_DrawTexturedGlyphs2D(float x, float y,
	const unsigned char *text, int count, float scale, int xor_mask,
	int atlas_width, int atlas_height);
void RSXGL_DrawTexturedFan(const float *positions,
	int position_stride_floats, const float *texcoords,
	int texcoord_stride_floats, int count);
void RSXGL_DrawParticleTriangles(const float *positions,
	const float *texcoords, const float *colors, int particle_count);
void RSXGL_DrawAliasPrimitive(GLenum mode, const float *positions,
	const float *texcoords, const float *colors, int count);
void RSXGL_DrawAliasShadowPrimitive(GLenum mode, const float *positions,
	int count);
int RSXGL_DrawPreparedAlias(const float *positions, int position_count,
	int position_stride_floats, const float *colors,
	int color_stride_floats, const rsxgl_alias_draw_t *draws, int draw_count,
	const rsxgl_alias_ref_t *refs, int ref_count,
	const uint16_t *prepared_indices, int prepared_index_count);
int RSXGL_CombinedLightmapsEnabled(void);
int RSXGL_BeginLightmappedBatch(GLuint texture, GLuint lightmap,
	float light_scale);
void RSXGL_DrawLightmappedFanPrepared(const float *positions,
	int position_stride_floats, const float *texcoords,
	int texcoord_stride_floats, const float *lightcoords,
	int lightcoord_stride_floats, int count);
void RSXGL_EndLightmappedBatch(void);
void RSXGL_DrawLightmappedFan(GLuint texture, GLuint lightmap,
	const float *positions, int position_stride_floats,
	const float *texcoords, int texcoord_stride_floats,
	const float *lightcoords, int lightcoord_stride_floats,
	int count, float light_scale);
void RSXGL_ResetStaticWorldGeometry(void);
void RSXGL_FinalizeStaticWorldGeometry(void);
int RSXGL_RegisterStaticLightmappedFan(const float *positions,
	int position_stride_floats, const float *texcoords,
	int texcoord_stride_floats, const float *lightcoords,
	int lightcoord_stride_floats, int count, unsigned int *generation,
	unsigned int *vertex_offset, unsigned short *first_vertex);
int RSXGL_DrawStaticLightmappedFan(unsigned int generation,
	unsigned int vertex_offset, unsigned short first_vertex, int count,
	const unsigned short *prepared_indices, int prepared_index_count);

void APIENTRY glAlphaFunc(GLenum func, GLfloat ref);
void APIENTRY glBegin(GLenum mode);
void APIENTRY glBindTexture(GLenum target, GLuint texture);
void APIENTRY glBlendFunc(GLenum sfactor, GLenum dfactor);
void APIENTRY glClear(GLbitfield mask);
void APIENTRY glClearColor(GLfloat red, GLfloat green, GLfloat blue, GLfloat alpha);
void APIENTRY glClearStencil(GLint s);
void APIENTRY glColor4f(GLfloat red, GLfloat green, GLfloat blue, GLfloat alpha);
void APIENTRY glColorMask(GLboolean red, GLboolean green, GLboolean blue, GLboolean alpha);
void APIENTRY glColorPointer(GLint size, GLenum type, GLsizei stride, const GLvoid *pointer);
void APIENTRY glCullFace(GLenum mode);
void APIENTRY glDeleteTextures(GLsizei n, const GLuint *textures);
void APIENTRY glDepthFunc(GLenum func);
void APIENTRY glDepthMask(GLboolean flag);
void APIENTRY glDepthRange(GLdouble near_value, GLdouble far_value);
void APIENTRY glDisable(GLenum cap);
void APIENTRY glDisableClientState(GLenum array);
void APIENTRY glDrawArrays(GLenum mode, GLint first, GLsizei count);
void APIENTRY glDrawBuffer(GLenum mode);
void APIENTRY glEnable(GLenum cap);
void APIENTRY glEnableClientState(GLenum array);
void APIENTRY glEnd(void);
void APIENTRY glFinish(void);
void APIENTRY glFrustum(GLdouble left, GLdouble right, GLdouble bottom, GLdouble top, GLdouble near_value, GLdouble far_value);
void APIENTRY glGetFloatv(GLenum pname, GLfloat *params);
const GLubyte *APIENTRY glGetString(GLenum name);
void APIENTRY glHint(GLenum target, GLenum mode);
void APIENTRY glLoadIdentity(void);
void APIENTRY glLoadMatrixf(const GLfloat *matrix);
void APIENTRY glMatrixMode(GLenum mode);
void APIENTRY glOrtho(GLdouble left, GLdouble right, GLdouble bottom, GLdouble top, GLdouble near_value, GLdouble far_value);
void APIENTRY glPixelStorei(GLenum pname, GLint param);
void APIENTRY glPointSize(GLfloat size);
void APIENTRY glPolygonMode(GLenum face, GLenum mode);
void APIENTRY glPolygonOffset(GLfloat factor, GLfloat units);
void APIENTRY glPopMatrix(void);
void APIENTRY glPushMatrix(void);
void APIENTRY glReadPixels(GLint x, GLint y, GLsizei width, GLsizei height, GLenum format, GLenum type, GLvoid *pixels);
void APIENTRY glRotatef(GLfloat angle, GLfloat x, GLfloat y, GLfloat z);
void APIENTRY glScalef(GLfloat x, GLfloat y, GLfloat z);
void APIENTRY glScissor(GLint x, GLint y, GLsizei width, GLsizei height);
void APIENTRY glShadeModel(GLenum mode);
void APIENTRY glStencilFunc(GLenum func, GLint ref, GLuint mask);
void APIENTRY glStencilMask(GLuint mask);
void APIENTRY glStencilOp(GLenum fail, GLenum zfail, GLenum zpass);
void APIENTRY glTexCoordPointer(GLint size, GLenum type, GLsizei stride, const GLvoid *pointer);
void APIENTRY glTexEnvf(GLenum target, GLenum pname, GLfloat param);
void APIENTRY glTexEnvi(GLenum target, GLenum pname, GLint param);
void APIENTRY glTexImage2D(GLenum target, GLint level, GLint internalformat, GLsizei width, GLsizei height, GLint border, GLenum format, GLenum type, const GLvoid *pixels);
void APIENTRY glTexParameteri(GLenum target, GLenum pname, GLint param);
void APIENTRY glTexSubImage2D(GLenum target, GLint level, GLint xoffset, GLint yoffset, GLsizei width, GLsizei height, GLenum format, GLenum type, const GLvoid *pixels);
void APIENTRY glTranslatef(GLfloat x, GLfloat y, GLfloat z);
void APIENTRY glVertex2f(GLfloat x, GLfloat y);
void APIENTRY glVertex2i(GLint x, GLint y);
void APIENTRY glVertex3fv(const GLfloat *v);
void APIENTRY glVertexPointer(GLint size, GLenum type, GLsizei stride, const GLvoid *pointer);
void APIENTRY glViewport(GLint x, GLint y, GLsizei width, GLsizei height);

#ifdef __cplusplus
}
#endif

#endif
