#include <linux/pagemap.h>
#include <linux/vmalloc.h>
#include <linux/pagevec.h>
#include <linux/mpage.h>
#include <linux/writeback.h>
#include <linux/buffer_head.h>
#include <linux/uio.h>

#include "vdfs2.h"

static ssize_t write_small_file_page(struct writeback_control *wbc,
	struct inode *inode, void *buffer, size_t size, loff_t write_pos);
static int writepage_small(struct address_space *mapping,
		struct writeback_control *wbc);
static int writepage_tiny(struct address_space *mapping,
		struct writeback_control *wbc);
static ssize_t process_tiny_file(struct kiocb *iocb, struct iov_iter *from,
		struct page *page);
static ssize_t process_small_file(struct kiocb *iocb, struct iov_iter *from,
		struct page *page);

/**
 * @brief			Read multiple pages function.
 * @param [in, out]	ractl	Readahead control
 */
void vdfs2_readahead_tinysmall(struct readahead_control *ractl)
{
	struct inode *inode = ractl->mapping->host;
	struct vdfs2_inode_info *inode_info = VDFS2_I(inode);
	int ret = 0;
	struct page *page;

	mutex_lock(&inode_info->truncate_mutex);
	if (is_vdfs2_inode_flag_set(inode, TINY_FILE) ||
		is_vdfs2_inode_flag_set(inode, SMALL_FILE)) {
		/* go through the list of pages */
		while ((page = readahead_page(ractl))) {
			prefetchw(&page->flags);
			ret = add_to_page_cache_lru(page, ractl->mapping,
					page->index, GFP_KERNEL);
			if (!ret) {
				if (page->index == 0)
					ret = read_tiny_small_page(page);
				else {
					void *data;
					data = kmap(page);
					memset(data, 0, PAGE_SIZE);
					kunmap(page);
					SetPageUptodate(page);
					unlock_page(page);
				}
			} else if (ret == -EEXIST)
				ret = 0;
			else
				break;
		}
		mutex_unlock(&inode_info->truncate_mutex);
		return;
	}
	mutex_unlock(&inode_info->truncate_mutex);
	mpage_readahead(ractl, vdfs2_get_block);
}

/** Brief: get or create a first page for tiny or small file and return it
 *  unlocked
 *
 * */
static struct page *vdfs2_get_tinysmall_page(struct inode *inode)
{
	struct address_space *mapping = inode->i_mapping;
	gfp_t gfp_mask = (mapping_gfp_mask(mapping)) | __GFP_ZERO;
	struct page *page = NULL;
	int ret = 0;
	int index = 0;
again:
	page = find_get_page(mapping, index);
	if (!page) {
		page = alloc_page(gfp_mask);
		if (!page)
			return ERR_PTR(-ENOMEM);

		page->index = index;

		ret = add_to_page_cache_lru(page, mapping, index, GFP_KERNEL);
		if (ret) {
			__free_page(page);
			goto again;
		}
	} else
		lock_page(page);

	if (PageUptodate(page))
		return page;

	/* if it's first page of tiny or small file */
	if ((is_vdfs2_inode_flag_set(inode, TINY_FILE) ||
		is_vdfs2_inode_flag_set(inode, SMALL_FILE))) {

		ret = read_tiny_small_page(page);
		if (ret) {
			unlock_page(page);
			put_page(page);
			return ERR_PTR(ret);
		}
		lock_page(page);
		SetPageUptodate(page);
	}

	return page;
}


int init_small_files_area_manager(struct vdfs2_sb_info *sbi)
{
	int ret = 0;
	struct vdfs2_small_files *small;

	small = kzalloc(sizeof(struct vdfs2_small_files), GFP_KERNEL);
	if (!small)
		return -ENOMEM;

	small->log_cell_size = VDFS2_RAW_SB(sbi)->log_cell_size;
	small->cell_size = 1 << VDFS2_RAW_SB(sbi)->log_cell_size;
	small->bitmap_inode =
			vdfs2_special_iget(sbi->sb, VDFS2_SMALL_AREA_BITMAP);
	if (IS_ERR(small->bitmap_inode)) {
		ret = PTR_ERR(small->bitmap_inode);
		kfree(small);
		return ret;
	}

	small->area_inode = vdfs2_special_iget(sbi->sb, VDFS2_SMALL_AREA);
	if (IS_ERR(small->area_inode))
		ret = PTR_ERR(small->area_inode);

	sbi->small_area = small;
	return ret;
}

void destroy_small_files_area_manager(struct vdfs2_sb_info *sbi)
{
	iput(sbi->small_area->area_inode);
	iput(sbi->small_area->bitmap_inode);
	kfree(sbi->small_area);
}

