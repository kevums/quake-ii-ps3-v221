#ifndef YQ2_PSMOVE_PS3_H
#define YQ2_PSMOVE_PS3_H

#include "../../../common/header/common.h"

qboolean PSMove_Init(void);
void PSMove_Shutdown(void);
qboolean PSMove_Update(float *yaw_rate, float *pitch_rate);
void PSMove_RequestCalibration(void);
void PSMove_PrintStatus(void);
const char *PSMove_GetStatusText(void);
const char *PSMove_GetStatusDetail(void);
qboolean PSMove_ShouldDrawStatus(void);

#endif
