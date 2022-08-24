/*
**  %Z% %I% %W% %G% %U%
**
**  ZZ_Copyright_BEGIN
**
**
**  Licensed Materials - Property of IBM
**
**  IBM Linear Tape File System Single Drive Edition Version 2.2.0.2 for Linux and Mac OS X
**
**  Copyright IBM Corp. 2010, 2014
**
**  This file is part of the IBM Linear Tape File System Single Drive Edition for Linux and Mac OS X
**  (formally known as IBM Linear Tape File System)
**
**  The IBM Linear Tape File System Single Drive Edition for Linux and Mac OS X is free software;
**  you can redistribute it and/or modify it under the terms of the GNU Lesser
**  General Public License as published by the Free Software Foundation,
**  version 2.1 of the License.
**
**  The IBM Linear Tape File System Single Drive Edition for Linux and Mac OS X is distributed in the
**  hope that it will be useful, but WITHOUT ANY WARRANTY; without even the
**  implied warranty of MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.
**  See the GNU Lesser General Public License for more details.
**
**  You should have received a copy of the GNU Lesser General Public
**  License along with this library; if not, write to the Free Software
**  Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA 02111-1307 USA
**  or download the license from <http://www.gnu.org/licenses/>.
**
**
**  ZZ_Copyright_END
**
*************************************************************************************
**
** COMPONENT NAME:  IBM Linear Tape File System
**
** FILE NAME:       ltfs_fsops.c
**
** DESCRIPTION:     LTFS file and directory operations.
**
** AUTHORS:         Brian Biskeborn
**                  IBM Almaden Research Center
**                  bbiskebo@us.ibm.com
**
**                  Lucas C. Villa Real
**                  IBM Almaden Research Center
**                  lucasvr@us.ibm.com
**
*************************************************************************************
**
**  (C) Copyright 2015-2017 Hewlett Packard Enterprise Development LP
**
**  13/10/2017 Added changes to support SNIA 2.4
**
*************************************************************************************
*/

#include "ltfs.h"
#include "ltfs_internal.h"
#include "ltfs_fsops.h"
#include "ltfs_fsops_raw.h"
#include "fs.h"
#include "iosched.h"
#include "tape.h"
#include "xattr.h"
#include "dcache.h"
#include "pathname.h"
#include "index_criteria.h"
#include "arch/time_internal.h"

int ltfs_fsops_open(const char *path, bool open_write, bool use_iosched, struct dentry **d,
	struct ltfs_volume *vol)
{
	int ret;
	char *path_norm;

	CHECK_ARG_NULL(path, -LTFS_NULL_ARG);
	CHECK_ARG_NULL(d, -LTFS_NULL_ARG);
	CHECK_ARG_NULL(vol, -LTFS_NULL_ARG);

	if (open_write) {
#ifdef HPE_mingw_BUILD
		ret = tape_read_only(vol->device, ltfs_part_id2num(ltfs_ip_id(vol), vol));
		if (ret == 0 || ret == -LTFS_LESS_SPACE)
			ret = tape_read_only(vol->device, ltfs_part_id2num(ltfs_dp_id(vol), vol));
#else /* Appending was allowed when VAL locked hence differenciating the code */
		ret = ltfs_get_tape_readonly(vol);
#endif
		/* Check for a read-only volume, but ignore ENOSPC: file systems do
		 * not typically check for medium full on open.
		 */
		if (ret < 0 && (ret != -LTFS_LESS_SPACE || ret != -LTFS_NO_SPACE))
			return ret;
	}

	/* Validate and normalize the path */
	ret = pathname_format(path, &path_norm, true, true);
	if (ret == -LTFS_INVALID_PATH)
		return -LTFS_INVALID_SRC_PATH;
	else if (ret < 0) {
		ltfsmsg(LTFS_ERR, "11039E", ret);
		return ret;
	}

	if (use_iosched && iosched_initialized(vol))
		ret = iosched_open(path_norm, open_write, d, vol);
	else
		ret = ltfs_fsraw_open(path_norm, open_write, d, vol);

	if ( ret==0 ){
		if ( open_write && (**d).isslink ) {
			ltfs_fsops_close(*d, false, open_write, use_iosched, vol);
			ret=-LTFS_RDONLY_VOLUME;
		}
	}

	free(path_norm);
	return ret;
}

int ltfs_fsops_open_combo(const char *path, bool open_write, bool use_iosched,
						  struct dentry **d, bool *is_readonly,
						  bool isopendir, struct ltfs_volume *vol)
{
	int ret;
	char *path_norm;
	struct dentry *dtmp;


	CHECK_ARG_NULL(path, -LTFS_NULL_ARG);
	CHECK_ARG_NULL(d, -LTFS_NULL_ARG);
	CHECK_ARG_NULL(vol, -LTFS_NULL_ARG);

	if (open_write) {
		ret = ltfs_get_tape_readonly(vol);
		if (ret < 0 && ret != -LTFS_LESS_SPACE)
			return ret;
	}

	/* Validate and normalize the path */
	ret = pathname_format(path, &path_norm, true, true);
	if (ret == -LTFS_INVALID_PATH)
		return -LTFS_INVALID_SRC_PATH;
	else if (ret < 0) {
		ltfsmsg(LTFS_ERR, "11039E", ret);
		return ret;
	}

	ret = ltfs_get_volume_lock(false, vol);
	if (ret < 0)
		goto out_open_combo;

	if (dcache_initialized(NULL))
		ret = dcache_open(path_norm, &dtmp, vol);
	else
		ret = fs_path_lookup(path_norm, 0, &dtmp, vol->index);

	if (ret<0) {
		releaseread_mrsw(&vol->lock);
		goto out_open_combo;
	}

	if ( (isopendir && !dtmp->isdir) || (!isopendir && dtmp->isdir) )
		ret = -LTFS_NO_DENTRY;

	if (dcache_initialized(NULL))
		dcache_close(dtmp, true, true, vol);
	else
		fs_release_dentry(dtmp);
	releaseread_mrsw(&vol->lock);

	if (ret<0)
		goto out_open_combo;

	if (use_iosched && iosched_initialized(vol))
		ret = iosched_open(path_norm, open_write, d, vol);
	else
		ret = ltfs_fsraw_open(path_norm, open_write, d, vol);

	if (*d && ret == 0)
		*is_readonly = (*d)->readonly;

out_open_combo:
	free(path_norm);
	return ret;
}

int ltfs_fsops_close(struct dentry *d, bool dirty, bool open_write, bool use_iosched, struct ltfs_volume *vol)
{
	int ret, ret_u = 0;

	CHECK_ARG_NULL(d, -LTFS_NULL_ARG);
	CHECK_ARG_NULL(vol, -LTFS_NULL_ARG);

	if (d->need_update_time) {
		acquirewrite_mrsw(&d->meta_lock);
		get_current_timespec(&d->modify_time);
		d->change_time = d->modify_time;
		releasewrite_mrsw(&d->meta_lock);
		d->need_update_time = false;
	}

	if (dirty && dcache_initialized(NULL))
		dcache_flush(d, FLUSH_ALL, vol);

	if (open_write)
		ret_u = ltfs_fsops_update_used_blocks(d, vol);

	if (use_iosched && ! d->isdir && iosched_initialized(vol))
		ret = iosched_close(d, dirty, vol);
	else
		ret = ltfs_fsraw_close(d);

	if ( !ret && ret_u)
		ret = ret_u;

	return ret;
}

int ltfs_fsops_update_used_blocks(struct dentry *d, struct ltfs_volume *vol)
{
	int ret;
	uint64_t used_save = 0;
	int64_t used_diff = 0;

	CHECK_ARG_NULL(d, -LTFS_NULL_ARG);
	CHECK_ARG_NULL(vol, -LTFS_NULL_ARG);

	acquireread_mrsw(&d->contents_lock);
	acquirewrite_mrsw(&d->meta_lock);
	used_save = d->used_blocks;
	d->used_blocks = fs_get_used_blocks(d);
	used_diff = d->used_blocks - used_save;
	releasewrite_mrsw(&d->meta_lock);
	releaseread_mrsw(&d->contents_lock);

	ret = ltfs_update_valid_block_count(vol, used_diff);

	return ret;
}

int ltfs_fsops_create(const char *path, bool isdir, bool readonly, struct dentry **dentry,
	struct ltfs_volume *vol)
{
	int ret;
	char *path_norm, *filename, *dentry_path = NULL;
	struct dentry *d = NULL, *parent = NULL;

	CHECK_ARG_NULL(path, -LTFS_NULL_ARG);
	CHECK_ARG_NULL(dentry, -LTFS_NULL_ARG);
	CHECK_ARG_NULL(vol, -LTFS_NULL_ARG);

	/* Make sure the device is online and writable */
	ret = ltfs_get_tape_readonly(vol);
	if (ret < 0)
		return ret;

	ret = ltfs_test_unit_ready(vol);
	if (ret < 0) {
		ltfsmsg(LTFS_ERR, "11047E");
		return ret;
	}

