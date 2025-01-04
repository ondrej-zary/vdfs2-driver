/**
 * @file	fs/vdfs2/inode.c
 * @brief	Basic inode operations.
 * @date	01/17/2012
 *
 * eMMCFS -- Samsung eMMC chip oriented File System, Version 1.
 *
 * This file implements inode operations and its related functions.
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

#include <linux/slab.h>
#include <linux/buffer_head.h>
#include <linux/writeback.h>
#include <linux/nls.h>
#include <linux/mpage.h>
#include <linux/version.h>
#include <linux/pagemap.h>
#include <linux/pagevec.h>
#include <linux/bio.h>
#include <linux/uaccess.h>
#include <linux/blkdev.h>
#include <linux/security.h>

#include "vdfs2.h"
#include "cattree.h"
#include "packtree.h"
#include "hlinktree.h"
#include "debug.h"

/* For testing purpose use fake page cash size */
/*#define VDFS2_PAGE_SHIFT 5*/
/*#define VDFS2_BITMAP_PAGE_SHIFT (3 + VDFS2_PAGE_SHIFT)*/
/*#define VDFS2_BITMAP_PAGE_MASK (((__u64) 1 << VDFS2_BITMAP_PAGE_SHIFT)\
 * - 1)*/

/**
 * @brief		Create inode.
 * @param [out]	dir		The inode to be created
 * @param [in]	dentry	Struct dentry with information
 * @param [in]	mode	Mode of creation
 * @param [in]	nd		Struct with name data
 * @return		Returns 0 on success, errno on failure
 */
#if LINUX_VERSION_CODE >= KERNEL_VERSION(3, 8, 5)
static int vdfs2_create(struct user_namespace *mnt_userns, struct inode *dir,
		struct dentry *dentry, umode_t mode, bool excl);
#else
static int vdfs2_create(struct inode *dir, struct dentry *dentry, int mode,
		struct nameidata *nd);
#endif

static int vdfs2_get_block_prep_da(struct inode *inode, sector_t iblock,
		struct buffer_head *bh_result, int create) ;

/**
 * @brief		Allocate new inode.
 * @param [in]	dir		Parent directory
 * @param [in]	mode	Mode of operation
 * @return		Returns pointer to newly created inode on success,
 *			errno on failure
 */
static struct inode *vdfs2_new_inode(struct inode *dir, umode_t mode);
struct inode *vdfs2_get_inode_from_record(struct vdfs2_cattree_record *record);

/**
 * @brief		Get root folder.
 * @param [in]	tree	Pointer to btree information
 * @param [out] fd	Buffer for finded data
 * @return		Returns 0 on success, errno on failure
 */
struct inode *vdfs2_get_root_inode(struct vdfs2_btree *tree)
{
	struct inode *root_inode = NULL;
	struct vdfs2_cattree_record *record = NULL;

	record = vdfs2_cattree_find(tree, VDFS2_ROOTDIR_OBJ_ID,
			VDFS2_ROOTDIR_NAME, sizeof(VDFS2_ROOTDIR_NAME) - 1,
			VDFS2_BNODE_MODE_RO);

	if (IS_ERR(record)) {
		/* Pass error code to return value */
		root_inode = (void *)record;
		goto exit;
	}

	root_inode = vdfs2_get_inode_from_record(record);
	vdfs2_release_record((struct vdfs2_btree_gen_record *) record);
exit:
	return root_inode;
}


static int vdfs2_fill_cattree_record(struct inode *inode,
		struct vdfs2_cattree_record *record)
{
	void *pvalue = record->val;
	int ret = 0;

	BUG_ON(!pvalue || IS_ERR(pvalue));

	VDFS2_I(inode)->record_type = record->key->record_type;

	if (VDFS2_GET_CATTREE_RECORD_TYPE(record) == VDFS2_CATALOG_HLINK_RECORD)
		vdfs2_fill_hlink_value(inode, pvalue);
	else
		ret = vdfs2_fill_cattree_value(inode, pvalue);

	return ret;
}

/* Replacement for vdfs2_add_new_cattree_object */
static int vdfs2_insert_cattree_object(struct vdfs2_btree *tree,
		struct inode *inode, u64 parent_id)
{
	struct vdfs2_cattree_record *record = NULL;
	u8 record_type;
	int ret = 0;
	char *name = VDFS2_I(inode)->name;
	int len = strlen(name);

	ret = vdfs2_get_record_type_on_mode(inode, &record_type);

	if (ret)
		return ret;

	record = vdfs2_cattree_place_record(tree, parent_id, name, len,
			record_type);

	if (IS_ERR(record))
		return PTR_ERR(record);

	ret = vdfs2_fill_cattree_record(inode, record);
	vdfs2_release_dirty_record((struct vdfs2_btree_gen_record *) record);

	return ret;
}

/**
 * @brief		Method to read (list) directory.
 * @param [in]	filp	File pointer
 * @param [in]	ctx	Directory context
 * @return		Returns count of files/dirs
 */
static int vdfs2_readdir(struct file *filp, struct dir_context *ctx)
{
	struct inode *inode = filp->f_path.dentry->d_inode;
	struct vdfs2_inode_info *inode_info = VDFS2_I(inode);
	struct dentry *dentry = filp->f_path.dentry;
	struct vdfs2_sb_info *sbi = inode->i_sb->s_fs_info;
	__u64 catalog_id = inode->i_ino;
	int ret = 0;
	struct vdfs2_cattree_record *record;
	struct vdfs2_btree *btree = sbi->catalog_tree;
	/* return 0 if no more entries in the directory */
	switch (ctx->pos) {
	case 0:
		if (!dir_emit(ctx, ".", 1, inode->i_ino, DT_DIR))
			goto exit_noput;
		ctx->pos++;
		fallthrough;
	case 1:
		if (!dir_emit(ctx, "..", 2,
			dentry->d_parent->d_inode->i_ino, DT_DIR))
			goto exit_noput;
		ctx->pos++;
		break;
	default:
		if (!filp->private_data)
			return 0;
		break;
	}

	if (inode_info->record_type >= VDFS2_CATALOG_PTREE_RECORD) {
		if (inode_info->ptree.tree_info == NULL) {
			struct installed_packtree_info *packtree;
			packtree = vdfs2_get_packtree(inode);
			if (IS_ERR(packtree))
				return PTR_ERR(packtree);
			inode_info->ptree.tree_info = packtree;
			atomic_set(&(packtree->open_count), 0);
		}

		if (inode_info->record_type == VDFS2_CATALOG_PTREE_ROOT)
			catalog_id =  VDFS2_ROOT_INO;
		else
			catalog_id -=
				inode_info->ptree.tree_info->params.start_ino;

		btree = inode_info->ptree.tree_info->tree;
	}

	VDFS2_DEBUG_MUTEX("cattree mutex r lock");
	mutex_r_lock(sbi->catalog_tree->rw_tree_lock);
	VDFS2_DEBUG_MUTEX("cattree mutex r lock succ");
	if (!filp->private_data) {
		record = vdfs2_cattree_get_first_child(btree, catalog_id);
	} else {
		char *name = filp->private_data;
		record = vdfs2_cattree_find(btree, catalog_id,
				name, strlen(name), VDFS2_BNODE_MODE_RO);
	}

	if (IS_ERR(record)) {
		ret = (PTR_ERR(record) == -EISDIR) ? 0 : PTR_ERR(record);
		goto fail;
	}

	while (1) {
		struct vdfs2_catalog_folder_record *cattree_val;
		__u64 obj_id = 0;
		u8 record_type;
		umode_t object_mode;

		if (record->key->parent_id != cpu_to_le64(catalog_id))
			goto exit;

		cattree_val = record->val;
		record_type = record->key->record_type;
		if (record_type >= VDFS2_CATALOG_PTREE_FOLDER) {
			object_mode = le16_to_cpu(
				((struct vdfs2_pack_common_value *)record->val)->
				permissions.file_mode);
			obj_id = le64_to_cpu(((struct vdfs2_pack_common_value *)
				record->val)->object_id) + VDFS2_I(inode)->
					ptree.tree_info->params.start_ino;
		} else if (record_type == VDFS2_CATALOG_PTREE_ROOT) {
			struct vdfs2_pack_insert_point_value *val = record->val;
			object_mode =
				le16_to_cpu(val->common.permissions.file_mode);
			obj_id = le64_to_cpu(val->common.object_id);
		} else if (record_type == VDFS2_CATALOG_HLINK_RECORD) {
			object_mode = le16_to_cpu((
					(struct vdfs2_catalog_hlink_record *)
					cattree_val)->file_mode);
			obj_id = le64_to_cpu(
					((struct vdfs2_catalog_hlink_record *)
					cattree_val)->object_id);
		} else {
			object_mode = le16_to_cpu(cattree_val->
					permissions.file_mode);
			obj_id = le64_to_cpu(cattree_val->object_id);
		}

		ret = dir_emit(ctx, record->key->name.unicode_str,
				record->key->name.length,
				obj_id, IFTODT(object_mode));

		if (!ret) {
			char *private_data;
			if (!filp->private_data) {
				private_data = kzalloc(VDFS2_CAT_MAX_NAME,
						GFP_KERNEL);
				filp->private_data = private_data;
				if (!private_data) {
					ret = -ENOMEM;
					goto fail;
				}
			} else {
				private_data = filp->private_data;
			}

			strncpy(private_data, record->key->name.unicode_str,
					VDFS2_CAT_MAX_NAME);

			ret = 0;
			goto exit;
		}

		ctx->pos++;
		ret = vdfs2_cattree_get_next_record(record);
		if ((ret == -ENOENT) ||
			record->key->parent_id != cpu_to_le64(catalog_id)) {
			/* No more entries */
			kfree(filp->private_data);
			filp->private_data = NULL;
			ret = 0;
			break;
		} else if (ret) {
			goto fail;
		}

	}

exit:
	vdfs2_release_record((struct vdfs2_btree_gen_record *) record);
exit_noput:
	VDFS2_DEBUG_MUTEX("cattree mutex r lock un");
	mutex_r_unlock(sbi->catalog_tree->rw_tree_lock);
	return ret;
fail:
	VDFS2_DEBUG_INO("finished with err (%d)", ret);
	if (!IS_ERR_OR_NULL(record))
		vdfs2_release_record((struct vdfs2_btree_gen_record *) record);

	VDFS2_DEBUG_MUTEX("cattree mutex r lock un");
	mutex_r_unlock(sbi->catalog_tree->rw_tree_lock);
	return ret;
}

/**
 * @brief		Method to look up an entry in a directory.
 * @param [in]	dir		Parent directory
 * @param [in]	dentry	Searching entry
 * @param [in]	nd		Associated nameidata
 * @return		Returns pointer to found dentry, NULL if it is
 *			not found, ERR_PTR(errno) on failure
 */
#if LINUX_VERSION_CODE >= KERNEL_VERSION(3, 8, 5)
struct dentry *vdfs2_lookup(struct inode *dir, struct dentry *dentry,
						unsigned int flags)
#else
struct dentry *vdfs2_lookup(struct inode *dir, struct dentry *dentry,
						struct nameidata *nd)
#endif
{
	struct super_block *sb = dir->i_sb;
	struct vdfs2_sb_info *sbi = sb->s_fs_info;
	struct vdfs2_cattree_record *record;
	struct vdfs2_inode_info *dir_inode_info = VDFS2_I(dir);
	struct inode *found_inode = NULL;
	int err = 0;
	struct vdfs2_btree *tree = sbi->catalog_tree;
	__u64 catalog_id = dir->i_ino;
	struct dentry *ret;

	if (dentry->d_name.len > VDFS2_CAT_MAX_NAME)
		return ERR_PTR(-ENAMETOOLONG);

	if (!S_ISDIR(dir->i_mode))
		return ERR_PTR(-ENOTDIR);

	if (dir_inode_info->record_type == VDFS2_CATALOG_PTREE_ROOT) {
		if (dir_inode_info->ptree.tree_info == NULL) {
			struct installed_packtree_info *packtree;
			packtree = vdfs2_get_packtree(dir);
			if (IS_ERR_OR_NULL(packtree))
				return (struct dentry *)packtree;
			dir_inode_info->ptree.tree_info = packtree;
			atomic_set(&(packtree->open_count), 0);
		}
	}

	if (VDFS2_I(dir)->record_type >= VDFS2_CATALOG_PTREE_RECORD) {
		tree = VDFS2_I(dir)->ptree.tree_info->tree;
		if (VDFS2_I(dir)->record_type == VDFS2_CATALOG_PTREE_ROOT)
			catalog_id = VDFS2_ROOT_INO;
		else
			catalog_id -=
			VDFS2_I(dir)->ptree.tree_info->params.start_ino;
	}

	VDFS2_DEBUG_MUTEX("cattree mutex r lock");
	mutex_r_lock(tree->rw_tree_lock);
	VDFS2_DEBUG_MUTEX("cattree mutex r lock succ");

	record = vdfs2_cattree_find(tree, catalog_id,
			dentry->d_name.name, dentry->d_name.len,
			VDFS2_BNODE_MODE_RO);
	if (!record) {
		err = -EFAULT;
		goto exit;
	}
	if (IS_ERR(record)) {
		/* Pass error code to return value */
		err = PTR_ERR(record);
		goto exit;
	}

	found_inode = vdfs2_get_inode_from_record(record);
	if (!found_inode)
		err = -EFAULT;
	if (IS_ERR(found_inode))
		err = PTR_ERR(found_inode);

	vdfs2_release_record((struct vdfs2_btree_gen_record *) record);
#ifdef CONFIG_VDFS2_QUOTA
	VDFS2_I(found_inode)->quota_index = VDFS2_I(dir)->quota_index;
#endif

	if ((!err) &&
		(VDFS2_I(dir)->record_type >= VDFS2_CATALOG_PTREE_RECORD)) {
		VDFS2_I(found_inode)->ptree.tree_info =
				VDFS2_I(dir)->ptree.tree_info;
	}

exit:
	VDFS2_DEBUG_MUTEX("cattree mutex r lock un");
	mutex_r_unlock(tree->rw_tree_lock);

	if (err && err != -ENOENT)
		return ERR_PTR(err);
	else {
		ret = d_splice_alias(found_inode, dentry);
#ifdef CONFIG_VDFS2_QUOTA
		if (found_inode &&
			is_vdfs2_inode_flag_set(found_inode, HAS_QUOTA) &&
				VDFS2_I(found_inode)->quota_index == -1) {
			int index = get_quota(dentry);
			if (IS_ERR_VALUE(index))
				return ERR_PTR(index);

			VDFS2_I(found_inode)->quota_index = index;
		}
#endif
		return ret;
	}
}

