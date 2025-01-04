/**
 * @file	fs/vdfs2/debug.h
 * @brief	The eMMCFS kernel debug support.
 * @date	01/17/2012
 *
 * eMMCFS -- Samsung eMMC chip oriented File System, Version 1.
 *
 * This files defines debug tools.
 *
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

#ifndef _VDFS2_DEBUG_H_
#define _VDFS2_DEBUG_H_

#ifndef USER_SPACE
#include <linux/crc32.h>
#endif

/**
 * @brief		Memory dump.
 * @param [in]	type	Sets debug type (see VDFS2_DBG_*).
 * @param [in]	buf	Pointer to memory.
 * @param [in]	len	Byte count in the dump.
 * @return	void
 */
#define VDFS2_MDUMP(type, buf, len)\
	do {\
		if ((type) & vdfs2_debug_mask) {\
			VDFS2_DEBUG(type, "");\
			print_hex_dump(KERN_INFO, "vdfs2 ",\
					DUMP_PREFIX_ADDRESS, 16, 1, buf, len,\
					true);\
		} \
	} while (0)

/**
 * @brief		Print error message to kernel ring buffer.
 * @param [in]	fmt	Printf format string.
 * @return	void
 */
#define VDFS2_ERR(fmt, ...)\
	do {\
		printk(KERN_ERR "vdfs2-ERROR:%d:%s: " fmt "\n", __LINE__,\
			__func__, ##__VA_ARGS__);\
	} while (0)

/** Enables VDFS2_DEBUG_SB() in super.c */
#define VDFS2_DBG_SB	(1 << 0)

/** Enables VDFS2_DEBUG_INO() in inode.c */
#define VDFS2_DBG_INO	(1 << 1)

/** Enables VDFS2_DEBUG_INO() in fsm.c, fsm_btree.c */
#define VDFS2_DBG_FSM	(1 << 2)

/** Enables VDFS2_DEBUG_SNAPSHOT() in snapshot.c */
#define VDFS2_DBG_SNAPSHOT	(1 << 3)

/** Enables VDFS2_DEBUG_MUTEX() driver-wide */
#define VDFS2_DBG_MUTEX	(1 << 4)

/** Enables VDFS2_DEBUG_TRN() driver-wide. Auxiliary, you can remove this, if
 * there is no more free vdfs2_debug_mask bits */
#define VDFS2_DBG_TRANSACTION (1 << 5)

#define VDFS2_DBG_BTREE (1 << 6)

/* Non-permanent debug */
#define VDFS2_DBG_TMP (1 << 7)


#if defined(CONFIG_VDFS2_DEBUG)
/**
 * @brief		Print debug information.
 * @param [in]	type	Sets debug type (see VDFS2_DBG_*).
 * @param [in]	fmt	Printf format string.
 * @return	void
 */
#define VDFS2_DEBUG(type, fmt, ...)\
	do {\
		if ((type) & vdfs2_debug_mask)\
			printk(KERN_INFO "%s:%d:%s: " fmt "\n", __FILE__,\
				__LINE__, __func__, ##__VA_ARGS__);\
	} while (0)
#else
#define VDFS2_DEBUG(type, fmt, ...) do {} while (0)
#endif

/**
 * @brief		Print debug information in super.c.
 * @param [in]	fmt	Printf format string.
 * @return	void
 */
#define VDFS2_DEBUG_SB(fmt, ...) VDFS2_DEBUG(VDFS2_DBG_SB, fmt,\
						##__VA_ARGS__)

/**
 * @brief		Print debug information in inode.c.
 * @param [in]	fmt	Printf format string.
 * @return	void
 */
#define VDFS2_DEBUG_INO(fmt, ...) VDFS2_DEBUG(VDFS2_DBG_INO, fmt,\
						##__VA_ARGS__)

/**
 * @brief		Print debug information in fsm.c, fsm_btree.c.
 * @param [in]	fmt	Printf format string.
 * @return	void
 */
#define VDFS2_DEBUG_FSM(fmt, ...) VDFS2_DEBUG(VDFS2_DBG_FSM, fmt,\
						##__VA_ARGS__)

/**
 * @brief		Print debug information in snapshot.c.
 * @param [in]	fmt	Printf format string.
 * @return	void
 */
#define VDFS2_DEBUG_SNAPSHOT(fmt, ...) VDFS2_DEBUG(VDFS2_DBG_SNAPSHOT, fmt,\
						##__VA_ARGS__)

/**
 * @brief		TODO Print debug information in ...
 * @param [in]	fmt	Printf format string.
 * @return	void
 */
#define VDFS2_DEBUG_MUTEX(fmt, ...) VDFS2_DEBUG(VDFS2_DBG_MUTEX, fmt,\
						##__VA_ARGS__)

/**
 * @brief		Print debug information with pid.
 * @param [in]	fmt	Printf format string.
 * @return	void
 */
#define VDFS2_DEBUG_TRN(fmt, ...) VDFS2_DEBUG(VDFS2_DBG_TRANSACTION,\
				"pid=%d " fmt,\
				((struct task_struct *) current)->pid,\
				##__VA_ARGS__)

/**
 * @brief		Print non-permanent debug information.
 * @param [in]	fmt	Printf format string.
 * @return	void
 */
#define VDFS2_DEBUG_TMP(fmt, ...) VDFS2_DEBUG(VDFS2_DBG_TMP,\
				"pid=%d " fmt,\
				((struct task_struct *) current)->pid,\
				##__VA_ARGS__)

#define VDFS2_DEBUG_BTREE(fmt, ...) VDFS2_DEBUG(VDFS2_DBG_BTREE, fmt, \
				##__VA_ARGS__)

extern unsigned int vdfs2_debug_mask;

#if defined(CONFIG_VDFS2_DEBUG)
void vdfs2_debug_print_sb(struct vdfs2_sb_info *sbi);
#else
static inline void vdfs2_debug_print_sb(
		struct vdfs2_sb_info *sbi  __attribute__ ((unused))) {}
#endif

#endif /* _VDFS2_DEBUG_H_ */
