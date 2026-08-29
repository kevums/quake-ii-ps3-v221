/*
 * Link-time PS Move isolation backend.
 *
 * This intentionally contains no libgem, libcamera or SPURS references. It is
 * used for physical-console recovery builds when an imported Move library must
 * be excluded from the SELF before main() can run.
 */

#include "header/psmove_ps3.h"

qboolean
PSMove_Init(void)
{
	Com_Printf("PS Move is disabled in this recovery build.\n");
	return false;
}

void
PSMove_Shutdown(void)
{
}

qboolean
PSMove_Update(float *yaw_rate, float *pitch_rate)
{
	*yaw_rate = 0.0f;
	*pitch_rate = 0.0f;
	return false;
}

void
PSMove_RequestCalibration(void)
{
	Com_Printf("PS Move calibration is unavailable in this recovery build.\n");
}

void
PSMove_PrintStatus(void)
{
	Com_Printf("PS Move is link-time isolated in this recovery build.\n");
}

const char *
PSMove_GetStatusText(void)
{
	return "PS Move disabled (recovery build)";
}

const char *
PSMove_GetStatusDetail(void)
{
	return "This build excludes GEM/camera imports to guarantee title startup.";
}

qboolean
PSMove_ShouldDrawStatus(void)
{
	return false;
}