int vdfs2_get_free_cell(struct vdfs2_sb_info *sbi, u64 *cell_num)
{
	int ret = 0;
	struct vdfs2_small_files *small = sbi->small_area;
	unsigned int page_index = 0;
	struct page *page;
	void *data;
	int real_page_size = VDFS2_BITS_PER_PAGE(SMALL_AREA_BITMAP_MAGIC_LEN);

	*cell_num = ULLONG_MAX;
	while (*cell_num == ULLONG_MAX) {
		page = vdfs2_read_or_create_page(small->bitmap_inode,
				page_index);

		if (IS_ERR_OR_NULL(page)) {
			ret = PTR_ERR(page);
			goto exit;
		}
		lock_page(page);
		data = kmap(page);

		*cell_num = vdfs2_find_next_zero_bit(data, PAGE_SIZE << 3, 0,
				sbi->block_size, SMALL_AREA_BITMAP_MAGIC_LEN);
		if (*cell_num < real_page_size) {

			VDFS2_BUG_ON(vdfs2_set_bits(data, PAGE_SIZE << 3,
				*cell_num, 1, SMALL_AREA_BITMAP_MAGIC_LEN,
				sbi->block_size));

			*cell_num += page_index * real_page_size;
			vdfs2_add_chunk_bitmap(sbi, page, 1);
		} else
			*cell_num = ULLONG_MAX;
		kunmap(page);
		unlock_page(page);
		put_page(page);
		page_index++;
	}
exit:
	return ret;
}

int vdfs2_free_cell(struct vdfs2_sb_info *sbi, __u64 cell_n)
{
	int real_page_size = VDFS2_BITS_PER_PAGE(SMALL_AREA_BITMAP_MAGIC_LEN);
	int ret = 0;
	__u64 page_index = cell_n;
	void *data;
	struct page *page;

	if (cell_n == ULLONG_MAX)
		return 0;

	if (!sbi->small_area)
		goto exit;

	cell_n = do_div(page_index, real_page_size);

	page = vdfs2_read_or_create_page(sbi->small_area->bitmap_inode,
		page_index);

	if (IS_ERR_OR_NULL(page)) {
		ret = PTR_ERR(page);
		goto exit;
	}
	lock_page(page);
	data = kmap(page);

	if (vdfs2_clear_bits(data, PAGE_SIZE, cell_n, 1,
			SMALL_AREA_BITMAP_MAGIC_LEN, sbi->block_size)) {
		printk(KERN_ERR " vdfs2_free_cell_n %llu", cell_n);
		if (!is_sbi_flag_set(sbi, IS_MOUNT_FINISHED))
			ret = -EFAULT;
		else
			VDFS2_BUG();
	}

	vdfs2_add_chunk_bitmap(sbi, page, 1);
	kunmap(page);
	unlock_page(page);
	put_page(page);

exit:
	return ret;
}
static unsigned vdfs2_pagevec_lookup_first_page(struct pagevec *pvec,
		struct address_space *mapping, struct writeback_control *wbc)
{
	pgoff_t writeback_index;
	pgoff_t page_index;
	pgoff_t end;
	pgoff_t start_index = 0;
	int tag;
	if (wbc->range_cyclic) {
		writeback_index = mapping->writeback_index;
		page_index = writeback_index;
		end = -1;
	} else {
		page_index = wbc->range_start >> PAGE_SHIFT;
		end = wbc->range_end >> PAGE_SHIFT;

	}
	if (wbc->sync_mode == WB_SYNC_ALL || wbc->tagged_writepages)
		tag = PAGECACHE_TAG_TOWRITE;
	else
		tag = PAGECACHE_TAG_DIRTY;

	if (wbc->sync_mode == WB_SYNC_ALL || wbc->tagged_writepages)
		tag_pages_for_writeback(mapping, page_index, end);
	pvec->nr = find_get_pages_range_tag(mapping, &start_index, (pgoff_t)-1,
		tag, 1, pvec->pages);
	return pagevec_count(pvec);
}

int writepage_tiny_small(struct address_space *mapping,
		struct writeback_control *wbc)
{
	struct inode *inode = mapping->host;
	struct vdfs2_inode_info *inode_info = VDFS2_I(inode);
	struct super_block *sb = inode->i_sb;
	struct vdfs2_sb_info *sbi = sb->s_fs_info;
	int ret = 0;
	struct pagevec pvec;
	int nr_pages = 0;
	int first_page_found = 0;
	struct page *page;
	int i;

	pagevec_init(&pvec);

	nr_pages = vdfs2_pagevec_lookup_first_page(&pvec, mapping, wbc);