	/* Validate and normalize the path */
	ret = pathname_format(path, &path_norm, true, true);
	if (ret < 0) {
		if (ret != -LTFS_INVALID_PATH)
			ltfsmsg(LTFS_ERR, "11048E", ret);
		return ret;
	}

	ret = ltfs_get_volume_lock(false, vol);
	if (ret < 0) {
		free(path_norm);
		return ret;
	}

	/* Look up parent directory */
	fs_split_path(path_norm, &filename, strlen(path_norm) + 1);
	if (dcache_initialized(NULL)) {
		ret = asprintf(&dentry_path, "%s/%s", path_norm, filename);
		if (ret < 0) {
			ltfsmsg(LTFS_ERR, "10001E", "ltfs_fsops_create: dentry_path");
			goto out_dispose;
		}
	}

	/* Lookup the parent dentry. On success, parent->contents_lock will be held in write mode */
	ret = fs_path_lookup(path_norm, LOCK_DENTRY_CONTENTS_W, &parent, vol->index);
	if (ret < 0) {
		if (ret != -LTFS_NO_DENTRY && ret != -LTFS_NAMETOOLONG)
			ltfsmsg(LTFS_ERR, "11049E", ret);
		goto out_free;
	}

	/* Make sure target path doesn't exist */
	ret = fs_directory_lookup(parent, filename, &d);

	if (ret < 0) {
		if (ret != -LTFS_NAMETOOLONG)
			ltfsmsg(LTFS_ERR, "11049E", ret);
		goto out_dispose;
	} else if (d) {
		releasewrite_mrsw(&parent->contents_lock);
		if (dcache_initialized(NULL))
			dcache_close(d, true, false, vol);
		else
			fs_release_dentry(d);
		fs_release_dentry(parent);
		ret = -LTFS_DENTRY_EXISTS;
		goto out_free;
	}

	/* Allocate and set up the dentry */
	d = fs_allocate_dentry(NULL, NULL, filename, isdir, readonly, true, vol->index);
	if (! d) {
		ltfsmsg(LTFS_ERR, "11167E");
		ret = -LTFS_NO_MEMORY;
		goto out_dispose;
	}

	acquirewrite_mrsw(&parent->meta_lock);
	acquirewrite_mrsw(&d->meta_lock);

	// HPE MD 12.10.2017 If a file is being created here it is openforwrite until the file is closed and
	// as such needs to be flagged.  To complie with SNIA 2.4

	if (!isdir)
	{
		d->openforwrite = true;
	}

	/* Set times */
	get_current_timespec(&d->creation_time);
	d->modify_time = d->creation_time;
	d->access_time = d->creation_time;
	d->change_time = d->creation_time;
	d->backup_time = d->creation_time;
	parent->modify_time = d->creation_time;
	parent->change_time = d->creation_time;

	/* Decide whether to write to IP */
	if (! isdir && index_criteria_get_max_filesize(vol))
		d->matches_name_criteria = index_criteria_match(d, vol);

	/* Set up reference counters and pointers */
	d->vol = vol;
	d->parent = parent;
	++d->link_count;
	++d->numhandles;

	/* Block end */
	if (isdir)
		++parent->link_count;

	d->child_list=NULL;
	d->parent->child_list = fs_add_key_to_hash_table(d->parent->child_list, d, &ret);
	if (ret != 0) {
		ltfsmsg(LTFS_ERR, "11319E", "ltfs_fsops_create", ret);
		releasewrite_mrsw(&d->meta_lock);
		releasewrite_mrsw(&parent->meta_lock);
		goto out_dispose;
	}

	releasewrite_mrsw(&d->meta_lock);
	releasewrite_mrsw(&parent->meta_lock);

	ltfs_mutex_lock(&vol->index->dirty_lock);
	if (! isdir)
		++vol->index->file_count;
	ltfs_set_index_dirty(false, false, vol->index);
	ltfs_mutex_unlock(&vol->index->dirty_lock);

	*dentry = d;
	ret = 0;

out_dispose:
	releasewrite_mrsw(&parent->contents_lock);
	if (ret == 0 && dcache_initialized(NULL)) {
		ret = dcache_create(dentry_path, d, vol);
		if (ret < 0) {
			dcache_unlink(dentry_path, d, vol);
			fs_release_dentry(d);
			/* TODO: roll back counters */
		}
	}
	fs_release_dentry(parent);

out_free:
	releaseread_mrsw(&vol->lock);
	if (dentry_path)
		free(dentry_path);
	free(path_norm);
	return ret;
}

int ltfs_fsops_unlink(const char *path, ltfs_file_id *id, struct ltfs_volume *vol)
{
	int ret = 0;
	char *path_norm = NULL;
	struct dentry *d = NULL, *parent = NULL;
	struct name_list *namelist = NULL;

	CHECK_ARG_NULL(path, -LTFS_NULL_ARG);
	CHECK_ARG_NULL(vol, -LTFS_NULL_ARG);

	id->uid = 0;
	id->ino = 0;

	/* Make sure the device is online and writable */
	ret = ltfs_get_tape_readonly(vol);
	if (ret < 0 && ret != -LTFS_LESS_SPACE)
		return ret;
	ret = ltfs_test_unit_ready(vol);
	if (ret < 0) {
		ltfsmsg(LTFS_ERR, "11050E");
		return ret;
	}

	/* Validate and normalize the path */
	ret = pathname_format(path, &path_norm, true, true);
	if (ret == -LTFS_INVALID_PATH)
		return -LTFS_INVALID_SRC_PATH;
	else if (ret < 0) {
		ltfsmsg(LTFS_ERR, "11051E", ret);
		return ret;
	}

	/* Cannot remove the root dentry */
	if (path_norm[1] == '\0') {
		free(path_norm);
		return -LTFS_UNLINKROOT;
	}

	ret = ltfs_get_volume_lock(false, vol);
	if (ret < 0) {
		free(path_norm);
		return ret;
	}

	/* Find the dentry */
	ret = fs_path_lookup(path_norm, LOCK_PARENT_CONTENTS_W, &d, vol->index);
	if (ret < 0) {
		if (ret != -LTFS_NO_DENTRY && ret != -LTFS_NAMETOOLONG)
			ltfsmsg(LTFS_ERR, "11052E", ret);
		releaseread_mrsw(&vol->lock);
		free(path_norm);
		return ret;
	}
	parent = d->parent;

	/* Can't remove non-empty directories */
	if (d->isdir) {
		ret = 0;
		acquireread_mrsw(&d->contents_lock);
		if (HASH_COUNT(d->child_list) != 0)
			ret = -LTFS_DIRNOTEMPTY;
		releaseread_mrsw(&d->contents_lock);
		if (ret < 0)
			goto out;
	}

	acquirewrite_mrsw(&parent->meta_lock);
	acquirewrite_mrsw(&d->meta_lock);

	if (dcache_initialized(NULL)) {
		/*
		 * Actually parent and target metalock is not required when call dcache_unlink().
		 * But we intended to keep it for safety.
		 */
		ret = dcache_unlink(path_norm, d, vol);
		if (ret < 0) {
			releasewrite_mrsw(&d->meta_lock);
			goto out;
		}
	}

	get_current_timespec(&parent->modify_time);
	parent->change_time = parent->modify_time;

	namelist = fs_find_key_from_hash_table(parent->child_list, d->platform_safe_name, &ret);
	if (namelist) {
		HASH_DEL(parent->child_list, namelist);
		free(namelist->name);
		free(namelist);
	}
	else {
		ltfsmsg(LTFS_ERR, "11320E", "ltfs_fsops_unlink", ret);
		releasewrite_mrsw(&d->meta_lock);
		goto out;
	}
	id->uid = d->uid;
	id->ino = d->ino;
	d->deleted = true;
	d->parent = NULL;
	--d->link_count;
	if (d->isdir)
		--parent->link_count;
	--d->numhandles;
	releasewrite_mrsw(&d->meta_lock);

	ltfs_mutex_lock(&vol->index->dirty_lock);
	if (! d->isdir)
		--vol->index->file_count;
	ltfs_set_index_dirty(false, false, vol->index);
	ltfs_mutex_unlock(&vol->index->dirty_lock);

	ltfs_update_valid_block_count_unlocked(vol, -1 * (int64_t)d->used_blocks);

out:
	releasewrite_mrsw(&parent->contents_lock);
	fs_release_dentry_unlocked(parent); /* parent->meta_lock is released here */

	releaseread_mrsw(&vol->lock);

	if (ret == 0 && iosched_initialized(vol))
		iosched_update_data_placement(d, vol);

	free(path_norm);
	fs_release_dentry(d);
	return ret;
}

