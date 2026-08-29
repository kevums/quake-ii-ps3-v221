#ifndef YQ2_PS3_SPU_FRAME_H
#define YQ2_PS3_SPU_FRAME_H

#include <stdint.h>

#define PS3_SPU_FRAME_MAX_WORKERS 5
#define PS3_SPU_FRAME_MAX_SOURCE_PITCH 2048
#define PS3_SPU_FRAME_MAX_DEST_PITCH 2048

#define PS3_SPU_FRAME_COMMAND_CONVERT 1
#define PS3_SPU_FRAME_COMMAND_EXIT 2

#define PS3_SPU_FRAME_STATUS_OK 0
#define PS3_SPU_FRAME_STATUS_BAD_JOB 1

/*
 * Shared PPU/SPU job descriptor.  Keep this exactly 128 bytes and aligned for
 * one DMA transfer.  The first 16 bytes are also the completion response.
 */
typedef struct ps3_spu_frame_job_s
{
	volatile uint32_t command;
	volatile uint32_t sync;
	volatile uint32_t status;
	uint32_t generation;

	uint32_t source_width;
	uint32_t source_height;
	uint32_t source_pitch;
	uint32_t destination_width;
	uint32_t destination_height;
	uint32_t destination_pitch;
	uint32_t first_row;
	uint32_t row_count;

	uint64_t source_ea;
	uint64_t palette_ea;
	uint64_t destination_ea;

	uint32_t reserved[14];
} __attribute__((aligned(128))) ps3_spu_frame_job_t;

typedef char ps3_spu_frame_job_size_must_be_128[
	(sizeof(ps3_spu_frame_job_t) == 128) ? 1 : -1];

#endif