	/* someone wrote all pages for us */
	if (nr_pages == 0) {
		pagevec_release(&pvec);
		return 0;
	}
	/* lookup for first page */
	for (i = 0; i < nr_pages; i++) {
		page = pvec.pages[i];
		if (page->index == 0) {
			first_page_found = 1;
			break;
		}
	}
	/* if no first page in vector, read it from volume */
	if (!first_page_found) {
		page = vdfs2_get_tinysmall_page(inode);
		if (IS_ERR(page)) {
			pagevec_release(&pvec);
			return PTR_ERR(page);
		}
		set_page_dirty(page);
		unlock_page(page);
	}
	vdfs2_start_transaction(sbi);
	/* lock the mutex before starting work with the inode */
	lock_page(page);
	mutex_lock(&inode_info->truncate_mutex);
	if (is_vdfs2_inode_flag_set(inode, TINY_FILE)) {
		ret = writepage_tiny(mapping, wbc);
	} else if (is_vdfs2_inode_flag_set(inode, SMALL_FILE)) {
		ret = writepage_small(mapping, wbc);
	} else {
		mutex_unlock(&inode_info->truncate_mutex);
		unlock_page(page);
		ret = vdfs2_mpage_writepages(mapping, wbc);
		goto exit;
	}
	mutex_unlock(&inode_info->truncate_mutex);
	unlock_page(page);
exit:
	vdfs2_stop_transaction(sbi);
	return ret;
}

static int writepage_tiny(struct address_space *mapping,
		struct writeback_control *wbc)
{
	struct inode *inode = mapping->host;
	struct vdfs2_inode_info *inode_info = VDFS2_I(inode);
	struct super_block *sb = inode->i_sb;
	struct vdfs2_sb_info *sbi = sb->s_fs_info;
	struct pagevec pvec;
	int ret = 0;
	void *file_data;
	int nr_pages = 0;
	pagevec_init(&pvec);
	nr_pages = vdfs2_pagevec_lookup_first_page(&pvec, mapping, wbc);

	/* someone wrote all pages for us */
	if (nr_pages == 0) {
		pagevec_release(&pvec);
		return 0;
	}
	/* if it's realy tiny file */
	if (VDFS2_I(inode)->tiny.i_size < TINY_DATA_SIZE) {
		VDFS2_BUG_ON(pvec.pages[0]->index != 0);
		if (!PageDirty(pvec.pages[0]))
			goto exit;

		file_data = kmap(pvec.pages[0]);
		memcpy(VDFS2_I(inode)->tiny.data, file_data,
			VDFS2_I(inode)->tiny.i_size);
		BUG_ON(!clear_page_dirty_for_io(pvec.pages[0]));
		set_page_writeback(pvec.pages[0]);
		kunmap(pvec.pages[0]);

		VDFS2_I(inode)->tiny.len = VDFS2_I(inode)->tiny.i_size;
		end_page_writeback(pvec.pages[0]);
		ret = vdfs2_write_inode_to_bnode(inode);
	/* Size of file is fit to cell of small area. This situation is
	 * possible if user made truncate to current file and then start mmap
	 */
	} else if (VDFS2_I(inode)->tiny.i_size < sbi->small_area->cell_size &&
			(test_option(sbi, TINYSMALL) != 0)) {
		u64 new_size = 0L;

		new_size = inode_info->tiny.i_size;
		memset(&inode_info->tiny, 0, sizeof(inode_info->small));
		inode_info->small.len = new_size;
		inode_info->small.i_size = new_size;
		inode_info->small.cell = ULLONG_MAX;
		clear_vdfs2_inode_flag(inode, TINY_FILE);
		set_vdfs2_inode_flag(inode, SMALL_FILE);

		if (!PageDirty(pvec.pages[0]))
			goto exit;

		file_data = kmap(pvec.pages[0]);
		ret = write_small_file_page(wbc, inode, file_data, new_size, 0);
		if (IS_ERR_VALUE(ret)) {
			kunmap(pvec.pages[0]);
			goto exit;
		}
		BUG_ON(!clear_page_dirty_for_io(pvec.pages[0]));
		set_page_writeback(pvec.pages[0]);
		kunmap(pvec.pages[0]);
		end_page_writeback(pvec.pages[0]);

		atomic64_dec(&sbi->tiny_files_counter);
		ret = vdfs2_write_inode_to_bnode(inode);

	/* Regular file */
	} else {

		/* reset tiny area */
		memset(&inode_info->tiny, 0, sizeof(inode_info->fork));
		clear_vdfs2_inode_flag(inode, TINY_FILE);
		ret = vdfs2_write_inode_to_bnode(inode);
		if (IS_ERR_VALUE(ret))
			goto exit;


		/* unlock the mutex during writepages */
		mutex_unlock(&VDFS2_I(inode)->truncate_mutex);
		unlock_page(pvec.pages[0]);

		ret = vdfs2_mpage_writepages(mapping, wbc);
		/* lock the mutex, this will be unlocked on higher level */
		lock_page(pvec.pages[0]);
		mutex_lock(&VDFS2_I(inode)->truncate_mutex);
	}
exit:
	pagevec_release(&pvec);
	return ret;

}

