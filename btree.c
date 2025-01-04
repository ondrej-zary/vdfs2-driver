/**
 * @file	fs/vdfs2/btree.c
 * @brief	Basic B-tree opreations.
 * @date	03/05/2012
 *
 * eMMCFS -- Samsung eMMC chip oriented File System, Version 1.
 *
 * This file implements interface and functionality for eMMCFS B-tree
 *
 * Copyright 2011 by Samsung Electronics, Inc.,
 *
 * This software is the confidential and proprietary information
 * of Samsung Electronics, Inc. ("Confidential Information").  You
 * shall not disclose such Confidential Information and shall use
 * it only in accordance with the terms of the license agreement
 * you entered into with Samsung.
 */

#ifndef USER_SPACE
#include <linux/slab.h>
#include <linux/kernel.h>
#include <linux/pagevec.h>
#include <linux/vmalloc.h>

#else
#include "vdfs2_tools.h"
#endif

#include "vdfs2_layout.h"
#include "vdfs2.h"
#include "btree.h"
#include "debug.h"

#define BIN_SEARCH

/** Leaf level in any tree. */
#define VDFS2_BTREE_LEAF_LVL 1

#define VDFS2_BNODE_RESERVE(btree) (vdfs2_btree_get_height(btree)*4)

static struct kmem_cache *btree_traverse_stack_cachep;
static struct kmem_cache *btree_record_info_cachep;
static struct kmem_cache *btree_todo_cachep;

static int __vdfs2_btree_remove(struct vdfs2_btree *tree,
		struct vdfs2_generic_key *key, int level);
/**
 * @brief		Increase B-tree height.
 * @param [in]	btree	B-tree for which the height is increased
 * @return	void
 */
static void vdfs2_btree_inc_height(struct vdfs2_btree *btree)
{
	u16 btree_height = le16_to_cpu(VDFS2_BTREE_HEAD(btree)->btree_height);

	btree_height++;
	VDFS2_BTREE_HEAD(btree)->btree_height = cpu_to_le16(btree_height);
	vdfs2_mark_bnode_dirty(btree->head_bnode);
}

/**
 * @brief		Decrease B-tree height.
 * @param [in]	btree	B-tree for which the height will be decreased
 * @return	void
 */
static void vdfs2_btree_dec_height(struct vdfs2_btree *btree)
{
	u16 btree_height = le16_to_cpu(VDFS2_BTREE_HEAD(btree)->btree_height);
	btree_height--;
	VDFS2_BUG_ON(btree_height == 0);
	VDFS2_BTREE_HEAD(btree)->btree_height = cpu_to_le16(btree_height);
	vdfs2_mark_bnode_dirty(btree->head_bnode);

}

/**
 * @brief		Get B-tree height.
 * @param [in]	btree	B-tree for which the height is requested
 * @return		Returns height of corresponding B-tree
 */
u16 vdfs2_btree_get_height(struct vdfs2_btree *btree)
{
	u16 ret;
	ret = le16_to_cpu(VDFS2_BTREE_HEAD(btree)->btree_height);
	return ret;
}

/**
 * @brief		Get B-tree rood bnode id.
 * @param [in]	btree	Tree for which the root bnode is requested
 * @return		Returns bnode id of the root bnode
 */
u32 vdfs2_btree_get_root_id(struct vdfs2_btree *btree)
{
	u32 ret;
	ret = le32_to_cpu(VDFS2_BTREE_HEAD(btree)->root_bnode_id);
	return ret;
}

/**
 * @brief				Set B-tree rood bnode id.
 * @param [in]	btree			B-tree for which the root bnode is set
 * @param [in]	new_root_bnode_id	New id value for root bnode
 * @return	void
 */
static void vdfs2_btree_set_root_id(struct vdfs2_btree *btree,
		u32 new_root_bnode_id)
{
	VDFS2_BTREE_HEAD(btree)->root_bnode_id = cpu_to_le32(
			new_root_bnode_id);
	vdfs2_mark_bnode_dirty(btree->head_bnode);
}

/**
 * @brief	Structure contains the path in which the search was done to
 *		avoid recursion. Path is contained as array of these records.
 */
struct path_container {
	/** The bnode passed while searching */
	struct vdfs2_bnode *bnode;
	/** Index of record in the bnode */
	int index;
};


/**
 * @brief		Get address of cell containing offset of record with
 *			specified index.
 * @param [in]	bnode	The bnode containing necessary record
 * @param [in]	index	Index of offset to receive
 * @return		Returns address in memory of offset place
 */
void *vdfs2_get_offset_addr(struct vdfs2_bnode *bnode, unsigned int index)
{
	void *ret;
	ret = bnode->data + bnode->host->node_size_bytes -
		VDFS2_BNODE_FIRST_OFFSET -
		sizeof(vdfs2_bt_off_t) * (index + 1);

	if (((unsigned)(ret - bnode->data)) >= bnode->host->node_size_bytes) {
		if (!is_sbi_flag_set(bnode->host->sbi, IS_MOUNT_FINISHED))
			return ERR_PTR(-EFAULT);
		else
			VDFS2_BUG();
	}
	return ret;
}

/**
 * @brief		Get offset for record with specified index
 *			inside bnode.
 * @param [in]	bnode	The bnode containing necessary record
 * @param [in]	index	Index of offset to get
 * @return		Returns offset starting from bnode start address
 */
vdfs2_bt_off_t vdfs2_get_offset(struct vdfs2_bnode *bnode, unsigned int index)
{
	vdfs2_bt_off_t *p_ret;
	p_ret = vdfs2_get_offset_addr(bnode, index);
	if (IS_ERR(p_ret))
		return VDFS2_BT_INVALID_OFFSET;

	return *p_ret;
}

static inline int is_offset_correct(struct vdfs2_bnode *bnode,
		vdfs2_bt_off_t offset)
{
	if (offset == 0)
		return 0;
	if (offset >= bnode->host->node_size_bytes)
		return 0;

	return 1;
}

static int check_bnode_offset_area(struct vdfs2_bnode *bnode)
{
	int recs_count, i;
	vdfs2_bt_off_t offset;

	recs_count = VDFS2_BNODE_DSCR(bnode)->recs_count;
	for (i = 0; i < recs_count; i++) {
		offset = vdfs2_get_offset(bnode, i);
		if (!is_offset_correct(bnode, offset))
			return -1;
	}

	return 0;
}

/**
 * @brief		Get memory address of record with specified index.
 * @param [in]	bnode	The bnode containing necessary record
 * @param [in]	index	Index of offset to get
 * @return		Returns memory pointer to record with corresponding
 *			index
 */
static void *get_record(struct vdfs2_bnode *bnode, unsigned int index)
{
	void *ret;
	vdfs2_bt_off_t offset;

	if (!bnode || index > VDFS2_BNODE_DSCR(bnode)->recs_count)
		return ERR_PTR(-EINVAL);

	offset = vdfs2_get_offset(bnode, index);
	if (!is_offset_correct(bnode, offset))
		return ERR_PTR(-EINVAL);

	ret = (void *)bnode->data + offset;
	return ret;
}

/**
 * @brief		Set offset for record with specified index
 *			inside bnode.
 * @param [in]	bnode	The bnode containing necessary record
 * @param [in]	index	Index of offset to set
 * @param [in]	new_val	New value of offset
 * @return		0 - success, error code - fail
 */
static int check_set_offset(struct vdfs2_bnode *bnode, unsigned int index,
		vdfs2_bt_off_t new_val)
{
	vdfs2_bt_off_t *offset = vdfs2_get_offset_addr(bnode, index);

	if (IS_ERR(offset) || !offset) {
		if (!is_sbi_flag_set(bnode->host->sbi, IS_MOUNT_FINISHED))
			return -EFAULT;
		else
			VDFS2_BUG();
	}

	if (!is_offset_correct(bnode, new_val)) {
		if (!is_sbi_flag_set(bnode->host->sbi, IS_MOUNT_FINISHED))
			return -EFAULT;
		else
			VDFS2_BUG();
	}
	*offset = new_val;


	return 0;
}

/**
 * @brief		Interface to function get_record.
 * @param [in]	bnode	Descriptor of bnode
 * @param [in]	index	Index of offset to get
 * @return		Returns memory pointer to record with corresponding index
 */
void *vdfs2_get_btree_record(struct vdfs2_bnode *bnode, int index)
{
	void *ret;
	ret = get_record(bnode, index);
	return ret;
}

/**
 * @brief		Get descriptor of next bnode.
 * @param [in]	bnode	Current bnode
 * @return		Returns next bnode
 */
static struct vdfs2_bnode *__vdfs2_get_next_bnode(struct vdfs2_bnode *bnode,
		int wait)
{
	struct vdfs2_bnode *ret;

	if (VDFS2_BNODE_DSCR(bnode)->next_node_id == VDFS2_INVALID_NODE_ID) {
		ret = ERR_PTR(-ENOENT);
		return ret;
	}
	ret = vdfs2_get_bnode(bnode->host,
		le32_to_cpu(VDFS2_BNODE_DSCR(bnode)->next_node_id),
		bnode->mode, wait);
	return ret;
}
/**
 * @brief		Get descriptor of next bnode.
 * @param [in]	bnode	Current bnode
 * @return		Returns next bnode
 */
struct vdfs2_bnode *vdfs2_get_next_bnode(struct vdfs2_bnode *bnode)
{
	return __vdfs2_get_next_bnode(bnode, VDFS2_WAIT_BNODE_UNLOCK);
}

/**
 * @brief			Get next record transparently to user.
 * @param [in,out]	__bnode	Current bnode
 * @param [in,out]	index	Index of offset to get
 * @return			Returns address of the next record even
 *				locating in the next bnode
 */
void *__vdfs2_get_next_btree_record(struct vdfs2_bnode **__bnode,
		int *index)
{
	struct vdfs2_bnode *bnode = *__bnode;
	struct vdfs2_bnode *next_bnode;
	void *ret;

	VDFS2_BUG_ON(!bnode);

	if (*index+1 < VDFS2_BNODE_DSCR(bnode)->recs_count) {
		(*index)++;
		ret = get_record(bnode, *index);
		return ret;
	}

	next_bnode = vdfs2_get_next_bnode(bnode);
	if (IS_ERR(next_bnode)) {
		ret = (void *) next_bnode;
		return ret;
	}

	vdfs2_put_bnode(bnode);
	*__bnode = bnode = next_bnode;
	*index = 0;

	ret = get_record(bnode, *index);
	return ret;
}

#if defined(CONFIG_VDFS2_META_SANITY_CHECK)
void vdfs2_bnode_sanity_check(struct vdfs2_bnode *bnode)
{
	struct vdfs2_gen_node_descr *bnode_desc = VDFS2_BNODE_DSCR(bnode);
	struct vdfs2_btree *btree = bnode->host;
	unsigned int max_bnode_size = btree->pages_per_node * PAGE_CACHE_SIZE;
	int record_count = le16_to_cpu(bnode_desc->recs_count);
	int count;
	vdfs2_bt_off_t offset, min_offset = max_bnode_size;
	struct vdfs2_generic_key *key;

	if (bnode->node_id == 0)
		return;

	if (record_count == 0) {
		BUG_ON(vdfs2_get_offset(bnode, 0) !=
				sizeof(struct vdfs2_gen_node_descr));
		return;
	}

	BUG_ON(le16_to_cpu(bnode_desc->free_space) > max_bnode_size);
	BUG_ON(record_count > 1000);

	for (count = 0; count < record_count; count++) {
		offset = vdfs2_get_offset(bnode, count);
		BUG_ON(!is_offset_correct(bnode, offset));
		min_offset = (min_offset > offset) ? offset : min_offset;
	}

	offset = min_offset;

	for (count = 0; count < record_count; count++) {
		key = bnode->data + offset;
		offset += key->record_len;
		BUG_ON(!is_offset_correct(bnode, offset));
	}

	BUG_ON(offset != vdfs2_get_offset(bnode, record_count));
}

