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
** FILE NAME:       pathname.c
**
** DESCRIPTION:     Unicode text analysis and processing routines.
**
** AUTHORS:         Brian Biskeborn
**                  IBM Almaden Research Center
**                  bbiskebo@us.ibm.com
**
*************************************************************************************
**
**  (C) Copyright 2015 - 2017 Hewlett Packard Enterprise Development LP
**  10/13/17 Added support for SNIA 2.4
**
*************************************************************************************
**
** Copyright (C) 2012 OSR Open Systems Resources, Inc.
** 
************************************************************************************* 
*/

#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <errno.h>

#ifdef __APPLE__
#include <ICU/unicode/uchar.h>
#include <ICU/unicode/ustring.h>
#include <ICU/unicode/utypes.h>
#include <ICU/unicode/ucnv.h>
#include <ICU/unicode/unorm.h>
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
#include <unicode/uchar.h>
#include <unicode/ustring.h>
#include <unicode/utypes.h>
#define __MINGW32__

#else 
#include <unicode/uchar.h>
#include <unicode/ustring.h>
#include <unicode/utypes.h>
#endif /* #if defined(HPE_mingw_BUILD) && defined(__MINGW32__) */

#include <unicode/ucnv.h>
#include <unicode/unorm.h>
#endif

#include "ltfs.h"
#include "pathname.h"
#include "libltfs/ltfslogging.h"

int _pathname_is_utf8(const char *name, size_t size);
int _pathname_validate(const char *name, bool allow_slash);
int _pathname_valid_in_xml(UChar32 c);
int _pathname_valid_in_xml_for_xattrvalue(UChar32 c);
int _pathname_format_icu(const char *src, char **dest, bool validate, bool allow_slash);
int _pathname_check_utf8_icu(const char *src, size_t size);
int _pathname_foldcase_utf8_icu(const char *src, char **dest);
int _pathname_normalize_utf8_icu(const char *src, char **dest);
int _pathname_foldcase_icu(const UChar *src, UChar **dest);
int _pathname_normalize_nfc_icu(const UChar *src, UChar **dest);
int _pathname_normalize_nfd_icu(const UChar *src, UChar **dest);
int _pathname_utf8_to_utf16_icu(const char *src, UChar **dest);
int _pathname_utf16_to_utf8_icu(const UChar *src, char **dest);
int _pathname_system_to_utf16_icu(const char *src, UChar **dest);
int _pathname_utf8_to_system_icu(const char *src, char **dest);
int _pathname_normalize_utf8_nfd_icu(const char *src, char **dest);


/**
 * Convert a path name in the system locale to the canonical LTFS form (UTF-8, NFC).
 * @param name file, directory, or xattr name to format
 * @param new_name on success, points to newly allocated buffer containing the
 *                 null-terminated, formatted name.
 * @param validate true to check the name for invalid characters. Also checks the length
 *                 of the name if allow_slash is false.
 * @param allow_slash true if the name is allowed to contain '/'. Ignored if validate is false.
 * @return number of bytes in output, or a negative value on error.
 */
int pathname_format(const char *name, char **new_name, bool validate, bool allow_slash)
{
	int ret;

	CHECK_ARG_NULL(name, -LTFS_NULL_ARG);
	CHECK_ARG_NULL(new_name, -LTFS_NULL_ARG);

	ret = _pathname_format_icu(name, new_name, validate, allow_slash);
	return ret;
}

/**
 * Convert a path name in the canonical LTFS form back to the system locale.
 * @param name path to convert
 * @param new_name on success, contains converted name in an newly allocated buffer.
 * @return 0 on success or a negative value on error.
 */
int pathname_unformat(const char *name, char **new_name)
{
	int ret;

	CHECK_ARG_NULL(name, -LTFS_NULL_ARG);
	CHECK_ARG_NULL(new_name, -LTFS_NULL_ARG);

	ret = _pathname_utf8_to_system_icu(name, new_name);
	return ret;
}

/**
 * Perform caseless matching.
 * @param name1 A file name to be matched.
 * @param name2 A file name to be matched.
 * @param result Outputs matching result.
 * @return 0 on success or a negative value on error.
 */
