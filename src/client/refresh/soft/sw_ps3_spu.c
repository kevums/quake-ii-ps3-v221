/*
 * Optional PS3 SPU frame conversion backend.
 *
 * The workers convert Quake II's 8-bit software framebuffer into XRGB and
 * optionally scale it.  DMA is restricted to aligned main-memory staging
 * buffers; the PPU performs the final linear copy into RSX local memory.
 */

#include <malloc.h>
#include <string.h>

#include <sys/spu.h>
#include <sys/systime.h>

#include "header/local_ps3.h"
#include "header/ps3_spu_frame.h"
#include "ps3_frame_spu_bin.h"

#define PTR_TO_EA(pointer) ((u64)(void *)(pointer))
#define PS3_SPU_GROUP_PRIORITY 200
#define PS3_SPU_WAIT_ITERATIONS 10000
#define PS3_SPU_WAIT_USEC 100

static cvar_t *ps3_spu_accel;
static cvar_t *ps3_spu_workers;

static sysSpuImage frame_image;
static sys_spu_group_t frame_group;
static sys_spu_thread_t frame_threads[PS3_SPU_FRAME_MAX_WORKERS];
static ps3_spu_frame_job_t *frame_jobs;
static qboolean image_loaded;
static qboolean group_created;
static qboolean group_started;
static qboolean acceleration_ready;
static int active_workers;
static uint32_t job_generation;

static uint8_t *source_staging;
static size_t source_staging_capacity;
static uint32_t source_staging_pitch;
static uint32_t *destination_staging;
static size_t destination_staging_capacity;
static uint32_t destination_staging_pitch;
static uint32_t *palette_staging;

static void
PS3SPU_FreeMemory(void)
{
	free(source_staging);
	source_staging = NULL;
	source_staging_capacity = 0;
	source_staging_pitch = 0;

	free(destination_staging);
	destination_staging = NULL;
	destination_staging_capacity = 0;
	destination_staging_pitch = 0;

	free(palette_staging);
	palette_staging = NULL;

	free(frame_jobs);
	frame_jobs = NULL;
}

static void
PS3SPU_DestroyRuntime(qboolean terminate)
{
	if (group_started)
	{
		u32 cause = 0;
		u32 status = 0;

		if (terminate)
		{
			sysSpuThreadGroupTerminate(frame_group, 1);
		}

		sysSpuThreadGroupJoin(frame_group, &cause, &status);
		group_started = false;
	}

	if (group_created)
	{
		sysSpuThreadGroupDestroy(frame_group);
		group_created = false;
	}

	if (image_loaded)
	{
		sysSpuImageClose(&frame_image);
		image_loaded = false;
	}

	PS3SPU_FreeMemory();
	acceleration_ready = false;
	active_workers = 0;
}

static void
PS3SPU_DisableAfterFault(const char *reason)
{
	R_Printf(PRINT_ALL, "PS3 SPU acceleration disabled: %s\n", reason);
	PS3SPU_DestroyRuntime(true);

	if (ps3_spu_accel)
	{
		ri.Cvar_Set("ps3_spu_accel", "0");
	}
}