void  vdfs2_meta_sanity_check_bnode(void *bnode_data, struct vdfs2_btree *btree,
		pgoff_t index)
{
	struct vdfs2_gen_node_descr *bnode_desc;
	struct vdfs2_bnode bnode;
	bnode_desc = bnode_data;

	if (index)
		bnode.node_id = le32_to_cpu(
		bnode_desc->node_id);
	else
		bnode.node_id = 0;
	bnode.pages = NULL;
	bnode.mode = VDFS2_BNODE_MODE_RO;
	bnode.host = btree;
	bnode.data = bnode_desc;
	vdfs2_bnode_sanity_check(&bnode);
}

#endif

static inline int form_gen_record(
		struct vdfs2_bnode *bnode, void *raw_record,
		struct vdfs2_btree_record_info *rec_info, int pos)
{
	int err_ret = 0;
	rec_info->rec_pos.bnode = bnode;
	rec_info->rec_pos.pos = pos;
	rec_info->gen_record.key = raw_record;
	rec_info->gen_record.val = get_value_pointer(raw_record);
	if (IS_ERR(rec_info->gen_record.val))
		err_ret = -EINVAL;
	return err_ret;
}

struct vdfs2_btree_gen_record *vdfs2_btree_build_gen_record(
		struct vdfs2_btree *btree, __u32 bnode_id, __u32 pos)
{
	void *raw_record;
	struct vdfs2_bnode *bnode;
	struct vdfs2_btree_record_info *rec_info;
	void *err_ret;

	rec_info = kmem_cache_alloc(btree_record_info_cachep, GFP_KERNEL);
	if (!rec_info)
		return ERR_PTR(-ENOMEM);

	bnode = __vdfs2_get_bnode(btree, bnode_id, VDFS2_BNODE_MODE_RO);
	if (IS_ERR(bnode)) {
		err_ret = bnode;
		goto err_exit;
	}

	raw_record = vdfs2_get_btree_record(bnode, pos);
	if (IS_ERR(raw_record)) {
		err_ret = raw_record;
		goto err_exit_put;
	}

	form_gen_record(bnode, raw_record, rec_info, pos);

	return &rec_info->gen_record;

err_exit_put:
	vdfs2_put_bnode(bnode);
err_exit:
	kmem_cache_free(btree_record_info_cachep, rec_info);
	return err_ret;
}

int vdfs2_get_next_btree_record(struct vdfs2_btree_gen_record *record)
{
	void *raw_record;
	struct vdfs2_btree_record_info *rec_info = VDFS2_BTREE_REC_I(record);
	struct vdfs2_bnode *bnode = rec_info->rec_pos.bnode;
	int pos = rec_info->rec_pos.pos;

	raw_record = __vdfs2_get_next_btree_record(&bnode, &pos);
	if (IS_ERR(raw_record))
		return PTR_ERR(raw_record);

	form_gen_record(bnode, raw_record, rec_info, pos);

	return 0;
}

/**
 * @brief		Get child bnode contained in specified index record.
 * @param [in]	btree	B-tree which record belongs to
 * @param [in]	record	Record containing info about child
 * @param [in]	mode	If the requested for r/o or r/w
 * @return		Returns child bnode descriptor
 */
static struct vdfs2_bnode *get_child_bnode(struct vdfs2_btree *btree,
		void *record, enum vdfs2_get_bnode_mode mode)
{
	struct vdfs2_generic_key *key;
	struct generic_index_value *value;
	struct vdfs2_bnode *ret;

	if (IS_ERR(record))
		return record;

	key = (struct vdfs2_generic_key *)record;
	value = (void *)key + key->key_len;

	if (value->node_id == 0) {
		if (!is_sbi_flag_set(btree->sbi, IS_MOUNT_FINISHED))
			return ERR_PTR(-EFAULT);
		else
			VDFS2_BUG();
	}
	ret = __vdfs2_get_bnode(btree, value->node_id, mode);
	return ret;
}

/**
 * @brief			Checks whether node has enough space to
 *				add adding_record.
 * @param [in]	node_descr		Node descriptor to add
 * @param [in]	adding_record	Record to add
 * @return			Returns 0 if the space is enough, 1 otherwise
 */
static int is_node_full(struct vdfs2_bnode *bnode,
		struct vdfs2_generic_key *gen_key)
{
	return VDFS2_BNODE_DSCR(bnode)->free_space <
		gen_key->record_len + sizeof(vdfs2_bt_off_t);
}

/**
 * @brief		Inits newly created bnode descriptor.
 * @param [in]	bnode	Node descriptor to add
 * @param [in]	type	TODO
 * @return	void
 */
static void init_new_node_descr(struct vdfs2_bnode *bnode,
		enum vdfs2_node_type type)
{
	struct vdfs2_gen_node_descr *node_descr = VDFS2_BNODE_DSCR(bnode);


	*((__u16 *) node_descr->magic) = VDFS2_NODE_DESCR_MAGIC;
	node_descr->free_space = cpu_to_le32(bnode->host->node_size_bytes -
		sizeof(struct vdfs2_gen_node_descr) -
		sizeof(vdfs2_bt_off_t) - VDFS2_BNODE_FIRST_OFFSET);
	node_descr->recs_count = 0;
	node_descr->type = type;
	node_descr->node_id = cpu_to_le32(bnode->node_id);

	node_descr->next_node_id = VDFS2_INVALID_NODE_ID;
	node_descr->prev_node_id = VDFS2_INVALID_NODE_ID;

	BUG_ON(check_set_offset(bnode, 0,
				sizeof(struct vdfs2_gen_node_descr)));

}

#ifdef USER_TEST
void test_init_new_node_descr(struct vdfs2_bnode *bnode,
		enum vdfs2_node_type type)
{
	return init_new_node_descr(bnode, type);
}

#endif

void vdfs2_temp_stub_init_new_node_descr(struct vdfs2_bnode *bnode,
		enum vdfs2_node_type type)
{
	init_new_node_descr(bnode, type);
}

/**
 * @brief			Inserts new data into bnode.
 * @param [in]	bnode		The bnode to add
 * @param [in]	new_record	Record to add
 * @param [in]	insert_pos	Position to insert
 * @return	void
 */
/* TODO: need to replace this function by __place_into_node */
static int insert_into_node(struct vdfs2_bnode *bnode,
		void *new_record, int insert_pos)
{
	struct vdfs2_gen_node_descr *node_descr_ptr = bnode->data;
	struct vdfs2_generic_key *new_key =
		(struct vdfs2_generic_key *) new_record;
	void *free_space = NULL;
	vdfs2_bt_off_t *offset;
	int moving_offsets_num;
	int err = 0;

	VDFS2_BUG_ON(node_descr_ptr->free_space < new_key->record_len);

	offset = vdfs2_get_offset_addr(bnode, node_descr_ptr->recs_count);
	if (IS_ERR(offset))
		return PTR_ERR(offset);
	if (!is_offset_correct(bnode, *offset))
		return -EFAULT;

	free_space = (void *) node_descr_ptr + *offset;

	memcpy(free_space, new_record, new_key->record_len);

	/* Prepere space for new offset */
	moving_offsets_num = node_descr_ptr->recs_count - insert_pos;
	if (moving_offsets_num > 0)
		memmove(offset, (void *) (offset + 1),
				moving_offsets_num * sizeof(vdfs2_bt_off_t));
	/* Put new offset */
	err = check_set_offset(bnode, insert_pos,
			free_space - (void *) node_descr_ptr);
	if (err)
		return err;

	/* Check if node have room for another one offset (to free space) */
	VDFS2_BUG_ON(node_descr_ptr->free_space < sizeof(vdfs2_bt_off_t));

	node_descr_ptr->recs_count++;
	node_descr_ptr->free_space -= new_key->record_len +
			sizeof(vdfs2_bt_off_t);

	err = check_set_offset(bnode, node_descr_ptr->recs_count,
			free_space + new_key->record_len -
			(void *) node_descr_ptr);

	return err;
}

static void *__place_into_node(struct vdfs2_bnode *bnode,
		struct vdfs2_generic_key *new_key, int new_rec_len,
		int insert_pos, int place_only_key)
{
	struct vdfs2_gen_node_descr *node_descr_ptr = bnode->data;
	void *new_record_ptr = NULL;
	vdfs2_bt_off_t *offset;
	int moving_offsets_num;
	int err = 0;
	int data_len = place_only_key ? VDFS2_KEY_LEN(new_key) :
		(__u16) new_rec_len;

	__u16 free_space = le16_to_cpu(node_descr_ptr->free_space);
	__u16 recs_count = le16_to_cpu(node_descr_ptr->recs_count);
	/* Check if node have room for another one offset (to free space) */
	VDFS2_BUG_ON(free_space < (new_rec_len + sizeof(vdfs2_bt_off_t)));

	offset = vdfs2_get_offset_addr(bnode, recs_count);
	if (IS_ERR(offset))
		return (void *) offset;
	if (!is_offset_correct(bnode, *offset))
		return ERR_PTR(-EFAULT);

	new_record_ptr = (void *) node_descr_ptr + *offset;

	memcpy(new_record_ptr, new_key, data_len);
	((struct vdfs2_generic_key *) new_record_ptr)->record_len =
		cpu_to_le32(new_rec_len);

	/* Prepare space for new offset */
	moving_offsets_num = recs_count - insert_pos;
	if (moving_offsets_num > 0)
		memmove(offset, (void *) (offset + 1),
				moving_offsets_num * sizeof(vdfs2_bt_off_t));
	/* Put new offset */
	err = check_set_offset(bnode, insert_pos,
			new_record_ptr - (void *) node_descr_ptr);
	if (err)
		return ERR_PTR(err);

	recs_count++;
	node_descr_ptr->recs_count = cpu_to_le16(recs_count);
	free_space -= new_rec_len + sizeof(vdfs2_bt_off_t);
	node_descr_ptr->free_space = cpu_to_le16(free_space);

	err = check_set_offset(bnode, recs_count,
			new_record_ptr + new_rec_len - (void *) node_descr_ptr);

	if (err)
		return ERR_PTR(err);
	return new_record_ptr;
}

static inline int append_bnode(struct vdfs2_bnode *bnode, void *append_record)
{
	struct vdfs2_generic_key *key = append_record;
	int record_len = VDFS2_RECORD_LEN(key);
	void *record = __place_into_node(bnode, append_record, record_len,
			VDFS2_BNODE_DSCR(bnode)->recs_count, 0);

	if (IS_ERR(record))
		return PTR_ERR(record);

	return 0;
}

/* @brief	Insert key and reserve space for value in the given bnode at the
 *		given position. Function ignores field new_key->record_len. This
 *		made to avoid additional memory allocation for the key and
 *		copying memory. We going to use existing key instead (for
 *		example key from another bnode */
static inline int insert_key_into_bnode(
		struct vdfs2_bnode *bnode, struct vdfs2_generic_key *new_key,
		int new_rec_len, int insert_pos,
		struct vdfs2_btree_record_info *res_rec_info)
{
	void *raw_record;