int pathname_caseless_match(const char *name1, const char *name2, int *result)
{
	int ret;
	UChar *dname1, *dname2;

	CHECK_ARG_NULL(name1, -LTFS_NULL_ARG);
	CHECK_ARG_NULL(name2, -LTFS_NULL_ARG);

	if (! (ret=pathname_prepare_caseless(name1, &dname1, true))) {
		if (! (ret=pathname_prepare_caseless(name2, &dname2, true))) {
			*result = u_strcmp(dname1, dname2);
			free(dname2);
		}
		free(dname1);
	}

	return ret;
}

/**
 * Prepare a name for canonical caseless matching.
 * @param name File name to prepare, in UTF-8.
 * @param new_name Outputs NFD(toCaseFold(NFD(name))).
 * @param use_nfc True to convert the output to NFC, false to leave it in NFD.
 * @return 0 on success or a negative value on error.
 */
int pathname_prepare_caseless(const char *name, UChar **new_name, bool use_nfc)
{
	int ret;
	bool need_initial_nfd;
	UChar *icu_name, *icu_nfd, *icu_fold, *tmp;

	CHECK_ARG_NULL(name, -LTFS_NULL_ARG);
	CHECK_ARG_NULL(new_name, -LTFS_NULL_ARG);

	/* Convert to ICU's internal UTF-16 representation. */
	ret = _pathname_utf8_to_utf16_icu(name, &icu_name);
	if (ret < 0)
		return ret;

	/* Figure out whether an initial NFD mapping is needed. This is the case if the string
	 * contains U+0345 or a code point whose canonical decomposition contains U+0345.
	 * All such code points fall between U+1F80 and U+1FFF, inclusive. */
	need_initial_nfd = false;
	tmp = icu_name;
	while (*tmp) {
		if (*tmp == 0x0345 || (*tmp >= 0x1f80 && *tmp <= 0x1fff)) {
			need_initial_nfd = true;
			break;
		}
		++tmp;
	}

	/* Convert to NFD if needed, then case fold the name. */
	if (need_initial_nfd) {
		ret = _pathname_normalize_nfd_icu(icu_name, &icu_nfd);
		if (icu_name != icu_nfd)
			free(icu_name);
		if (ret < 0)
			return ret;
		ret = _pathname_foldcase_icu(icu_nfd, &icu_fold);
		free(icu_nfd);
		if (ret < 0)
			return ret;
	} else {
		ret = _pathname_foldcase_icu(icu_name, &icu_fold);
		free(icu_name);
		if (ret < 0)
			return ret;
	}

	/* Perform the final normalization mapping to the output. */
	if (use_nfc)
		ret = _pathname_normalize_nfc_icu(icu_fold, new_name);
	else
		ret = _pathname_normalize_nfd_icu(icu_fold, new_name);
	if (icu_fold != *new_name)
		free(icu_fold);
	if (ret < 0)
		return ret;

	return 0;
}

/**
 * Normalize a UTF-8 string to NFC.
 * @param name string to normalize, null-terminated
 * @param new_name on success points to a newly allocated buffer containing the normalized string
 * @return 0 on success or a negative value on error.
 */
int pathname_normalize(const char *name, char **new_name)
{
	int ret;

	CHECK_ARG_NULL(name, -LTFS_NULL_ARG);
	CHECK_ARG_NULL(new_name, -LTFS_NULL_ARG);

	ret = _pathname_normalize_utf8_icu(name, new_name);
	return ret;
}

/**
 * Validate a file name.
 * @return 0 on success or a negative value on error.
 */
// HPE MD 22.09.2017 Change made to function to allow larger names when percent encoded.
int pathname_validate_file(const char *name, int percentencoded)
{
	int namelen, ret;

	CHECK_ARG_NULL(name, -LTFS_NULL_ARG);

	namelen = pathname_strlen(name);
	if (namelen < 0)
		return namelen;
	if (namelen > ( LTFS_FILENAME_MAX + ( 2 * LTFS_FILENAME_MAX * percentencoded ) ) ) {
		return -LTFS_NAMETOOLONG;
	}
	ret = _pathname_validate(name, false);
	return ret;
}

/**
 * Validate an extended attribute name. Names must conform to the same contraints as file and
 * directory names. Additionally, "ltfs*" names are not valid unless they are recognized by
 * this implementation.
 * @param name Name to validate.
 * HPE MD 21.09.2017 SNIA 2.4 allows / to be used in xattr names and as such no longer conform to
 * the same constraints as file and directory names.
 */
