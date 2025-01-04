/**
 * @file	fs/vdfs2/btree.h
 * @brief	Basic B-tree operations - interfaces and prototypes.
 * @date	0/05/2012
 *
 * eMMCFS -- Samsung eMMC chip oriented File System, Version 1.
 *
 * This file contains prototypes, constants and enumerations for vdfs2 btree
 * functioning
 *
 * Copyright 2011 by Samsung Electronics, Inc.,
 *
 * This software is the confidential and proprietary information
 * of Samsung Electronics, Inc. ("Confidential Information").  You
 * shall not disclose such Confidential Information and shall use
 * it only in accordance with the terms of the license agreement
 * you entered into with Samsung.
 */

#ifndef BTREE_H_
#define BTREE_H_

#include "vdfs2_layout.h"


#ifndef USER_SPACE
/* #define CONFIG_VDFS2_DEBUG_GET_BNODE */
#endif

#ifdef CONFIG_VDFS2_DEBUG_GET_BNODE
#include <linux/stacktrace.h>
#endif

#define VDFS2_BNODE_DSCR(bnode) ((struct vdfs2_gen_node_descr *) \
		(bnode)->data)

#define VDFS2_BNODE_RECS_NR(bnode) \
	le16_to_cpu(VDFS2_BNODE_DSCR(bnode)->recs_count)

#define VDFS2_KEY_LEN(key) (le32_to_cpu(key->key_len))
#define VDFS2_RECORD_LEN(key) (le32_to_cpu(key->record_len))
#define VDFS2_NEXT_BNODE_ID(bnode) \
	(le32_to_cpu(VDFS2_BNODE_DSCR(bnode)->next_node_id))
#define VDFS2_PREV_BNODE_ID(bnode) \
	(le32_to_cpu(VDFS2_BNODE_DSCR(bnode)->prev_node_id))

#define VDFS2_MAX_BTREE_KEY_LEN (sizeof(struct vdfs2_cattree_key) + 1)
#define VDFS2_MAX_BTREE_REC_LEN (sizeof(struct vdfs2_cattree_key) + 1 + \
		sizeof(struct vdfs2_fork) + sizeof(struct \
				vdfs2_catalog_folder_record))

/** How many free space node should have on removing records, to merge
 * with neighborhood */
#define VDFS2_BNODE_MERGE_LIMIT	(0.7)

#define VDFS2_GET_BNODE_STACK_ITEMS 20

#define VDFS2_WAIT_BNODE_UNLOCK 1
#define VDFS2_NOWAIT_BNODE_UNLOCK 0

#ifndef USER_SPACE
#include "mutex_on_sem.h"
#include <linux/rbtree.h>
#endif

/**
 * @brief		Get pointer to value in key-value pair.
 * @param [in]	key_ptr	Pointer to key-value pair
 * @return		Returns pointer to value in key-value pair
 */
static inline void *get_value_pointer(void *key_ptr)
{
	struct vdfs2_cattree_key *key = key_ptr;
	if (key->gen_key.key_len > VDFS2_MAX_BTREE_KEY_LEN)
		return ERR_PTR(-EINVAL);
	else
		return key_ptr + key->gen_key.key_len;
};

/** TODO */
enum vdfs2_get_bnode_mode {
	/** The bnode is operated in read-write mode */
	VDFS2_BNODE_MODE_RW = 0,
	/** The bnode is operated in read-only mode */
	VDFS2_BNODE_MODE_RO
};

/** TODO */
enum vdfs2_node_type {
	VDFS2_FIRST_NODE_TYPE = 1,
	/** bnode type is index */
	VDFS2_NODE_INDEX = VDFS2_FIRST_NODE_TYPE,
	/** bnode type is leaf */
	VDFS2_NODE_LEAF,
	VDFS2_NODE_NR,
};