	raw_record = __place_into_node(bnode, new_key, new_rec_len,
			insert_pos, 1);
	if (IS_ERR(raw_record))
		return PTR_ERR(raw_record);

	form_gen_record(bnode, raw_record, res_rec_info, insert_pos);

	return 0;
}

int vdfs2_temp_stub_insert_into_node(struct vdfs2_bnode *bnode,
		void *new_record, int insert_pos)
{
	return insert_into_node(bnode, new_record, insert_pos);
}

/**
 * @brief			Forms necessary info from first record
 *				of node to update upper levels.
 * @param [in]	new_node	New node from which new record is formed
 * @param [in]	adding_record	Pointer memory are to form new record
 * @return	void
 */
static void form_new_adding_record(struct vdfs2_bnode *new_node,
		void *adding_record)
{
	struct vdfs2_generic_key *new_key = get_record(new_node, 0);
	struct vdfs2_generic_key *forming_key;
	struct generic_index_value *value;

	/* Possible wrong new_key isn't checked*/
	BUG_ON(!new_key || IS_ERR(new_key));
	forming_key = (struct vdfs2_generic_key *)adding_record;
	value = (struct generic_index_value *)(adding_record +
							new_key->key_len);

	VDFS2_BUG_ON(new_key->record_len > new_node->host->max_record_len);
	memcpy(adding_record, new_key, new_key->key_len);
	forming_key->record_len = new_key->key_len +
			sizeof(struct generic_index_value);
	value->node_id = new_node->node_id;
}


/**
 * @brief			Removes data from bnode.
 * @param [in]	bnode		The bnode from which data is removed
 * @param [in]	del_index	Index of record to be removed
 * @return	void
 */
static int delete_from_node(struct vdfs2_bnode *bnode, int del_index)
{
	struct vdfs2_gen_node_descr *node = VDFS2_BNODE_DSCR(bnode);
	struct vdfs2_generic_key *del_key =
		(struct vdfs2_generic_key *) get_record(bnode, del_index);
	int i;
	vdfs2_bt_off_t rec_len;
	vdfs2_bt_off_t free_space_offset;
	vdfs2_bt_off_t del_offset;
	int err = 0;

	/*possible wrong del_key isn't checked*/
	if (!del_key || IS_ERR(del_key)) {
		if (!is_sbi_flag_set(bnode->host->sbi, IS_MOUNT_FINISHED))
			return -EFAULT;
		else
			BUG();
	}

	rec_len = del_key->record_len;
	free_space_offset = vdfs2_get_offset(bnode, node->recs_count);
	del_offset = vdfs2_get_offset(bnode, del_index);

	if (!is_offset_correct(bnode, free_space_offset) ||
			!is_offset_correct(bnode, del_offset)) {
		if (!is_sbi_flag_set(bnode->host->sbi, IS_MOUNT_FINISHED))
			return -EFAULT;
		else
			BUG();
	}

	if (rec_len > VDFS2_MAX_BTREE_REC_LEN) {
		if (!is_sbi_flag_set(bnode->host->sbi, IS_MOUNT_FINISHED))
			return -EFAULT;
		else
			BUG();
	}

	memmove((void *)del_key, (void *)del_key + rec_len,
		free_space_offset - del_offset - rec_len);

	/* TODO - while deleting we should do two separate things -
	 * decrease the offsets that are bigger than deleting offset and
	 * shift all record offsets more than deleting in offset table
	 * up.
	 * I've separated them in two cycles because previous version
	 * worked not so good. Think how they can be united in one cycle
	 * without loosing functionality and readability.
	 */
	for (i = 0; i < node->recs_count; i++) {
		vdfs2_bt_off_t cur_offs = vdfs2_get_offset(bnode, i);
		if (cur_offs > del_offset) {
			err = check_set_offset(bnode, i, cur_offs - rec_len);
			if (err)
				return err;
		}
	}

	for (i = del_index; i < node->recs_count; i++) {
		vdfs2_bt_off_t next_offset = vdfs2_get_offset(bnode, i + 1);
		err = check_set_offset(bnode, i, next_offset);
		if (err)
			return err;
	}

	node->recs_count--;
	node->free_space += rec_len + sizeof(vdfs2_bt_off_t);
	err = check_set_offset(bnode,
			node->recs_count, free_space_offset - rec_len);
	if (err)
		return err;

#if defined(CONFIG_VDFS2_DEBUG)
{
	vdfs2_bt_off_t *offset;
	void *free_spc_pointer = NULL;

	offset = vdfs2_get_offset_addr(bnode, node->recs_count);

	if (IS_ERR(offset))
		return PTR_ERR(offset);
	if (!is_offset_correct(bnode, *offset))
		return -EFAULT;

	free_spc_pointer = (void *) node + *offset;
	VDFS2_BUG_ON(node->free_space != (void *) offset - free_spc_pointer);

	memset(free_spc_pointer, 0xA5, node->free_space);
}
#endif

	vdfs2_mark_bnode_dirty(bnode);

	return 0;
}


static void *binary_search(struct vdfs2_bnode *bnode,
		struct vdfs2_generic_key *key, int *pos)
{
	void *record = NULL, *first_record = NULL;
	int first = 0, last = VDFS2_BNODE_RECS_NR(bnode) - 1, mid;
	int cmp_res;
	vdfs2_btree_key_cmp *comp_fn = bnode->host->comp_fn;

	*pos = -1;
	first_record = get_record(bnode, first);
	if (IS_ERR(first_record))
		return first_record;

	cmp_res = comp_fn(first_record, key);
	if (unlikely(cmp_res > 0))
		/* It is possible in dangling bnode check */
		return ERR_PTR(-ENOENT);
	if (0 == cmp_res || last == first) {
		*pos = first;
		return first_record;
	}

	record = get_record(bnode, last);
	if (IS_ERR(record))
		return record;

	if (comp_fn(record, key) <= 0) {
		*pos = last;
		return record;
	}

	while (first < last - 1) {
		mid = first + ((last + 1 - first) >> 1);

		record = get_record(bnode, mid);
		if (IS_ERR(record))
			return record;
		cmp_res = comp_fn(record, key);

		if (cmp_res == 0) {
			*pos = mid;
			return record;
		}

		if (cmp_res < 0) {
			first = mid;
			first_record = record;
			continue;
		}

		if (cmp_res > 0) {
			last = mid;
			continue;
		}
	}

	*pos = first;
	return first_record;
}

struct vdfs2_btree_traverse_stack {
	__u32 bnode_id;
	int pos;
	int level;
	struct list_head list;
};

struct vdfs2_btree_todo_list {
	/* At this moment todo list can contain only insert index operation
	 * In feature it will also support removing and merging */
	struct list_head list;
	struct vdfs2_btree_traverse_stack *traverse_data;
	/*struct vdfs2_generic_key *gen_key;*/
	__u32 point_to_bnode_id;
	__u32 current_root_id;
};

int vdfs2_init_btree_caches(void)
{
	btree_traverse_stack_cachep = NULL;
	btree_record_info_cachep = NULL;
	btree_todo_cachep = NULL;


	btree_traverse_stack_cachep = KMEM_CACHE(vdfs2_btree_traverse_stack,
			SLAB_RECLAIM_ACCOUNT);
	if (!btree_traverse_stack_cachep)
		goto err_exit;

	btree_record_info_cachep = KMEM_CACHE(vdfs2_btree_record_info,
			SLAB_RECLAIM_ACCOUNT);
	if (!btree_traverse_stack_cachep)
		goto err_exit;

	btree_todo_cachep = KMEM_CACHE(vdfs2_btree_todo_list,
			SLAB_RECLAIM_ACCOUNT);
	if (!btree_todo_cachep)
		goto err_exit;

	return 0;

err_exit:
	if (btree_traverse_stack_cachep)
		kmem_cache_destroy(btree_traverse_stack_cachep);
	if (btree_record_info_cachep)
		kmem_cache_destroy(btree_record_info_cachep);
	if (btree_todo_cachep)
		kmem_cache_destroy(btree_todo_cachep);
	return -ENOMEM;
}

void vdfs2_destroy_btree_caches(void)
{
	kmem_cache_destroy(btree_traverse_stack_cachep);
	kmem_cache_destroy(btree_record_info_cachep);
	kmem_cache_destroy(btree_todo_cachep);
}

static int traverse_stack_push(struct list_head *stack, __u32 bnode_id,
		int pos, int level)
{
	struct vdfs2_btree_traverse_stack *stack_item;

	stack_item = kmem_cache_alloc(btree_traverse_stack_cachep, GFP_KERNEL);
	if (!stack_item)
		return -ENOMEM;

	stack_item->bnode_id = bnode_id;
	stack_item->pos = pos;
	stack_item->level = level;

	list_add(&stack_item->list, stack);

	return 0;
}

static void traverse_stack_destroy(struct list_head *stack)
{
	struct vdfs2_btree_traverse_stack *traverse_data;

	while (!list_empty(stack)) {
		traverse_data = list_first_entry(stack,
				struct vdfs2_btree_traverse_stack, list);
		list_del(&traverse_data->list);
		kmem_cache_free(btree_traverse_stack_cachep, traverse_data);
	}
}

#ifdef USER_SPACE
#ifdef CONFIG_VDFS2_DEBUG
int vdfs2_check_btree_slub_caches_empty(void)
{
	if (btree_traverse_stack_cachep->items_num) {
		VDFS2_ERR("Traverse stack cache has %d items",
				btree_traverse_stack_cachep->items_num);
		return -1;
	}

	if (btree_record_info_cachep->items_num) {
		VDFS2_ERR("Bnode record info cache has %d items",
				btree_record_info_cachep->items_num);
		return -1;
	}

	if (btree_todo_cachep->items_num) {
		VDFS2_ERR("Todo cache has %d items",
				btree_todo_cachep->items_num);
		return -1;
	}
	return 0;
}
#endif
#endif

static inline void put_add_index_work(struct list_head *todo_list_head,
		struct vdfs2_btree_traverse_stack *traverse_data,
		__u32 point_to_bnode_id, __u32 current_root_id)
{
	/* If we fail during adding work - it's ok. This will be
	 * done next time, when somebody find dangling bnode */

	struct vdfs2_btree_todo_list *add_index_work;

	add_index_work = kmem_cache_alloc(btree_todo_cachep, GFP_KERNEL);
	if (!add_index_work)
		return;

	add_index_work->point_to_bnode_id = point_to_bnode_id;
	add_index_work->traverse_data = traverse_data;
	add_index_work->current_root_id = current_root_id;
	list_add(&add_index_work->list, todo_list_head);

}

