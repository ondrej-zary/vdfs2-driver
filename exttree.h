#ifndef EXTTREE_H_
#define EXTTREE_H_

#ifdef USER_SPACE
#include "vdfs2_tools.h"
#endif

#include "vdfs2_layout.h"
#ifndef USER_SPACE
#include "btree.h"
#endif

/* vdfs2_exttree_key.iblock special values */
#define IBLOCK_DOES_NOT_MATTER	0xFFFFFFFF /* any extent for specific inode */
#define IBLOCK_MAX_NUMBER	0xFFFFFFFE /* find last extent ... */

struct vdfs2_exttree_record {
	/** Key */
	struct vdfs2_exttree_key *key;
	/** Extent for key */
	struct vdfs2_extent *lextent;
};

struct vdfs2_exttree_record *vdfs2_extent_find(struct vdfs2_sb_info *sbi,
		__u64 object_id, sector_t iblock, enum vdfs2_get_bnode_mode
		mode);

int vdfs2_exttree_get_next_record(struct vdfs2_exttree_record *record);

struct vdfs2_exttree_record *vdfs2_exttree_find_first_record(
		struct vdfs2_sb_info *sbi, __u64 object_id,
		enum vdfs2_get_bnode_mode mode);

struct  vdfs2_exttree_record *vdfs2_find_last_extent(struct vdfs2_sb_info *sbi,
		__u64 object_id, enum vdfs2_get_bnode_mode mode);

int vdfs2_exttree_remove(struct vdfs2_btree *btree, __u64 object_id,
		sector_t iblock);
/**
 * @brief	Extents tree key compare function.
 */
int vdfs2_exttree_cmpfn(struct vdfs2_generic_key *__key1,
		struct vdfs2_generic_key *__key2);
int vdfs2_exttree_add(struct vdfs2_sb_info *sbi, unsigned long object_id,
		struct vdfs2_extent_info *extent);
#endif
