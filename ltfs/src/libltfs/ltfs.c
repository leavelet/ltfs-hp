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
** FILE NAME:       ltfs.c
**
** DESCRIPTION:     LTFS core operations.
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
**  06/10/10 Reference PACKAGE_OWNER (from ltfs.h) in ltfs_volume_alloc rather than
**            using a hardcoded value
**  07/06/10 Extended handling of ENOSPC to permit current file to complete writing
**            (assuming sufficient space on tape)
**           Added 'force' parameter to ltfs_format_tape, must be TRUE to allow 
**            reformatting of existing LTFS volume (see also mkltfs.c)
**  03/04/11 Added buffer flush in ltfs_write_index to ensure correct VCR value is
**            used when constructing VCI to be stored
**  07/09/13 Do not unload in ltfs_recover_eod_simple because it is unnecessary and
**            breaks the recovery process by kicking the tape right out
**  03/25/14 In mkdir_p, convert \ to / so that loop iterates properly on Windows
**  06/16/17 In mkdir_p, return -1 on failure not 1 since caller looks for (< 0)
**  10/13/17 Added support for SNIA 2.4
**
*************************************************************************************
**
** Copyright (C) 2012 OSR Open Systems Resources, Inc.
** 
************************************************************************************* 
*/

#ifdef __APPLE__
#include <ICU/unicode/uclean.h>
#else

/* 
 * OSR
 * 
 * Some ICU header files are ill formed and do not include the
 * right files for the MinGW environment if __MINGW32__ is 
 * defined. Strange, yes, but true 
 *  
 */
#if defined(HPE_mingw_BUILD) && defined(__MINGW32__)

#undef __MINGW32__
#include <unicode/uclean.h>
#define __MINGW32__

#else 
#include <unicode/uclean.h>
#endif /* #if defined(HPE_mingw_BUILD) && defined(__MINGW32__) */

#endif

#include "arch/uuid_internal.h"

#include "fs.h"
#include "ltfs.h"
#include "ltfs_internal.h"
#include "libltfs/ltfslogging.h"
#include "ltfs_copyright.h"
#include "tape.h"
#include "tape_ops.h"
#include "pathname.h"
#include "arch/time_internal.h"
#include "index_criteria.h"
#include "xattr.h"
#include "xml_libltfs.h"
#include "label.h"
#include "arch/version.h"
#include "arch/filename_handling.h"
#include "libltfs/arch/errormap.h"
#include "iosched.h"
#include "dcache.h"
#include "kmi.h"

#define MIN(a, b) ((a) < (b) ? (a) : (b))
/*
 * OSR
 *
 * Linker complains about too many definitions of copyright if
 * this is not left out of the build
 */
#ifndef HPE_mingw_BUILD
volatile char *copyright = LTFS_COPYRIGHT_0"\n"LTFS_COPYRIGHT_1"\n"LTFS_COPYRIGHT_2"\n" \
	LTFS_COPYRIGHT_3"\n"LTFS_COPYRIGHT_4"\n"LTFS_COPYRIGHT_5"\n";
#endif /* HPE_mingw_BUILD */

/** \file
 * The typical use case for this library is as follows.
 * For each drive to be controlled:
 *   ltfs_volume_alloc() to allocate a new LTFS volume
 *   ltfs_mount() to read the LTFS data structures into memory
 *   read and modify the filesystem
 *   ltfs_unmount() to flush data and make the tape consistent
 *   ltfs_volume_free() to free in-memory volume data
 */

/**
 * Get a string representing the running version of libltfs.
 */
const char *ltfs_version()
{
	return PACKAGE_VERSION;
}

/**
 * Get a string representing the version of LTFS format specification.
 */
const char *ltfs_format_version()
{
	return LTFS_INDEX_VERSION_STR;
}


/**
 * Initialize the LTFS functions, currently the XML parser and the logging component.
 */
int ltfs_init(int log_level, bool use_syslog, bool print_thread_id)
{
	int ret;

	ret = ltfsprintf_init(log_level, use_syslog, print_thread_id);
	if (ret < 0) {
		fprintf(stderr, "LTFS9011E Logging initialization failed\n");
		return ret;
	}

	ret = errormap_init();
	if (ret < 0) {
		ltfsprintf_finish();
		return ret;
	}

	ret = ltfs_trace_init();
	if (ret < 0) {
		ltfsprintf_finish();
		return ret;
	}

	xml_init();

	return 0;
}

/**
 * Initialize the fs conponents of libltfs
 */
int ltfs_fs_init(void)
{
	int ret;

	ret = fs_init_inode();
	if (ret < 0)
		ltfsmsg(LTFS_ERR, "17232E", ret);

	return ret;
}

/**
 * This function can be used to change the libltfs logging level after
 * ltfs_init() has been called.
 */
void ltfs_set_log_level(int log_level)
{
	ltfs_log_level = log_level;
}

/**
 * This function can be used to change the libltfs logging level for syslog
 * after ltfs_init() has been called.
 */
void ltfs_set_syslog_level(int syslog_level)
{
	ltfs_syslog_level = syslog_level;
}

/**
 * libltfs signal handler. Only set condition variable
 * Actual termination should be processed by each command
 */
bool interrupted = false;
void _ltfs_terminate(int signal)
{
	interrupted = true;
}

/**
 * Check function to detect terminate condition which is set b signal handler.
 */
bool ltfs_is_interrupted(void)
{
	return interrupted;
}

/**
 * This function can be used to enable libltfs signal handler
 * to kill ltfs, mkltfs, ltfsck cleanly
 */
int ltfs_set_signal_handlers(void)
#ifdef mingw_PLATFORM
{
  return 0;
}
#else
{
	sighandler_t ret;

	interrupted = false;

	/* Terminate by CTRL-C */
	ret = signal(SIGINT, _ltfs_terminate);
	if(ret == SIG_ERR)
		return -LTFS_SIG_HANDLER_ERR;

	/* Terminate by disconnecting terminal */
	ret = signal(SIGHUP, _ltfs_terminate);
	if(ret == SIG_ERR) {
		signal(SIGINT, SIG_DFL);
		return -LTFS_SIG_HANDLER_ERR;
	}

	/* Terminate by CTRL-\ */
	ret = signal(SIGQUIT, _ltfs_terminate);
	if(ret == SIG_ERR) {
		signal(SIGINT, SIG_DFL);
		signal(SIGHUP, SIG_DFL);
		return -LTFS_SIG_HANDLER_ERR;
	}

	/* Terminate by default signal of kill command */
	ret = signal(SIGTERM, _ltfs_terminate);
	if(ret == SIG_ERR) {
		signal(SIGINT, SIG_DFL);
		signal(SIGHUP, SIG_DFL);
		signal(SIGQUIT, SIG_DFL);
		return -LTFS_SIG_HANDLER_ERR;
	}

	return 0;
}
#endif

/**
 * This function can be used to disable libltfs signal handler.
 * This function will be called before calling fuse_main
 */
int ltfs_unset_signal_handlers(void)
#ifdef mingw_PLATFORM
{
  return 0;
}
#else
{
	sighandler_t rc;
	int ret = 0;

	rc = signal(SIGINT, SIG_DFL);
	if (rc == SIG_ERR)
		ret = -LTFS_SIG_HANDLER_ERR;

	rc = signal(SIGHUP, SIG_DFL);
	if (rc == SIG_ERR)
		ret = -LTFS_SIG_HANDLER_ERR;

	rc = signal(SIGQUIT, SIG_DFL);
	if (rc == SIG_ERR)
		ret = -LTFS_SIG_HANDLER_ERR;

	rc = signal(SIGTERM, SIG_DFL);
	if (rc == SIG_ERR)
		ret = -LTFS_SIG_HANDLER_ERR;

	return ret;
}
#endif

/**
 * Call this after all ltfs_* calls are finished.
 */
int ltfs_finish()
{
	xml_finish();
	ltfs_trace_destroy();
	errormap_finish();
	ltfsprintf_finish();
	return 0;
}

/**
 * Initialize an LTFS volume.
 * @param execname name of program calling ltfs_volume_alloc, used in the "creator" tag when
 *                 writing labels and index files. May be NULL if the program does not intend
 *                 to write to the tape.
 * @param volume points to a newly allocated struct ltfs_volume on success
 * @return 0 on success or a negative value on error
 */
int ltfs_volume_alloc(const char *execname, struct ltfs_volume **volume)
{
	int ret;
	struct ltfs_volume *newvol;

	CHECK_ARG_NULL(volume, -LTFS_NULL_ARG);

	newvol = calloc(1, sizeof(struct ltfs_volume));
	if (!newvol) {
		/* Memory allocation failed */
		ltfsmsg(LTFS_ERR, "10001E", "ltfs_volume_alloc");
		return -LTFS_NO_MEMORY;
	}

	ret = tape_device_alloc(&newvol->device);
	if (ret < 0) {
		/* Couldn't allocate device data structure */
		ltfsmsg(LTFS_ERR, "11000E");
		goto out_volfree;
	}

	ret = label_alloc(&newvol->label);
	if (ret < 0) {
		/* Failed to allocate label data */
		ltfsmsg(LTFS_ERR, "11001E");
		goto out_devfree;
	}

	ret = ltfs_index_alloc(&newvol->index, newvol);
	if (ret < 0) {
		/* Failed to allocate index data */
		ltfsmsg(LTFS_ERR, "11002E");
		goto out_labelfree;
	}

	newvol->livelink = false;
	newvol->mountpoint_len = 0;
	newvol->set_pew = false;

	ret = init_mrsw(&newvol->lock);
	if (ret < 0) {
		ltfsmsg(LTFS_ERR, "10002E", ret);
		goto out_indexfree;
	}
	ret = ltfs_thread_mutex_init(&newvol->reval_lock);
	if (ret) {
		ltfsmsg(LTFS_ERR, "10002E", ret);
		ret = -LTFS_MUTEX_INIT;
		goto out_lockfree;
	}
	ret = ltfs_thread_cond_init(&newvol->reval_cond);
	if (ret) {
		ltfsmsg(LTFS_ERR, "10003E", ret);
		ret = -LTFS_MUTEX_INIT;
		goto out_lockfree2;
	}

	if (execname) {
		ret = asprintf(&newvol->creator, CREATOR_STRING_FORMAT,
			PACKAGE_OWNER, PACKAGE_VERSION, PLATFORM, execname);
		if (ret < 0) {
			/* Memory allocation failed */
			ltfsmsg(LTFS_ERR, "10001E", "ltfs_volume_alloc, creator string");
			ret = -LTFS_NO_MEMORY;
			goto out_condfree;
		}
	}

	/* Initialize the volume advisory locking bitfield*/
	newvol->lockbits.bitfield = 0x00;
	ret = ltfs_thread_mutex_init(&newvol->lockbits.lock_bitfield);
	if (ret) {
		ltfsmsg(LTFS_ERR, "10002E", ret);
		ret = -LTFS_MUTEX_INIT;
		goto out_lockfree;
	}

	*volume = newvol;

	return 0;

out_condfree:
	ltfs_thread_cond_destroy(&newvol->reval_cond);
out_lockfree2:
	ltfs_thread_mutex_destroy(&newvol->reval_lock);
out_lockfree:
	destroy_mrsw(&newvol->lock);
out_indexfree:
	ltfs_index_free(&newvol->index);
out_labelfree:
	label_free(&newvol->label);
out_devfree:
	tape_device_free(&newvol->device, newvol->kmi_handle, false);
out_volfree:
	free(newvol);

	return ret;
}

/**
 * Free an LTFS volume.
 * @param force force free memory if reference counter is not 0
 * @param volume the volume to free, set to NULL on success
 */
void _ltfs_volume_free(bool force, struct ltfs_volume **volume)
{
	if (volume && *volume) {
		label_free(&(*volume)->label);
		_ltfs_index_free(force, &(*volume)->index);
		if ((*volume)->device)
			tape_device_free(&(*volume)->device, (*volume)->kmi_handle, false);

		if ((*volume)->mam_attr.appl_vendor) {
			free((*volume)->mam_attr.appl_vendor);
			(*volume)->mam_attr.appl_vendor = NULL;
		}
		if ((*volume)->mam_attr.appl_name) {
			free((*volume)->mam_attr.appl_name);
			(*volume)->mam_attr.appl_name = NULL;
		}
		if ((*volume)->mam_attr.appl_ver) {
			free((*volume)->mam_attr.appl_ver);
			(*volume)->mam_attr.appl_ver = NULL;
		}
		if ((*volume)->mam_attr.appl_format_ver) {
			free((*volume)->mam_attr.appl_format_ver);
			(*volume)->mam_attr.appl_format_ver = NULL;
		}
		if ((*volume)->mam_attr.volume_name) {
			free((*volume)->mam_attr.volume_name);
			(*volume)->mam_attr.volume_name = NULL;
		}
		if ((*volume)->mam_attr.barcode) {
			free((*volume)->mam_attr.barcode);
			(*volume)->mam_attr.barcode = NULL;
		}
		if ((*volume)->last_block)
			free((*volume)->last_block);
		if ((*volume)->creator)
			free((*volume)->creator);
		if ((*volume)->mountpoint)
			free((*volume)->mountpoint);
		destroy_mrsw(&(*volume)->lock);
		ltfs_thread_mutex_destroy(&(*volume)->reval_lock);
		ltfs_thread_cond_destroy(&(*volume)->reval_cond);
		free(*volume);
		*volume = NULL;
	}
}

/**
 * Get the backend's default device name.
 * @param ops tape operations for the backend
 * @return a constant string to the backend's default device or NULL if the
 *  backend doesn't define a default one.
 */
const char *ltfs_default_device_name(struct tape_ops *ops)
{
	const char *devname = NULL;

	CHECK_ARG_NULL(ops, NULL);

	devname = tape_default_device_name(ops);
	return devname;
}

/**
 * Convenience wrapper for tape_device_open.
 */
int ltfs_device_open(const char *devname, struct tape_ops *ops, struct ltfs_volume *vol)
{
	int ret;
	unsigned int block_size;

	CHECK_ARG_NULL(vol, -LTFS_NULL_ARG);
	ret = tape_device_open(vol->device, devname, ops, vol->kmi_handle);

	if (ret < 0)
		return ret;

	ret = tape_get_max_blocksize(vol->device, &block_size);
	if (ret < 0) {
		ltfsmsg(LTFS_ERR, "17195E", "open", ret);
		return ret;
	}
	ltfsmsg(LTFS_INFO, "17160I", block_size);

	return 0;
}

/**
 * Convenience wrapper for tape_device_reopen.
 */
int ltfs_device_reopen(const char *devname, struct ltfs_volume *vol)
{
	CHECK_ARG_NULL(vol, -LTFS_NULL_ARG);

	return tape_device_reopen(vol->device, devname);
}

/**
 * Convenience wrappers for tape_device_close.
 */
void ltfs_device_close(struct ltfs_volume *vol)
{
	if (vol)
		tape_device_close(vol->device, vol->kmi_handle, false);
}

void ltfs_device_close_skip_append_only_mode(struct ltfs_volume *vol)
{
	if (vol)
		_tape_device_close(vol->device, vol->kmi_handle, true, false);
}

/**
 * Setup tape device.
 */
int ltfs_setup_device(struct ltfs_volume *vol)
{
#ifdef QUANTUM_BUILD
	// Quantum's V3085 firmware doesn't support device configuration extension mode page 
	// 10h subpage 01h or append-only mode
	return 0;
#else
	int ret;
	bool enabled;

	CHECK_ARG_NULL(vol, -LTFS_NULL_ARG);

	/* Check a cartrige is loaded or at lock position
	   and suppress unnessesary senses before issueing mode select in follwing part */
	ret = tape_is_cartridge_loadable(vol->device);
	if (ret < 0)
		return ret;

	/* Set Programmable Early Warning Space so that half of
	   Index partition space is reserved for index file. */
	ret = tape_set_pews(vol->device, vol->set_pew);
	if (ret < 0)
		return ret;

	/*
	 * HPE change: HP drives don't support the append only mode functionality.
	 * All the options and messages related to that are disabled. Code remains
	 * but is non-functional.
	 */
	if (vol->append_only_mode) {
		ltfsmsg(LTFS_INFO, "17157I", "to append-only mode");
		ret = tape_enable_append_only_mode(vol->device, true);
	} else {
		/* Check write mode and reset to write-anywhere mode if required. */
		ltfsmsg(LTFS_INFO, "17157I", "to write-anywhere mode");
		ret = tape_get_append_only_mode_setting(vol->device, &enabled);
		if (ret < 0)
			return ret;
		if (enabled) {
			ltfsmsg(LTFS_INFO, "17157I", "from append-only mode to write-anywhere mode");
			ret = tape_enable_append_only_mode(vol->device, false);
		}
	}

	return ret;
#endif /* QUANTUM_BUILD */
}

/**
 * Check whether the device is ready.
 * This is a locking wrapper for tape_test_unit_ready.
 * Must not be called with a lock on the volume or on the device.
 * @param vol LTFS volume.
 */
int ltfs_test_unit_ready(struct ltfs_volume *vol)
{
	int ret;

	CHECK_ARG_NULL(vol, -LTFS_NULL_ARG);

start:
	ret = ltfs_get_volume_lock(false, vol);
	if (ret < 0)
		return ret;
	ret = tape_device_lock(vol->device);
	if (ret == -LTFS_DEVICE_FENCED) {
		ret = ltfs_wait_revalidation(vol);
		if (ret == 0)
			goto start;
		else
			return ret;
	} else if (ret < 0) {
		ltfsmsg(LTFS_ERR, "12010E", __FUNCTION__);
		releaseread_mrsw(&vol->lock);
		return ret;
	}

	ret = tape_test_unit_ready(vol->device);
	if (NEED_REVAL(ret)) {
		tape_start_fence(vol->device);
		tape_device_unlock(vol->device);
		ret = ltfs_revalidate(false, vol);
		if (ret == 0)
			goto start;
	} else if (IS_UNEXPECTED_MOVE(ret)) {
		vol->reval = -LTFS_REVAL_FAILED;
		tape_device_unlock(vol->device);
		releaseread_mrsw(&vol->lock);
	} else {
		/* Users generally don't care what kind of backend error occurred, only that
		 * the device is not ready. This is needed to ensure that FUSE operations
		 * return EBUSY when getting device readiness fails. */
		if (ret <= -EDEV_ERR_MIN)
			ret = -LTFS_DEVICE_UNREADY;
		tape_device_unlock(vol->device);
		releaseread_mrsw(&vol->lock);
	}
	return ret;
}

/**
 * Convenience wrapper for tape_parse_opts.
 */
int ltfs_parse_tape_backend_opts(void *opt_args, struct ltfs_volume *vol)
{
	int ret;

	CHECK_ARG_NULL(vol, -LTFS_NULL_ARG);
	CHECK_ARG_NULL(opt_args, -LTFS_NULL_ARG);

	ret = tape_parse_opts(vol->device, opt_args);

	return ret;
}

/**
 * Convenience wrapper for kmi_parse_opts.
 */
int ltfs_parse_kmi_backend_opts(void *opt_args, struct ltfs_volume *vol)
{
	int ret;

	CHECK_ARG_NULL(vol, -LTFS_NULL_ARG);
	CHECK_ARG_NULL(opt_args, -LTFS_NULL_ARG);

	ret = kmi_parse_opts(vol->kmi_handle, opt_args);

	return ret;
}

/**
 * Convenience wrapper for tape_parse_library_backend_opts.
 */
int ltfs_parse_library_backend_opts(void *opt_args, void *opts)
{
	int rc;

	CHECK_ARG_NULL(opt_args, -LTFS_NULL_ARG);
	CHECK_ARG_NULL(opts, -LTFS_NULL_ARG);

	rc = tape_parse_library_backend_opts(opts, opt_args);

	return rc;
}

/**
 * Get capacity data in filesystem block units. Converts the result of
 * tape_get_capacity from partition 0/1 to data/index,
 * and from native device capacities to filesystem block units.
 * Must not be called with a lock on the volume or on the device.
 */