static struct vdfs2_bnode *btree_traverse_level(struct vdfs2_btree *btree,
		struct vdfs2_generic_key *gen_key,
		__u32 start_bnode_id,
		enum vdfs2_get_bnode_mode mode,
		struct list_head *stack,
		struct list_head *todo_list_head,
		int *pos, int wait)
{
	struct vdfs2_bnode *bnode, *dang_bnode = NULL;
	void *record, *dang_record;
	int dang_pos = -1;
	void *err_ret = NULL;
	__u32 curr_bnode_id = start_bnode_id;

	bnode = vdfs2_get_bnode(btree, curr_bnode_id, mode, wait);
	if (IS_ERR(bnode))
		return bnode;

	record = binary_search(bnode, gen_key, pos);

	if (IS_ERR(record)) {
		err_ret = record;
		goto err_exit;
	}
	/* Check if bnode is dangling */
	while (unlikely(*pos == VDFS2_BNODE_RECS_NR(bnode) - 1 &&
			VDFS2_BNODE_DSCR(bnode)->next_node_id != 0)) {
		/* Last position in the bnode and there is next bnode - need
		 * to check the neighborhood */
		dang_bnode = vdfs2_get_bnode(btree,
				VDFS2_BNODE_DSCR(bnode)->next_node_id,
				bnode->mode, wait);

		if (IS_ERR(dang_bnode)) {
			err_ret = dang_bnode;
			dang_bnode = NULL;
			goto err_exit;
		}

		dang_record = binary_search(dang_bnode, gen_key, &dang_pos);
		if (PTR_ERR(dang_record) == -ENOENT) {
			/* It is not a danging bnode */
			vdfs2_put_bnode(dang_bnode);
			break;
		} else {
			if (IS_ERR(dang_record)) {
				err_ret = dang_record;
				goto err_exit;
			}
#ifndef USER_SPACE
			/* Currently btree is not multithread, dangling bnodes
			 * should not appear. They allowed to appear only in the
			 * btree user space */
			VDFS2_BUG();
#endif
			vdfs2_put_bnode(bnode);
			bnode = dang_bnode;
			dang_bnode = NULL;
			record = dang_record;
			*pos = dang_pos;
			curr_bnode_id = bnode->node_id;

			if (todo_list_head) {
				struct vdfs2_btree_traverse_stack *traverse_data;

				/* If todo stack is present, stack stack has
				 * to be present */
				VDFS2_BUG_ON(!stack);
				if (!list_empty(stack))
					traverse_data = list_first_entry(stack,
							typeof(*traverse_data),
							list);
				else
					traverse_data = NULL;

				put_add_index_work(todo_list_head,
						traverse_data,
						bnode->node_id,
						vdfs2_btree_get_root_id(btree)
						);
			}
		}
	}

	VDFS2_DEBUG_BUG_ON(!record || IS_ERR(record));

	return bnode;

err_exit:
	if (!IS_ERR_OR_NULL(dang_bnode))
		vdfs2_put_bnode(dang_bnode);
	vdfs2_put_bnode(bnode);
	return err_ret;
}

static struct vdfs2_bnode *vdfs2_btree_traverse(struct vdfs2_btree *btree,
		struct vdfs2_generic_key *gen_key,
		enum vdfs2_get_bnode_mode mode, struct list_head *stack,
		struct list_head *todo_list, int *pos, int till_level)
{
	__u32 curr_bnode_id = vdfs2_btree_get_root_id(btree);
	struct vdfs2_bnode *bnode = NULL;
	int level, ret;
	void *err_ret = NULL;



	VDFS2_DEBUG_BUG_ON(till_level < VDFS2_BTREE_LEAF_LVL ||
			till_level > vdfs2_btree_get_height(btree));

	for (level = vdfs2_btree_get_height(btree);
			level >= till_level; level--) {
		bnode = btree_traverse_level(btree, gen_key, curr_bnode_id,
				mode, stack, todo_list, pos,
				VDFS2_WAIT_BNODE_UNLOCK);

		if (IS_ERR(bnode))
			return bnode;

		if (stack) {
			ret = traverse_stack_push(stack, bnode->node_id, *pos,
					level);
			if (ret)
				return ERR_PTR(ret);
		}

		if (level > till_level) {
			void *record = vdfs2_get_btree_record(bnode, *pos);
			struct generic_index_value *index_val;

			if (IS_ERR(record)) {

				VDFS2_DEBUG_BUG_ON(1);

				err_ret = record;
				goto err_exit;
			}

			index_val = get_value_pointer(record);

			if (IS_ERR(index_val)) {
				err_ret = index_val;
				goto err_exit;
			}

			curr_bnode_id = le32_to_cpu(index_val->node_id);
			vdfs2_put_bnode(bnode);
		}

	}

	return bnode;

err_exit:
	if (stack)
		traverse_stack_destroy(stack);
	return err_ret;
}

/**
 * @brief			Splits bnode data to two new bnodes.
 * @param [in|out] left_bnode	Source bnode that is splitted
 * @param [in]	right_bnode	New, empty bnode
 * @param [in]	new_key		This key will be placed into one of the
 *				resulting bnode
 * @param [in]  new_rec_len	Split ignores record len from the key, it uses
 *				this value instead
 * @param [in]  insert_pos	Position in the original bnode, where key was
 *				supposed to be placed
 * @param [out] res_rec_info	resulting record
 * @return			0 - ok, error code in othercase
 */
static int split_bnode(struct vdfs2_bnode *left_bnode,
		struct vdfs2_bnode *right_bnode,
		struct vdfs2_generic_key *new_key, int new_rec_len,
		int insert_pos, struct vdfs2_btree_record_info *res_rec_info)
{
	struct vdfs2_btree *btree = left_bnode->host;
	struct vdfs2_bnode orig_bnode;
	unsigned int used_space;
	int i;
	int part_to_insert = 0;
	int inserted = 0;
	int err = 0;
	__le32 orig_prev, orig_next;

	__u8 bnode_type = VDFS2_BNODE_DSCR(left_bnode)->type;
	int recs_count = VDFS2_BNODE_RECS_NR(left_bnode);

	init_new_node_descr(right_bnode, bnode_type);

	mutex_lock(&btree->split_buff_lock);

	/* Copy original contents of the left_bnode,
	 * before it will be rewritten */
	memcpy(&orig_bnode, left_bnode, sizeof(orig_bnode));
	orig_bnode.data = btree->split_buff;
	memcpy(orig_bnode.data, left_bnode->data, btree->node_size_bytes);

	/* Now we can do with left_bnode everything we want.
	 * Let's start from reinitialization */
	init_new_node_descr(left_bnode, bnode_type);

	used_space = btree->node_size_bytes -
			VDFS2_BNODE_DSCR(left_bnode)->free_space;
	VDFS2_BUG_ON(recs_count == 0);

	i = 0;
	while (i < recs_count || !inserted) {
		struct vdfs2_bnode *ins_bnode;
		int new_rec_select = (i == insert_pos && !inserted);

		struct vdfs2_generic_key *key = (struct vdfs2_generic_key *) (
				(new_rec_select) ?
				new_key : get_record(&orig_bnode, i));

		if (IS_ERR(key) || !key) {
			if (key == NULL)
				err = -EFAULT;
			else
				err = PTR_ERR(key);
			goto restore_orig_bnode;
		}

		/* possible wrong key isn't checked*/
		used_space += key->record_len + sizeof(vdfs2_bt_off_t);


		/* TODO: we need any rounding here - select the neares to
		 * the node middle split bound */
		ins_bnode = ((part_to_insert) ? right_bnode : left_bnode);

		if (new_rec_select)
			err = insert_key_into_bnode(ins_bnode, key, new_rec_len,
					VDFS2_BNODE_RECS_NR(ins_bnode),
					res_rec_info);
		else
			err = append_bnode(ins_bnode, key);

		if (err)
			goto restore_orig_bnode;

		if (used_space > btree->node_size_bytes / 2)
			part_to_insert = 1;

		if (!new_rec_select)
			++i;
		else
			inserted = 1;
	}

	orig_prev = VDFS2_BNODE_DSCR(&orig_bnode)->prev_node_id;
	orig_next = VDFS2_BNODE_DSCR(&orig_bnode)->next_node_id;
	orig_bnode.data = NULL;

	{
		struct vdfs2_gen_node_descr *left_descr, *right_descr;

		left_descr = VDFS2_BNODE_DSCR(left_bnode);
		right_descr = VDFS2_BNODE_DSCR(right_bnode);

		left_descr->prev_node_id = orig_prev;
		left_descr->next_node_id = right_descr->node_id;
		right_descr->prev_node_id = left_descr->node_id;
		right_descr->next_node_id = orig_next;

		if (le32_to_cpu(orig_next) != VDFS2_INVALID_NODE_ID) {
			struct vdfs2_bnode *next_right_bnode;

			next_right_bnode = __vdfs2_get_bnode(btree,
				le32_to_cpu(orig_next), VDFS2_BNODE_MODE_RW);
			if (IS_ERR(next_right_bnode))
				goto restore_orig_bnode;
			VDFS2_BNODE_DSCR(next_right_bnode)->prev_node_id =
				right_descr->node_id;
			vdfs2_mark_bnode_dirty(next_right_bnode);
			vdfs2_put_bnode(next_right_bnode);
		}
	}
	mutex_unlock(&btree->split_buff_lock);
	return 0;

restore_orig_bnode:
	VDFS2_ERR("unable to split bnode %d - recovering original state",
			left_bnode->node_id);
	orig_bnode.data = left_bnode->data;
	memcpy(left_bnode, &orig_bnode, sizeof(orig_bnode));
	memcpy(left_bnode->data, btree->split_buff, btree->node_size_bytes);
	mutex_unlock(&btree->split_buff_lock);
	return err;
}

#ifdef USER_SPACE
#define vdfs2_check_bnode_reserve(btree) (0)
#endif

static struct vdfs2_btree_gen_record *place_key_in_bnode(
		struct vdfs2_btree_traverse_stack *traverse_data,
		struct vdfs2_bnode **__bnode, int pos,
		struct vdfs2_generic_key *gen_key, int new_rec_len,
		struct list_head *todo_list_head)
{
	void *raw_record;
	int ret = 0;
	struct vdfs2_btree_record_info *rec_info;
	struct vdfs2_bnode *bnode = *__bnode;
	struct vdfs2_btree *btree = bnode->host;
	struct vdfs2_bnode  *right_bnode;
	void *err_ret;

	rec_info = kmem_cache_alloc(btree_record_info_cachep, GFP_KERNEL);
	if (!rec_info)
		return ERR_PTR(-ENOMEM);

	raw_record = get_record(bnode, pos);
	if (IS_ERR(raw_record)) {
		err_ret = raw_record;
		goto err_exit;
	}

	/* Check if adding record already exists */
	if (btree->comp_fn(raw_record, gen_key) == 0) {
		err_ret = ERR_PTR(-EEXIST);
		goto err_exit;
	}

	if (is_node_full(bnode, gen_key)) {
		ret = vdfs2_check_bnode_reserve(btree);
		if (ret) {
			err_ret = ERR_PTR(ret);
			goto err_exit;
		}

		right_bnode = vdfs2_alloc_new_bnode(btree);
		if (IS_ERR(right_bnode)) {
			err_ret = right_bnode;
			goto err_exit;
		}
		ret = split_bnode(bnode, right_bnode, gen_key, new_rec_len,
				pos + 1, rec_info);

		vdfs2_mark_bnode_dirty(bnode);
		vdfs2_mark_bnode_dirty(right_bnode);
		if (ret) {
			vdfs2_destroy_bnode(right_bnode);
			err_ret = ERR_PTR(ret);
			goto err_exit;
		}

		if (traverse_data->level == vdfs2_btree_get_height(btree))
			traverse_data = NULL;
		else
			traverse_data = list_entry(traverse_data->list.next,
					typeof(*traverse_data), list);

		put_add_index_work(todo_list_head, traverse_data,
				right_bnode->node_id,
				vdfs2_btree_get_root_id(btree));

		if (rec_info->rec_pos.bnode == bnode) {
			/* Result bnode has not changed */
			vdfs2_put_bnode(right_bnode);
		} else {
			/* Result record placed in the new bnode -
			 * need to change resulting bnode */
			vdfs2_put_bnode(bnode);
			*__bnode = rec_info->rec_pos.bnode;
		}
	} else {
		ret = insert_key_into_bnode(bnode, gen_key, new_rec_len,
				pos + 1, rec_info);
		vdfs2_mark_bnode_dirty(bnode);
		if (ret) {
			err_ret = ERR_PTR(ret);
			goto err_exit;
		}
	}


