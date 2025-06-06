/*
 * Copyright (C) 2007 Oracle.  All rights reserved.
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public
 * License v2 as published by the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * General Public License for more details.
 *
 * You should have received a copy of the GNU General Public
 * License along with this program; if not, write to the
 * Free Software Foundation, Inc., 59 Temple Place - Suite 330,
 * Boston, MA 021110-1307, USA.
 */

#include "kerncompat.h"
#include <stdio.h>
#include <errno.h>
#include "kernel-shared/ctree.h"
#include "kernel-shared/disk-io.h"
#include "kernel-shared/transaction.h"
#include "kernel-shared/file-item.h"
#include "kernel-shared/extent_io.h"
#include "common/internal.h"

#define MAX_CSUM_ITEMS(r, size) ((((BTRFS_LEAF_DATA_SIZE(r->fs_info) - \
			       sizeof(struct btrfs_item) * 2) / \
			       size) - 1))
int btrfs_insert_file_extent(struct btrfs_trans_handle *trans,
			     struct btrfs_root *root,
			     u64 ino, u64 file_pos,
			     struct btrfs_file_extent_item *stack_fi)
{
	int ret = 0;
	int is_hole = 0;
	struct btrfs_key file_key;

	if (btrfs_stack_file_extent_disk_bytenr(stack_fi) == 0)
		is_hole = 1;

	/* For NO_HOLES, we don't insert hole file extent */
	if (btrfs_fs_incompat(root->fs_info, NO_HOLES) && is_hole)
		return 0;

	/* For hole, its disk_bytenr and disk_num_bytes must be 0 */
	if (is_hole)
		btrfs_set_stack_file_extent_disk_num_bytes(stack_fi, 0);

	file_key.objectid = ino;
	file_key.type = BTRFS_EXTENT_DATA_KEY;
	file_key.offset = file_pos;

	btrfs_set_stack_file_extent_generation(stack_fi, trans->transid);
	ret = btrfs_insert_item(trans, root, &file_key, stack_fi,
				sizeof(struct btrfs_file_extent_item));
	return ret;
}

int btrfs_insert_inline_extent(struct btrfs_trans_handle *trans,
			       struct btrfs_root *root, u64 objectid,
			       u64 offset, const char *buffer, size_t size,
			       enum btrfs_compression_type comp,
			       u64 ram_bytes)
{
	struct btrfs_fs_info *fs_info = trans->fs_info;
	struct btrfs_key key;
	struct btrfs_path *path;
	struct extent_buffer *leaf;
	unsigned long ptr;
	struct btrfs_file_extent_item *ei;
	u32 datasize;
	int err = 0;
	int ret;

	if (size > max(btrfs_symlink_max_size(fs_info),
		       btrfs_data_inline_max_size(fs_info)))
		return -EUCLEAN;

	path = btrfs_alloc_path();
	if (!path)
		return -ENOMEM;

	key.objectid = objectid;
	key.type = BTRFS_EXTENT_DATA_KEY;
	key.offset = offset;

	datasize = btrfs_file_extent_calc_inline_size(size);
	ret = btrfs_insert_empty_item(trans, root, path, &key, datasize);
	if (ret) {
		err = ret;
		goto fail;
	}

	leaf = path->nodes[0];
	ei = btrfs_item_ptr(leaf, path->slots[0],
			    struct btrfs_file_extent_item);
	btrfs_set_file_extent_generation(leaf, ei, trans->transid);
	btrfs_set_file_extent_type(leaf, ei, BTRFS_FILE_EXTENT_INLINE);
	btrfs_set_file_extent_ram_bytes(leaf, ei, ram_bytes);
	btrfs_set_file_extent_compression(leaf, ei, comp);
	btrfs_set_file_extent_encryption(leaf, ei, 0);
	btrfs_set_file_extent_other_encoding(leaf, ei, 0);

	ptr = btrfs_file_extent_inline_start(ei) + offset - key.offset;
	write_extent_buffer(leaf, buffer, ptr, size);
	btrfs_mark_buffer_dirty(leaf);
fail:
	btrfs_free_path(path);
	return err;
}