int ltfs_capacity_data(struct device_capacity *cap, struct ltfs_volume *vol)
{
	int ret;
	CHECK_ARG_NULL(vol, -LTFS_NULL_ARG);

start:
	ret = ltfs_get_volume_lock(false, vol);
	if (ret < 0)
		return ret;
	ret = ltfs_capacity_data_unlocked(cap, vol);
	if (ret == -LTFS_DEVICE_FENCED) {
		ret = ltfs_wait_revalidation(vol);
		if (ret == 0)
			goto start;
	} else if (NEED_REVAL(ret)) {
		ret = ltfs_revalidate(false, vol);
		if (ret == 0)
			goto start;
	} else if (IS_UNEXPECTED_MOVE(ret)) {
		vol->reval = -LTFS_REVAL_FAILED;
		releaseread_mrsw(&vol->lock);
	}else
		releaseread_mrsw(&vol->lock);
	return ret;
}

/**
 * Non-locking version of ltfs_capacity_data(). Call this function with a read lock
 * on the volume.
 */
int ltfs_capacity_data_unlocked(struct device_capacity *cap, struct ltfs_volume *vol)
{
	int ret;
	struct tc_remaining_cap phys_cap; /* physical capacity, partitions 0/1 */
	double cap_scale = 1024.0 * 1024.0 / vol->label->blocksize;

	CHECK_ARG_NULL(vol, -LTFS_NULL_ARG);
	CHECK_ARG_NULL(cap, -LTFS_NULL_ARG);

	if (vol->device) {
		ret = tape_device_lock(vol->device);
		if (ret < 0) {
			ltfsmsg(LTFS_ERR, "12010E", __FUNCTION__);
			return ret;
		}

		ret = tape_test_unit_ready(vol->device);
		if (ret < 0) {
			if (NEED_REVAL(ret))
				tape_start_fence(vol->device);
			else if (IS_UNEXPECTED_MOVE(ret))
				vol->reval = -LTFS_REVAL_FAILED;
			tape_device_unlock(vol->device);
			return ret;
		}

		ret = tape_get_capacity(vol->device, &phys_cap);
		if (NEED_REVAL(ret))
			tape_start_fence(vol->device);
		else if (IS_UNEXPECTED_MOVE(ret))
			vol->reval = -LTFS_REVAL_FAILED;
		tape_device_unlock(vol->device);
		if (ret < 0) {
			ltfsmsg(LTFS_ERR, "11003E", ret);
			return ret;
		}

		if (vol->label->part_num2id[1] == vol->label->partid_ip) {
			cap->remaining_dp = phys_cap.remaining_p0 * cap_scale;
			cap->remaining_ip = phys_cap.remaining_p1 * cap_scale;
			cap->total_dp = phys_cap.max_p0 * cap_scale;
			cap->total_ip = phys_cap.max_p1 * cap_scale;
		} else {
			cap->remaining_ip = phys_cap.remaining_p0 * cap_scale;
			cap->remaining_dp = phys_cap.remaining_p1 * cap_scale;
			cap->total_ip = phys_cap.max_p0 * cap_scale;
			cap->total_dp = phys_cap.max_p1 * cap_scale;
		}

		if (cap->total_dp <= cap->total_ip / 2)
			cap->total_dp = 0;
		else
			cap->total_dp -= (cap->total_ip / 2);

		ret = ltfs_get_partition_readonly(ltfs_dp_id(vol), vol);
		if (ret == -LTFS_NO_SPACE || ret == -LTFS_LESS_SPACE)
			cap->remaining_dp = 0;
		else if (cap->remaining_dp <= cap->total_ip / 2)
			cap->remaining_dp = 0;
		else
			cap->remaining_dp -= (cap->total_ip / 2);

		memcpy(&vol->capacity_cache, cap, sizeof(struct device_capacity));

	} else
		memcpy(cap, &vol->capacity_cache, sizeof(struct device_capacity));

	return 0;
}

/**
 * Get media health data from the device.
 * This is a locking wrapper for tape_get_cartridge_health().
 * Must be called with a lock held on the volume and no lock held on the device.
 */
int ltfs_get_cartridge_health(cartridge_health_info *h, struct ltfs_volume *vol)
{
	int ret = 0;

	CHECK_ARG_NULL(h, -LTFS_NULL_ARG);
	CHECK_ARG_NULL(vol, -LTFS_NULL_ARG);

	if (vol->device) {
		ret = tape_device_lock(vol->device);
		if (ret < 0) {
			ltfsmsg(LTFS_ERR, "12010E", __FUNCTION__);
			return ret;
		}

		ret = tape_test_unit_ready(vol->device);
		if (ret < 0) {
			if (NEED_REVAL(ret))
				tape_start_fence(vol->device);
			else if (IS_UNEXPECTED_MOVE(ret))
				vol->reval = -LTFS_REVAL_FAILED;
			tape_device_unlock(vol->device);
			return ret;
		}

		ret = tape_get_cartridge_health(vol->device, &vol->health_cache);
		if (NEED_REVAL(ret))
			tape_start_fence(vol->device);
		else if (IS_UNEXPECTED_MOVE(ret))
			vol->reval = -LTFS_REVAL_FAILED;
		memcpy(h, &vol->health_cache, sizeof(cartridge_health_info));
		tape_device_unlock(vol->device);
	} else
		memcpy(h, &vol->health_cache, sizeof(cartridge_health_info));

	return ret;
}

/**
 * Get tape alert from the device.
 * This is a locking wrapper for tape_get_tape_alert().
 * Must be called with no lock held on the volume and no lock held on the device.
 */
int ltfs_get_tape_alert(uint64_t *tape_alert, struct ltfs_volume *vol)
{
	int ret;
	CHECK_ARG_NULL(vol, -LTFS_NULL_ARG);

start:
	ret = ltfs_get_volume_lock(false, vol);
	if (ret < 0)
		return ret;
	ret = ltfs_get_tape_alert_unlocked(tape_alert, vol);
	if (ret == -LTFS_DEVICE_FENCED) {
		ret = ltfs_wait_revalidation(vol);
		if (ret == 0)
			goto start;
	} else if (NEED_REVAL(ret)) {
		ret = ltfs_revalidate(false, vol);
		if (ret == 0)
			goto start;
	} else if (IS_UNEXPECTED_MOVE(ret)) {
		vol->reval = -LTFS_REVAL_FAILED;
		releaseread_mrsw(&vol->lock);
	} else
		releaseread_mrsw(&vol->lock);
	return ret;
}

/**
 * Get tape alert from the device.
 * This is a locking wrapper for tape_get_tape_alert().
 * Must be called with a lock held on the volume and no lock held on the device.
 */
int ltfs_get_tape_alert_unlocked(uint64_t *tape_alert, struct ltfs_volume *vol)
{
	int ret = 0;

	CHECK_ARG_NULL(tape_alert, -LTFS_NULL_ARG);
	CHECK_ARG_NULL(vol, -LTFS_NULL_ARG);

	if (vol->device) {
		ret = tape_device_lock(vol->device);
		if (ret < 0) {
			ltfsmsg(LTFS_ERR, "12010E", __FUNCTION__);
			return ret;
		}

		ret = tape_test_unit_ready(vol->device);
		if (ret < 0) {
			if (NEED_REVAL(ret))
				tape_start_fence(vol->device);
			else if (IS_UNEXPECTED_MOVE(ret))
				vol->reval = -LTFS_REVAL_FAILED;
			tape_device_unlock(vol->device);
			return ret;
		}

		ret = tape_get_tape_alert(vol->device, &vol->tape_alert);
		if (NEED_REVAL(ret))
			tape_start_fence(vol->device);
		else if (IS_UNEXPECTED_MOVE(ret))
			vol->reval = -LTFS_REVAL_FAILED;
		memcpy(tape_alert, &vol->tape_alert, sizeof(uint64_t));
		tape_device_unlock(vol->device);
	} else
		memcpy(tape_alert, &vol->tape_alert, sizeof(uint64_t));

	return ret;
}

/**
 * Clear latched tape alert flag in the backend (Write clear)
 * This is a locking wrapper for tape_get_tape_alert().
 * Must be called with a lock held on the volume and no lock held on the device.
 */
int ltfs_clear_tape_alert(uint64_t tape_alert, struct ltfs_volume *vol)
{
	int ret = 0;

	CHECK_ARG_NULL(tape_alert, -LTFS_NULL_ARG);
	CHECK_ARG_NULL(vol, -LTFS_NULL_ARG);

	if (vol->device) {
		ret = tape_device_lock(vol->device);
		if (ret < 0) {
			ltfsmsg(LTFS_ERR, "12010E", __FUNCTION__);
			return ret;
		}

		ret = tape_clear_tape_alert(vol->device, tape_alert);
		if (NEED_REVAL(ret))
			tape_start_fence(vol->device);
		else if (IS_UNEXPECTED_MOVE(ret))
			vol->reval = -LTFS_REVAL_FAILED;
		tape_device_unlock(vol->device);
	} else
		vol->tape_alert &= ~tape_alert;

	return ret;
}

/**
 * Get current appned point of DP
 * Must be called with a lock held on the volume and no lock held on the device.
 */
int ltfs_get_append_position(uint64_t *pos, struct ltfs_volume *vol)
{
	int ret = 0;

	CHECK_ARG_NULL(pos, -LTFS_NULL_ARG);
	CHECK_ARG_NULL(vol, -LTFS_NULL_ARG);
	CHECK_ARG_NULL(vol->index, -LTFS_NULL_ARG);

	*pos = 0;

	if (vol->device) {
		ret = tape_get_append_position(vol->device, ltfs_part_id2num(ltfs_dp_id(vol), vol), pos);
		if (*pos == 0) {
			if (vol->index->selfptr.partition == ltfs_dp_id(vol))
				*pos = vol->index->selfptr.block;
			else
				*pos = vol->index->backptr.block;
		}
	} else {
		if (vol->index->selfptr.partition == ltfs_dp_id(vol))
			*pos = vol->index->selfptr.block;
		else
			*pos = vol->index->backptr.block;
	}

	return ret;
}

/**
 * Get vendor unique (backend unique) xattribute
 * This is a locking wrapper for tape_get_vendorunique_xattr().
 * Must be called with a lock held on the volume and no lock held on the device.
 */
int ltfs_get_vendorunique_xattr(const char *name, char **buf, struct ltfs_volume *vol)
{
	int ret = 0;

	CHECK_ARG_NULL(name, -LTFS_NULL_ARG);
	CHECK_ARG_NULL(vol, -LTFS_NULL_ARG);

	if (vol->device) {
		ret = tape_device_lock(vol->device);
		if (ret < 0) {
			ltfsmsg(LTFS_ERR, "12010E", __FUNCTION__);
			return ret;
		}

		ret = tape_get_vendorunique_xattr(vol->device, name, buf);
		if (NEED_REVAL(ret))
			tape_start_fence(vol->device);
		else if (IS_UNEXPECTED_MOVE(ret))
			vol->reval = -LTFS_REVAL_FAILED;
		tape_device_unlock(vol->device);
	} else {
		ret = asprintf(buf, "Not Mounted");
		if (ret < 0)
			ret = -LTFS_NO_MEMORY;
		else
			ret = 0;
	}

	return ret;
}

/**
 * Set vendor unique (backend unique) xattribute
 * This is a locking wrapper for tape_set_vendorunique_xattr().
 * Must be called with a lock held on the volume and no lock held on the device.
 */
int ltfs_set_vendorunique_xattr(const char *name, const char *value, size_t size, struct ltfs_volume *vol)
{
	int ret = 0;

	CHECK_ARG_NULL(name, -LTFS_NULL_ARG);
	CHECK_ARG_NULL(vol, -LTFS_NULL_ARG);

	if (vol->device) {
		ret = tape_device_lock(vol->device);
		if (ret < 0) {
			ltfsmsg(LTFS_ERR, "12010E", __FUNCTION__);
			return ret;
		}

		ret = tape_set_vendorunique_xattr(vol->device, name, value, size);
		if (NEED_REVAL(ret))
			tape_start_fence(vol->device);
		else if (IS_UNEXPECTED_MOVE(ret))
			vol->reval = -LTFS_REVAL_FAILED;
		tape_device_unlock(vol->device);
	} else
		ret = LTFS_NO_DEVICE;

	return ret;
}

/**
 * Get the block size of a volume.
 * @param vol LTFS volume.
 * @return The block size on success, or 0 if vol is NULL or invalid.
 */
unsigned long ltfs_get_blocksize(struct ltfs_volume *vol)
{
	int ret;
	unsigned long blocksize;

	CHECK_ARG_NULL(vol, 0);

	ret = ltfs_get_volume_lock(false, vol);
	if (ret < 0)
		return 0;
	if (! vol->label) {
		releaseread_mrsw(&vol->lock);
		return LTFS_DEFAULT_BLOCKSIZE;
	}
	blocksize = vol->label->blocksize;
	releaseread_mrsw(&vol->lock);

	if (!blocksize)
		blocksize = LTFS_DEFAULT_BLOCKSIZE;

	return blocksize;
}

bool ltfs_get_compression(struct ltfs_volume *vol)
{
	int ret;
	bool compression;

	CHECK_ARG_NULL(vol, false);

	ret = ltfs_get_volume_lock(false, vol);
	if (ret < 0)
		return false;
	if (! vol->label) {
		releaseread_mrsw(&vol->lock);
		return false;
	}
	compression = vol->label->enable_compression;
	releaseread_mrsw(&vol->lock);
	return compression;
}

struct ltfs_timespec ltfs_get_format_time(struct ltfs_volume *vol)
{
	int err;
	struct ltfs_timespec ret;
	memset(&ret, 0, sizeof(ret));

	CHECK_ARG_NULL(vol, ret);

	err = ltfs_get_volume_lock(false, vol);
	if (err < 0)
		return ret;
	if (! vol->label) {
		releaseread_mrsw(&vol->lock);
		return ret;
	}
	ret = vol->label->format_time;
	releaseread_mrsw(&vol->lock);
	return ret;
}

/**
 * Get volume file count.
 * @param vol LTFS volume.
 * @return File count on success (may be 0) or 0 if vol is NULL or invalid.
 */
uint64_t ltfs_get_file_count(struct ltfs_volume *vol)
{
	uint64_t ret;
	int err;
	CHECK_ARG_NULL(vol, 0);
	err = ltfs_get_volume_lock(false, vol);
	if (err < 0)
		return 0;
	if (! vol->index) {
		releaseread_mrsw(&vol->lock);
		return 0;
	}
	ltfs_mutex_lock(&vol->index->dirty_lock);
	ret = vol->index->file_count;
	ltfs_mutex_unlock(&vol->index->dirty_lock);
	releaseread_mrsw(&vol->lock);
	return ret;
}

/**
 * Get number of valid blocks
 * @param vol LTFS volume.
 * @return Valid block count on success or 0 if vol is NULL or invalid.
 */
uint64_t ltfs_get_valid_block_count(struct ltfs_volume *vol)
{
	uint64_t ret;
	int err;

	err = ltfs_get_volume_lock(false, vol);
	if (err < 0)
		return 0;

	ret = ltfs_get_valid_block_count_unlocked(vol);
	releaseread_mrsw(&vol->lock);

	return ret;
}

/**
 * Get number of valid blocks. Caller must hold the volume lock
 * @param vol LTFS volume.
 * @return Valid block count on success or 0 if vol is NULL or invalid.
 */
uint64_t ltfs_get_valid_block_count_unlocked(struct ltfs_volume *vol)
{
	uint64_t ret;
	CHECK_ARG_NULL(vol, 0);

	if (! vol->index)
		return 0;

	ltfs_mutex_lock(&vol->index->dirty_lock);
	ret = vol->index->valid_blocks;
	ltfs_mutex_unlock(&vol->index->dirty_lock);

	return ret;
}

/**
 * Update number of valid blocks
 * @param vol LTFS volume.
 * @param count update count to number of valid blocks
 * @return Valid block count on success or 0 if vol is NULL or invalid.
 */
int ltfs_update_valid_block_count(struct ltfs_volume *vol, int64_t c)
{
	int ret;

	ret = ltfs_get_volume_lock(false, vol);
	if (ret < 0)
		return ret;

	ret = ltfs_update_valid_block_count_unlocked(vol, c);
	releaseread_mrsw(&vol->lock);

	return ret;
}

/**
 * Update number of valid blocks. Caller must hold volume lock
 * @param vol LTFS volume.
 * @param count update count to number of valid blocks
 * @return Valid block count on success or 0 if vol is NULL or invalid.
 */
int ltfs_update_valid_block_count_unlocked(struct ltfs_volume *vol, int64_t c)
{
	CHECK_ARG_NULL(vol, -LTFS_NULL_ARG);
	CHECK_ARG_NULL(vol->index, -LTFS_NULL_ARG);

	ltfs_mutex_lock(&vol->index->dirty_lock);
	vol->index->valid_blocks += c;
	ltfs_mutex_unlock(&vol->index->dirty_lock);

	return 0;
}

unsigned int ltfs_get_index_generation(struct ltfs_volume *vol)
{
	unsigned int ret;
	int err;
	CHECK_ARG_NULL(vol, 0);
	err = ltfs_get_volume_lock(false, vol);
	if (err < 0)
		return 0;
	ret = vol->index->generation;
	releaseread_mrsw(&vol->lock);
	return ret;
}

struct ltfs_timespec ltfs_get_index_time(struct ltfs_volume *vol)
{
	struct ltfs_timespec ret;
	int err;
	memset(&ret, 0, sizeof(ret));
	CHECK_ARG_NULL(vol, ret);
	err = ltfs_get_volume_lock(false, vol);
	if (err < 0)
		return ret;
	ret = vol->index->mod_time;
	releaseread_mrsw(&vol->lock);
	return ret;
}

struct tape_offset ltfs_get_index_selfpointer(struct ltfs_volume *vol)
{
	struct tape_offset ret;
	int err;
	memset(&ret, 0, sizeof(ret));
	CHECK_ARG_NULL(vol, ret);
	err = ltfs_get_volume_lock(false, vol);
	if (err < 0)
		return ret;
	ret = vol->index->selfptr;
	releaseread_mrsw(&vol->lock);
	return ret;
}

struct tape_offset ltfs_get_index_backpointer(struct ltfs_volume *vol)
{
	struct tape_offset ret;
	int err;
	memset(&ret, 0, sizeof(ret));
	CHECK_ARG_NULL(vol, ret);
	err = ltfs_get_volume_lock(false, vol);
	if (err < 0)
		return ret;
	ret = vol->index->backptr;
	releaseread_mrsw(&vol->lock);
	return ret;
}

int ltfs_get_index_commit_message(char **msg, struct ltfs_volume *vol)
{
	char *ret = NULL;
	int err;

	CHECK_ARG_NULL(msg, -LTFS_NULL_ARG);
	CHECK_ARG_NULL(vol, -LTFS_NULL_ARG);

	err = ltfs_get_volume_lock(false, vol);
	if (err < 0)
		return err;
	if (vol->index->commit_message) {
		ret = strdup(vol->index->commit_message);
		if (! ret) {
			ltfsmsg(LTFS_ERR, "10001E", __FUNCTION__);
			releaseread_mrsw(&vol->lock);
			return -LTFS_NO_MEMORY;
		}
	}
	releaseread_mrsw(&vol->lock);

	*msg = ret;
	return 0;
}

int ltfs_get_index_creator(char **msg, struct ltfs_volume *vol)
{
	char *ret = NULL;
	int err;

	CHECK_ARG_NULL(msg, -LTFS_NULL_ARG);
	CHECK_ARG_NULL(vol, -LTFS_NULL_ARG);

	err = ltfs_get_volume_lock(false, vol);
	if (err < 0)
		return err;
	if (vol->index->creator) {
		ret = strdup(vol->index->creator);
		if (! ret) {
			ltfsmsg(LTFS_ERR, "10001E", __FUNCTION__);
			releaseread_mrsw(&vol->lock);
			return -LTFS_NO_MEMORY;
		}
	}
	releaseread_mrsw(&vol->lock);

	*msg = ret;
	return 0;
}

int ltfs_get_volume_name(char **msg, struct ltfs_volume *vol)
{
	char *ret = NULL;
	int err;

	CHECK_ARG_NULL(msg, -LTFS_NULL_ARG);
	CHECK_ARG_NULL(vol, -LTFS_NULL_ARG);

	err = ltfs_get_volume_lock(false, vol);
	if (err < 0)
		return err;
	if (vol->index->volume_name) {
		ret = strdup(vol->index->volume_name);
		if (! ret) {
			ltfsmsg(LTFS_ERR, "10001E", __FUNCTION__);
			releaseread_mrsw(&vol->lock);
			return -LTFS_NO_MEMORY;
		}
	}
	releaseread_mrsw(&vol->lock);

	*msg = ret;
	return 0;
}