	return &rec_info->gen_record;
err_exit:
	kmem_cache_free(btree_record_info_cachep, rec_info);
	return err_ret;
}

static int build_new_root(struct vdfs2_btree *btree, __u32 current_root_id)
{
	struct vdfs2_bnode *new_root;
	struct vdfs2_bnode *curr_bnode, *next_bnode;
	struct vdfs2_btree_record_info *rec_info;
	int err = 0;
	int pos = 0;
	int new_rec_len = 0;

	if (current_root_id != vdfs2_btree_get_root_id(btree))
		/* Seems that somebody already created new root. Do nothing */
		return 0;

	rec_info = kmem_cache_alloc(btree_record_info_cachep, GFP_KERNEL);
	if (!rec_info)
		return -ENOMEM;

	new_root = vdfs2_alloc_new_bnode(btree);
	if (IS_ERR(new_root)) {
		kmem_cache_free(btree_record_info_cachep, rec_info);
		return PTR_ERR(new_root);
	}
	init_new_node_descr(new_root, VDFS2_NODE_INDEX);

	curr_bnode = vdfs2_get_bnode(btree, vdfs2_btree_get_root_id(btree),
			VDFS2_BNODE_MODE_RO, VDFS2_NOWAIT_BNODE_UNLOCK);

	if (IS_ERR(curr_bnode)) {
		err = PTR_ERR(curr_bnode);
		goto err_exit;
	}

	if (VDFS2_NEXT_BNODE_ID(curr_bnode) == VDFS2_INVALID_NODE_ID) {
		/* There is no any dangling bnodes at root level - possibly
		 * somebody already created new root. It is not a error */
		err = 0;
		goto err_exit;
	}

	while (curr_bnode) {
		struct vdfs2_generic_key *key;
		struct generic_index_value *index_val;

		if (IS_ERR(curr_bnode)) {
			err = PTR_ERR(curr_bnode);
			goto err_exit;
		}

		key = vdfs2_get_btree_record(curr_bnode, 0);
		if (IS_ERR(key)) {
			vdfs2_put_bnode(curr_bnode);
			err = PTR_ERR(key);
			goto err_exit;
		}

		new_rec_len = VDFS2_KEY_LEN(key) + sizeof(*index_val);

		if (is_node_full(new_root, key)) {
			/* There is too many dangling bnodes at root level,
			 * and indexes to them does not fit into one bnode */
			vdfs2_put_bnode(curr_bnode);
			break;
		}
		err = insert_key_into_bnode(new_root, key, new_rec_len,
				pos++, rec_info);

		if (err)
			goto err_exit;

		index_val = rec_info->gen_record.val;
		index_val->node_id = cpu_to_le32(curr_bnode->node_id);

		next_bnode =  __vdfs2_get_next_bnode(curr_bnode,
				VDFS2_NOWAIT_BNODE_UNLOCK);
		vdfs2_put_bnode(curr_bnode);

		if (IS_ERR(next_bnode)) {
			if (PTR_ERR(next_bnode) == -ENOENT)
				curr_bnode = NULL;
			else {
				err = PTR_ERR(next_bnode);
				goto err_exit;
			}
		} else
			curr_bnode = next_bnode;
	}

	kmem_cache_free(btree_record_info_cachep, rec_info);

	vdfs2_btree_inc_height(btree);
	vdfs2_btree_set_root_id(btree, new_root->node_id);

	vdfs2_mark_bnode_dirty(new_root);
	vdfs2_mark_bnode_dirty(btree->head_bnode);
	vdfs2_put_bnode(new_root);

	return 0;

err_exit:
	kmem_cache_free(btree_record_info_cachep, rec_info);
	vdfs2_destroy_bnode(new_root);
	return err;
}

static int place_index_record(struct vdfs2_btree *btree,
		struct vdfs2_btree_traverse_stack *traverse_data,
		__u32 point_to_bnode_id, __u32 current_root_id,
		struct list_head *todo_list_head)
{
	struct vdfs2_generic_key *gen_key;
	struct vdfs2_bnode *bnode;
	struct vdfs2_btree_gen_record *gen_record;
	struct generic_index_value *index_val;
	int pos;
	int new_rec_len;
	struct vdfs2_bnode *point_to_bnode;

	if (!traverse_data) {
		/* New root is needed */
		return build_new_root(btree, current_root_id);
	}

	point_to_bnode = vdfs2_get_bnode(btree, point_to_bnode_id,
			VDFS2_BNODE_MODE_RO, VDFS2_NOWAIT_BNODE_UNLOCK);
	if (IS_ERR(point_to_bnode))
		return PTR_ERR(point_to_bnode);

	gen_key = vdfs2_get_btree_record(point_to_bnode, 0);
	if (IS_ERR(gen_key)) {
		vdfs2_put_bnode(point_to_bnode);
		return PTR_ERR(gen_key);
	}

	bnode = btree_traverse_level(btree, gen_key,
			traverse_data->bnode_id, VDFS2_BNODE_MODE_RW,
			NULL, NULL, &pos, VDFS2_NOWAIT_BNODE_UNLOCK);
	if (IS_ERR(bnode)) {
		VDFS2_ERR("can not insert index");
		vdfs2_put_bnode(point_to_bnode);
		return PTR_ERR(bnode);
	}

	new_rec_len = VDFS2_KEY_LEN(gen_key) + sizeof(*index_val);
	gen_record = place_key_in_bnode(traverse_data, &bnode,
			pos, gen_key, new_rec_len, todo_list_head);
	vdfs2_put_bnode(point_to_bnode);
	if (IS_ERR(gen_record)) {
		vdfs2_put_bnode(bnode);
		return PTR_ERR(gen_record);
	}

	index_val = gen_record->val;
	index_val->node_id = cpu_to_le32(point_to_bnode_id);

	vdfs2_release_dirty_record(gen_record);

	return 0;
}

/* This function exists only for backward compatibility with old remove
 * algorithm. At the end, the only function vdfs2_btree_place_data is needed */
static struct vdfs2_btree_gen_record *place_data_at_level(
		struct vdfs2_btree *btree, struct vdfs2_generic_key *gen_key,
		int level)
{
	struct vdfs2_bnode *bnode;
	struct vdfs2_btree_gen_record *record;
	struct vdfs2_btree_traverse_stack *traverse_data;
	int pos;
	LIST_HEAD(traverse_stack);
	LIST_HEAD(todo_list);

	bnode = vdfs2_btree_traverse(btree, gen_key, VDFS2_BNODE_MODE_RW,
			 &traverse_stack, &todo_list, &pos, level);
	if (IS_ERR(bnode))
		return (void *) bnode;

	VDFS2_DEBUG_BUG_ON(list_empty(&traverse_stack));

	traverse_data = list_first_entry(&traverse_stack,
			struct vdfs2_btree_traverse_stack, list);

	record = place_key_in_bnode(traverse_data, &bnode, pos, gen_key,
			VDFS2_RECORD_LEN(gen_key), &todo_list);
	if (IS_ERR(record))
		vdfs2_put_bnode(bnode);

	while (!list_empty(&todo_list)) {
		int err;
		struct vdfs2_btree_todo_list *curr_task;

		curr_task = list_first_entry(&todo_list,
					struct vdfs2_btree_todo_list, list);

		list_del(&curr_task->list);

		err = place_index_record(btree, curr_task->traverse_data,
				curr_task->point_to_bnode_id,
				curr_task->current_root_id, &todo_list);
		if (err)
			VDFS2_DEBUG_BTREE("can not insert index");

		kmem_cache_free(btree_todo_cachep, curr_task);
	}

	/* Destroy traverse stack */
	traverse_stack_destroy(&traverse_stack);

	return record;
}

struct vdfs2_btree_gen_record *vdfs2_btree_place_data(struct vdfs2_btree *btree,
		struct vdfs2_generic_key *gen_key)
{
	return place_data_at_level(btree, gen_key, VDFS2_BTREE_LEAF_LVL);
}

/* Full replacement for vdfs2_btree_find */
struct vdfs2_btree_gen_record *vdfs2_btree_find(struct vdfs2_btree *btree,
		struct vdfs2_generic_key *key, enum vdfs2_get_bnode_mode mode)
{
	struct vdfs2_bnode  *bnode;
	void *err_ret = NULL;
	void *raw_record;
	struct vdfs2_btree_record_info *rec_info;
	int pos = 0;

	rec_info = kmem_cache_alloc(btree_record_info_cachep, GFP_KERNEL);
	if (!rec_info)
		return ERR_PTR(-ENOMEM);

	bnode = vdfs2_btree_traverse(btree, key, mode, NULL, NULL, &pos,
			VDFS2_BTREE_LEAF_LVL);
	if (IS_ERR(bnode)) {
		err_ret = bnode;
		goto err_exit;
	}

	raw_record = vdfs2_get_btree_record(bnode, pos);
	if (IS_ERR(raw_record)) {
		VDFS2_ERR("Unable to get record%d from bnode#%d",
				pos, bnode->node_id);
		err_ret = raw_record;
		goto err_exit_put;
	}

	if (form_gen_record(bnode, raw_record, rec_info, pos)) {
		err_ret =  ERR_PTR(-EINVAL);
		goto err_exit_put;
	}

	return &rec_info->gen_record;

err_exit_put:
	vdfs2_put_bnode(bnode);
err_exit:
	kmem_cache_free(btree_record_info_cachep, rec_info);
	return err_ret;

}

/**
 * @brief		!!Depricated!!!
 *			Temporary translation from old algorithms to new
 *			Finds data in the B-tree.
 * @param [in]	tree	The B-tree to search in
 * @param [in]	key	Key descriptor to search
 * @param [in]	level	B-tree level (0 - root) to start the search
 * @param [out] pos	Saves position into bnode
 * @param [out] path	If specified saves the bnodes passed and index in them
 * @param [in]	mode	Gets the bnode in specified mode
 * @return		Returns bnode containing specified record or error
 *			if occurred
 */
static struct vdfs2_bnode *btree_find(struct vdfs2_btree *btree,
				struct vdfs2_generic_key *key,
				int level, int *pos,
				enum vdfs2_get_bnode_mode mode)
{
	return vdfs2_btree_traverse(btree, key, mode, NULL, NULL, pos,
			level);
}


/**
 * @brief		Interface for finding data in the whole tree.
 * @param [in]	tree	B-tree to search in
 * @param [in]	key	Key descriptor to search
 * @param [out] pos	Saves position into bnode
 * @param [in]	mode	Gets the bnode in specified mode
 * @return		Returns bnode containing specified record
 *			or error if occurred
 */
/* NOTE: deprecated function. Use vdfs2_btree_find instead */
struct vdfs2_bnode *__vdfs2_btree_find(struct vdfs2_btree *tree,
				struct vdfs2_generic_key *key,
				int *pos, enum vdfs2_get_bnode_mode mode)
{
	struct vdfs2_bnode  *ret;

	ret = btree_find(tree, key, VDFS2_BTREE_LEAF_LVL, pos, mode);
	return ret;
}

/**
 * @brief			DEPRECATED!!!
 *				Nothing more then translation from old algorithm
 *				to new one. It lives here until remove is
 *				rewritten.
 *
 *				Inserts record in specified tree at specified
 *				level.
 * @param [in]	btree		B-tree to insert to
 * @param [in]	new_data	Pointer to record to be inserted
 * @param [in]	level		Level to start inserting
 * @return			Returns 0 if success, error code if fail
 */