int ltfs_fsops_rename(const char *from, const char *to, ltfs_file_id *id, struct ltfs_volume *vol)
{
	int ret = 0;
	char *from_norm = NULL, *to_norm = NULL;
	char *from_norm_copy = NULL, *to_norm_copy = NULL;
	char *from_filename = NULL, *to_filename = NULL;
	char *to_filename_copy = NULL, *to_filename_copy2 = NULL;
	struct dentry *fromdir = NULL, *todir = NULL;
	struct dentry *fromdentry = NULL, *todentry = NULL;
	struct ltfs_timespec newtime;
	struct name_list *namelist = NULL;

	CHECK_ARG_NULL(from, -LTFS_NULL_ARG);
	CHECK_ARG_NULL(to, -LTFS_NULL_ARG);
	CHECK_ARG_NULL(vol, -LTFS_NULL_ARG);

	id->uid = 0;
	id->ino = 0;

	/* Make sure the device is online and writable */
	ret = ltfs_get_tape_readonly(vol);
	if (ret < 0 && ret != -LTFS_LESS_SPACE)
		return ret;
	ret = ltfs_test_unit_ready(vol);
	if (ret < 0) {
		ltfsmsg(LTFS_ERR, "11053E");
		return ret;
	}

	/* Validate and normalize the paths */
	ret = pathname_format(from, &from_norm, true, true);
	if (ret == -LTFS_INVALID_PATH)
		return -LTFS_INVALID_SRC_PATH;
	else if (ret < 0) {
		ltfsmsg(LTFS_ERR, "11054E", ret);
		return ret;
	}
	ret = pathname_format(to, &to_norm, true, true);
	if (ret < 0) {
		if (ret != -LTFS_INVALID_PATH)
			ltfsmsg(LTFS_ERR, "11055E", ret);
		goto out_free;
	}

	if (dcache_initialized(NULL)) {
		from_norm_copy = strdup(from_norm);
		to_norm_copy = strdup(to_norm);
		if (! from_norm_copy || ! to_norm_copy) {
			ltfsmsg(LTFS_ERR, "10001E", "ltfs_fsops_rename: file name copy");
			ret = -LTFS_NO_MEMORY;
			goto out_free;
		}
	}

	/* Split paths into directory and file name */
	fs_split_path(from_norm, &from_filename, strlen(from_norm) + 1);
	fs_split_path(to_norm, &to_filename, strlen(to_norm) + 1);

	/* Allocate memory for new file name */
	to_filename_copy = strdup(to_filename);
	to_filename_copy2 = strdup(to_filename);
	if (! to_filename_copy || ! to_filename_copy2) {
		ltfsmsg(LTFS_ERR, "10001E", "ltfs_fsops_rename: file name copy");
		ret = -LTFS_NO_MEMORY;
		goto out_free;
	}

	ret = ltfs_get_volume_lock(false, vol);
	if (ret < 0)
		goto out_free;
	ltfs_mutex_lock(&vol->index->rename_lock);

	if (dcache_initialized(NULL)) {
		/* Rename the cached files */
		ret = dcache_rename(from_norm_copy, to_norm_copy, &fromdentry, vol);
		if (ret == 0)
			ltfs_set_index_dirty(true, false, vol->index);
		goto out_release;
	}

	/* Look up directories and lock them */
	ret = fs_path_lookup(from_norm, 0, &fromdir, vol->index);
	if (ret < 0) {
		if (ret != -LTFS_NO_DENTRY && ret != -LTFS_NAMETOOLONG)
			ltfsmsg(LTFS_ERR, "11056E", ret);
		goto out_release;
	}

	ret = fs_path_lookup(to_norm, 0, &todir, vol->index);
	if (ret < 0) {
		if (ret != -LTFS_NO_DENTRY && ret != -LTFS_NAMETOOLONG)
			ltfsmsg(LTFS_ERR, "11057E", ret);
		/* fromdir meta_lock is needed because the exit code calls fs_release_dentry_unlocked */
		acquirewrite_mrsw(&fromdir->meta_lock);
		goto out_release;
	}

	/* Take locks in the appropriate order and look up the source and destination dentries */
	if (todir == fromdir || fs_is_predecessor(todir, fromdir)) {
		acquirewrite_mrsw(&todir->contents_lock);
		acquirewrite_mrsw(&todir->meta_lock);

		ret = fs_directory_lookup(todir, to_filename, &todentry);
		if (fromdir != todir) {
			acquirewrite_mrsw(&fromdir->contents_lock);
			acquirewrite_mrsw(&fromdir->meta_lock);
		}
		if (ret < 0) {
			if (ret != -LTFS_NAMETOOLONG)
				ltfsmsg(LTFS_ERR, "11057E", ret);
			goto out_unlock;
		}

		ret = fs_directory_lookup(fromdir, from_filename, &fromdentry);
		if (ret < 0 || ! fromdentry) {
			if (ret < 0 && ret != -LTFS_NAMETOOLONG)
				ltfsmsg(LTFS_ERR, "11056E", ret);
			if (! fromdentry)
				ret = -LTFS_NO_DENTRY;
			if (todentry) {
				if (todentry == fromdir)
					--todentry->numhandles;
				else
					fs_release_dentry(todentry);
			}
			goto out_unlock;
		}
	} else {
		acquirewrite_mrsw(&fromdir->contents_lock);
		acquirewrite_mrsw(&fromdir->meta_lock);

		ret = fs_directory_lookup(fromdir, from_filename, &fromdentry);
		acquirewrite_mrsw(&todir->contents_lock);
		acquirewrite_mrsw(&todir->meta_lock);
		if (ret < 0) {
			if (ret != -LTFS_NAMETOOLONG)
				ltfsmsg(LTFS_ERR, "11056E", ret);
			goto out_unlock;
		} else if (! fromdentry) {
			ret = -LTFS_NO_DENTRY;
			goto out_unlock;
		}

		ret = fs_directory_lookup(todir, to_filename, &todentry);
		if (ret < 0) {
			if (ret != -LTFS_NAMETOOLONG)
				ltfsmsg(LTFS_ERR, "11057E", ret);
			if (fromdentry) { /* BEAM: constant condition - fromdentry has always non-zero value here. */
				if (fromdentry == todir)
					--fromdentry->numhandles;
				else
					fs_release_dentry(fromdentry);
			}
			goto out_unlock;
		}
	}

	/* Validate some extra failure cases. Normally FUSE will not ask a file system to rename
	 * a dentry into a subdirectory of itself, or any dentry to one of its predecessors.
	 * However, this can happen if the names used for the source and destination paths
	 * are considered different by FUSE, but the same by LTFS. That's the case for names
	 * that are "similar" except for Unicode normalization. */
	ret = 0;
	if (fromdentry == todir || fs_is_predecessor(fromdentry, todir))
		ret = -LTFS_RENAMELOOP;
	else if (todentry && (todentry == fromdir || fs_is_predecessor(todentry, fromdir))) {
		ret = fromdentry->isdir ? -LTFS_DIRNOTEMPTY : -LTFS_ISFILE;
		if (fromdentry->isdir)
			ret = -LTFS_DIRNOTEMPTY;
	}
	if (ret < 0) {
		/* Need to release fromdentry and todentry. This is slightly tricky because if we get
		 * here, it's possible that one of those is equal to one of the directories. In that
		 * case, we need to ensure that the directory reference and locks are released correctly
		 * later on, so just decrement the handle count instead of calling fs_release_dentry(). */
		if (fromdentry != todir)
			fs_release_dentry(fromdentry);
		else
			--fromdentry->numhandles;
		if (todentry) {
			if (todentry != fromdir)
				fs_release_dentry(todentry);
			else
				--todentry->numhandles;
		}
		goto out_unlock;
	}

#ifdef __APPLE__
	/*
	 * Directory move is inhibited because of a MacFUSE bug.
	 * MacFUSE requests unexpected path after directory move, and that problem
	 * causes an unexpected move.
	 */
	if (fromdentry->isdir && fromdir != todir) {
		ltfsmsg(LTFS_INFO, "11259I");
		ret = -LTFS_DIRMOVE;
		if (todentry && fromdentry != todentry)
			fs_release_dentry(todentry);
		fs_release_dentry(fromdentry);
		goto out_unlock;
	}
#endif

	/* If the destination dentry was found and is distinct from the source dentry, try
	 * to unlink it before going forward with the rename. */
	if (todentry && todentry != fromdentry) {
		if (todentry->isdir) {
			ret = 0;
			acquireread_mrsw(&todentry->contents_lock);
			if (HASH_COUNT(todentry->child_list) != 0)
				ret = -LTFS_DIRNOTEMPTY;
			releaseread_mrsw(&todentry->contents_lock);
			if (ret < 0) {
				fs_release_dentry(fromdentry);
				fs_release_dentry(todentry);
				goto out_unlock;
			}
		}
		acquirewrite_mrsw(&todentry->meta_lock);
		if (todentry->isdir)
			--todir->link_count;
		--todentry->numhandles;
		--todentry->link_count;
		todentry->parent = NULL;
		todentry->deleted = true;

		namelist = fs_find_key_from_hash_table(todir->child_list, todentry->platform_safe_name, &ret);
		if (namelist) {
			HASH_DEL(todir->child_list, namelist);
			free(namelist->name);
			free(namelist);
		}
		else {
			ltfsmsg(LTFS_ERR, "11320E", "ltfs_fsops_rename", ret);
			releasewrite_mrsw(&todentry->meta_lock);
			goto out_unlock;
		}
		if (! todir->isdir)
			fs_decrement_file_count(vol->index);
		fs_release_dentry_unlocked(todentry);
		todentry = NULL;
	} else if (todentry) {
		/* Don't return immediately if todentry == fromdentry, as the name could still change
		 * if the new name is not identical to the old (i.e. different case on case-insensitive
		 * platforms). */
		fs_release_dentry(todentry);
	}

	/* Remove fromdentry from old directory */
	acquirewrite_mrsw(&fromdentry->meta_lock);
	namelist = fs_find_key_from_hash_table(fromdir->child_list, fromdentry->platform_safe_name, &ret);
	if (namelist) {
		HASH_DEL(fromdir->child_list, namelist);
		free(namelist->name);
		free(namelist);
	}
	else {
		ltfsmsg(LTFS_ERR, "11320E", "ltfs_fsops_rename", ret);
		releasewrite_mrsw(&fromdentry->meta_lock);
		goto out_unlock;
	}

	if (fromdentry->isdir)
		--fromdir->link_count;

	if (fromdentry->isdir)
		++todir->link_count;

	/* Update times */
	get_current_timespec(&newtime);
	fromdir->modify_time = newtime;
	fromdir->change_time = newtime;
	todir->modify_time = newtime;
	todir->change_time = newtime;
	fromdentry->change_time = newtime;

	/* Update fromdentry */
	fromdentry->parent = todir;
	if (fromdentry->name)
		free(fromdentry->name);
	if (fromdentry->platform_safe_name)
		free(fromdentry->platform_safe_name);
	fromdentry->name = to_filename_copy;
	fromdentry->platform_safe_name = to_filename_copy2;
	fromdentry->matches_name_criteria = index_criteria_match(fromdentry, vol);

	/* Add fromdentry to new directory */
	todir->child_list = fs_add_key_to_hash_table(todir->child_list, fromdentry, &ret);
	if (ret != 0) {
		ltfsmsg(LTFS_ERR, "11319E", "ltfs_fsops_rename", ret);
		releasewrite_mrsw(&fromdentry->meta_lock);
		goto out_unlock;
	}

	if (! iosched_initialized(vol))
		fs_release_dentry_unlocked(fromdentry);
	else
		releasewrite_mrsw(&fromdentry->meta_lock);

	ltfs_set_index_dirty(true, false, vol->index);

	ret = 0;

out_unlock:
	/* Release contents locks. The meta_locks are released by fs_release_dentry_unlocked. */
	releasewrite_mrsw(&fromdir->contents_lock);
	if (fromdir != todir)
		releasewrite_mrsw(&todir->contents_lock);

out_release:
	if (! dcache_initialized(NULL)) {
		if (fromdir)
			fs_release_dentry_unlocked(fromdir);
		if (todir) {
			if (todir == fromdir)
				fs_release_dentry(todir);
			else
				fs_release_dentry_unlocked(todir);
		}
	}
	ltfs_mutex_unlock(&vol->index->rename_lock);
	releaseread_mrsw(&vol->lock);

	if (fromdentry) {
		id->uid = fromdentry->uid;
		id->ino = fromdentry->ino;
	}

	/* Tell the scheduler about fromdentry's new name, ignoring errors (because the
	 * rename already finished, no going back now) */
	if (ret == 0 && iosched_initialized(vol) && fromdentry) {
		iosched_update_data_placement(fromdentry, vol);
		fs_release_dentry(fromdentry);
	}

out_free:
	if (from_norm)
		free(from_norm);
	if (to_norm)
		free(to_norm);
	if (from_norm_copy)
		free(from_norm_copy);
	if (to_norm_copy)
		free(to_norm_copy);
	if (ret < 0 || dcache_initialized(NULL)) {
		if (to_filename_copy)
			free(to_filename_copy);
		if (to_filename_copy2)
			free(to_filename_copy2);
	}

	return ret;
}