struct btrfs_csum_item *
btrfs_lookup_csum(struct btrfs_trans_handle *trans,
		  struct btrfs_root *root,
		  struct btrfs_path *path,
		  u64 bytenr, u64 csum_objectid, u16 csum_type, int cow)
{
	int ret;
	struct btrfs_key file_key;
	struct btrfs_key found_key;
	struct btrfs_csum_item *item;
	struct extent_buffer *leaf;
	u64 csum_offset = 0;
	u16 csum_size = btrfs_csum_type_size(csum_type);
	int csums_in_item;

	file_key.objectid = csum_objectid;
	file_key.type = BTRFS_EXTENT_CSUM_KEY;
	file_key.offset = bytenr;
	ret = btrfs_search_slot(trans, root, &file_key, path, 0, cow);
	if (ret < 0)
		goto fail;
	leaf = path->nodes[0];

	if (ret > 0) {
		ret = 1;
		if (path->slots[0] == 0)
			goto fail;
		path->slots[0]--;
		btrfs_item_key_to_cpu(leaf, &found_key, path->slots[0]);
		if (found_key.type != BTRFS_EXTENT_CSUM_KEY ||
		    found_key.objectid != csum_objectid)
			goto fail;

		csum_offset = (bytenr - found_key.offset) /
				root->fs_info->sectorsize;
		csums_in_item = btrfs_item_size(leaf, path->slots[0]);
		csums_in_item /= csum_size;

		if (csum_offset >= csums_in_item) {
			ret = -EFBIG;
			goto fail;
		}
	}
	item = btrfs_item_ptr(leaf, path->slots[0], struct btrfs_csum_item);
	item = (struct btrfs_csum_item *)((unsigned char *)item +
					  csum_offset * csum_size);
	return item;
fail:
	if (ret > 0)
		ret = -ENOENT;
	return ERR_PTR(ret);
}