static int make_small_file_ordinary(struct inode *inode)
{
	struct vdfs2_inode_info *inode_info = VDFS2_I(inode);
	struct vdfs2_sb_info *sbi = VDFS2_SB(inode->i_sb);
	int ret;


	/* if it's still small file*/
	vdfs2_free_cell(sbi, inode_info->small.cell);
	memset(&inode_info->fork, 0, sizeof(inode_info->fork));
	clear_vdfs2_inode_flag(inode, SMALL_FILE);
	ret = vdfs2_write_inode_to_bnode(inode);

	return ret;
}

static int write_page_small_file(struct writeback_control *wbc,
	struct page *page)
{
	int ret = 0;
	struct inode *inode = page->mapping->host;
	void *data;

	data = kmap(page);
	if (!data)
		return -ENOMEM;

	ret = write_small_file_page(wbc, inode, data, i_size_read(inode), 0);

	kunmap(page);
	return ret;
}

static int writepage_small(struct address_space *mapping,
		struct writeback_control *wbc)
{
	int ret = 0;
	struct inode *inode = mapping->host;
	struct super_block *sb = inode->i_sb;
	struct vdfs2_sb_info *sbi = sb->s_fs_info;
	struct vdfs2_inode_info *inode_info = VDFS2_I(inode);
	int nr_pages;
	struct pagevec pvec;
	pagevec_init(&pvec);
	nr_pages = vdfs2_pagevec_lookup_first_page(&pvec, mapping, wbc);

	/* someone wrote all pages for us */
	if (nr_pages == 0)
		return 0;

	/* this is small file*/
	if (VDFS2_I(inode)->small.i_size < sbi->small_area->cell_size) {

		if (!PageDirty(pvec.pages[0]))
			goto func_end;

		ret = write_page_small_file(wbc, pvec.pages[0]);
		if (!IS_ERR_VALUE(ret))
			ret = 0;
		clear_page_dirty_for_io(pvec.pages[0]);
		set_page_writeback(pvec.pages[0]);
		end_page_writeback(pvec.pages[0]);
	} else { /* we need to expand file */

		ret = make_small_file_ordinary(inode);
		mutex_unlock(&inode_info->truncate_mutex);
		unlock_page(pvec.pages[0]);
		if (!IS_ERR_VALUE(ret))
			ret = vdfs2_mpage_writepages(mapping, wbc);
		/* to be unlocked on higher level */
		lock_page(pvec.pages[0]);
		mutex_lock(&inode_info->truncate_mutex);

	}
func_end:
	pagevec_release(&pvec);
	return ret;
}

int write_tiny_small_page(struct page *page, struct writeback_control *wbc)
{
	struct inode *inode = page->mapping->host;
	struct vdfs2_inode_info *inode_info = VDFS2_I(inode);
	struct vdfs2_sb_info *sbi = VDFS2_SB(inode->i_sb);
	void *data;
	int err = 0;

	BUG_ON(!PageLocked(page));
	/* unlock page just to prevent deadlock with transaction sem */
	unlock_page(page);
	vdfs2_start_transaction(sbi);
	lock_page(page);
	/* someone wrote it for us */
	if (!PageDirty(page))
		goto exit_unlock;

	if (PageWriteback(page)) {
		if (wbc->sync_mode != WB_SYNC_NONE)
			wait_on_page_writeback(page);
		else
			goto exit_unlock;

	}
	BUG_ON(PageWriteback(page));
	if (!clear_page_dirty_for_io(page))
		goto exit_unlock;

	data = kmap(page);

	if (!data) {
		unlock_page(page);
		vdfs2_stop_transaction(sbi);
		return -ENOMEM;
	}

	mutex_lock(&inode_info->truncate_mutex);

	if (is_vdfs2_inode_flag_set(inode, TINY_FILE)) {
		memcpy(inode_info->tiny.data, data, inode_info->tiny.len);
		err = vdfs2_write_inode_to_bnode(inode);

	} else if (is_vdfs2_inode_flag_set(inode, SMALL_FILE)) {
		u64 cell, page_index, offset;
		int log_cell_size = sbi->small_area->log_cell_size;
		int log_cells_per_page = (PAGE_SHIFT - log_cell_size);
		struct page *small_area_page;
		void *small_area_data;

		if (inode_info->small.cell == ULLONG_MAX) {
			err = vdfs2_get_free_cell(sbi, &cell);
			if (err)
				goto exit;
#ifdef CONFIG_VDFS2_QUOTA
			if (inode_info->quota_index != -1) {
				sbi->quotas[inode_info->quota_index].has +=
						sbi->small_area->cell_size;
				update_has_quota(sbi,
					sbi->quotas[inode_info->quota_index].
					ino, inode_info->quota_index);
			}
#endif

			inode_info->small.cell = cell;
		} else
			cell = inode_info->small.cell;

		page_index = cell >> log_cells_per_page;
		offset = cell - (page_index << log_cells_per_page);

		small_area_page = vdfs2_read_or_create_small_area_page(
				sbi->small_area->area_inode, page_index);

		if (IS_ERR(small_area_page)) {
			err = PTR_ERR(small_area_page);
			goto exit;
		}
		lock_page(small_area_page);

		small_area_data = kmap(small_area_page);
		if (!small_area_data) {
			unlock_page(small_area_page);
			err = -ENOMEM;
			goto exit;
		}

		memcpy(small_area_data, data, inode_info->small.len);

		kunmap(small_area_page);
		set_page_dirty(small_area_page);
		unlock_page(small_area_page);
		put_page(small_area_page);


		err = vdfs2_write_inode_to_bnode(inode);
	} else {
		mutex_unlock(&inode_info->truncate_mutex);
		kunmap(page);
		err = block_write_full_page(page, vdfs2_get_block, wbc);
		goto reg_file_exit;
	}

	if (!err && PageDirty(page))
		clear_page_dirty_for_io(page);
exit:
	mutex_unlock(&inode_info->truncate_mutex);
	kunmap(page);
exit_unlock:
	unlock_page(page);
reg_file_exit:
	vdfs2_stop_transaction(sbi);
#if defined(CONFIG_VDFS2_DEBUG)
	if (err)
		VDFS2_ERR("err = %d, ino#%lu name=%s, page index: %lu",
			err, inode->i_ino, inode_info->name, page->index);
#endif
	return err;
}

