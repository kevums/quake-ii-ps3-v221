/* Draw Quake's underwater, damage and power-up tint in the active eye's 2D
 * viewport.  The legacy GL1 path used a perspective-space quad and inherited
 * the previous draw's blend function, which could saturate one stereo eye. */

#include "header/local.h"

extern float v_blend[4];

void
R_PolyBlend(void)
{
	if (!gl1_polyblend->value || !v_blend[3])
	{
		return;
	}

	R_SetGL2D();
	glDisable(GL_ALPHA_TEST);
	glDisable(GL_DEPTH_TEST);
	glDisable(GL_TEXTURE_2D);
	glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
	glEnable(GL_BLEND);
	glColor4f(v_blend[0], v_blend[1], v_blend[2], v_blend[3]);

#ifdef PS3_NATIVE_RSX
	RSXGL_DrawTexturedQuad2D(0.0f, 0.0f, (float)vid.width,
		(float)vid.height, 0.0f, 0.0f, 1.0f, 1.0f);
#else
	{
		GLfloat vertices[] = {
			0.0f, 0.0f,
			(float)vid.width, 0.0f,
			(float)vid.width, (float)vid.height,
			0.0f, (float)vid.height
		};
		glEnableClientState(GL_VERTEX_ARRAY);
		glVertexPointer(2, GL_FLOAT, 0, vertices);
		glDrawArrays(GL_TRIANGLE_FAN, 0, 4);
		glDisableClientState(GL_VERTEX_ARRAY);
	}
#endif

	glDisable(GL_BLEND);
	glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
	glEnable(GL_TEXTURE_2D);
	glEnable(GL_ALPHA_TEST);
	glColor4f(1.0f, 1.0f, 1.0f, 1.0f);
}
