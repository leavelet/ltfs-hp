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
** FILE NAME:       xml_reader.c
**
** DESCRIPTION:     XML parser routines for Indexes and Labels.
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
** Copyright (C) 2012 OSR Open Systems Resources, Inc.
** 
************************************************************************************* 
**
**  (C) Copyright 2015 - 2017 Hewlett Packard Enterprise Development LP
**  10/13/17 Added support for SNIA 2.4
**
*************************************************************************************
*/

#include "ltfs.h"
#include "xml_libltfs.h"
#include "fs.h"
#include "tape.h"
#include "base64.h"
#include "pathname.h"
#include "index_criteria.h"
#include "arch/time_internal.h"
#include "ltfsprintf.h"

/**
 * Read a text node, returning the buffer provided by libxml2.
 * Untraced to avoid big verbosity.
 * @param reader the XML source
 * @param value the returned buffer. *value is set to NULL on error. It must not be used after
 *              any subsequent read call to the XML parser.
 * @return 0 on success or a negative value on error.
 */
int xml_scan_text(xmlTextReaderPtr reader, const char **value)
{
	int type;

	if (xml_reader_read(reader) < 0)
		return -1;

	type = xmlTextReaderNodeType(reader);
	if (type == XML_ELEMENT_DECL)
		*value = "";
	else if (type == XML_TEXT_NODE || type == XML_DTD_NODE) {
		/* The type XML_DTD_NODE is produced if the text of the node consists of whitespace only.
		 * Since we also actually try to get the text, this does not lead to incorrect parsing. */
		*value = (const char *)xmlTextReaderConstValue(reader);
		if (!(*value)) {
			ltfsmsg(LTFS_ERR, "17035E");
			return -1;
		}
	} else {
		ltfsmsg(LTFS_ERR, "17036E", type);
		return -1;
	}

	return 0;
}

/**
 * Grabs tags from the stream until the a start tag is detected or the end of the containing tag
 * is found.
 * Untraced to avoid big verbosity.
 * @param reader XML source
 * @param containing_name Name of the parent tag
 * @param name Outputs the tag name
 * @param type Outputs the tag type, XML_ELEMENT_NODE for a start tag or XML_ELEMENT_DECL for the
 *             end of the containing tag.
 * @return 0 on success or a negative value on error
 */
int xml_next_tag(xmlTextReaderPtr reader, const char *containing_name,
	const char **name, int *type)
{
	do {
		if (xml_reader_read(reader) < 0)
			return -1;
		*name = (const char *)xmlTextReaderConstName(reader);
		*type = xmlTextReaderNodeType(reader);
	} while (strcmp(*name, containing_name) && (*type) != XML_ELEMENT_NODE);

	return 0;
}

/**
 * Skip to the last node of the current tag.
 * The reader must point to the start node of the tag (type XML_ELEMENT_NODE).
 * This is used to skip a tag and its contents entirely, e.g. when handling unknown tags.
 */
int xml_skip_tag(xmlTextReaderPtr reader)
{
	int ret, empty, type = XML_ELEMENT_NODE, depth, start_depth;

	depth = start_depth = xmlTextReaderDepth(reader);
	if (start_depth < 0) {
		ltfsmsg(LTFS_ERR, "17093E");
		return -1;
	}

	check_empty();
	while (! empty && (type != XML_ELEMENT_DECL || depth > start_depth)) {
		ret = xmlTextReaderRead(reader);
		if (ret < 0) {
			ltfsmsg(LTFS_ERR, "17093E");
			return -1;
		} else if (ret == 0) {
			ltfsmsg(LTFS_ERR, "17038E");
			return -1;
		}
		type = xmlTextReaderNodeType(reader);
		if (type < 0) {
			ltfsmsg(LTFS_ERR, "17093E");
			return -1;
		}
		depth = xmlTextReaderDepth(reader);
		if (depth < 0) {
			ltfsmsg(LTFS_ERR, "17093E");
			return -1;
		}
	}

	return 0;
}

/**
 * Store a tag to a list of preserved tags. The tags will be preserved when the
 * corresponding LTFS entity (index, file, directory) is written back to tape.
 * @param reader The XML source. It should be positioned at the start of an XML tag
 *               (node type XML_ELEMENT_DECL).
 * @param tag_count Pointer to size of the tag list, incremented on success.
 * @param tag_list Pointer to the tag list, updated on success.
 * @return 0 on success or -1 on error.
 */