int ltfs_fsops_getattr(struct dentry *d, struct dentry_attr *attr, struct ltfs_volume *vol)
{
	int ret;

	CHECK_ARG_NULL(d, -LTFS_NULL_ARG);
	CHECK_ARG_NULL(attr, -LTFS_NULL_ARG);
	CHECK_ARG_NULL(vol, -LTFS_NULL_ARG);

	ret = ltfs_get_volume_lock(false, vol);
	if (ret < 0)
		return ret;
	acquireread_mrsw(&d->meta_lock);

	if(d->isslink)
		attr->size = strlen(d->target);
	else
		attr->size = d->size;

	attr->alloc_size = d->realsize;
	attr->blocksize = vol->label->blocksize;
	attr->uid = d->uid;
	attr->nlink = d->link_count;
	attr->create_time = d->creation_time;
	attr->access_time = d->access_time;
	attr->modify_time = d->modify_time;
	attr->change_time = d->change_time;
	attr->backup_time = d->backup_time;
	attr->readonly = d->readonly;
	attr->isdir = d->isdir;
	attr->isslink = d->isslink;

	releaseread_mrsw(&d->meta_lock);
	releaseread_mrsw(&vol->lock);

	if (! d->isdir && !d->isslink && iosched_initialized(vol))
		attr->size = iosched_get_filesize(d, vol);

	return 0;
}

int ltfs_fsops_getattr_path(const char *path, struct dentry_attr *attr, ltfs_file_id *id, struct ltfs_volume *vol)
{
	int ret;
	struct dentry *d;

	id->uid = 0;
	id->ino = 0;

	CHECK_ARG_NULL(path, -LTFS_NULL_ARG);
	CHECK_ARG_NULL(vol, -LTFS_NULL_ARG);

	ret = ltfs_fsops_open(path, false, false, &d, vol);
	if (ret < 0)
		return ret;

	ret = ltfs_fsops_getattr(d, attr, vol);
	id->uid = d->uid;
	id->ino = d->ino;
	ltfs_fsops_close(d, false, false, false, vol);

	return ret;
}

int ltfs_fsops_setxattr(const char *path, const char *name, const char *value, size_t size,
						int flags, ltfs_file_id *id, struct ltfs_volume *vol)
{
	int ret;
	struct dentry *d;
	char *new_path = NULL, *new_name = NULL;
	const char *new_name_strip;
	bool write_lock;
	int ret_restore;
	char value_restore[LTFS_MAX_XATTR_SIZE];
	bool ignore = false;

	id->uid = 0;
	id->ino = 0;

	CHECK_ARG_NULL(path, -LTFS_NULL_ARG);
	CHECK_ARG_NULL(name, -LTFS_NULL_ARG);
	CHECK_ARG_NULL(value, -LTFS_NULL_ARG);
	CHECK_ARG_NULL(vol, -LTFS_NULL_ARG);
	if (size > LTFS_MAX_XATTR_SIZE)
		return -LTFS_LARGE_XATTR; /* this is the error returned by ext3 when the xattr is too large */

	/* Check if the extended attribute writing is allowed */
	if (! strncmp(name, "ltfs.volumeLockState", strlen("ltfs.volumeLockState"))
			|| ! strncmp(name, "user.ltfs.volumeLockState", strlen("user.ltfs.volumeLockState"))) {
		ret = tape_read_only(vol->device, ltfs_part_id2num(ltfs_ip_id(vol), vol));
		if ((ret == 0) || (ret == -LTFS_LESS_SPACE) || (ret == -LTFS_NO_SPACE)) {
			ret = tape_read_only(vol->device, ltfs_part_id2num(ltfs_dp_id(vol), vol));
			if ((ret == 0) || (ret == -LTFS_LESS_SPACE) || (ret == -LTFS_NO_SPACE)) {
				ignore = true;
			}
		}
	} else {
		ret = ltfs_get_tape_readonly(vol);
	}

	if ((ret < 0) && (ignore == false))
		return ret;

	ret = ltfs_test_unit_ready(vol);
	if (ret < 0) {
		/* Cannot set extended attribute: device is not ready */
		ltfsmsg(LTFS_ERR, "11117E");
		return ret;
	}

	/* Format and validate the path and xattr name. */
	ret = pathname_format(path, &new_path, true, true);
	if (ret == -LTFS_INVALID_PATH)
		return -LTFS_INVALID_SRC_PATH;
	else if (ret == -LTFS_NAMETOOLONG)
		return ret;
	else if (ret < 0) {
		ltfsmsg(LTFS_ERR, "11118E", ret);
		return ret;
	}
	// HPE MD 22.09.2017 / is now allow in xattrs
	ret = pathname_format(name, &new_name, true, true);
	if (ret < 0) {
		if (ret != -LTFS_INVALID_PATH && ret != -LTFS_NAMETOOLONG)
			ltfsmsg(LTFS_ERR, "11119E", ret);
		goto out_free;
	}

	new_name_strip = _xattr_strip_name(new_name);
	if (! new_name_strip) {
		/* Namespace is not supported (Linux) */
		ret = -LTFS_XATTR_NAMESPACE;
		goto out_free;
	}
	// HPE MD 22.09.2017 function was changed for SNIA 2.4 extra param 0 
	// will cause function to perform as before.
	ret = pathname_validate_xattr_name(new_name_strip, 0);
	if (ret < 0) {
		if (ret != -LTFS_INVALID_PATH && ret != -LTFS_NAMETOOLONG) /* normal errors */
			ltfsmsg(LTFS_ERR, "11120E", ret);
		goto out_free;
	}

	/* Special case: if we are syncing the volume, flush the scheduler buffers
	 * before taking locks. */
start:
	if (! strcmp(new_name_strip, "ltfs.sync") && ! strcmp(path, "/")) {
		ret = ltfs_fsops_flush(NULL, false, vol);
		if (ret < 0) {
			ltfsmsg(LTFS_ERR, "11325E", ret);
			goto out_free;
		}
		ret = ltfs_get_volume_lock(true, vol);
		write_lock = true;
	} else {
		ret = ltfs_get_volume_lock(false, vol);
		write_lock = false;
	}
	if (ret < 0)
		goto out_free;

	if (dcache_initialized(NULL))
		ret = dcache_open(new_path, &d, vol);
	else
		ret = fs_path_lookup(new_path, 0, &d, vol->index);
	if (ret < 0) {
		if (ret != -LTFS_NO_DENTRY && ret != -LTFS_NAMETOOLONG) /* normal errors */
			ltfsmsg(LTFS_ERR, "11121E", ret);
		release_mrsw(&vol->lock);
		goto out_free;
	}

	id->uid = d->uid;
	id->ino = d->ino;

	/* Save original value */
	ret_restore = xattr_get(d, new_name_strip, value_restore, sizeof(value_restore), vol);

	ret = xattr_set(d, new_name_strip, value, size, flags, vol);
	if (dcache_initialized(NULL)) {
		if (ret == 0) {
			ret = dcache_setxattr(new_path, d, new_name_strip, value, size, flags, vol);
			if (ret < 0) {
				if (ret_restore >= 0)
					xattr_set(d, new_name_strip, value_restore, ret_restore, XATTR_REPLACE, vol);
				else
					xattr_remove(d, new_name_strip, vol);
			}
		}
		dcache_close(d, true, true, vol);
	} else
		fs_release_dentry(d);

	if (NEED_REVAL(ret)) {
		ret = ltfs_revalidate(write_lock, vol);
		if (ret == 0)
			goto start;
	} else if (IS_UNEXPECTED_MOVE(ret)) {
		vol->reval = -LTFS_REVAL_FAILED;
		release_mrsw(&vol->lock);
	} else
		release_mrsw(&vol->lock);
out_free:
	if (new_name)
		free(new_name);
	if (new_path)
		free(new_path);
	return ret;
}