int pathname_validate_xattr_name(const char *name, int percentencoded)
{
    int namelen, ret;

    CHECK_ARG_NULL(name, -LTFS_NULL_ARG);

    namelen = pathname_strlen(name);
    if (namelen < 0)
        return namelen;
    if (namelen > ( LTFS_FILENAME_MAX + ( 2 * LTFS_FILENAME_MAX * percentencoded ) ) ) {
        return -LTFS_NAMETOOLONG;
    }

    ret = _pathname_validate(name, true);
    return ret;
}

/**
 * Check an xattr value for well-formed, XML-valid UTF-8.
 * @param name String to check.
 * @param size Length of string to check.
 * @return 0 if string can be safely placed in an Index, 1 if it must be base64 encoded, or
 *         a negative value on error.
 */
int pathname_validate_xattr_value(const char *name, size_t size)
{
	int ret;
	UChar32 c;
	int32_t i = 0;

	CHECK_ARG_NULL(name, -LTFS_NULL_ARG);

	/* Check for UTF-8. */
	ret = _pathname_is_utf8(name, size);
	if (ret < 0)
		return ret;
	else if (ret == 1)
		return 1;

	/* Check for characters disallowed in XML. */
	while (i < (ssize_t) size) {
		U8_NEXT(name, i, (int32_t) size, c);
		if (c < 0) {
			ltfsmsg(LTFS_ERR, "11234E");
			return -LTFS_ICU_ERROR;
		}

		if (_pathname_valid_in_xml_for_xattrvalue(c) == 0)
			return 1;
	}

	return 0;
}



/* Private pathname functions are not traced. This is because (a) tracing their inner workings
 * is not expected to be interesting, and (b) tracing them produces large amounts of output,
 * obscuring more interesting traces.
 *
 * The call path from public pathname functions to private ones is rather simple, so tracing
 * the private functions is unlikely to yield any useful debugging information.
 */



/**
 * Count the code points in a null-terminated UTF-8 string.
 * Note: this function assumes the input string is well-formed UTF-8!
 * @return number of code points on success or a negative value on error.
 */
int pathname_strlen(const char *name)
{
	const char *tmp = name;
	int ret = 0;

	CHECK_ARG_NULL(name, -LTFS_NULL_ARG);

	while (*tmp) {
		if (! (*tmp & 0x80) || (*tmp & 0xC0) == 0xC0)
			++ret;
		++tmp;
	}
	return ret;
}

/**
 * Truncate a string to the given size.
 * @param name string to truncate
 * @param size new string size
 * @return 0 on success or a negative value on error
 */
int pathname_truncate(char *name, size_t size)
{
	char *tmp = name;
	size_t len = 0;

	CHECK_ARG_NULL(name, -LTFS_NULL_ARG);

	while (*tmp) {
		if (! (*tmp & 0x80) || (*tmp & 0xC0) == 0xC0) {
			if (len++ == size) {
				*tmp = '\0';
				break;
			}
		}
		++tmp;
	}
	return 0;
}

/**
 * Check whether a given buffer contains a valid UTF-8 string. Used to decide whether
 * to base64-encode xattr values, which is why it takes an explicit size parameter.
 * @param name buffer to check
 * @param size buffer size
 * @return 0 on success, 1 if string is not UTF-8, or a negative value on error.
 */
int _pathname_is_utf8(const char *name, size_t size)
{
	int ret;

	CHECK_ARG_NULL(name, -LTFS_NULL_ARG);

	ret = _pathname_check_utf8_icu(name, size);
	return ret;
}

/**
 * Check a pathname for characters that are not allowed in file names.
 * @param name string to check, null-terminated
 * @param allow_slash if true, allow slashes in the string (valid for paths, not for files)
 * @return 0 if name is valid or a negative value on error.
 */
int _pathname_validate(const char *name, bool allow_slash)
{
	UChar32 c;
	int32_t i = 0, len;

	CHECK_ARG_NULL(name, -LTFS_NULL_ARG);

	len = strlen(name);
	while (i < len) {
		U8_NEXT(name, i, len, c);
		if (c < 0) {
			ltfsmsg(LTFS_ERR, "11235E");
			return -LTFS_ICU_ERROR;
		}
      // HPE MD Removed checking of : as percent encoding should now handle it
		if (_pathname_valid_in_xml(c) == 0 || (! allow_slash && c == '/'))
			return -LTFS_INVALID_PATH;
	}

	return 0;
}

/**
 * Determine whether a given Unicode code point is valid in XML.
 * @param c Code point to check.
 * @return 1 if valid or 0 if not.
 */
