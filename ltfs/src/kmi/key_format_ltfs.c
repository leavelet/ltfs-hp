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
** FILE NAME:       kmi/key_format_ltfs.c
**
** DESCRIPTION:     Implements the LTFS specific format manager.
**
** AUTHOR:          Yutaka Oishi
**                  IBM Yamato, Japan
**                  oishi@jp.ibm.com
**
*************************************************************************************
*/

#ifdef mingw_PLATFORM
#include "libltfs/arch/win/win_util.h"
#endif
#include "libltfs/ltfs.h"
#include "libltfs/base64.h"
#include "key_format_ltfs.h"

#ifndef mingw_PLATFORM
#include <sys/mman.h>
#include <sys/resource.h>
#endif

enum kfl_state {
	KFL_UNINITIALIZED,
	KFL_INITIALIZED,
	KFL_SET,
	KFL_CLEARED,
	KFL_DESTROYED,
};

static enum kfl_state state = KFL_UNINITIALIZED;

struct key_format_ltfs_data {
	struct ltfs_volume *vol;    /**< A reference to the LTFS volume structure */
	void *data;                 /**< encryption key list */
};

/**
 * Check the key is LTFS specific format or not.
 * @param key key on LTFS specific format
 * @return 0 on success or a negative value on error.
 */
static int is_key(const unsigned char * const key)
{
	int i;

	for (i = 0; i < (DK_LENGTH * 8 + 5) / 6; ++i) {
		if (! isalnum(*(key + i)) && *(key + i) != '+' && *(key + i) != '/') {
			ltfsmsg(LTFS_ERR, "15600E", __FUNCTION__, "DK");
			return -LTFS_BAD_ARG;
		}
	}
	for (; i % 4; ++i) { /* BEAM: loop doesn't iterate - Use loop for future enhancement */
		if (*(key + i) != '=') {
			ltfsmsg(LTFS_ERR, "15600E", __FUNCTION__, "DK padding");
			return -LTFS_BAD_ARG;
		}
	}

	return 0;
}

/**
 * Check the key-alias is LTFS specific format or not.
 * @param keyalias key-alias on LTFS specific format
 * @return 0 on success or a negative value on error.
 */
static int is_keyalias(const unsigned char * const keyalias)
{
	int i;

	for (i = 0; i < DKI_ASCII_LENGTH; ++i) {
		if (! isprint(*(keyalias + i))) {
			ltfsmsg(LTFS_ERR, "15600E", __FUNCTION__, "DKi ascii");
			return -LTFS_BAD_ARG;
		}
	}
	for (; i < DKI_ASCII_LENGTH + (DKI_LENGTH - DKI_ASCII_LENGTH) * 2; ++i) {
		if (! isxdigit(*(keyalias + i))) {
			ltfsmsg(LTFS_ERR, "15600E", __FUNCTION__, "DKi binary");
			return -LTFS_BAD_ARG;
		}
	}

	return 0;
}

/**
 * Check syntax and get number of DK and DKi pairs
 * @param dk_list DK and DKi pairs' list on LTFS specific format.
 * @return the number of DK and DKi pairs on success or a negative value on error.
 */
static int get_num_of_keys(const unsigned char * const dk_list)
{
	const size_t length = strlen((const char *) dk_list);
	const size_t key_length = ((DK_LENGTH * 8 + 5) / 6 + 3) / 4 * 4;
	const size_t keyalias_length = DKI_ASCII_LENGTH + (DKI_LENGTH - DKI_ASCII_LENGTH) * 2;
	int num_of_keys = 0;

	CHECK_ARG_NULL(dk_list, -LTFS_NULL_ARG);

	if (key_length + SEPARATOR_LENGTH + keyalias_length <= length) {
		unsigned int i = 0;
		do {
			if (num_of_keys)
				i += SEPARATOR_LENGTH; /* skip DK and DKi pair's separator '/'. */

			int ret = is_key(dk_list + i);
			if (ret < 0) {
				ltfsmsg(LTFS_ERR, "15600E", __FUNCTION__, "kmi_dk_list");
				return -LTFS_BAD_ARG;
			}
			i += key_length;
			if (*(dk_list + i) != ':') {
				ltfsmsg(LTFS_ERR, "15600E", __FUNCTION__, "Separator of DK and DKi is incorrect.");
				return -LTFS_BAD_ARG;
			}
			i += SEPARATOR_LENGTH;
			ret = is_keyalias(dk_list + i);
			if (ret < 0) {
				ltfsmsg(LTFS_ERR, "15600E", __FUNCTION__, "kmi_dk_list"); /* is_keyalias is called for kmi_dk_list */
				return -LTFS_BAD_ARG;
			}
			i += keyalias_length;
			++num_of_keys;
		} while (i + SEPARATOR_LENGTH + key_length + SEPARATOR_LENGTH + keyalias_length <= length &&
				 *(dk_list + i) == '/');

		if (i != length) {
			ltfsmsg(LTFS_ERR, "15600E", __FUNCTION__, "Invalid length of kmi_dk_list.");
			return -LTFS_BAD_ARG;
		}
	}

	return num_of_keys;
}