static int btree_insert_at_level(struct vdfs2_btree *btree,
		void *new_data, int level)
{
	struct vdfs2_generic_key *key = new_data;
	struct vdfs2_btree_gen_record *record;
	int val_len;

	record = place_data_at_level(btree, key, level);
	if (IS_ERR(record))
		return PTR_ERR(record);

	val_len = le32_to_cpu(key->record_len) - le32_to_cpu(key->key_len);

	memcpy(record->val, get_value_pointer(new_data), val_len);

	vdfs2_release_record(record);

	return 0;
}

/**
 *		!!!DEPRECATED!!!
 * @brief			Interface for simple insertion into tree.
 * @param [in]	btree		B-tree to insert to
 * @param [in]	new_data	Pointer to record to be inserted
 * @return			Returns 0 if success, error code if fail
 */
int vdfs2_btree_insert(struct vdfs2_btree *btree, void *new_data)
{
	int ret;

	ret = btree_insert_at_level(btree, new_data, VDFS2_BTREE_LEAF_LVL);
	return ret;
}


/**
 * @brief			Changes key on upper level if first key is
 *				removed.
 * @param [in]	tree		B-tree update
 * @param [in]	key		Key to be removed from specified level
 * @param [in]	point_to	The bnode from which to take new key
 * @param [in]	level		Level where to change key
 * @return			Returns pointer to new key or error if fail
 */
static struct vdfs2_generic_key *btree_update_key(
		struct vdfs2_btree *tree,
		struct vdfs2_generic_key *key,
		struct vdfs2_bnode *point_to, int level)
{
	struct vdfs2_generic_key *found_key;
	struct vdfs2_bnode *bnode;
	void *new_data;
	struct vdfs2_generic_key *ret;
	int err = 0;
	int pos;

	new_data = kzalloc(tree->max_record_len, GFP_KERNEL);
	if (!new_data)
		return ERR_PTR(-ENOMEM);


	form_new_adding_record(point_to, new_data);

	err = btree_insert_at_level(tree, new_data, level);
	if (err)
		goto error_exit;

	bnode = btree_find(tree, key, level, &pos, VDFS2_BNODE_MODE_RW);
	if (IS_ERR(bnode)) {
		err = -EINVAL;
		goto error_exit;
	}

	found_key = vdfs2_get_btree_record(bnode, pos);
	if (IS_ERR(found_key)) {
		err = PTR_ERR(found_key);
		goto error_exit;
	}
	if (tree->comp_fn(key, found_key))
		/* We have faced dangling bnode, nothing to remove at the
		 * index level */
		goto exit;


	err = delete_from_node(bnode, pos);
	if (err) {
		vdfs2_put_bnode(bnode);
		goto error_exit;
	}

	if (pos == 0 && level < vdfs2_btree_get_height(tree)) {
		ret = btree_update_key(tree, key, bnode, level + 1);
		if (IS_ERR(ret))
			err = PTR_ERR(ret);
		else
			kfree(ret);
	}


exit:
	vdfs2_put_bnode(bnode);

	if (err)
		goto error_exit;

	return new_data;

error_exit:
	kfree(new_data);
	new_data = NULL;
	return ERR_PTR(err);
}

/**
 * @brief			Merges data from right bnode to left bnode.
 * @param [in]	tree		B-tree to be updated
 * @param [in]	parent_bnode_id	Parent bnode id for both left and right bnode
 * @param [in]	left_bnode_id	Id of left (receiving) bnode
 * @param [in]	right_bnode_id	Id of right (giving back) bnode
 * @param [in]	level		Level where bnodes are merged
 * @return			Returns the error code
 */
static int btree_merge(struct vdfs2_btree *tree, int left_bnode_id,
		int right_bnode_id, int level, int is_right_dangling)
{
	struct vdfs2_bnode *left_bnode = NULL, *right_bnode = NULL;
	struct vdfs2_gen_node_descr *left = NULL, *right = NULL;
	void *data_start;
	int i;
	vdfs2_bt_off_t start_offset;
	vdfs2_bt_off_t *l_free_space_addr;
	void *left_free_space;
	int err = 0;

	left_bnode   = __vdfs2_get_bnode(tree, left_bnode_id,
			VDFS2_BNODE_MODE_RW);
	if (IS_ERR(left_bnode))
		return PTR_ERR(left_bnode);

	right_bnode  = __vdfs2_get_bnode(tree, right_bnode_id,
			VDFS2_BNODE_MODE_RW);
	if (IS_ERR(right_bnode)) {
		vdfs2_put_bnode(left_bnode);
		return PTR_ERR(right_bnode);
	}

	if (check_bnode_offset_area(left_bnode) ||
			check_bnode_offset_area(right_bnode)) {
		if (!is_sbi_flag_set(tree->sbi, IS_MOUNT_FINISHED)) {
			vdfs2_put_bnode(right_bnode);
			vdfs2_put_bnode(left_bnode);
			return -EFAULT;
		} else
			BUG();
	}

	left   = VDFS2_BNODE_DSCR(left_bnode);
	right  = VDFS2_BNODE_DSCR(right_bnode);

	data_start = (void *)right + sizeof(struct vdfs2_gen_node_descr);
	l_free_space_addr = vdfs2_get_offset_addr(left_bnode, left->recs_count);
	if (IS_ERR(l_free_space_addr)) {
		vdfs2_put_bnode(left_bnode);
		vdfs2_put_bnode(right_bnode);
		return PTR_ERR(l_free_space_addr);
	}

	left_free_space = (void *)left + *l_free_space_addr;


	memmove(left_free_space, data_start,
				vdfs2_get_offset(right_bnode, right->recs_count) -
				sizeof(struct vdfs2_gen_node_descr));

	start_offset = vdfs2_get_offset(left_bnode, left->recs_count);
	for (i = 0; i <= right->recs_count; i++) {
		unsigned int record_index = i + left->recs_count;
		vdfs2_bt_off_t record_offset = start_offset +
			vdfs2_get_offset(right_bnode, i) -
			sizeof(struct vdfs2_gen_node_descr);

		if (check_set_offset(left_bnode, record_index,
					record_offset)) {
			if (!is_sbi_flag_set(tree->sbi, IS_MOUNT_FINISHED))
				return -EFAULT;
			else
				VDFS2_BUG();
		}
	}

	left->free_space -= (vdfs2_get_offset(right_bnode, right->recs_count) -
					sizeof(struct vdfs2_gen_node_descr)) +
					right->recs_count *
					sizeof(vdfs2_bt_off_t);

	left->recs_count = left->recs_count + right->recs_count;
	left->next_node_id = right->next_node_id;

	if (right->next_node_id != VDFS2_INVALID_NODE_ID) {
		struct vdfs2_bnode *next_bnode =
				__vdfs2_get_bnode(tree, right->next_node_id,
						VDFS2_BNODE_MODE_RW);
		if (IS_ERR(next_bnode)) {
			vdfs2_put_bnode(left_bnode);
			vdfs2_put_bnode(right_bnode);
			return PTR_ERR(right_bnode);
		}
		VDFS2_BNODE_DSCR(next_bnode)->prev_node_id = left->node_id;
		left->next_node_id = next_bnode->node_id;

		vdfs2_mark_bnode_dirty(next_bnode);
		vdfs2_put_bnode(next_bnode);
	}

	vdfs2_mark_bnode_dirty(left_bnode);
	vdfs2_put_bnode(left_bnode);

	if (!is_right_dangling) {
		struct vdfs2_generic_key *key = NULL;
		/* possible bad offset */
		void *record = get_record(right_bnode, 0);

		if (IS_ERR(record))
			return PTR_ERR(record);

		key = kzalloc(((struct vdfs2_generic_key *)record)->key_len,
				GFP_KERNEL);
		if (!key)
			return -ENOMEM;

		memcpy(key, record,
		 ((struct vdfs2_generic_key *)record)->key_len);

		key->record_len = key->key_len +
			sizeof(struct generic_index_value);
		err = __vdfs2_btree_remove(tree, key, level+1);

		kfree(key);
		key = NULL;

		if (err)
			return err;
	}

	err = vdfs2_destroy_bnode(right_bnode);

	return err;
}

static int is_bnode_dangling(struct vdfs2_bnode *parent, int pos,
		__u32 bnode_id)
{
	void *record_ptr;
	struct generic_index_value *value;

	record_ptr = get_record(parent, pos);
	value = get_value_pointer(record_ptr);

	if (le32_to_cpu(value->node_id) == bnode_id)
		return 0;
	else
		return 1;
}

/**
 * @brief				Determines if the tree needs to be
 *					balanced and "victims" for this.
 * @param [in]	tree			B-tree to be updated
 * @param [in]	key			Key descriptor which was removed
 * @param [in]	level			Level at which balancing should be done
 * @param [in]	bnode_id		The bnode id from which key was removed
 * @param [in]	bnode_free_space	Free space in bnode after deletion
 * @return	err val
 */
static int rebalance(struct vdfs2_btree *tree, struct vdfs2_generic_key *key,
		int level, int bnode_id, unsigned int bnode_free_space)
{
	unsigned int neigh_free_space, neigh_id;
	unsigned int left_id, right_id;
	struct vdfs2_bnode *neigh = NULL;
	struct vdfs2_bnode *parent = NULL;
	struct vdfs2_bnode *cur = NULL;
	int pos, err = 0;
	int is_right_dangling;
	int merge_with_prev;

	cur = __vdfs2_get_bnode(tree, bnode_id, VDFS2_BNODE_MODE_RW);
	if (IS_ERR(cur))
		return PTR_ERR(cur);

	/* Step1: Choose the direction of the merge */
	if (level == vdfs2_btree_get_height(tree)) {
		/* We are facing root level dangling bnodes */
		if (VDFS2_PREV_BNODE_ID(cur) != VDFS2_INVALID_NODE_ID)
			merge_with_prev = 1;
		else if (VDFS2_NEXT_BNODE_ID(cur) != VDFS2_INVALID_NODE_ID)
			merge_with_prev = 0;
		else
			/* There is no neighborhoods, do nothing */
			goto exit;

		is_right_dangling = 1;
	} else {
		parent = btree_find(tree, key, level + 1, &pos,
				VDFS2_BNODE_MODE_RW);
		if (IS_ERR(parent)) {
			err = PTR_ERR(parent);
			goto exit;
		}

		/* if it's not first element */
		if (pos > 0) {
			merge_with_prev = 1;
			is_right_dangling = is_bnode_dangling(parent, pos,
					bnode_id);
		/* if it's not last element */
		} else if (pos < VDFS2_BNODE_DSCR(parent)->recs_count-1) {
			merge_with_prev = 0;
			is_right_dangling = is_bnode_dangling(parent, pos + 1,
					VDFS2_NEXT_BNODE_ID(cur));
		/* parent have only record, do nothing */
		} else {
			vdfs2_put_bnode(parent);
			goto exit;
		}

		vdfs2_put_bnode(parent);
	}

	/* Step 2: choose left and right bnodes, basing on the
	 * merge direction */
	if (merge_with_prev) {
		/* merge current with previous node */
		neigh_id = VDFS2_BNODE_DSCR(cur)->prev_node_id;
		left_id = neigh_id;
		right_id = VDFS2_BNODE_DSCR(cur)->node_id;
	} else {
		/* merge current with next node */
		neigh_id = VDFS2_BNODE_DSCR(cur)->next_node_id;
		left_id = VDFS2_BNODE_DSCR(cur)->node_id;
		right_id = neigh_id;
	}

	neigh = __vdfs2_get_bnode(tree, neigh_id, VDFS2_BNODE_MODE_RW);
	if (IS_ERR(neigh)) {
		err = PTR_ERR(neigh);
		goto exit;
	}

	neigh_free_space = ((struct vdfs2_gen_node_descr *)
			neigh->data)->free_space;
	vdfs2_put_bnode(neigh);

	if (bnode_free_space + neigh_free_space >=
			tree->node_size_bytes) {
		vdfs2_put_bnode(cur);
		return btree_merge(tree, left_id, right_id, level,
				is_right_dangling);
	}

exit:
	vdfs2_put_bnode(cur);
	return err;
}