/**
 * @brief		Get free inode index[es].
 * @param [in]	sbi	Pointer to superblock information
 * @param [out]	i_ino	Resulting inode number
 * @param [in]	count	Requested inode numbers count.
 * @return		Returns 0 if success, err code if fault
 */
int vdfs2_get_free_inode(struct vdfs2_sb_info *sbi, ino_t *i_ino, int count)
{
	struct page *page = NULL;
	void *data;
	__u64 last_used = atomic64_read(&sbi->free_inode_bitmap.last_used);
	__u64 page_index = last_used;
	/* find_from is modulo, page_index is result of div */
	__u64 find_from = do_div(page_index, VDFS2_BIT_BLKSIZE(PAGE_SIZE,
			INODE_BITMAP_MAGIC_LEN));
	/* first id on the page. */
	unsigned int id_offset = page_index * VDFS2_BIT_BLKSIZE(PAGE_SIZE,
			INODE_BITMAP_MAGIC_LEN);
	/* bits per block */
	int data_size = VDFS2_BIT_BLKSIZE(PAGE_SIZE,
			INODE_BITMAP_MAGIC_LEN);
	int err = 0;
	int pass = 0;
	int start_page = page_index;
	pgoff_t total_pages = VDFS2_LAST_TABLE_INDEX(sbi,
			VDFS2_FREE_INODE_BITMAP_INO) + 1;
	*i_ino = 0;

	if (count > data_size)
		return -ENOMEM; /* todo we can allocate inode numbers chunk
		 only within one page*/

	while (*i_ino == 0) {
		page = vdfs2_read_or_create_page(sbi->free_inode_bitmap.inode,
				page_index);
		if (IS_ERR_OR_NULL(page))
			return PTR_ERR(page);
		lock_page(page);

		data = kmap(page);
		*i_ino = bitmap_find_next_zero_area(data +
			INODE_BITMAP_MAGIC_LEN,
			data_size, find_from, count, 0);
		/* free area is found */
		if ((*i_ino + count - 1) < data_size) {
			VDFS2_BUG_ON(*i_ino + id_offset < VDFS2_1ST_FILE_INO);
			if (count > 1)
				bitmap_set(data + INODE_BITMAP_MAGIC_LEN,
						*i_ino, count);
			else
				VDFS2_BUG_ON(test_and_set_bit(*i_ino,
						data + INODE_BITMAP_MAGIC_LEN));
			*i_ino += id_offset;
			if (atomic64_read(&sbi->free_inode_bitmap.last_used) <
				(*i_ino  + count - 1))
				atomic64_set(&sbi->free_inode_bitmap.last_used,
					*i_ino + count - 1);

			vdfs2_add_chunk_bitmap(sbi, page, 1);
		} else { /* if no free bits in current page */
			*i_ino = 0;
			page_index++;
			/* if we reach last page go to first one */
			page_index = (page_index == total_pages) ? 0 :
					page_index;
			/* if it's second cycle expand the file */
			if (pass == 1)
				page_index = total_pages;
			/* if it's start page, increase pass counter */
			else if (page_index == start_page)
				pass++;
			id_offset = ((int)page_index) * data_size;
			/* if it's first page, increase the inode generation */
			if (page_index == 0) {
				/* for first page we should look up from
				 * VDFS2_1ST_FILE_INO bit*/
				atomic64_set(&sbi->free_inode_bitmap.last_used,
					VDFS2_1ST_FILE_INO);
				find_from = VDFS2_1ST_FILE_INO;
				/* increase generation of the inodes */
				le32_add_cpu(
					&(VDFS2_RAW_EXSB(sbi)->generation), 1);
				vdfs2_dirty_super(sbi);
			} else
				find_from = 0;
			VDFS2_DEBUG_INO("move to next page"
				" ind = %lld, id_off = %d, data = %d\n",
				page_index, id_offset, data_size);
		}

		kunmap(page);
		unlock_page(page);
		put_page(page);

	}

	return err;
}

/**
 * @brief		Free several inodes.
 *		Agreement: inode chunks (for installed packtrees)
 *		can be allocated only within single page of inodes bitmap.
 *		So free requests also can not exceeds page boundaries.
 * @param [in]	sbi	Superblock information
 * @param [in]	inode_n	Start index of inodes to be free
 * @param [in]	count	Count of inodes to be free
 * @return		Returns error code
 */
int vdfs2_free_inode_n(struct vdfs2_sb_info *sbi, __u64 inode_n, int count)
{
	void *data;
	struct page *page = NULL;
	__u64 page_index = inode_n;
	/* offset inside page */
	__u32 int_offset = do_div(page_index, VDFS2_BIT_BLKSIZE(PAGE_SIZE,
			INODE_BITMAP_MAGIC_LEN));

	page = vdfs2_read_or_create_page(sbi->free_inode_bitmap.inode,
			page_index);
	if (IS_ERR_OR_NULL(page))
		return PTR_ERR(page);

	lock_page(page);
	data = kmap(page);
	for (; count; count--)
		if (!test_and_clear_bit(int_offset + count - 1,
			data + INODE_BITMAP_MAGIC_LEN)) {
			VDFS2_DEBUG_INO("vdfs2_free_inode_n %llu"
				, inode_n);
			VDFS2_BUG();
		}

	vdfs2_add_chunk_bitmap(sbi, page, 1);
	kunmap(page);
	unlock_page(page);
	put_page(page);
	return 0;
}

/**
 * @brief		Unlink function.
 * @param [in]	dir		Pointer to inode
 * @param [in]	dentry	Pointer to directory entry
 * @return		Returns error codes
 */
static int vdfs2_unlink(struct inode *dir, struct dentry *dentry)
{
	struct inode *inode = dentry->d_inode;
	struct super_block *sb = inode->i_sb;
	struct vdfs2_sb_info *sbi = sb->s_fs_info;
	int is_hard_link = is_vdfs2_inode_flag_set(inode, HARD_LINK);
	int ret = 0;
	char ino_name[UUL_MAX_LEN + 1];
	int len = 0;

	if (check_permissions(sbi))
		return -EINTR;
	/* packtree removal only through install.vdfs2 -u packtree_root_dir */
	if (VDFS2_I(dir)->record_type >= VDFS2_CATALOG_PTREE_RECORD)
		return -EPERM;

	VDFS2_DEBUG_INO("unlink '%s', ino = %lu", dentry->d_iname,
			inode->i_ino);

	vdfs2_start_transaction(sbi);
	mutex_lock(&VDFS2_I(inode)->truncate_mutex);
	mutex_w_lock(sbi->catalog_tree->rw_tree_lock);

	drop_nlink(inode);
	VDFS2_BUG_ON(inode->i_nlink > VDFS2_LINK_MAX);

	if (inode->i_nlink) {
		/* must be hard link */
		if (!is_vdfs2_inode_flag_set(inode, HARD_LINK)) {
			/* volume broken? */
			VDFS2_DEBUG_INO("Incorect nlink count inode %lu",
					inode->i_ino);
			ret = -EINVAL;
			goto exit_inc_nlink;
		}
		inode->i_ctime = current_time(dir);
	} else {
		snprintf(ino_name, UUL_MAX_LEN + 1, "%lu", inode->i_ino);
		len = strlen(ino_name);
		if (!VDFS2_I(inode)->name || len >
			strlen(VDFS2_I(inode)->name)) {
			kfree(VDFS2_I(inode)->name);
			VDFS2_I(inode)->name = kzalloc(len + 1, GFP_KERNEL);
		}
		VDFS2_I(inode)->parent_id = VDFS2_ORPHAN_INODES_INO;
		memcpy(VDFS2_I(inode)->name, ino_name, len + 1);
		clear_vdfs2_inode_flag(inode, HARD_LINK);
		ret = vdfs2_insert_cattree_object(sbi->catalog_tree, inode,
			VDFS2_ORPHAN_INODES_INO);
		if (ret)
			goto exit_inc_nlink;
	}
	ret = vdfs2_cattree_remove(sbi, dir->i_ino, dentry->d_name.name,
			dentry->d_name.len);
	if (ret)
		goto exit_kill_orphan_record;
	vdfs2_remove_indirect_index(sbi->catalog_tree, inode);
	mutex_w_unlock(sbi->catalog_tree->rw_tree_lock);
	if (is_hard_link) {
		if (!inode->i_nlink)
			/* remove from hard link tree must be success always */
			BUG_ON(vdfs2_hard_link_remove(sbi, inode->i_ino));
		else
			/* write inode to bnode must be success always here*/
			BUG_ON(vdfs2_write_inode_to_bnode(inode));
	}

	if (!inode->i_nlink) {
		remove_inode_hash(dentry->d_inode);
		if (is_vdfs2_inode_flag_set(inode, TINY_FILE))
			atomic64_dec(&sbi->tiny_files_counter);
	}

	mutex_lock_nested(&VDFS2_I(dir)->truncate_mutex, VDFS2_REG_DIR_M);
	if (dir->i_size != 0)
		dir->i_size--;
	else
		VDFS2_DEBUG_INO("Files count mismatch");

	dir->i_ctime = current_time(dir);
	dir->i_mtime = current_time(dir);
	ret = vdfs2_write_inode_to_bnode(dir);
	mutex_unlock(&VDFS2_I(dir)->truncate_mutex);

	goto exit;

exit_kill_orphan_record:
	BUG_ON(vdfs2_cattree_remove(sbi, VDFS2_ORPHAN_INODES_INO,
			VDFS2_I(inode)->name, len));
exit_inc_nlink:
	if (is_hard_link) {
		set_vdfs2_inode_flag(inode, HARD_LINK);
		kfree(VDFS2_I(inode)->name);
		VDFS2_I(inode)->name = NULL;
		VDFS2_I(inode)->parent_id = 0;
	}
	mutex_w_unlock(sbi->catalog_tree->rw_tree_lock);
	inc_nlink(inode);
exit:
	mutex_unlock(&VDFS2_I(inode)->truncate_mutex);
	vdfs2_stop_transaction(sbi);
	return ret;
}

/**
 * @brief		Gets file's extent with iblock less and closest
 *			to the given one
 * @param [in]	inode	Pointer to the file's inode
 * @param [in]	iblock	Requested iblock
 * @return		Returns extent, or err code on failure
 */
int vdfs2_get_iblock_extent(struct inode *inode, sector_t iblock,
		struct vdfs2_extent_info *result, sector_t *hint_block)
{
	struct vdfs2_inode_info *inode_info = VDFS2_I(inode);
	struct vdfs2_sb_info *sbi = VDFS2_SB(inode->i_sb);
	struct vdfs2_fork_info *fork = &inode_info->fork;
	int ret = 0, pos;
	sector_t last_iblock;

	if (!fork->total_block_count)
		return 0;


	for (pos = 0; pos < fork->used_extents; pos++) {
		last_iblock = fork->extents[pos].iblock +
				fork->extents[pos].block_count - 1;
		if ((iblock >= fork->extents[pos].iblock) &&
				(iblock <= last_iblock)) {
			/* extent is found */
			memcpy(result, &fork->extents[pos], sizeof(*result));
			goto exit;
		}
	}
	/* required extent is not found
	 * if no extent(s) for the inode in extents overflow tree
	 * the last used extent in fork can be used for allocataion
	 * hint calculation */
	if (fork->used_extents < VDFS2_EXTENTS_COUNT_IN_FORK) {
		memcpy(result, &fork->extents[pos - 1],
			sizeof(*result));
		goto not_found;
	}

	/* extent is't found in fork */
	/* now we must to look up for extent in extents overflow B-tree */
	ret = vdfs2_exttree_get_extent(sbi, inode->i_ino, iblock, result);

	if (ret && ret != -ENOENT)
		return ret;

	if (result->first_block == 0) {
		/* no extents in extents overflow tree */
		memcpy(result, &fork->extents[VDFS2_EXTENTS_COUNT_IN_FORK - 1],
				sizeof(*result));
		goto not_found;
	}

	last_iblock = result->iblock + result->block_count - 1;
	/*check : it is a required extent or not*/
	if ((iblock >= result->iblock) && (iblock <= last_iblock))
		goto exit;

not_found:
	if (iblock == result->iblock + result->block_count)
		*hint_block = result->first_block + result->block_count;
	else
		*hint_block = 0;