int read_tiny_small_page(struct page *page)
{
	struct inode *inode = page->mapping->host;
	struct vdfs2_inode_info *inode_info = VDFS2_I(inode);
	struct vdfs2_sb_info *sbi = VDFS2_SB(inode->i_sb);
	void *data;
	int err = 0;
	BUG_ON(page->index != 0);
	BUG_ON(!PageLocked(page));
	data = kmap(page);
	if (!data) {
		unlock_page(page);
		return -ENOMEM;
	}

	if (is_vdfs2_inode_flag_set(inode, TINY_FILE)) {
		loff_t i_size = inode_info->tiny.i_size;

		memcpy(data, inode_info->tiny.data, inode_info->tiny.len);
		memset(data + inode_info->tiny.len, 0, min_t(loff_t, i_size,
				PAGE_SIZE) - inode_info->tiny.len);
	} else {
		u64 cell;
		loff_t i_size = inode_info->small.i_size;
		struct page *small_area_page;
		int page_index, offset;
		void *small_area_data;
		int log_cell_size = sbi->small_area->log_cell_size;
		int log_cells_per_page = (PAGE_SHIFT - log_cell_size);

		cell = inode_info->small.cell;
		page_index = cell >> log_cells_per_page;
		offset = cell - (page_index << log_cells_per_page);

		small_area_page = vdfs2_read_or_create_small_area_page(
			sbi->small_area->area_inode, page_index);

		if (IS_ERR(small_area_page)) {
			err = PTR_ERR(small_area_page);
			goto exit;
		}
		lock_page(small_area_page);
		small_area_data = kmap(small_area_page);
		if (!small_area_data) {
			err = -ENOMEM;
			unlock_page(small_area_page);
			goto exit;
		}

		memcpy(data, small_area_data + offset * (1 << log_cell_size),
				inode_info->small.len);

		memset(data + inode_info->small.len, 0, min_t(loff_t, i_size,
				PAGE_SIZE) - inode_info->small.len);


		kunmap(small_area_page);
		unlock_page(small_area_page);
		put_page(small_area_page);
	}

exit:
	kunmap(page);
	SetPageUptodate(page);
	unlock_page(page);
#if defined(CONFIG_VDFS2_DEBUG)
	if (err)
		VDFS2_DEBUG_INO("err = %d, ino#%lu name=%s, page index: %lu",
			err, inode->i_ino, inode_info->name, page->index);
#endif
	return err;

}

/**
 * @brief	Tiny files writing
 * @param [in]	iocb		Struct describing writing file
 * @param [in]	from		iov_iter to write from
 * @return	Returns number of bytes written or an error code
 */
static ssize_t write_tiny_small_file(struct kiocb *iocb,
	struct iov_iter *from, struct page *page)
{
	struct inode *inode = INODE(iocb);
	struct vdfs2_inode_info *inode_info = VDFS2_I(inode);
#ifdef CONFIG_VDFS2_QUOTA
	struct vdfs2_sb_info *sbi = inode->i_sb->s_fs_info;
#endif
	ssize_t ret = 0;
	void *data = NULL;

#ifdef CONFIG_VDFS2_QUOTA
	if (inode_info->quota_index != -1 &&
			sbi->quotas[inode_info->quota_index].has +
			sbi->small_area->cell_size >
			sbi->quotas[inode_info->quota_index].max) {
		ret = -ENOSPC;
		goto exit;
	}
#endif

	data = kmap(page);