static size_t convert_key(const unsigned char * const enc, unsigned char * const key)
{
	unsigned char *dec = NULL;
	size_t size = base64_decode(enc, ((DK_LENGTH * 8 + 5) / 6 + 3) / 4 * 4, &dec);

	if (size == DK_LENGTH)
		memcpy(key, dec, DK_LENGTH);
	if (size)
		free(dec);

	return size;
}

/**
 * convert key-alias from ASCII and hex dump style to binary style
 * @param ascii_and_hex key-alias on LTFS specific format.
 * @param bin key-alias on binary format
 * @return 0 on success or a negative value on error.
 */
static void convert_keyalias(const unsigned char * const ascii_and_hex, unsigned char * const bin)
{
	memcpy(bin, ascii_and_hex, DKI_ASCII_LENGTH);

	int i = 0;
	for (i = 0; i < DKI_LENGTH - DKI_ASCII_LENGTH; ++i) {
		unsigned char tmp[3] = {0};
		tmp[0] = *(ascii_and_hex + DKI_ASCII_LENGTH + i * 2);
		tmp[1] = *(ascii_and_hex + DKI_ASCII_LENGTH + i * 2 + 1);
		*(bin + DKI_ASCII_LENGTH + i) = strtoul((char *) tmp, NULL, 0x10);
	}
}

/**
 * Initialize the key manager interface plugin.
 * @param vol LTFS volume
 * @id message id for initializing key manager interface plug-in
 * @return a pointer to the private data on success or NULL on error.
 */
void *key_format_ltfs_init(struct ltfs_volume *vol, const char *id)
{
	CHECK_ARG_NULL(vol, NULL);

#ifndef mingw_PLATFORM
	/*
	 * On Windows, this function is called at not only KFL_UNINITIALIZED but also KFL_INITIALIZED, KFL_CLEARED
	 * and KFL_DESTROYED because the process keep running after a user eject a cartridge.
	 */
	if (state != KFL_UNINITIALIZED) {
		ltfsmsg(LTFS_ERR, "15605E", state, KFL_UNINITIALIZED, __FUNCTION__);
		return NULL;
	}
#endif

	struct key_format_ltfs_data *priv = calloc(1, sizeof(struct key_format_ltfs_data));
	if (! priv) {
		ltfsmsg(LTFS_ERR, "10001E", __FUNCTION__);
		return NULL;
	}
	priv->vol = vol;
	priv->data = calloc(1, sizeof(struct key_format_ltfs));
	if (! priv->data) {
		ltfsmsg(LTFS_ERR, "10001E", __FUNCTION__);
		return NULL;
	}

	state = KFL_INITIALIZED;
	ltfsmsg(LTFS_DEBUG, id);
	return priv;
}

/**
 * Destroy the key manager interface plugin.
 * @param kmi_handle the key manager interface handle
 * @id message id for destroying key manager interface plug-in
 * @return 0 on success or a negative value on error.
 */
int key_format_ltfs_destroy(void * const kmi_handle, const char *id)
{
	struct key_format_ltfs_data *priv = (struct key_format_ltfs_data *) kmi_handle;
	CHECK_ARG_NULL(kmi_handle, -LTFS_NULL_ARG);

	free(priv->data);
	free(priv);
	state = KFL_DESTROYED;
	ltfsmsg(LTFS_DEBUG, id);
	return 0;
}

/**
 * Set key list
 * @param dk_list DK and DKi pair on LTFS specific format.
 * @param data output of DK and DKi list
 * @return 0 on success or a negative value on error.
 */