	result->first_block = 0;
exit:
	return 0;

}

/**
 * @brief			Add allocated space into the fork or the exttree
 * @param [in]	inode_info	Pointer to inode_info structure.
 * @param [in]	iblock		First logical block number of allocated space.
 * @param [in]	block		First physical block number of allocated space.
 * @param [in]	blk_cnt		Allocated space size in blocks.
 * @param [in]	update_bnode	It's flag which control the update of the
				bnode. 1 - update bnode rec, 0 - update only
				inode structs.
 * @return			Returns physical block number, or err_code
 */
static int insert_extent(struct vdfs2_inode_info *inode_info,
		struct vdfs2_extent_info *extent, int update_bnode)
{

	struct inode *inode = &inode_info->vfs_inode;
	struct vdfs2_sb_info *sbi = inode->i_sb->s_fs_info;
	struct vdfs2_fork_info *fork = &inode_info->fork;
	int pos = 0, ret = 0, count;

	/* try to expand extent in vdfs2_inode_info fork by new extent*/
	sector_t last_iblock, last_extent_block;

	if (fork->used_extents == 0) {
		fork->used_extents++;
		memcpy(&fork->extents[pos], extent, sizeof(*extent));
		goto update_on_disk_layout;
	}

	if (extent->iblock < fork->extents[0].iblock)
		goto insert_in_fork;

	/* find a place for insertion */
	for (pos = 0; pos < fork->used_extents; pos++) {
		if (extent->iblock < fork->extents[pos].iblock)
			break;
	}
	/* we need previous extent */
	pos--;

	/* try to extend extent in fork */
	last_iblock = fork->extents[pos].iblock +
				fork->extents[pos].block_count - 1;

	last_extent_block = fork->extents[pos].first_block +
			fork->extents[pos].block_count - 1;

	if ((last_iblock + 1 == extent->iblock) &&
		(last_extent_block + 1 == extent->first_block)) {
		/* expand extent in fork */
		fork->extents[pos].block_count += extent->block_count;
		/* FIXME check overwrite next extent */
		goto update_on_disk_layout;
	}

	/* we can not expand last extent in fork */
	/* now we have a following options:
	 * 1. insert in fork
	 * 2. insert into extents overflow btree
	 * 3a. shift extents if fork to right, push out rightest extent
	 * 3b. shift extents in fork to right and insert in fork
	 * into extents overflow btee
	 * */
	pos++;
insert_in_fork:
	if (pos < VDFS2_EXTENTS_COUNT_IN_FORK &&
			fork->extents[pos].first_block == 0) {
		/* 1. insert in fork */
		memcpy(&fork->extents[pos], extent, sizeof(*extent));
		fork->used_extents++;
	} else if (pos == VDFS2_EXTENTS_COUNT_IN_FORK) {
		/* 2. insert into extents overflow btree */
		ret = vdfs2_extree_insert_extent(sbi, inode->i_ino,
						extent);
		if (ret)
			goto exit;

		goto update_on_disk_layout;
	} else {
		if (fork->used_extents == VDFS2_EXTENTS_COUNT_IN_FORK) {
			/* 3a push out rightest extent into extents
			 * overflow btee */
			ret = vdfs2_extree_insert_extent(sbi, inode->i_ino,
				&fork->extents[VDFS2_EXTENTS_COUNT_IN_FORK - 1]);
			if (ret)
				goto exit;
		} else
			fork->used_extents++;

		/*  3b. shift extents in fork to right  */
		for (count = fork->used_extents - 1; count > pos; count--)
			memcpy(&fork->extents[count], &fork->extents[count - 1],
						sizeof(*extent));
		memcpy(&fork->extents[pos], extent, sizeof(*extent));
	}

update_on_disk_layout:
	fork->total_block_count += extent->block_count;
	inode_add_bytes(inode, sbi->sb->s_blocksize * extent->block_count);
	if (update_bnode) {
		ret = vdfs2_write_inode_to_bnode(inode);
		VDFS2_BUG_ON(ret);
	}

exit:
	return ret;
}

/**
 * @brief				Logical to physical block number
 *					translation for already allocated
 *					blocks.
 * @param [in]		inode_info	Pointer to inode_info structure.
 * @param [in]		iblock		Requested logical block number
 * @param [in, out]	max_blocks	Distance (in blocks) from returned
 *					block to end of extent is returned
 *					through this pointer.
 * @return				Returns physical block number.
 */
sector_t vdfs2_find_old_block(struct vdfs2_inode_info *inode_info,
	sector_t iblock, __u32 *max_blocks)
{
	struct vdfs2_fork_info *fork = &inode_info->fork;
	unsigned int i;
	sector_t iblock_start = iblock;

	for (i = 0; i < fork->used_extents; i++)
		if (iblock < fork->extents[i].block_count) {
			if (max_blocks)
				*max_blocks = fork->extents[i].block_count -
					iblock;
			return fork->extents[i].first_block + iblock;
		} else
			iblock -= fork->extents[i].block_count;
	return vdfs2_exttree_get_block(inode_info, iblock_start, max_blocks);
}


void vdfs2_free_reserved_space(struct inode *inode, sector_t iblocks_count)
{
	struct vdfs2_sb_info *sbi = VDFS2_SB(inode->i_sb);
	/* if block count is valid and fsm exist, free the space.
	 * fsm may not exist in case of umount */
	if (iblocks_count && sbi->fsm_info) {
		mutex_lock(&sbi->fsm_info->lock);
		percpu_counter_add(&sbi->free_blocks_count, iblocks_count);
#ifdef CONFIG_VDFS2_QUOTA
		if (VDFS2_I(inode)->quota_index != -1) {
			sbi->quotas[VDFS2_I(inode)->quota_index].has -=
				(iblocks_count << sbi->block_size_shift);
			update_has_quota(sbi,
				sbi->quotas[VDFS2_I(inode)->quota_index].ino,
				VDFS2_I(inode)->quota_index);
		}
#endif
		mutex_unlock(&sbi->fsm_info->lock);
	}
	return;
}

static int vdfs2_reserve_space(struct vdfs2_sb_info *sbi)
{

	int ret = 0;
	mutex_lock(&sbi->fsm_info->lock);
	if (percpu_counter_sum(&sbi->free_blocks_count))
		percpu_counter_dec(&sbi->free_blocks_count);
	else
		ret = -ENOSPC;
	mutex_unlock(&sbi->fsm_info->lock);
	return ret;
}
/*
 * */
int vdfs2_get_block_prep_da(struct inode *inode, sector_t iblock,
		struct buffer_head *bh_result, int create) {
	struct vdfs2_inode_info *inode_info = VDFS2_I(inode);
	struct vdfs2_sb_info *sbi = inode->i_sb->s_fs_info;
	struct super_block *sb = inode->i_sb;
	sector_t offset_alloc_hint = 0, res_block;
	int err = 0;
	__u32 max_blocks = 1;
	__u32 buffer_size = bh_result->b_size >> sbi->block_size_shift;

	struct vdfs2_extent_info extent;
	if (!create)
		BUG();
	memset(&extent, 0x0, sizeof(extent));
	mutex_lock_nested(&inode_info->truncate_mutex, SMALL_AREA);

	if (is_vdfs2_inode_flag_set(inode, TINY_FILE) ||
		is_vdfs2_inode_flag_set(inode, SMALL_FILE))
		goto exit;

	/* get extent contains iblock*/
	err = vdfs2_get_iblock_extent(&inode_info->vfs_inode, iblock, &extent,
			&offset_alloc_hint);

	if (err)
		goto exit;

	if (extent.first_block)
		goto done;

	if (buffer_delay(bh_result))
		goto exit;

	err = vdfs2_reserve_space(VDFS2_SB(inode->i_sb));
	if (err)
		/* not enough space to reserve */
		goto exit;
#ifdef CONFIG_VDFS2_QUOTA
	if (inode_info->quota_index != -1) {
		sbi->quotas[inode_info->quota_index].has +=
				(1 << sbi->block_size_shift);
		if (sbi->quotas[inode_info->quota_index].has >
				sbi->quotas[inode_info->quota_index].max) {
			err = -ENOSPC;
			vdfs2_free_reserved_space(inode, 1);
			goto exit;
		}

		update_has_quota(sbi,
			sbi->quotas[inode_info->quota_index].ino,
			inode_info->quota_index);
	}
#endif
	map_bh(bh_result, inode->i_sb, VDFS2_INVALID_BLOCK);
	set_buffer_new(bh_result);
	set_buffer_delay(bh_result);
	err = vdfs2_runtime_extent_add(iblock, offset_alloc_hint,
			&inode_info->runtime_extents);
	if (err)
		vdfs2_free_reserved_space(inode, 1);
	goto exit;
done:
	res_block = extent.first_block + (iblock - extent.iblock);
	max_blocks = extent.block_count - (iblock - extent.iblock);
	BUG_ON(res_block > extent.first_block + extent.block_count);

	if (res_block >
		(sb->s_bdev->bd_inode->i_size >> sbi->block_size_shift)) {
		if (!is_sbi_flag_set(sbi, IS_MOUNT_FINISHED)) {
			VDFS2_ERR("Block beyond block bound requested");
			err = -EFAULT;
			goto exit;
		} else {
			BUG();
		}
	}
	mutex_unlock(&inode_info->truncate_mutex);
	clear_buffer_new(bh_result);
	map_bh(bh_result, inode->i_sb, res_block);
	bh_result->b_size = sb->s_blocksize * min(max_blocks, buffer_size);


	return 0;
exit:
	mutex_unlock(&inode_info->truncate_mutex);
	return err;
}
/**
 * @brief				Logical to physical block numbers
 *					translation.
 * @param [in]		inode		Pointer to inode structure.
 * @param [in]		iblock		Requested logical block number.
 * @param [in, out]	bh_result	Pointer to buffer_head.
 * @param [in]		create		"Expand file allowed" flag.
 * @param [in]		da		delay allocation flag, if 1, the buffer
 *					already reserved space.
 * @return				0 on success, or error code
 */
int vdfs2_get_int_block(struct inode *inode, sector_t iblock,
	struct buffer_head *bh_result, int create, int da)
{
	struct vdfs2_inode_info *inode_info = VDFS2_I(inode);
	struct vdfs2_sb_info *sbi = inode->i_sb->s_fs_info;
	struct super_block *sb = inode->i_sb;
	sector_t offset_alloc_hint = 0, res_block;
	int alloc = 0;
	__u32 max_blocks = 1;
	int count = 1;
	__u32 buffer_size = bh_result->b_size >> sbi->block_size_shift;
	int err = 0;
	struct vdfs2_extent_info extent;

	memset(&extent, 0x0, sizeof(extent));
	mutex_lock_nested(&inode_info->truncate_mutex, SMALL_AREA);

	if (is_vdfs2_inode_flag_set(inode, TINY_FILE) ||
				is_vdfs2_inode_flag_set(inode, SMALL_FILE)) {
		if (create)
			BUG();
		else
			goto exit_no_error_quota;
	}

	/* get extent contains iblock*/
	err = vdfs2_get_iblock_extent(&inode_info->vfs_inode, iblock, &extent,
			&offset_alloc_hint);

	if (err)
		goto exit_no_error_quota;

	if (extent.first_block)
		goto done;

	if (!create)
		goto exit_no_error_quota;

	if (da) {
		if(!vdfs2_runtime_extent_exists(iblock,
				&inode_info->runtime_extents))
			BUG();
	} else {
		if (buffer_delay(bh_result))
			BUG();
	}
#ifdef CONFIG_VDFS2_QUOTA
	if (!da && inode_info->quota_index != -1) {
		if (sbi->quotas[inode_info->quota_index].has +
				(count << sbi->block_size_shift) >
				sbi->quotas[inode_info->quota_index].max) {
			err = -ENOSPC;
			goto exit_no_error_quota;
		} else {
			sbi->quotas[inode_info->quota_index].has +=
					(count << sbi->block_size_shift);
			update_has_quota(sbi,
				sbi->quotas[inode_info->quota_index].ino,
				inode_info->quota_index);
		}
	}
#endif
	extent.block_count = count;
	extent.first_block = vdfs2_fsm_get_free_block(sbi, offset_alloc_hint,
			&extent.block_count, 0, 1, da);

	if (!extent.first_block) {
		err = -ENOSPC;
		goto exit;
	}

	extent.iblock = iblock;
	if (da)
		err = vdfs2_runtime_extent_del(extent.iblock,
			&inode_info->runtime_extents);
	if (!err)
		err = insert_extent(inode_info, &extent, 1);
	if (err) {
		vdfs2_fsm_put_free_block(inode_info, extent.first_block,
			extent.block_count, da);
		goto exit;
	}

	alloc = 1;

done:
	res_block = extent.first_block + (iblock - extent.iblock);
	max_blocks = extent.block_count - (iblock - extent.iblock);
	BUG_ON(res_block > extent.first_block + extent.block_count);

	if (res_block >
		(sb->s_bdev->bd_inode->i_size >> sbi->block_size_shift)) {
		if (!is_sbi_flag_set(sbi, IS_MOUNT_FINISHED)) {
			VDFS2_ERR("Block beyond block bound requested");
			err = -EFAULT;
			goto exit;
		} else {
			BUG();
		}
	}