	if (!data) {
		ret = -ENOMEM;
		goto exit;
	}
	ret = __copy_from_user_inatomic(data + iocb->ki_pos,
			from->iov->iov_base, iov_iter_count(from));
	kunmap(page);
	set_page_dirty(page);
	if (is_vdfs2_inode_flag_set(inode, TINY_FILE)) {
		inode_info->tiny.len = iocb->ki_pos + iov_iter_count(from);
		/* update i_size only if it's smaller than tiny.len */
		if (inode_info->tiny.i_size < inode_info->tiny.len)
			inode_info->tiny.i_size = inode_info->tiny.len;
		i_size_write(inode, inode_info->tiny.i_size);
	} else if (is_vdfs2_inode_flag_set(inode, SMALL_FILE)) {
		inode_info->small.len = iocb->ki_pos + iov_iter_count(from);
		/* update i_size only if it's smaller than small.len */
		if (inode_info->small.i_size < inode_info->small.len)
			inode_info->small.i_size = inode_info->small.len;
		i_size_write(inode, inode_info->small.i_size);
#ifdef CONFIG_VDFS2_QUOTA
		if (inode_info->quota_index != -1) {
			sbi->quotas[inode_info->quota_index].has +=
					sbi->small_area->cell_size;
			update_has_quota(sbi,
				sbi->quotas[inode_info->quota_index].ino,
				inode_info->quota_index);
		}
#endif
	}
	ret = vdfs2_write_inode_to_bnode(inode);
	if (!IS_ERR_VALUE(ret))
		ret = iov_iter_count(from);
#ifdef CONFIG_VDFS2_QUOTA
	else if (inode_info->quota_index != -1 &&
			is_vdfs2_inode_flag_set(inode, SMALL_FILE)) {
		sbi->quotas[inode_info->quota_index].has -=
					sbi->small_area->cell_size;
		update_has_quota(sbi, sbi->quotas[inode_info->quota_index].ino,
				inode_info->quota_index);
	}
#endif

	iocb->ki_pos += iov_iter_count(from);
exit:
	return ret;
}

/**
 * @brief	Small files writing
 * @param [in]	inode		inode struct
 * @param [in]	buffer		Buffer to write
 * @param [in]	size		size of buffer
 * @param [in]	write_pos	Position to write from
 * @return	Returns number of bytes written or an error code
 */
static ssize_t write_small_file_page(struct writeback_control *wbc,
	struct inode *inode, void *buffer, size_t size, loff_t write_pos)
{
	struct vdfs2_inode_info *inode_info = VDFS2_I(inode);
	struct vdfs2_sb_info *sbi = VDFS2_SB(inode->i_sb);
	u64 cell;
	struct page *page;
	int log_cell_size = sbi->small_area->log_cell_size;
	int log_cells_per_page = (PAGE_SHIFT - log_cell_size);
	int blkbits = sbi->small_area->area_inode->i_blkbits;
	long unsigned int page_index, offset;
	int ret = 0;
	int len = write_pos + size;
	void *data;
	sector_t offset_alloc_hint = 0;
	sector_t iblock;
	struct vdfs2_extent_info extent = {0};
	int sync = wbc->sync_mode == WB_SYNC_ALL ? 1 : 0;
	if (inode_info->small.cell == ULLONG_MAX) {
		ret = vdfs2_get_free_cell(sbi, &cell);
		if (ret)
			goto exit;
		inode_info->small.cell = cell;
	} else
		cell = inode_info->small.cell;

	page_index = ((long unsigned int)cell) >> log_cells_per_page;
	offset = ((long unsigned int)cell) - (page_index << log_cells_per_page);

	page = vdfs2_read_or_create_small_area_page(sbi->small_area->area_inode,
			page_index);

	if (IS_ERR(page)) {
		ret = PTR_ERR(page);
		goto exit;
	}

	lock_page(page);
	data = kmap(page);
	if (!data) {
		unlock_page(page);
		ret = -ENOMEM;
		goto exit;
	}
	memcpy(data + offset *  (1 << log_cell_size) + write_pos,
		buffer, size);
	kunmap(page);
	/* write directly to dist if it's sync mode */
	if (sync) {
		if (PageWriteback(page))
			wait_on_page_writeback(page);
		set_page_writeback(page);
		iblock = ((sector_t)page->index) << (PAGE_SHIFT - blkbits);
		ret = vdfs2_get_iblock_extent(sbi->small_area->area_inode, iblock,
				&extent, &offset_alloc_hint);

		if (ret) {
			unlock_page(page);
			end_page_writeback(page);
			put_page(page);
			goto exit;
		}

		if (extent.first_block) {
			ret = vdfs2_write_page(sbi, page,
				(extent.first_block + (iblock -
					extent.iblock)) << (blkbits -
					SECTOR_SIZE_SHIFT),
				sbi->block_size >> SECTOR_SIZE_SHIFT, 0, sync);
			if (ret) {
				unlock_page(page);
				put_page(page);
				goto exit;
			}
		} else
			VDFS2_BUG();
	/* just dirty page if it's async mode */
	} else
		set_page_dirty(page);