int ltfs_fsops_getxattr(const char *path, const char *name, char *value, size_t size,
						ltfs_file_id *id, struct ltfs_volume *vol)
{
	int ret;
	struct dentry *d;
	char *new_path = NULL, *new_name = NULL;
	const char *new_name_strip;

	id->uid = 0;
	id->ino = 0;

	CHECK_ARG_NULL(path, -LTFS_NULL_ARG);
	CHECK_ARG_NULL(name, -LTFS_NULL_ARG);
	CHECK_ARG_NULL(vol, -LTFS_NULL_ARG);
	if (size > 0 && ! value) {
		ltfsmsg(LTFS_ERR, "11123E");
		return -LTFS_BAD_ARG;
	}

	/* Format and validate the path and xattr name. */
	ret = pathname_format(path, &new_path, true, true);
	if (ret == -LTFS_INVALID_PATH)
		return -LTFS_INVALID_SRC_PATH;
	else if (ret == -LTFS_NAMETOOLONG)
		return ret;
	else if (ret < 0) {
		ltfsmsg(LTFS_ERR, "11124E", ret);
		return ret;
	}
	// HPE MD 22.09.2017 / is now allow in xattrs
	ret = pathname_format(name, &new_name, true, true);
	if (ret < 0) {
		if (ret != -LTFS_INVALID_PATH && ret != -LTFS_NAMETOOLONG)
			ltfsmsg(LTFS_ERR, "11125E", ret);
		goto out_free;
	}
	new_name_strip = _xattr_strip_name(new_name);
	if (! new_name_strip) {
		/* Namespace is not supported (Linux) */
		ret = -LTFS_NO_XATTR;
		goto out_free;
	}
	// HPE MD 22.09.2017 function was changed for SNIA 2.4 extra param 0 
	// will cause function to perform as before.
	ret = pathname_validate_xattr_name(new_name_strip, 0);
	if (ret < 0) {
		if (ret != -LTFS_INVALID_PATH && ret != -LTFS_NAMETOOLONG) /* normal errors */
			ltfsmsg(LTFS_ERR, "11126E", ret);
		goto out_free;
	}

	/* Grab locks and find the dentry. */
start:
	ret = ltfs_get_volume_lock(false, vol);
	if (ret < 0)
		goto out_free;

	if (dcache_initialized(NULL))
		ret = dcache_open(new_path, &d, vol);
	else
		ret = fs_path_lookup(new_path, 0, &d, vol->index);
	if (ret < 0) {
		if (ret != -LTFS_NO_DENTRY && ret != -LTFS_NAMETOOLONG) /* normal errors */
			ltfsmsg(LTFS_ERR, "11127E", ret);
		releaseread_mrsw(&vol->lock);
		goto out_free;
	}

	id->uid = d->uid;
	id->ino = d->ino;

	if (dcache_initialized(NULL)) {
		ret = dcache_getxattr(new_path, d, new_name_strip, value, size, vol);
		dcache_close(d, true, true, vol);
	} else {
		ret = xattr_get(d, new_name_strip, value, size, vol);
		fs_release_dentry(d);
	}
	if (ret == -LTFS_RESTART_OPERATION)
		goto start;

	releaseread_mrsw(&vol->lock);
out_free:
	if (new_path)
		free(new_path);
	if (new_name)
		free(new_name);
	return ret;
}

int ltfs_fsops_listxattr(const char *path, char *list, size_t size, ltfs_file_id *id, struct ltfs_volume *vol)
{
	struct dentry *d;
	char *new_path;
	int ret;

	id->uid = 0;
	id->ino = 0;

	CHECK_ARG_NULL(path, -LTFS_NULL_ARG);
	CHECK_ARG_NULL(vol, -LTFS_NULL_ARG);
	if (size > 0 && ! list) {
		ltfsmsg(LTFS_ERR, "11130E");
		return -LTFS_BAD_ARG;
	}

	ret = pathname_format(path, &new_path, true, true);
	if (ret == -LTFS_INVALID_PATH)
		return -LTFS_INVALID_SRC_PATH;
	else if (ret == -LTFS_NAMETOOLONG)
		return ret;
	else if (ret < 0) {
		ltfsmsg(LTFS_ERR, "11131E", ret);
		return ret;
	}

	ret = ltfs_get_volume_lock(false, vol);
	if (ret < 0) {
		free(new_path);
		return ret;
	}

	if (dcache_initialized(NULL))
		ret = dcache_open(new_path, &d, vol);
	else
		ret = fs_path_lookup(new_path, 0, &d, vol->index);
	if (ret < 0) {
		if (ret != -LTFS_NO_DENTRY && ret != -LTFS_NAMETOOLONG) /* normal errors */
			ltfsmsg(LTFS_ERR, "11132E", ret);
		free(new_path);
		releaseread_mrsw(&vol->lock);
		return ret;
	}

	id->uid = d->uid;
	id->ino = d->ino;

	if (dcache_initialized(NULL)) {
		ret = dcache_listxattr(new_path, d, list, size, vol);
		dcache_close(d, true, true, vol);
	} else {
		ret = xattr_list(d, list, size, vol);
		fs_release_dentry(d);
	}

	free(new_path);
	releaseread_mrsw(&vol->lock);
	return ret;
}