int _pathname_valid_in_xml(UChar32 c)
{
	// HPE MD 22.09.2017 Removed character checking for characters that will be percent encoded.
	if ( (c >= 0xd800 && c <= 0xdfff) || c == 0xfffe || c == 0xffff  || c == 0)
		return 0;
	else
		return 1;
}

/**
 * Determine whether a given Unicode code point is valid in XML for an
 *  extended attribute value.  Diverged from preceding function because
 *  EA values are encoded using base64 rather than percent encoding used
 *  for EA names (and for directory & file names).  HPE 09-Aug-18
 * @param c Code point to check.
 * @return 1 if valid or 0 if not (i.e. needs base64 encoding).
 */
int _pathname_valid_in_xml_for_xattrvalue (UChar32 c)
{
	if ((c >= 0 && c <= 0x1f && c != 0x09 && c != 0x0a && c != 0x0d) ||
	    (c >= 0xd800 && c <= 0xdfff) || c == 0xfffe || c == 0xffff) {
		return 0;
	} else {
		return 1;
	}
}

/**
 * Convert a path name in the system locale to the canonical LTFS form (UTF-8, NFC).
 * @param name file, directory, or xattr name to format
 * @param new_name on success, points to newly allocated buffer containing the
 *                 null-terminated, formatted name.
 * @param validate true to check the name for length and invalid characters
 * @param allow_slash true if the name is allowed to contain '/'. Ignored if validate is false.
 * @return number of bytes in output, or a negative value on error.
 */
int _pathname_format_icu(const char *src, char **dest, bool validate, bool allow_slash)
{
	int ret;
	UChar *utf16_name, *utf16_name_norm;

	/* convert to UTF-16 for normalization with ICU */
	ret = _pathname_system_to_utf16_icu(src, &utf16_name);
	if (ret < 0)
		return ret;

	/* normalize */
	ret = _pathname_normalize_nfc_icu(utf16_name, &utf16_name_norm);
	if (utf16_name != utf16_name_norm)
		free(utf16_name);
	if (ret < 0)
		return ret;

	/* convert to UTF-8 */
	ret = _pathname_utf16_to_utf8_icu(utf16_name_norm, dest);
	free(utf16_name_norm);
	if (ret < 0)
		return ret;

	if (validate) {
		/* check length of the name unless it's supposed to be a path */
		if (! allow_slash) {
			ret = pathname_strlen(*dest);
			if (ret < 0) {
				free(*dest);
				*dest = NULL;
				return ret;
			}
			if (ret > LTFS_FILENAME_MAX) {
				free(*dest);
				*dest = NULL;
				return -LTFS_NAMETOOLONG;
			}
		}

		/* check path for invalid characters */
		ret = _pathname_validate(*dest, allow_slash);
		if (ret < 0) {
			free(*dest);
			*dest = NULL;
			return ret;
		}
	}

	return 0;
}

int pathname_nfd_normaize(const char *name, char **new_name)
{
	int ret;
	CHECK_ARG_NULL(name, -LTFS_NULL_ARG);
	CHECK_ARG_NULL(new_name, -LTFS_NULL_ARG);

	ret = _pathname_normalize_utf8_nfd_icu(name, new_name);
	return ret;
}

int _pathname_normalize_utf8_nfd_icu(const char *src, char **dest)
{
	UChar *icu_str, *icu_str_norm;
	int ret;

	ret = _pathname_utf8_to_utf16_icu(src, &icu_str);
	if (ret < 0)
		return ret;

	ret = _pathname_normalize_nfd_icu(icu_str, &icu_str_norm);
	if (icu_str != icu_str_norm)
		free(icu_str);
	if (ret < 0)
		return ret;

	ret = _pathname_utf16_to_utf8_icu(icu_str_norm, dest);
	free(icu_str_norm);
	return ret;
}

/**
 * Check whether a given string is valid UTF-8.
 * @param str string to check, not necessarily null-terminated
 * @param size length of string in bytes
 * @return 0 on success, 1 if string is not UTF-8, or a negative value on error.
 */
int _pathname_check_utf8_icu(const char *src, size_t size)
{
	UErrorCode err = U_ZERO_ERROR;

	u_strFromUTF8(NULL, 0, NULL, src, (int32_t)size, &err);
	if (U_FAILURE(err) && err != U_BUFFER_OVERFLOW_ERROR)
		return 1;

	return 0;
}

