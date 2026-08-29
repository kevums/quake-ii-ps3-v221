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
 * This file is the starting point of the program. It does some platform
 * specific initialization stuff and calls the common initialization code.
 *
 * =======================================================================
 */

#include "../../common/header/common.h"

int
main(int argc, char **argv)
{
#ifdef PS3_DISC_BUILD
	Sys_Mkdir("/dev_hdd0/tmp/q2ps3-v221/USRDIR");
#endif
	Sys_BootTrace("main: entered", true);

#ifdef PS3_DISC_BUILD
	if (Sys_SetWorkDir("/dev_bdvd/PS3_GAME"))
	{
		Sys_BootTrace("main: chdir disc PS3_GAME complete", false);
	}
	else if (Sys_SetWorkDir("/app_home"))
	{
		Sys_BootTrace("main: chdir app_home fallback complete", false);
	}
	else
	{
		Sys_BootTrace("main: disc and app_home chdir failed", false);
	}
#else
	if (!Sys_SetWorkDir("/dev_hdd0/game/QUAKE2000"))
	{
		Sys_BootTrace("main: chdir QUAKE2000 failed", false);
	}
	else
	{
		Sys_BootTrace("main: chdir QUAKE2000 complete", false);
	}
#endif

	// Call the initialization code.
	// Never returns.
	Sys_BootTrace("main: before Qcommon_Init", false);
	Qcommon_Init(argc, argv);

	return 0;
}