int btrfs_csum_file_block(struct btrfs_trans_handle *trans, u64 logical,
			  u64 csum_objectid, u32 csum_type, const char *data)
{
	struct btrfs_root *root = btrfs_csum_root(trans->fs_info, logical);
	int ret = 0;
	struct btrfs_key file_key;
	struct btrfs_key found_key;
	u64 next_offset = (u64)-1;
	int found_next = 0;
	struct btrfs_path *path;
	struct btrfs_csum_item *item;
	struct extent_buffer *leaf = NULL;
	u64 csum_offset;
	u8 csum_result[BTRFS_CSUM_SIZE];
	u32 sectorsize = root->fs_info->sectorsize;
	u32 nritems;
	u32 ins_size;
	u16 csum_size = btrfs_csum_type_size(csum_type);

	path = btrfs_alloc_path();
	if (!path)
		return -ENOMEM;

	file_key.objectid = csum_objectid;
	file_key.type = BTRFS_EXTENT_CSUM_KEY;
	file_key.offset = logical;

	item = btrfs_lookup_csum(trans, root, path, logical, csum_objectid,
				 csum_type, 1);
	if (!IS_ERR(item)) {
		leaf = path->nodes[0];
		ret = 0;
		goto found;
	}
	ret = PTR_ERR(item);
	if (ret == -EFBIG) {
		u32 item_size;

		/* printf("item not big enough for bytenr %llu\n", bytenr); */
		/* we found one, but it isn't big enough yet */
		leaf = path->nodes[0];
		item_size = btrfs_item_size(leaf, path->slots[0]);
		if ((item_size / csum_size) >= MAX_CSUM_ITEMS(root, csum_size)) {
			/* already at max size, make a new one */
			goto insert;
		}
	} else {
		int slot = path->slots[0] + 1;
		/* we didn't find a csum item, insert one */
		nritems = btrfs_header_nritems(path->nodes[0]);
		if (path->slots[0] >= nritems - 1) {
			ret = btrfs_next_leaf(root, path);
			if (ret == 1)
				found_next = 1;
			if (ret != 0)
				goto insert;
			slot = 0;
		}
		btrfs_item_key_to_cpu(path->nodes[0], &found_key, slot);
		if (found_key.objectid != csum_objectid ||
		    found_key.type != BTRFS_EXTENT_CSUM_KEY) {
			found_next = 1;
			goto insert;
		}
		next_offset = found_key.offset;
		found_next = 1;
		goto insert;
	}

	/*
	 * at this point, we know the tree has an item, but it isn't big
	 * enough yet to put our csum in.  Grow it
	 */
	btrfs_release_path(path);
	path->search_for_extension = 1;
	ret = btrfs_search_slot(trans, root, &file_key, path,
				csum_size, 1);
	path->search_for_extension = 0;
	if (ret < 0)
		goto fail;
	if (ret == 0) {
		BUG();
	}
	if (path->slots[0] == 0) {
		goto insert;
	}
	path->slots[0]--;
	leaf = path->nodes[0];
	btrfs_item_key_to_cpu(leaf, &found_key, path->slots[0]);
	csum_offset = (file_key.offset - found_key.offset) / sectorsize;
	if (found_key.objectid != csum_objectid ||
	    found_key.type != BTRFS_EXTENT_CSUM_KEY ||
	    csum_offset >= MAX_CSUM_ITEMS(root, csum_size)) {
		goto insert;
	}
	if (csum_offset >= btrfs_item_size(leaf, path->slots[0]) /
	    csum_size) {
		u32 diff = (csum_offset + 1) * csum_size;
		diff = diff - btrfs_item_size(leaf, path->slots[0]);
		if (diff != csum_size)
			goto insert;
		btrfs_extend_item(path, diff);
		goto csum;
	}

insert:
	btrfs_release_path(path);
	csum_offset = 0;
	if (found_next) {
		u64 tmp = min(logical + sectorsize, next_offset);
		tmp -= file_key.offset;
		tmp /= sectorsize;
		tmp = max((u64)1, tmp);
		tmp = min(tmp, (u64)MAX_CSUM_ITEMS(root, csum_size));
		ins_size = csum_size * tmp;
	} else {
		ins_size = csum_size;
	}
	ret = btrfs_insert_empty_item(trans, root, path, &file_key,
				      ins_size);
	if (ret < 0)
		goto fail;
	if (ret != 0) {
		WARN_ON(1);
		goto fail;
	}
csum:
	leaf = path->nodes[0];
	item = btrfs_item_ptr(leaf, path->slots[0], struct btrfs_csum_item);
	ret = 0;
	item = (struct btrfs_csum_item *)((unsigned char *)item +
					  csum_offset * csum_size);
found:
	btrfs_csum_data(csum_type, (u8 *)data, csum_result, sectorsize);
	write_extent_buffer(leaf, csum_result, (unsigned long)item,
			    csum_size);
	btrfs_mark_buffer_dirty(path->nodes[0]);
fail:
	btrfs_free_path(path);
	return ret;
}

/*
 * helper function for csum removal, this expects the
 * key to describe the csum pointed to by the path, and it expects
 * the csum to overlap the range [bytenr, len]
 *
 * The csum should not be entirely contained in the range and the
 * range should not be entirely contained in the csum.
 *
 * This calls btrfs_truncate_item with the correct args based on the
 * overlap, and fixes up the key as required.
 */
static noinline int truncate_one_csum(struct btrfs_root *root,
				      struct btrfs_path *path,
				      struct btrfs_key *key,
				      u64 bytenr, u64 len)
{
	struct extent_buffer *leaf;
	u16 csum_size = root->fs_info->csum_size;
	u64 csum_end;
	u64 end_byte = bytenr + len;
	u32 blocksize = root->fs_info->sectorsize;

	leaf = path->nodes[0];
	csum_end = btrfs_item_size(leaf, path->slots[0]) / csum_size;
	csum_end *= root->fs_info->sectorsize;
	csum_end += key->offset;

	if (key->offset < bytenr && csum_end <= end_byte) {
		/*
		 *         [ bytenr - len ]
		 *         [   ]
		 *   [csum     ]
		 *   A simple truncate off the end of the item
		 */
		u32 new_size = (bytenr - key->offset) / blocksize;
		new_size *= csum_size;
		btrfs_truncate_item(path, new_size, 1);
	} else if (key->offset >= bytenr && csum_end > end_byte &&
		   end_byte > key->offset) {
		/*
		 *         [ bytenr - len ]
		 *                 [ ]
		 *                 [csum     ]
		 * we need to truncate from the beginning of the csum
		 */
		u32 new_size = (csum_end - end_byte) / blocksize;
		new_size *= csum_size;

		btrfs_truncate_item(path, new_size, 0);

		key->offset = end_byte;
		btrfs_set_item_key_safe(root->fs_info, path, key);
	} else {
		BUG();
	}
	return 0;
}