/**
 * Fold the case of a UTF-8 string. The result is suitable for case-insensitive comparisons.
 * @param src string to process
 * @param dest on success, points to a newly allocated buffer containing the output string.
 * @return 0 on success or a negative value on error.
 */
int _pathname_foldcase_utf8_icu(const char *src, char **dest)
{
	UChar *icu_str, *icu_str_fold;
	int ret;

	ret = _pathname_utf8_to_utf16_icu(src, &icu_str);
	if (ret < 0)
		return ret;

	ret = _pathname_foldcase_icu(icu_str, &icu_str_fold);
	free(icu_str);
	if (ret < 0)
		return ret;

	ret = _pathname_utf16_to_utf8_icu(icu_str_fold, dest);
	free(icu_str_fold);
	return ret;
}

/**
 * Normalize a UTF-8 string to NFC using ICU.
 * @param name string to normalize, null-terminated
 * @param new_name on success points to a newly allocated buffer containing the normalized string
 * @return 0 on success or a negative value on error.
 */
int _pathname_normalize_utf8_icu(const char *src, char **dest)
{
	UChar *icu_str, *icu_str_norm;
	int ret;

	ret = _pathname_utf8_to_utf16_icu(src, &icu_str);
	if (ret < 0)
		return ret;

	ret = _pathname_normalize_nfc_icu(icu_str, &icu_str_norm);
	if (icu_str != icu_str_norm)
		free(icu_str);
	if (ret < 0)
		return ret;

	ret = _pathname_utf16_to_utf8_icu(icu_str_norm, dest);
	free(icu_str_norm);
	return ret;
}

/**
 * Fold the case of a string in ICU internal representation. The result is suitable for
 * case-insensitive comparisons.
 * @param src string to process
 * @param dest on success, points to a newly allocated buffer containing the output string.
 * @return 0 on success or a negative value on error.
 */
int _pathname_foldcase_icu(const UChar *src, UChar **dest)
{
	UErrorCode err = U_ZERO_ERROR;
	int32_t destlen;

	destlen = u_strFoldCase(NULL, 0, src, -1, U_FOLD_CASE_DEFAULT, &err);
	if (U_FAILURE(err) && err != U_BUFFER_OVERFLOW_ERROR) {
		ltfsmsg(LTFS_ERR, "11236E", err);
		return -LTFS_ICU_ERROR;
	}
	err = U_ZERO_ERROR;

	*dest = malloc((destlen + 1) * sizeof(UChar));
	if (! *dest) {
		ltfsmsg(LTFS_ERR, "10001E", __FUNCTION__);
		return -LTFS_NO_MEMORY;
	}

	u_strFoldCase(*dest, destlen + 1, src, -1, U_FOLD_CASE_DEFAULT, &err);
	if (U_FAILURE(err)) {
		ltfsmsg(LTFS_ERR, "11237E", err);
		free(*dest);
		*dest = NULL;
		return -LTFS_ICU_ERROR;
	}

	return 0;
}

/**
 * Normalize a string in internal ICU representation to NFC.
 * @param src string to normalize
 * @param dest On success, points to a buffer containing the output string.
 *             The resulting pointer is src if the input string is already normalized;
 *             otherwise, the output buffer is newly allocated and should be freed
 *             by the caller.
 * @return 0 on success or a negative value on error.
 */
int _pathname_normalize_nfc_icu(const UChar *src, UChar **dest)
{
	UErrorCode err = U_ZERO_ERROR;
	int32_t destlen;

	/* Do a quick check to decide whether this string is already normalized. */
	if (unorm_quickCheck(src, -1, UNORM_NFC, &err) == UNORM_YES) {
		*dest = (UChar *)src;
		return 0;
	}
	err = U_ZERO_ERROR;

	destlen = unorm_normalize(src, -1, UNORM_NFC, 0, NULL, 0, &err);
	if (U_FAILURE(err) && err != U_BUFFER_OVERFLOW_ERROR) {
		ltfsmsg(LTFS_ERR, "11238E", err);
		return -LTFS_ICU_ERROR;
	}
	err = U_ZERO_ERROR;

	*dest = malloc((destlen + 1) * sizeof(UChar));
	if (! *dest) {
		ltfsmsg(LTFS_ERR, "10001E", __FUNCTION__);
		return -LTFS_NO_MEMORY;
	}

	unorm_normalize(src, -1, UNORM_NFC, 0, *dest, destlen + 1, &err);
	if (U_FAILURE(err)) {
		ltfsmsg(LTFS_ERR, "11239E", err);
		free(*dest);
		*dest = NULL;
		return -LTFS_ICU_ERROR;
	}

	return 0;
}