int ltfs_get_index_version(struct ltfs_volume *vol)
{
	int ret;

	CHECK_ARG_NULL(vol, 0);

	ret = ltfs_get_volume_lock(false, vol);
	if (ret < 0)
		return ret;
	ltfs_mutex_lock(&vol->index->dirty_lock);
	ret = vol->index->version;
	ltfs_mutex_unlock(&vol->index->dirty_lock);
	releaseread_mrsw(&vol->lock);

	return ret;
}

/**
 * Get the active index criteria for a volume.
 * This function performs no locking because the index criteria are
 * immutable during multithreaded operation.
 * @return A const pointer to the index criteria. This pointer is owned by the volume, so it
 *         is only valid as long as the volume exists.
 */
const struct index_criteria *ltfs_get_index_criteria(struct ltfs_volume *vol)
{
	CHECK_ARG_NULL(vol, NULL);
	if (vol->index)
		return &vol->index->index_criteria;
	return NULL;
}

bool ltfs_get_criteria_allow_update(struct ltfs_volume *vol)
{
	CHECK_ARG_NULL(vol, false);
	return vol->index->criteria_allow_update;
}

/**
 * Given a logical partition ID, return the corresponding physical partition number.
 * @param id partition ID
 * @param vol LTFS volume
 * @return a small integer on success, or (tape_partition_t)-1 on failure.
 */
tape_partition_t ltfs_part_id2num(char id, struct ltfs_volume *vol)
{
	CHECK_ARG_NULL(vol, (tape_partition_t)-1);
	CHECK_ARG_NULL(vol->label, (tape_partition_t)-1);

	if (vol->label->part_num2id[0] == id)
		return 0;
	else if (vol->label->part_num2id[1] == id)
		return 1;
	else
		return (tape_partition_t)-1;
}

/**
 * Perform the first part of a mount or check operation.
 * This consists of loading the tape, reading labels from both partitions and
 * performing basic setup based on the label contents.
 * This function is called automatically by ltfs_mount(), but it may be useful to call it
 * directly before interacting with the volume in an unusual way (e.g. before an index search
 * or rollback operation).
 */
int ltfs_start_mount(bool trial, struct ltfs_volume *vol)
{
	int ret = 0;
	uint32_t tape_maxblk = 0;
	struct tc_remaining_cap cap;

	memset(&cap, 0, sizeof(struct tc_remaining_cap));

	/* load tape */
	INTERRUPTED_RETURN();
	ltfsmsg(LTFS_DEBUG, "11012D"); /* loading the tape... */
	ret = tape_load_tape(vol->device, vol->kmi_handle);
	if (ret < 0) {
		if (ret == -LTFS_UNSUPPORTED_MEDIUM)
			ltfsmsg(LTFS_ERR, "11298E");
		else {
			/*
			 * OSR
			 *
			 * Make this a warning instead of an error. We log errors to the
			 * event log and this just ends up being noise.
			 */
#ifndef HPE_mingw_BUILD		
			ltfsmsg(LTFS_ERR, "11006E");
#else
			ltfsmsg(LTFS_WARN, "11006E");
#endif			
		}
		return ret;
	}
	ltfsmsg(LTFS_DEBUG, "11007D"); /* tape is loaded */

	/* Check partition */
	ret = tape_get_capacity(vol->device, &cap);
	if (ret < 0) {
		ltfsmsg(LTFS_ERR, "17167E", ret);
		return ret;
	} else if (cap.max_p0 == (uint64_t) 0 || cap.max_p1 == (uint64_t) 0) {
		if (! trial)
			ltfsmsg(LTFS_ERR, "17168E");
		return -LTFS_NOT_PARTITIONED;
	}

	/* read labels from both partitions and compare them */
	INTERRUPTED_RETURN();
	ltfsmsg(LTFS_DEBUG, "11008D"); /* read partition labels ... */
	ret = ltfs_read_labels(trial, vol);
	if (ret < 0) {
		/* Failed to read partition labels */
		ltfsmsg(LTFS_ERR, "11009E");
		return ret;
	}

	ret = tape_set_compression(vol->device, vol->label->enable_compression);
	if (ret < 0) {
		/* Failed to set compression */
		ltfsmsg(LTFS_ERR, "11010E");
		return ret;
	}

	ret = tape_get_max_blocksize(vol->device, &tape_maxblk);
	if (ret < 0) {
		ltfsmsg(LTFS_ERR, "17195E", "mount", ret);
		return ret;
	}
	if (tape_maxblk < vol->label->blocksize) {
		/* Blocksize too large for device */
		ltfsmsg(LTFS_ERR, "11011E", vol->label->blocksize, tape_maxblk);
		return -LTFS_LARGE_BLOCKSIZE;
	}

	return 0;
}

/**
 * Read LTFS data structures from a tape, checking for consistency (and restoring it
 * if possible). This function doesn't bother locking vol->index, as it must complete before any
 * other index operations are valid.
 * @param force_full force a more thorough medium check?
 * @param deep_recovery allow fancy recovery procedures?
 * @param recover_extra If deep recovery enabled, place extra blocks in a lost&found directory?
 * @param gen The target generation to roll back mount. Specify 0 to mount read-write at the
 *            latest generation.
 * @param vol the volume to load
 * @return 0 on success or a negative value on error.
 */
int ltfs_mount(bool force_full, bool deep_recovery, bool recover_extra, bool recover_symlink,
			   unsigned short gen, struct ltfs_volume *vol)
{
	int ret = 0;
	uint64_t volume_change_ref;
	struct tc_position seekpos;
	struct ltfs_index *index = NULL;
	bool is_worm;
	bool is_worm_recovery_mount = false;
	/* TODO: is_worm_recovery_mount should be set by user via option */

	ltfsmsg(LTFS_INFO, "11005I");

	CHECK_ARG_NULL(vol, -LTFS_NULL_ARG);

	/* load tape, read indexes, set compression */
	ret = ltfs_start_mount(false, vol);
	if (ret < 0) {
		/* ltfs_start_mount() generated an appropriate error message */
		goto out_unlock;
	}

	/* prevent the original index from being freed */
	ltfs_mutex_lock(&vol->index->refcount_lock);
	index = vol->index;
	index->refcount++;
	ltfs_mutex_unlock(&vol->index->refcount_lock);

	ltfsmsg(LTFS_DEBUG, "11013D"); /* partition labels are valid */
	ltfsmsg(LTFS_DEBUG, "11014D"); /* read MAM parameters... */

	/* Check EOD status in both partitions */
	INTERRUPTED_GOTO(ret, out_unlock);
	ret = ltfs_check_eod_status(vol);
	if (!vol->skip_eod_check && !is_worm_recovery_mount) {
		if (ret < 0)
			goto out_unlock;
	}

	/* read MAM parameters. */
	INTERRUPTED_GOTO(ret, out_unlock);
	ret = tape_get_cart_coherency(vol->device, ltfs_part_id2num(vol->label->partid_ip, vol),
		&vol->ip_coh);
	if (ret != 0 || strcmp(vol->ip_coh.uuid, vol->label->vol_uuid)) {
		/* MAM parameter for index partition invalid */
		ltfsmsg(LTFS_WARN, "11016W");
		memset(&vol->ip_coh, 0, sizeof(struct tc_coherency));
	}

	ret = tape_get_cart_coherency(vol->device, ltfs_part_id2num(vol->label->partid_dp, vol),
		&vol->dp_coh);
	if (ret != 0 || strcmp(vol->dp_coh.uuid, vol->label->vol_uuid)) { /* attribute was invalid */
		/* MAM parameter for data partition invalid */
		ltfsmsg(LTFS_WARN, "11017W");
		memset(&vol->dp_coh, 0, sizeof(struct tc_coherency));
	}

	ret = tape_get_volume_change_reference(vol->device, &volume_change_ref);
	if (ret < 0 || volume_change_ref == 0 || volume_change_ref == UINT64_MAX) {
		/* MAM parameters are invalid. */
		ltfsmsg(LTFS_WARN, "11015W");
		memset(&vol->ip_coh, 0, sizeof(struct tc_coherency));
		memset(&vol->dp_coh, 0, sizeof(struct tc_coherency));
	}

	/* Don't trust version 0 MAM parameters. LTFS versions up to 1.0.1 have
	 * a bug that writes incorrect data to one partition's MAM parameter. */
	if (vol->ip_coh.version == 0 || vol->dp_coh.version == 0)
		force_full = true;

	ltfsmsg(LTFS_DEBUG, "11018D"); /* Done reading MAM parameters */

	ret = tape_get_MAMattributes(vol->device, TC_MAM_PAGE_ATTRIBUTE_ALL,
	            ltfs_part_id2num(vol->label->partid_ip, vol), &vol->mam_attr);

	ltfsmsg(LTFS_DEBUG, "17300D"); /* Done reading MAM attributes */
	ltfsmsg(LTFS_DEBUG, "11019D"); /* Checking volume consistency... */

	/* check for consistency */
	INTERRUPTED_GOTO(ret, out_unlock);
	if (! force_full && volume_change_ref > 0
		&& volume_change_ref == vol->ip_coh.volume_change_ref
		&& volume_change_ref == vol->dp_coh.volume_change_ref) {
		if (vol->ip_coh.count < vol->dp_coh.count) {
			seekpos.partition = ltfs_part_id2num(vol->label->partid_dp, vol);
			seekpos.block = vol->dp_coh.set_id;
			ret = tape_seek(vol->device, &seekpos);
			if (ret == -EDEV_EOD_DETECTED) {
				INTERRUPTED_GOTO(ret, out_unlock);
				/* MAM parameters could be corrupted, try a full consistency check */
				ltfsmsg(LTFS_INFO, "11026I");
				ret = ltfs_check_medium(true, deep_recovery, recover_extra, recover_symlink, vol);
				if (ret < 0) {
					ltfsmsg(LTFS_ERR, "11027E");
					goto out_unlock;
				}
			} else if (ret < 0) {
				ltfsmsg(LTFS_ERR, "11020E"); /* seek to DP Index failed */
				goto out_unlock;
			} else {
				INTERRUPTED_GOTO(ret, out_unlock);
				ret = ltfs_read_index(0, false, vol);
				if (ret < 0) {
					ltfsmsg(LTFS_ERR, "11021E"); /* read DP Index failed */
					goto out_unlock;
				}
				INTERRUPTED_GOTO(ret, out_unlock);
				ltfsmsg(LTFS_INFO, "11022I");
				ret = ltfs_write_index(vol->label->partid_ip, SYNC_RECOVERY, vol);
				if (ret < 0)
					goto out_unlock;
			}
		} else {
			seekpos.partition = ltfs_part_id2num(vol->label->partid_ip, vol);
			seekpos.block = vol->ip_coh.set_id;
			ret = tape_seek(vol->device, &seekpos);
			if (ret == -EDEV_EOD_DETECTED) {
				INTERRUPTED_GOTO(ret, out_unlock);
				/* MAM parameters could be corrupted, try a full consistency check */
				ltfsmsg(LTFS_INFO, "11026I");
				ret = ltfs_check_medium(true, deep_recovery, recover_extra, recover_symlink, vol);
				if (ret < 0) {
					ltfsmsg(LTFS_ERR, "11027E");
					goto out_unlock;
				}
			} else if (ret < 0) {
				ltfsmsg(LTFS_ERR, "11023E"); /* seek to IP Index failed */
				goto out_unlock;
			} else {
				INTERRUPTED_GOTO(ret, out_unlock);
				ret = ltfs_read_index(0, false, vol);
				if (ret < 0) {
					ltfsmsg(LTFS_ERR, "11024E"); /* read IP Index failed */
					goto out_unlock;
				}
				ltfsmsg(LTFS_DEBUG, "11025D"); /* volume is consistent */
			}
		}
	} else if (is_worm_recovery_mount) {
		/* Skip consistency check because of WORM recovery mount */
	} else {
		/* do a full consistency check on the medium itself */
		INTERRUPTED_GOTO(ret, out_unlock);
		ltfsmsg(LTFS_INFO, "11026I"); /* performing full medium consistency check */
		ret = ltfs_check_medium(true, deep_recovery, recover_extra, recover_symlink, vol);
		if (ret < 0) {
			ltfsmsg(LTFS_ERR, "11027E"); /* consistency check failed */
			goto out_unlock;
		}
	}

	ltfsmsg(LTFS_DEBUG, "11028D"); /* finished consistency check */



	/* Make roll back mount if necessary */
	INTERRUPTED_GOTO(ret, out_unlock);
	vol->rollback_mount = false;
	if(gen != 0 && gen != vol->index->generation) {
		if(is_worm_recovery_mount){
			ret = ltfs_traverse_index_no_eod(vol, ltfs_ip_id(vol), gen, NULL, NULL, NULL);
			if(ret < 0)
				ret = ltfs_traverse_index_no_eod(vol, ltfs_dp_id(vol), gen, NULL, NULL, NULL);
		} else if(vol->traverse_mode == TRAVERSE_FORWARD){
			ret = ltfs_traverse_index_forward(vol, ltfs_ip_id(vol), gen, NULL, NULL, NULL);
			if(ret < 0)
				ret = ltfs_traverse_index_forward(vol, ltfs_dp_id(vol), gen, NULL, NULL, NULL);
		} else {
			ret = ltfs_traverse_index_backward(vol, ltfs_ip_id(vol), gen, NULL, NULL, NULL);
			if(ret < 0)
				ret = ltfs_traverse_index_backward(vol, ltfs_dp_id(vol), gen, NULL, NULL, NULL);
		}
		if (ret < 0) {
			ltfsmsg(LTFS_ERR, "17079E", gen);
			goto out_unlock;
		} else {
			vol->rollback_mount = true;
			ltfs_unset_index_dirty(false, vol->index);
			tape_force_read_only(vol->device);
			goto out_unlock;
		}
	}

	/* set append position for index partition */
	INTERRUPTED_GOTO(ret, out_unlock);
	ret = tape_get_worm_status(vol->device, &is_worm);
	if (ret == 0 && is_worm) {
		ret = tape_set_append_position(vol->device, ltfs_part_id2num(ltfs_ip_id(vol), vol), 0);
	}
	else {
		ret = tape_set_append_position(
			vol->device, ltfs_part_id2num(ltfs_ip_id(vol), vol), vol->index->selfptr.block - 1);
	}

	if (ret < 0) {
		ltfsmsg(LTFS_ERR, "11029E"); /* failed to save append position for IP */
		goto out_unlock;
	}

	/* Issue a warning if the UID space is exhausted: new create/mkdir requests will be rejected */
	if (vol->index->uid_number == 0)
		ltfsmsg(LTFS_WARN, "11307W", vol->label->vol_uuid);

	/* Clear the commit message so it doesn't carry over from the previous session */
	/* TODO: is this the right place to clear the commit message? */
	if (vol->index->commit_message) {
		free(vol->index->commit_message);
		vol->index->commit_message = NULL;
	}

	/* If we reach this point, both partitions end in an index file. */
	vol->ip_index_file_end = true;
	vol->dp_index_file_end = true;

	/* load tape attribute from Cartridge Memory*/
	/*if (!vol->t_attr)
		ret = ltfs_load_all_attributes(vol);*/

out_unlock:
	if (index && vol->index)
		ltfs_index_free(&index);
	else if (index && !vol->index)
		vol->index = index;

	return ret;
}

/**
 * Load cartridge attribute varues from CM
 * @param vol the volume to get attribute
 */
#if 0
int ltfs_load_all_attributes(struct ltfs_volume *vol)
{
	int ret = 0;

	if (!vol->t_attr) {
		/* load tape attribute from Cartridge Memory*/
		vol->t_attr = (struct tape_attr *) calloc(1, sizeof(struct tape_attr));
		if (! vol->t_attr) {
			ltfsmsg(LTFS_ERR, "10001E", "ltfs_load_all_attribute: vol->t_attr");
			ret = -LTFS_NO_MEMORY;
		} else
			tape_load_all_attribute_from_cm(vol->device, vol->t_attr);
	}

	return ret;
}
#endif /* 0 */

/**
 * Set the dirty or atime_dirty bit in an index.
 * This also upgrades the index's version number to the latest version.
 * @param locking True to take idx->dirty_lock, false if that lock is already held.
 * @param atime True to set the atime_dirty flag, false to set the dirty flag.
 * @param idx Index to modify.
 */
void ltfs_set_index_dirty(bool locking, bool atime, struct ltfs_index *idx)
{
	bool was_dirty;
	if (idx) {
		if (locking)
			ltfs_mutex_lock(&idx->dirty_lock);
		was_dirty = idx->dirty;
		if (atime)
			idx->atime_dirty = true;
		else
			idx->dirty = true;
		if (! atime || (atime && idx->use_atime))
			idx->version = LTFS_INDEX_VERSION;
		if (!was_dirty && idx->dirty && dcache_initialized(idx->root->vol))
				dcache_set_dirty(true, idx->root->vol);
		if (locking)
			ltfs_mutex_unlock(&idx->dirty_lock);
	}
}

/**
 * Unset the dirty flags for an index, optionally upgrading the index's version field.
 * @param update_version True to force the index's version number to the current version. This
 *                       flag should be used just after writing an index.
 * @param idx Index to modify.
 */
void ltfs_unset_index_dirty(bool update_version, struct ltfs_index *idx)
{
	bool was_dirty = false;
	if (idx) {
		ltfs_mutex_lock(&idx->dirty_lock);
		was_dirty = idx->dirty;
		idx->dirty = false;
		idx->atime_dirty = false;
		if (was_dirty && dcache_initialized(idx->root->vol))
				dcache_set_dirty(false, idx->root->vol);
		if (update_version)
			idx->version = LTFS_INDEX_VERSION;
		ltfs_mutex_unlock(&idx->dirty_lock);
	}
}

/**
 * Make cartridge consistent and close the associated device.
 * The current index file must reside on the index partition, so write that index file
 * if needed.
 * This function doesn't bother locking vol->index, as it will be called after all other index
 * operations are complete.
 */
int ltfs_unmount(char *reason, struct ltfs_volume *vol)
{
	int ret;
	cartridge_health_info h;

	ltfsmsg(LTFS_DEBUG, "11032D"); /* Unmount the volume... */

start:
	ret = ltfs_get_volume_lock(true, vol);
	if (ret == 0) {
		if (vol->rollback_mount == false &&
			 (ltfs_is_dirty(vol) || vol->index->selfptr.partition != ltfs_ip_id(vol))) {
			/* If write error is set in MAM we are not writing the index in both partitions while unmounting
			 * This is done to prevent writing the index when a cartridge with PWE set is mounted as read-only
			 * with the latest index from either partition(For rollback mount same thing is done)*/
			tape_get_MAMattributes(vol->device,
								   TC_MAM_VOL_LOCK_STATE,
								   ltfs_part_id2num(vol->label->partid_ip, vol),
								   &vol->mam_attr);
			if ( (vol->mam_attr.volumelockstate != PWE_MAM) && 
                 (vol->mam_attr.volumelockstate != DPPWE_MAM) && 
                 (vol->mam_attr.volumelockstate != IPPWE_MAM) &&
                 (vol->mam_attr.volumelockstate != DP_IP_PWE_MAM) )
				ret = ltfs_write_index(ltfs_ip_id(vol), reason, vol);
			if (NEED_REVAL(ret)) {
				ret = ltfs_revalidate(true, vol);
				if (ret == 0) {
					releasewrite_mrsw(&vol->lock);
					goto start;
				} else {
					ltfsmsg(LTFS_ERR, "11033E");
					/* Reset revalidation flag to allow future mount attempts */
					ltfs_thread_mutex_lock(&vol->reval_lock);
					vol->reval = 0;
					ltfs_thread_mutex_unlock(&vol->reval_lock);
					releasewrite_mrsw(&vol->lock);
					return ret;
				}
			} else if (ret < 0) {
				if (IS_UNEXPECTED_MOVE(ret))
					vol->reval = -LTFS_REVAL_FAILED;
				ltfsmsg(LTFS_ERR, "11033E"); /* could not unmount, failed to write Index */
				releasewrite_mrsw(&vol->lock);
				return ret;
			}
		}
	} else {
		/* could not unmount */
		return ret;
	}

	ltfs_thread_mutex_lock(&vol->reval_lock);
	vol->reval = 0;
	ltfs_thread_mutex_unlock(&vol->reval_lock);

	/* Update cartridge health cache */
	ret = ltfs_get_cartridge_health(&h, vol);
	if (NEED_REVAL(ret)) /* Ignore this case, just need to release the fence */
		tape_release_fence(vol->device);

	releasewrite_mrsw(&vol->lock);

	ltfsmsg(LTFS_INFO, "11034I"); /* unmount successful */
	return 0;
}