int xml_save_tag(xmlTextReaderPtr reader, size_t *tag_count, unsigned char ***tag_list)
{
	size_t c = (*tag_count) + 1;
	unsigned char **t;
	unsigned char *tag_value;

#if LIBXML_VERSION < 20620
	/* OS X 10.5 ships with an old version of libxml2 that doesn't
	 * support xmlTextReaderReadOuterXml. */
	int ret, bufsize;
	xmlDocPtr doc;
	xmlNodePtr node;
	xmlBufferPtr buf;

	/* NOTE: caller must do xmlFreeDoc(xmlTextReaderCurrentDoc(reader)) when parsing is
	 * finished, as this call modifies the behavior of xmlFreeTextReader. */
	doc = xmlTextReaderCurrentDoc(reader);
	if (! doc) {
		ltfsmsg(LTFS_ERR, "17200E", "xmlTextReaderCurrentDoc");
		return -1;
	}
	node = xmlTextReaderExpand(reader);
	if (! node) {
		ltfsmsg(LTFS_ERR, "17200E", "xmlTextReaderExpand");
		return -1;
	}
	buf = xmlBufferCreate();
	if (! buf) {
		ltfsmsg(LTFS_ERR, "17200E", "xmlBufferCreate");
		return -1;
	}

	ret = xmlNodeDump(buf, doc, node, 0, 0);
	if (ret < 0) {
		ltfsmsg(LTFS_ERR, "17200E", "xmlNodeDump");
		return -1;
	}
	bufsize = xmlBufferLength(buf);
	tag_value = malloc(bufsize + 1);
	if (! tag_value) {
		xmlBufferFree(buf);
		ltfsmsg(LTFS_ERR, "10001E", "_xml_save_tag: tag value");
		return -1;
	}
	memcpy(tag_value, xmlBufferContent(buf), bufsize);
	tag_value[bufsize] = '\0';
	xmlBufferFree(buf);

#else
	tag_value = xmlTextReaderReadOuterXml(reader);
	if (! tag_value) {
		ltfsmsg(LTFS_ERR, "17091E");
		return -1;
	}
#endif /* __APPLE__ */

	t = realloc(*tag_list, c * sizeof(unsigned char *));
	if (! t) {
		ltfsmsg(LTFS_ERR, "10001E", __FUNCTION__);
		free(tag_value);
		return -1;
	}
	t[c-1] = tag_value;

	*tag_count = c;
	*tag_list = t;
	return 0;
}

/**
 * Read a node from the given XML stream.
 * Note: this function assumes that you really do want a node. That is, it will return an
 * error if it detects end of stream, even if validation went fine.
 * Untraced to avoid big verbosity.
 * @param reader the XML source
 * @return 0 on success or a negative value on error.
 */
int xml_reader_read(xmlTextReaderPtr reader)
{
	int ret = xmlTextReaderRead(reader);
	if (ret < 0) {
		ltfsmsg(LTFS_ERR, "17037E");
		return -1;
	} else if (ret == 0) {
		ltfsmsg(LTFS_ERR, "17038E");
		return -1;
	}
	return 0;
}

/**
 * Parse a UUID from the tape into a provided buffer, converting to lower-case. The output buffer
 * must be at least 37 bytes.
 */
int xml_parse_uuid(char *out_val, const char *val)
{
	int i;

	CHECK_ARG_NULL(val, -LTFS_NULL_ARG);

	if (strlen(val) != 36) {
		ltfsmsg(LTFS_ERR, "17029E", val);
		return -1;
	}
	strcpy(out_val, val);

	for (i=0; i<36; ++i) {
		if (i == 8 || i == 13 || i == 18 || i == 23) {
			if (val[i] != '-') {
				ltfsmsg(LTFS_ERR, "17029E", val);
				return -1;
			}
		} else {
			if ((val[i] < '0' || val[i] > '9') && (val[i] < 'a' || val[i] > 'f') &&
				(val[i] < 'A' || val[i] > 'F')) {
				ltfsmsg(LTFS_ERR, "17029E", val);
				return -1;
			}
		}

		/* convert to lower-case */
		if (val[i] >= 'A' && val[i] <= 'F')
			out_val[i] += 32;
	}

	return 0;
}

/**
 * Parse a file, directory, or xattr name. The name is normalized to NFC and checked for
 * length and invalid characters.
 * @param out_val On success, points to a newly allocated buffer holding the normalized name.
 * @param value Name to process.
 * @return 0 on success or a negative value on error.
 * HPE MD 21.09.2017 xattr names are no longer checked the same as file and directory names as
 * from SNIA 2.4 / is allowed in xattr name.  New function below to cover this.
 */
int xml_parse_filename(char **out_val, const char *value, int percentencoded)
{
	int ret;

	CHECK_ARG_NULL(out_val, -LTFS_NULL_ARG);
	CHECK_ARG_NULL(value, -LTFS_NULL_ARG);

	ret = pathname_normalize(value, out_val);
	if (ret < 0) {
		ltfsmsg(LTFS_ERR, "17030E", value);
		return ret;
	}
	else if (pathname_validate_file(*out_val, percentencoded) < 0) {
		ltfsmsg(LTFS_ERR, "17031E", "file / dir name", value);
		free(*out_val);
		*out_val = NULL;
		return -1;
	}

	return 0;
}