/**
 * Normalize a string in internal ICU representation to NFD.
 * @param src string to normalize
 * @param dest on success, points to a newly allocated buffer containing the output string.
 * @return 0 on success or a negative value on error.
 */
int _pathname_normalize_nfd_icu(const UChar *src, UChar **dest)
{
	UErrorCode err = U_ZERO_ERROR;
	int32_t destlen;

	/* Do a quick check to decide whether this string is already normalized. */
	if (unorm_quickCheck(src, -1, UNORM_NFD, &err) == UNORM_YES) {
		*dest = (UChar *)src;
		return 0;
	}
	err = U_ZERO_ERROR;

	destlen = unorm_normalize(src, -1, UNORM_NFD, 0, NULL, 0, &err);
	if (U_FAILURE(err) && err != U_BUFFER_OVERFLOW_ERROR) {
		ltfsmsg(LTFS_ERR, "11240E", err);
		return -LTFS_ICU_ERROR;
	}
	err = U_ZERO_ERROR;

	*dest = malloc((destlen + 1) * sizeof(UChar));
	if (! *dest) {
		ltfsmsg(LTFS_ERR, "10001E", __FUNCTION__);
		return -LTFS_NO_MEMORY;
	}

	unorm_normalize(src, -1, UNORM_NFD, 0, *dest, destlen + 1, &err);
	if (U_FAILURE(err)) {
		ltfsmsg(LTFS_ERR, "11241E", err);
		free(*dest);
		*dest = NULL;
		return -LTFS_ICU_ERROR;
	}

	return 0;
}

/**
 * Convert a UTF-8 string to ICU's UTF-16 representation.
 * @param src string to convert
 * @param dest on success, holds a newly allocated buffer containing converted string
 * @return 0 on success or a negative value on error.
 */
int _pathname_utf8_to_utf16_icu(const char *src, UChar **dest)
{
	UErrorCode err = U_ZERO_ERROR;
	int32_t destlen;

	/* get required buffer size */
	u_strFromUTF8(NULL, 0, &destlen, src, -1, &err);
	if (U_FAILURE(err) && err != U_BUFFER_OVERFLOW_ERROR) {
		ltfsmsg(LTFS_ERR, "11242E", err);
		return -LTFS_ICU_ERROR;
	}
	err = U_ZERO_ERROR;

	*dest = malloc((destlen + 1) * sizeof(UChar));
	if (! *dest) {
		ltfsmsg(LTFS_ERR, "10001E", __FUNCTION__);
		return -LTFS_NO_MEMORY;
	}

	/* convert to ICU's UTF-16 representation */
	u_strFromUTF8(*dest, destlen + 1, NULL, src, -1, &err);
	if (U_FAILURE(err)) {
		ltfsmsg(LTFS_ERR, "11243E", err);
		free(*dest);
		*dest = NULL;
		return -LTFS_ICU_ERROR;
	}

	return destlen;
}

/**
 * Convert a string in the internal ICU representation to UTF-8 without performing normalization
 * or any other modifications.
 * @param src string to convert
 * @param dest on success, points to a newly allocated buffer containing the output string.
 * @return 0 on success or a negative value on error.
 */
int _pathname_utf16_to_utf8_icu(const UChar *src, char **dest)
{
	UErrorCode err = U_ZERO_ERROR;
	int32_t destlen;

	u_strToUTF8(NULL, 0, &destlen, src, -1, &err);
	if (U_FAILURE(err) && err != U_BUFFER_OVERFLOW_ERROR) {
		ltfsmsg(LTFS_ERR, "11244E", err);
		return -LTFS_ICU_ERROR;
	}
	err = U_ZERO_ERROR;

	*dest = malloc(destlen + 1);
	if (! *dest) {
		ltfsmsg(LTFS_ERR, "10001E", __FUNCTION__);
		return -LTFS_NO_MEMORY;
	}

	u_strToUTF8(*dest, destlen + 1, NULL, src, -1, &err);
	if (U_FAILURE(err)) {
		ltfsmsg(LTFS_ERR, "11245E", err);
		free(*dest);
		*dest = NULL;
		return -LTFS_ICU_ERROR;
	}

	return 0;
}