/**
 * Dump dentry tree.
 * caller must hold volume lock
 */
void ltfs_dump_tree_unlocked(struct ltfs_index *index)
{
	if (index && index->root) {
		/* dump the tree */
		printf("*** FILESYSTEM DUMP ***\n");
		fs_dump_tree(index->root);
		printf("***********************\n");
	}
}

/**
 * Dump dentry tree.
 * caller must hold volume lock
 */
void ltfs_dump_tree(struct ltfs_volume *vol)
{
	int ret;

	ret = ltfs_get_volume_lock(true, vol);
	if (ret == 0) {
		ltfs_dump_tree_unlocked(vol->index);
		releasewrite_mrsw(&vol->lock);
	}
}

/**
 * Returns true if dirty bit is set, or if atime updates are enabled and atime_dirty is set.
 */
bool ltfs_is_dirty(struct ltfs_volume *vol)
{
	CHECK_ARG_NULL(vol, false);
	return (vol->index->dirty || (vol->index->use_atime && vol->index->atime_dirty));
}

/**
 * Eject the cartridge associated to the LTFS volume's device
 * @param vol LTFS volume
 * @return 0 on success or a negative value on error
 */
int ltfs_eject_tape(struct ltfs_volume *vol)
{
	int ret;

	ltfsmsg(LTFS_INFO, "11289I"); /* Ejecting cartridge... */

	INTERRUPTED_RETURN();
	ret = tape_unload_tape(vol->device);
	if (ret < 0) {
		ltfsmsg(LTFS_ERR, "11290E", __FUNCTION__); /* Failed to eject the cartridge */
		return ret;
	}

	ltfsmsg(LTFS_INFO, "11291I"); /* Eject successful */
	return ret;
}

/**
 * Check whether the underlying medium for a volume's status is writable or not.
 * Convenience wrapper for tape_read_only.
 * This function returns -LTFS_WRITE_PROTECT if the medium is write-protected.
 * Returns -LTFS_WRITE_ERROR returns if a write error has previously occurred.
 * Returns -LTFS_NO_SPACE if IP or DP is in early warning zone.
 * Returns -LTFS_LESS_SPACE if DP is in programmable early warning zone.
 * Returns -LTFS_VOLUME_DP_WRITEERRLOCKED if write error in data partition.
 * Returns -LTFS_VOLUME_IP_WRITEERRLOCKED if write error in index partition.
 * @param vol LTFS volume
 * @return 0 if the device is writable or a negative value on error. In particular,
 *         -LTFS_NO_SPACE is returned if the specified partition is out of space,
 *         -LTFS_LESS_SPACE is returned if there is no space to create or update file,
 *         -LTFS_WRITE_PROTECT is returned if the medium is write protected and
 *         -LTFS_WRITE_ERROR is returned if a write error has previously occurred.
 *         Other negative values indicate an operational problem (could not get the read-only
 *         status).
 *
  * HPE MD 25.09.2017 Added support for SNIA 2.4.
 */
int ltfs_get_tape_readonly(struct ltfs_volume *vol)
{
	int ret;
	CHECK_ARG_NULL(vol, -LTFS_NULL_ARG);

	/* Find out if the volume is locked/permanently locked or locked due to write error */
	pthread_mutex_lock(& vol->lockbits.lock_bitfield);
	if (ltfs_find_lockbit(vol, PWE)) 
	{
		ltfsmsg(LTFS_INFO, "17308I");
		pthread_mutex_unlock(& vol->lockbits.lock_bitfield);
		return -LTFS_VOLUME_WRITEERRLOCKED;
	}
	else if (ltfs_find_lockbit(vol, DPPWE)) 
	{
		ltfsmsg(LTFS_INFO, "17327I");
		pthread_mutex_unlock(& vol->lockbits.lock_bitfield);
		return -LTFS_VOLUME_DP_WRITEERRLOCKED;
	}
	else if (ltfs_find_lockbit(vol, IPPWE)) 
	{
		ltfsmsg(LTFS_INFO, "17328I");
		pthread_mutex_unlock(& vol->lockbits.lock_bitfield);
		return -LTFS_VOLUME_IP_WRITEERRLOCKED;
	} 
	else if (ltfs_find_lockbit(vol, LOCKED)) 
	{
		ltfsmsg(LTFS_INFO, "17305I");
		pthread_mutex_unlock(& vol->lockbits.lock_bitfield);
		return -LTFS_VOLUME_LOCKED;
	} 
	else if (ltfs_find_lockbit(vol, PERM_LOCKED)) 
	{
		ltfsmsg(LTFS_INFO, "17309I");
		pthread_mutex_unlock(& vol->lockbits.lock_bitfield);
		return -LTFS_VOLUME_PERMLOCKED;
	}
	pthread_mutex_unlock(& vol->lockbits.lock_bitfield);

	ret = tape_read_only(vol->device, ltfs_part_id2num(ltfs_ip_id(vol), vol));
	if (! ret || ret == -LTFS_LESS_SPACE)
		ret = tape_read_only(vol->device, ltfs_part_id2num(ltfs_dp_id(vol), vol));

	return ret;
}

/**
 * Check whether the underlying medium for a volume's status is logically writable or not.
 * Convenience wrapper for tape_logically_read_only.
 * This function returns -LTFS_LOGICAL_WRITE_PROTECT if the medium is logically write-protected i.e. incompatible medium
 * @param vol LTFS volume
 * @return 0 if the device is writable or a negative value on error. In particular,
 *         -LTFS_LOGICAL_WRITE_PROTECT is returned if the medium is logically write protected i.e. incompatible medium
 *         and Other negative values indicate an operational problem (could not get the read-only
 *         status).
 */
int ltfs_get_tape_logically_readonly(struct ltfs_volume *vol)
{
	int ret;
	CHECK_ARG_NULL(vol, -LTFS_NULL_ARG);

	ret = tape_logically_read_only(vol->device);

	return ret;
}

/*
 * Check if a cartridge is locked using the latch
 * @param vol LTFS volume
 * @ret 1 for success else 0 for failure
 */
int ltfs_get_physically_write_protected(struct ltfs_volume *vol) {
	int ret;
	CHECK_ARG_NULL(vol, -LTFS_NULL_ARG);

	ret = tape_get_physically_write_protected(vol->device);

	return ret;
}
/**
 * Check whether the specified partition has additional space to write. Also check whether
 * the tape is write-protected or not, just as ltfs_get_tape_read_only() does.
 * @param partition Partition to be checked
 * @param vol LTFS volume
 * @return 0 if the device is writable or a negative value on error. In particular,
 *         -LTFS_NO_SPACE is returned if the specified partition is out of space,
 *         -LTFS_LESS_SPACE is returned if there is no space to create or update file,
 *         -LTFS_WRITE_PROTECT is returned if the medium is write protected and
 *         -LTFS_WRITE_ERROR is returned if a write error has previously occurred.
 *         Other negative values indicate an operational problem (could not get the read-only
 *         status).
 */
int ltfs_get_partition_readonly(char partition, struct ltfs_volume *vol)
{
	CHECK_ARG_NULL(vol, -LTFS_NULL_ARG);
	if (partition != ltfs_dp_id(vol) && partition != ltfs_ip_id(vol)) {
		ltfsmsg(LTFS_ERR, "11306E");
		return -LTFS_BAD_PARTNUM;
	}

	return tape_read_only(vol->device, ltfs_part_id2num(partition, vol));
}

/**
 * Set access time behavior of a volume.
 * @param use_atime if true, atime updates dirty the index
 * @param vol LTFS volume. This function has no effect if @vol is NULL.
 */
void ltfs_use_atime(bool use_atime, struct ltfs_volume *vol)
{
	int ret;
	if (vol) {
		ret = ltfs_get_volume_lock(true, vol);
		if (ret < 0)
			return;
		vol->index->use_atime = use_atime;
		releasewrite_mrsw(&vol->lock);
	}
}

/**
 * Configure EOD (end of data) checking for a volume. This should be done before
 * calling ltfs_mount.
 * The EOD check is enabled by default.
 */
void ltfs_set_eod_check(bool use, struct ltfs_volume *vol)
{
	if (vol)
		vol->skip_eod_check = ! use;
}

/**
 * Set the index traversal mode. Used when looking for indexes.
 * @param mode Traversal mode, must be TRAVERSE_FORWARD or TRAVERSE_BACKWARD.
 * @param vol LTFS volume.
 */
void ltfs_set_traverse_mode(int mode, struct ltfs_volume *vol)
{
	if (mode != TRAVERSE_FORWARD && mode != TRAVERSE_BACKWARD) {
		ltfsmsg(LTFS_WARN, "11310W", mode);
		return;
	}
	if (vol)
		vol->traverse_mode = mode;
}

/**
 * Set a data placement policy override.
 * This should be run after ltfs_mount() but before issuing any file operations on the
 * volume.
 * @param rules Data placement policy, in the syntax understood by index_criteria.c.
 * @param permanent The value is true when this function is called from mkltfs, otherwise it is false.
 * @param vol LTFS volume.
 * @return 0 on success or a negative value on error.
 */
int ltfs_override_policy(const char *rules, bool permanent, struct ltfs_volume *vol)
{
	int ret = 0;

	CHECK_ARG_NULL(rules, -LTFS_NULL_ARG);
	CHECK_ARG_NULL(vol, -LTFS_NULL_ARG);

	if (vol->index) {
		if (! vol->index->criteria_allow_update && ! permanent)
			ret = -LTFS_POLICY_IMMUTABLE;
		else {
			ret = index_criteria_parse(rules, vol);
			if (ret == 0 && permanent) {
				ret = index_criteria_dup_rules(&vol->index->original_criteria,
					&vol->index->index_criteria);
			}
		}
	}

	return ret;
}

/**
 * Set minimum and maximum cache sizes for the I/O scheduler.
 * The sizes are in units of MiB (1048576 bytes).
 * @param min_size Starting cache size.
 * @param max_size Maximum cache size.
 * @param vol LTFS volume.
 * @return 0 on success or -LTFS_NULL_ARG if vol is NULL.
 */
int ltfs_set_scheduler_cache(size_t min_size, size_t max_size, struct ltfs_volume *vol)
{
	CHECK_ARG_NULL(vol, -LTFS_NULL_ARG);
	vol->cache_size_min = min_size;
	vol->cache_size_max = max_size;
	return 0;
}

size_t ltfs_min_cache_size(struct ltfs_volume *vol)
{
	CHECK_ARG_NULL(vol, 0);
	return vol->cache_size_min ? vol->cache_size_min : LTFS_MIN_CACHE_SIZE_DEFAULT;
}

size_t ltfs_max_cache_size(struct ltfs_volume *vol)
{
	CHECK_ARG_NULL(vol, 0);
	return vol->cache_size_max ? vol->cache_size_max : LTFS_MAX_CACHE_SIZE_DEFAULT;
}

/**
 * Write an index file to the given partition.
 * This should only be called after a successful ltfs_mount or ltfs_format,
 * when the cartridge is known to be in a sane state.
 * The caller must hold vol->lock for write if thread safety is required.
 * @param partition partition in which the schema should be written to
 * @param vol LTFS volume
 * @return 0 on success or a negative value on error
 */
int ltfs_write_index(char partition, char *reason, struct ltfs_volume *vol)
{
	int ret;
	struct tape_offset old_selfptr, old_backptr;
	struct ltfs_timespec modtime_old = { .tv_sec = 0, .tv_nsec = 0 };
	bool generation_inc = false;
	struct tc_position physical_selfptr;
	bool immed = false;

	CHECK_ARG_NULL(vol, -LTFS_NULL_ARG);

	/* Initialization. */
	memset((void *) &old_selfptr, 0, sizeof(struct tape_offset));
	memset((void *) &old_backptr, 0, sizeof(struct tape_offset));
	memset((void *) &physical_selfptr, 0, sizeof(struct tc_position));
	
	/* Check read-only status. Ignore the out-of-space condition, as this should not
	 * prevent writing an Index. */
	ret = ltfs_get_partition_readonly(ltfs_ip_id(vol), vol);
	if (! ret || ret == -LTFS_NO_SPACE || ret == -LTFS_LESS_SPACE)
		ret = ltfs_get_partition_readonly(ltfs_dp_id(vol), vol);
	if (ret < 0 && ret != -LTFS_NO_SPACE && ret != -LTFS_LESS_SPACE)
		return ret;

	/* There is no need to grab the tape device lock here. All other multithreaded users
	 * of the tape device do so with some kind of volume lock held, and this function
	 * executes with an exclusive lock on the volume. */

	/* write to data partition first if required */
	if (partition == ltfs_ip_id(vol) &&
		(! vol->dp_index_file_end ||
		 (vol->ip_index_file_end && vol->index->selfptr.partition == ltfs_ip_id(vol)))) {
		ret = ltfs_write_index(ltfs_dp_id(vol), reason, vol);
		if (NEED_REVAL(ret))
			return ret; /* can't ignore POR, tape could have changed */
		else if (IS_UNEXPECTED_MOVE(ret)) {
			vol->reval = -LTFS_REVAL_FAILED;
			return ret;
		}
		/* ignore return value: we want to keep trying even if, e.g., the DP fills up */
	}

	/* update index generation */
	if (ltfs_is_dirty(vol)) {
		modtime_old = vol->index->mod_time;
		generation_inc = true;
		get_current_timespec(&vol->index->mod_time);
		++vol->index->generation;
	}

	/* locate to append position */
	ret = tape_seek_append_position(vol->device, ltfs_part_id2num(partition, vol), partition == vol->label->partid_ip);
	if (ret < 0) {
		ltfsmsg(LTFS_ERR, "11080E", partition, ret);
		if (generation_inc) {
			vol->index->mod_time = modtime_old;
			--vol->index->generation;
		}
		return ret;
	}

	/* update back pointer */
	old_backptr = vol->index->backptr;
	if (vol->index->selfptr.partition == ltfs_dp_id(vol))
		memcpy(&vol->index->backptr, &vol->index->selfptr, sizeof(struct tape_offset));

	/* update self pointer */
	ret = tape_get_position(vol->device, &physical_selfptr);
	if (ret < 0) {
		ltfsmsg(LTFS_ERR, "11081E", ret);
		if (generation_inc) {
			vol->index->mod_time = modtime_old;
			--vol->index->generation;
		}
		vol->index->backptr = old_backptr;
		return ret;
	}
	old_selfptr = vol->index->selfptr;
	vol->index->selfptr.partition = partition;
	vol->index->selfptr.partition = vol->label->part_num2id[physical_selfptr.partition];
	vol->index->selfptr.block = physical_selfptr.block;
	++vol->index->selfptr.block; /* point to first data block, not preceding filemark */

	/* Write the Index. */
	if ((partition == ltfs_ip_id(vol)) && !vol->ip_index_file_end) {
		ret = tape_write_filemark(vol->device, 0, true, true, false);	// Flush data before writing FM
		if (ret < 0) {
			ltfsmsg(LTFS_ERR, "11326E", ret);
			if (generation_inc) {
				vol->index->mod_time = modtime_old;
				--vol->index->generation;
			}
			vol->index->backptr = old_backptr;
			vol->index->selfptr = old_selfptr;
			return ret;
		}
	}

	ret = tape_write_filemark(vol->device, 1, true, true, true);	// immediate WFM
	if (ret < 0) {
		ltfsmsg(LTFS_ERR, "11082E", ret);
		if (generation_inc) {
			vol->index->mod_time = modtime_old;
			--vol->index->generation;
		}
		vol->index->backptr = old_backptr;
		vol->index->selfptr = old_selfptr;
		return ret;
	}

	ret = xml_schema_to_tape(reason, vol);
	if (ret < 0) {
		ltfsmsg(LTFS_ERR, "11083E", ret);
		if (generation_inc) {
			vol->index->mod_time = modtime_old;
			--vol->index->generation;
		}
		vol->index->backptr = old_backptr;
		vol->index->selfptr = old_selfptr;
		return ret;
	}

	/* WFM immed @ format */
	immed = (strcmp(reason, SYNC_FORMAT) == 0);
	ret = tape_write_filemark(vol->device, 1, true, true, immed);

#if (defined HPE_BUILD) || (defined QUANTUM_BUILD) || (defined GENERIC_OEM_BUILD)
	/*
	 * Write 0 filemarks to flush the buffer to tape, to ensure that the
	 *  Volume Change Reference value has been updated before we update the
	 *  Volume Coherency Information value in the CM.  Writing zero filemarks
	 *  is a benign operation and a well known method for flushing the buffer.
	 */
	if (! ret)
		ret = tape_write_filemark (vol->device, 0, true, true, immed);
#endif

	if (ret < 0) {
		ltfsmsg(LTFS_ERR, "11084E", ret);
		if (generation_inc) {
			vol->index->mod_time = modtime_old;
			--vol->index->generation;
		}
		vol->index->backptr = old_backptr;
		vol->index->selfptr = old_selfptr;
		return ret;
	}

	/* Update MAM parameters. */
	if (partition == ltfs_ip_id(vol))
		vol->ip_index_file_end = true;
	else /* partition == ltfs_dp_id(vol) */
		vol->dp_index_file_end = true;

	/* The MAM may be inaccessible, or it may not be available on this medium. Either way,
	 * ignore failures when updating MAM parameters. */
	ltfs_update_cart_coherency(vol);

	/* Updating the MAM attribute ltfs.ApplicationFormatVersion */
	ret = tape_update_mam_attributes(vol->device, NULL, TC_MAM_APP_FORMAT_VERSION, NULL, NOLOCK_MAM);
	if (! ret) {
		ret = tape_get_MAMattributes(vol->device, TC_MAM_APP_FORMAT_VERSION,
				ltfs_part_id2num(vol->label->partid_ip, vol), &vol->mam_attr);
	}

	/* update append position */
	if (partition == ltfs_ip_id(vol)) {
		tape_set_append_position(vol->device, ltfs_part_id2num(partition, vol),
			vol->index->selfptr.block - 1);
	}

	if (dcache_initialized(NULL)) {
		dcache_set_dirty(false, vol);
		if (generation_inc) {
			dcache_set_generation(vol->index->generation, vol);
		}
	}

	ltfs_unset_index_dirty(true, vol->index);
	return 0;
}

/**
 * Write down the state of the current LTFS file system to a XML file on disk.
 * Acquires a write lock on vol->index->root->lock.
 * @param work_dir LTFS work directory.
 * @param need_gen include generation number to file name
 * @param vol LTFS volume
 * @return 0 on success or a negative value on error
 */
int ltfs_save_index_to_disk(const char *work_dir, char * reason, bool need_gen, struct ltfs_volume *vol)
{
	char *path = NULL;
	int ret = 0;
	char barcode[7] = {0};

	CHECK_ARG_NULL(work_dir, -LTFS_NULL_ARG);
	CHECK_ARG_NULL(vol, -LTFS_NULL_ARG);
	CHECK_ARG_NULL(vol->index, -LTFS_NULL_ARG);
	CHECK_ARG_NULL(vol->label, -LTFS_NULL_ARG);

	/* Write the schema to a file on disk */
#if 0
	ltfsmsg(LTFS_DEBUG, "17182D", vol->label->vol_uuid, vol->label->barcode);
	if (need_gen) {
		if (strcmp(vol->label->barcode, "      "))
			ret = asprintf(&path, "%s/%s-%d.schema", work_dir, vol->label->barcode, vol->index->generation);
		else
			ret = asprintf(&path, "%s/%s-%d.schema", work_dir, vol->label->vol_uuid, vol->index->generation);
	} else {
		if (strcmp(vol->label->barcode, "      "))
			ret = asprintf(&path, "%s/%s.schema", work_dir, vol->label->barcode);
		else
			ret = asprintf(&path, "%s/%s.schema", work_dir, vol->label->vol_uuid);
	}
#endif /* 0 */
	
	ltfsmsg(LTFS_DEBUG, "17182D", vol->label->vol_uuid, vol->mam_attr.barcode);

	if (vol->mam_attr.barcode) {
		strncpy(barcode, vol->mam_attr.barcode, 6);
		barcode[6] = '\0';
	}
	if (need_gen) {
		if (vol->mam_attr.barcode)
			ret = asprintf(&path, "%s/%s-%d.schema", work_dir,
					barcode, vol->index->generation);
		else
			ret = asprintf(&path, "%s/%s-%d.schema", work_dir,
					vol->label->vol_uuid, vol->index->generation);
	} else {
		if (vol->mam_attr.barcode)
			ret = asprintf(&path, "%s/%s.schema", work_dir,
					barcode);
		else
			ret = asprintf(&path, "%s/%s.schema", work_dir,
					vol->label->vol_uuid);
	}

	if (ret < 0) {
		ltfsmsg(LTFS_ERR, "10001E", "ltfs_save_index_to_disk: path");
		return -ENOMEM;
	}
	ret = xml_schema_to_file(path, vol->index->creator, reason, vol->index);
	if (ret < 0) {
		/* Error writing XML schema to file '%s' on disk */
		ltfsmsg(LTFS_ERR, "17183E", path);
		free(path);
		return ret;
	}

	/* Change index file's mode */
	if (chmod(path, S_IRUSR | S_IWUSR | S_IRGRP | S_IWGRP | S_IROTH | S_IWOTH)) {
		ret = -errno;
		ltfsmsg(LTFS_ERR, "17184E", errno);
	}

	free(path);
	return ret;
}