/** TODO */
enum vdfs2_btree_type {
	VDFS2_BTREE_FIRST_TYPE = 0,
	/** btree is catalog tree */
	VDFS2_BTREE_CATALOG = VDFS2_BTREE_FIRST_TYPE,
	/** btree is extents overflow tree */
	VDFS2_BTREE_EXTENTS,
	VDFS2_BTREE_PACK,
	VDFS2_BTREE_HARD_LINK,
	/** btree is xattr tree */
	VDFS2_BTREE_XATTRS,
	VDFS2_BTREE_TYPES_NR
};

typedef int (vdfs2_btree_key_cmp)(struct vdfs2_generic_key *,
		struct vdfs2_generic_key *);

#ifdef CONFIG_VDFS2_DEBUG_GET_BNODE
struct vdfs2_get_bnode_trace {
	unsigned long stack_entries[VDFS2_GET_BNODE_STACK_ITEMS];
	struct stack_trace trace;
	struct list_head list;
};
#endif

/**
 * @brief	Structure contains information about bnode in runtime.
 */
struct vdfs2_bnode {
	/** Pointer to memory area where contents of bnode is mapped to */
	void *data;

	/** The bnode's id */
	__u32 node_id;

	/** Array of pointers to \b struct \b page representing pages in
	 * memory containing data of this bnode
	 */
	struct page **pages;

	/** Pointer to tree containing this bnode */
	struct vdfs2_btree *host;

	/** Mode in which bnode is got */
	enum vdfs2_get_bnode_mode mode;

	struct hlist_node hash_node;

	struct list_head lru_node;

	atomic_t ref_count;

	int state;
	int is_dirty;

#ifdef CONFIG_VDFS2_DEBUG_GET_BNODE
	struct list_head get_traces_list;
	spinlock_t get_traces_lock;
#endif
};

#define	VDFS2_BNODE_HASH_SIZE_SHIFT 10
#define	VDFS2_BNODE_HASH_SIZE (1 << VDFS2_BNODE_HASH_SIZE_SHIFT)
#define	VDFS2_BNODE_HASH_MASK (VDFS2_BNODE_HASH_SIZE - 1)
#define VDFS2_ACTIVE_SIZE (VDFS2_BNODE_HASH_SIZE)
#define VDFS2_PASSIVE_SIZE (VDFS2_BNODE_HASH_SIZE)
#define VDFS2_BNODE_CACHE_ITEMS 1000
/**
 * @brief	An eMMCFS B-tree held in memory.
 */
struct vdfs2_btree {
	/** Pointer superblock, possessing  this tree */
	struct vdfs2_sb_info *sbi;

	/** The inode of special file containing  this tree */
	struct inode *inode;
	/** Number of pages enough to contain whole bnode in memory */
	unsigned int pages_per_node;
	/** Number of pages enough to contain whole bnode in memory
	 * (power of 2)*/
	unsigned int log_pages_per_node;
	/** Size of bnode in bytes */
	unsigned int node_size_bytes;
	/** Maximal available record length for this tree */
	unsigned short max_record_len;

	/** Type of the tree */
	enum vdfs2_btree_type btree_type;
	/** Comparison function for this tree */
	vdfs2_btree_key_cmp *comp_fn;

	/** Pointer to head (containing essential info)  bnode */
	struct vdfs2_bnode *head_bnode;
	/** Info about free bnode list */
	struct vdfs2_bnode_bitmap *bitmap;

	struct mutex	hash_lock;

	struct hlist_head hash_table[VDFS2_BNODE_HASH_SIZE];

	int active_use_count;
	struct list_head active_use;

	int passive_use_count;
	struct list_head passive_use;

	/** Lock to protect tree operations */
	rw_mutex_t *rw_tree_lock;
	/** Offset in blocks of tree metadata area start in inode.
	 * (!=0 for packtree metadata appended to the end of squasfs image) */
	__u64 tree_metadata_offset;
	__u32 start_ino;

