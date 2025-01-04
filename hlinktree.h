/**
 * @file	hlinktree.h
 * @brief	Hard link B-tree, contains hard link value
 * @date	24/05/2013
 *
 * VDFS2 -- Samsung eMMC chip oriented File System.
 *
 * This file defines hard link btree interface and constants
 *
 * @see
 *
 * Copyright 2013 by Samsung Electronics, Inc.,
 *
 * This software is the confidential and proprietary information
 * of Samsung Electronics, Inc. ("Confidential Information").  You
 * shall not disclose such Confidential Information and shall use
 * it only in accordance with the terms of the license agreement
 * you entered into with Samsung.
 */


#ifndef HLINKTREE_H_
#define HLINKTREE_H_

#ifdef USER_SPACE
#include "vdfs2_tools.h"
#endif

#include "vdfs2_layout.h"

struct vdfs2_hlink_record {
	/* Hard link btee key */
	struct vdfs2_hlinktree_key *key;
	/** Hard link tree value (inode & fork) */
	struct vdfs2_catalog_file_record *hardlink_value;
};

struct vdfs2_hlink_record *vdfs2_hard_link_find(
		struct vdfs2_sb_info *sbi, __u64 inode_i_ino,
		enum vdfs2_get_bnode_mode mode);

int vdfs2_hard_link_insert(struct vdfs2_sb_info *sbi,
		struct vdfs2_catalog_file_record *file_value);

int vdfs2_hard_link_remove(struct vdfs2_sb_info *sbi, __u64 inode_i_ino);


int vdfs2_hlinktree_cmpfn(struct vdfs2_generic_key *__key1,
		struct vdfs2_generic_key *__key2);

void vdfs2_release_hlink(struct vdfs2_hlink_record *record);
void vdfs2_release_hlink_dirty(struct vdfs2_hlink_record *record);
int vdfs2_hlinktree_cache_init(void);
void vdfs2_hlinktree_cache_destroy(void);

#ifndef USER_SPACE
struct vdfs2_hlinktree_key *vdfs2_get_hlinktree_key(void);
void vdfs2_put_hlinktree_key(struct vdfs2_hlinktree_key *key);
#endif

#endif /* HLINKTREE_H_ */
