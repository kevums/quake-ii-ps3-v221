#include <stdint.h>
#include <spu_mfcio.h>
#include <sys/spu_thread.h>

#include "../../header/ps3_spu_frame.h"

#define DMA_TAG 1

static ps3_spu_frame_job_t job __attribute__((aligned(128)));
static uint32_t palette[256] __attribute__((aligned(128)));
static uint8_t source_row[PS3_SPU_FRAME_MAX_SOURCE_PITCH]
	__attribute__((aligned(128)));
static uint32_t destination_row[PS3_SPU_FRAME_MAX_DEST_PITCH]
	__attribute__((aligned(128)));

static void
WaitForDMA(void)
{
	mfc_write_tag_mask(1 << DMA_TAG);
	spu_mfcstat(MFC_TAG_UPDATE_ALL);
}

static void
SendCompletion(uint64_t job_ea, uint32_t status)
{
	job.status = status;
	job.sync = job.generation;

	/* Fence the response behind every output DMA issued by this job. */
	mfc_putf(&job, job_ea, 16, DMA_TAG, 0, 0);
	WaitForDMA();
}

int
main(uint64_t job_ea, uint64_t unused2, uint64_t unused3, uint64_t unused4)
{
	(void)unused2;
	(void)unused3;
	(void)unused4;

	for (;;)
	{
		uint32_t row;

		/* Each PPU signal announces that the descriptor has been replaced. */
		spu_read_signal1();
		mfc_get(&job, job_ea, sizeof(job), DMA_TAG, 0, 0);
		WaitForDMA();

		if (job.command == PS3_SPU_FRAME_COMMAND_EXIT)
		{
			SendCompletion(job_ea, PS3_SPU_FRAME_STATUS_OK);
			break;
		}

		if (job.command != PS3_SPU_FRAME_COMMAND_CONVERT ||
			job.source_width == 0 || job.source_height == 0 ||
			job.destination_width == 0 || job.destination_height == 0 ||
			job.source_pitch > PS3_SPU_FRAME_MAX_SOURCE_PITCH ||
			job.destination_pitch > PS3_SPU_FRAME_MAX_DEST_PITCH ||
			job.source_width > job.source_pitch ||
			job.destination_width > job.destination_pitch ||
			job.first_row + job.row_count > job.destination_height)
		{
			SendCompletion(job_ea, PS3_SPU_FRAME_STATUS_BAD_JOB);
			continue;
		}

		mfc_get(palette, job.palette_ea, sizeof(palette), DMA_TAG, 0, 0);
		WaitForDMA();

		for (row = job.first_row;
			row < job.first_row + job.row_count; row++)
		{
			uint32_t source_y =
				(row * job.source_height) / job.destination_height;
			uint32_t x;
			uint64_t source_ea = job.source_ea +
				(uint64_t)source_y * job.source_pitch;
			uint64_t destination_ea = job.destination_ea +
				(uint64_t)row * job.destination_pitch * sizeof(uint32_t);

			mfc_get(source_row, source_ea, job.source_pitch,
				DMA_TAG, 0, 0);
			WaitForDMA();

			for (x = 0; x < job.destination_width; x++)
			{
				uint32_t source_x =
					(x * job.source_width) / job.destination_width;
				destination_row[x] = palette[source_row[source_x]];
			}

			for (; x < job.destination_pitch; x++)
			{
				destination_row[x] = 0;
			}

			mfc_put(destination_row, destination_ea,
				job.destination_pitch * sizeof(uint32_t),
				DMA_TAG, 0, 0);
			WaitForDMA();
		}

		SendCompletion(job_ea, PS3_SPU_FRAME_STATUS_OK);
	}

	spu_thread_exit(0);
	return 0;
}