void
PS3SPU_Init(void)
{
	sysSpuThreadGroupAttribute group_attribute;
	sysSpuThreadAttribute thread_attribute;
	int requested_workers;
	int result;
	int index;

	ps3_spu_accel = ri.Cvar_Get("ps3_spu_accel", "0", CVAR_ARCHIVE);
	ps3_spu_workers = ri.Cvar_Get("ps3_spu_workers", "4", CVAR_ARCHIVE);

	if (ps3_spu_accel->value <= 0.0f)
	{
		R_Printf(PRINT_ALL, "PS3 SPU acceleration is disabled.\n");
		return;
	}

	requested_workers = (int)ps3_spu_workers->value;
	if (requested_workers < 1)
	{
		requested_workers = 1;
	}
	else if (requested_workers > PS3_SPU_FRAME_MAX_WORKERS)
	{
		requested_workers = PS3_SPU_FRAME_MAX_WORKERS;
	}
	ri.Cvar_SetValue("ps3_spu_workers", requested_workers);

	frame_jobs = memalign(128,
		requested_workers * sizeof(ps3_spu_frame_job_t));
	palette_staging = memalign(128, 256 * sizeof(uint32_t));
	if (!frame_jobs || !palette_staging)
	{
		PS3SPU_DisableAfterFault("couldn't allocate aligned job memory");
		return;
	}
	memset(frame_jobs, 0,
		requested_workers * sizeof(ps3_spu_frame_job_t));

	/* The GameOS limit is six; one is deliberately reserved for PS Move. */
	result = sysSpuInitialize(6, 0);
	if (result != 0)
	{
		R_Printf(PRINT_ALL,
			"sysSpuInitialize returned 0x%x; trying the existing SPU pool.\n",
			result);
	}

	result = sysSpuImageImport(&frame_image, ps3_frame_spu_bin, 0);
	if (result != 0)
	{
		PS3SPU_DisableAfterFault("couldn't import the frame worker image");
		return;
	}
	image_loaded = true;

	sysSpuThreadGroupAttributeInitialize(group_attribute);
	sysSpuThreadGroupAttributeName(group_attribute, "yq2frame");
	result = sysSpuThreadGroupCreate(&frame_group, requested_workers,
		PS3_SPU_GROUP_PRIORITY, &group_attribute);
	if (result != 0)
	{
		PS3SPU_DisableAfterFault("couldn't create the worker group");
		return;
	}
	group_created = true;

	sysSpuThreadAttributeInitialize(thread_attribute);
	sysSpuThreadAttributeName(thread_attribute, "yq2frame-worker");

	for (index = 0; index < requested_workers; index++)
	{
		sysSpuThreadArgument argument;

		sysSpuThreadArgumentInitialize(argument);
		argument.arg0 = PTR_TO_EA(&frame_jobs[index]);

		result = sysSpuThreadInitialize(&frame_threads[index], frame_group,
			index, &frame_image, &thread_attribute, &argument);
		if (result == 0)
		{
			result = sysSpuThreadSetConfiguration(frame_threads[index],
				SPU_SIGNAL1_OVERWRITE | SPU_SIGNAL2_OVERWRITE);
		}

		if (result != 0)
		{
			PS3SPU_DisableAfterFault("couldn't initialize a worker thread");
			return;
		}
	}

	result = sysSpuThreadGroupStart(frame_group);
	if (result != 0)
	{
		PS3SPU_DisableAfterFault("couldn't start the worker group");
		return;
	}
	group_started = true;
	active_workers = requested_workers;
	acceleration_ready = true;
	R_Printf(PRINT_ALL, "PS3 SPU acceleration enabled with %d worker%s.\n",
		active_workers, active_workers == 1 ? "" : "s");
}

void
PS3SPU_Shutdown(void)
{
	int index;
	int wait_iteration;
	qboolean completed = false;

	if (!group_started || !frame_jobs)
	{
		PS3SPU_DestroyRuntime(false);
		return;
	}

	job_generation++;
	if (job_generation == 0)
	{
		job_generation++;
	}

	for (index = 0; index < active_workers; index++)
	{
		frame_jobs[index].command = PS3_SPU_FRAME_COMMAND_EXIT;
		frame_jobs[index].status = PS3_SPU_FRAME_STATUS_OK;
		frame_jobs[index].generation = job_generation;
		frame_jobs[index].sync = 0;
	}
	__sync_synchronize();

	for (index = 0; index < active_workers; index++)
	{
		if (sysSpuThreadWriteSignal(frame_threads[index], 0, 1) != 0)
		{
			PS3SPU_DestroyRuntime(true);
			return;
		}
	}

	for (wait_iteration = 0;
		wait_iteration < PS3_SPU_WAIT_ITERATIONS; wait_iteration++)
	{
		completed = true;
		for (index = 0; index < active_workers; index++)
		{
			if (frame_jobs[index].sync != job_generation)
			{
				completed = false;
				break;
			}
		}

		if (completed)
		{
			break;
		}
		sysUsleep(PS3_SPU_WAIT_USEC);
	}

	PS3SPU_DestroyRuntime(!completed);
}

static qboolean
PS3SPU_EnsureStaging(uint32_t source_width, uint32_t source_height,
	uint32_t destination_width, uint32_t destination_height)
{
	uint32_t needed_source_pitch = (source_width + 127U) & ~127U;
	uint32_t needed_destination_pitch = (destination_width + 3U) & ~3U;
	size_t needed_source_size =
		(size_t)needed_source_pitch * source_height;
	size_t needed_destination_size =
		(size_t)needed_destination_pitch * destination_height *
		sizeof(uint32_t);

	if (needed_source_pitch > PS3_SPU_FRAME_MAX_SOURCE_PITCH ||
		needed_destination_pitch > PS3_SPU_FRAME_MAX_DEST_PITCH)
	{
		return false;
	}

	if (needed_source_size > source_staging_capacity)
	{
		uint8_t *replacement = memalign(128, needed_source_size);
		if (!replacement)
		{
			return false;
		}
		free(source_staging);
		source_staging = replacement;
		source_staging_capacity = needed_source_size;
	}

	if (needed_destination_size > destination_staging_capacity)
	{
		uint32_t *replacement = memalign(128, needed_destination_size);
		if (!replacement)
		{
			return false;
		}
		free(destination_staging);
		destination_staging = replacement;
		destination_staging_capacity = needed_destination_size;
	}

	source_staging_pitch = needed_source_pitch;
	destination_staging_pitch = needed_destination_pitch;
	return true;
}