/**
 * @brief		Removes the specified key at level.
 * @param [in]	tree	B-tree to update
 * @param [in]	key	Key descriptor to remove
 * @param [in]	level	Level at which key should be searched
 * @return		Returns 0 in case of success, error code otherwise
 */
static int __vdfs2_btree_remove(struct vdfs2_btree *tree,
		struct vdfs2_generic_key *key, int level)
{
	struct vdfs2_bnode *bnode;
	struct vdfs2_gen_node_descr *node;
	struct vdfs2_generic_key *new_key = NULL;

	void *record;
	int pos;
	int err = 0;

	VDFS2_BUG_ON(!tree->comp_fn);

	/*
	 * TODO: something strange is happening here
	 * this __FUNCTION__ replaced with BUG_ON
	if (level > tree->btree_height) {
		tree->btree_height = 0;
		tree->root_bnode_id = -1;
		return 0;
	}
	*/
	VDFS2_BUG_ON(level > vdfs2_btree_get_height(tree));

	bnode = btree_find(tree, key, level, &pos, VDFS2_BNODE_MODE_RW);
	if (IS_ERR(bnode))
		return PTR_ERR(bnode);

	node = bnode->data;
	if (!node) {
		err = -EINVAL;
		goto exit_put_bnode;
	}

	record = get_record(bnode, pos);
	if (IS_ERR(record)) {
		err = PTR_ERR(record);
		goto exit_put_bnode;
	}

	if (tree->comp_fn(key, record)) {
		/* There are 2 possibilities why this happened:
		 * 1) We have faced dangling bnode, nothing to remove at the
		 *      index level
		 * 2) Somebody trying to remove key which is not present in
		 *      the btree
		 */
		err = -ENOENT;
		goto exit_put_bnode;
	}

	err = delete_from_node(bnode, pos);
	if (err)
		goto exit_put_bnode;

	if (node->recs_count == 0) {
		/* Bnode is empty, before removing it update prev and next
		 * pointers of neighborhoods */
		if (node->prev_node_id != VDFS2_INVALID_NODE_ID) {
			struct vdfs2_bnode *prev_bnode =
				__vdfs2_get_bnode(tree, node->prev_node_id,
						VDFS2_BNODE_MODE_RW);

			/* TODO: error path */
			VDFS2_BUG_ON(IS_ERR(prev_bnode));

			VDFS2_BNODE_DSCR(prev_bnode)->next_node_id =
				node->next_node_id;

			vdfs2_mark_bnode_dirty(prev_bnode);
			vdfs2_put_bnode(prev_bnode);
		}

		if (node->next_node_id != VDFS2_INVALID_NODE_ID) {
			struct vdfs2_bnode *next_bnode =
				__vdfs2_get_bnode(tree, node->next_node_id,
						VDFS2_BNODE_MODE_RW);

			/* TODO: error path */
			VDFS2_BUG_ON(IS_ERR(next_bnode));

			VDFS2_BNODE_DSCR(next_bnode)->prev_node_id =
				node->prev_node_id;

			vdfs2_mark_bnode_dirty(next_bnode);
			vdfs2_put_bnode(next_bnode);
		}

		err = vdfs2_destroy_bnode(bnode);
		if (err) {
			if (!is_sbi_flag_set(bnode->host->sbi,
						IS_MOUNT_FINISHED))
				return err;
			else
				VDFS2_BUG();
		}

		VDFS2_BUG_ON(bnode->node_id ==
				vdfs2_btree_get_root_id(tree));

		if (level != vdfs2_btree_get_height(tree)) {
			err =  __vdfs2_btree_remove(tree, key, level + 1);
			if (err == -ENOENT)
				/* This means we just removed dangling bnode -
				 * there is no record in the parent bnode,
				 * pointing to this bnode
				 */
				return 0;
			else
				return err;
		} else
			return 0;
	} else if ((pos == 0) && ((level < vdfs2_btree_get_height(tree)))) {
		/* Left position of bnode have been changed, so index record,
		 * pointing to current bnode, is outdated. Update it */
		new_key = btree_update_key(tree, key, bnode, level + 1);

		if (IS_ERR(new_key)) {
			err = PTR_ERR(new_key);
			goto exit_put_bnode;
		}

		key = new_key;
	}


	if (node->free_space > tree->node_size_bytes / 3) {
		if (bnode->node_id != vdfs2_btree_get_root_id(tree)) {
			/* It is NOT a root bnode */
			int node_id = bnode->node_id,
			    node_fs = ((struct vdfs2_gen_node_descr *)
				bnode->data)->free_space;
			vdfs2_put_bnode(bnode);
			err = rebalance(tree, key, level, node_id, node_fs);
			if (err && is_sbi_flag_set(tree->sbi,
						IS_MOUNT_FINISHED))
				BUG();
			goto exit;
		} else if (node->recs_count == 1) {
			/* It is a root bnode and now it contains the only
			 * pointer. If we are here we have to decrease btree
			 * height */
			struct vdfs2_bnode *bnode_n;
			if (vdfs2_btree_get_height(tree) <= 1)
				goto exit_put_bnode;

			/* The only child of current root, will be the
			 * new root*/
			bnode_n = get_child_bnode(tree, get_record(bnode, 0),
					VDFS2_BNODE_MODE_RW);
			if (IS_ERR(bnode_n)) {
				err = PTR_ERR(bnode_n);
				goto exit_put_bnode;
			}

			vdfs2_btree_set_root_id(tree, bnode_n->node_id);
			vdfs2_btree_dec_height(tree);
			vdfs2_put_bnode(bnode_n);

			/* Old root is free now */
			err = vdfs2_destroy_bnode(bnode);
			goto exit;
		}
	}

exit_put_bnode:
	vdfs2_put_bnode(bnode);

exit:
	if (!IS_ERR(new_key))
		kfree(new_key);
	new_key = NULL;
	return err;
}

/**
 * @brief		Interface for key removal.
 * @param [in]	tree	B-tree to be update
 * @param [in]	key	Key descriptor to be removed
 * @return		Returns 0 in case of success, error code otherwise.
 */
int vdfs2_btree_remove(struct vdfs2_btree *tree,
			struct vdfs2_generic_key *key)
{
	int ret;
	ret = __vdfs2_btree_remove(tree, key, 1);
	return ret;
}

static int check_ordering_at_level(struct vdfs2_bnode *bnode)
{
	int ret = 0, cmp_res = 0, check_ret = 0;
	void *raw_record = NULL;
	struct vdfs2_btree_gen_record old_record, *new_record;
	struct vdfs2_btree_record_info *rec_info = NULL;
	vdfs2_btree_key_cmp *comp_fn = NULL;

	if (IS_ERR(bnode)) {
		VDFS2_ERR("wrong bnode");
		return -EINVAL;
	}

	comp_fn = bnode->host->comp_fn;
	rec_info = kmem_cache_alloc(btree_record_info_cachep, GFP_KERNEL);
	if (!rec_info)
		return -ENOMEM;

	new_record = &rec_info->gen_record;
	raw_record = vdfs2_get_btree_record(bnode, 0);
	rec_info->gen_record.key = raw_record;
	rec_info->rec_pos.bnode = bnode;
	rec_info->rec_pos.pos = 0;

	if (IS_ERR(raw_record)) {
		VDFS2_ERR("can't get record\n");
		vdfs2_release_record(new_record);
		return -EINVAL;
	}

	while (true) {
		old_record = *new_record;
		ret = vdfs2_get_next_btree_record(new_record);
		if (ret < 0) {
			vdfs2_release_record(new_record);
			if (ret == -ENOENT)
				return check_ret;
			else {
				VDFS2_ERR("can't get record\n");
				return -EINVAL;
			}
		}
		cmp_res = comp_fn((struct vdfs2_generic_key *) old_record.key,
			(struct vdfs2_generic_key *) new_record->key);

		if (cmp_res > 0) {
			VDFS2_ERR("Key values aren't in increasing order\n"
			"bnode id %d\n", bnode->node_id);
			check_ret = -EINVAL;
		}
	}
}

static int check_btree_links_at_level(struct vdfs2_bnode *start_bnode)
{
	struct vdfs2_btree_record_info *rec_info;
	struct vdfs2_btree *btree = start_bnode->host;
	struct vdfs2_btree_gen_record *record;
	void *raw_record;
	struct generic_index_value *index_val;
	struct vdfs2_bnode *bnode = start_bnode;
	__u32 prev, curr, next;
	int total_dang_num = 0;

	if (IS_ERR(bnode)) {
		VDFS2_ERR("wrong bnode");
		goto exit_no_record;
	}

	rec_info = kmem_cache_alloc(btree_record_info_cachep, GFP_KERNEL);
	if (IS_ERR(rec_info))
		goto exit_no_record;
	record = &rec_info->gen_record;
	raw_record = vdfs2_get_btree_record(bnode, 0);

	if (IS_ERR(raw_record))
		goto err_exit;

	form_gen_record(bnode, raw_record, rec_info, 0);
	curr = prev = VDFS2_INVALID_NODE_ID;
	index_val = record->val;
	next = le32_to_cpu(index_val->node_id);

	while (true) {
		int ret;
		int dang_count = 0;

		prev = curr;
		curr = next;

		if (curr == VDFS2_INVALID_NODE_ID)
			/* End of the level */
			goto exit;

		ret = vdfs2_get_next_btree_record(record);
		if (ret == -ENOENT)
			next = VDFS2_INVALID_NODE_ID;
		else if (ret)
			goto err_exit;
		else {
			index_val = record->val;
			next = le32_to_cpu(index_val->node_id);
		}

		bnode = __vdfs2_get_bnode(btree, curr, VDFS2_BNODE_MODE_RO);
		if (IS_ERR(bnode))
			goto err_exit;

		if (VDFS2_PREV_BNODE_ID(bnode) != prev) {
			VDFS2_ERR("prev bnode id mismatch in bnode#%u:"
					" expected %u, was %u",
					bnode->node_id, prev,
					VDFS2_PREV_BNODE_ID(bnode));
			goto err_exit;
		}

		/* Go through dangling bnodes */
		while (VDFS2_NEXT_BNODE_ID(bnode) != next &&
				VDFS2_NEXT_BNODE_ID(bnode) !=
				VDFS2_INVALID_NODE_ID) {
			prev = curr;
			curr = VDFS2_NEXT_BNODE_ID(bnode);

			vdfs2_put_bnode(bnode);
			bnode = __vdfs2_get_bnode(btree, curr,
					VDFS2_BNODE_MODE_RO);

			if (IS_ERR(bnode)) {
				VDFS2_ERR("Can not go through btree level: "
						"error getting next bnode");
				return PTR_ERR(bnode);
			}

			if (VDFS2_PREV_BNODE_ID(bnode) != prev) {
				VDFS2_ERR("prev bnode id mismatch in bnode#%u:"
						" expected %u, was %u",
						bnode->node_id, prev,
						VDFS2_PREV_BNODE_ID(bnode));
				goto err_exit;
			}

			dang_count++;

			VDFS2_DEBUG_BTREE("Dangling bnode#%u (%d)",
					bnode->node_id, dang_count);
		}
		total_dang_num += dang_count;
		if (dang_count > 1)
			VDFS2_DEBUG_BTREE("Danging chain of %d bnodes",
					dang_count);

		if (VDFS2_NEXT_BNODE_ID(bnode) != next) {
			VDFS2_ERR("next bnode id mismatch in bnode#%u: "
					"expected %u, was %u",
					bnode->node_id, next,
					VDFS2_NEXT_BNODE_ID(bnode));
			goto err_exit;
		}
		vdfs2_put_bnode(bnode);
	}

exit:
	vdfs2_release_record(record);
	return total_dang_num;
err_exit:
	vdfs2_release_record(record);
exit_no_record:
	return -1;
}

