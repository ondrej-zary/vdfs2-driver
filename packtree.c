/**
 * @file	packtree.c
 * @brief	Installed squashfs image catalog tree implementation.
 * @author	Igor Skalkin, i.skalkin@samsung.com
 * @date	03/05/2013
 *
* eMMCFS -- Samsung eMMC chip oriented File System, Version 1.
 *
 * This file implements catalog tree functions for metadata of
 * installed squashfs image.
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

#include <linux/buffer_head.h>
#include <linux/file.h>
#include <linux/vmalloc.h>
#include "vdfs2.h"
#include "cattree.h"
#include "packtree.h"
#include "debug.h"

/**
 * @brief		Method to get packtree source image inode.
 * @param [in]	sb	Pointer to superblock
 * @param [in]	parent_id	packtree image parent id.
 * @param [in]	name		packtree image name.
 * @param [in]	name_len	packtree image name_len.
 * @return		Returns pointer to inode on success,
 *			ERR_PTR(errno) on failure
 */
struct inode *get_packtree_image_inode(struct vdfs2_sb_info *sbi,
		__u64 parent_id, __u8 *name, int name_len)
{
	struct inode *inode;
	struct vdfs2_cattree_record *record;

	mutex_r_lock(sbi->catalog_tree->rw_tree_lock);
	record = vdfs2_cattree_find(sbi->catalog_tree, parent_id, name, name_len,
			VDFS2_BNODE_MODE_RO);

	if (IS_ERR(record)) {
		/* Pass error code to return value */
		inode = (void *)record;
		goto err_exit;
	}

	inode = vdfs2_get_inode_from_record(record);
	vdfs2_release_record((struct vdfs2_btree_gen_record *) record);
err_exit:
	mutex_r_unlock(sbi->catalog_tree->rw_tree_lock);
	return inode;
}

static int unlock_source_image(struct vdfs2_sb_info *sbi,
		struct vdfs2_pack_insert_point_info *info)
{
	int rc = 0;
	struct inode *image_inode;
	struct vdfs2_cattree_record *record;
	record = vdfs2_cattree_find(sbi->catalog_tree,
			info->source_image_parent_object_id,
			info->source_image_name.unicode_str,
			info->source_image_name.length,
			VDFS2_BNODE_MODE_RW);
	if (IS_ERR(record))
		return PTR_ERR(record);

	image_inode = vdfs2_get_inode_from_record(record);
	if (IS_ERR(image_inode)) {
		vdfs2_release_record((struct vdfs2_btree_gen_record *) record);
		return PTR_ERR(image_inode);
	}

	mutex_lock(&VDFS2_I(image_inode)->truncate_mutex);
	VDFS2_I(image_inode)->flags &= ~(1 << VDFS2_IMMUTABLE);
	vdfs2_set_vfs_inode_flags(image_inode);
	image_inode->i_ctime = vdfs2_current_time(image_inode);
	rc = vdfs2_write_inode_to_bnode(image_inode);
	mutex_unlock(&VDFS2_I(image_inode)->truncate_mutex);
	iput(image_inode);
	vdfs2_release_record((struct vdfs2_btree_gen_record *) record);
	return rc;
}

static void release_packtree_inodes(struct vdfs2_sb_info *sbi,
		struct vdfs2_pack_insert_point_info *info)
{
	int i;
	for (i = 0; i < info->pmc.inodes_cnt; i++) {
		__u64 ino_no = info->start_ino + i + 1;
		struct inode *inode = ilookup(sbi->sb, ino_no);
		if (inode) {
			/*printk(KERN_ERR "release_ptree_inode %lld", ino_no);*/
			remove_inode_hash(inode);
			inode->i_size = 0;
			truncate_inode_pages(&inode->i_data, 0);
			invalidate_inode_buffers(inode);
			iput(inode);
		}
	}
}

static int update_parent_dir(struct vdfs2_sb_info *sbi, struct inode *inode,
		struct ioctl_uninstall_params *params)
{
	int rc = 0;
	struct inode *parent_dir_inode;
	struct vdfs2_cattree_record *record;

	if (VDFS2_I(inode)->parent_id == VDFS2_ROOT_INO)
		record = vdfs2_cattree_find(sbi->catalog_tree,
			VDFS2_ROOTDIR_OBJ_ID, VDFS2_ROOTDIR_NAME,
			strlen(VDFS2_ROOTDIR_NAME), VDFS2_BNODE_MODE_RW);
	else
		record = vdfs2_cattree_find(sbi->catalog_tree,
			params->parent_id, params->name, strlen(params->name),
			VDFS2_BNODE_MODE_RW);
	if (IS_ERR(record))
		return PTR_ERR(record);