qboolean
PS3SPU_ConvertFrame(const pixel_t *source, uint32_t source_width,
	uint32_t source_height, uint32_t *destination,
	uint32_t destination_pitch, uint32_t destination_width,
	uint32_t destination_height, const uint32_t *palette)
{
	uint32_t rows_per_worker;
	int index;
	int wait_iteration;
	qboolean completed = false;

	if (!acceleration_ready || !source || !destination || !palette ||
		source_width == 0 || source_height == 0 ||
		destination_width == 0 || destination_height == 0)
	{
		return false;
	}

	if (!PS3SPU_EnsureStaging(source_width, source_height,
		destination_width, destination_height))
	{
		PS3SPU_DisableAfterFault("frame dimensions or staging allocation failed");
		return false;
	}

	for (index = 0; index < (int)source_height; index++)
	{
		uint8_t *staging_row = source_staging +
			(size_t)index * source_staging_pitch;
		memcpy(staging_row, source + (size_t)index * source_width,
			source_width);
		if (source_staging_pitch > source_width)
		{
			memset(staging_row + source_width, 0,
				source_staging_pitch - source_width);
		}
	}
	memcpy(palette_staging, palette, 256 * sizeof(uint32_t));

	job_generation++;
	if (job_generation == 0)
	{
		job_generation++;
	}
	rows_per_worker = (destination_height + active_workers - 1) /
		active_workers;

	for (index = 0; index < active_workers; index++)
	{
		ps3_spu_frame_job_t *frame_job = &frame_jobs[index];
		uint32_t first_row = index * rows_per_worker;
		uint32_t row_count = 0;

		if (first_row < destination_height)
		{
			row_count = destination_height - first_row;
			if (row_count > rows_per_worker)
			{
				row_count = rows_per_worker;
			}
		}

		frame_job->command = PS3_SPU_FRAME_COMMAND_CONVERT;
		frame_job->sync = 0;
		frame_job->status = PS3_SPU_FRAME_STATUS_OK;
		frame_job->generation = job_generation;
		frame_job->source_width = source_width;
		frame_job->source_height = source_height;
		frame_job->source_pitch = source_staging_pitch;
		frame_job->destination_width = destination_width;
		frame_job->destination_height = destination_height;
		frame_job->destination_pitch = destination_staging_pitch;
		frame_job->first_row = first_row;
		frame_job->row_count = row_count;
		frame_job->source_ea = PTR_TO_EA(source_staging);
		frame_job->palette_ea = PTR_TO_EA(palette_staging);
		frame_job->destination_ea = PTR_TO_EA(destination_staging);
	}
	__sync_synchronize();

	for (index = 0; index < active_workers; index++)
	{
		if (sysSpuThreadWriteSignal(frame_threads[index], 0, 1) != 0)
		{
			PS3SPU_DisableAfterFault("couldn't signal a frame worker");
			return false;
		}
	}

	for (wait_iteration = 0;
		wait_iteration < PS3_SPU_WAIT_ITERATIONS; wait_iteration++)
	{
		completed = true;
		for (index = 0; index < active_workers; index++)
		{
			if (frame_jobs[index].sync != job_generation)
			{
				completed = false;
				break;
			}
		}

		if (completed)
		{
			break;
		}
		sysUsleep(PS3_SPU_WAIT_USEC);
	}

	if (!completed)
	{
		PS3SPU_DisableAfterFault("frame worker timeout");
		return false;
	}
	__sync_synchronize();

	for (index = 0; index < active_workers; index++)
	{
		if (frame_jobs[index].status != PS3_SPU_FRAME_STATUS_OK)
		{
			PS3SPU_DisableAfterFault("frame worker rejected its job");
			return false;
		}
	}

	for (index = 0; index < (int)destination_height; index++)
	{
		memcpy(destination + (size_t)index * destination_pitch,
			destination_staging + (size_t)index * destination_staging_pitch,
			destination_width * sizeof(uint32_t));
	}

	return true;
}