	mutex_unlock(&inode_info->truncate_mutex);
	clear_buffer_new(bh_result);
	map_bh(bh_result, inode->i_sb, res_block);
	bh_result->b_size = sb->s_blocksize * min(max_blocks, buffer_size);

	if (alloc)
		set_buffer_new(bh_result);
	return 0;
exit:
#ifdef CONFIG_VDFS2_QUOTA
	if (!da && inode_info->quota_index != -1) {
		sbi->quotas[inode_info->quota_index].has -=
				(count << sbi->block_size_shift);
		update_has_quota(sbi, sbi->quotas[inode_info->quota_index].ino,
				inode_info->quota_index);
	}
#endif
exit_no_error_quota:
	mutex_unlock(&inode_info->truncate_mutex);
	return err;

}

/**
 * @brief				Logical to physical block numbers
 *					translation.
 * @param [in]		inode		Pointer to inode structure.
 * @param [in]		iblock		Requested logical block number.
 * @param [in, out]	bh_result	Pointer to buffer_head.
 * @param [in]		create		"Expand file allowed" flag.
 * @return			Returns physical block number,
 *					0 if ENOSPC
 */
int vdfs2_get_block(struct inode *inode, sector_t iblock,
	struct buffer_head *bh_result, int create)
{
	return vdfs2_get_int_block(inode, iblock, bh_result, create, 0);
}

int vdfs2_get_block_da(struct inode *inode, sector_t iblock,
	struct buffer_head *bh_result, int create)
{
	return vdfs2_get_int_block(inode, iblock, bh_result, create, 1);
}

int vdfs2_get_block_bug(struct inode *inode, sector_t iblock,
	struct buffer_head *bh_result, int create)
{
	BUG();
	return 0;
}

static int vdfs2_releasepage(struct page *page, gfp_t gfp_mask)
{
	if (!page_has_buffers(page))
		return 0;

	if (buffer_delay(page_buffers(page)))
		return 0;

	return try_to_free_buffers(page);
}

/**
 * @brief		Read page function.
 * @param [in]	file	Pointer to file structure
 * @param [out]	page	Pointer to page structure
 * @return		Returns error codes
 */
static int vdfs2_readpage(struct file *file, struct page *page)
{
	struct inode *inode = page->mapping->host;
	struct vdfs2_inode_info *inode_info = VDFS2_I(inode);
	int ret;
	/* check TINY and SMALL flag twice, first time without lock,
	and second time with lock, this is made for perfomance reason */
	if (is_vdfs2_inode_flag_set(inode, TINY_FILE) ||
		is_vdfs2_inode_flag_set(inode, SMALL_FILE))
		if (page->index == 0) {
			mutex_lock(&inode_info->truncate_mutex);
			if (is_vdfs2_inode_flag_set(inode, TINY_FILE) ||
			is_vdfs2_inode_flag_set(inode, SMALL_FILE)) {
				ret = read_tiny_small_page(page);
				mutex_unlock(&inode_info->truncate_mutex);
			} else {
				mutex_unlock(&inode_info->truncate_mutex);
				goto exit;
			}
			/* if there is error, print DEBUG INFO */
#if defined(CONFIG_VDFS2_DEBUG)
			if (ret)
				VDFS2_ERR("err = %d, ino#%lu name=%s,"
					"page index: %lu", ret, inode->i_ino,
					inode_info->name, page->index);
#endif
			return ret;
		}
exit:
	ret = mpage_readpage(page, vdfs2_get_block);
	/* if there is error, print DEBUG iNFO */
#if defined(CONFIG_VDFS2_DEBUG)
	if (ret)
		VDFS2_ERR("err = %d, ino#%lu name=%s, page index: %lu",
			ret, inode->i_ino, inode_info->name, page->index);
#endif
	return ret;
}


/**
 * @brief		Read page function.
 * @param [in]	file	Pointer to file structure
 * @param [out]	page	Pointer to page structure
 * @return		Returns error codes
 */
static int vdfs2_readpage_special(struct file *file, struct page *page)
{
	BUG();
}

/**
 * @brief			Read multiple pages function.
 * @param [in, out]	ractl	Readahead control
 */
static void vdfs2_readahead(struct readahead_control *ractl)
{
	struct inode *inode = ractl->mapping->host;
	if (is_vdfs2_inode_flag_set(inode, TINY_FILE) ||
		is_vdfs2_inode_flag_set(inode, SMALL_FILE)) {
		vdfs2_readahead_tinysmall(ractl);
	} else
		mpage_readahead(ractl, vdfs2_get_block);
}

static void vdfs2_readahead_special(struct readahead_control *ractl)
{
	BUG();
}


/**
 * @brief		Update all metadata.
 * @param [in]	sbi		Pointer to superblock information
 * @return		Returns error codes
 */
int vdfs2_update_metadata(struct vdfs2_sb_info *sbi)
{
	int error;

	sbi->snapshot_info->flags = 1;
	error = vdfs2_sync_metadata(sbi);
	sbi->snapshot_info->flags = 0;

	return error;
}


/**
 * @brief		Write pages.
 * @param [in]	page	List of pages
 * @param [in]	wbc		Write back control array
 * @return		Returns error codes
 */
static int vdfs2_writepage(struct page *page, struct writeback_control *wbc)
{
	struct inode *inode = page->mapping->host;
	struct buffer_head *bh;
	int ret = 0;

	if (is_vdfs2_inode_flag_set(inode, TINY_FILE) ||
			is_vdfs2_inode_flag_set(inode, SMALL_FILE))
		ret = write_tiny_small_page(page, wbc);
	if (ret)
		goto exit;

	if (page->mapping->host->i_ino > VDFS2_LSFILE)
		goto write;

	set_page_dirty(page);
	return AOP_WRITEPAGE_ACTIVATE;

write:
	if (!page_has_buffers(page))
		goto redirty_page;

	bh = page_buffers(page);
	if ((!buffer_mapped(bh) || buffer_delay(bh)) && buffer_dirty(bh))
		goto redirty_page;

	ret = block_write_full_page(page, vdfs2_get_block_bug, wbc);
exit:
	if (ret)
		VDFS2_ERR("err = %d, ino#%lu name=%s, page index: %lu, "
				" wbc->sync_mode = %d", ret, inode->i_ino,
				VDFS2_I(inode)->name, page->index,
				wbc->sync_mode);
	return ret;
redirty_page:
	redirty_page_for_writepage(wbc, page);
	unlock_page(page);
	return 0;
}

int vdfs2_allocate_space(struct vdfs2_inode_info *inode_info)
{
	struct list_head *ptr;
	struct list_head *next;
	struct vdfs2_runtime_extent_info *entry;
	struct inode *inode = &inode_info->vfs_inode;
	struct vdfs2_sb_info *sbi = inode->i_sb->s_fs_info;
	u32 count = 0;
	struct vdfs2_extent_info extent;
	int err = 0;

	if (list_empty(&inode_info->runtime_extents))
		return 0;
	memset(&extent, 0x0, sizeof(extent));
	list_for_each_safe(ptr, next, &inode_info->runtime_extents) {
		entry = list_entry(ptr, struct vdfs2_runtime_extent_info, list);
again:
		count = entry->block_count;

		extent.first_block = vdfs2_fsm_get_free_block(sbi, entry->
				alloc_hint, &count, 0, 1, 1);

		if (!extent.first_block) {
			/* it shouldn't happen because space
			 * was reserved early in write_iter */
			BUG();
			goto exit;
		}

		extent.iblock = entry->iblock;
		extent.block_count = count;
		err = insert_extent(inode_info, &extent, 0);
		if (err) {
			vdfs2_fsm_put_free_block(inode_info,
				extent.first_block, extent.block_count, 1);
			goto exit;
		}
		entry->iblock += count;
		entry->block_count -= count;
		/* if we still have blocks in the chunk */
		if (entry->block_count)
			goto again;
		else {
			list_del(&entry->list);
			kfree(entry);
		}
	}
exit:
	if (!err) {
#ifdef CONFIG_VDFS2_QUOTA
		if (inode_info->quota_index != -1)
			update_has_quota(sbi,
				sbi->quotas[inode_info->quota_index].ino,
				inode_info->quota_index);
#endif
		err = vdfs2_write_inode_to_bnode(inode);
	}
	return err;
}

int vdfs2_mpage_writepages(struct address_space *mapping,
		struct writeback_control *wbc)
{
	struct inode *inode = mapping->host;
	int ret;
	struct blk_plug plug;
	struct vdfs2_sb_info *sbi = inode->i_sb->s_fs_info;
	struct vdfs2_inode_info *inode_info = VDFS2_I(inode);
	struct vdfs2_mpage_data mpd = {
			.bio = NULL,
			.last_block_in_bio = 0,
	};
	vdfs2_start_transaction(sbi);
	/* dont allocate space for packtree inodes */
	if ((inode_info->record_type == VDFS2_CATALOG_FILE_RECORD) ||
		(inode_info->record_type == VDFS2_CATALOG_HLINK_RECORD)) {
		/* if we have runtime extents, allocate space on volume*/
		if (!list_empty(&inode_info->runtime_extents)) {
			mutex_lock(&inode_info->truncate_mutex);
			ret = vdfs2_allocate_space(inode_info);
			mutex_unlock(&inode_info->truncate_mutex);
		}
	}
	blk_start_plug(&plug);
	/* write dirty pages */
	ret = write_cache_pages(mapping, wbc, vdfs2_mpage_writepage, &mpd);
	if (mpd.bio)
		vdfs2_mpage_bio_submit(mpd.bio);
	blk_finish_plug(&plug);
	vdfs2_stop_transaction(sbi);
	return ret;
}
/**
 * @brief		Write some dirty pages.
 * @param [in]	mapping	Address space mapping (holds pages)
 * @param [in]	wbc		Writeback control - how many pages to write
 *			and write mode
 * @return		Returns 0 on success, errno on failure
 */

static int vdfs2_writepages(struct address_space *mapping,
		struct writeback_control *wbc)
{
	struct inode *inode = mapping->host;
	int err = 0;
	if (is_vdfs2_inode_flag_set(inode, TINY_FILE) ||
		is_vdfs2_inode_flag_set(inode, SMALL_FILE))
		err = writepage_tiny_small(mapping, wbc);
	else
		err = vdfs2_mpage_writepages(mapping, wbc);
	/* if there is error, print DEBUG iNFO */
#if defined(CONFIG_VDFS2_DEBUG)
	if (err)
		VDFS2_ERR("err = %d, ino#%lu name=%s, "
				"wbc->sync_mode = %d", err, inode->i_ino,
				VDFS2_I(inode)->name, wbc->sync_mode);
#endif
	return err;
}

/**
 * @brief		Write some dirty pages.
 * @param [in]	mapping	Address space mapping (holds pages)
 * @param [in]	wbc		Writeback control - how many pages to write
 *			and write mode
 * @return		Returns 0 on success, errno on failure
 */
static int vdfs2_writepages_special(struct address_space *mapping,
		struct writeback_control *wbc)
{
	return 0;
}
/**
 * @brief		Write begin with snapshots.
 * @param [in]	file	Pointer to file structure
 * @param [in]	mapping Address of pages mapping
 * @param [in]	pos		Position
 * @param [in]	len		Length
 * @param [in]	flags	Flags
 * @param [in]	pagep	Pages array
 * @param [in]	fs_data	Data array
 * @return		Returns error codes
 */
static int vdfs2_write_begin(struct file *file, struct address_space *mapping,
			loff_t pos, unsigned len, unsigned flags,
			struct page **pagep, void **fsdata)
{
	int rc = 0;
	vdfs2_start_transaction(VDFS2_SB(mapping->host->i_sb));
#if LINUX_VERSION_CODE == KERNEL_VERSION(2, 6, 35)
	*pagep = NULL;
	rc = block_write_begin(file, mapping, pos, len, flags, pagep,
		NULL, vdfs2_get_block_prep_da);
#elif LINUX_VERSION_CODE == KERNEL_VERSION(3, 0, 20) ||\
		LINUX_VERSION_CODE == KERNEL_VERSION(3, 0, 33) ||\
		LINUX_VERSION_CODE >= KERNEL_VERSION(3, 8, 5)
	rc = block_write_begin(mapping, pos, len, flags, pagep,
		vdfs2_get_block_prep_da);
#else
	BUILD_BUG();
#endif
	if (rc)
		vdfs2_stop_transaction(VDFS2_SB(mapping->host->i_sb));
	return rc;
}

/**
 * @brief		TODO Write begin with snapshots.
 * @param [in]	file	Pointer to file structure
 * @param [in]	mapping	Address of pages mapping
 * @param [in]	pos		Position
 * @param [in]	len		Length
 * @param [in]	copied	Whould it be copied
 * @param [in]	page	Page pointer
 * @param [in]	fs_data	Data
 * @return		Returns error codes
 */
static int vdfs2_write_end(struct file *file, struct address_space *mapping,
			loff_t pos, unsigned len, unsigned copied,
			struct page *page, void *fsdata)
{
	struct inode *inode = mapping->host;
	int i_size_changed = 0;

	copied = block_write_end(file, mapping, pos, len, copied, page, fsdata);
	mutex_lock(&VDFS2_I(inode)->truncate_mutex);
	/*
	 * No need to use i_size_read() here, the i_size
	 * cannot change under us because we hold i_mutex.
	 *
	 * But it's important to update i_size while still holding page lock:
	 * page write out could otherwise come in and zero beyond i_size.
	 */
	if (pos+copied > inode->i_size) {
		i_size_write(inode, pos+copied);
		i_size_changed = 1;
	}