/**
* HPE new function to support SNIA 2.4
* Parse an xattr name. The name is normalized to NFC and checked for
* length and invalid characters.
* @param out_val On success, points to a newly allocated buffer holding the normalized name.
* @param value Name to process.
* @return 0 on success or a negative value on error.
*/
int xml_parse_xattrname(char **out_val, const char *value, int percentencoded)
{
    int ret;

    CHECK_ARG_NULL(out_val, -LTFS_NULL_ARG);
    CHECK_ARG_NULL(value, -LTFS_NULL_ARG);

    ret = pathname_normalize(value, out_val);
    if (ret < 0) {
        ltfsmsg(LTFS_ERR, "17030E", value);
        return ret;
    }
    else if (pathname_validate_xattr_name(*out_val, percentencoded) < 0) {
        ltfsmsg(LTFS_ERR, "17031E", "xattr name", value);
        free(*out_val);
        *out_val = NULL;
        return -1;
    }

    return 0;
}

/**
 * Parse a base-10 signed long long from a string.
 * This function does not print an error message because it usually doesn't
 * have enough information to say something helpful.
 */
int xml_parse_ll(long long *out_val, const char *val)
{
	size_t vallen;
	char *invalid_start;

	CHECK_ARG_NULL(out_val, -LTFS_NULL_ARG);
	CHECK_ARG_NULL(val, -LTFS_NULL_ARG);

	vallen = strlen(val);
	if (vallen == 0)
		return -1;

	*out_val = strtoll(val, &invalid_start, 10);
	if(*invalid_start != '\0')
		return -1;

	return 0;
}

/**
 * Parse a positive base-10 unsigned long long from a string.
 * This function does not print an error message because it usually doesn't
 * have enough information to say something helpful.
 */
int xml_parse_ull(unsigned long long *out_val, const char *val)
{
	size_t vallen;
	char *invalid_start;

	CHECK_ARG_NULL(out_val, -LTFS_NULL_ARG);
	CHECK_ARG_NULL(val, -LTFS_NULL_ARG);

	vallen = strlen(val);
	if (vallen == 0)
		return -1;

	*out_val = strtoull(val, &invalid_start, 10);
	if(*invalid_start != '\0')
		return -1;

	return 0;
}

/**
 * Parse a positive base-16 unsigned long long from a string.
 * This function does not print an error message because it usually doesn't
 * have enough information to say something helpful.
 */
int xml_parse_xll(unsigned long long *out_val, const char *val)
{
	size_t vallen;
	char *invalid_start;

	CHECK_ARG_NULL(out_val, -LTFS_NULL_ARG);
	CHECK_ARG_NULL(val, -LTFS_NULL_ARG);

	vallen = strlen(val);
	if (vallen == 0)
		return -1;

	*out_val = strtoull(val, &invalid_start, 16);
	if(*invalid_start != '\0')
		return -1;

	return 0;
}

/**
 * Parse a boolean value from a string. Per the W3C boolean datatype, "true" and "1" return true
 * and "false" and "0" return false.
 * @return 0 on success or a negative value on error.
 */
int xml_parse_bool(bool *out_val, const char *value)
{
	CHECK_ARG_NULL(out_val, -LTFS_NULL_ARG);
	CHECK_ARG_NULL(value, -LTFS_NULL_ARG);

	if (! strcmp(value, "true") || ! strcmp(value, "1"))
		*out_val = true;
	else if (! strcmp(value, "false") || ! strcmp(value, "0"))
		*out_val = false;
	else {
		ltfsmsg(LTFS_ERR, "17032E");
		return -1;
	}

	return 0;
}

/**
 * Parse a time from the XML file into a timespec structure.
 */
int xml_parse_time(bool msg, const char *fmt_time, struct ltfs_timespec *rawtime)
{
	struct tm tm;
	int ret;

	CHECK_ARG_NULL(fmt_time, -LTFS_NULL_ARG);
	CHECK_ARG_NULL(rawtime, -LTFS_NULL_ARG);

	ret = sscanf(fmt_time, "%d-%2d-%2dT%2d:%2d:%2d.%9ldZ",
				 &tm.tm_year, &tm.tm_mon, &tm.tm_mday,
				 &tm.tm_hour, &tm.tm_min, &tm.tm_sec,
				 &rawtime->tv_nsec);
	if( ret != 7 ) {
		if (msg)
			ltfsmsg(LTFS_ERR, "17034E", fmt_time, ret);
		return -1;
	}

	tm.tm_year -= 1900;
	tm.tm_mon -= 1;

	/*
	 * convert parsed UTC time back to Unix time.
	 */
	rawtime->tv_sec = ltfs_timegm(&tm);
	ret = normalize_ltfs_time(rawtime);

	return ret;
}