/**
 * Get the logical ID of the data partition.
 * Untraced because this is unlikely to be interesting.
 * @param vol LTFS volume
 * @return the logical ID of the data partition or 0 if an invalid handle is supplied.
 */
char ltfs_dp_id(struct ltfs_volume *vol)
{
	if (! vol || ! vol->label) {
		ltfsmsg(LTFS_WARN, "11090W");
		return 0;
	}
	return vol->label->partid_dp;
}

/**
 * Get the logical ID of the index partition.
 * Untraced because this is unlikely to be interesting.
 * @param vol LTFS volume
 * @return the logical ID of the index partition or 0 if an invalid handle is supplied.
 */
char ltfs_ip_id(struct ltfs_volume *vol)
{
	if (! vol || ! vol->label) {
		ltfsmsg(LTFS_WARN, "11091W");
		return 0;
	}
	return vol->label->partid_ip;
}

const char *ltfs_get_volume_uuid(struct ltfs_volume *vol)
{
	CHECK_ARG_NULL(vol, NULL);
	return vol->label->vol_uuid;
}

const char *ltfs_get_barcode(struct ltfs_volume *vol)
{
	CHECK_ARG_NULL(vol, NULL);

	if (vol->mam_attr.barcode)
		return vol->mam_attr.barcode;
	else
		return vol->label->barcode;
}

/**
 * Set the block size for a volume.
 * This function should only be called just prior to formatting a volume.
 * @param blocksize New block size.
 * @param vol LTFS volume.
 * @return 0 on success, -LTFS_SMALL_BLOCKSIZE or -LTFS_NULL_ARG on error.
 */
int ltfs_set_blocksize(unsigned long blocksize, struct ltfs_volume *vol)
{
	CHECK_ARG_NULL(vol, -LTFS_NULL_ARG);
	if (blocksize < LTFS_MIN_BLOCKSIZE)
		return -LTFS_SMALL_BLOCKSIZE;
	vol->label->blocksize = blocksize;
	return 0;
}

/**
 * Set compression on an LTFS volume.
 * This function should only be called just prior to formatting a volume.
 */
int ltfs_set_compression(bool enable_compression, struct ltfs_volume *vol)
{
	CHECK_ARG_NULL(vol, -LTFS_NULL_ARG);
	vol->label->enable_compression = enable_compression;
	return 0;
}

/**
 * Set barcode on an LTFS volume.
 * This function should only be called just prior to formatting a volume.
 * @return 0 on success. -LTFS_BARCODE_LENGTH, -LTFS_BARCODE_INVALID, -LTFS_NULL_ARG on error.
 */
int ltfs_set_barcode(const char *barcode, struct ltfs_volume *vol)
{
	const char *tmp = barcode;

	CHECK_ARG_NULL(vol, -LTFS_NULL_ARG);

	if (barcode && strlen(barcode) > 0) {
		if (strlen(barcode) != 6)
			return -LTFS_BARCODE_LENGTH;
		while (*tmp) {
			if ((*tmp < '0' || *tmp > '9') && (*tmp < 'A' || *tmp > 'Z'))
				return -LTFS_BARCODE_INVALID;
			++tmp;
		}
		strcpy(vol->label->barcode, barcode);
	} else
		strcpy(vol->label->barcode, "      ");
	return 0;
}

/**
 * Validate barcode.
 * @param barcode A character string which needs to be validated.
 * @return 0 on success. -LTFS_BARCODE_LENGTH, -LTFS_BARCODE_INVALID on error.
 */
int ltfs_validate_barcode(const char *barcode)
{
	const char *tmp = barcode;

	if (!barcode || strlen(barcode) == 0 || strlen(barcode) < 6 || strlen(barcode) > 32)
		return -LTFS_BARCODE_LENGTH;
	while (*tmp) {
		if ((*tmp < '0' || *tmp > '9') && (*tmp < 'A' || *tmp > 'Z'))
			return -LTFS_BARCODE_INVALID;
		++tmp;
	}

	return 0;
}

/**
 * This is used to convert lowercase charaters of barcode to uppercase.
 * @param string String whose lowercase characters are to be converted to uppercase
 * @return 0 on success. -LTFS_BARCODE_LENGTH on error
 */
int ltfs_string_toupper(char *barcode)
{
	if (!barcode || strlen(barcode) == 0)
		return -LTFS_BARCODE_LENGTH;

	while (*barcode) {
		if (*barcode >= 'a' && *barcode <= 'z') {
			*barcode = *barcode - 32;
		}
		barcode++;
	}
	
	return 0;
}

/**
 * Set or clear the volume name.
 * @param volname New volume name, may be NULL. If present, the caller is responsible for
 *                formatting the name in UTF-8 NFC, e.g. using pathname_format().
 * @param vol LTFS volume.
 * @return 0 on success or a negative value on error.
 */
int ltfs_set_volume_name(const char *volname, struct ltfs_volume *vol)
{
	int ret;
	char *name_dup = NULL;

	CHECK_ARG_NULL(vol, -LTFS_NULL_ARG);

	if (volname) {
		// HPE MD 22.09.2017 function was changed for SNIA 2.4 extra param 0 
		// will cause function to perform as before.
		ret = pathname_validate_file(volname, 0);
		if (ret < 0)
			return ret;
		name_dup = strdup(volname);
		if (! name_dup) {
			ltfsmsg(LTFS_ERR, "10001E", __FUNCTION__);
			return -LTFS_NO_MEMORY;
		}
	}

	ret = ltfs_get_volume_lock(false, vol);
	if (ret < 0) {
		if (name_dup)
			free(name_dup);
		return ret;
	}
	ltfs_mutex_lock(&vol->index->dirty_lock);
	if (vol->index->volume_name)
		free(vol->index->volume_name);

	/* LTFS VOLUME is the default Volume Name */
	if (!volname) {
		vol->index->volume_name = (char *) calloc(1,
				(strlen("LTFS VOLUME") + 1));
		strncpy(vol->index->volume_name, "LTFS VOLUME", strlen("LTFS VOLUME"));
	} else {
		if (strlen(name_dup) <= MAX_VOLUME_NAME_SIZE) {
			vol->index->volume_name = (char *) calloc(1,(strlen(name_dup) + 1));
			strncpy(vol->index->volume_name, name_dup, strlen(name_dup));
		} else {
			vol->index->volume_name = (char *) calloc(1, (MAX_VOLUME_NAME_SIZE + 1));
			strncpy(vol->index->volume_name, name_dup, MAX_VOLUME_NAME_SIZE);
		}
	}

	ltfs_set_index_dirty(false, false, vol->index);
	ltfs_mutex_unlock(&vol->index->dirty_lock);
	releaseread_mrsw(&vol->lock);
	return 0;
}

/**
 * Set volume lock state to unlocked during format (mkltfs).
 * @param vol LTFS volume.
 * @return 0 on success or a negative value on error.
 */
int ltfs_set_volume_lockstate(struct ltfs_volume *vol, mam_lockval vol_lockstate, bool isdirty)
{
	CHECK_ARG_NULL(vol, -LTFS_NULL_ARG);

	if (vol->index->volumelockstate)
		free(vol->index->volumelockstate);

	if (vol_lockstate == UNLOCKED_MAM) {
		vol->index->volumelockstate = (char *) calloc(1,
				(strlen("unlocked") + 1));
		strncpy(vol->index->volumelockstate, "unlocked", strlen("unlocked"));
	} else if (vol_lockstate == LOCKED_MAM) {
		vol->index->volumelockstate = (char *) calloc(1,
				(strlen("locked") + 1));
		strncpy(vol->index->volumelockstate, "locked", strlen("locked"));
	} else if (vol_lockstate == PERMLOCKED_MAM) {
		vol->index->volumelockstate = (char *) calloc(1,
				(strlen("permlocked") + 1));
		strncpy(vol->index->volumelockstate, "permlocked", strlen("permlocked"));
	}

	if (isdirty) {
		ltfs_set_index_dirty(true, false, vol->index);
	}
	return 0;
}

/**
 * Set the partition map for a volume.
 * Do not call this function except immediately before formatting the volume.
 * @param dp Data partition logical ID, 'a' through 'z'
 * @param ip Index partition logical ID, 'a' through 'z'
 * @param dp_num Data partition physical number, 0 or 1
 * @param ip_num Index partition physical number, 0 or 1
 * @return 0 on success, -LTFS_NULL_ARG if vol is NULL, or -LTFS_BAD_PARTNUM if the
 *         other arguments do not form a valid partition map.
 */
int ltfs_set_partition_map(char dp, char ip, int dp_num, int ip_num, struct ltfs_volume *vol)
{
	CHECK_ARG_NULL(vol, -LTFS_NULL_ARG);
	if (dp_num == ip_num || (dp_num != 0 && dp_num != 1) || (ip_num != 0 && ip_num != 1))
		return -LTFS_BAD_PARTNUM;
	if (dp < 'a' || dp > 'z' || ip < 'a' || ip > 'z' || dp == ip)
		return -LTFS_BAD_PARTNUM;
	vol->label->partid_ip = ip;
	vol->label->partid_dp = dp;
	vol->label->part_num2id[dp_num] = dp;
	vol->label->part_num2id[ip_num] = ip;
	return 0;
}

/**
 * Set reset capacity flag in a volume that forces to reset the tape medium's
 * total capacity propotion.
 * May not be functional except immediately before formatting the volume.
 * @param reset true to reset the proportion to 100%
 * @return 0 on success
 */
int ltfs_reset_capacity(bool reset, struct ltfs_volume *vol)
{
	CHECK_ARG_NULL(vol, -LTFS_NULL_ARG);
	vol->reset_capacity = reset;
	return 0;
}

/**
 * Write a label construct to a partition, based on information from an LTFS volume structure.
 * This function performs no locking, so take the tape device lock beforehand if needed.
 * @param partition Partition to write label.
 * @param vol LTFS volume to take partition information from.
 * @return 0 on success or a negative value on error.
 */
int ltfs_write_label(tape_partition_t partition, struct ltfs_volume *vol)
{
	int ret;
	struct tc_position seekpos;
	/* HPE-SOS doesn't use the CRC bits. This code is non-functional. */
#if 0
	char ansi_label[80 + LTFS_CRC_SIZE];
#endif /* 0 */
	char ansi_label[80];
	char *buf;
	xmlBufferPtr xml_buf;
	ssize_t nw;
	uint32_t idx_part;
	bool is_index_part = false;

	/* Seek to beginning of the specified partition */
	seekpos.partition = partition;
	seekpos.block = 0;
	ret = tape_seek(vol->device, &seekpos);
	if (ret < 0) {
		ltfsmsg(LTFS_ERR, "11101E", ret, partition);
		return ret;
	}
	
	// HPE MD 28.09.2017 This has probably been established above but as I am unsure of some of this
	// I would rather replicate and know I am identifying the correct partition.
   
   idx_part = ltfs_part_id2num(ltfs_ip_id(vol), vol);
   
   if (idx_part == vol->device->position.partition)
   {
      is_index_part = true;
   }

	/* Write ANSI label */
	/* HPE-SOS doesn't use the CRC bits. This code is non-functional. */
#if 0
	label_make_ansi_label(vol, ansi_label, sizeof(ansi_label) - LTFS_CRC_SIZE);
	nw = tape_write(vol->device, ansi_label, sizeof(ansi_label) - LTFS_CRC_SIZE, is_index_part, true, false);
#endif /* 0 */
	label_make_ansi_label(vol, ansi_label, sizeof(ansi_label));
	nw = tape_write(vol->device, ansi_label, sizeof(ansi_label), is_index_part, true, false);
	if (nw < 0) {
		ltfsmsg(LTFS_ERR, "11102E", (int)nw, partition);
		return nw;
	}

	ret = tape_write_filemark(vol->device, 1, true, false, true);
	if (ret < 0) {
		ltfsmsg(LTFS_ERR, "11104E", ret, partition);
		return ret;
	}

	/* write XML label */
	xml_buf = xml_make_label(vol->creator, partition, vol->label);
	if (! xml_buf) {
		ltfsmsg(LTFS_ERR, "11105E");
		return -LTFS_NO_MEMORY; /* TODO: this is the most likely error, but not the only possible one */
	}

	/* HPE-SOS doesn't use the CRC bits. This code is non-functional. */
#if 0
	buf = calloc(1, xmlBufferLength(xml_buf) + LTFS_CRC_SIZE);
#endif /* 0 */
	buf = calloc(1, xmlBufferLength(xml_buf));
	if (!buf) {
		/* Memory allocation failed */
		ltfsmsg(LTFS_ERR, "10001E", "label buffer");
		xmlBufferFree(xml_buf);
		return -LTFS_NO_MEMORY;
	}

	memcpy(buf, (void *)xmlBufferContent(xml_buf), xmlBufferLength(xml_buf));

	nw = tape_write(vol->device, buf, xmlBufferLength(xml_buf), is_index_part, true, false);
	if (nw < 0) {
		ltfsmsg(LTFS_ERR, "11106E", (int)nw, partition);
		free(buf);
		xmlBufferFree(xml_buf);
		return -nw;
	}
	free(buf);
	xmlBufferFree(xml_buf);

	ret = tape_write_filemark(vol->device, 1, true, false, true);
	if (ret < 0) {
		ltfsmsg(LTFS_ERR, "11108E", ret, partition);
		return ret;
	}

	return 0;
}

/**
 * Format a tape. This means creating 2 partitions, then writing a label and an index to each
 * partition. The caller is responsible for loading the appropriate tape backend and opening
 * the device.
 * @param vol LTFS volume. The label structure receives a new UUID and format time; all other
 *            label fields should be filled in correctly before calling this function.
 * @return 0 on success or a negative value on error.
 */
int ltfs_format_tape(struct ltfs_volume *vol)
{
	int ret;
	uint32_t tape_maxblk;

	CHECK_ARG_NULL(vol, -LTFS_NULL_ARG);

	INTERRUPTED_RETURN();

	ret = ltfs_get_partition_readonly(ltfs_ip_id(vol), vol);
	if (! ret || ret == -LTFS_NO_SPACE || ret == -LTFS_LESS_SPACE)
		ret = ltfs_get_partition_readonly(ltfs_dp_id(vol), vol);
	if (ret < 0 && ret != -LTFS_NO_SPACE && ret != -LTFS_LESS_SPACE) {
		ltfsmsg(LTFS_ERR, "11095E");
		return ret;
	}

	ret = tape_get_max_blocksize(vol->device, &tape_maxblk);
	if (ret < 0) {
		ltfsmsg(LTFS_ERR, "17195E", "format", ret);
		return ret;
	}

	if (tape_maxblk < vol->label->blocksize) {
		ltfsmsg(LTFS_ERR, "11096E", vol->label->blocksize, tape_maxblk);
		return -LTFS_LARGE_BLOCKSIZE;
	}

	/* Set up the label: generate UUID and format time */
	ltfs_gen_uuid(vol->label->vol_uuid);
	get_current_timespec(&vol->label->format_time);

	/* Duplicate creator */
	vol->label->creator = strdup(vol->creator);
	if (!vol->label->creator) {
		ltfsmsg(LTFS_ERR, "10001E", __FUNCTION__);
		return -LTFS_NO_MEMORY;
	}

	/* Set appropriate volume modification time, UUID, and root directory's uid */
	strcpy(vol->index->vol_uuid, vol->label->vol_uuid);
	vol->index->mod_time = vol->label->format_time;
	vol->index->root->creation_time = vol->index->mod_time;
	vol->index->root->change_time = vol->index->mod_time;
	vol->index->root->modify_time = vol->index->mod_time;
	vol->index->root->access_time = vol->index->mod_time;
	vol->index->root->backup_time = vol->index->mod_time;
	ltfs_set_index_dirty(true, false, vol->index);

	/* Reset capacity proportion */
	if (vol->reset_capacity) {
		ltfsmsg(LTFS_INFO, "17165I");
		ret = tape_reset_capacity(vol->device);
		if (ret < 0) {
			ltfsmsg(LTFS_ERR, "11311E", ret);
			return ret;
		}
	}

	/* Format the tape */
	INTERRUPTED_RETURN();
	ltfsmsg(LTFS_INFO, "11097I");
	ret = tape_format(vol->device, ltfs_part_id2num(vol->label->partid_ip, vol),
			vol->index->volume_name, vol->label->barcode, vol->label->vol_uuid);
	if (ret < 0) {
		ltfsmsg(LTFS_ERR, "11098E", ret);
		return ret;
	}

	ret = tape_set_compression(vol->device, vol->label->enable_compression);
	if (ret < 0) {
		ltfsmsg(LTFS_ERR, "11099E", ret);
		return ret;
	}

	if (vol->kmi_handle) {
		unsigned char *keyalias = NULL;
		unsigned char *key = NULL;
		ret = kmi_get_key(&keyalias, &key, vol->kmi_handle);
		if (ret < 0) {
			ltfsmsg(LTFS_ERR, "11314E", ret);
			return ret;
		}
		ret = tape_set_key(vol->device, keyalias, key);
		if (ret < 0) {
			ltfsmsg(LTFS_ERR, "11315E", ret);
			return ret;
		}
	}

	/* Write data partition */
	INTERRUPTED_RETURN();
	ltfsmsg(LTFS_INFO, "11100I", vol->label->partid_dp);
	ret = ltfs_write_label(ltfs_part_id2num(vol->label->partid_dp, vol), vol);
	if (ret < 0)
		return ret;
	ltfsmsg(LTFS_INFO, "11278I", vol->label->partid_dp); /* "Writing Index to ..." */
	ret = ltfs_write_index(vol->label->partid_dp, SYNC_FORMAT, vol);
	if (ret < 0) {
		ltfsmsg(LTFS_ERR, "11279E", vol->label->partid_dp, ret);
		return ret;
	}

	/* Write index partition */
	INTERRUPTED_RETURN();
	ltfsmsg(LTFS_INFO, "11100I", vol->label->partid_ip);
	ret = ltfs_write_label(ltfs_part_id2num(vol->label->partid_ip, vol), vol);
	if (ret < 0)
		return ret;
	ltfsmsg(LTFS_INFO, "11278I", vol->label->partid_ip); /* "Writing Index to ..." */
	ret = ltfs_write_index(vol->label->partid_ip, SYNC_FORMAT, vol);
	if (ret < 0) {
		ltfsmsg(LTFS_ERR, "11279E", vol->label->partid_ip, ret);
		return ret;
	}

	return 0;
}

/**
 * Unformat a tape. This means creating 1 partition. This means all data on the tape will be destroyed.
 * The caller is responsible for loading the appropriate tape backend and opening
 * the device.
 * @param vol LTFS volume.
 * @param long_wipe invoke long erase after un-partitioning
 * @return 0 on success or a negative value on error.
 */