	unlock_page(page);
	put_page(page);

	if (i_size_changed)
		vdfs2_write_inode_to_bnode(inode);
	mutex_unlock(&VDFS2_I(inode)->truncate_mutex);
	vdfs2_stop_transaction(VDFS2_SB(inode->i_sb));
	return copied;
}

/**
 * @brief		Called during file opening process.
 * @param [in]	inode	Pointer to inode information
 * @param [in]	file	Pointer to file structure
 * @return		Returns error codes
 */
static int vdfs2_file_open(struct inode *inode, struct file *filp)
{
	int rc = generic_file_open(inode, filp);
	struct vdfs2_inode_info *inode_info = VDFS2_I(inode);
	if (rc)
		return rc;
	atomic_inc(&(inode_info->open_count));
	return 0;
}

/**
 * @brief		Release file.
 * @param [in]	inode	Pointer to inode information
 * @param [in]	file	Pointer to file structure
 * @return		Returns error codes
 */
static int vdfs2_file_release(struct inode *inode, struct file *file)
{
	struct vdfs2_inode_info *inode_info = VDFS2_I(inode);
	VDFS2_DEBUG_INO("#%lu", inode->i_ino);
	atomic_dec(&(inode_info->open_count));
	return 0;
}

/**
 * @brief		Function mkdir.
 * @param [in]	dir	Pointer to inode
 * @param [in]	dentry	Pointer to directory entry
 * @param [in]	mode	Mode of operation
 * @return		Returns error codes
 */
#if LINUX_VERSION_CODE >= KERNEL_VERSION(3, 8, 5)
static int vdfs2_mkdir(struct user_namespace *mnt_userns, struct inode *dir,
			struct dentry *dentry, umode_t mode)
#else
static int vdfs2_mkdir(struct inode *dir, struct dentry *dentry, int mode)
#endif
{
	return vdfs2_create(mnt_userns, dir, dentry, S_IFDIR | mode, NULL);
}

/**
 * @brief		Function rmdir.
 * @param [in]	dir	Pointer to inode
 * @param [in]	dentry	Pointer to directory entry
 * @return		Returns error codes
 */
static int vdfs2_rmdir(struct inode *dir, struct dentry *dentry)
{
	if (dentry->d_inode->i_size)
		return -ENOTEMPTY;

	return vdfs2_unlink(dir, dentry);
}

static int vdfs2_truncate_pages(struct inode *inode, loff_t newsize)
{
	int error = 0;

	error = inode_newsize_ok(inode, newsize);
	if (error)
		goto exit;

	if (!(S_ISREG(inode->i_mode) || S_ISDIR(inode->i_mode) ||
			S_ISLNK(inode->i_mode))) {
		error = -EINVAL;
		goto exit;
	}

	if (IS_APPEND(inode) || IS_IMMUTABLE(inode)) {
		error = -EPERM;
		goto exit;
	}

	error = block_truncate_page(inode->i_mapping, newsize,
			vdfs2_get_block);
exit:
	return error;
}


static int vdfs2_update_inode(struct inode *inode, loff_t newsize)
{
	int error = 0;
	loff_t oldsize = inode->i_size;

	if (newsize < oldsize) {
		if (is_vdfs2_inode_flag_set(inode, TINY_FILE)) {
			VDFS2_I(inode)->tiny.len = newsize;
			VDFS2_I(inode)->tiny.i_size = newsize;
		} else if (is_vdfs2_inode_flag_set(inode, SMALL_FILE)) {
			VDFS2_I(inode)->small.len = newsize;
			VDFS2_I(inode)->small.i_size = newsize;
		} else
			error = vdfs2_truncate_blocks(inode, newsize);
	} else {
		if (is_vdfs2_inode_flag_set(inode, TINY_FILE))
			VDFS2_I(inode)->tiny.i_size = newsize;
		else if (is_vdfs2_inode_flag_set(inode, SMALL_FILE))
			VDFS2_I(inode)->small.i_size = newsize;
	}

	if (!error)
		i_size_write(inode, newsize);
	else
		return error;


	inode->i_mtime = inode->i_ctime =
			current_time(inode);

	return error;
}

/**
 * @brief		Set attributes.
 * @param [in]	dentry	Pointer to directory entry
 * @param [in]	iattr	Attributes to be set
 * @return		Returns error codes
 */
static int vdfs2_setattr(struct user_namespace *mnt_userns,
			struct dentry *dentry, struct iattr *iattr)
{
	struct inode *inode = dentry->d_inode;
	int error = 0;
#ifdef CONFIG_VDFS2_QUOTA
	struct vdfs2_sb_info *sbi = inode->i_sb->s_fs_info;
#endif

	vdfs2_start_transaction(VDFS2_SB(inode->i_sb));
	error = setattr_prepare(&init_user_ns, dentry, iattr);
	if (error)
		goto exit;
#ifdef CONFIG_VDFS2_QUOTA
	if (is_vdfs2_inode_flag_set(inode, SMALL_FILE) &&
			VDFS2_I(inode)->quota_index != -1 &&
			sbi->quotas[VDFS2_I(inode)->quota_index].has +
			sbi->small_area->cell_size >
			sbi->quotas[VDFS2_I(inode)->quota_index].max) {

		error = -ENOSPC;
		goto exit;
	}
#endif
	if ((iattr->ia_valid & ATTR_SIZE) &&
			iattr->ia_size != i_size_read(inode)) {
		error = vdfs2_truncate_pages(inode, iattr->ia_size);
		if (error)
			goto exit;

		truncate_pagecache(inode, iattr->ia_size);

		mutex_lock(&VDFS2_I(inode)->truncate_mutex);
		error = vdfs2_update_inode(inode, iattr->ia_size);
		if (error)
			goto exit_unlock;
	} else
		mutex_lock(&VDFS2_I(inode)->truncate_mutex);

	setattr_copy(&init_user_ns, inode, iattr);

	error = vdfs2_write_inode_to_bnode(inode);

exit_unlock:
	mutex_unlock(&VDFS2_I(inode)->truncate_mutex);
exit:
	vdfs2_stop_transaction(VDFS2_SB(inode->i_sb));

	return error;
}

/**
 * @brief		Make bmap.
 * @param [in]	mapping	Address of pages mapping
 * @param [in]	block	Block number
 * @return		TODO Returns 0 on success, errno on failure
 */
static sector_t vdfs2_bmap(struct address_space *mapping, sector_t block)
{
	return generic_block_bmap(mapping, block, vdfs2_get_block);
}

/**
 * @brief			Function rename.
 * @param [in]	old_dir		Pointer to old dir struct
 * @param [in]	old_dentry	Pointer to old dir entry struct
 * @param [in]	new_dir		Pointer to new dir struct
 * @param [in]	new_dentry	Pointer to new dir entry struct
 * @return			Returns error codes
 */
static int vdfs2_rename(struct user_namespace *mnt_userns,
		struct inode *old_dir, struct dentry *old_dentry,
		struct inode *new_dir, struct dentry *new_dentry,
		unsigned int flags)
{
	struct vdfs2_find_data fd;
	struct vdfs2_sb_info *sbi = old_dir->i_sb->s_fs_info;
	struct inode *mv_inode = old_dentry->d_inode;
	int ret = 0;
	struct vdfs2_cattree_key *rm_key = NULL, *key = NULL;
	if (flags)
		return -EINVAL;
	if (check_permissions(sbi))
		return -EINTR;
	if (new_dentry->d_name.len > VDFS2_CAT_MAX_NAME)
		return -ENAMETOOLONG;
	if (new_dentry->d_inode) {
		struct inode *new_dentry_inode = new_dentry->d_inode;
		if (S_ISDIR(new_dentry_inode->i_mode) &&
				new_dentry_inode->i_size > 0)
			return -ENOTEMPTY;

		ret = vdfs2_unlink(new_dir, new_dentry);
		if (ret)
			return ret;
	}

	vdfs2_start_transaction(sbi);
	/* Find old record */
	VDFS2_DEBUG_MUTEX("cattree mutex w lock");
	mutex_lock(&VDFS2_I(mv_inode)->truncate_mutex);
	mutex_w_lock(sbi->catalog_tree->rw_tree_lock);
	VDFS2_DEBUG_MUTEX("cattree mutex w lock succ");

	fd.bnode = NULL;
	ret = vdfs2_cattree_find_old(sbi->catalog_tree, old_dir->i_ino,
			(char *) old_dentry->d_name.name,
			old_dentry->d_name.len, &fd, VDFS2_BNODE_MODE_RW);
	if (ret) {
		VDFS2_DEBUG_MUTEX("cattree mutex w lock un");
		mutex_w_unlock(sbi->catalog_tree->rw_tree_lock);
		mutex_unlock(&VDFS2_I(mv_inode)->truncate_mutex);
		goto exit;
	}

	rm_key = vdfs2_get_btree_record(fd.bnode, fd.pos);
	if (IS_ERR(rm_key)) {
		mutex_w_unlock(sbi->catalog_tree->rw_tree_lock);
		mutex_unlock(&VDFS2_I(mv_inode)->truncate_mutex);
		ret = PTR_ERR(rm_key);
		goto exit;
	}

	/* found_key is a part of bnode, wich we are going to modify,
	 * so we have to copy save its current value */
	key = kzalloc(le32_to_cpu(rm_key->gen_key.key_len), GFP_KERNEL);
	if (!key) {
		vdfs2_put_bnode(fd.bnode);
		VDFS2_DEBUG_MUTEX("cattree mutex w lock un");
		mutex_w_unlock(sbi->catalog_tree->rw_tree_lock);
		mutex_unlock(&VDFS2_I(mv_inode)->truncate_mutex);
		ret = -ENOMEM;
		goto exit;
	}
	memcpy(key, rm_key, le32_to_cpu(rm_key->gen_key.key_len));
	vdfs2_put_bnode(fd.bnode);


	ret = vdfs2_add_new_cattree_object(mv_inode, new_dir->i_ino,
			&new_dentry->d_name);
	if (ret) {
		/* TODO change to some erroneous action */
		VDFS2_DEBUG_INO("can not rename #%d; old_dir_ino=%lu "
				"oldname=%s new_dir_ino=%lu newname=%s",
				ret,
				old_dir->i_ino, old_dentry->d_name.name,
				new_dir->i_ino, new_dentry->d_name.name);
		kfree(key);
		mutex_w_unlock(sbi->catalog_tree->rw_tree_lock);
		mutex_unlock(&VDFS2_I(mv_inode)->truncate_mutex);
		goto exit;
	}

	ret = vdfs2_btree_remove(sbi->catalog_tree, &key->gen_key);
	kfree(key);
	if (ret)
		/* TODO change to some erroneous action */
		VDFS2_BUG();
	if (!(is_vdfs2_inode_flag_set(mv_inode, HARD_LINK))) {
		char *saved_name;
		VDFS2_I(mv_inode)->parent_id = new_dir->i_ino;
		kfree(VDFS2_I(mv_inode)->name);
		saved_name = kzalloc(new_dentry->d_name.len + 1, GFP_KERNEL);
		if (!saved_name) {
			iput(mv_inode);
			ret = -ENOMEM;
			mutex_w_unlock(sbi->catalog_tree->rw_tree_lock);
			mutex_unlock(&VDFS2_I(mv_inode)->truncate_mutex);
			goto exit;
		}

		strncpy(saved_name, new_dentry->d_name.name,
				new_dentry->d_name.len + 1);
		VDFS2_I(mv_inode)->name = saved_name;
	}



	VDFS2_DEBUG_MUTEX("cattree mutex w lock un");
	mutex_w_unlock(sbi->catalog_tree->rw_tree_lock);
	mutex_unlock(&VDFS2_I(mv_inode)->truncate_mutex);

	mutex_lock(&VDFS2_I(old_dir)->truncate_mutex);
	if (old_dir->i_size != 0)
		old_dir->i_size--;
	else
		VDFS2_DEBUG_INO("Files count mismatch");

	ret = vdfs2_write_inode_to_bnode(old_dir);
	mutex_unlock(&VDFS2_I(old_dir)->truncate_mutex);
	if (ret)
		goto exit;

	mutex_lock(&VDFS2_I(new_dir)->truncate_mutex);
	new_dir->i_size++;
	ret = vdfs2_write_inode_to_bnode(new_dir);
	mutex_unlock(&VDFS2_I(new_dir)->truncate_mutex);
	if (ret)
		goto exit;

	mv_inode->i_ctime = current_time(mv_inode);
exit:
	vdfs2_stop_transaction(sbi);
	return ret;
}

/**
 * @brief			Create hardlink record .
 * @param [in]	hlink_id	Hardlink id
 * @param [in]	par_ino_n	Parent inode number
 * @param [in]	name		Name
 * @return			Returns pointer to buffer with hardlink
 */
static void *form_hlink_record(ino_t ino_n, ino_t par_ino_n, umode_t file_mode,
		struct qstr *name)
{
	void *record = vdfs2_alloc_cattree_key(name->len,
			VDFS2_CATALOG_HLINK_RECORD);
	struct vdfs2_catalog_hlink_record *hlink_val;

	if (!record)
		return ERR_PTR(-ENOMEM);

	vdfs2_fill_cattree_key(record, par_ino_n, name->name, name->len);
	hlink_val = get_value_pointer(record);
	hlink_val->object_id = cpu_to_le64(ino_n);
	hlink_val->file_mode = cpu_to_le16(file_mode);