	parent_dir_inode = vdfs2_get_inode_from_record(record);
	if (IS_ERR(parent_dir_inode)) {
		vdfs2_release_record((struct vdfs2_btree_gen_record *) record);
		return PTR_ERR(parent_dir_inode);
	}

	if (parent_dir_inode->i_size != 0)
		parent_dir_inode->i_size--;
	else
		VDFS2_DEBUG_INO("Files count mismatch");

	parent_dir_inode->i_mtime = vdfs2_current_time(parent_dir_inode);

	rc = vdfs2_fill_cattree_value(parent_dir_inode, record->val);
	vdfs2_release_dirty_record((struct vdfs2_btree_gen_record *) record);

	iput(parent_dir_inode);
	return rc;
}

/**
 * @brief		Uninstall packtree image function.
 * @param [in]	inode	Pointer to inode
 * @param [in]	params	Pointer to structure with information about
 *			parent directory of installed packtree image.
 * @return		Returns error codes
 */
int vdfs2_uninstall_packtree(struct inode *inode,
		struct ioctl_uninstall_params *params)
{
	struct vdfs2_cattree_key *key;
	struct vdfs2_sb_info *sbi = VDFS2_SB(inode->i_sb);
	int rc = 0;
	struct dentry *de;

	if (check_permissions(VDFS2_SB(inode->i_sb)))
		return -EINTR;

	key = vdfs2_alloc_cattree_key(strlen(VDFS2_I(inode)->name),
			VDFS2_CATALOG_RECORD_DUMMY);
	if (IS_ERR(key))
		return PTR_ERR(key);

	vdfs2_fill_cattree_key(key, VDFS2_I(inode)->parent_id,
		VDFS2_I(inode)->name, strlen(VDFS2_I(inode)->name));

	VDFS2_DEBUG_MUTEX("cattree mutex w lock");
	mutex_w_lock(sbi->catalog_tree->rw_tree_lock);
	VDFS2_DEBUG_MUTEX("cattree mutex w lock succ");
	vdfs2_remove_indirect_index(sbi->catalog_tree, inode);
	rc = vdfs2_btree_remove(sbi->catalog_tree, &key->gen_key);

	if (rc) {
		VDFS2_DEBUG_MUTEX("cattree mutex w lock un");
		mutex_w_unlock(sbi->catalog_tree->rw_tree_lock);
		goto exit;
	}

	rc = update_parent_dir(sbi, inode, params);
	if (rc) {
		VDFS2_DEBUG_MUTEX("cattree mutex w lock un");
		mutex_w_unlock(sbi->catalog_tree->rw_tree_lock);
		goto exit;
	}

	drop_nlink(inode);
	VDFS2_BUG_ON(inode->i_nlink > VDFS2_LINK_MAX);

	VDFS2_DEBUG_MUTEX("cattree mutex w lock un");
	mutex_w_unlock(sbi->catalog_tree->rw_tree_lock);

	remove_inode_hash(inode);

	rc = vdfs2_free_inode_n(VDFS2_SB(inode->i_sb),
			VDFS2_I(inode)->ptree.root.start_ino,
			VDFS2_I(inode)->ptree.root.pmc.inodes_cnt + 1);
		if (rc)
			goto exit;

	rc = unlock_source_image(sbi, &VDFS2_I(inode)->ptree.root);
	if (rc)
		goto exit;

	sbi->files_count -= VDFS2_I(inode)->ptree.root.pmc.inodes_cnt;

	if (VDFS2_I(inode)->ptree.tree_info) {
		release_packtree_inodes(sbi, &VDFS2_I(inode)->ptree.root);
		list_del(&VDFS2_I(inode)->ptree.tree_info->list);
		vdfs2_destroy_packtree(VDFS2_I(inode)->ptree.tree_info);
	}

	de = d_find_alias(inode);
	if (de) {
		dput(de);
		d_drop(de); /* todo */
	}

exit:
	kfree(key);
	return rc;
}