int ltfs_unformat_tape(struct ltfs_volume *vol, bool long_wipe)
{
	int ret;

	CHECK_ARG_NULL(vol, -LTFS_NULL_ARG);

	INTERRUPTED_RETURN();
	ret = tape_load_tape(vol->device, vol->kmi_handle);
	if (ret < 0) {
		if (ret == -LTFS_UNSUPPORTED_MEDIUM)
			ltfsmsg(LTFS_ERR, "11299E");
		else
			ltfsmsg(LTFS_ERR, "11093E", ret);
		return ret;
	}

	/* Make sure it's not logically write protected i.e. incompatible medium*/
	ret = tape_logically_read_only(vol->device);
	if (ret == -LTFS_LOGICAL_WRITE_PROTECT) {
		ltfsmsg(LTFS_ERR, "11330E");
		return ret;
	}

	ret = ltfs_get_partition_readonly(ltfs_ip_id(vol), vol);
	if (! ret || ret == -LTFS_NO_SPACE || ret == -LTFS_LESS_SPACE)
		ret = ltfs_get_partition_readonly(ltfs_dp_id(vol), vol);
	if (ret < 0 && ret != -LTFS_NO_SPACE && ret != -LTFS_LESS_SPACE) {
		ltfsmsg(LTFS_ERR, "11095E");
		return ret;
	}

	/* Unformat the tape */
	INTERRUPTED_RETURN();
	ltfsmsg(LTFS_INFO, "17071I");
	ret = tape_unformat(vol->device);
	if (ret < 0) {
		ltfsmsg(LTFS_ERR, "17072E", ret);
		return ret;
	}

	INTERRUPTED_RETURN();
	if (long_wipe) {
		ltfsmsg(LTFS_INFO, "17201I");
		ret = tape_erase(vol->device, true);
		if (ret < 0) {
			ltfsmsg(LTFS_ERR, "17202E", ret);
			return ret;
		}
	}

	return 0;
}

/**
 * Wait for revalidation to complete and return the result.
 * Call this function with a lock (read or write) on the volume.
 * @param vol LTFS volume.
 * @return 0 on success, -LTFS_REVAL_FAILED if revalidation failed, or -LTFS_NULL_ARG
 *         if vol is NULL.
 */
int ltfs_wait_revalidation(struct ltfs_volume *vol)
{
	int ret;
	CHECK_ARG_NULL(vol, -LTFS_NULL_ARG);
	release_mrsw(&vol->lock);
	ltfs_thread_mutex_lock(&vol->reval_lock);
	while (vol->reval == -LTFS_REVAL_RUNNING) /* BEAM: infinite loop */
		ltfs_thread_cond_wait(&vol->reval_cond, &vol->reval_lock);
	ret = vol->reval;
	ltfs_thread_mutex_unlock(&vol->reval_lock);
	return ret;
}

/**
 * Get a read or write lock on the volume, waiting for medium
 * revalidation to finish if necessary.
 * @param exclusive Take a write lock on the volume?
 * @param vol LTFS volume.
 * @return 0 on success, -LTFS_REVAL_FAILED if the revalidation failed, or
 *         -LTFS_NULL_ARG if vol is NULL.
 */
int ltfs_get_volume_lock(bool exclusive, struct ltfs_volume *vol)
{
	int ret;
	CHECK_ARG_NULL(vol, -LTFS_NULL_ARG);

start:
	ltfs_thread_mutex_lock(&vol->reval_lock);
	while (vol->reval == -LTFS_REVAL_RUNNING) /* BEAM: infinite loop */
		ltfs_thread_cond_wait(&vol->reval_cond, &vol->reval_lock);
	ltfs_thread_mutex_unlock(&vol->reval_lock);

	if (exclusive)
		acquirewrite_mrsw(&vol->lock);
	else
		acquireread_mrsw(&vol->lock);

	ltfs_thread_mutex_lock(&vol->reval_lock);
	ret = vol->reval;
	ltfs_thread_mutex_unlock(&vol->reval_lock);

	if (ret < 0)
		release_mrsw(&vol->lock);
	if (ret == -LTFS_REVAL_RUNNING)
		goto start;
	return ret;
}

int _ltfs_revalidate_mam(struct ltfs_volume *vol)
{
	int ret;
	struct tc_coherency coh0, coh1;

	ret = tape_get_cart_coherency(vol->device, 0, &coh0);
	if (ret < 0)
		return ret;
	ret = tape_get_cart_coherency(vol->device, 1, &coh1);
	if (ret < 0)
		return ret;

	ltfsmsg(LTFS_DEBUG, "17166D", "coh0", coh0.volume_change_ref, coh0.count, coh0.set_id,
			coh0.version, coh0.uuid, vol->label->part_num2id[0]);
	ltfsmsg(LTFS_DEBUG, "17166D", "coh1", coh1.volume_change_ref, coh1.count, coh1.set_id,
			coh1.version, coh1.uuid, vol->label->part_num2id[0]);
	ltfsmsg(LTFS_DEBUG, "17166D", "IP", vol->ip_coh.volume_change_ref, vol->ip_coh.count, vol->ip_coh.set_id,
			vol->ip_coh.version, vol->ip_coh.uuid, vol->label->partid_ip);
	ltfsmsg(LTFS_DEBUG, "17166D", "DP", vol->dp_coh.volume_change_ref, vol->dp_coh.count, vol->dp_coh.set_id,
			vol->dp_coh.version, vol->dp_coh.uuid, vol->label->partid_dp);

	if (vol->label->part_num2id[0] == vol->label->partid_dp) {
		if (coh0.volume_change_ref != vol->dp_coh.volume_change_ref
			|| coh0.count != vol->dp_coh.count
			|| coh0.set_id != vol->dp_coh.set_id
			|| strcmp(coh0.uuid, vol->dp_coh.uuid)
			|| coh0.version != vol->dp_coh.version)
			return -LTFS_REVAL_FAILED;
		else if (coh1.volume_change_ref != vol->ip_coh.volume_change_ref
			|| coh1.count != vol->ip_coh.count
			|| coh1.set_id != vol->ip_coh.set_id
			|| strcmp(coh1.uuid, vol->ip_coh.uuid)
			|| coh1.version != vol->ip_coh.version)
			return -LTFS_REVAL_FAILED;
	} else {
		if (coh0.volume_change_ref != vol->ip_coh.volume_change_ref
			|| coh0.count != vol->ip_coh.count
			|| coh0.set_id != vol->ip_coh.set_id
			|| strcmp(coh0.uuid, vol->ip_coh.uuid)
			|| coh0.version != vol->ip_coh.version)
			return -LTFS_REVAL_FAILED;
		else if (coh1.volume_change_ref != vol->dp_coh.volume_change_ref
			|| coh1.count != vol->dp_coh.count
			|| coh1.set_id != vol->dp_coh.set_id
			|| strcmp(coh1.uuid, vol->dp_coh.uuid)
			|| coh1.version != vol->dp_coh.version)
			return -LTFS_REVAL_FAILED;
	}

	return 0;
}

/**
 * Revalidate the medium.
 * Call this function with a lock (read or write) on the volume.
 * @param have_write_lock Does the caller hold a write lock on the volume? If unset, a
 *                        read lock is assumed.
 * @param vol LTFS volume.
 * @return 0 on success, -LTFS_NULL_ARG if vol is NULL or has no associated device,
 *         or -LTFS_REVAL_FAILED if the medium cannot be revalidated.
 */
int ltfs_revalidate(bool have_write_lock, struct ltfs_volume *vol)
{
	int ret;
	struct ltfs_label *old_label = vol->label;
	struct tc_position pos, eod_pos;
	tape_block_t append_pos[2];
	tape_partition_t dp_num, ip_num;

	CHECK_ARG_NULL(vol, -LTFS_NULL_ARG);

	ltfsmsg(LTFS_DEBUG, "11312D");

	/* Block other libltfs operations until revalidation finishes */
	ltfs_thread_mutex_lock(&vol->reval_lock);
	vol->reval = -LTFS_REVAL_RUNNING;
	ltfs_thread_mutex_unlock(&vol->reval_lock);

	if (! have_write_lock) {
		release_mrsw(&vol->lock);
		acquirewrite_mrsw(&vol->lock);
	}

	/* Save old append positions */
	append_pos[0] = vol->device->append_pos[0];
	append_pos[1] = vol->device->append_pos[1];

	/* Set up mode pages */
	ret = ltfs_setup_device(vol);
	if (ret < 0)
		goto out;

	/* Invalidate device information cache and re-reserve the device */
	vol->device->device_reserved = false;
	vol->device->medium_locked = false;
	ret = tape_reserve_device(vol->device);
	if (ret < 0)
		goto out;

	/* Re-read labels */
	ret = label_alloc(&vol->label);
	if (ret < 0)
		goto out;

	/* This function issue load and refresh the current position */
	ret = ltfs_start_mount(false, vol);
	if (ret < 0) {
		label_free(&vol->label);
		vol->label = old_label;
		goto out;
	}

	/* Compare label to the old one. Need to fake this_partition field to prevent
	 * label_compare() from complaining. */
	vol->label->this_partition = vol->label->partid_dp;
	old_label->this_partition = vol->label->partid_ip;
	ret = label_compare(old_label, vol->label);
	label_free(&vol->label);
	vol->label = old_label;
	if (ret < 0)
		goto out;

	/* Check EOD status and MAM parameters */
	ret = ltfs_check_eod_status(vol);
	if (ret < 0)
		goto out;
	ret = _ltfs_revalidate_mam(vol);
	if (ret < 0)
		goto out;

	/* Find DP EOD */
	dp_num = ltfs_part_id2num(ltfs_dp_id(vol), vol);
	ret = tape_seek_eod(vol->device, dp_num);
	vol->device->append_pos[dp_num] = append_pos[dp_num];
	if (ret < 0)
		goto out;
	ret = tape_get_position(vol->device, &eod_pos);
	if (ret < 0)
		goto out;
	if (! vol->dp_index_file_end && vol->device->append_pos[dp_num] == 0) {
		/* No way to validate the DP. This shouldn't happen anyway */
		ret = -LTFS_REVAL_FAILED;
		goto out;
	}

	/* Check for DP index */
	if (vol->dp_index_file_end) {
		ret = tape_spacefm(vol->device, -1);
		if (ret < 0)
			goto out;
		ret = tape_get_position(vol->device, &pos);
		if (ret < 0)
			goto out;
		if (pos.block != eod_pos.block - 1) {
			/* Partition does not end in a file mark */
			ret = -LTFS_REVAL_FAILED;
			goto out;
		}

		ret = tape_spacefm(vol->device, -1);
		if (ret < 0)
			goto out;
		ret = tape_spacefm(vol->device, 1);
		if (ret < 0)
			goto out;
		ret = tape_get_position(vol->device, &pos);
		if (ret < 0)
			goto out;
		if (vol->index->selfptr.partition == ltfs_dp_id(vol) &&
			vol->index->selfptr.block != pos.block) {
			ret = -LTFS_REVAL_FAILED;
			goto out;
		} else if (vol->index->selfptr.partition != ltfs_dp_id(vol) &&
			vol->index->backptr.partition == ltfs_dp_id(vol) &&
			vol->index->backptr.block != pos.block) {
			ret = -LTFS_REVAL_FAILED;
			goto out;
		}
	}

	/* Check DP append position */
	if (vol->device->append_pos[dp_num] != 0) {
		/* Make sure the current position matches the cached append position */
		if (vol->device->append_pos[dp_num] != eod_pos.block) {
			ret = -LTFS_REVAL_FAILED;
			goto out;
		}
	}

	/* Find IP EOD */
	ip_num = ltfs_part_id2num(ltfs_ip_id(vol), vol);
	ret = tape_seek_eod(vol->device, ip_num);
	if (ret < 0)
		goto out;
	vol->device->append_pos[ip_num] = append_pos[ip_num];
	ret = tape_get_position(vol->device, &eod_pos);
	if (ret < 0)
		goto out;

	if (! vol->ip_index_file_end && vol->device->append_pos[ip_num] == 0) {
		/* No way to validate the IP. This shouldn't happen anyway */
		ret = -LTFS_REVAL_FAILED;
		goto out;
	}

	/* Check for IP index */
	if (vol->ip_index_file_end) {
		ret = tape_spacefm(vol->device, -1);
		if (ret < 0)
			goto out;
		ret = tape_get_position(vol->device, &pos);
		if (ret < 0)
			goto out;
		if (pos.block != eod_pos.block - 1) {
			/* Partition does not end in a file mark */
			ret = -LTFS_REVAL_FAILED;
			goto out;
		}

		ret = tape_spacefm(vol->device, -1);
		if (ret < 0)
			goto out;
		ret = tape_spacefm(vol->device, 1);
		if (ret < 0)
			goto out;
		ret = tape_get_position(vol->device, &pos);
		if (ret < 0)
			goto out;
		if (vol->index->selfptr.partition == ltfs_ip_id(vol) &&
			vol->index->selfptr.block != pos.block) {
			ret = -LTFS_REVAL_FAILED;
			goto out;
		}
	} else {
		ret = tape_get_position(vol->device, &pos);
		if (ret < 0)
			goto out;
	}

	/* Check IP append position */
	if (vol->device->append_pos[ip_num] != 0) {
		/* Make sure the current position matches the cached append position */
		if (vol->device->append_pos[ip_num] != pos.block - 1) {
			ret = -LTFS_REVAL_FAILED;
			goto out;
		}
	}

	ret = 0;

out:
	/* Record revalidation result and release locks */
	tape_release_fence(vol->device);
	ltfs_thread_mutex_lock(&vol->reval_lock);
	vol->reval = ret < 0 ? -LTFS_REVAL_FAILED : 0;
	ltfs_thread_cond_broadcast(&vol->reval_cond);
	ltfs_thread_mutex_unlock(&vol->reval_lock);
	releasewrite_mrsw(&vol->lock);

	if (ret < 0)
		ltfsmsg(LTFS_ERR, "11313E", ret);

	return ret;
}

/**
 * Write index to tape if the index is dirty, and if there is space available
 * on the data partition.
 * @param vol LTFS volume
 * @param index_locking Take index lock while writing an index
 * @return 0 on success or a negative value on error
 */
int ltfs_sync_index(char *reason, bool index_locking, struct ltfs_volume *vol)
{
	int ret = 0;
	bool dirty;
	char partition;
	bool dp_index_file_end, ip_index_file_end;

	CHECK_ARG_NULL(vol, -LTFS_NULL_ARG);
	CHECK_ARG_NULL(vol->index, -LTFS_NULL_ARG);

start:
	ret = ltfs_get_partition_readonly(ltfs_dp_id(vol), vol);
	if (ret < 0)
		return 0; /* Don't return the error. It's not important, and usually ignored anyway. */

	if (index_locking) {
		ret = ltfs_get_volume_lock(false, vol);
		if (ret < 0)
			return ret;
	}

	ltfs_mutex_lock(&vol->index->dirty_lock);
	dirty = vol->index->dirty;
	ltfs_mutex_unlock(&vol->index->dirty_lock);
	dp_index_file_end = vol->dp_index_file_end;
	ip_index_file_end = vol->ip_index_file_end;

	if (index_locking)
		releaseread_mrsw(&vol->lock);

	if (dirty) {
		/* Force a new XML schema to be flushed to the tape */
		ltfsmsg(LTFS_DEBUG, "17068D");
		/* If the DP ends in an index and the IP doesn't, then we're most likely positioned
		 * at the end of the IP, and writing an index there is allowed without first putting
		 * down a DP index. */
		if (dp_index_file_end && ! ip_index_file_end)
			partition = ltfs_ip_id(vol);
		else /* Otherwise, it's faster to write an index to the DP. */
			partition = ltfs_dp_id(vol);
		if (index_locking) {
			ret = ltfs_get_volume_lock(true, vol);
			if (ret < 0)
				return ret;
		}

		/*
		 * Write index with holding device lock (Added in 2013/2/25)
		 * From design point of view, this lock is not needed because all requests, file system
		 * requests and oob requests need to hold volume lock to issue a scsi command to drive.
		 * But some times we have a problem around here. So we decided to add the fail safe code
		 * for avoiding the problem.
		 */
		ret = tape_device_lock(vol->device);
		if (ret < 0) {
			ltfsmsg(LTFS_ERR, "12010E", __FUNCTION__);
			if (index_locking)
				releasewrite_mrsw(&vol->lock);
			return ret;
		}
		ret = ltfs_write_index(partition, reason, vol);
		tape_device_unlock(vol->device);

		if (IS_UNEXPECTED_MOVE(ret))
			vol->reval = -LTFS_REVAL_FAILED;

		if (index_locking && NEED_REVAL(ret)) {
			ret = ltfs_revalidate(true, vol);
			if (ret == 0)
				goto start;
		} else if (index_locking)
			releasewrite_mrsw(&vol->lock);
		if (ret)
			ltfsmsg(LTFS_ERR, "17069E");
	}

	return ret;
}

/**
 * Traverse index on a partition of EOD-less tape by forward direction
 * @param vol LTFS volume
 * @param partition partition to traverse
 * @param gen generation to search. 0 to search all
 * @param func call back function to call when an index is find. NULL not to call
 * @param list this pointer is specified as 3rd arguments of call back function
 * @param priv private data for call back function
 * @return 0 on success or a negative value on error
 */
int ltfs_traverse_index_no_eod(struct ltfs_volume *vol, char partition, unsigned int gen,
								f_index_found func, void **list, void* priv)
{
	int ret, func_ret;

	ret = tape_locate_first_index(vol->device, ltfs_part_id2num(partition, vol));
	if (ret < 0) {
		ltfsmsg(LTFS_ERR, "17082E", 'N', partition);
		return ret;
	}

	while (true) {
		ltfs_index_free(&vol->index);
		ltfs_index_alloc(&vol->index, vol);
		ret = ltfs_read_index(0, false, vol);
		if (ret < 0 && ret != -LTFS_UNSUPPORTED_INDEX_VERSION) {
			ltfsmsg(LTFS_ERR, "17075E", 'N', vol->device->position.block, partition);
			return ret;
		} else if (ret == -LTFS_UNSUPPORTED_INDEX_VERSION) {
			ret = tape_spacefm(vol->device, 1);
			if (ret < 0)
				return ret;

			vol->index->generation = -1;
			vol->index->selfptr.block = vol->device->position.block - 1;
			vol->index->selfptr.partition =
				vol->label->part_num2id[vol->device->position.partition];
		}

		ltfsmsg(LTFS_DEBUG, "17080D", 'N', vol->index->generation, partition);
		if (func) {
			func_ret = (*func)(vol, gen, list, priv);
			if(func_ret < 0) {
				ltfsmsg(LTFS_ERR, "17081E", 'N', func_ret, partition);
				return func_ret;
			} else if (func_ret > 0) /* Break if call back function returns positive value */
				return 0;
		}
		INTERRUPTED_RETURN();

		if(vol->index->generation != (unsigned int)-1 && gen != 0 && vol->index->generation >= gen)
			break;

		ret = tape_locate_next_index(vol->device);
		if (ret < 0) {
			ltfsmsg(LTFS_INFO, "17208I", ret, vol->index->generation);
			break;
		}
	}

	if(gen != 0) {
		if(vol->index->generation != gen) {
			ltfsmsg(LTFS_DEBUG, "17078D", 'N', gen, partition);
			return -LTFS_NO_INDEX;
		} else {
			ltfsmsg(LTFS_INFO, "17077I", 'N', gen, partition);
			return 0;
		}
	}

	return 0;
}

/**
 * Traverse index on a partition by forward direction
 * @param vol LTFS volume
 * @param partition partition to traverse
 * @param gen generation to search. 0 to search all
 * @param func call back function to call when an index is find. NULL not to call
 * @param list this pointer is specified as 3rd arguments of call back function
 * @param priv private data for call back function
 * @return 0 on success or a negative value on error
 */