	return record;
}

/**
 * @brief			Add hardlink record .
 * @param [in]	cat_tree	Pointer to catalog tree
 * @param [in]	hlink_id	Hardlink id
 * @param [in]	par_ino_n	Parent inode number
 * @param [in]	name		Name
 * @return			Returns error codes
 */
static int add_hlink_record(struct vdfs2_btree *cat_tree, ino_t ino_n,
		ino_t par_ino_n, umode_t file_mode, struct qstr *name)
{
	struct vdfs2_catalog_hlink_record *hlink_value;
	struct vdfs2_cattree_record *record;

	record = vdfs2_cattree_place_record(cat_tree, par_ino_n, name->name,
			name->len, VDFS2_CATALOG_HLINK_RECORD);
	if (IS_ERR(record))
		return PTR_ERR(record);

	hlink_value = (struct vdfs2_catalog_hlink_record *)record->val;
	hlink_value->file_mode = cpu_to_le16(file_mode);
	hlink_value->object_id = cpu_to_le64(ino_n);

	vdfs2_release_dirty_record((struct vdfs2_btree_gen_record *) record);
	return 0;
}

/**
 * @brief       Transform record from regular file to hard link
 *              Resulting record length stays unchanged, but only a part of the
 *              record is used for real data
 * */

static int transform_record_into_hlink(struct vdfs2_cattree_key *record,
		ino_t ino, ino_t par_ino, umode_t file_mode, struct qstr *name)
{
	struct vdfs2_cattree_key *hlink_rec;
	size_t bytes_to_copy;

	hlink_rec = form_hlink_record(ino, par_ino, file_mode, name);

	if (IS_ERR(hlink_rec))
		return -ENOMEM;

	VDFS2_BUG_ON(record->gen_key.record_len <=
			hlink_rec->gen_key.record_len);

	bytes_to_copy = le32_to_cpu(hlink_rec->gen_key.record_len);
	hlink_rec->gen_key.record_len = record->gen_key.record_len;

	memcpy(record, hlink_rec, le32_to_cpu(bytes_to_copy));

	kfree(hlink_rec);

	return 0;
}

/**
 * @brief			Create link.
 * @param [in]	old_dentry	Old dentry (source name for hard link)
 * @param [in]	dir		The inode dir pointer
 * @param [out]	dentry		Pointer to result dentry
 * @return			Returns error codes
 */
static int vdfs2_link(struct dentry *old_dentry, struct inode *dir,
	struct dentry *dentry)
{
	struct inode *inode = old_dentry->d_inode;
	struct inode *par_inode = old_dentry->d_parent->d_inode;

	struct vdfs2_sb_info *sbi = old_dentry->d_inode->i_sb->s_fs_info;
	struct vdfs2_cattree_key *found_key;
	struct vdfs2_cattree_record *record;
	int ret;
	if (check_permissions(sbi))
		return -EINTR;
	if (dentry->d_name.len > VDFS2_CAT_MAX_NAME)
		return -ENAMETOOLONG;

	if (inode->i_nlink >= VDFS2_LINK_MAX)
		return -EMLINK;

	vdfs2_start_transaction(sbi);

	mutex_lock(&VDFS2_I(inode)->truncate_mutex);

	VDFS2_DEBUG_MUTEX("cattree mutex w lock");
	mutex_w_lock(sbi->catalog_tree->rw_tree_lock);
	VDFS2_DEBUG_MUTEX("cattree mutex w lock succ");

	record = vdfs2_cattree_find(sbi->catalog_tree, par_inode->i_ino,
			old_dentry->d_name.name, old_dentry->d_name.len,
			VDFS2_BNODE_MODE_RW);

	if (IS_ERR(record)) {
		ret = PTR_ERR(record);
		goto err_exit;
	}


	if (record->key->record_type != VDFS2_CATALOG_HLINK_RECORD) {
		/* move inode metadata into hardlink btree */
		ret = vdfs2_hard_link_insert(sbi,
			(struct vdfs2_catalog_file_record *)record->val);

		if (ret) {
			vdfs2_release_record((struct vdfs2_btree_gen_record *)
					record);
			goto err_exit;
		}
		found_key = record->key;

		ret = transform_record_into_hlink(found_key, inode->i_ino,
				par_inode->i_ino, par_inode->i_mode,
				&old_dentry->d_name);

		if (ret) {
			vdfs2_hard_link_remove(sbi, par_inode->i_ino);
			vdfs2_release_record((struct vdfs2_btree_gen_record *)
					record);
			goto err_exit;
		}

		kfree(VDFS2_I(inode)->name);
		VDFS2_I(inode)->name = NULL;
		set_vdfs2_inode_flag(inode, HARD_LINK);
		vdfs2_release_dirty_record(
				(struct vdfs2_btree_gen_record *) record);

	} else
		vdfs2_release_record((struct vdfs2_btree_gen_record *) record);

	/*   */
	ret = add_hlink_record(sbi->catalog_tree, inode->i_ino, dir->i_ino,
			inode->i_mode, &dentry->d_name);
	if (ret)
		goto err_exit;

	VDFS2_DEBUG_MUTEX("cattree mutex w lock un");
	mutex_w_unlock(sbi->catalog_tree->rw_tree_lock);

	inode->i_ctime = current_time(inode);


#if LINUX_VERSION_CODE == KERNEL_VERSION(2, 6, 35)
	atomic_inc(&inode->i_count);
#elif LINUX_VERSION_CODE == KERNEL_VERSION(3, 0, 20) ||\
		LINUX_VERSION_CODE == KERNEL_VERSION(3, 0, 33) ||\
		LINUX_VERSION_CODE >= KERNEL_VERSION(3, 8, 5)
	ihold(inode);
#endif

	d_instantiate(dentry, inode);
	inode_inc_link_count(inode);

	ret = vdfs2_write_inode_to_bnode(inode);
	mutex_unlock(&VDFS2_I(inode)->truncate_mutex);
	if (ret)
		goto exit;

	mutex_lock(&VDFS2_I(dir)->truncate_mutex);
	dir->i_ctime = current_time(dir);
	dir->i_mtime = current_time(dir);
	dir->i_size++;
	ret = vdfs2_write_inode_to_bnode(dir);
	mutex_unlock(&VDFS2_I(dir)->truncate_mutex);
	if (ret)
		goto exit;

	sbi->files_count++;
exit:
	vdfs2_stop_transaction(sbi);
	return ret;
err_exit:
	VDFS2_DEBUG_MUTEX("cattree mutex w lock un");
	mutex_w_unlock(sbi->catalog_tree->rw_tree_lock);
	mutex_unlock(&VDFS2_I(inode)->truncate_mutex);
	goto exit;
}

/**
 * @brief			Make node.
 * @param [in,out]	dir		Directory where node will be created
 * @param [in]		dentry	Created dentry
 * @param [in]		mode	Mode for file
 * @param [in]		rdev	Device
 * @return			Returns 0 on success, errno on failure
 */
#if LINUX_VERSION_CODE >= KERNEL_VERSION(3, 8, 5)
static int vdfs2_mknod(struct user_namespace *mnt_userns, struct inode *dir,
			struct dentry *dentry, umode_t mode, dev_t rdev)
#else
static int vdfs2_mknod(struct inode *dir, struct dentry *dentry,
			int mode, dev_t rdev)
#endif
{
	struct inode *created_ino;
	int ret;

	vdfs2_start_transaction(VDFS2_SB(dir->i_sb));

	ret = vdfs2_create(mnt_userns, dir, dentry, mode, NULL);
	if (ret)
		goto exit;

	created_ino = dentry->d_inode;
	init_special_inode(created_ino, created_ino->i_mode, rdev);
	mutex_lock(&VDFS2_I(created_ino)->truncate_mutex);
	ret = vdfs2_write_inode_to_bnode(created_ino);
	mutex_unlock(&VDFS2_I(created_ino)->truncate_mutex);
exit:
	vdfs2_stop_transaction(VDFS2_SB(dir->i_sb));
	return ret;
}

/**
 * @brief			Make symlink.
 * @param [in,out]	dir		Directory where node will be created
 * @param [in]		dentry	Created dentry
 * @param [in]		symname Symbolic link name
 * @return			Returns 0 on success, errno on failure
 */
static int vdfs2_symlink(struct user_namespace *mnt_userns, struct inode *dir,
			struct dentry *dentry, const char *symname)
{
	int ret;
	struct inode *created_ino;
	unsigned int len = strlen(symname);

	if ((len > VDFS2_FULL_PATH_LEN) ||
			(dentry->d_name.len > VDFS2_CAT_MAX_NAME))
		return -ENAMETOOLONG;

	vdfs2_start_transaction(VDFS2_SB(dir->i_sb));

	ret = vdfs2_create(mnt_userns, dir, dentry, S_IFLNK | S_IRWXUGO, NULL);

	if (ret)
		goto exit;

	clear_vdfs2_inode_flag(dentry->d_inode, TINY_FILE);

	created_ino = dentry->d_inode;
	ret = page_symlink(created_ino, symname, ++len);
exit:
	vdfs2_stop_transaction(VDFS2_SB(dir->i_sb));
	return ret;
}

/**
 * The eMMCFS address space operations.
 */
const struct address_space_operations vdfs2_aops = {
	.readpage	= vdfs2_readpage,
	.readahead	= vdfs2_readahead,
	.writepage	= vdfs2_writepage,
	.writepages	= vdfs2_writepages,
#if LINUX_VERSION_CODE == KERNEL_VERSION(2, 6, 35)
	.sync_page	= block_sync_page,
#endif
	.write_begin	= vdfs2_write_begin,
	.write_end	= vdfs2_write_end,
	.bmap		= vdfs2_bmap,
	.migratepage	= buffer_migrate_page,
	.releasepage = vdfs2_releasepage,
	.set_page_dirty = __set_page_dirty_buffers,

};

static const struct address_space_operations vdfs2_aops_special = {
	.readpage	= vdfs2_readpage_special,
	.readahead	= vdfs2_readahead_special,
	.writepage	= vdfs2_writepage,
	.writepages	= vdfs2_writepages_special,
#if LINUX_VERSION_CODE == KERNEL_VERSION(2, 6, 35)
	.sync_page	= block_sync_page,
#endif
	.write_begin	= vdfs2_write_begin,
	.write_end	= vdfs2_write_end,
	.bmap		= vdfs2_bmap,
	.set_page_dirty = __set_page_dirty_buffers,
};

/**
 * The eMMCFS directory inode operations.
 */
static const struct inode_operations vdfs2_dir_inode_operations = {
	/* d.voytik-TODO-19-01-2012-11-15-00:
	 * [vdfs2_dir_inode_ops] add to vdfs2_dir_inode_operations
	 * necessary methods */
	.create		= vdfs2_create,
	.symlink	= vdfs2_symlink,
	.lookup		= vdfs2_lookup,
	.link		= vdfs2_link,
	.unlink		= vdfs2_unlink,
	.mkdir		= vdfs2_mkdir,
	.rmdir		= vdfs2_rmdir,
	.mknod		= vdfs2_mknod,
	.rename		= vdfs2_rename,
	.setattr	= vdfs2_setattr,
	.listxattr	= vdfs2_listxattr,
};

/**
 * The eMMCFS symlink inode operations.
 */
static const struct inode_operations vdfs2_symlink_inode_operations = {
	.get_link	= page_get_link,
	.setattr	= vdfs2_setattr,
	.listxattr	= vdfs2_listxattr,
};


/**
 * The eMMCFS directory operations.
 */
const struct file_operations vdfs2_dir_operations = {
	/* d.voytik-TODO-19-01-2012-11-16-00:
	 * [vdfs2_dir_ops] add to vdfs2_dir_operations necessary methods */
	.llseek		= generic_file_llseek,
	.read		= generic_read_dir,
	.iterate	= vdfs2_readdir,
	.unlocked_ioctl = vdfs2_dir_ioctl,
};

/**
 * TODO
 */
#if LINUX_VERSION_CODE >= KERNEL_VERSION(3, 8, 5)
int vdfs2_file_fsync(struct file *file, loff_t start, loff_t end, int datasync)
#else
static int vdfs2_file_fsync(struct file *file, int datasync)
#endif
{
	struct super_block *sb = file->f_mapping->host->i_sb;
	int ret = 0;

#if LINUX_VERSION_CODE >= KERNEL_VERSION(3, 8, 5)
	ret = generic_file_fsync(file, start, end, datasync);
#else
	ret = generic_file_fsync(file, datasync);
#endif
	if (ret)
		return ret;

	ret = vdfs2_sync_fs(sb, 1);
	return (!ret || ret == -EINVAL) ? 0 : ret;
}

#if LINUX_VERSION_CODE == KERNEL_VERSION(2, 6, 35)
long vdfs2_fallocate(struct inode *inode, int mode, loff_t offset, loff_t len);
#elif LINUX_VERSION_CODE == KERNEL_VERSION(3, 0, 20) || \
		LINUX_VERSION_CODE == KERNEL_VERSION(3, 0, 33) ||\
		LINUX_VERSION_CODE >= KERNEL_VERSION(3, 8, 5)
long vdfs2_fallocate(struct file *file, int mode, loff_t offset, loff_t len);
#endif

#define MAX(a, b) ((a) > (b) ? (a) : (b))