/* todo */
int vdfs2_create_chunk_offsets_table(struct installed_packtree_info *packtree_info)
{
	int rc, i, len = (packtree_info->params.pmc.chunk_cnt + 1) << 3;

	packtree_info->chunk_table = vzalloc(len);
	if (!packtree_info->chunk_table)
		return -ENOMEM;

	rc = vdfs2_read_squashfs_image_simple(packtree_info->tree->inode,
		packtree_info->params.pmc.chtab_off, len,
		packtree_info->chunk_table);
	if (rc) {
		vfree(packtree_info->chunk_table);
		return rc;
	}
	for (i = 0; i <= packtree_info->params.pmc.chunk_cnt; i++)
		packtree_info->chunk_table[i] =
				le64_to_cpu(packtree_info->chunk_table[i]);
	return rc;
}

/**
 * The vdfs2 unpack inode address space operations.
 */
static const struct address_space_operations unpack_inode_aops = {
};

static struct installed_packtree_info *init_packtree(struct vdfs2_sb_info *sbi,
		struct inode *packtree_root)
{
	struct inode *packtree_image_inode = NULL;
	struct installed_packtree_info *packtree_info = NULL;
	struct vdfs2_inode_info *root_inode_info = VDFS2_I(packtree_root);
	int ret = 0;

	BUG_ON(root_inode_info->record_type != VDFS2_CATALOG_PTREE_ROOT);
	BUG_ON(root_inode_info->ptree.tree_info != NULL);

	if (memcmp(root_inode_info->ptree.root.packtree_layout_version,
			VDFS2_PACK_METADATA_VERSION,
			strlen(VDFS2_PACK_METADATA_VERSION))) {
		printk(KERN_ERR "Packtree layout mismatch:\n"
			"Image file had been expanded and installed by"
			" previous version of install.vdfs2 utility.");
		return ERR_PTR(-EINVAL);
	}

	packtree_info = kzalloc(sizeof(*packtree_info), GFP_KERNEL);
	if (!packtree_info)
		return ERR_PTR(-ENOMEM);

	packtree_info->tree = kzalloc(sizeof(struct vdfs2_btree), GFP_KERNEL);
	if (!packtree_info->tree) {
		kfree(packtree_info);
		return ERR_PTR(-ENOMEM);
	}
#ifdef CONFIG_VDFS2_DEBUG
	mutex_init(&packtree_info->dump_lock);
	packtree_info->print_once = 0;
#endif


	packtree_info->tree->btree_type = VDFS2_BTREE_PACK;
	packtree_info->tree->max_record_len =
			vdfs2_get_packtree_max_record_size();

	packtree_info->tree->tree_metadata_offset =
			root_inode_info->ptree.root.pmc.packoffset;
	packtree_info->tree->start_ino =
			root_inode_info->ptree.root.start_ino;

	memcpy(&packtree_info->params, &root_inode_info->ptree.root,
			sizeof(struct vdfs2_pack_insert_point_info));

	packtree_image_inode = get_packtree_image_inode(sbi,
			packtree_info->params.source_image_parent_object_id,
			packtree_info->params.source_image_name.unicode_str,
			packtree_info->params.source_image_name.length);
	if (IS_ERR(packtree_image_inode)) {
		ret = PTR_ERR(packtree_image_inode);
		VDFS2_ERR(" get_packtree_image_inode fail");
		goto fail;
	}

	ret = vdfs2_fill_btree(sbi, packtree_info->tree, packtree_image_inode);
	if (ret) {
		VDFS2_ERR("vdfs2_fill_btree %ld fail",
				packtree_image_inode->i_ino);
		iput(packtree_image_inode);
		goto fail;
	}
	packtree_info->tree->comp_fn = test_option(sbi, CASE_INSENSITIVE) ?
		vdfs2_cattree_cmpfn_ci : vdfs2_cattree_cmpfn;

	ret = vdfs2_create_chunk_offsets_table(packtree_info);
	if (ret) {
		vdfs2_put_btree(packtree_info->tree);
		kfree(packtree_info);
		return ERR_PTR(ret);
	}

	INIT_LIST_HEAD(&packtree_info->list);
	list_add(&packtree_info->list, &sbi->packtree_images.list);

	if (!packtree_info->unpacked_inode)
		packtree_info->unpacked_inode =
			iget_locked(sbi->sb,
					packtree_info->params.start_ino + 1);
	if (!packtree_info->unpacked_inode) {
		ret = -ENOMEM;
		vdfs2_put_btree(packtree_info->tree);
		kfree(packtree_info);
		return ERR_PTR(ret);
	}
	if (!(packtree_info->unpacked_inode->i_state & I_NEW))
		return packtree_info;

