/**
 * @file	fs/vdfs2/debug.c
 * @brief	eMMCFS kernel debug support
 * @author	TODO
 * @date	TODO
 *
 * eMMCFS -- Samsung eMMC chip oriented File System, Version 1.
 * TODO: Detailed description
 * @see		TODO: documents
 *
 * Copyright 2011 by Samsung Electronics, Inc.,
 *
 * This software is the confidential and proprietary information
 * of Samsung Electronics, Inc. ("Confidential Information").  You
 * shall not disclose such Confidential Information and shall use
 * it only in accordance with the terms of the license agreement
 * you entered into with Samsung.
 */

#include "vdfs2.h"
#include "debug.h"

/**
 * @brief		Prints super block parameters
 * @param [in] sbi	Pointer to super block info data structure
 * @return		void
 */
#if defined(CONFIG_VDFS2_DEBUG)
void vdfs2_debug_print_sb(struct vdfs2_sb_info *sbi)
{
	VDFS2_DEBUG_SB("\nbytes in block = %u\n"\
			"blocks in leb = %u\ntotal lebs count = %llu\n"
			"free blocks count = %llu\n"
			"files count = %llu",
			sbi->block_size, VDFS2_BLOCKS_IN_LEB(sbi),
			sbi->total_leb_count, percpu_counter_sum(
			&sbi->free_blocks_count),
			sbi->files_count);
}
#else
/* inline void vdfs2_debug_print_sb(struct vdfs2_sb_info *sbi) {} */
#endif