int ltfs_traverse_index_forward(struct ltfs_volume *vol, char partition, unsigned int gen,
								f_index_found func, void **list, void* priv)
{
	int ret, func_ret;
	struct tape_offset last_index;

	ret = tape_locate_last_index(vol->device, ltfs_part_id2num(partition, vol));
	if (ret < 0) {
		ltfsmsg(LTFS_ERR, "17083E", 'F', partition);
		return ret;
	}

	last_index.partition = partition;
	last_index.block = vol->device->position.block;

	ret = tape_locate_first_index(vol->device, ltfs_part_id2num(partition, vol));
	if (ret < 0) {
		ltfsmsg(LTFS_ERR, "17082E", 'F', partition);
		return ret;
	}

	while (last_index.block >= vol->device->position.block) {
		ltfs_index_free(&vol->index);
		ltfs_index_alloc(&vol->index, vol);
		ret = ltfs_read_index(0, false, vol);
		if (ret < 0 && ret != -LTFS_UNSUPPORTED_INDEX_VERSION) {
			ltfsmsg(LTFS_ERR, "17075E", 'F', vol->device->position.block, partition);
			return ret;
		} else if (ret == -LTFS_UNSUPPORTED_INDEX_VERSION) {
			ret = tape_spacefm(vol->device, 1);
			if (ret < 0)
				return ret;
			vol->index->generation = -1;
			vol->index->selfptr.block = vol->device->position.block - 1;
			vol->index->selfptr.partition =
				vol->label->part_num2id[vol->device->position.partition];
		}

		ltfsmsg(LTFS_DEBUG, "17080D", 'F', vol->index->generation, partition);
		if (func) {
			func_ret = (*func)(vol, gen, list, priv);
			if(func_ret < 0) {
				ltfsmsg(LTFS_ERR, "17081E", 'F', func_ret, partition);
				return func_ret;
			} else if (func_ret > 0) /* Break if call back function returns positive value */
				return 0;
		}
		INTERRUPTED_RETURN();

		if(vol->index->generation != (unsigned int)-1 && gen != 0 && vol->index->generation >= gen)
			break;

		if (last_index.block > vol->device->position.block) {
			ret = tape_locate_next_index(vol->device);
			if (ret < 0) {
				ltfsmsg(LTFS_ERR, "17076E", 'F', partition);
				return ret;
			}
		}
	}

	if(gen != 0) {
		if(vol->index->generation != gen) {
			ltfsmsg(LTFS_DEBUG, "17078D", 'F', gen, partition);
			return -LTFS_NO_INDEX;
		} else {
			ltfsmsg(LTFS_INFO, "17077I", 'F', gen, partition);
			return 0;
		}
	}

	return 0;
}

/**
 * Traverse index on a partition by backward direction
 * @param vol LTFS volume
 * @param partition partition to traverse
 * @param gen generation to search. 0 to search all
 * @param func call back function to call when an index is find. NULL not to call
 * @param list this pointer is specified as 3rd arguments of call back function
 * @return 0 on success or a negative value on error
 */
int ltfs_traverse_index_backward(struct ltfs_volume *vol, char partition, unsigned int gen,
								 f_index_found func, void **list, void* priv)
{
	int ret, func_ret;

	ret = tape_locate_last_index(vol->device, ltfs_part_id2num(partition, vol));
	if (ret < 0) {
		ltfsmsg(LTFS_ERR, "17083E", 'B', partition);
		return ret;
	}

	while (1) {
		if (4 >= vol->device->position.block)
			break;

		ltfs_index_free(&vol->index);
		ltfs_index_alloc(&vol->index, vol);
		ret = ltfs_read_index(0, false, vol);
		if (ret < 0 && ret != -LTFS_UNSUPPORTED_INDEX_VERSION) {
			ltfsmsg(LTFS_ERR, "17075E", 'B', vol->device->position.block, partition);
			return ret;
		} else if (ret == -LTFS_UNSUPPORTED_INDEX_VERSION) {
			ret = tape_spacefm(vol->device, 1);
			if (ret < 0)
				return ret;
			vol->index->generation = -1;
			vol->index->selfptr.block = vol->device->position.block - 1;
			vol->index->selfptr.partition =
				vol->label->part_num2id[vol->device->position.partition];
		}

		ltfsmsg(LTFS_DEBUG, "17080D", 'B', vol->index->generation, partition);

		if (func) {
			func_ret = (*func)(vol, gen, list, priv);
			if(func_ret < 0) {
				ltfsmsg(LTFS_ERR, "17081E", 'B', func_ret, partition);
				return func_ret;
			} else if (func_ret > 0) /* Break if call back function returns positive value */
				return 0;
		}
		INTERRUPTED_RETURN();

		if(vol->index->generation != (unsigned int)-1 && gen != 0 && vol->index->generation <= gen)
			break;

		ret = tape_locate_previous_index(vol->device);
		if (ret < 0) {
			ltfsmsg(LTFS_ERR, "17076E", 'B', partition);
			return ret;
		}
	}

	if(gen != 0) {
		if(vol->index->generation != gen) {
			ltfsmsg(LTFS_DEBUG, "17078D", 'B', gen, partition);
			return -LTFS_NO_INDEX;
		} else {
			ltfsmsg(LTFS_INFO, "17077I", 'B', gen, partition);
			return 0;
		}
	}

	return 0;
}

/**
 * Check EOD status
 * @param vol LTFS volume
 * @return 0 on success or a negative value on error
 *         -LTFS_BOTH_EOD_MISSING on both EODs are missing (include CM corruption)
 *         -LTFS_EOD_MISSING_MEDIUM on EOD is missing
 *         -LTFS_UNEXPECTED_VALUE on unexpected status
 */
int ltfs_check_eod_status(struct ltfs_volume *vol)
{
	int ret = 0;
	int eod_status_ip = 0, eod_status_dp = 0;
	bool is_worm;

	eod_status_ip = tape_check_eod_status(vol->device,
										  ltfs_part_id2num(vol->label->partid_ip, vol));
	eod_status_dp = tape_check_eod_status(vol->device, ltfs_part_id2num(vol->label->partid_dp, vol));
	if(eod_status_ip == EOD_UNKNOWN || eod_status_dp == EOD_UNKNOWN) {
		/* Backend cannnot support EOD status check, print warning  */
		ltfsmsg(LTFS_WARN, "17145W");
		ltfsmsg(LTFS_INFO, "17147I");
	} else if(eod_status_ip == EOD_MISSING || eod_status_dp == EOD_MISSING) {
		ret = tape_get_worm_status(vol->device, &is_worm);

		/* EOD is missing in both or one of partitions, print message and exit */
		if(eod_status_ip == EOD_MISSING && eod_status_dp == EOD_MISSING) {
			ltfsmsg(LTFS_ERR, "17142E");
			if (is_worm) {
				ltfsmsg(LTFS_ERR, "17207E");
			}
			else {
				ltfsmsg(LTFS_ERR, "17148E");
			}
			ret = -LTFS_BOTH_EOD_MISSING;
		} else if (eod_status_ip == EOD_MISSING) {
			ltfsmsg(LTFS_ERR, "17146E", "IP", ltfs_part_id2num(vol->label->partid_ip, vol));
			if (is_worm) {
				ltfsmsg(LTFS_ERR, "17207E");
			}
			else {
				ltfsmsg(LTFS_ERR, "17148E");
			}
			ret = -LTFS_EOD_MISSING_MEDIUM;
		} else if (eod_status_dp == EOD_MISSING) {
			ltfsmsg(LTFS_ERR, "17146E", "DP", ltfs_part_id2num(vol->label->partid_dp, vol));
			if (is_worm) {
				ltfsmsg(LTFS_ERR, "17207E");
			}
			else {
				ltfsmsg(LTFS_ERR, "17148E");
			}
			ret = -LTFS_EOD_MISSING_MEDIUM;
		} else {
			ltfsmsg(LTFS_ERR, "17126E", eod_status_ip, eod_status_dp);
			ret = -LTFS_UNEXPECTED_VALUE;
		}
	}

	return ret;
}

/**
 * Detect final record number of DP
 * @param vol LTFS volume
 * @param pos pointer to position structure.
 *            When this function is successed, this stricture shows the final record of IP.
 * @return 0 on success or a negative value on error.
 */
static int _ltfs_detect_final_rec_dp(struct ltfs_volume *vol, struct tc_position *pos)
{
	tape_block_t end_pos, index_end_pos;
	bool fm_after, blocks_after;
	struct tc_position seekpos;
	int ret;
	unsigned int ip_coh_gen = vol->ip_coh.count;
	unsigned int dp_coh_gen = vol->dp_coh.count;

	/* Read the final index of IP */
	INTERRUPTED_RETURN();
	ltfsmsg(LTFS_INFO, "17114I");
	ret = ltfs_seek_index(vol->label->partid_ip, &end_pos, &index_end_pos,
						  &fm_after, &blocks_after, false, vol);
	if (ret < 0) {
		ltfsmsg(LTFS_ERR, "17115E");
		return ret;
	}

	/* Compare self pointer and a pointer in MAM */
	if (vol->index->generation == ip_coh_gen &&
		vol->index->generation == dp_coh_gen) {
		/*
		 * MAM points Index partition, Locate to the back pointer of IP and
		 * read the index pointed to the back pointer
		 */
		seekpos.block = vol->index->backptr.block;
		seekpos.partition = ltfs_part_id2num(vol->index->backptr.partition, vol);
	} else if (dp_coh_gen == ip_coh_gen &&
			   vol->index->generation != ip_coh_gen) {
		/*
		 * MAM points Data partition, Locate to the position pointed to MAM and
		 * read the index
		 */
		seekpos.block = vol->ip_coh.set_id;
		seekpos.partition = ltfs_part_id2num(vol->label->partid_dp, vol);
	} else {
		ltfsmsg(LTFS_ERR, "17123E",
				vol->index->generation,
				ip_coh_gen,
				dp_coh_gen);
		return -LTFS_UNEXPECTED_VALUE;
	}

	INTERRUPTED_RETURN();
	ltfsmsg(LTFS_INFO, "17118I", "DP", seekpos.partition, seekpos.block);
	ret = tape_seek(vol->device, &seekpos);
	if (ret < 0) {
		ltfsmsg(LTFS_ERR, "17119E", "DP",  ret);
		return ret;
	}

	INTERRUPTED_RETURN();
	ltfsmsg(LTFS_INFO, "17120I", "DP", seekpos.partition, seekpos.block);
	ret = ltfs_read_index(0, false, vol);
	if (ret < 0) {
		ltfsmsg(LTFS_ERR, "17121E", "DP",  ret);
		return ret;
	}

	return 0;
}

/**
 * Detect final record number of IP from the final index of DP
 * @param vol LTFS volume
 * @param pos pointer to position structure.
 *            When this function is successed, this stricture shows the final record of IP.
 * @return 0 on success or a negative value on error.
 */
int _ltfs_detect_final_rec_ip(struct ltfs_volume *vol, struct tc_position *pos)
{
	tape_block_t end_pos, index_end_pos, dp_last = 0, ip_last = 0;
	bool fm_after, blocks_after;
	struct tc_position seekpos;
	int ret;

	/* Detect the final record number of IP from
       the final index of DP */
	INTERRUPTED_RETURN();
	ltfsmsg(LTFS_INFO, "17116I");
	ret = ltfs_seek_index(vol->label->partid_dp, &end_pos, &index_end_pos,
						  &fm_after, &blocks_after, false, vol);
	if (ret < 0) {
		ltfsmsg(LTFS_ERR, "17117E");
		return ret;
	}

	_ltfs_last_ref(vol->index->root, &dp_last, &ip_last, vol);

	INTERRUPTED_RETURN();
	seekpos.block = ip_last;
	seekpos.partition = ltfs_part_id2num(vol->label->partid_ip, vol);
	ltfsmsg(LTFS_INFO, "17124I", "IP", seekpos.partition, seekpos.block);
	ret = tape_seek(vol->device, &seekpos);
	if (ret < 0){
		ltfsmsg(LTFS_ERR, "17125E", "DP",  ret);
		return ret;
	}

	return 0;
}

/**
 * Recover EOD information from MAM and records on the medium
 * @param vol LTFS volume. The label structure receives a new UUID and format time; all other
 *            label fields should be filled in correctly before calling this function.
 * @return 0 on success or a negative value on error.
 */
int ltfs_recover_eod(struct ltfs_volume *vol)
{
	char no_eod_part_id;
	int eod_status_ip, eod_status_dp, ret;
	bool need_verify = false;
	struct tc_position seekpos;

	ltfsmsg(LTFS_INFO, "17139I");

	/* Check EOD status in both partitions */
	INTERRUPTED_RETURN();
	eod_status_ip = tape_check_eod_status(vol->device, ltfs_part_id2num(vol->label->partid_ip, vol));
	eod_status_dp = tape_check_eod_status(vol->device, ltfs_part_id2num(vol->label->partid_dp, vol));
	if(eod_status_ip == EOD_UNKNOWN || eod_status_dp == EOD_UNKNOWN) {
		/* Backend cannnot support EOD status check */
		ltfsmsg(LTFS_ERR, "17140E");
		return -LTFS_UNSUPPORTED;
	} else if(eod_status_ip == EOD_GOOD && eod_status_dp == EOD_GOOD) {
		/* Both EODs are good, no need to perform EOD recovery */
		ltfsmsg(LTFS_INFO, "17141I");
		return 0;
	} else if(eod_status_ip == EOD_MISSING && eod_status_dp == EOD_MISSING) {
		/* Both EODs are missing, Unrecoverable */
		ltfsmsg(LTFS_ERR, "17142E");
		return -LTFS_UNSUPPORTED;
	} else if(eod_status_ip == EOD_GOOD && eod_status_dp == EOD_MISSING) {
		/* EOD of DP is missing */
		ltfsmsg(LTFS_INFO, "17143I", "DP", ltfs_part_id2num(vol->label->partid_dp, vol));
		no_eod_part_id = vol->label->partid_dp;
	} else if(eod_status_ip == EOD_MISSING && eod_status_dp == EOD_GOOD) {
		/* EOD of IP is missing */
		ltfsmsg(LTFS_INFO, "17143I", "IP", ltfs_part_id2num(vol->label->partid_ip, vol));
		no_eod_part_id = vol->label->partid_ip;
	} else {
		// Unexpected result
		ltfsmsg(LTFS_ERR, "17126E", eod_status_ip, eod_status_dp);
		return -LTFS_UNEXPECTED_VALUE;
	}

	/* Check version field in MAM */
	INTERRUPTED_RETURN();
	ret = tape_get_cart_coherency(vol->device, ltfs_part_id2num(vol->label->partid_ip, vol),
								  &vol->ip_coh);
	if (ret < 0) {
		ltfsmsg(LTFS_ERR, "17144E", "IP");
		return ret;
	}

	ret = tape_get_cart_coherency(vol->device, ltfs_part_id2num(vol->label->partid_dp, vol),
		&vol->dp_coh);
	if (ret < 0) {
		ltfsmsg(LTFS_ERR, "17144E", "DP");
		return ret;
	}

	if(vol->ip_coh.version == 0 && vol->dp_coh.version == 0){
		/* MAM is written by PGA1 or earlier */
		ltfsmsg(LTFS_INFO, "17110I");
		need_verify = true;
	} else if (vol->ip_coh.version >= 1 && vol->dp_coh.version >= 1 &&
			   vol->ip_coh.version == vol->dp_coh.version){
		/* MAM is written by PGA2 or later (includes version2) */
		ltfsmsg(LTFS_INFO, "17111I");
		need_verify = false;
	} else {
		/* Unexpected condition. Cannot support */
		ltfsmsg(LTFS_ERR, "17107E", vol->ip_coh.version, vol->dp_coh.version);
		return -LTFS_UNEXPECTED_VALUE;
	}

	/* Go to final unmount point */
	INTERRUPTED_RETURN();
	if(need_verify) {
		/* MAM points the partition which has EOD */
		if(no_eod_part_id == vol->label->partid_dp) {
			/* Go to the end of final index of corrupted data partition */
			ltfsmsg(LTFS_INFO, "17112I");
			ret = _ltfs_detect_final_rec_dp(vol, &seekpos);
		} else if(no_eod_part_id == vol->label->partid_ip) {
			/* Go to the end of final record of corrupted index partition */
			ltfsmsg(LTFS_INFO, "17112I");
			ret = _ltfs_detect_final_rec_ip(vol, &seekpos);
		} else {
			ltfsmsg(LTFS_ERR, "17108E", no_eod_part_id, no_eod_part_id);
			return -LTFS_UNEXPECTED_VALUE;
		}

		if (ret < 0) {
			ltfsmsg(LTFS_ERR, "17109E");
			return ret;
		}
	} else {
		/* Go to the end of final index of corrupted partition */
		if(no_eod_part_id == vol->label->partid_ip) {
			/*
			 * In index partition, Index will be overwritten.
			 * Locate to before the index in IP
             */
			seekpos.block     = vol->ip_coh.set_id - 1;
			seekpos.partition = ltfs_part_id2num(vol->label->partid_ip, vol);;
		} else if(no_eod_part_id == vol->label->partid_dp) {
			seekpos.block = vol->dp_coh.set_id;
			seekpos.partition = ltfs_part_id2num(vol->label->partid_dp, vol);;
		} else {
			ltfsmsg(LTFS_ERR, "17108E", no_eod_part_id, no_eod_part_id);
			return -LTFS_UNEXPECTED_VALUE;
		}
		ltfsmsg(LTFS_INFO, "17113I", seekpos.partition, seekpos.block);

		/* Locate to target and read index */
		ret = tape_seek(vol->device, &seekpos);
		if (ret < 0)
			return ret;

		if(no_eod_part_id == vol->label->partid_dp) {
			/*
			 * In index partition, Index will be overwritten.
			 * Read an index only current partition is DP.
			 */
			ret = ltfs_read_index(0, false, vol);
			if (ret < 0)
				return ret;
		}
	}

	/* Recover EOD status */
	INTERRUPTED_RETURN();
	ret = tape_recover_eod_status(vol->device, vol->kmi_handle);
	if(ret < 0) {
		ltfsmsg(LTFS_ERR, "17137E", ret);
		return ret;
	}

	ltfsmsg(LTFS_INFO, "17138I", ret);

	return 0;
}

/**
 * Allow prevent medium removal
 * @param vol LTFS volume
 * @return 0
 */
int ltfs_release_medium(struct ltfs_volume *vol)
{
	int ret = -EDEV_UNKNOWN, i;
	bool loaded = false;

	/* Check cartridge is already loaded not not */
	for(i = 0; i < 3&& ret < 0; i++) {
		ret = tape_test_unit_ready(vol->device);
	}
	loaded = (ret == 0);

	if(loaded)
		tape_unload_tape(vol->device);

	return 0;
}

/**
 * Wait the drive goes to ready state
 * @param device handle to tape device
 * @return 0 on success or a negative value on error
 */
int ltfs_wait_device_ready(struct ltfs_volume *vol)
{
	CHECK_ARG_NULL(vol, -LTFS_NULL_ARG);

	return tape_wait_device_ready(vol->device, vol->kmi_handle);
}

/**
 * Recover EOD missing status simlpy (only try to spece EOD)
 * @param vol LTFS volume. The label structure receives a new UUID and format time; all other
 *            label fields should be filled in correctly before calling this function.
 */
void ltfs_recover_eod_simple(struct ltfs_volume *vol)
{
	bool corrupted = false;
	int eod_status_ip, eod_status_dp;

	eod_status_ip = tape_check_eod_status(vol->device, ltfs_part_id2num(vol->label->partid_ip, vol));
	if (eod_status_ip == EOD_MISSING) {
		ltfsmsg(LTFS_INFO, "17161I", "IP");
		ltfsmsg(LTFS_INFO, "17162I");
		corrupted = true;
		tape_seek_eod(vol->device, ltfs_part_id2num(vol->label->partid_ip, vol));
	}

	eod_status_dp = tape_check_eod_status(vol->device, ltfs_part_id2num(vol->label->partid_dp, vol));
	if (eod_status_dp == EOD_MISSING) {
		ltfsmsg(LTFS_INFO, "17161I", "DP");
		ltfsmsg(LTFS_INFO, "17162I");
		corrupted = true;
		tape_seek_eod(vol->device, ltfs_part_id2num(vol->label->partid_dp, vol));
	}

	if (corrupted) {
	/* HPE Change - the unload is unnecessary with our drive and also causes problems */
#if !(defined(HPE_BUILD) || defined(GENERIC_OEM_BUILD) || defined(QUANTUM_BUILD))
		tape_unload_tape(vol->device);
#endif
		tape_load_tape(vol->device, vol->kmi_handle);
	}

	return;
}

/**
 * Print tape device list.
 */
int ltfs_print_device_list(struct tape_ops *ops)
{
	struct tc_drive_info *buf;
	int i, count = 0, info_count = 0, c = 0, ret = 0;

	/* Get device count */
	count = tape_get_device_list(ops, NULL, 0);
	if (count) {
		buf = (struct tc_drive_info *)calloc(count * 2, sizeof(struct tc_drive_info));
		if (! buf) {
			ltfsmsg(LTFS_ERR, "10001E", __FUNCTION__);
			ret = -LTFS_NO_MEMORY;
			return ret;
		}
		info_count = tape_get_device_list(ops, buf, count * 2);
	}

	/* Print device list */
	ltfsresult("17073I");
	c = MIN(info_count, (count * 2));
	for (i = 0; i < c; i++)
		if (buf[i].name[0] && buf[i].vendor[0] &&
				buf[i].model[0] && buf[i].serial_number[0] && buf[i].product_name[0])
			ltfsresult("17074I", buf[i].name, buf[i].vendor,
				buf[i].model, buf[i].serial_number, buf[i].product_name);
	ret = 0;

	return ret;
}