	packtree_info->unpacked_inode->i_mode = 0;
	VDFS2_I(packtree_info->unpacked_inode)->record_type =
			VDFS2_CATALOG_UNPACK_INODE;
	unlock_new_inode(packtree_info->unpacked_inode);
	packtree_info->unpacked_inode->i_mapping->a_ops = &unpack_inode_aops;
	i_size_write(packtree_info->unpacked_inode, 0);

	return packtree_info;
fail:
	kfree(packtree_info->tree);
	kfree(packtree_info);
	return ERR_PTR(ret);
}

static struct installed_packtree_info *lookup_in_opened_list(
		struct vdfs2_sb_info *sbi, __u64 i_ino)
{
	struct list_head *pos;

	list_for_each(pos, &sbi->packtree_images.list) {
			struct installed_packtree_info *packtree_info =
			(struct installed_packtree_info *)list_entry(pos,
					struct installed_packtree_info, list);

			if (i_ino == packtree_info->params.start_ino)
				return packtree_info;
		}
	return NULL;
}

struct installed_packtree_info *vdfs2_get_packtree(struct inode *root_inode)
{
	struct vdfs2_sb_info *sbi = VDFS2_SB(root_inode->i_sb);
	struct installed_packtree_info *pack_tree = NULL;

	BUG_ON(VDFS2_I(root_inode)->record_type != VDFS2_CATALOG_PTREE_ROOT);

	mutex_lock(&sbi->packtree_images.lock_pactree_list);
	pack_tree = lookup_in_opened_list(sbi, root_inode->i_ino);
	if (!pack_tree)
		pack_tree = init_packtree(sbi, root_inode);
	mutex_unlock(&sbi->packtree_images.lock_pactree_list);

	return pack_tree;
}


void vdfs2_destroy_packtree(struct installed_packtree_info *ptr)
{
	vdfs2_put_btree(ptr->tree);
	vfree(ptr->chunk_table);
	iput(ptr->unpacked_inode);
	kfree(ptr);
}

/* TODO comment*/
int *vdfs2_destroy_packtrees_list(struct vdfs2_sb_info *sbi)
{
	struct list_head *pos, *q;

	mutex_lock(&sbi->packtree_images.lock_pactree_list);
	list_for_each_safe(pos, q, &sbi->packtree_images.list) {
		struct installed_packtree_info *ptr =
			list_entry(pos, struct installed_packtree_info, list);
		list_del(pos);
		vdfs2_destroy_packtree(ptr);
	}
	mutex_unlock(&sbi->packtree_images.lock_pactree_list);
	return NULL;
}

#include <linux/namei.h>

static int update_dentry_with_inode(struct dentry *parent,
		struct inode *install_inode)
{
	int ret = 0;
	struct vdfs2_inode_info *inode_info = VDFS2_I(install_inode);
	struct dentry *dentry = NULL;
	char *name = inode_info->name;
	char len = strlen(inode_info->name);

	/* TODO: possible wrong locking-class I_MUTEX_CHILD, change in case
	 * of locking problems */
	mutex_lock_nested(&parent->d_inode->i_mutex, I_MUTEX_CHILD);
	dentry = lookup_one_len(name, parent, len);

	if (dentry->d_inode) {
		mutex_unlock(&parent->d_inode->i_mutex);
		return 0;
	}

	d_instantiate(dentry, install_inode);
	dput(dentry);
	mutex_unlock(&parent->d_inode->i_mutex);
	return ret;
}

/**
 * @brief			Create and and to catalog Btree packed tree
 *				insertion point.
 * @param [in]	parent_dir	Intallation point parent file
 * @param [in]	image_file	Packed image file
 * @param [in]	pm		Installation parameters
 * @return			Returns 0 on success, errno on failure
 */
int vdfs2_install_packtree(struct file *parent_dir, struct file *image_file,
		struct ioctl_install_params *pm)
{
	int ret = 0;
	struct inode *parent_inode = parent_dir->f_path.dentry->d_inode;
	struct vdfs2_sb_info *sbi = VDFS2_SB(parent_inode->i_sb);
	struct vdfs2_cattree_record *record = NULL;
	struct vdfs2_pack_insert_point_value *ipv;
	struct timespec curr_time = vdfs2_current_time(parent_inode);
	struct inode *install_point_inode = NULL;
	ino_t ino = 0;