/**
 * Convert a string in the system locale to ICU's UTF-16 representation.
 * TODO: better performance by caching a converter and using a mutex?
 * @param src string to convert, null-terminated
 * @param dest on success, holds a newly allocated buffer containing the converted string
 * @return 0 on success or a negative value on error.
 */
int _pathname_system_to_utf16_icu(const char *src, UChar **dest)
{
	UErrorCode err = U_ZERO_ERROR;
	UConverter *syslocale;
	int32_t destlen;

#ifndef HPE_mingw_BUILD
	/* open converter for the system locale */
	syslocale = ucnv_open(NULL, &err);
	if (U_FAILURE(err)) {
		ltfsmsg(LTFS_ERR, "11246E", err);
		return -LTFS_ICU_ERROR;
	}
#else
	syslocale = ucnv_open("UTF-8", &err);
	if (U_FAILURE(err)) {
		ltfsmsg(LTFS_ERR, "11246E", err);
		return -LTFS_ICU_ERROR;
	}
#endif /* HPE_mingw_BUILD */

	ucnv_setToUCallBack(syslocale, UCNV_TO_U_CALLBACK_STOP, NULL, NULL, NULL, &err);
	if (U_FAILURE(err)) {
		ltfsmsg(LTFS_ERR, "11247E", err);
		ucnv_close(syslocale);
		return -LTFS_ICU_ERROR;
	}

	/* perform the conversion */
	destlen = ucnv_toUChars(syslocale, NULL, 0, src, -1, &err);
	if (U_FAILURE(err) && err != U_BUFFER_OVERFLOW_ERROR) {
		ltfsmsg(LTFS_ERR, "11248E", err, src);
		ucnv_close(syslocale);
		return -LTFS_ICU_ERROR;
	}
	err = U_ZERO_ERROR;

	*dest = malloc((destlen + 1) * sizeof(UChar));
	if (! *dest) {
		ltfsmsg(LTFS_ERR, "10001E", __FUNCTION__);
		ucnv_close(syslocale);
		return -LTFS_NO_MEMORY;
	}

	ucnv_toUChars(syslocale, *dest, destlen + 1, src, -1, &err);
	if (U_FAILURE(err)) {
		ltfsmsg(LTFS_ERR, "11249E", err, src);
		ucnv_close(syslocale);
		free(*dest);
		*dest = NULL;
		return -LTFS_ICU_ERROR;
	}

	ucnv_close(syslocale);
	return 0;
}

/**
 * Convert a path name in the canonical LTFS form back to the system locale.
 * TODO: better performance by caching converters and using a mutex?
 * @param name path to convert
 * @param new_name on success, contains converted name in an newly allocated buffer.
 * @return 0 on success or a negative value on error.
 */
int _pathname_utf8_to_system_icu(const char *src, char **dest)
{
	const char *syslocale;
	UErrorCode err = U_ZERO_ERROR;
	int32_t destlen;

#ifndef HPE_mingw_BUILD
	/* If current locale is UTF-8, no conversion needed */
	syslocale = ucnv_getDefaultName();
#else
	syslocale = "UTF-8";
#endif /* HPE_mingw_BUILD */
	if (! strcmp(syslocale, "UTF-8")) {
		*dest = strdup(src);
		if (! *dest)
			return -LTFS_NO_MEMORY;
		return 0;
	}

	/* System locale doesn't match internal usage, so really do the conversion */
	destlen = ucnv_convert(NULL, "UTF-8", NULL, 0, src, -1, &err);
	if (U_FAILURE(err) && err != U_BUFFER_OVERFLOW_ERROR) {
		ltfsmsg(LTFS_ERR, "11250E", err);
		return -LTFS_ICU_ERROR;
	}
	err = U_ZERO_ERROR;

	*dest = malloc(destlen + 1);
	if (! *dest) {
		ltfsmsg(LTFS_ERR, "10001E", __FUNCTION__);
		return -LTFS_NO_MEMORY;
	}

	ucnv_convert(NULL, "UTF-8", *dest, destlen + 1, src, -1, &err);
	if (U_FAILURE(err)) {
		ltfsmsg(LTFS_ERR, "11251E", err);
		free(*dest);
		*dest = NULL;
		return -LTFS_ICU_ERROR;
	}

	return 0;
}