/**
 * @brief		Calculation of writing position in a case when data is
 *			appending to a target file
 * @param [in]	iocb	Struct describing writing file
 * @param [in]	pos	Position to write from
 * @return		Returns the writing position
 */
static inline loff_t get_real_writing_position(struct kiocb *iocb, loff_t pos)
{
	loff_t write_pos = 0;
	if (iocb->ki_filp->f_flags & O_APPEND)
		write_pos = i_size_read(INODE(iocb));

	write_pos = MAX(write_pos, pos);
	iocb->ki_pos = write_pos;
	return write_pos;
}


/**
	iocb->ki_pos = write_pos;
 * @brief		VDFS2 function for write iter
 * @param [in]	iocb	Struct describing writing file
 * @param [in]	from	iov_iter to write from
 * @return		Returns number of bytes written or an error code
 */
static ssize_t vdfs2_file_write_iter(struct kiocb *iocb, struct iov_iter *from)
{
	struct inode *inode = INODE(iocb);
	struct super_block *sb = inode->i_sb;
	struct vdfs2_sb_info *sbi = sb->s_fs_info;
	int count = 5;
	ssize_t ret = 0;

	if (check_permissions((struct vdfs2_sb_info *)inode->i_sb->s_fs_info))
		return -EINTR;
again:
	/* We are trying to write iov_iter_count(from) bytes from iov->iov_base */
	/* The target file is tiny or small */
	if (is_vdfs2_inode_flag_set(inode, TINY_FILE) ||
		is_vdfs2_inode_flag_set(inode, SMALL_FILE)) {
		get_real_writing_position(iocb, iocb->ki_pos);
		ret = process_tiny_small_file(iocb, from);
	/* The target file is a normal file */
	} else
		ret = generic_file_write_iter(iocb, from);

	if (ret == -ENOSPC && sbi->next_free_blocks && count) {
		/* try to free released blocks */
		flush_delayed_work(&sbi->delayed_commit);
		count--;
		goto again;
	}
#if defined(CONFIG_VDFS2_DEBUG)
	if (IS_ERR_VALUE(ret))
		VDFS2_ERR("err = %d, ino#%lu name=%s",
			ret, inode->i_ino, VDFS2_I(inode)->name);
#endif
	return ret;
}

/**
 * @brief		VDFS2 function for read iter
 * @param [in]	iocb	Struct describing reading file
 * @param [out]	to	iov_iter to read to
 * @return		Returns number of bytes read or an error code
 */
static ssize_t vdfs2_file_read_iter(struct kiocb *iocb, struct iov_iter *to)
{
	struct inode *inode = INODE(iocb);
	ssize_t ret;
	if (check_permissions((struct vdfs2_sb_info *)inode->i_sb->s_fs_info))
		return -EINTR;

	ret = generic_file_read_iter(iocb, to);
	return ret;
}

/**
 * The eMMCFS file operations.
 */
static const struct file_operations vdfs2_file_operations = {
	.llseek		= generic_file_llseek,
	.read_iter	= vdfs2_file_read_iter,
	.write_iter	= vdfs2_file_write_iter,
	.mmap		= generic_file_mmap,
	.open		= vdfs2_file_open,
	.release	= vdfs2_file_release,
	.fsync		= vdfs2_file_fsync,
	.unlocked_ioctl = vdfs2_ioctl,
#if LINUX_VERSION_CODE == KERNEL_VERSION(3, 0, 20) || \
	LINUX_VERSION_CODE == KERNEL_VERSION(3, 0, 33) ||\
	LINUX_VERSION_CODE >= KERNEL_VERSION(3, 8, 5)
	.fallocate	= vdfs2_fallocate,
#endif
};

/**
 * The eMMCFS files inode operations.
 */
static const struct inode_operations vdfs2_file_inode_operations = {
		/* FIXME & TODO is this correct : use same function as in
		vdfs2_dir_inode_operations? */
		/*.truncate	= vdfs2_file_truncate, depricated*/
		.setattr	= vdfs2_setattr,
#if LINUX_VERSION_CODE == KERNEL_VERSION(2, 6, 35)
		.fallocate	= vdfs2_fallocate,
#endif
		.listxattr	= vdfs2_listxattr,
};


static int vdfs2_fill_inode_fork(struct inode *inode,
		struct vdfs2_catalog_file_record *file_val)
{
	struct vdfs2_inode_info *inf = VDFS2_I(inode);
	int ret = 0;

	/* TODO special files like fifo or soket does not have fork */
	if (is_vdfs2_inode_flag_set(inode, TINY_FILE)) {
		memcpy(inf->tiny.data, file_val->tiny.data, TINY_DATA_SIZE);
		inf->tiny.len = le16_to_cpu(file_val->tiny.len);
		inf->tiny.i_size = le64_to_cpu(file_val->tiny.i_size);
		inode->i_size = inf->tiny.i_size;
		inode->i_blocks = 0;
	} else if (is_vdfs2_inode_flag_set(inode, SMALL_FILE)) {
		inf->small.cell = le64_to_cpu(file_val->small.cell);
		inf->small.len = le16_to_cpu(file_val->small.len);
		inf->small.i_size = le64_to_cpu(file_val->small.i_size);
		inode->i_size = inf->small.i_size;
		inode->i_blocks = 0;
	} else
		ret = vdfs2_parse_fork(inode, &file_val->data_fork);

	return ret;
}


static int vdfs2_fill_inode(struct inode *inode,
		struct vdfs2_catalog_folder_record *folder_val)
{
	int ret = 0;
	/*struct vdfs2_sb_info *sbi = */

	VDFS2_I(inode)->flags = le32_to_cpu(folder_val->flags);
	vdfs2_set_vfs_inode_flags(inode);

	atomic_set(&(VDFS2_I(inode)->open_count), 0);

	inode->i_mode = le16_to_cpu(folder_val->permissions.file_mode);
	i_uid_write(inode, le32_to_cpu(folder_val->permissions.uid));
	i_gid_write(inode, le32_to_cpu(folder_val->permissions.gid));
#if LINUX_VERSION_CODE >= KERNEL_VERSION(3, 8, 5)
	set_nlink(inode, le64_to_cpu(folder_val->links_count));
#else
	inode->i_nlink = le64_to_cpu(folder_val->links_count);
#endif
	inode->i_generation = inode->i_ino;

	inode->i_mtime.tv_sec =
			le64_to_cpu(folder_val->modification_time.seconds);
	inode->i_atime.tv_sec =
			le64_to_cpu(folder_val->access_time.seconds);
	inode->i_ctime.tv_sec =
			le64_to_cpu(folder_val->creation_time.seconds);

	inode->i_mtime.tv_nsec =
			le64_to_cpu(folder_val->modification_time.nanoseconds);
	inode->i_atime.tv_nsec =
			le64_to_cpu(folder_val->access_time.nanoseconds);
	inode->i_ctime.tv_nsec =
			le64_to_cpu(folder_val->creation_time.nanoseconds);

	if (S_ISLNK(inode->i_mode)) {
		inode->i_op = &vdfs2_symlink_inode_operations;
		inode->i_mapping->a_ops = &vdfs2_aops;
		inode->i_fop = &vdfs2_file_operations;
		inode_nohighmem(inode);

	} else if S_ISREG(inode->i_mode) {
		inode->i_op = &vdfs2_file_inode_operations;
		inode->i_mapping->a_ops = &vdfs2_aops;
		inode->i_fop = &vdfs2_file_operations;

	} else if S_ISDIR(inode->i_mode) {
		inode->i_size = (loff_t)le64_to_cpu(
				folder_val->total_items_count);
		inode->i_op = &vdfs2_dir_inode_operations;
		inode->i_fop = &vdfs2_dir_operations;
	} else if (S_ISCHR(inode->i_mode) || S_ISBLK(inode->i_mode) ||
			S_ISFIFO(inode->i_mode) || S_ISSOCK(inode->i_mode)) {
		inode->i_opflags &= ~IOP_XATTR;
		inode->i_mapping->a_ops = &vdfs2_aops;
		init_special_inode(inode, inode->i_mode,
			(dev_t)le64_to_cpu(folder_val->total_items_count));
	} else {
		/* UNKNOWN object type*/
		ret = -EINVAL;
	}

	return ret;
}

struct inode *vdfs2_get_inode_from_record(struct vdfs2_cattree_record *record)
{
	struct vdfs2_btree *tree =
		VDFS2_BTREE_REC_I((void *) record)->rec_pos.bnode->host;
	struct vdfs2_sb_info *sbi = tree->sbi;
	struct vdfs2_catalog_folder_record *folder_rec = NULL;
	struct vdfs2_catalog_file_record *file_rec = NULL;
	struct vdfs2_hlink_record *hard_link_record = NULL;
	__u64 ino = 0;
	struct inode *inode;
	int ret = 0;

	if (IS_ERR(record) || !record)
		return ERR_PTR(-EFAULT);

	if (record->key->record_type >= VDFS2_CATALOG_PTREE_RECORD) {
		ino = le64_to_cpu((
			(struct vdfs2_pack_common_value *)record->val)->
				object_id);
		if (tree->btree_type == VDFS2_BTREE_PACK)
			ino += tree->start_ino;
	} else if (record->key->record_type == VDFS2_CATALOG_HLINK_RECORD) {
		struct vdfs2_catalog_hlink_record *hlink_record = record->val;
		ino = le64_to_cpu(hlink_record->object_id);
	} else {
		struct vdfs2_catalog_folder_record *f_record = record->val;
		ino = le64_to_cpu(f_record->object_id);
	}

	inode = iget_locked(sbi->sb, ino);

	if (!inode) {
		inode = ERR_PTR(-ENOMEM);
		goto exit;
	}

	if (!(inode->i_state & I_NEW))
		goto exit;

	VDFS2_I(inode)->record_type = record->key->record_type;

	/* create inode from pack tree */
	if (record->key->record_type >= VDFS2_CATALOG_PTREE_RECORD) {
		ret = vdfs2_read_packtree_inode(inode, record->key);
		if (ret)
			goto error_exit;
		else {
#ifdef CONFIG_VDFS2_QUOTA
			VDFS2_I(inode)->quota_index = -1;
#endif
			unlock_new_inode(inode);
			goto exit;
		}
	}

	/* create inode from catalog tree*/
	if (record->key->record_type == VDFS2_CATALOG_HLINK_RECORD) {
		mutex_r_lock_nested(sbi->hardlink_tree->rw_tree_lock, HL_TREE);
		hard_link_record = vdfs2_hard_link_find(sbi, ino,
				VDFS2_BNODE_MODE_RO);
		if (IS_ERR(hard_link_record)) {
			mutex_r_unlock(sbi->hardlink_tree->rw_tree_lock);
			ret = PTR_ERR(hard_link_record);
			goto error_exit;
		}
		file_rec = hard_link_record->hardlink_value;
		folder_rec = &file_rec->common;
	} else if (record->key->record_type == VDFS2_CATALOG_FILE_RECORD) {
		file_rec = record->val;
		folder_rec = &file_rec->common;
	} else if (record->key->record_type == VDFS2_CATALOG_FOLDER_RECORD) {
		folder_rec =
			(struct vdfs2_catalog_folder_record *)record->val;
	} else {
		if (!is_sbi_flag_set(sbi, IS_MOUNT_FINISHED)) {
			ret = -EFAULT;
			goto error_exit;
		} else
			VDFS2_BUG();
	}

	ret = vdfs2_fill_inode(inode, folder_rec);
	if (ret)
		goto error_put_hrd;

	if (file_rec && (S_ISLNK(inode->i_mode) || S_ISREG((inode->i_mode))))
		ret = vdfs2_fill_inode_fork(inode, file_rec);

	if (ret)
		goto error_put_hrd;

	if (record->key->record_type == VDFS2_CATALOG_HLINK_RECORD) {
		set_vdfs2_inode_flag(inode, HARD_LINK);
		vdfs2_release_record((struct vdfs2_btree_gen_record *)
				hard_link_record);
		mutex_r_unlock(sbi->hardlink_tree->rw_tree_lock);
	} else {
		char *new_name;
		unsigned int str_len;
		struct vdfs2_cattree_key *key = record->key;
		new_name = kzalloc(key->name.length + 1, GFP_KERNEL);
		if (!new_name) {
			ret = -ENOMEM;
			goto error_put_hrd;
		}

		str_len = min(key->name.length, (unsigned) VDFS2_CAT_MAX_NAME);
		memcpy(new_name, key->name.unicode_str,
			min(key->name.length, (unsigned) VDFS2_CAT_MAX_NAME));
		new_name[str_len] = 0;
		VDFS2_BUG_ON(VDFS2_I(inode)->name);
		VDFS2_I(inode)->name = new_name;
		VDFS2_I(inode)->parent_id = le64_to_cpu(key->parent_id);
	}

#ifdef CONFIG_VDFS2_QUOTA
	VDFS2_I(inode)->quota_index = -1;
#endif
	unlock_new_inode(inode);

exit:
	return inode;
error_put_hrd:
	if (record->key->record_type == VDFS2_CATALOG_HLINK_RECORD) {
		if (hard_link_record)
			vdfs2_release_record((struct vdfs2_btree_gen_record *)
				hard_link_record);
		mutex_r_unlock(sbi->hardlink_tree->rw_tree_lock);
	}
error_exit:
	iget_failed(inode);
	return ERR_PTR(ret);
}

/**
 * @brief		The eMMCFS inode constructor.
 * @param [in]	dir		Directory, where inode will be created
 * @param [in]	mode	Mode for created inode
 * @return		Returns pointer to inode on success, errno on failure
 */