	unlock_page(page);
	put_page(page);
	if (inode_info->small.len < len)
		inode_info->small.len = len;
	/* if writing buffer is above the file size */
	if (i_size_read(inode) < len) {
		inode_info->small.i_size = inode_info->small.len;
		i_size_write(inode, len);
	}

	ret = vdfs2_write_inode_to_bnode(inode);

exit:
	return ret;
}

/**
 * @brief		Function for convertion tiny files to small files
 * @param [in]	iocb	Struct describing writing file
 * @param [in]	from	iov_iter to write from
 * @return		Returns number of bytes written or an error code
 */
static ssize_t convert_tiny_file_to_small_file(struct kiocb *iocb,
		struct iov_iter *from, struct page *page)
{
	void *data = NULL;
	struct inode *inode = INODE(iocb);
	struct vdfs2_inode_info *inode_info = VDFS2_I(inode);
	struct vdfs2_sb_info *sbi = VDFS2_SB(inode->i_sb);
	int ret;
	u16 len;

#ifdef CONFIG_VDFS2_QUOTA
	if (inode_info->quota_index != -1 &&
			sbi->quotas[inode_info->quota_index].has +
			sbi->small_area->cell_size >
			sbi->quotas[inode_info->quota_index].max) {
		ret = -ENOSPC;
		goto exit;
	}
#endif

	data = kmap(page);
	if (!data) {
		ret = -ENOMEM;
		goto exit;
	}
	len = inode_info->tiny.len;
	/* copy user data */
	ret = __copy_from_user_inatomic(data + iocb->ki_pos,
		from->iov->iov_base, iov_iter_count(from));
	/* convert tiny inode info to small inode info */
	memset(&inode_info->tiny, 0, sizeof(inode_info->small));
	inode_info->small.cell = ULLONG_MAX;
	clear_vdfs2_inode_flag(inode, TINY_FILE);
	set_vdfs2_inode_flag(inode, SMALL_FILE);
	atomic64_dec(&sbi->tiny_files_counter);
	iocb->ki_pos = iocb->ki_pos + iov_iter_count(from);
	inode_info->small.i_size = i_size_read(inode);
	inode_info->small.len =  len + iov_iter_count(from);
	/* if data was written successfully */
	ret = iov_iter_count(from);
	if (iocb->ki_pos > inode_info->small.len)
		inode_info->small.len = iocb->ki_pos;
	/* update i_size only if it's smaller than small.len */
	if (inode_info->small.i_size < inode_info->small.len)
		inode_info->small.i_size = inode_info->small.len;
	i_size_write(inode, inode_info->small.i_size);
	kunmap(page);
	set_page_dirty(page);

	if (vdfs2_write_inode_to_bnode(inode) != 0)
		ret = 0;
exit:

#ifdef CONFIG_VDFS2_QUOTA
	if (inode_info->quota_index != -1 && !IS_ERR_VALUE(ret)) {
		sbi->quotas[inode_info->quota_index].has +=
				sbi->small_area->cell_size;
		update_has_quota(sbi,
			sbi->quotas[inode_info->quota_index].ino,
			inode_info->quota_index);
	}
#endif
	return ret;
}


/**
 * @brief		Function for convertion tiny files to ordinary files
 * @param [in]	iocb	Struct describing writing file
 * @param [in]	from	iov_iter to write from
 * @return		Returns number of bytes written or an error code
 */
static ssize_t convert_tiny_file_to_ordinary_file(struct kiocb *iocb,
		struct iov_iter *from, struct page *page)
{
	ssize_t ret = 0;
	struct inode *inode = INODE(iocb);
	struct vdfs2_inode_info *inode_info = VDFS2_I(inode);
	struct vdfs2_sb_info *sbi = VDFS2_SB(inode->i_sb);
	if (IS_ERR(page))
		return PTR_ERR(page);

	clear_vdfs2_inode_flag(inode, TINY_FILE);
	/* reset tiny area */
	memset(&inode_info->tiny, 0, sizeof(inode_info->fork));
	set_page_dirty(page);

	mutex_unlock(&inode_info->truncate_mutex);
	unlock_page(page);
	vdfs2_stop_transaction(sbi);
	atomic64_dec(&VDFS2_SB(inode->i_sb)->tiny_files_counter);
	/* Writing user data */
	ret = generic_file_write_iter(iocb, from);
	vdfs2_start_transaction(sbi);
	lock_page(page);
	mutex_lock(&inode_info->truncate_mutex);
	return ret;
}

/**
 * @brief		Function for convertion small files to ordinary files
 * @param [in]	iocb	Struct describing writing file
 * @param [in]	from	iov_iter to write from
 * @return		Returns number of bytes written or an error code
 */