int ltfs_fsops_removexattr(const char *path, const char *name, ltfs_file_id *id, struct ltfs_volume *vol)
{
	int ret;
	struct dentry *d;
	char *new_path = NULL, *new_name = NULL;
	const char *new_name_strip;

	id->uid = 0;
	id->ino = 0;

	CHECK_ARG_NULL(path, -LTFS_NULL_ARG);
	CHECK_ARG_NULL(name, -LTFS_NULL_ARG);
	CHECK_ARG_NULL(vol, -LTFS_NULL_ARG);

	ret = ltfs_get_tape_readonly(vol);
	if (ret < 0 && ret != -LTFS_LESS_SPACE)
		return ret;
	ret = ltfs_test_unit_ready(vol);
	if (ret < 0) {
		/* Cannot remove extended attribute: device is not ready */
		ltfsmsg(LTFS_ERR, "11135E");
		return ret;
	}

	/* Format and validate the path and xattr name. */
	ret = pathname_format(path, &new_path, true, true);
	if (ret == -LTFS_INVALID_PATH)
		return -LTFS_INVALID_SRC_PATH;
	else if (ret == -LTFS_NAMETOOLONG)
		return ret;
	else if (ret < 0) {
		ltfsmsg(LTFS_ERR, "11136E", ret);
		return ret;
	}
	// HPE MD 22.09.2017 / is now allow in xattrs
	ret = pathname_format(name, &new_name, true, true);
	if (ret < 0) {
		if (ret != -LTFS_INVALID_PATH && ret != -LTFS_NAMETOOLONG)
			ltfsmsg(LTFS_ERR, "11137E", ret);
		goto out_free;
	}
	new_name_strip = _xattr_strip_name(new_name);
	if (! new_name_strip) {
		/* Namespace is not supported (Linux) */
		ret = -LTFS_NO_XATTR;
		goto out_free;
	}
	// HPE MD 22.09.2017 function was changed for SNIA 2.4 extra param 0 
	// will cause function to perform as before.
	ret = pathname_validate_xattr_name(new_name_strip, 0);
	if (ret < 0) {
		if (ret != -LTFS_INVALID_PATH && ret != -LTFS_NAMETOOLONG) /* normal errors */
			ltfsmsg(LTFS_ERR, "11138E", ret);
		goto out_free;
	}

	/* Grab locks and find the dentry. */
	ret = ltfs_get_volume_lock(false, vol);
	if (ret < 0)
		goto out_free;

	if (dcache_initialized(NULL))
		ret = dcache_open(new_path, &d, vol);
	else
		ret = fs_path_lookup(new_path, 0, &d, vol->index);
	if (ret < 0) {
		if (ret != -LTFS_NO_DENTRY && ret != -LTFS_NAMETOOLONG)
			ltfsmsg(LTFS_ERR, "11139E", ret);
		releaseread_mrsw(&vol->lock);
		goto out_free;
	}

	id->uid = d->uid;
	id->ino = d->ino;

	ret = xattr_remove(d, new_name_strip, vol);
	if (dcache_initialized(NULL)) {
		if (ret == 0)
			ret = dcache_removexattr(new_path, d, new_name_strip, vol);
		dcache_close(d, true, true, vol);
	} else
		fs_release_dentry(d);
	releaseread_mrsw(&vol->lock);

out_free:
	if (new_path)
		free(new_path);
	if (new_name)
		free(new_name);
	return ret;
}

int ltfs_fsops_readdir(struct dentry *d, void *buf, ltfs_dir_filler filler, void *filler_priv,
	struct ltfs_volume *vol)
{
	int ret = 0;
	struct name_list *entry, *tmp;

	CHECK_ARG_NULL(d, -LTFS_NULL_ARG);
	CHECK_ARG_NULL(filler, -LTFS_NULL_ARG);
	CHECK_ARG_NULL(vol, -LTFS_NULL_ARG);

	if (! d->isdir)
		return -LTFS_ISFILE;

	ret = ltfs_get_volume_lock(false, vol);
	if (ret < 0)
		return ret;

	acquireread_mrsw(&d->contents_lock);
	if (dcache_initialized(NULL)) {
		int i;
		char **namelist = NULL;
		ret = dcache_readdir(d, false, (void ***) &namelist, vol);
		if (ret == 0 && namelist) {
			for (i=0; namelist[i]; ++i) {
				ret = filler(buf, namelist[i], filler_priv);
				if (ret < 0)
					break;
			}
			for (i=0; namelist[i]; ++i)
				free(namelist[i]);
			free(namelist);
		}
	} else {
		if (HASH_COUNT(d->child_list) != 0) {
			HASH_SORT(d->child_list, fs_hash_sort_by_uid);
			HASH_ITER(hh, d->child_list, entry, tmp) {
				ret = filler(buf, entry->d->platform_safe_name, filler_priv);
				if (ret < 0)
					break;
			}
		}
	}
	releaseread_mrsw(&d->contents_lock);

	/* Update access time */
	if (ret == 0) {
		acquirewrite_mrsw(&d->meta_lock);
		get_current_timespec(&d->access_time);
		releasewrite_mrsw(&d->meta_lock);
		ltfs_set_index_dirty(true, true, vol->index);
	}

	releaseread_mrsw(&vol->lock);
	return ret;
}

int _ltfs_fsops_read_direntry(struct dentry *d, struct ltfs_direntry *dirent,
							  unsigned long index, bool root, struct ltfs_volume *vol)
{
	unsigned long i = 0;
	struct dentry *target = NULL;
	struct name_list *entry, *tmp;

	CHECK_ARG_NULL(d, -LTFS_NULL_ARG);
	CHECK_ARG_NULL(dirent, -LTFS_NULL_ARG);

	acquireread_mrsw(&d->contents_lock);

	if ( ! d->isdir ) {
		releaseread_mrsw(&d->contents_lock);
		return -LTFS_ISFILE;
	}

	dirent->name = NULL;
	dirent->platform_safe_name = NULL;

	/* Handle the current directory and the parent directory */
	if( (! root) || d->parent ) {
		switch (index) {
		case 0: /* Return current dir */
			dirent->name = ".";
			dirent->platform_safe_name = ".";
			target = d;
			i = index;
			break;
		case 1:
			dirent->name = "..";
			dirent->platform_safe_name = "..";
			target = d->parent;
			i = index;
			break;
		default:
			i = 2;
			break;
		}
	}

	if (dcache_initialized(NULL)) {
		int ret = 0;

		releaseread_mrsw(&d->contents_lock);
		if (target) {
			acquireread_mrsw(&target->meta_lock);
			dirent->creation_time = target->creation_time;
			dirent->access_time   = target->access_time;
			dirent->modify_time   = target->modify_time;
			dirent->change_time   = target->change_time;
			dirent->isdir         = target->isdir;
			dirent->readonly      = target->readonly;
			dirent->isslink       = target->isslink;
			dirent->realsize      = target->realsize;
			dirent->size          = target->size;
			if (! dirent->platform_safe_name ) {
				dirent->name          = target->name;
				dirent->platform_safe_name = target->platform_safe_name;
			}
			releaseread_mrsw(&target->meta_lock);
		}
		else {
			ret = dcache_read_direntry(d, dirent, index, vol);
		}
		return ret;
	}
	else {
		/* Search target dentry from directory entry */
		if (! target) {
			if(HASH_COUNT(d->child_list) != 0) {
				HASH_ITER(hh, d->child_list, entry, tmp) {
					if(entry->d->deleted) continue;
					if(!entry->d->platform_safe_name) continue;
					if(i == index) {
						target = entry->d;
						break;
					}
					i++;
				}
			}
		}
		releaseread_mrsw(&d->contents_lock);

		/* Cannot find the target dentry*/
		if(i != index || ! target )
			return -LTFS_NO_DENTRY;

		/* Set target dentry information to the buffer */
		acquireread_mrsw(&target->meta_lock);
		dirent->creation_time = target->creation_time;
		dirent->access_time   = target->access_time;
		dirent->modify_time   = target->modify_time;
		dirent->change_time   = target->change_time;
		dirent->isdir         = target->isdir;
		dirent->readonly      = target->readonly;
		dirent->isslink       = target->isslink;
		dirent->realsize      = target->realsize;
		dirent->size          = target->size;
		if (! dirent->platform_safe_name ) {
			dirent->name          = target->name;
			dirent->platform_safe_name = target->platform_safe_name;
		}
		releaseread_mrsw(&target->meta_lock);
	}
	return 0;
}

int ltfs_fsops_read_direntry(struct dentry *d, struct ltfs_direntry *dirent, unsigned long index,
							 struct ltfs_volume *vol)
{
	return _ltfs_fsops_read_direntry(d, dirent, index, true, vol);
}

int ltfs_fsops_read_direntry_noroot(struct dentry *d, struct ltfs_direntry *dirent,
									unsigned long index, struct ltfs_volume *vol)
{
	return _ltfs_fsops_read_direntry(d, dirent, index, false, vol);
}

int ltfs_fsops_utimens(struct dentry *d, const struct ltfs_timespec ts[2], struct ltfs_volume *vol)
{
	int ret;

	CHECK_ARG_NULL(d, -LTFS_NULL_ARG);
	CHECK_ARG_NULL(ts, -LTFS_NULL_ARG);
	CHECK_ARG_NULL(vol, -LTFS_NULL_ARG);

	/* Make sure the device is online and writable */
	ret = ltfs_get_tape_readonly(vol);
	if (ret < 0 && ret != -LTFS_LESS_SPACE)
		return ret;
	ret = ltfs_test_unit_ready(vol);
	if (ret < 0) {
		ltfsmsg(LTFS_ERR, "11045E");
		return ret;
	}

	ret = ltfs_get_volume_lock(false, vol);
	if (ret < 0)
		return ret;
	acquirewrite_mrsw(&d->meta_lock);

	if (d->access_time.tv_sec != ts[0].tv_sec || d->access_time.tv_nsec != ts[0].tv_nsec) {
		d->access_time = ts[0];
		ret = normalize_ltfs_time(&d->access_time);
		if (ret == LTFS_TIME_OUT_OF_RANGE)
			ltfsmsg(LTFS_WARN, "17217W", "atime",
					d->platform_safe_name, d->uid, ts[0].tv_sec);
		get_current_timespec(&d->change_time);
		ltfs_set_index_dirty(true, true, vol->index);
	}
	if (d->modify_time.tv_sec != ts[1].tv_sec || d->modify_time.tv_nsec != ts[1].tv_nsec) {
		d->modify_time = ts[1];
		ret = normalize_ltfs_time(&d->modify_time);
		if (ret == LTFS_TIME_OUT_OF_RANGE)
			ltfsmsg(LTFS_WARN, "17217W", "mtime",
					d->platform_safe_name, d->uid, ts[1].tv_sec);
		get_current_timespec(&d->change_time);
		ltfs_set_index_dirty(true, false, vol->index);
	}
	if (dcache_initialized(NULL))
		dcache_flush(d, FLUSH_METADATA, vol);

	releasewrite_mrsw(&d->meta_lock);
	releaseread_mrsw(&vol->lock);

	return 0;
}