	/* +2 becouse of: +1 - for insertion point and +1 for unpacked inode */
	ret = vdfs2_get_free_inode(sbi, &ino, pm->pmc.inodes_cnt + 1);
	if (ret)
		return ret;

	VDFS2_DEBUG_MUTEX("cattree mutex w lock");
	mutex_w_lock(sbi->catalog_tree->rw_tree_lock);
	VDFS2_DEBUG_MUTEX("cattree mutex w lock succ");

	record = vdfs2_cattree_place_record(sbi->catalog_tree,
			parent_inode->i_ino, pm->dst_dir_name,
			strlen(pm->dst_dir_name), VDFS2_CATALOG_PTREE_ROOT);

	if (IS_ERR(record)) {
		VDFS2_DEBUG_MUTEX("cattree mutex w lock un");
		mutex_w_unlock(sbi->catalog_tree->rw_tree_lock);
		vdfs2_free_inode_n(sbi, ino, pm->pmc.inodes_cnt + 1);
		return PTR_ERR(record);
	}

	ipv = (struct vdfs2_pack_insert_point_value *)record->val;
	ipv->common.links_count = cpu_to_le64(1);
	ipv->common.size = cpu_to_le64(pm->pmc.inodes_cnt);
	ipv->common.object_id = cpu_to_le64(ino);
	ipv->common.permissions.file_mode = cpu_to_le16(parent_inode->i_mode);

	ipv->common.permissions.uid = cpu_to_le32(
			from_kuid(&init_user_ns, current_fsuid()));
	if (parent_inode->i_mode & S_ISGID)
		ipv->common.permissions.gid =
			cpu_to_le32(i_gid_read(parent_inode));
	 else
		ipv->common.permissions.gid = cpu_to_le32(
				from_kgid(&init_user_ns, current_fsgid()));

	ipv->common.creation_time.seconds = cpu_to_le64(curr_time.tv_sec);
	ipv->common.creation_time.nanoseconds = cpu_to_le64(curr_time.tv_nsec);
	ipv->start_ino = cpu_to_le64(ino);
	memcpy(ipv->pmc.packtree_layout_version,
			pm->packtree_layout_version, 4);
	ipv->pmc.inodes_cnt = cpu_to_le64(pm->pmc.inodes_cnt);
	ipv->pmc.packoffset = cpu_to_le64(pm->pmc.packoffset);
	ipv->pmc.nfs_offset = cpu_to_le64(pm->pmc.nfs_offset);
	ipv->pmc.xattr_off = cpu_to_le64(pm->pmc.xattr_off);
	ipv->pmc.chtab_off = cpu_to_le64(pm->pmc.chtab_off);
	ipv->pmc.squash_bss = cpu_to_le16(pm->pmc.squash_bss);
	ipv->pmc.compr_type = cpu_to_le16(pm->pmc.compr_type);
	ipv->pmc.chunk_cnt = cpu_to_le16(pm->pmc.chunk_cnt);
	ipv->source_image_parent_object_id = cpu_to_le64(parent_inode->i_ino);

	ipv->source_image_name.length =
			cpu_to_le32(image_file->f_dentry->d_name.len);

	memcpy(ipv->source_image_name.unicode_str,
			image_file->f_dentry->d_name.name,
			image_file->f_dentry->d_name.len);

	vdfs2_mark_record_dirty((struct vdfs2_btree_gen_record *)record);

	/* to see a new folder in target dir we have to create new tree inode
	 * here and build a packtree in readdir function
	 */
	install_point_inode = iget_locked(sbi->sb, ino);
	if (!install_point_inode) {
		ret = -ENOMEM;
		goto exit;
	}
	if (!(install_point_inode->i_state & I_NEW))
		vdfs2_init_inode(VDFS2_I(install_point_inode));

	ret = vdfs2_read_packtree_inode(install_point_inode,
			(struct vdfs2_cattree_key *)record->key);

	vdfs2_release_record((struct vdfs2_btree_gen_record *)record);
	VDFS2_DEBUG_MUTEX("cattree mutex w lock un");
	mutex_w_unlock(sbi->catalog_tree->rw_tree_lock);

	if (ret)
		goto error_exit;

	unlock_new_inode(install_point_inode);
	/* create a new dentry and connect to parent dir */
	update_dentry_with_inode(parent_dir->f_path.dentry,
			install_point_inode);

exit:
	return ret;

error_exit:
	iget_failed(install_point_inode);
	return ret;
}