	void *split_buff;
	struct mutex split_buff_lock;

#ifndef USER_SPACE
#ifdef CONFIG_VDFS2_NFS_SUPPORT
	struct rb_root nfs_files_root;
#endif
#endif
};

/**
 * @brief	Macro gets essential information about tree contained in head
 *		tree node.
 */
#define VDFS2_BTREE_HEAD(btree) ((struct vdfs2_raw_btree_head *) \
	(btree->head_bnode->data))

struct vdfs2_btree_record_pos {
	struct vdfs2_bnode *bnode;
	int pos;
};

/**
 * @brief	Runtime structure representing free bnode id bitmap.
 */
struct vdfs2_bnode_bitmap {
	/** Memory area containing bitmap itself */
	void *data;
	/** Size of bitmap */
	__u64 size;
	/** Starting bnode id */
	__u32 start_id;
	/** Maximal bnode id */
	__u32 end_id;
	/** First free id for quick free bnode id search */
	__u32 first_free_id;
	/** Amount of free bnode ids */
	__u32 free_num;
	/** Amount of bits in bitmap */
	__u32 bits_num;
	/** Pointer to bnode containing this bitmap */
	struct vdfs2_bnode *host;

	/** locking to modify data atomically */
	spinlock_t spinlock;
};

/** @brief	Find function information.
 */
struct vdfs2_find_data {
	/** Filled by find */
	struct vdfs2_bnode *bnode;
	/** The bnode ID */
	__u32 bnode_id;
	/** Position */
	__s32 pos;
};

struct vdfs2_btree_gen_record {
	void *key;
	void *val;
};

struct vdfs2_btree_record_info {
	struct vdfs2_btree_gen_record gen_record;
	struct vdfs2_btree_record_pos rec_pos;
};


static inline struct vdfs2_btree_record_info *VDFS2_BTREE_REC_I(
		struct vdfs2_btree_gen_record *record)
{
	return container_of(record, struct vdfs2_btree_record_info, gen_record);
}


/* Set this to sizeof(crc_type) to handle CRC */
#define VDFS2_BNODE_FIRST_OFFSET 4
#define VDFS2_INVALID_NODE_ID 0

typedef __u32 vdfs2_bt_off_t;
#define VDFS2_BT_INVALID_OFFSET ((__u32) (((__u64) 1 << 32) - 1))

/**
 * @brief	Interface for finding data in the whole tree.
 */
/* NOTE: deprecated function. Use vdfs2_btree_find instead */
struct vdfs2_bnode  *__vdfs2_btree_find(struct vdfs2_btree *tree,
				struct vdfs2_generic_key *key,
				int *pos, enum vdfs2_get_bnode_mode mode);
struct vdfs2_btree_gen_record *vdfs2_btree_find(struct vdfs2_btree *btree,
		struct vdfs2_generic_key *key,
		enum vdfs2_get_bnode_mode mode);

struct vdfs2_btree_gen_record *vdfs2_btree_build_gen_record(
		struct vdfs2_btree *btree, __u32 bnode_id, __u32 pos);
/**
 * @brief	Interface to function get_record.
 */
void *vdfs2_get_btree_record(struct vdfs2_bnode *bnode, int index);

/**
 * @brief	Interface for simple insertion into tree.
 */
int vdfs2_btree_insert(struct vdfs2_btree *btree, void *new_data);
struct vdfs2_btree_gen_record *vdfs2_btree_place_data(struct vdfs2_btree *btree,
		struct vdfs2_generic_key *gen_key);
int vdfs2_get_next_btree_record(struct vdfs2_btree_gen_record *record);

/**
 * @brief	Interface for key removal.
 */
int vdfs2_btree_remove(struct vdfs2_btree *tree,
			struct vdfs2_generic_key *key);


/* Essential for B-tree algorithm functions */

/**
 * @brief	Removes the specified key at level.
 */