/* Very simplified version of check_btree_links_at_level for only root
 * level */
static int check_btree_links_at_root_level(struct vdfs2_btree *btree)
{
	struct vdfs2_bnode *bnode;
	__u32 prev, curr;
	int dangling_bnodes_num = 0;

	curr = vdfs2_btree_get_root_id(btree);
	prev = VDFS2_INVALID_NODE_ID;

	while (curr != VDFS2_INVALID_NODE_ID) {
		bnode = __vdfs2_get_bnode(btree, curr, VDFS2_BNODE_MODE_RO);
		if (IS_ERR(bnode)) {
			VDFS2_ERR("Can not get_bnode during check");
			return PTR_ERR(bnode);
		}

		if (VDFS2_PREV_BNODE_ID(bnode) != prev) {
			VDFS2_ERR("prev bnode id mismatch in bnode#%u:"
					" expected %u, was %u",
					bnode->node_id, prev,
					VDFS2_PREV_BNODE_ID(bnode));
		}

		if (VDFS2_NEXT_BNODE_ID(bnode) == VDFS2_INVALID_NODE_ID) {
			vdfs2_put_bnode(bnode);
			break;
		}

		dangling_bnodes_num++;
		prev = curr;
		curr = VDFS2_NEXT_BNODE_ID(bnode);
		VDFS2_DEBUG_BTREE("Dangling bnode#%u at root level (%d)",
				curr, dangling_bnodes_num);
		vdfs2_put_bnode(bnode);
	}

	return dangling_bnodes_num;
}

int vdfs2_check_btree_links(struct vdfs2_btree *btree, int *dang_num)
{
	struct vdfs2_bnode *bnode;
	int level, ret;
	__u32 bnode_id = vdfs2_btree_get_root_id(btree);
	if (dang_num)
		*dang_num = 0;


	ret = check_btree_links_at_root_level(btree);
	if (ret < 0)
		return ret;
	else if (dang_num)
		*dang_num += ret;
	for (level = vdfs2_btree_get_height(btree);
			level > VDFS2_BTREE_LEAF_LVL; level--) {
		void *record;
		struct generic_index_value *index_val;

		bnode = __vdfs2_get_bnode(btree, bnode_id, VDFS2_BNODE_MODE_RO);
		ret = check_btree_links_at_level(bnode);
		if (ret < 0)
			return -1;
		if (dang_num)
			*dang_num += ret;

		record = vdfs2_get_btree_record(bnode, 0);
		if (IS_ERR(record)) {
			VDFS2_ERR("Can't get record\n");
			return -1;
		}

		index_val = get_value_pointer(record);
		bnode_id = le32_to_cpu(index_val->node_id);
	}

	return 0;
}

int vdfs2_check_btree_records_order(struct vdfs2_btree *btree)
{
	struct vdfs2_bnode *bnode;
	int level, ret;
	__u32 bnode_id = vdfs2_btree_get_root_id(btree);

	for (level = vdfs2_btree_get_height(btree); level > 0; level--) {
		void *record;
		struct generic_index_value *index_val;

		bnode = __vdfs2_get_bnode(btree, bnode_id, VDFS2_BNODE_MODE_RO);
		ret = check_ordering_at_level(bnode);
		if (ret)
			return -1;

		record = vdfs2_get_btree_record(bnode, 0);
		if (IS_ERR(record)) {
			VDFS2_ERR("Can't get record\n");
			return -1;
		}

		index_val = get_value_pointer(record);
		bnode_id = le32_to_cpu(index_val->node_id);
	}

	return 0;
}

void vdfs2_release_record(struct vdfs2_btree_gen_record *record)
{
	struct vdfs2_btree_record_info *rec_info =
		VDFS2_BTREE_REC_I((void *) record);

	vdfs2_put_bnode(rec_info->rec_pos.bnode);
	kmem_cache_free(btree_record_info_cachep, rec_info);
}

void vdfs2_release_dirty_record(struct vdfs2_btree_gen_record *record)
{
	struct vdfs2_btree_record_info *rec_info =
		VDFS2_BTREE_REC_I((void *) record);

	vdfs2_mark_bnode_dirty(rec_info->rec_pos.bnode);
	vdfs2_release_record(record);
}

void vdfs2_mark_record_dirty(struct vdfs2_btree_gen_record *record)
{
	struct vdfs2_btree_record_info *rec_info =
		VDFS2_BTREE_REC_I((void *) record);

	vdfs2_mark_bnode_dirty(rec_info->rec_pos.bnode);
}


#ifdef USER_SPACE


u_int32_t btree_get_bitmap_size(struct vdfs2_sb_info *sbi)
{
	u_int32_t nodes_bitmap_size;
	/* get bitmap size in bytes */
	nodes_bitmap_size = get_bnode_size(sbi) -
		sizeof(struct vdfs2_raw_btree_head) -
		sizeof(unsigned int);

	return nodes_bitmap_size;
}

u_int32_t find_first_free_node_id(struct vdfs2_tools_btree_info *tree)
{
	u_int32_t rc = 0;
	struct vdfs2_raw_btree_head *head_bnode_desc;
	if (tree->vdfs2_btree.head_bnode) {
		head_bnode_desc = tree->vdfs2_btree.head_bnode->data;
		rc = find_first_zero_bit(&head_bnode_desc->bitmap,
			btree_get_bitmap_size(tree->vdfs2_btree.sbi) * 8);
	}
	return rc;
}

int test_and_clear_bnode_bitmap_bit(struct vdfs2_bnode *bnode)
{
	struct vdfs2_tools_btree_info *tree =
		container_of(bnode->host, struct vdfs2_tools_btree_info,
				vdfs2_btree);

	struct vdfs2_raw_btree_head *head_bnode_desc =
			tree->vdfs2_btree.head_bnode->data;
	char *bitmap = (char *) &head_bnode_desc->bitmap;
	int rc = bitmap[bnode->node_id >> 3] & (1 << (bnode->node_id % 8));
	util_clear_bits(bitmap, bnode->node_id, 1);
	return rc;
}

int test_and_set_bnode_bitmap_bit(struct vdfs2_tools_btree_info *tree,
		struct vdfs2_bnode *bnode)
{
	struct vdfs2_raw_btree_head *head_bnode_desc =
			tree->vdfs2_btree.head_bnode->data;
	char *bitmap = (char *) &head_bnode_desc->bitmap;
	int rc = bitmap[bnode->node_id >> 3] & (1 << (bnode->node_id % 8));
	util_set_bits(bitmap, bnode->node_id, 1);
	return rc;
}

void make_node_bitmap(__u8 *nodes_bitmap, u_int32_t nodes_bitmap_size,
		u_int32_t used_nodes)
{
	u_int32_t count;
	u_int32_t byte;

	memset(nodes_bitmap, 0, nodes_bitmap_size);
	for (count = 0; count < used_nodes; count++) {
		byte = count >> 3;
		nodes_bitmap[byte] |= 1 << (count % 8);
	}
}

int btree_init(struct vdfs2_sb_info *sbi,
		struct vdfs2_tools_btree_info *tree,
		int btree_type, short max_record_len)
{
	int ret = 0;
	tree->vdfs2_btree.node_size_bytes = get_bnode_size(sbi);
	tree->vdfs2_btree.sbi = sbi;

	tree->vdfs2_btree.btree_type = btree_type;
	tree->vdfs2_btree.max_record_len = max_record_len;

	/* Init head bnode */
	tree->vdfs2_btree.head_bnode = vdfs2_alloc_new_bnode(&tree->vdfs2_btree);
	if (IS_ERR(tree->vdfs2_btree.head_bnode)) {
		ret = (PTR_ERR(tree->vdfs2_btree.head_bnode));
		tree->vdfs2_btree.head_bnode = 0;
		goto error_exit;
	}
	init_head_bnode(tree->vdfs2_btree.head_bnode);

	mutex_init(&tree->vdfs2_btree.split_buff_lock);
	tree->vdfs2_btree.split_buff = malloc(tree->vdfs2_btree.node_size_bytes);
	if (!tree->vdfs2_btree.split_buff) {
		ret = -ENOMEM;
		goto error_exit;
	}

	return 0;
error_exit:
	return ret;
}

void init_head_bnode(struct vdfs2_bnode *head_bnode)
{
	struct vdfs2_raw_btree_head *head_bnode_desc = head_bnode->data;
	struct vdfs2_sb_info *sbi = head_bnode->host->sbi;
	u_int32_t nodes_bitmap_size;
	u_int32_t used_nodes = 1; /* 1 - head */
	u_int32_t root_bnode_id = 1;
	u_int16_t btree_height = 1;

	memset(head_bnode->data, 0x0, get_bnode_size(sbi));

	set_magic(head_bnode_desc->magic, VDFS2_BTREE_HEAD_NODE_MAGIC);
	head_bnode_desc->root_bnode_id = cpu_to_le32(root_bnode_id);
	head_bnode_desc->btree_height = cpu_to_le16(btree_height);

	nodes_bitmap_size = btree_get_bitmap_size(sbi);

	make_node_bitmap((__u8 *)&head_bnode_desc->bitmap, nodes_bitmap_size,
			used_nodes);
}

void put_bnode(struct vdfs2_bnode *bnode)
{
	free(bnode);
}

/* will be implemented for image creation */
int expand_tree(struct vdfs2_tools_btree_info *tree)
{
	u_int64_t i;
	u_int64_t new_allocated_bnodes_count = tree->allocated_bnodes_count;
	if (!new_allocated_bnodes_count)
		/* tree must have at least 2 bnodes */
		new_allocated_bnodes_count = 2;
	else
		new_allocated_bnodes_count *= 2;
	tree->bnode_array = realloc(tree->bnode_array,
			new_allocated_bnodes_count *
			sizeof(struct vdfs2_bnode *));
	if (!tree->bnode_array)
		return -ENOMEM;
	for (i = tree->allocated_bnodes_count; i < new_allocated_bnodes_count;
			i++)
		tree->bnode_array[i] = 0;
	tree->tree.buffer_size = new_allocated_bnodes_count *
			get_bnode_size(tree->vdfs2_btree.sbi);
	tree->allocated_bnodes_count = new_allocated_bnodes_count;
	return 0;
}

void btree_destroy_tree(struct vdfs2_tools_btree_info *tree)
{
	__u64 i;
	__u64 bnodes_count;
	if (!tree->bnode_array)
		return;
	bnodes_count = get_bnodes_count(tree);
	for (i = 0; i < bnodes_count; i++) {
		free(tree->bnode_array[i]->data);
		free(tree->bnode_array[i]);
	}
	free(tree->bnode_array);
	free(tree->vdfs2_btree.split_buff);
}

#endif