/*
 * deletes the csum items from the csum tree for a given
 * range of bytes.
 */
int btrfs_del_csums(struct btrfs_trans_handle *trans, struct btrfs_root *root,
		    u64 bytenr, u64 len)
{
	struct btrfs_path *path;
	struct btrfs_key key;
	u64 end_byte = bytenr + len;
	u64 csum_end;
	struct extent_buffer *leaf;
	int ret;
	u16 csum_size = trans->fs_info->csum_size;
	int blocksize = trans->fs_info->sectorsize;

	path = btrfs_alloc_path();
	if (!path)
		return -ENOMEM;

	while (1) {
		key.objectid = BTRFS_EXTENT_CSUM_OBJECTID;
		key.type = BTRFS_EXTENT_CSUM_KEY;
		key.offset = end_byte - 1;

		ret = btrfs_search_slot(trans, root, &key, path, -1, 1);
		if (ret > 0) {
			if (path->slots[0] == 0)
				goto out;
			path->slots[0]--;
		}
		leaf = path->nodes[0];
		btrfs_item_key_to_cpu(leaf, &key, path->slots[0]);

		if (key.objectid != BTRFS_EXTENT_CSUM_OBJECTID ||
		    key.type != BTRFS_EXTENT_CSUM_KEY) {
			break;
		}

		if (key.offset >= end_byte)
			break;

		csum_end = btrfs_item_size(leaf, path->slots[0]) / csum_size;
		csum_end *= blocksize;
		csum_end += key.offset;

		/* this csum ends before we start, we're done */
		if (csum_end <= bytenr)
			break;

		/* delete the entire item, it is inside our range */
		if (key.offset >= bytenr && csum_end <= end_byte) {
			ret = btrfs_del_item(trans, root, path);
			BUG_ON(ret);
		} else if (key.offset < bytenr && csum_end > end_byte) {
			unsigned long offset;
			unsigned long shift_len;
			unsigned long item_offset;
			/*
			 *        [ bytenr - len ]
			 *     [csum                ]
			 *
			 * Our bytes are in the middle of the csum,
			 * we need to split this item and insert a new one.
			 *
			 * But we can't drop the path because the
			 * csum could change, get removed, extended etc.
			 *
			 * The trick here is the max size of a csum item leaves
			 * enough room in the tree block for a single
			 * item header.  So, we split the item in place,
			 * adding a new header pointing to the existing
			 * bytes.  Then we loop around again and we have
			 * a nicely formed csum item that we can neatly
			 * truncate.
			 */
			offset = (bytenr - key.offset) / blocksize;
			offset *= csum_size;

			shift_len = (len / blocksize) * csum_size;

			item_offset = btrfs_item_ptr_offset(leaf,
							    path->slots[0]);

			memset_extent_buffer(leaf, 0, item_offset + offset,
					     shift_len);
			key.offset = bytenr;

			/*
			 * btrfs_split_item returns -EAGAIN when the
			 * item changed size or key
			 */
			ret = btrfs_split_item(trans, root, path, &key,
					       offset);
			BUG_ON(ret && ret != -EAGAIN);

			key.offset = end_byte - 1;
		} else {
			ret = truncate_one_csum(root, path, &key, bytenr,
						len);
			BUG_ON(ret);
		}
		btrfs_release_path(path);
	}
out:
	btrfs_free_path(path);
	return 0;
}