static struct inode *vdfs2_new_inode(struct inode *dir, umode_t mode)
{
	struct super_block *sb = dir->i_sb;
	struct  vdfs2_sb_info *sbi = VDFS2_SB(sb);
	ino_t ino = 0;
	struct inode *inode;
	int err, i;
	struct vdfs2_fork_info *ifork;

	err = vdfs2_get_free_inode(sb->s_fs_info, &ino, 1);

	if (err)
		return ERR_PTR(err);

	/*VDFS2_DEBUG_INO("#%lu", ino);*/
	inode = new_inode(sb);
	if (!inode) {
		err = -ENOMEM;
		goto err_exit_noiput;
	}

	inode->i_ino = ino;

	if (test_option(sbi, DMASK) && S_ISDIR(mode))
		mode &= ~sbi->dmask;

	if (test_option(sbi, FMASK) && S_ISREG(mode))
		mode &= ~sbi->fmask;

	inode_init_owner(&init_user_ns, inode, dir, mode);

#if LINUX_VERSION_CODE >= KERNEL_VERSION(3, 8, 5)
	set_nlink(inode, 1);
#else
	inode->i_nlink = 1;
#endif
	inode->i_size = 0;
	inode->i_generation = le32_to_cpu(VDFS2_RAW_EXSB(sbi)->generation);
	inode->i_blocks = 0;
	inode->i_mtime = inode->i_atime = inode->i_ctime =
			current_time(inode);
	atomic_set(&(VDFS2_I(inode)->open_count), 0);

	/* todo actual inheritance mask and mode-dependent masking */
	VDFS2_I(inode)->flags = VDFS2_I(dir)->flags & VDFS2_FL_INHERITED;
	vdfs2_set_vfs_inode_flags(inode);

	if (S_ISDIR(mode))
		inode->i_op =  &vdfs2_dir_inode_operations;
	else if (S_ISLNK(mode)) {
		inode->i_op = &vdfs2_symlink_inode_operations;
		inode_nohighmem(inode);
	} else
		inode->i_op = &vdfs2_file_inode_operations;

	inode->i_mapping->a_ops = &vdfs2_aops;
	inode->i_fop = (S_ISDIR(mode)) ?
			&vdfs2_dir_operations : &vdfs2_file_operations;

	/* Init extents with zeros - file is empty */
	ifork = &(VDFS2_I(inode)->fork);
	ifork->used_extents = 0;
	for (i = VDFS2_EXTENTS_COUNT_IN_FORK - 1; i >= 0; i--) {
		ifork->extents[i].first_block = 0;
		ifork->extents[i].block_count = 0;
		ifork->extents[i].iblock = 0;
	}
	ifork->total_block_count = 0;
	ifork->prealloc_start_block = 0;
	ifork->prealloc_block_count = 0;

	VDFS2_I(inode)->parent_id = 0;

	if (insert_inode_locked(inode) < 0) {
		err = -EINVAL;
		goto err_exit;
	}

	return inode;

err_exit:
	iput(inode);
err_exit_noiput:
	if (vdfs2_free_inode_n(sb->s_fs_info, ino, 1))
		VDFS2_ERR("can not free inode while handling error");

	return ERR_PTR(err);
}


/**
 * @brief			Standard callback to create file.
 * @param [in,out]	dir		Directory where node will be created
 * @param [in]		dentry	Created dentry
 * @param [in]		mode	Mode for file
 * @param [in]		nd	Namedata for file
 * @return			Returns 0 on success, errno on failure
 */
#if LINUX_VERSION_CODE >= KERNEL_VERSION(3, 8, 5)
static int vdfs2_create(struct user_namespace *mnt_userns, struct inode *dir,
		struct dentry *dentry, umode_t mode, bool excl)
#else
static int vdfs2_create(struct inode *dir, struct dentry *dentry, int mode,
		struct nameidata *nd)
#endif
{
	struct super_block *sb = dir->i_sb;
	struct vdfs2_sb_info *sbi = sb->s_fs_info;
	struct inode *inode;
	char *saved_name;
	int ret = 0;
	/* TODO - ext2 does it - detrmine if it's necessary here */
	/* dquot_initialize(dir); */
	if (check_permissions(sbi))
		return -EINTR;
	VDFS2_DEBUG_INO("'%s' dir = %ld", dentry->d_name.name, dir->i_ino);

	if (strlen(dentry->d_name.name) > VDFS2_UNICODE_STRING_MAX_LEN)
		return -ENAMETOOLONG;

	saved_name = kzalloc(dentry->d_name.len + 1, GFP_KERNEL);
	if (!saved_name) {
		ret = -ENOMEM;
		goto exit;
	}

	vdfs2_start_transaction(sbi);
	inode = vdfs2_new_inode(dir, mode);

	if (IS_ERR(inode)) {
		kfree(saved_name);
		ret = PTR_ERR(inode);
		goto exit;
	}


	strncpy(saved_name, dentry->d_name.name, dentry->d_name.len + 1);

	VDFS2_I(inode)->name = saved_name;
	VDFS2_I(inode)->parent_id = dir->i_ino;
#ifdef CONFIG_VDFS2_QUOTA
	VDFS2_I(inode)->quota_index = -1;
#endif

	VDFS2_DEBUG_MUTEX("cattree mutex w lock");
	mutex_w_lock(sbi->catalog_tree->rw_tree_lock);
	VDFS2_DEBUG_MUTEX("cattree mutex w lock succ");
	/* if it's a file */
	if ((S_ISREG(inode->i_mode) || S_ISLNK(inode->i_mode)) &&
				(inode->i_ino >= VDFS2_1ST_FILE_INO) &&
				/* and tiny files are enabled */
				(test_option(sbi, TINY) ||
				test_option(sbi, TINYSMALL))) {
		/* set tiny flag */
		set_vdfs2_inode_flag(inode, TINY_FILE);
		atomic64_inc(&sbi->tiny_files_counter);
	}
	unlock_new_inode(inode);
	ret = vdfs2_insert_cattree_object(sbi->catalog_tree, inode, dir->i_ino);
	if (ret)
		goto error_exit;

	ret = security_inode_init_security(inode, dir,
			&dentry->d_name, vdfs2_init_security_xattrs, NULL);
	if (ret && ret != -EOPNOTSUPP) {
		BUG_ON(vdfs2_cattree_remove(sbi, inode->i_ino,
			VDFS2_I(inode)->name, strlen(VDFS2_I(inode)->name)));
		goto error_exit;
	}

	d_instantiate(dentry, inode);
	vdfs2_put_inode_into_dirty_list(inode);
	VDFS2_DEBUG_MUTEX("cattree mutex w lock un");
	mutex_w_unlock(sbi->catalog_tree->rw_tree_lock);

	mutex_lock(&VDFS2_I(dir)->truncate_mutex);
	dir->i_size++;
	sbi->files_count++;
	dir->i_ctime = current_time(dir);
	dir->i_mtime = current_time(dir);
	ret = vdfs2_write_inode_to_bnode(dir);
	mutex_unlock(&VDFS2_I(dir)->truncate_mutex);
#ifdef CONFIG_VDFS2_QUOTA
	if (VDFS2_I(dir)->quota_index != -1)
		VDFS2_I(inode)->quota_index =
				VDFS2_I(dir)->quota_index;
#endif
exit:
	vdfs2_stop_transaction(sbi);
	return ret;
error_exit:
	iput(inode);
	vdfs2_free_inode_n(sbi, inode->i_ino, 1);
	mutex_w_unlock(sbi->catalog_tree->rw_tree_lock);
	kfree(saved_name);
	vdfs2_stop_transaction(sbi);
	return ret;
}

/**
 * @brief			Write inode to bnode.
 * @param [in,out]	inode	The inode, that will be written to bnode
 * @return			Returns 0 on success, errno on failure
 */
int vdfs2_write_inode_to_bnode(struct inode *inode)
{
	struct super_block *sb = inode->i_sb;
	struct vdfs2_sb_info *sbi = sb->s_fs_info;
	struct vdfs2_inode_info *inode_info = VDFS2_I(inode);
	int ret = 0;

	if (inode->i_ino < VDFS2_1ST_FILE_INO && inode->i_ino != VDFS2_ROOT_INO)
		return 0;

	vdfs2_put_inode_into_dirty_list(inode);

	if (VDFS2_I(inode)->record_type >= VDFS2_CATALOG_PTREE_ROOT)
		return 0;

	BUG_ON(!mutex_is_locked(&inode_info->truncate_mutex));

	if (is_vdfs2_inode_flag_set(inode, HARD_LINK)) {
		struct vdfs2_hlink_record *hard_link_info;

		mutex_w_lock(sbi->hardlink_tree->rw_tree_lock);
		hard_link_info = vdfs2_hard_link_find(sbi, inode->i_ino,
				VDFS2_BNODE_MODE_RW);

		if (IS_ERR(hard_link_info)) {
			ret = PTR_ERR(hard_link_info);
			mutex_w_unlock(sbi->catalog_tree->rw_tree_lock);
			goto exit;
		}

		ret = vdfs2_fill_cattree_value(inode,
				(void *)hard_link_info->hardlink_value);

		vdfs2_release_dirty_record((struct vdfs2_btree_gen_record *)
				hard_link_info);
		mutex_w_unlock(sbi->hardlink_tree->rw_tree_lock);
	} else {
		struct vdfs2_cattree_record *record;

		mutex_w_lock(sbi->catalog_tree->rw_tree_lock);
		record = vdfs2_cattree_find(sbi->catalog_tree,
			inode_info->parent_id, inode_info->name,
			strlen(inode_info->name), VDFS2_BNODE_MODE_RW);

		if (IS_ERR(record)) {
			ret = PTR_ERR(record);
			mutex_w_unlock(sbi->catalog_tree->rw_tree_lock);
			goto exit;
		}

		ret = vdfs2_fill_cattree_value(inode, record->val);

		vdfs2_release_dirty_record((struct vdfs2_btree_gen_record *)
				record);
		mutex_w_unlock(sbi->catalog_tree->rw_tree_lock);
	}

exit:
	return ret;
}

/**
 * @brief		Method to read inode to inode cache.
 * @param [in]	sb	Pointer to superblock
 * @param [in]	ino	The inode number
 * @return		Returns pointer to inode on success,
 *			ERR_PTR(errno) on failure
 */
struct inode *vdfs2_special_iget(struct super_block *sb, unsigned long ino)
{
	struct inode *inode;
	struct vdfs2_sb_info *sbi = sb->s_fs_info;
	struct vdfs2_extended_super_block *exsb = VDFS2_RAW_EXSB(sbi);
	int ret = 0;
	gfp_t gfp_mask;
	loff_t size;

	VDFS2_BUG_ON(ino >= VDFS2_1ST_FILE_INO);
	VDFS2_DEBUG_INO("inode #%lu", ino);
	inode = iget_locked(sb, ino);
	if (!inode) {
		ret = -ENOMEM;
		goto err_exit_no_fail;
	}

	if (!(inode->i_state & I_NEW))
		goto exit;

	inode->i_mode = 0;

	/* Metadata pages can not be migrated */
#define GFP_MOVABLE_MASK (__GFP_RECLAIMABLE|__GFP_MOVABLE)
	gfp_mask = (mapping_gfp_mask(inode->i_mapping) & ~GFP_MOVABLE_MASK);
	mapping_set_gfp_mask(inode->i_mapping, gfp_mask);

	if (ino == VDFS2_SMALL_AREA) {
		struct vdfs2_inode_info *inode_info = VDFS2_I(inode);
		ret = vdfs2_parse_fork(inode, &exsb->small_area);
		size = ((loff_t)inode_info->fork.total_block_count) <<
				sbi->log_block_size;
		inode->i_mapping->a_ops = &vdfs2_aops;

	} else {
		size = vdfs2_special_file_size(sbi, ino);
		inode->i_mapping->a_ops = &vdfs2_aops_special;
	}
	i_size_write(inode, size);

	if (ret) {
		iput(inode);
		goto err_exit_no_fail;
	}
#ifdef CONFIG_VDFS2_QUOTA
	VDFS2_I(inode)->quota_index = -1;
#endif
	unlock_new_inode(inode);
exit:
	return inode;
err_exit_no_fail:
	VDFS2_DEBUG_INO("inode #%lu read FAILED", ino);
	return ERR_PTR(ret);
}

/**
 * @brief		Propagate flags from vfs inode i_flags
 *			to VDFS2_I(inode)->flags.
 * @param [in]	inode	Pointer to vfs inode structure.
  * @return		none.
 */
void vdfs2_get_vfs_inode_flags(struct inode *inode)
{
	VDFS2_I(inode)->flags &= ~(1 << VDFS2_IMMUTABLE);
	if (inode->i_flags & S_IMMUTABLE)
		VDFS2_I(inode)->flags |= (1 << VDFS2_IMMUTABLE);
}

/**
 * @brief		Set vfs inode i_flags according to
 *			VDFS2_I(inode)->flags.
 * @param [in]	inode	Pointer to vfs inode structure.
  * @return		none.
 */
void vdfs2_set_vfs_inode_flags(struct inode *inode)
{
	inode->i_flags &= ~S_IMMUTABLE;
	if (VDFS2_I(inode)->flags & (1 << VDFS2_IMMUTABLE))
		inode->i_flags |= S_IMMUTABLE;
}