static int set_dk_list(const unsigned char * const dk_list, void **data)
{
	int num_of_keys = 0;
	struct key_format_ltfs **priv = (struct key_format_ltfs **) data;

	CHECK_ARG_NULL(data, -LTFS_NULL_ARG);
	CHECK_ARG_NULL(*data, -LTFS_NULL_ARG);

	if (state != KFL_INITIALIZED && state != KFL_CLEARED) {
		ltfsmsg(LTFS_ERR, "15605E", state, KFL_INITIALIZED, __FUNCTION__);
		return -LTFS_INVALID_SEQUENCE;
	}

	if (dk_list) {
		num_of_keys = get_num_of_keys(dk_list);
		if (num_of_keys < 0)
			return num_of_keys;
	}

	if (num_of_keys) {
		(*priv)->dk_list = calloc(num_of_keys, sizeof(struct key));
		if (! (*priv)->dk_list) {
			ltfsmsg(LTFS_ERR, "10001E", __FUNCTION__);
			return -LTFS_NO_MEMORY;
		}
		(*priv)->num_of_keys = num_of_keys;

		int i = 0;
		unsigned int offset = 0;
		for (i = 0; i < num_of_keys; ++i) {
			convert_key(dk_list + offset, ((*priv)->dk_list + i)->dk);
			offset += ((DK_LENGTH * 8 + 5) / 6 + 3) / 4 * 4 + 1;
			convert_keyalias(dk_list + offset, ((*priv)->dk_list + i)->dki);
			offset += DKI_ASCII_LENGTH + (DKI_LENGTH - DKI_ASCII_LENGTH) * 2 + 1;
		}
	}

	state = KFL_SET;
	return 0;
}

/**
 * Get Key
 * @param keyalias Get key of the key-alias. If *keyalias is NULL, get key of default key-alias
 * @param key Memory is allocated and key is stored at the address.
 * @param data DK and DKi which are parsed from LTFS specific format
 * @return 0 on success or a negative value on error.
 */
static int get_key(unsigned char **keyalias, unsigned char **key, void *data, unsigned char * const dki_for_format)
{
	struct key_format_ltfs *priv = (struct key_format_ltfs *) data;

	CHECK_ARG_NULL(keyalias, -LTFS_NULL_ARG);
	CHECK_ARG_NULL(key, -LTFS_NULL_ARG);

	*key = NULL;

	if (priv) {
		if (! *keyalias) {
			if (! dki_for_format)
				return 0; /* This is not an error path but a normal pass. Make a non-encrypted cartridge. */
			*keyalias = calloc(DKI_LENGTH, sizeof(char));
			if (! *keyalias) {
				ltfsmsg(LTFS_ERR, "10001E", __FUNCTION__);
				return -LTFS_NO_MEMORY;
			}
			convert_keyalias(dki_for_format, *keyalias);
		}

		int i;
		for (i = 0; i < priv->num_of_keys; ++i) {
			if (! memcmp(*keyalias, (priv->dk_list + i)->dki, DKI_LENGTH)) {
				*key = calloc(DK_LENGTH, sizeof(char));
				if (! *key) {
					ltfsmsg(LTFS_ERR, "10001E", __FUNCTION__);
					return -LTFS_NO_MEMORY;
				}
				memcpy(*key, (priv->dk_list + i)->dk, DK_LENGTH);
				break;
			}
		}
		if (! *key) {
			ltfsmsg(LTFS_ERR, "15603E");
			return -LTFS_KEY_NOT_FOUND;
		}
	}
	return 0;
}

/**
 * Clear DK and DKi list for the next parse or destroying them
 * @param data DK and DKi which are parsed from LTFS specific format
 * @return 0 on success or a negative value on error.
 */
static int clear(void **data)
{
	struct key_format_ltfs **priv = (struct key_format_ltfs **) data;

	CHECK_ARG_NULL(data, -LTFS_NULL_ARG);

	if (*priv) {
		if ((*priv)->dk_list) {
			memset((*priv)->dk_list, 0, sizeof(struct key) * (*priv)->num_of_keys);
			free((*priv)->dk_list);
			(*priv)->dk_list = NULL;
		}
		(*priv)->num_of_keys = 0; /* clear num_of_keys after clearing dk_list */
	}
	if (state == KFL_SET)
		state = KFL_CLEARED;
	return 0;
}

/**
 * Get Key
 * @param keyalias Get key of the key-alias. If *keyalias is NULL, get key of default key-alias
 * @param key Memory is allocated and key is stored at the address.
 * @param kmi_handle the key manager interface handle
 * @param dk_list data key and data key identifier pairs' list on direct plug-in format
 * @param dki_for_format data key identifier to format a cartridge
 * @return 0 on success or a negative value on error.
 */
int key_format_ltfs_get_key(unsigned char **keyalias, unsigned char **key, void * const kmi_handle,
	unsigned char * const dk_list, unsigned char * const dki_for_format)
{

	struct key_format_ltfs_data *priv = (struct key_format_ltfs_data *) kmi_handle;
	CHECK_ARG_NULL(kmi_handle, -LTFS_NULL_ARG);
	int ret = set_dk_list(dk_list, &priv->data);
	if (ret < 0) {
		ltfsmsg(LTFS_ERR, "15606E");
		return ret;
	}
	ret = get_key(keyalias, key, priv->data, dki_for_format);
	if (ret < 0) {
		ltfsmsg(LTFS_ERR, "15607E");
		(void) clear(&priv->data);
		return ret;
	}

	return clear(&priv->data);
}