int ltfs_fsops_utimens_path(const char *path, const struct ltfs_timespec ts[2], ltfs_file_id *id, struct ltfs_volume *vol)
{
	int ret;
	struct dentry *d;

	id->uid = 0;
	id->ino = 0;

	CHECK_ARG_NULL(path, -LTFS_NULL_ARG);
	CHECK_ARG_NULL(vol, -LTFS_NULL_ARG);

	ret = ltfs_fsops_open(path, false, false, &d, vol);
	if (ret < 0)
		return ret;

	ret = ltfs_fsops_utimens(d, ts, vol);
	id->uid = d->uid;
	id->ino = d->ino;
	ltfs_fsops_close(d, false, false, false, vol);

	return ret;
}

int ltfs_fsops_utimens_all(struct dentry *d, const struct ltfs_timespec ts[4], struct ltfs_volume *vol)
{
	int ret;
    bool isctime=false;

	CHECK_ARG_NULL(d, -LTFS_NULL_ARG);
	CHECK_ARG_NULL(ts, -LTFS_NULL_ARG);
	CHECK_ARG_NULL(vol, -LTFS_NULL_ARG);

	/* Make sure the device is online and writable */
	ret = ltfs_get_tape_readonly(vol);
	if (ret < 0 && ret != -LTFS_LESS_SPACE)
		return ret;
	ret = ltfs_test_unit_ready(vol);
	if (ret < 0) {
		ltfsmsg(LTFS_ERR, "11045E");
		return ret;
	}

	ret = ltfs_get_volume_lock(false, vol);
	if (ret < 0)
		return ret;

	acquirewrite_mrsw(&d->meta_lock);

	if (ts[3].tv_sec != 0 || ts[3].tv_nsec != 0) {
		d->change_time = ts[3];
		ret = normalize_ltfs_time(&d->change_time);
		if (ret == LTFS_TIME_OUT_OF_RANGE)
			ltfsmsg(LTFS_WARN, "17217W", "ctime",
					d->platform_safe_name, d->uid, ts[3].tv_sec);
		isctime=true;
		ltfs_set_index_dirty(true, false, vol->index);
	}
	if (ts[0].tv_sec != 0 || ts[0].tv_nsec != 0) {
		d->access_time = ts[0];
		ret = normalize_ltfs_time(&d->access_time);
		if (ret == LTFS_TIME_OUT_OF_RANGE)
			ltfsmsg(LTFS_WARN, "17217W", "atime",
					d->platform_safe_name, d->uid, ts[0].tv_sec);
		if(!isctime) get_current_timespec(&d->change_time);
		ltfs_set_index_dirty(true, true, vol->index);
	}
	if (ts[1].tv_sec != 0 || ts[1].tv_nsec != 0) {
		d->modify_time = ts[1];
		ret = normalize_ltfs_time(&d->modify_time);
		if (ret == LTFS_TIME_OUT_OF_RANGE)
			ltfsmsg(LTFS_WARN, "17217W", "mtime",
					d->platform_safe_name, d->uid, ts[1].tv_sec);
		if(!isctime) get_current_timespec(&d->change_time);
		ltfs_set_index_dirty(true, false, vol->index);
	}
	if (ts[2].tv_sec != 0 || ts[2].tv_nsec != 0) {
		d->creation_time = ts[2];
		ret = normalize_ltfs_time(&d->creation_time);
		if (ret == LTFS_TIME_OUT_OF_RANGE)
			ltfsmsg(LTFS_WARN, "17217W", "creation_time",
					d->platform_safe_name, d->uid, ts[2].tv_sec);
		if(!isctime) get_current_timespec(&d->change_time);
		ltfs_set_index_dirty(true, false, vol->index);
	}

	if (dcache_initialized(NULL))
		dcache_flush(d, FLUSH_METADATA, vol);

	releasewrite_mrsw(&d->meta_lock);
	releaseread_mrsw(&vol->lock);

	return 0;
}


int ltfs_fsops_set_readonly(struct dentry *d, bool readonly, struct ltfs_volume *vol)
{
	int ret;

	CHECK_ARG_NULL(d, -LTFS_NULL_ARG);
	CHECK_ARG_NULL(vol, -LTFS_NULL_ARG);

	/* Make sure the device is online and writable */
	ret = ltfs_get_tape_readonly(vol);
	if (ret < 0 && ret != -LTFS_LESS_SPACE)
		return ret;
	ret = ltfs_test_unit_ready(vol);
	if (ret < 0) {
		ltfsmsg(LTFS_ERR, "11046E");
		return ret;
	}

	ret = ltfs_get_volume_lock(false, vol);
	if (ret < 0)
		return ret;
	acquirewrite_mrsw(&d->meta_lock);
	if (readonly != d->readonly) {
		d->readonly = readonly;
		get_current_timespec(&d->change_time);
		ltfs_set_index_dirty(true, false, vol->index);
		if (dcache_initialized(NULL))
			dcache_flush(d, FLUSH_METADATA, vol);
	}
	releasewrite_mrsw(&d->meta_lock);
	releaseread_mrsw(&vol->lock);

	return 0;
}

int ltfs_fsops_set_readonly_path(const char *path, bool readonly, ltfs_file_id *id, struct ltfs_volume *vol)
{
	int ret;
	struct dentry *d;

	id->uid = 0;
	id->ino = 0;

	CHECK_ARG_NULL(path, -LTFS_NULL_ARG);
	CHECK_ARG_NULL(vol, -LTFS_NULL_ARG);

	ret = ltfs_fsops_open(path, false, false, &d, vol);
	if (ret < 0)
		return ret;

	ret = ltfs_fsops_set_readonly(d, readonly, vol);
	id->uid = d->uid;
	id->ino = d->ino;
	ltfs_fsops_close(d, false, false, false, vol);

	return ret;
}

int ltfs_fsops_write(struct dentry *d, const char *buf, size_t count, off_t offset,
	bool isupdatetime, struct ltfs_volume *vol)
{
	ssize_t ret;

	CHECK_ARG_NULL(d, -LTFS_NULL_ARG);
	CHECK_ARG_NULL(buf, -LTFS_NULL_ARG);
	CHECK_ARG_NULL(vol, -LTFS_NULL_ARG);
	if (d->isdir)
		return -LTFS_ISDIRECTORY;

	/* We don't check for device read-only here because the I/O scheduler and ltfs_fsraw_write()
	 * both know to do that themselves.
	 * We don't check for unit ready here because this call guarantees a write request later,
	 * which will catch the unready condition. */

	if (iosched_initialized(vol)) {
		ret = iosched_write(d, buf, count, offset, isupdatetime, vol);
		if (!isupdatetime && ret>=0)
			d->need_update_time = true;
	} else {
		if (isupdatetime)
			ret = ltfs_fsraw_write(d, buf, count, offset, ltfs_dp_id(vol), true, vol);
		else {
			ret = ltfs_fsraw_write(d, buf, count, offset, ltfs_dp_id(vol), false, vol);
			if (ret>=0) d->need_update_time = true;
		}
	}

	if (ret < 0)
		return ret;
	else
		return 0;
}

ssize_t ltfs_fsops_read(struct dentry *d, char *buf, size_t count, off_t offset,
	struct ltfs_volume *vol)
{
	ssize_t ret;

	CHECK_ARG_NULL(d, -LTFS_NULL_ARG);
	CHECK_ARG_NULL(buf, -LTFS_NULL_ARG);
	CHECK_ARG_NULL(vol, -LTFS_NULL_ARG);
	if (d->isdir)
		return -LTFS_ISDIRECTORY;

	if (iosched_initialized(vol))
		ret = iosched_read(d, buf, count, offset, vol);
	else
		ret = ltfs_fsraw_read(d, buf, count, offset, vol);

	return ret;
}

int ltfs_fsops_truncate(struct dentry *d, off_t length, struct ltfs_volume *vol)
{
	int ret;

	CHECK_ARG_NULL(d, -LTFS_NULL_ARG);
	CHECK_ARG_NULL(vol, -LTFS_NULL_ARG);
	if (length < 0) {
		ltfsmsg(LTFS_ERR, "11059E");
		return -LTFS_BAD_ARG;
	} else if (d->isdir)
		return -LTFS_ISDIRECTORY;

	ret = ltfs_get_tape_readonly(vol);
	if (ret < 0)
		return ret;
	ret = ltfs_test_unit_ready(vol);
	if (ret < 0) {
		ltfsmsg(LTFS_ERR, "11062E");
		return ret;
	}

	if (iosched_initialized(vol))
		ret = iosched_truncate(d, length, vol);
	else
		ret = ltfs_fsraw_truncate(d, length, vol);

	if (ret == 0 && dcache_initialized(NULL))
		dcache_flush(d, (FLUSH_EXTENT_LIST | FLUSH_METADATA), vol);

	ret = ltfs_fsops_update_used_blocks(d, vol);

	return ret;
}

