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
** FILE NAME:       kmi/simple.c
**
** DESCRIPTION:     Implements the simple key manager interface plugin.
**
** AUTHOR:          Yutaka Oishi
**                  IBM Yamato, Japan
**                  oishi@jp.ibm.com
**
*************************************************************************************
*/

#include "libltfs/kmi_ops.h"
#include "libltfs/ltfs_fuse_version.h"
#include <fuse.h>
#include "key_format_ltfs.h"

#ifdef mingw_PLATFORM
#include "libltfs/arch/win/win_util.h"
#endif

struct kmi_simple_options_data {
	unsigned char *dk;             /**< Data key */
	unsigned char *dki;            /**< Data key identifier */
	unsigned char *dk_for_format;  /**< Data key for formatting a volume */
	unsigned char *dki_for_format; /**< Data key identifier for formatting a volume */
	unsigned char *dk_list;        /**< DK and DKi pairs' list */
};

static struct kmi_simple_options_data priv;

#define KMI_SIMPLE_OPT(templ,offset,value) { templ, offsetof(struct kmi_simple_options_data, offset), value }

static struct fuse_opt kmi_simple_options[] = {
	KMI_SIMPLE_OPT("kmi_dk=%s",             dk,             0),
	KMI_SIMPLE_OPT("kmi_dki=%s",            dki,            0),
	KMI_SIMPLE_OPT("kmi_dk_for_format=%s",  dk_for_format,  0),
	KMI_SIMPLE_OPT("kmi_dki_for_format=%s", dki_for_format, 0),
	KMI_SIMPLE_OPT("kmi_dk_list=%s",        dk_list,        0),
	FUSE_OPT_END
};

/**
 * Initialize the simple key manager interface plugin.
 * @param vol LTFS volume
 * @return a pointer to the private data on success or NULL on error.
 */
void *simple_init(struct ltfs_volume *vol)
{
	return key_format_ltfs_init(vol, "15500D");
}

/**
 * Destroy the simple key manager interface plugin.
 * @param kmi_handle the key manager interface handle
 * @return 0 on success or a negative value on error.
 */
int simple_destroy(void * const kmi_handle)
{
	return key_format_ltfs_destroy(kmi_handle, "15501D");
}

/**
 * Get Key
 * @param keyalias Get key of the key-alias. If *keyalias is NULL, get key of default key-alias
 * @param key Memory is allocated and key is stored at the address.
 * @param kmi_handle the key manager interface handle
 * @return 0 on success or a negative value on error.
 */
int simple_get_key(unsigned char **keyalias, unsigned char **key, void * const kmi_handle)
{
	return key_format_ltfs_get_key(keyalias, key, kmi_handle, priv.dk_list, priv.dki_for_format);
}

/**
 * Print a help message for the simple plugin.
 */
int simple_help_message(void)
{
	ltfsresult("15608I");

	return 0;
}

static int null_parser(void *priv, const char *arg, int key, struct fuse_args *outargs)
{
	return 1;
}

/**
 * Parse simple plugin specific options.
 * @param opt_args Pointer to a FUSE argument structure, suitable for passing to
 *                 fuse_opt_parse(). See the file backend for an example of argument parsing.
 * @return 0 on success or a negative value on error.
 */
int simple_parse_opts(void *opt_args)
{
	struct fuse_args *args = (struct fuse_args *) opt_args;
	int ret;

#ifdef mingw_PLATFORM
	/* Initialized kmi_simple_options_data because it is reused by multi mounts on Windows. */
	free(priv.dk);
	priv.dk = NULL;
	free(priv.dki);
	priv.dki = NULL;
	free(priv.dk_for_format);
	priv.dk_for_format = NULL;
	free(priv.dki_for_format);
	priv.dki_for_format = NULL;
	free(priv.dk_list);
	priv.dk_list = NULL;
#endif

	/* fuse_opt_parse can handle a NULL device parameter just fine */
	ret = fuse_opt_parse(args, &priv, kmi_simple_options, null_parser);
	if (ret < 0) {
		ltfsmsg(LTFS_ERR, "15604E", ret);
		return ret;
	}

	if (((priv.dk != NULL) ^ (priv.dki != NULL)) || (priv.dk_for_format != NULL && priv.dki_for_format == NULL)) {
		ltfsmsg(LTFS_ERR, "15604E", 0);
		return -1;
	}

	if (priv.dk != NULL && priv.dki != NULL && priv.dk_for_format != NULL && priv.dki_for_format != NULL) {
		if ((strcmp((char *) priv.dk, (char *) priv.dk_for_format) == 0) ^ (strcmp((char *) priv.dki, (char *) priv.dki_for_format) == 0)) {
			ltfsmsg(LTFS_ERR, "15604E", 1);
			return -1;
		}
	}

	struct {
		unsigned char *dk;
		unsigned char *dki;
	} key[] = { { priv.dk, priv.dki }, { priv.dk_for_format, priv.dki_for_format } };

	for (unsigned int i = 0; i < sizeof(key) / sizeof(key[0]); ++i) {
		if (key[i].dk == NULL)
			continue;

		const size_t original_dk_list_len = (priv.dk_list ? strlen((char *) priv.dk_list) : 0);
		const size_t dk_list_len = (priv.dk_list ? strlen((char *) priv.dk_list) + strlen("/") : 0)
			+ strlen((char *) key[i].dk) + strlen(":") + strlen((char *) key[i].dki) + 1;

		if (priv.dk_list)
			priv.dk_list = realloc(priv.dk_list, dk_list_len);
		else
			priv.dk_list = calloc(dk_list_len, sizeof(unsigned char));
		if (priv.dk_list == NULL) {
			ltfsmsg(LTFS_ERR, "10001E", __FUNCTION__);
			return -LTFS_NO_MEMORY;
		}
		*(priv.dk_list + original_dk_list_len) = '\0';

		if (original_dk_list_len)
			strcat((char *) priv.dk_list, "/");
		strcat((char *) priv.dk_list, (char *) key[i].dk);
		strcat((char *) priv.dk_list, ":");
		strcat((char *) priv.dk_list, (char *) key[i].dki);
	}

	return 0;
}

struct kmi_ops simple_ops = {
	.init         = simple_init,
	.destroy      = simple_destroy,
	.get_key      = simple_get_key,
	.help_message = simple_help_message,
	.parse_opts   = simple_parse_opts,
};

struct kmi_ops *kmi_get_ops(void)
{
	return &simple_ops;
}

#ifndef mingw_PLATFORM
extern char kmi_simple_dat[];
#endif

const char *kmi_get_message_bundle_name(void ** const message_data)
{
#ifndef mingw_PLATFORM
	*message_data = kmi_simple_dat;
#else
	*message_data = NULL;
#endif
	return "kmi_simple";
}