/* int vdfs2_btree_remove(struct vdfs2_btree *tree,
		struct vdfs2_generic_key *key, int level); */


void vdfs2_put_bnode(struct vdfs2_bnode *bnode);
struct vdfs2_bnode *vdfs2_get_bnode(struct vdfs2_btree *btree,
	__u32 node_id, enum vdfs2_get_bnode_mode mode, int wait);
/* Temporal stubs */
#define __vdfs2_get_bnode(btree, node_id, mode) vdfs2_get_bnode(btree, node_id, \
		mode, VDFS2_WAIT_BNODE_UNLOCK)

/**
 * @brief	Mark bnode as free.
 */
int vdfs2_destroy_bnode(struct vdfs2_bnode *bnode);

/**
 * @brief	Create, reserve and prepare new bnode.
 */
struct vdfs2_bnode *vdfs2_alloc_new_bnode(struct vdfs2_btree *btree);

/**
 * @brief	Get descriptor of next bnode.
 */
struct vdfs2_bnode *vdfs2_get_next_bnode(struct vdfs2_bnode *bnode);

/**
 * @brief	Mark bnode as dirty (data on disk and in memory differs).
 */
void vdfs2_mark_bnode_dirty(struct vdfs2_bnode *node);

/**
 * @brief	Get B-tree rood bnode id.
 */
u32 vdfs2_btree_get_root_id(struct vdfs2_btree *btree);

/**
 * @brief	Interface for freeing bnode structure without
 *		saving it into cache.
 */

void vdfs2_put_cache_bnode(struct vdfs2_bnode *bnode);

/**
 * @brief			Get next record transparently to user.
 * @param [in,out]	__bnode	Current bnode
 * @param [in,out]	index	Index of offset to get
 * @return			Returns address of the next record even
 *				locating in the next bnode
 */
void *__vdfs2_get_next_btree_record(struct vdfs2_bnode **__bnode, int *index);

/**
 * @brief			B-tree destructor.
 * @param [in,out]	btree	Pointer to btree that will be destroyed
 * @return		void
 */
void vdfs2_put_btree(struct vdfs2_btree *btree);

void vdfs2_release_dirty_record(struct vdfs2_btree_gen_record *record);
void vdfs2_release_record(struct vdfs2_btree_gen_record *record);
void vdfs2_mark_record_dirty(struct vdfs2_btree_gen_record *record);

inline void vdfs2_calc_crc_for_bnode(void *bnode_data,
		struct vdfs2_btree *btree);

#ifdef USER_TEST
void test_init_new_node_descr(struct vdfs2_bnode *bnode,
		enum vdfs2_node_type type);
int vdfs2_temp_stub_insert_into_node(struct vdfs2_bnode *bnode,
		void *new_record, int insert_pos);
#endif

#ifndef USER_SPACE
int vdfs2_check_and_sign_dirty_bnodes(struct page **page,
		struct vdfs2_btree *btree, __u64 version);
#endif /* USER_SPACE */

int vdfs2_init_btree_caches(void);
void vdfs2_destroy_btree_caches(void);
int vdfs2_check_btree_slub_caches_empty(void);

int vdfs2_check_btree_links(struct vdfs2_btree *btree, int *dang_num);
int vdfs2_check_btree_records_order(struct vdfs2_btree *btree);

vdfs2_bt_off_t vdfs2_get_offset(struct vdfs2_bnode *bnode, unsigned int index);
void *vdfs2_get_offset_addr(struct vdfs2_bnode *bnode, unsigned int index);
u_int32_t btree_get_bitmap_size(struct vdfs2_sb_info *sbi);

void vdfs2_temp_stub_init_new_node_descr(struct vdfs2_bnode *bnode,
		enum vdfs2_node_type type);
int vdfs2_temp_stub_insert_into_node(struct vdfs2_bnode *bnode,
		void *new_record, int insert_pos);

#endif /* BTREE_H_ */