int ltfs_fsops_truncate_path(const char *path, off_t length, ltfs_file_id *id, struct ltfs_volume *vol)
{
	int ret;
	struct dentry *d;

	id->uid = 0;
	id->ino = 0;

	CHECK_ARG_NULL(path, -LTFS_NULL_ARG);
	CHECK_ARG_NULL(vol, -LTFS_NULL_ARG);

	ret = ltfs_fsops_open(path, true, false, &d, vol);
	if (ret < 0)
		return ret;

	ret = ltfs_fsops_truncate(d, length, vol);
	id->uid = d->uid;
	id->ino = d->ino;
	ltfs_fsops_close(d, false, true, false, vol);
	return ret;
}

int ltfs_fsops_flush(struct dentry *d, bool closeflag, struct ltfs_volume *vol)
{
	int ret = 0;

	CHECK_ARG_NULL(vol, -LTFS_NULL_ARG);
	if (d && d->isdir)
		return -LTFS_ISDIRECTORY;

	/* Don't need to check for read-only or unit ready here; the I/O scheduler will check for
	 * those conditions if it wants to know about them. */

	if (iosched_initialized(vol))
		ret = iosched_flush(d, closeflag, vol);

	if (dcache_initialized(NULL))
		dcache_flush(d, FLUSH_ALL, vol);

	return ret;
}

int ltfs_fsops_symlink_path(const char* to, const char* from, ltfs_file_id *id, struct ltfs_volume *vol)
{
	struct dentry *d = NULL;
	bool use_iosche=false;
	int ret=0, ret2=0;
	char *value = NULL;
	size_t size;

	id->uid = 0;
	id->ino = 0;

	CHECK_ARG_NULL(vol, -LTFS_NULL_ARG);

	if ( iosched_initialized(vol) ) use_iosche=true;

	ltfsmsg(LTFS_DEBUG, "11322D", from, to);

	/* Create a node */
	ret = ltfs_fsops_create(from, false, true, &d, vol);
	if (ret < 0)
		return ret;

	id->uid = d->uid;
	id->ino = d->ino;
	d->target = strdup(to);
	d->isslink = true;

	/* Set mount point length in EA (LiveLink support mode only) */
	if ( ( strncmp( to, vol->mountpoint, vol->mountpoint_len )==0 ) &&
		 ( to[vol->mountpoint_len]=='/' ) )
		ret = asprintf( &value, "%d", (int) vol->mountpoint_len );
	else
		ret = asprintf( &value, "0" );
	if ( ret < 0 )
		return -LTFS_NO_MEMORY;

	size = strlen(value);
	ltfsmsg(LTFS_DEBUG, "11323D", value);
	ret = xattr_set_mountpoint_length( d, value, size );

	if (value) {
		free(value);
		value = NULL;
	}

	ret2 = ltfs_fsops_close(d, true, true, use_iosche, vol);
	if ( ret==0 && ret2<0 ) {
		ret = ret2;
	}

	return ret;
}

int ltfs_fsops_readlink_path(const char* path, char* buf, size_t size, ltfs_file_id *id, struct ltfs_volume *vol)
{
	struct dentry *d;
	bool use_iosche=false;
	int ret=0;
	char value[32];
	int num1,num2;

	id->uid = 0;
	id->ino = 0;

	CHECK_ARG_NULL(vol, -LTFS_NULL_ARG);

	if ( iosched_initialized(vol) ) use_iosche=true;

	/* Open the file */
	ret = ltfs_fsops_open(path, false, use_iosche, &d, vol);
	if (ret < 0)
		return ret;

	id->uid = d->uid;
	id->ino = d->ino;
	
	if (! d->target) {
		ret = ltfs_fsops_close(d, false, false, use_iosche, vol);
		return -ENOENT;
	}
	
	if ( size < strlen(d->target)+1 ) {
		return -LTFS_SMALL_BUFFER;
	}
	strncpy(buf, d->target, size);

	if ( vol->livelink ) {
		memset( value, 0, sizeof(value));
		ret = xattr_get(d, LTFS_LIVELINK_EA_NAME, value, sizeof(value), vol);
		if ( ret > 0 ) {
			ltfsmsg(LTFS_DEBUG, "11323D", value);
			ret = sscanf(value, "%d:%d", &num1, &num2);
			if ( ( ret == 1 ) && ( num1 != 0 ) ){
				memset( buf, 0, size);
#ifndef mingw_PLATFORM
				if ( size < strlen(d->target)-num1+vol->mountpoint_len+1 ){
					return -LTFS_SMALL_BUFFER;
				}
				strcpy(buf, vol->mountpoint);
#endif
				strcat(buf, d->target+num1 );
				ltfsmsg(LTFS_DEBUG, "11324D", d->target, buf);
			}
		}
	}

	ret = ltfs_fsops_close(d, false, false, use_iosche, vol);
	if (ret < 0)
		return ret;


	return 0; /* Shall be return 0, if success */
}

int ltfs_fsops_target_absolute_path(const char* link, const char* target, char* buf, size_t size )
{
	char *work_buf, *target_buf, *temp_buf, *token, *next_token; // work buffer for string
	int len=0,len2=0;	// work variables for string length

	CHECK_ARG_NULL(link, -LTFS_NULL_ARG);
	CHECK_ARG_NULL(target, -LTFS_NULL_ARG);

	// need to set message and return code
	if (link[0]!='/') {
		return -LTFS_BAD_ARG;
	}

	len2 = strlen(target);
	// input target string is already absolute path
	if ( (target[0]=='/') && !strstr(target,"./" ) ) {
		if ( size < (size_t)len2+1) {
			return -LTFS_SMALL_BUFFER;
		}
		strcpy( buf, target );
		return 0;
	}

	len=strlen(link);
	work_buf = malloc(len+len2+1);
	if (!work_buf)  {
		return -LTFS_NO_MEMORY;
	}

	target_buf = malloc(len2+1);
	if (!target_buf) {
		free( work_buf );
		return -LTFS_NO_MEMORY;
	}

	if ( target[0]=='/' ) {
		temp_buf=strstr( target, "/." );		// get "/../ccc/target.txt" of "/aaa/../ccc/target.txt"
		strcpy(target_buf,temp_buf+1);			// copy "../ccc/target.txt"
		len=strlen(target_buf)+1;
		len=len2-len;
		strncpy( work_buf, target, len );		// copy "/aaa"
	} else {
		strcpy( work_buf, link );
		strcpy( target_buf, target );

		// split link file name then get current directory
		temp_buf=strrchr( work_buf, '/' );		// get "/link.txt" from "/aaa/bbb/link.txt"
		len = len-strlen(temp_buf);				// length of "/aaa/bbb"
	}

	// split target path directory then modify current directory with target path information
	token = strtok( target_buf,"/" );			//  get ".." from "../ccc/target.txt"
	while ( token ) {
		next_token = strtok( NULL,"/" );		// if next_token is NULL then token is filename
		if ( !next_token ) break;
		if ( strcmp( token,".." )==0 ) {
			work_buf[len]='\0';					// "/aaa/bbb\0link.txt"
			temp_buf=strrchr( work_buf, '/' );	// get "/bbb"
			if (!temp_buf) {
				*buf = '\0';					// out of ltfs range
				return 0;
			}
			len = len-strlen(temp_buf);			// length of "/aaa"
		} else if ( strcmp( token, "." ) ) {	// have directory name
			work_buf[len]='/';					// put '/ 'as "/aaa/"
			strncpy( work_buf+len+1, token, strlen(token)+1 );	// "/aaa/ccc\0"
			len=strlen(work_buf);
		}
		token = next_token;
	}
	work_buf[len]='/';									// put '/ 'as "/aaa/ccc/"
	strncpy( work_buf+len+1, token, strlen(token)+1 );	// "/aaa/ccc/target.txt\0"

	if ( size < strlen(work_buf)+1 ) {
		free( work_buf );
		free( target_buf );
		return -LTFS_SMALL_BUFFER;
	}

	strcpy( buf, work_buf );
	free( work_buf );
	free( target_buf );
	return 0;
}



int ltfs_fsops_volume_sync(struct ltfs_volume *vol)
{
	int ret;

	ret = ltfs_fsops_flush(NULL, false, vol);
	if (ret < 0)
		return ret;

	ret = ltfs_sync_index(SYNC_EXPLICIT, true, vol);

	return ret;
}

int ltfs_fsops_set_write_protected(struct ltfs_volume *vol)
{
	int ret = 0;

	CHECK_ARG_NULL(vol, -LTFS_NULL_ARG);

	pthread_mutex_lock(&vol->device->read_only_flag_mutex);
	vol->device->write_protect = 1;
	pthread_mutex_unlock(&vol->device->read_only_flag_mutex);

	return ret;
}