/**
 * Read callback for XML parser input using the libxml2 I/O routines. libxml2 reads
 * data in small, fixed-size chunks (typically 4096 bytes), so each block read from tape
 * gets buffered until it is used up.
 * This function detects whether the Index being parsed ends in a file mark, and if so, it
 * positions the tape before the file mark.
 */
int xml_input_tape_read_callback(void *context, char *buffer, int len)
{
	struct xml_input_tape *ctx = context;
	ssize_t nread, nr2;
	char *buf2;
	int bytes_saved, bytes_remaining;

	if (len == 0)
		return 0;

	/* Try to fill the whole buffer from cache. If that fails, try to read from tape. */
	if (len <= (int32_t) ctx->buf_used) {
		memcpy(buffer, ctx->buf + ctx->buf_start, len);
		ctx->buf_used -= len;
		if (ctx->buf_used > 0)
			ctx->buf_start += len;
		else
			ctx->buf_start = 0;
	} else {
		if (ctx->buf_used > 0) {
			memcpy(buffer, ctx->buf + ctx->buf_start, ctx->buf_used);
			bytes_saved = ctx->buf_used;
			ctx->buf_used = 0;
			ctx->buf_start = 0;
		} else
			bytes_saved = 0;
		bytes_remaining = len - bytes_saved;

		while (bytes_remaining > 0) {
			/* If we've reached EOD, we're at the end of the Index. */
			if (ctx->eod_pos > 0 && ctx->current_pos == ctx->eod_pos)
				return bytes_saved;

			/* If we've exhausted a small block, we're at the end of the Index. */
			if (ctx->saw_small_block)
				return bytes_saved;

			/* Try to read a block into the buffer. */
			nread = tape_read(ctx->vol->device, ctx->buf, ctx->buf_size, false,
				ctx->vol->kmi_handle);
			++ctx->current_pos;
			if (nread < 0) {
				/* We know we're not at EOD, so read errors are unexpected. */
				ltfsmsg(LTFS_ERR, "17039E", (int)nread);
				return -1;
			} else if ((size_t) nread < ctx->buf_size) {
				/* Caught a small read. If this is a file mark, position before it. If
				 * it's a record, look for a file mark following it. */
				ctx->saw_small_block = true;
				if (nread == 0) {
					ctx->saw_file_mark = true;
					if (tape_spacefm(ctx->vol->device, -1) < 0) {
						ltfsmsg(LTFS_ERR, "17040E");
						return -1;
					}
				} else if (ctx->eod_pos == 0 ||
					(ctx->eod_pos > 0 && ctx->current_pos < ctx->eod_pos)) {
					/* Look for a trailing file mark. */
					buf2 = malloc(ctx->vol->label->blocksize);
					if (! buf2) {
						ltfsmsg(LTFS_ERR, "10001E", __FUNCTION__);
						return -1;
					}
					nr2 = tape_read(ctx->vol->device, buf2, ctx->vol->label->blocksize, false,
						ctx->vol->kmi_handle);
					free(buf2);
					errno = 0; /* Clear errno because some OSs set errno in free() */
					if (nr2 < 0) { /* Still not at EOD, so read errors are cause for alarm. */
						ltfsmsg(LTFS_ERR, "17041E", (int)nr2);
						return -1;
					} else if (nr2 == 0) {
						ctx->saw_file_mark = true;
						if (tape_spacefm(ctx->vol->device, -1) < 0) {
							ltfsmsg(LTFS_ERR, "17040E");
							return -1;
						}
					}
				}
			}

			/* Fill the output buffer (and the cache, if there are bytes left over). */
			if (bytes_remaining > nread) {
				memcpy(buffer + bytes_saved, ctx->buf, nread);
				bytes_saved += nread;
				bytes_remaining -= nread;
			} else {
				memcpy(buffer + bytes_saved, ctx->buf, bytes_remaining);
				ctx->buf_start = bytes_remaining;
				ctx->buf_used = nread - bytes_remaining;
				bytes_saved = len;
				bytes_remaining = 0;
			}
		}
	}

	return len;
}

/**
 * Close callback for XML parser input using the libxml2 I/O routines. The input
 * buffer should be empty at this point, so just free the parser context.
 */
int xml_input_tape_close_callback(void *context)
{
	struct xml_input_tape *ctx = context;
	free(ctx->buf);
	free(ctx);
	return 0;
}