/**
 * Set livelink mode flag
 * @param vol LTFS volume. The label structure receives a new UUID and format time; all other
 *            label fields should be filled in correctly before calling this function.
 */
void ltfs_enable_livelink_mode(struct ltfs_volume *vol)
{
	vol->livelink = true;

	return;
}

/**
 * Create directory with the specified mode
 * @path path of the directory to be created
 * @mode directory creation modes
 * @return 0 on success
 */
int mkdir_p(const char *path, mode_t mode)
{
	char *ptr, buf[strlen(path) + 1];
	struct stat statbuf;
	int ret;

	sprintf(buf, "%s", path);
	ret = stat(path, &statbuf);
	if (ret == 0)
		return 0;

	/*
	 * HPE: convert forward-slash chars to back-slashes, so that the iterative mkdir works correctly
	 * on Windows
	 */
#ifdef HPE_mingw_BUILD
	ptr = buf;
	while (*ptr != '\0') {
		if (*ptr == '\\') {
			*ptr = '/';
		}
		ptr++;
	}
#endif /* HPE_mingw_BUILD */

	for (ptr = (buf[0] == '/') ? &buf[1] : buf; *ptr; ++ptr) {
		bool last = ptr[1] == '\0';
		if (*ptr == '/')
			*ptr = '\0';
		if (*ptr == '\0' || last) {
			/*
			 * OSR
			 *
			 * In our MinGW environment, mkdir takes one param
			 */
			/* TODO: No difference in the code for the two builds. Verify this. */
#ifdef HPE_mingw_BUILD
			ret = mkdir(buf, mode);
#else
			ret = mkdir(buf, mode);
#endif /* HPE_mingw_BUILD */

			if (ret && errno != EEXIST) {
				ltfsmsg(LTFS_ERR, "9014E", path, strerror(errno));
				return -1;  /* HPE: return -1 not 1 since caller treats <0 as failure */
			}
			if (!last)
				*(ptr++) = '/';
		}
	}
	return 0;
}

/**
 * This function updates the bitfield used for volume advisory locking status
 * and is called during mounting of the LTFS VOLUME
 *

 * @vol address of the LTFS volume structure
 * @bits the type of locking to be set in bitfield
 *
 * HPE MD 25.09.2017 Added support for SNIA 2.4
 */
int ltfs_update_bitfield(struct ltfs_volume *vol, set_bits bits)
{

	CHECK_ARG_NULL(vol, -LTFS_NULL_ARG);

    /* Setting up the bitfield */
    switch (bits) {

        case UNLOCKED:
            vol->lockbits.bitfield = (vol->lockbits.bitfield | UNLOCKED);

            break;
        case LOCKED:
        	vol->lockbits.bitfield =  (vol->lockbits.bitfield | LOCKED);

            break;
        case PERM_LOCKED:
        	vol->lockbits.bitfield =  (vol->lockbits.bitfield | PERM_LOCKED);

            break;
        case PHY_WRITE_PRTCT:
        	vol->lockbits.bitfield =  (vol->lockbits.bitfield | PHY_WRITE_PRTCT);

            break;
        case PRMWP:
        	vol->lockbits.bitfield =  (vol->lockbits.bitfield | PRMWP);

            break;
        case PERSWP:
        	vol->lockbits.bitfield =  (vol->lockbits.bitfield | PERSWP);

            break;
        case PWE:
        	vol->lockbits.bitfield =  (vol->lockbits.bitfield | PWE);
        	vol->lockbits.bitfield =  (vol->lockbits.bitfield | LOCKED);

            break;
        case DPPWE:
        	vol->lockbits.bitfield =  (vol->lockbits.bitfield | DPPWE);
        	vol->lockbits.bitfield =  (vol->lockbits.bitfield | LOCKED);

            break;
        case IPPWE:
        	vol->lockbits.bitfield =  (vol->lockbits.bitfield | IPPWE);
        	vol->lockbits.bitfield =  (vol->lockbits.bitfield | LOCKED);

            break;
        default:

            break;
    }

	return 0;
}

/*
 * Find out the lock state of the volume.The function should be called
 * using the vol->lockbits.lock_bitfield mutex lock and the lock should
 * only be released once the operation related to the lockbit is over
 *
 * @vol address of the LTFS volume structure
 * @lockstate lockstate to find from the bitfield
 * @return the value is TRUE if particular state is set else FALSE
 *
 * MD 25.09.2017 Added support for SNIA 2.4
 */
bool ltfs_find_lockbit(struct ltfs_volume *vol, unsigned lockstate)
{
	bool bitstate = FALSE;
	unsigned bitfield = 0x00;

	CHECK_ARG_NULL(vol, -LTFS_NULL_ARG);

	bitfield = vol->lockbits.bitfield;

	if (lockstate == UNLOCKED) 
	{
		if (! (bitfield & LOCKED))
		{   
			bitstate = TRUE;
		}	
	} 
	else if (lockstate == PWE)	
	{
		if ((bitfield & lockstate) && (bitfield & LOCKED))
	   {
			bitstate = TRUE;
		}	
	}
	else if (lockstate == DPPWE)	
	{
		if ((bitfield & lockstate) && (bitfield & LOCKED))
	   {
			bitstate = TRUE;
		}	
	}
	else if (lockstate == IPPWE)	
	{
		if ((bitfield & lockstate) && (bitfield & LOCKED))
	   {
			bitstate = TRUE;
		}	
	} 
	else 
	{
		if (bitfield & lockstate)
		{
			bitstate = TRUE;
		}	
	}

	return bitstate;
}
/**
 * This function finds out if a particular value can be set in the bitfield
 * considering various criteria
 * This function should be called using the vol->lockbits.lock_bitfield
 * mutex lock and the lock should only be released once the operation
 * related to the lockbit is over
 * @vol address of the LTFS volume structure
 * @bits the type of locking to be set in bitfield
 */
int ltfs_get_bitfield_info(struct ltfs_volume *vol, set_bits bits)
{
	int status = 0;
	unsigned bitfield = 0x00;

	CHECK_ARG_NULL(vol, -LTFS_NULL_ARG);

	bitfield = ltfs_retreive_bitfield(vol);

	switch (bits) {

		/* Check if the cartridge can be unlocked */
	    case UNLOCKED:

	    	if (! ltfs_find_lockbit(vol, PERM_LOCKED)
	    			&& ! ltfs_find_lockbit(vol, PHY_WRITE_PRTCT)
					&& ! ltfs_find_lockbit(vol, PWE)
					&& ! ltfs_find_lockbit(vol, DPPWE)
					&& ! ltfs_find_lockbit(vol, IPPWE)
					&& ! ltfs_find_lockbit(vol, PRMWP)
					&& ! ltfs_find_lockbit(vol, PERSWP)
					&& (ltfs_find_lockbit(vol, UNLOCKED) || ltfs_find_lockbit(vol, LOCKED))) {

	    		ltfsmsg(LTFS_INFO, "17310I");
	    	} else {
	    		status = -1;
	    		ltfsmsg(LTFS_ERR, "17311E",bitfield);
	    	}

	         break;
	    /* Check if the cartridge can be locked */
	    case LOCKED:

	    	if (! ltfs_find_lockbit(vol, PERM_LOCKED)
	    			&& ! ltfs_find_lockbit(vol, PHY_WRITE_PRTCT)
					&& ! ltfs_find_lockbit(vol, PWE)
					&& ! ltfs_find_lockbit(vol, DPPWE)
					&& ! ltfs_find_lockbit(vol, IPPWE)
					&& ! ltfs_find_lockbit(vol, PRMWP)
					&& ! ltfs_find_lockbit(vol, PERSWP)
					&& (ltfs_find_lockbit(vol, LOCKED) || ltfs_find_lockbit(vol, UNLOCKED))) {

	    		ltfsmsg(LTFS_INFO, "17312I");
	    	} else {
	    		status = -1;
	    		ltfsmsg(LTFS_ERR, "17313E",bitfield);
	    	}

	         break;
	    /* Check if the cartridge can be permanently locked */
	    case PERM_LOCKED:

	    	if (! ltfs_find_lockbit(vol, PHY_WRITE_PRTCT)
					&& ! ltfs_find_lockbit(vol, PWE)
					&& ! ltfs_find_lockbit(vol, DPPWE)
					&& ! ltfs_find_lockbit(vol, IPPWE)
					&& ! ltfs_find_lockbit(vol, PRMWP)
					&& ! ltfs_find_lockbit(vol, PERSWP)
					&& 	(ltfs_find_lockbit(vol, PERM_LOCKED)
						|| ltfs_find_lockbit(vol, LOCKED)
						|| ltfs_find_lockbit(vol, UNLOCKED))) {

	    		ltfsmsg(LTFS_INFO, "17314I");
	    	} else {
	    		status = -1;
	    		ltfsmsg(LTFS_ERR, "17315E",bitfield);
	    	}

	         break;
	    /* Check if the cartridge can be locked due to write error */
	    case PWE:

	    	if (! ltfs_find_lockbit(vol, PERM_LOCKED)
	    			&& ! ltfs_find_lockbit(vol, PHY_WRITE_PRTCT)
					&& ! ltfs_find_lockbit(vol, LOCKED)
					&& ! ltfs_find_lockbit(vol, PRMWP)
					&& ! ltfs_find_lockbit(vol, PERSWP)
					&& ! ltfs_find_lockbit(vol, DPPWE)
					&& ! ltfs_find_lockbit(vol, IPPWE)
					&& (ltfs_find_lockbit(vol, PWE)
					   || ltfs_find_lockbit(vol, UNLOCKED))) {
	    		ltfsmsg(LTFS_INFO, "17316I");
	    	} else {
	    		status = -1;
	    		ltfsmsg(LTFS_ERR, "17317E",bitfield);
	    	}

	         break;
	    /* The below two cases ate currently not supported so skipping the implementation*/
	    case PRMWP:

	    	 break;

	    case PERSWP:

	    	 break;
	    
	    // HPE MD 25.09.2017 The following two cases added for support of SNIA 2.4 
	    /* Check if the cartridge can be locked due to data partition write error */
	    case DPPWE:

	    	if (! ltfs_find_lockbit(vol, PERM_LOCKED)
	    			&& ! ltfs_find_lockbit(vol, PHY_WRITE_PRTCT)
					&& ! ltfs_find_lockbit(vol, LOCKED)
					&& ! ltfs_find_lockbit(vol, PRMWP)
					&& ! ltfs_find_lockbit(vol, PERSWP)
					&& ! ltfs_find_lockbit(vol, PWE)
					&& (ltfs_find_lockbit(vol, DPPWE)
					   || ltfs_find_lockbit(vol, UNLOCKED))) {
	    		ltfsmsg(LTFS_INFO, "17329I");
	    	} else {
	    		status = -1;
	    		ltfsmsg(LTFS_ERR, "17331E",bitfield);
	    	}

	         break;
	     /* Check if the cartridge can be locked due to index partition write error */
	    case IPPWE:

	    	if (! ltfs_find_lockbit(vol, PERM_LOCKED)
	    			&& ! ltfs_find_lockbit(vol, PHY_WRITE_PRTCT)
					&& ! ltfs_find_lockbit(vol, LOCKED)
					&& ! ltfs_find_lockbit(vol, PRMWP)
					&& ! ltfs_find_lockbit(vol, PERSWP)
					&& ! ltfs_find_lockbit(vol, PWE)
					&& (ltfs_find_lockbit(vol, IPPWE)
					   || ltfs_find_lockbit(vol, UNLOCKED))) {
	    		ltfsmsg(LTFS_INFO, "17330I");
	    	} else {
	    		status = -1;
	    		ltfsmsg(LTFS_ERR, "17332E",bitfield);
	    	}

	         break;    	 
	    default:

	         break;
	    }

	return status;
}

/*
 * This function sets the bitfield.
 * The function should be called using the vol->lockbits.lock_bitfield mutex
 * @vol address of the LTFS volume structure
 * @bits the type of locking to be set in bitfield
 * @return 0
 */
int ltfs_set_bitfield(struct ltfs_volume *vol, set_bits bits)
{
	CHECK_ARG_NULL(vol, -LTFS_NULL_ARG);

	switch(bits) {
		case UNLOCKED:

			vol->lockbits.bitfield = (vol->lockbits.bitfield & 0xFE);
			break;
		case LOCKED:

			vol->lockbits.bitfield = (vol->lockbits.bitfield | LOCKED);
			break;
		case PERM_LOCKED:

			vol->lockbits.bitfield = (vol->lockbits.bitfield & 0xFE);
			vol->lockbits.bitfield = (vol->lockbits.bitfield | PERM_LOCKED);
			break;
		case PWE:

			vol->lockbits.bitfield = (vol->lockbits.bitfield | PWE);
			vol->lockbits.bitfield = (vol->lockbits.bitfield | LOCKED);
			break;
		/* The below two cases are currently not supported so skipping the implementation*/
		case PRMWP:

			break;
		case PERSWP:

			break;
			
		// HPE MD 25.09.207 Added the following two conditions to support SNIA 2.4
			
		case DPPWE:

			vol->lockbits.bitfield = (vol->lockbits.bitfield | DPPWE);
			vol->lockbits.bitfield = (vol->lockbits.bitfield | LOCKED);
			break;
		case IPPWE:

			vol->lockbits.bitfield = (vol->lockbits.bitfield | IPPWE);
			vol->lockbits.bitfield = (vol->lockbits.bitfield | LOCKED);
			break;		
		default:

			break;
	}
	return 0;
}

/**
 * This function retrives the Volume Advisory Locking bitfield information
 * This function should be called using the vol->lockbits.lock_bitfield
 * mutex lock and the lock should only be released once the operation
 * related to the lockbit is over
 * @vol address of the LTFS volume structure
 * @return Volume Advisory Locking bitfield
 */
unsigned ltfs_retreive_bitfield(struct ltfs_volume *vol)
{
	unsigned bitfield = 0x00;

	bitfield = vol->lockbits.bitfield;

	return bitfield;
}

/*
 * This function will retreive the lockstate from MAM,Index,
 * device parameters and update the bitfield.
 * @vol address of the LTFS volume structure
 * @ret 0
 */
int ltfs_update_volumelockstate(struct ltfs_volume *vol) {

    unsigned char volumelockstate = 0x00;
    unsigned char temp_volumelockstate = 0xFF;
    int ret;

    CHECK_ARG_NULL(vol, -LTFS_NULL_ARG);

    /* Find out if the tape is physically or logically write protected
     * and update the bitfield accordingly
     */

    if (ltfs_get_physically_write_protected(vol))
        ltfs_update_bitfield(vol, PHY_WRITE_PRTCT);

    /* Find out from MAM and index about the lockstate */
    volumelockstate = vol->mam_attr.volumelockstate;

    // HPE MD 26.09.2017 The following added to support SNIA 2.4.  It is possible
    // that DPPWE and IPPWE could be set so need to check both and individually.

    if (volumelockstate == DPPWE_MAM)
    {
        ltfsmsg(LTFS_INFO, "17333I");
        ltfs_update_bitfield(vol, DPPWE);
    }
    else if (volumelockstate == IPPWE_MAM)
    {
        ltfsmsg(LTFS_INFO, "17334I");
        ltfs_update_bitfield(vol, IPPWE);
    }
    else if (volumelockstate == DP_IP_PWE_MAM)
    {
        ltfsmsg(LTFS_INFO, "17335I");
        ltfs_update_bitfield(vol, DPPWE);
        ltfs_update_bitfield(vol, IPPWE);
    }
    else if (volumelockstate == PWE_MAM) {

            ltfsmsg(LTFS_INFO, "17306I");
            ltfs_update_bitfield(vol, PWE);
	} else if (((vol->index->volumelockstate)
            && (!strncmp(vol->index->volumelockstate, "unlocked", strlen("unlocked"))))
            ){

        ltfs_update_bitfield(vol, UNLOCKED);
        temp_volumelockstate = UNLOCKED_MAM;
    } else if (((vol->index->volumelockstate)
            && (!strncmp(vol->index->volumelockstate, "locked", strlen("locked"))))
            ) {

        ltfsmsg(LTFS_INFO, "17305I");
        ltfs_update_bitfield(vol, LOCKED);
        temp_volumelockstate = LOCKED_MAM;
    } else if (((vol->index->volumelockstate)
            && (!strncmp(vol->index->volumelockstate, "permlocked", strlen("permlocked"))))
            ) {

        ltfsmsg(LTFS_INFO, "17307I");
        ltfs_update_bitfield(vol, PERM_LOCKED);
        temp_volumelockstate = PERMLOCKED_MAM;
    }

    if (temp_volumelockstate != 0xFF) {
        ret = tape_device_lock(vol->device);
        if (ret < 0) {
            ltfsmsg(LTFS_ERR, "12010E", __FUNCTION__);
            return ret;
        }
        ret = tape_update_mam_attributes(vol->device,
                                     NULL,
                                     TC_MAM_VOL_LOCK_STATE,
                                     NULL,
                                     temp_volumelockstate);
        tape_device_unlock(vol->device);
    }

    return 0;
}

/*
 * This function tries to find out the latest index from either partition and parse the same
 * @vol address of the LTFS volume structure
 * @return 0 for success
 */
int ltfs_mount_latest_index_either_partition(struct ltfs_volume *vol)
{
	int ret = -1, ipgen = 0, dpgen = 0;
	bool dp_have_index = false, ip_have_index = false;
	bool dp_blocks_after, ip_blocks_after;
	bool dp_fm_after, ip_fm_after;
	tape_block_t dp_eod, ip_eod, dp_endofidx = 0, ip_endofidx = 0;

	CHECK_ARG_NULL(vol, -LTFS_NULL_ARG);

	/* look for index files in the index partition*/
	ret = ltfs_seek_index(vol->label->partid_ip, &ip_eod, &ip_endofidx, &ip_fm_after, &ip_blocks_after, false, vol);
	ip_have_index = (ret == 0);
	if (ip_have_index) {
		ipgen = vol->index->generation;
		vol->index = NULL;
	}

	/* Look for index files in data partition */
	ret = ltfs_seek_index(vol->label->partid_dp, &dp_eod, &dp_endofidx, &dp_fm_after, &dp_blocks_after, false, vol);
	dp_have_index = (ret == 0);
	if (dp_have_index) {
		dpgen = vol->index->generation;
		vol->index = NULL;
	}

	/* If index is not found in either partition we need to return from here */
	if (! ip_have_index && ! dp_have_index)
		return -1;

	/* Check for the latest index to be considered for mounting and parse the same*/
	if (ipgen > dpgen) {
		ltfsmsg(LTFS_ERR, "17320I", ipgen);
		if ((ret = ltfs_traverse_index_backward(vol, ltfs_ip_id(vol), ipgen, NULL, NULL, NULL)) < 0)
			ltfsmsg(LTFS_ERR, "17079E", ipgen);

	} else {
		ltfsmsg(LTFS_ERR, "17320I", dpgen);
		if ((ret = ltfs_traverse_index_backward(vol, ltfs_dp_id(vol), dpgen, NULL, NULL, NULL)) < 0)
			ltfsmsg(LTFS_ERR, "17079E", dpgen);

	}

	return ret;
}

int ltfs_set_archivemanager_media_readonly(struct ltfs_volume *vol)
{
	int ret = 0;

	CHECK_ARG_NULL(vol, -LTFS_NULL_ARG);

	if (vol->mam_attr.appl_vendor) {
		if (strstr(vol->mam_attr.appl_vendor, "QStar") != NULL) {
			pthread_mutex_lock(&vol->device->read_only_flag_mutex);
			vol->device->write_protect = 1;
			pthread_mutex_unlock(&vol->device->read_only_flag_mutex);
			ret = 1;
		}
	}

	return ret;
}

int ltfs_get_archivemanager_media(struct ltfs_volume *vol)
{
	int ret = 0;

	CHECK_ARG_NULL(vol, -LTFS_NULL_ARG);

	if (vol->mam_attr.appl_vendor) {
		if (strstr(vol->mam_attr.appl_vendor, "QStar") != NULL) {
			ret = 1;
		}
	}

	return ret;
}
/* End of file */