static ssize_t convert_small_file_to_ordinary_file(struct kiocb *iocb,
		struct iov_iter *from, struct page *page)
{
	ssize_t ret;
	struct inode *inode = INODE(iocb);
	struct vdfs2_inode_info *inode_info = VDFS2_I(inode);
	struct vdfs2_sb_info *sbi = VDFS2_SB(inode->i_sb);
	if (IS_ERR(page))
		return PTR_ERR(page);

	ret = make_small_file_ordinary(inode);

	if (IS_ERR_VALUE(ret))
		return ret;

	set_page_dirty(page);

	mutex_unlock(&inode_info->truncate_mutex);
	unlock_page(page);
	vdfs2_stop_transaction(sbi);
	/* Writing user data */
	ret = generic_file_write_iter(iocb, from);
	vdfs2_start_transaction(sbi);
	lock_page(page);
	mutex_lock(&inode_info->truncate_mutex);
	return ret;
}

/**
 * @brief		Write tiny and small file to page cache
 * @param [in]	iocb	Struct describing writing file
 * @param [in]	from	iov_iter to write from
 * @return		Returns number of bytes written or an error code
 */
ssize_t process_tiny_small_file(struct kiocb *iocb, struct iov_iter *from)
{
	struct inode *inode = INODE(iocb);
	struct vdfs2_sb_info *sbi = VDFS2_SB(inode->i_sb);
	ssize_t ret = 0;
	struct page *page = NULL;


	vdfs2_start_transaction(sbi);
	page = vdfs2_get_tinysmall_page(inode);
	if (IS_ERR(page)) {
		ret = PTR_ERR(page);
		vdfs2_stop_transaction(sbi);
		goto exit;
	}
	/* lock the mutex before starting work with the inode */
	mutex_lock(&VDFS2_I(inode)->truncate_mutex);
	if (is_vdfs2_inode_flag_set(inode, TINY_FILE))
		ret = process_tiny_file(iocb, from, page);

	/* The target file is small*/
	else if (is_vdfs2_inode_flag_set(inode, SMALL_FILE))
		ret = process_small_file(iocb, from, page);

	/* The target file is a normal file */
	else {
		mutex_unlock(&VDFS2_I(inode)->truncate_mutex);
		unlock_page(page);
		put_page(page);
		vdfs2_stop_transaction(sbi);
		ret = generic_file_write_iter(iocb, from);
		goto exit;
	}
	mutex_unlock(&VDFS2_I(inode)->truncate_mutex);
	unlock_page(page);
	put_page(page);
	vdfs2_stop_transaction(sbi);
exit:
	return ret;
}
/**
 * @brief		Tiny files (data is stored at metadata area) processing
 * @param [in]	iocb	Struct describing writing file
 * @param [in]	from	iov_iter to write from
 * @return		Returns number of bytes written or an error code
 */
static ssize_t process_tiny_file(struct kiocb *iocb, struct iov_iter *from,
		struct page *page)
{
	ssize_t ret;
	struct inode *inode = INODE(iocb);
	struct vdfs2_sb_info *sbi = VDFS2_SB(inode->i_sb);
	if (unlikely(iocb->ki_filp->f_flags & O_DIRECT))
		return -EINVAL;

	/*The target file is tiny and writing data is placed in metadata */
	if (iocb->ki_pos + iov_iter_count(from) <= TINY_DATA_SIZE)
		ret = write_tiny_small_file(iocb, from, page);

	/* The target file is tiny and writing data isn't placed in metadata,
	 * but placed in small area cell*/
	else if ((iocb->ki_pos + iov_iter_count(from) <=
			VDFS2_SB(INODE(iocb)->i_sb)->small_area->cell_size) &&
			(test_option(sbi, TINYSMALL) != 0))
		ret = convert_tiny_file_to_small_file(iocb, from, page);

	/* The target file is tiny and writing data isn't placed neither
	 * metadata nor small area cell, clear tiny flag and write data in
	 * a common way */
	else
		ret = convert_tiny_file_to_ordinary_file(iocb, from, page);

	return ret;
}

/**
 * @brief		Small files (data is stored at special area) processing
 * @param [in]	iocb	Struct describing writing file
 * @param [in]	from	iov_iter to write from
 * @return		Returns number of bytes written or an error code
 */
static ssize_t process_small_file(struct kiocb *iocb, struct iov_iter *from,
		struct page *page)
{
	ssize_t ret;
	if (unlikely(iocb->ki_filp->f_flags & O_DIRECT))
		return -EINVAL;
	/* The target file is small, data is placed at small area cell */
	if (iocb->ki_pos + iov_iter_count(from) <=
			VDFS2_SB(INODE(iocb)->i_sb)->small_area->cell_size) {
		ret = write_tiny_small_file(iocb, from, page);
	/* The target file is small, data isn't placed at small area cell */
	} else {
		ret = convert_small_file_to_ordinary_file(iocb,	from, page);
	}
	return ret;
}