#ifdef CONFIG_VDFS2_NFS_SUPPORT
static struct installed_packtree_info *get_installed_packtree_info_n(
		struct vdfs2_sb_info *sbi, __u64 i_ino)
{
	struct list_head *pos;

	list_for_each(pos, &sbi->packtree_images.list) {
		struct installed_packtree_info *packtree_info =
			(struct installed_packtree_info *)list_entry(pos,
					struct installed_packtree_info, list);
		if ((i_ino >= packtree_info->params.start_ino) &&
			(i_ino <= packtree_info->params.start_ino +
					packtree_info->params.pmc.inodes_cnt))
			return packtree_info;
	}
	return NULL;
}
#endif


/**
 * @brief		Get inode using indirect key.
 * @param [in] sb	Superblock info structure.
 * @param [in] key	Indirect key structure.
 * @return		Returns pointer to inode on success, errno on failure.
 **/
struct inode *vdfs2_get_packtree_indirect_inode(struct super_block *sb,
		struct vdfs2_indirect_key *key)
{
#ifdef CONFIG_VDFS2_NFS_SUPPORT
	struct vdfs2_cattree_record *file_record = NULL;
	struct inode *inode = NULL;
	int ret = 0;
	struct page *page;
	char *data;
	struct vdfs2_pack_nfs_item coord;
	int offset;
	struct installed_packtree_info *packtree_info =
			get_installed_packtree_info_n(VDFS2_SB(sb), key->ino);
	if (!packtree_info)
		return NULL;
	offset = (key->ino - packtree_info->params.start_ino - 1) *
			sizeof(struct vdfs2_pack_nfs_item);

	page = read_mapping_page(packtree_info->tree->inode->i_mapping,
			packtree_info->params.pmc.nfs_offset +
			(offset >> PAGE_SHIFT), NULL);
	if (IS_ERR(page))
		return (struct inode *)page;

	lock_page(page);
	data = kmap(page);
	memcpy(&coord, &data[offset & (PAGE_SIZE - 1)], sizeof(coord));
	kunmap(page);
	unlock_page(page);
	page_cache_release(page);
	/*
	printk(KERN_ERR "vdfs2_get_packtree_indirect_inode %d bnode %d rec_i %d",
		(int)key->ino, (int)coord.bnode_id, (int)coord.record_i);*/

	/* look up for the file record */
	file_record = (struct vdfs2_cattree_record *)
		vdfs2_btree_build_gen_record(packtree_info->tree,
		le32_to_cpu(coord.bnode_id), le32_to_cpu(coord.record_i));
	if (IS_ERR(file_record)) {
		ret = -ESTALE;
		goto exit;
	}
	inode = vdfs2_get_inode_from_record(file_record);
	if (IS_ERR(inode))
		ret = PTR_ERR(inode);

exit:
	if (file_record && !IS_ERR(file_record))
		vdfs2_release_record(
				(struct vdfs2_btree_gen_record *) file_record);
	if (ret)
		return ERR_PTR(ret);
	else
		return inode;
#endif
	return NULL;
}

/* todo */
int vdfs2_read_squashfs_image_simple(struct inode *inode, __u64 offset, __u32 length,
		void *data)
{
	int ret = 0, i;
	void *mapped_pages;
	int pages_cnt = ((offset & (PAGE_SIZE - 1)) + length + PAGE_SIZE - 1) >>
			PAGE_SHIFT;
	struct page **pages =
			kzalloc(pages_cnt * sizeof(struct page *), GFP_KERNEL);
	if (!pages)
		return -ENOMEM;

	memset(pages, 0, pages_cnt * sizeof(struct page *));
	ret = vdfs2_read_or_create_pages(VDFS2_SB(inode->i_sb), inode, pages,
			offset >> PAGE_SHIFT, pages_cnt, 0);
	if (ret) {
		kfree(pages);
		return ret;
	}
	mapped_pages = vmap(pages, pages_cnt,  VM_MAP, PAGE_KERNEL);
	if (!mapped_pages) {
		ret = -ENOMEM;
		goto exit;
	}

	offset &= (PAGE_SIZE - 1);
	memcpy(data, mapped_pages + offset, length);
	vunmap(mapped_pages);

exit:
	for (i = 0; i < pages_cnt; i++)
		if (pages[i] && !IS_ERR(pages[i]))
			page_cache_release(pages[i]);
	kfree(pages);
	return ret;
}
