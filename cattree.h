
#ifndef CATTREE_H_
#define CATTREE_H_

#define VDFS2_ROOTDIR_NAME "root"
#define VDFS2_ROOTDIR_OBJ_ID ((__u64) 0)
#define VDFS2_CATALOG_KEY_SIGNATURE_FILE "NDlf"
#define VDFS2_CATALOG_KEY_SIGNATURE_FOLDER "NDld"
#define VDFS2_CATALOG_KEY_SIGNATURE_HLINK "NDlh"
#define VDFS2_EXT_OVERFL_LEAF "NDle"

struct vdfs2_cattree_record {
	struct vdfs2_cattree_key *key;
	/* Value type can be different */
	void *val;
};

#define VDFS2_CATTREE_FOLDVAL(record) \
	((struct vdfs2_catalog_folder_record *) (record->val))

struct vdfs2_cattree_record *vdfs2_cattree_find(struct vdfs2_btree *tree,
		__u64 parent_id, const char *name, int len,
		enum vdfs2_get_bnode_mode mode);

int vdfs2_cattree_remove(struct vdfs2_sb_info *sbi,
		__u64 parent_id, const char *name, int len);

struct vdfs2_cattree_record *vdfs2_cattree_get_first_child(
		struct vdfs2_btree *tree, __u64 catalog_id);

int vdfs2_cattree_get_next_record(struct vdfs2_cattree_record *record);

void vdfs2_release_cattree_dirty(struct vdfs2_cattree_record *record);

struct vdfs2_cattree_record *vdfs2_cattree_place_record(
		struct vdfs2_btree *tree, u64 parent_id,
		const char *name, int len, u8 record_type);

struct vdfs2_cattree_record *vdfs2_cattree_build_record(struct vdfs2_btree * tree,
		__u32 bnode_id, __u32 pos);

#include "vdfs2_layout.h"

/**
 * @brief	Catalog tree key compare function for case-sensitive usecase.
 */
int vdfs2_cattree_cmpfn(struct vdfs2_generic_key *__key1,
		struct vdfs2_generic_key *__key2);

/**
 * @brief	Fill already allocated value area (hardlink).
 */
void vdfs2_fill_hlink_value(struct inode *inode,
		struct vdfs2_catalog_hlink_record *hl_record);

#endif
