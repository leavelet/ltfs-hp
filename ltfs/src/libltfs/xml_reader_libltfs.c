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
** FILE NAME:       xml_reader_libltfs.c
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
**  (C) Copyright 2015 - 2017 Hewlett Packard Enterprise Development LP
**  10/13/17 Added support for SNIA 2.4
**
*************************************************************************************
*/

#include <libxml/xmlstring.h>
#include <libxml/xmlreader.h>

#include "ltfs.h"
#include "xml_libltfs.h"
#include "fs.h"
#include "tape.h"
#include "base64.h"
#include "pathname.h"
#include "index_criteria.h"
#include "arch/time_internal.h"
#include "ltfsprintf.h"

/* LTFS index version checks */
#define IDX_VERSION_SPARSE     MAKE_LTFS_VERSION(2,0,0)
#define IDX_VERSION_BACKUPTIME MAKE_LTFS_VERSION(2,0,0)
#define IDX_VERSION_UID        MAKE_LTFS_VERSION(2,0,0)

/* Label tag parsers */
int _xml_parse_label(xmlTextReaderPtr reader, struct ltfs_label *label);
int _xml_parse_label_location(xmlTextReaderPtr reader, struct ltfs_label *label);
int _xml_parse_partition_map(xmlTextReaderPtr reader, struct ltfs_label *label);

/* Schema tag parsers */
int _xml_parse_schema(xmlTextReaderPtr reader, struct ltfs_index *idx, struct ltfs_volume *vol);
int _xml_parse_policy(xmlTextReaderPtr reader, struct ltfs_index *idx);
int _xml_parse_ip_criteria(xmlTextReaderPtr reader, struct ltfs_index *idx);
int _xml_parse_dirtree(xmlTextReaderPtr reader, struct dentry *parent,
	struct ltfs_index *idx, struct ltfs_volume *vol, struct name_list *list);
int _xml_parse_dir_contents(xmlTextReaderPtr reader, struct dentry *dir, struct ltfs_index *idx);
int _xml_parse_file(xmlTextReaderPtr reader, struct ltfs_index *idx, struct dentry *dir, struct name_list *list);
int _xml_parse_extents(xmlTextReaderPtr reader, int idx_version, struct dentry *d);
int _xml_parse_one_extent(xmlTextReaderPtr reader, int idx_version, struct dentry *d);
int _xml_parse_xattrs(xmlTextReaderPtr reader, struct dentry *d);
int _xml_parse_one_xattr(xmlTextReaderPtr read, struct dentry *d);
int _xml_scan_tapepos(xmlTextReaderPtr reader, const char *tag, struct tape_offset *pos);

/* Generic tag parsers */
int _xml_parser_init(xmlTextReaderPtr reader, const char *top_name, int *idx_version,
	int min_version, int max_version);

/* Value parsers */
int _xml_parse_version(const char *version_str, int *version_int);
int _xml_parse_partition(const char *val);

/* Save symlink/extent conflicted dentries */
int _xml_save_symlink_conflict(struct ltfs_index *idx, struct dentry *d);

int xml_label_from_file(const char *filename, struct ltfs_label *label)
{
	int ret;
	xmlTextReaderPtr reader;

	CHECK_ARG_NULL(filename, -LTFS_NULL_ARG);
	CHECK_ARG_NULL(label, -LTFS_NULL_ARG);

	reader = xmlReaderForFile(filename, NULL, XML_PARSE_NOERROR | XML_PARSE_NOWARNING);
	if (! reader) {
		ltfsmsg(LTFS_ERR, "17007E", filename);
		return -1;
	}

	ret = _xml_parse_label(reader, label);
	if (ret < 0)
		ltfsmsg(LTFS_ERR, "17008E", filename);
	xmlFreeTextReader(reader);

	return ret;
}

int xml_label_from_mem(const char *buf, int buf_size, struct ltfs_label *label)
{
	int ret;
	xmlTextReaderPtr reader;

	CHECK_ARG_NULL(buf, -LTFS_NULL_ARG);
	CHECK_ARG_NULL(label, -LTFS_NULL_ARG);

	reader = xmlReaderForMemory(buf, buf_size, NULL, NULL, XML_PARSE_NOERROR | XML_PARSE_NOWARNING);
	if (! reader) {
		ltfsmsg(LTFS_ERR, "17009E");
		return -LTFS_LIBXML2_FAILURE;
	}

	ret = _xml_parse_label(reader, label);
	if (ret < 0) {
		ltfsmsg(LTFS_ERR, "17010E");
		ret = -LTFS_LABEL_INVALID;
	}
	xmlFreeTextReader(reader);

	return ret;
}

/**
 * Parse an XML schema file and populate the priv->root virtual dentry tree
 * with the nodes found during the scanning.
 * @param filename XML input file.
 * @param idx LTFS index.
 * @param vol LTFS volume to which the index belongs. May be NULL.
 * @return 0 on success or a negative value on error.
 */
int xml_schema_from_file(const char *filename, struct ltfs_index *idx, struct ltfs_volume *vol)
{
	int ret;
	xmlTextReaderPtr reader;
	xmlDocPtr doc;

	CHECK_ARG_NULL(filename, -LTFS_NULL_ARG);
	CHECK_ARG_NULL(idx, -LTFS_NULL_ARG);

	reader = xmlReaderForFile(filename, NULL, XML_PARSE_NOERROR | XML_PARSE_NOWARNING);
	if (! reader) {
		ltfsmsg(LTFS_ERR, "17011E", filename);
		return -1;
	}

	/* Workaround for old libxml2 version on OS X 10.5: the method used to preserve
	 * unknown tags modifies the behavior of xmlFreeTextReader so that an additional
	 * xmlDocFree call is required to free all memory. */
	doc = xmlTextReaderCurrentDoc(reader);
	ret = _xml_parse_schema(reader, idx, vol);
	if (ret < 0)
		ltfsmsg(LTFS_ERR, "17012E", filename);
	if (doc)
		xmlFreeDoc(doc);
	xmlFreeTextReader(reader);

#ifdef DEBUG
	/* dump the tree if it isn't too large */
	if (ret == 0 && idx->file_count < 1000)
		fs_dump_tree(idx->root);
#endif

	return ret;
}

/**
 * Parse an Index from tape and populate the vol->index->root virtual dentry tree
 * with the nodes found during the scanning.
 * If a file mark is encountered at the end of the Index, the tape is positioned before
 * the file mark.
 * @param eod_pos EOD block position for the current partition, or 0 to assume EOD will not be
 *                encountered during parsing.
 * @param vol LTFS volume.
 * @return 0 on success, 1 if parsing succeeded but no file mark was encountered,
 *         or a negative value on error.
 */
int xml_schema_from_tape(uint64_t eod_pos, struct ltfs_volume *vol)
{
	int ret;
	struct tc_position current_pos;
	struct xml_input_tape *ctx;
	xmlParserInputBufferPtr read_buf;
	xmlTextReaderPtr reader;
	xmlDocPtr doc;

	CHECK_ARG_NULL(vol, -LTFS_NULL_ARG);

	ret = tape_get_position(vol->device, &current_pos);
	if (ret < 0) {
		ltfsmsg(LTFS_ERR, "17013E", ret);
		return ret;
	}

	/* Create output callback context data structure. */
	ctx = malloc(sizeof(struct xml_input_tape));
	if (! ctx) {
		ltfsmsg(LTFS_ERR, "10001E", "xml_schema_from_tape: ctx");
		return -LTFS_NO_MEMORY;
	}
	ctx->buf = malloc(vol->label->blocksize + LTFS_CRC_SIZE);
	if (! ctx->buf) {
		ltfsmsg(LTFS_ERR, "10001E", "xml_schema_from_tape: ctx->buf");
		free(ctx);
		return -LTFS_NO_MEMORY;
	}
	ctx->vol = vol;
	ctx->current_pos = current_pos.block;
	ctx->eod_pos = eod_pos;
	ctx->saw_small_block = false;
	ctx->saw_file_mark = false;
	ctx->buf_size = vol->label->blocksize;
	ctx->buf_start = 0;
	ctx->buf_used = 0;

	/* Create input buffer pointer. */
	read_buf = xmlParserInputBufferCreateIO(xml_input_tape_read_callback,
											xml_input_tape_close_callback,
											ctx, XML_CHAR_ENCODING_NONE);
	if (! read_buf) {
		ltfsmsg(LTFS_ERR, "17014E");
		free(ctx->buf);
		free(ctx);
		return -LTFS_LIBXML2_FAILURE;
	}

	/* Create XML reader. */
	reader = xmlNewTextReader(read_buf, NULL);
	if (! reader) {
		ltfsmsg(LTFS_ERR, "17015E");
		xmlFreeParserInputBuffer(read_buf);
		return -LTFS_LIBXML2_FAILURE;
	}

	/* Workaround for old libxml2 version on OS X 10.5. See comment in xml_schema_from_file()
	 * for details. */
	doc = xmlTextReaderCurrentDoc(reader);

	/* Generate the Index. */
	ret = _xml_parse_schema(reader, vol->index, vol);
	if (ret < 0) {
		ltfsmsg(LTFS_ERR, "17016E");
		if ((ret != -LTFS_UNSUPPORTED_INDEX_VERSION)&&( ret != -LTFS_SYMLINK_CONFLICT)) {
			if (ret == -LTFS_NO_MEMORY)
				ret = -LTFS_NO_MEMORY;
			else
				ret = -LTFS_INDEX_INVALID;
		}
	} else if (ret == 0) {
		if( ! ctx->saw_file_mark)
			ret = 1;
	}
	if (doc)
		xmlFreeDoc(doc);
	xmlFreeTextReader(reader);
	xmlFreeParserInputBuffer(read_buf);

#ifdef DEBUG
	/* dump the tree if it isn't too large */
	if (ret >= 0 && vol->index->file_count < 1000)
		fs_dump_tree(vol->index->root);
#endif

	return ret;
}

static int _xml_parse_symlink_target(xmlTextReaderPtr reader, int idx_version, struct dentry *d)
{
	declare_parser_vars_symlinknode("symlink");
	declare_tracking_arrays(1, 0);

	while (true) {
		get_next_tag();

		if (! strcmp(name, "target")) {
			get_tag_text();
			d->isslink = true;
			d->target = strdup(value);
		} else
			ignore_unrecognized_tag();
	}

	return 0;
}

static int xml_symlinkinfo_from_file(const char *filename, struct dentry *d)
{
	declare_parser_vars_symlink("symlink");
	xmlTextReaderPtr reader;
	xmlDocPtr doc;
	int ret = 0;

	CHECK_ARG_NULL(filename, -LTFS_NULL_ARG);
	CHECK_ARG_NULL(d, -LTFS_NULL_ARG);

	reader = xmlReaderForFile(filename, NULL, XML_PARSE_NOERROR | XML_PARSE_NOWARNING);
	if (! reader) {
		ltfsmsg(LTFS_ERR, "17011E", filename);
		return -1;
	}

	/* Workaround for old libxml2 version on OS X 10.5: the method used to preserve
	 * unknown tags modifies the behavior of xmlFreeTextReader so that an additional
	 * xmlDocFree call is required to free all memory. */
	doc = xmlTextReaderCurrentDoc(reader);

	while (true) {
		get_next_tag();
		if (! strcmp(name, "symlink")) {
			ret = _xml_parse_symlink_target(reader, IDX_VERSION_SPARSE, d);
			if (ret < 0) {
				/* XML parser: failed to read extent list from file (%d) */
				ltfsmsg(LTFS_ERR, "17084E", ret);
			}
		}
		break;
	}

	if (doc)
		xmlFreeDoc(doc);
	xmlFreeTextReader(reader);

	return ret;
}

/**
 * Parse an extent list from a file and populate provided dentry with the extents read during
 * the scanning.
 *
 * @param filename File name from where to read the extent list from.
 * @param d Dentry where the extents are to be appended to.
 * @return 0 on success or a negative value on error.
 */
static int xml_extentlist_from_file(const char *filename, struct dentry *d)
{
	declare_extent_parser_vars("extentinfo");
	xmlTextReaderPtr reader;
	xmlDocPtr doc;
	int ret = 0;

	CHECK_ARG_NULL(filename, -LTFS_NULL_ARG);
	CHECK_ARG_NULL(d, -LTFS_NULL_ARG);

	reader = xmlReaderForFile(filename, NULL, XML_PARSE_NOERROR | XML_PARSE_NOWARNING);
	if (! reader) {
		ltfsmsg(LTFS_ERR, "17011E", filename);
		return -1;
	}

	/* Workaround for old libxml2 version on OS X 10.5: the method used to preserve
	 * unknown tags modifies the behavior of xmlFreeTextReader so that an additional
	 * xmlDocFree call is required to free all memory. */
	doc = xmlTextReaderCurrentDoc(reader);

	while (true) { /* BEAM: loop doesn't iterate - Because get_next_tag() macro uses "break", at most once loop is needed here. */
		get_next_tag();
		if (! strcmp(name, "extentinfo")) {
			ret = _xml_parse_extents(reader, IDX_VERSION_SPARSE, d);
			if (ret < 0) {
				/* XML parser: failed to read extent list from file (%d) */
				ltfsmsg(LTFS_ERR, "17084E", ret);
			}
		}
		break;
	}

	if (doc)
		xmlFreeDoc(doc);
	xmlFreeTextReader(reader);

	return ret;
}

int xml_extent_symlink_info_from_file(const char *filename, struct dentry *d)
{
	int ret;

	CHECK_ARG_NULL(filename, -LTFS_NULL_ARG);
	CHECK_ARG_NULL(d, -LTFS_NULL_ARG);

	ret = xml_extentlist_from_file(filename, d);

	if (d->realsize==0) {
		ret = xml_symlinkinfo_from_file(filename, d);
	}

	return ret;
}

/**
 * Start parsing an XML stream by checking the encoding and version.
 */
int _xml_parser_init(xmlTextReaderPtr reader, const char *top_name, int *idx_version,
	int min_version, int max_version)
{
	const char *name, *encoding;
	char *value;
	int type, ver;

	if (xml_next_tag(reader, "", &name, &type) < 0)
		return -1;
	if (strcmp(name, top_name)) {
		ltfsmsg(LTFS_ERR, "17017E", name);
		return -1;
	}

	/* reject this XML file if it isn't UTF-8 */
	encoding = (const char *)xmlTextReaderConstEncoding(reader);
	if (! encoding || strcmp(encoding, "UTF-8")) {
		ltfsmsg(LTFS_ERR, "17018E", encoding);
		return -1;
	}

	/* check the version attribute of the top-level tag */
	value = (char *)xmlTextReaderGetAttribute(reader, BAD_CAST "version");
	if (! value) {
		ltfsmsg(LTFS_ERR, "17019E");
		return -1;
	}
	if (_xml_parse_version(value, &ver) < 0) {
		ltfsmsg(LTFS_ERR, "17020E", value);
		return -LTFS_UNSUPPORTED_INDEX_VERSION;
	}
	if (ver < min_version || ver > max_version) {
		ltfsmsg(LTFS_ERR, "17021E", top_name, value);
		free(value);
		return -LTFS_UNSUPPORTED_INDEX_VERSION;
	}
	*idx_version = ver;
	
    /* 
     * OSR
     *
     * This allocation must be freed with xmlFree
     * 
     */
#ifndef HPE_mingw_BUILD
	free(value);
#else
	xmlFree(value);
#endif	

	return 0;
}

/**
 * Parse an XML label, populating the given label data structure.
 */
int _xml_parse_label(xmlTextReaderPtr reader, struct ltfs_label *label)
{
	int ret;
	unsigned long long value_int;
	declare_parser_vars("ltfslabel");
	declare_tracking_arrays(7, 0);

	/* start the parser: find top-level "label" tag, check version and encoding */
	if (_xml_parser_init(reader, parent_tag, &label->version,
		LTFS_LABEL_VERSION_MIN, LTFS_LABEL_VERSION_MAX) < 0)
		return -1;

	/* parse label contents */
	while (true) {
		get_next_tag();

		if (! strcmp(name, "creator")) {
			check_required_tag(0);
			get_tag_text();
			if (label->creator) {
				free(label->creator); 
				label->creator = NULL;
			}
			label->creator = strdup(value);
			if (! label->creator) {
				ltfsmsg(LTFS_ERR, "10001E", name);
				return -1;
			}
			check_tag_end("creator");

		} else if (! strcmp(name, "formattime")) {
			check_required_tag(1);
			get_tag_text();
			ret = xml_parse_time(true, value, &label->format_time);
			if (ret < 0)
				return -1;
			else if (ret == LTFS_TIME_OUT_OF_RANGE)
				ltfsmsg(LTFS_WARN, "17218W", "formattime", value);
			check_tag_end("formattime");

		} else if (! strcmp(name, "volumeuuid")) {
			check_required_tag(2);
			get_tag_text();
			if (xml_parse_uuid(label->vol_uuid, value) < 0)
				return -1;
			check_tag_end("volumeuuid");

		} else if (! strcmp(name, "location")) {
			check_required_tag(3);
			assert_not_empty();
			if (_xml_parse_label_location(reader, label) < 0)
				return -1;

		} else if (! strcmp(name, "partitions")) {
			check_required_tag(4);
			assert_not_empty();
			if (_xml_parse_partition_map(reader, label) < 0)
				return -1;

		} else if (! strcmp(name, "blocksize")) {
			check_required_tag(5);
			get_tag_text();
			if (xml_parse_ull(&value_int, value) < 0 || value_int == 0) {
				ltfsmsg(LTFS_ERR, "17022E", value);
				return -1;
			}
			label->blocksize = value_int;
			check_tag_end("blocksize");

		} else if (! strcmp(name, "compression")) {
			check_required_tag(6);
			get_tag_text();
			if (xml_parse_bool(&label->enable_compression, value) < 0)
				return -1;
			check_tag_end("compression");

		} else
			ignore_unrecognized_tag();
	}

	check_required_tags();
	return 0;
}

/**
 * Parse a partition location from a label.
 */
int _xml_parse_label_location(xmlTextReaderPtr reader, struct ltfs_label *label)
{
	declare_parser_vars("location");
	declare_tracking_arrays(1, 0);

	while (true) {
		get_next_tag();

		if (! strcmp(name, "partition")) {
			check_required_tag(0);
			get_tag_text();
			if (_xml_parse_partition(value) < 0)
				return -1;
			label->this_partition = value[0];
			check_tag_end("partition");

		} else
			ignore_unrecognized_tag();
	}

	check_required_tags();
	return 0;
}

/**
 * Parse a partition map from an XML file, storing it in the given label structure.
 */
int _xml_parse_partition_map(xmlTextReaderPtr reader, struct ltfs_label *label)
{
	declare_parser_vars("partitions");
	declare_tracking_arrays(2, 0);

	while (true) {
		get_next_tag();

		if (! strcmp(name, "index")) {
			check_required_tag(0);
			get_tag_text();
			if (_xml_parse_partition(value) < 0)
				return -1;
			label->partid_ip = value[0];
			check_tag_end("index");

		} else if (! strcmp(name, "data")) {
			check_required_tag(1);
			get_tag_text();
			if (_xml_parse_partition(value) < 0)
				return -1;
			label->partid_dp = value[0];
			check_tag_end("data");

		} else
			ignore_unrecognized_tag();
	}

	check_required_tags();
	return 0;
}

/**
 * Parse an index file from the given source and populate the priv->root virtual dentry tree.
 * with the nodes found during the scanning.
 * @param reader Source of XML data
 * @param idx LTFS index
 * @param vol LTFS volume to which the index belongs. May be NULL.
 * @return 0 on success or a negative value on error.
 */
int _xml_parse_schema(xmlTextReaderPtr reader, struct ltfs_index *idx, struct ltfs_volume *vol)
{
	int ret;
	unsigned long long value_int;
	declare_parser_vars("ltfsindex");
	declare_tracking_arrays(8, 4);

	/* start the parser: find top-level "index" tag, check version and encoding */
	ret = _xml_parser_init(reader, parent_tag, &idx->version,
						   LTFS_INDEX_VERSION_MIN, LTFS_INDEX_VERSION_MAX);
	if (ret < 0)
		return ret;

	if (idx->version < LTFS_INDEX_VERSION)
		ltfsmsg(LTFS_WARN, "17095W",
				LTFS_INDEX_VERSION_STR,
				LTFS_FORMAT_MAJOR(idx->version),
				LTFS_FORMAT_MINOR(idx->version),
				LTFS_FORMAT_REVISION(idx->version));
	else if (idx->version / 100 > LTFS_INDEX_VERSION / 100)
		ltfsmsg(LTFS_WARN, "17096W",
				LTFS_INDEX_VERSION_STR,
				LTFS_FORMAT_MAJOR(idx->version),
				LTFS_FORMAT_MINOR(idx->version),
				LTFS_FORMAT_REVISION(idx->version));
	else if (idx->version > LTFS_INDEX_VERSION)
		ltfsmsg(LTFS_WARN, "17234W",
				LTFS_INDEX_VERSION_STR,
				LTFS_FORMAT_MAJOR(idx->version),
				LTFS_FORMAT_MINOR(idx->version),
				LTFS_FORMAT_REVISION(idx->version));

	if (idx->commit_message) {
		free(idx->commit_message);
		idx->commit_message = NULL;
	}

	/* parse index file contents */
	while (true) {
		get_next_tag();

		if (! strcmp(name, "creator")) {
			check_required_tag(0);
			get_tag_text();
			if (idx->creator)
				free(idx->creator);
			idx->creator = strdup(value);
			if (! idx->creator) {
				ltfsmsg(LTFS_ERR, "10001E", name);
				return -1;
			}
			check_tag_end("creator");

		} else if (! strcmp(name, "volumeuuid")) {
			check_required_tag(1);
			get_tag_text();
			if (xml_parse_uuid(idx->vol_uuid, value) < 0)
				return -1;
			check_tag_end("volumeuuid");

		} else if (! strcmp(name, "generationnumber")) {
			check_required_tag(2);
			get_tag_text();
			if (xml_parse_ull(&value_int, value) < 0) {
				ltfsmsg(LTFS_ERR, "17023E", value);
				return -1;
			}
			idx->generation = value_int;
			check_tag_end("generationnumber");

		} else if (! strcmp(name, "updatetime")) {
			check_required_tag(3);
			get_tag_text();
			ret = xml_parse_time(true, value, &idx->mod_time);
			if (ret < 0)
				return -1;
			else if (ret == LTFS_TIME_OUT_OF_RANGE)
				ltfsmsg(LTFS_WARN, "17219W", "updatetime", value);

			check_tag_end("updatetime");

		} else if (! strcmp(name, "location")) {
			check_required_tag(4);
			assert_not_empty();
			if (_xml_scan_tapepos(reader, "location", &idx->selfptr) < 0)
				return -1;

		} else if (! strcmp(name, "allowpolicyupdate")) {
			check_required_tag(5);
			get_tag_text();
			if (xml_parse_bool(&idx->criteria_allow_update, value) < 0)
				return -1;
			check_tag_end("allowpolicyupdate");

		} else if (! strcmp(name, "directory")) {
			check_required_tag(6);
			assert_not_empty();
			if ((ret = _xml_parse_dirtree(reader, NULL, idx, vol, NULL)) < 0) {
				if (ret == -LTFS_NO_MEMORY)
					return -LTFS_NO_MEMORY;
				else
					return -1;
			}

		} else if (! strcmp(name, "previousgenerationlocation")) {
			check_optional_tag(0);
			assert_not_empty();
			if (_xml_scan_tapepos(reader, "previousgenerationlocation", &idx->backptr) < 0)
				return -1;

		} else if (! strcmp(name, "dataplacementpolicy")) {
			check_optional_tag(1);
			assert_not_empty();
			if (_xml_parse_policy(reader, idx) < 0)
				return -1;

		} else if (! strcmp(name, "comment")) {
			check_optional_tag(2);
			get_tag_text();
			if (strlen(value) > INDEX_MAX_COMMENT_LEN) {
				ltfsmsg(LTFS_ERR, "17094E");
				return -1;
			}
			idx->commit_message = strdup(value);
			if (! idx->commit_message) {
				ltfsmsg(LTFS_ERR, "10001E", "_xml_parse_schema: index comment");
				return -1;
			}
			check_tag_end("comment");

		} else if (! strcmp(name, "volumelockstate")) {
			check_optional_tag(3);
			get_tag_text();
			if (idx->volumelockstate)
				free(idx->volumelockstate);
			idx->volumelockstate = strdup(value);
			if (! idx->volumelockstate) {
				ltfsmsg(LTFS_ERR, "10001E", name);
				return -1;
			}
			check_tag_end("volumelockstate");
		} else if (idx->version >= IDX_VERSION_UID && ! strcmp(name, NEXTUID_TAGNAME)) {
			check_required_tag(7);
			get_tag_text();
			if (xml_parse_ull(&value_int, value) < 0)
				return -1;
			if (value_int > idx->uid_number)
				idx->uid_number = value_int;
			check_tag_end(NEXTUID_TAGNAME);
		} else if (! strcmp(name, NEXTUID_TAGNAME)) {
			ignore_unrecognized_tag();

		} else
			preserve_unrecognized_tag(idx);
	}

	/* For older index versions, assume we handle UIDs correctly.
	 * The idx->uid_number field is automatically initialized to 1, so it will be set correctly
	 * once all files and directories are parsed. */
	if (idx->version < IDX_VERSION_UID)
		check_required_tag(7);

	check_required_tags();

	if ( idx->symerr_count != 0 ) {
		return -LTFS_SYMLINK_CONFLICT;
	}


	return 0;
}

/**
 * Parse a data placement policy.
 */
int _xml_parse_policy(xmlTextReaderPtr reader, struct ltfs_index *idx)
{
	declare_parser_vars("dataplacementpolicy");
	declare_tracking_arrays(1, 0);

	/* get rid of unused variable warning */
	(void) value;

	/* parse the contents of the policy tag */
	while (true) {
		get_next_tag();

		if (! strcmp(name, "indexpartitioncriteria")) {
			check_required_tag(0);
			assert_not_empty();
			if (_xml_parse_ip_criteria(reader, idx) < 0)
				return -1;

		} else
			ignore_unrecognized_tag();
	}

	check_required_tags();
	return 0;
}

/**
 * Parse index partition criteria.
 */
int _xml_parse_ip_criteria(xmlTextReaderPtr reader, struct ltfs_index *idx)
{
	int ret;
	unsigned long long value_int;
	char *glob_norm;
	int num_patterns = 0;
	declare_parser_vars("indexpartitioncriteria");
	declare_tracking_arrays(1, 0);

	/* clear the glob pattern list first */
	index_criteria_free(&idx->original_criteria);
	index_criteria_free(&idx->index_criteria);

	/* We have a policy. */
	idx->original_criteria.have_criteria = true;

	while (true) {
		get_next_tag();

		if (! strcmp(name, "size")) {
			check_required_tag(0);
			get_tag_text();
			if (xml_parse_ull(&value_int, value) < 0) {
				ltfsmsg(LTFS_ERR, "17024E", value);
				return -1;
			}
			idx->original_criteria.max_filesize_criteria = value_int;
			check_tag_end("size");

		} else if (! strcmp(name, "name")) {
			get_tag_text();

			// HPE MD 22.09.2017 function was changed for SNIA 2.4 extra param 0 
			// will cause function to perform as before.
			if (pathname_validate_file(value, 0) < 0) {
				ltfsmsg(LTFS_ERR, "17098E", value);
				return -1;
			}

			++num_patterns;
			/* quite inefficient, but the number of patterns should be small. */
			idx->original_criteria.glob_patterns = realloc(idx->original_criteria.glob_patterns,
				(num_patterns + 1) * sizeof(char *));
			if (! idx->original_criteria.glob_patterns) {
				ltfsmsg(LTFS_ERR, "10001E", __FUNCTION__);
				return -1;
			}
			idx->original_criteria.glob_patterns[num_patterns] = NULL;

			ret = pathname_normalize(value, &glob_norm);
			if (ret < 0) {
				ltfsmsg(LTFS_ERR, "17025E", ret);
				return ret;
			}
			idx->original_criteria.glob_patterns[num_patterns - 1] = glob_norm;

			check_tag_end("name");

		} else
			ignore_unrecognized_tag();
	}

	/* Make an active copy of these index criteria. The caller can override idx->index_criteria
	 * later without affecting the criteria stored in future indexes (idx->original_criteria). */
	if (index_criteria_dup_rules(&idx->index_criteria, &idx->original_criteria) < 0) {
		/* Could not duplicate index criteria rules */
		ltfsmsg(LTFS_ERR, "11301E");
		return -1;
	}

	check_required_tags();
	return 0;
}

/**
 * Parse a directory tree from the given XML source into the given index data structure.
 * @param reader the XML source
 * @param parent Directory where the new subdirectory should be created, or NULL to populate the
 *               root dentry.
 * @param idx LTFS index data
 * @param vol LTFS volume to which the index belongs. May be NULL.
 * @return 0 on success or a negative value on error
 *
 * HPE MD 22.09.2017 Changes made to support SNIA 2.4 section 7.4 percent encoding 
 */
int _xml_parse_dirtree(xmlTextReaderPtr reader, struct dentry *parent,
	struct ltfs_index *idx, struct ltfs_volume *vol, struct name_list *dirname)
{
	int ret;
	int percentencoded = 0;
	unsigned long long value_int;
   char *dir_percent_encoded;
	struct dentry *dir;
	declare_parser_vars("directory");
	declare_tracking_arrays(9, 1);

	if (! parent && idx->root) {
		dir = idx->root;
		dir->vol = vol;
	} else {
		dir = fs_allocate_dentry(parent, NULL, NULL, true, false, false, idx);
		if (! dir) {
			ltfsmsg(LTFS_ERR, "10001E", __FUNCTION__);
			return -LTFS_NO_MEMORY;
		}
		if (! parent) {
			idx->root = dir;
			dir->vol = vol;
			++dir->link_count;
		}
	}

	while (true) {
		get_next_tag();

		if (!strcmp(name, "name")) {
			check_required_tag(0);

			if (parent) {
                // HPE get the percent encoded flag if it exists
                dir_percent_encoded = (char *)xmlTextReaderGetAttribute(reader, BAD_CAST "percentencoded");

                if (dir_percent_encoded && ! strcasecmp(dir_percent_encoded, "true")) {
                    //dir_percentencoded is boolean so assign to it
                    dir->percentencoded = true;
                    percentencoded = 1;
                }

              get_tag_text();
              if (xml_parse_filename(&dir->name, value, percentencoded) < 0)
                  return -1;
              dirname->name = dir->name;
              dirname->d = dir;

              check_tag_end("name");
            }else {
				/* this is the root directory, so set the volume name */
				check_empty();
				if (empty > 0) {
					value = NULL;
				} else {
					if (xml_scan_text(reader, &value) < 0)
						return -1;
				}

				if (value && strlen(value) > 0) {
					if (xml_parse_filename(&idx->volume_name, value, percentencoded) < 0)
						return -1;
					/* if the value is the empty string, then xml_scan_text consumed the "name"
					 * element end */
					check_tag_end("name");
				} else
					idx->volume_name = NULL;
			}

		} else if (!strcmp(name, "readonly")) {
			check_required_tag(1);
			get_tag_text();
			if (xml_parse_bool(&dir->readonly, value) < 0)
				return -1;
			check_tag_end("readonly");

		} else if (!strcmp(name, "modifytime")) {
			check_required_tag(2);
			get_tag_text();
			ret = xml_parse_time(true, value, &dir->modify_time);
			if (ret < 0)
				return -1;
			else if (ret == LTFS_TIME_OUT_OF_RANGE)
				ltfsmsg(LTFS_WARN, "17220W", "updatetime", dir->name, dir->uid, value);

			check_tag_end("modifytime");

		} else if (!strcmp(name, "creationtime")) {
			check_required_tag(3);
			get_tag_text();
			ret = xml_parse_time(true, value, &dir->creation_time);
			if (ret < 0)
				return -1;
			else if (ret == LTFS_TIME_OUT_OF_RANGE)
				ltfsmsg(LTFS_WARN, "17220W", "creationtime", dir->name, dir->uid, value);

			check_tag_end("creationtime");

		} else if (!strcmp(name, "accesstime")) {
			check_required_tag(4);
			get_tag_text();
			ret = xml_parse_time(true, value, &dir->access_time);
			if (ret < 0)
				return -1;
			else if (ret == LTFS_TIME_OUT_OF_RANGE)
				ltfsmsg(LTFS_WARN, "17220W", "accesstime", dir->name, dir->uid, value);

			check_tag_end("accesstime");

		} else if (!strcmp(name, "changetime")) {
			check_required_tag(5);
			get_tag_text();
			ret = xml_parse_time(true, value, &dir->change_time);
			if (ret < 0)
				return -1;
			else if (ret == LTFS_TIME_OUT_OF_RANGE)
				ltfsmsg(LTFS_WARN, "17220W", "changetime", dir->name, dir->uid, value);

			check_tag_end("changetime");

		} else if (! strcmp(name, "contents")) {
			check_required_tag(6);
			check_empty();
			if (empty == 0 && (ret = _xml_parse_dir_contents(reader, dir, idx)) < 0) {
				if (ret == -LTFS_NO_MEMORY)
					return ret;
				else
					return -1;
			}

		} else if (!strcmp(name, "extendedattributes")) {
			check_optional_tag(0);
			check_empty();
			if (empty == 0 && _xml_parse_xattrs(reader, dir) < 0)
					return -1;

		} else if (idx->version >= IDX_VERSION_UID && ! strcmp(name, UID_TAGNAME)) {
			check_required_tag(7);
			get_tag_text();
			if (xml_parse_ull(&value_int, value) < 0)
				return -1;
			dir->uid = value_int;
			if (dir->uid > idx->uid_number)
				idx->uid_number = dir->uid;
			if (parent) {
				dirname->uid  = dir->uid;
			}
			check_tag_end(UID_TAGNAME);
		} else if (! strcmp(name, UID_TAGNAME)) {
			ignore_unrecognized_tag();

		} else if (idx->version >= IDX_VERSION_BACKUPTIME && ! strcmp(name, BACKUPTIME_TAGNAME)) {
			check_required_tag(8);
			get_tag_text();
			ret = xml_parse_time(true, value, &dir->backup_time);
			if (ret < 0)
				return -1;
			else if (ret == LTFS_TIME_OUT_OF_RANGE)
				ltfsmsg(LTFS_WARN, "17220W", "backuptime", dir->name, dir->uid, value);

			check_tag_end(BACKUPTIME_TAGNAME);
		} else if (! strcmp(name, BACKUPTIME_TAGNAME)) {
			ignore_unrecognized_tag();

		} else
			preserve_unrecognized_tag(dir);
	}

	/* For old index versions, allocate a UID */
	if (idx->version < IDX_VERSION_UID) {
		check_required_tag(7);
		if (parent) {
			dir->uid = fs_allocate_uid(idx);
			if (dir->uid > idx->uid_number)
				idx->uid_number = dir->uid;
			dirname->uid  = dir->uid;
		}
		/* root directory already got assigned UID 1 by fs_allocate_dentry */
	}

	/* For old index versions, set backup time equal to creation time */
	if (idx->version < IDX_VERSION_BACKUPTIME) {
		check_required_tag(8);
		dir->backup_time = dir->creation_time;
	}

	check_required_tags();

	/* Validate UID: root directory must have uid==1, other dentries must have nonzero UID */
	/* TODO: would be nice to verify that there are no UID conflicts */
	if (parent && dir->uid == 1) {
		ltfsmsg(LTFS_ERR, "17101E");
		return -1;
	} else if (! parent && dir->uid != 1) {
		ltfsmsg(LTFS_ERR, "17100E");
		return -1;
	} else if (dir->uid == 0) {
		ltfsmsg(LTFS_ERR, "17106E");
		return -1;
	}

	return 0;
}

int _xml_parse_dir_contents(xmlTextReaderPtr reader, struct dentry *dir, struct ltfs_index *idx)
{
	int ret = 0;
	struct name_list *list = NULL, *entry_name = NULL;
	CHECK_ARG_NULL(dir, -LTFS_NULL_ARG);
	declare_parser_vars("contents");
	declare_tracking_arrays(0, 0);

	/* get rid of unused variable warning */
	(void) value;
	errno = 0;

	while (true) {
		get_next_tag();

		if (! strcmp(name, "file")) {
			assert_not_empty();
			entry_name = (struct name_list *)malloc(sizeof(struct name_list));
			if (!entry_name) {
				ltfsmsg(LTFS_ERR, "10001E", "_xml_parse_dir_contents: file");
				return -LTFS_NO_MEMORY;
			}
			if ((ret = _xml_parse_file(reader, idx, dir, entry_name)) < 0) {
				free(entry_name);
				if (ret == -LTFS_NO_MEMORY)
					return -LTFS_NO_MEMORY;
				else
					return -1;
			}

		} else if (! strcmp(name, "directory")) {
			assert_not_empty();
			entry_name = (struct name_list *)malloc(sizeof(struct name_list));
			if (!entry_name) {
				ltfsmsg(LTFS_ERR, "10001E", "_xml_parse_dir_contents: dir");
				return -LTFS_NO_MEMORY;
			}
			if ((ret =_xml_parse_dirtree(reader, dir, idx, dir->vol, entry_name)) < 0) {
				free(entry_name);
				if (ret == -LTFS_NO_MEMORY)
					return -LTFS_NO_MEMORY;
				else
					return -1;
			}

		} else {
			ignore_unrecognized_tag();
			entry_name = NULL;
		}
		if (!strcmp(name, "file") || (!strcmp(name, "directory") && !(!dir && idx->root))) {
			/* Make temporal hash table whose key is dentry name */
			HASH_ADD_KEYPTR(hh, list, entry_name->name, strlen(entry_name->name), entry_name);
			if (errno == ENOMEM) {
				struct name_list *list_ptr, *list_tmp;

				HASH_ITER(hh, list, list_ptr, list_tmp) {
					HASH_DEL(list, list_ptr);
					free(list_ptr);
				}

				ltfsmsg(LTFS_ERR, "10001E", "_xml_parse_dir_contents: add key");
				/* HPE fix for double free when a tape with a large index file is mounted
				 * on a host with low physical memory
				 */
				/*free(entry_name);*/

				return -LTFS_NO_MEMORY;
			}
		} else {
			free(entry_name);
			entry_name = NULL;
		}
	}

	/* At first, update platform_safe_name without replacing invalid chars in the directory.
	   The file or directory of which name contains invalid char is skipped in this step.
	   After that update platfrom_safe_name regarding skipped file or directory as the second
	   step. These steps make a prioritization of name mangling.*/
	if (fs_update_platform_safe_names(dir, idx, list)!=0) {
		return -1;
	}

	check_required_tags();
	return 0;
}

/**
 * Parse a file into the given directory.
 *
 * HPE MD 22.09.2017 Changes made to support SNIA 2.4 section 7.4 percent encoding 
 * HPE MD 12.10.2017 Changes made to support SNIA 2.4 Section 9.2.8 openforwrite.
 */
int _xml_parse_file(xmlTextReaderPtr reader, struct ltfs_index *idx, struct dentry *dir, struct name_list *filename)
{
	int ret;
	int percentencoded = 0;
	unsigned long long value_int;
	struct dentry *file;
    char *file_percent_encoded;
	struct extent_info *xt_last;
	declare_parser_vars("file");
	declare_tracking_arrays(9, 4);
	bool symlink_flag=false, extent_flag=false;

	file = fs_allocate_dentry(dir, NULL, NULL, false, false, false, idx);
	if (! file) {
		ltfsmsg(LTFS_ERR, "10001E", __FUNCTION__);
		return -LTFS_NO_MEMORY;
	}

	while (true) {
		get_next_tag();

        if (!strcmp(name, "name")) {
            check_required_tag(0);
            
            // HPE MD 22.09.2017 Changes made to reading name due to new percent encoding parameter
            
            file_percent_encoded = (char *)xmlTextReaderGetAttribute(reader, BAD_CAST "percentencoded");
            if (file_percent_encoded && !strcasecmp(file_percent_encoded, "true")) {
                file->percentencoded = true;
                percentencoded = 1;
            }
            
            get_tag_text();
            if (xml_parse_filename(&file->name, value, percentencoded) < 0)
                return -1;
            filename->name = file->name;
            filename->d = file;

            check_tag_end("name");
            }
            else if (!strcmp(name, "length")) {
                check_required_tag(1);
                get_tag_text();
                if (xml_parse_ull(&value_int, value) < 0)
                    return -1;
                file->size = value_int;
                check_tag_end("length");
        } else if (!strcmp(name, "readonly")) {
            check_required_tag(2);
            get_tag_text();
            if (xml_parse_bool(&file->readonly, value) < 0)
                return -1;
            check_tag_end("readonly");

		} else if (!strcmp(name, "openforwrite")) {
            check_optional_tag(0);
            get_tag_text();
            if (xml_parse_bool(&file->openforwrite, value) < 0)
            {
                return -1;
            }
            check_tag_end("openforwrite");

		} else if (!strcmp(name, "modifytime")) {
			check_required_tag(3);
			get_tag_text();
			ret = xml_parse_time(true, value, &file->modify_time);
			if (ret < 0)
				return -1;
			else if (ret == LTFS_TIME_OUT_OF_RANGE)
				ltfsmsg(LTFS_WARN, "17220W", "modifytime", file->name, file->uid, value);
			check_tag_end("modifytime");

		} else if (!strcmp(name, "creationtime")) {
			check_required_tag(4);
			get_tag_text();
			ret = xml_parse_time(true, value, &file->creation_time);
			if (ret < 0)
				return -1;
			else if (ret == LTFS_TIME_OUT_OF_RANGE)
				ltfsmsg(LTFS_WARN, "17220W", "creationtime", file->name, file->uid, value);

			check_tag_end("creationtime");

		} else if (!strcmp(name, "accesstime")) {
			check_required_tag(5);
			get_tag_text();
			ret = xml_parse_time(true, value, &file->access_time);
			if (ret < 0)
				return -1;
			else if (ret == LTFS_TIME_OUT_OF_RANGE)
				ltfsmsg(LTFS_WARN, "17220W", "accesstime", file->name, file->uid, value);
			check_tag_end("accesstime");

		} else if (!strcmp(name, "changetime")) {
			check_required_tag(6);
			get_tag_text();
			ret = xml_parse_time(true, value, &file->change_time);
			if (ret < 0)
				return -1;
			else if (ret == LTFS_TIME_OUT_OF_RANGE)
				ltfsmsg(LTFS_WARN, "17220W", "changetime", file->name, file->uid, value);
			check_tag_end("changetime");

		} else if (!strcmp(name, "extendedattributes")) {
			check_optional_tag(1);
			check_empty();
			if (empty == 0 && _xml_parse_xattrs(reader, file) < 0)
					return -1;

		} else if (!strcmp(name, "extentinfo")) {
			check_optional_tag(2);
			check_empty();
			if (empty == 0 && _xml_parse_extents(reader, idx->version, file) < 0)
					return -1;
			else extent_flag = true;

        } else if (!strcmp(name, "symlink")) {
            check_optional_tag(3);
			get_tag_text();
			file->target = strdup(value);
			file->isslink = true;
			symlink_flag = true;

        } else if (idx->version >= IDX_VERSION_UID && ! strcmp(name, UID_TAGNAME)) {
			check_required_tag(7);
			get_tag_text();
			if (xml_parse_ull(&value_int, value) < 0)
				return -1;
			file->uid = value_int;
			if (file->uid > idx->uid_number)
				idx->uid_number = file->uid;
			filename->uid  = file->uid;
			check_tag_end(UID_TAGNAME);
		} else if (! strcmp(name, UID_TAGNAME)) {
			ignore_unrecognized_tag();

		} else if (idx->version >= IDX_VERSION_BACKUPTIME && ! strcmp(name, BACKUPTIME_TAGNAME)) {
			check_required_tag(8);
			get_tag_text();
			ret = xml_parse_time(true, value, &file->backup_time);
			if (ret < 0)
				return -1;
			else if (ret == LTFS_TIME_OUT_OF_RANGE)
				ltfsmsg(LTFS_WARN, "17220W", "backuptime", file->name, file->uid, value);

			check_tag_end(BACKUPTIME_TAGNAME);
		} else if (! strcmp(name, BACKUPTIME_TAGNAME)) {
			ignore_unrecognized_tag();

		} else
			preserve_unrecognized_tag(file);
	}

	/* For old index versions, allocate a UID */
	if (idx->version < IDX_VERSION_UID) {
		check_required_tag(7);
		file->uid = fs_allocate_uid(idx);
		if (file->uid > idx->uid_number)
			idx->uid_number = file->uid;
		filename->uid  = file->uid;
	}

	/* For old index versions, set backup time equal to creation time */
	if (idx->version < IDX_VERSION_BACKUPTIME) {
		check_required_tag(8);
		file->backup_time = file->creation_time;
	}

	check_required_tags();

	/* check that file size is not shorter than the extent list */
	if (! TAILQ_EMPTY(&file->extentlist)) {
		xt_last = TAILQ_LAST(&file->extentlist, extent_struct);
		if (xt_last->fileoffset + xt_last->bytecount > file->size) {
			ltfsmsg(LTFS_ERR, "17026E");
			return -1;
		}
	}

	/* Validate UID: must be nonzero (UID 0 is reserved for the root directory) */
	if (file->uid == 0) {
		ltfsmsg(LTFS_ERR, "17101E");
		return -1;
	}

	if ( symlink_flag && extent_flag ) {
		ltfsmsg(LTFS_ERR, "17180E", file->name);
		if ( _xml_save_symlink_conflict( idx, file ) ) return -1;
	}

	return 0;
}

/**
 * Parse a file's extent list.
 */
int _xml_parse_extents(xmlTextReaderPtr reader, int idx_version, struct dentry *d)
{
	declare_parser_vars("extentinfo");
	declare_tracking_arrays(0, 0);

	/* get rid of unused variable warning */
	(void) value;

	while (true) {
		get_next_tag();

		if (! strcmp(name, "extent")) {
			assert_not_empty();
			if (_xml_parse_one_extent(reader, idx_version, d) < 0)
				return -1;

		} else
			ignore_unrecognized_tag();
	}

	check_required_tags();
	return 0;
}

/**
 * Parse an extent from an XML source, appending it to the given dentry's extent list.
 */
int _xml_parse_one_extent(xmlTextReaderPtr reader, int idx_version, struct dentry *d)
{
	unsigned long long value_int;
	struct extent_info *xt, *xt_last;
	declare_parser_vars("extent");
	declare_tracking_arrays(5, 0);

	xt = calloc(1, sizeof(struct extent_info));
	if (!xt) {
		ltfsmsg(LTFS_ERR, "10001E", __FUNCTION__);
		return -1;
	}

	while (true) {
		get_next_tag();

		if (! strcmp(name, "partition")) {
			check_required_tag(0);
			get_tag_text();
			if (_xml_parse_partition(value) < 0) {
				free(xt);
				return -1;
			}
			xt->start.partition = value[0];
			check_tag_end("partition");

		} else if (! strcmp(name, "startblock")) {
			check_required_tag(1);
			get_tag_text();
			if (xml_parse_ull(&value_int, value) < 0) {
				free(xt);
				return -1;
			}
			xt->start.block = value_int;
			check_tag_end("startblock");

		} else if (! strcmp(name, "byteoffset")) {
			check_required_tag(2);
			get_tag_text();
			if (xml_parse_ull(&value_int, value) < 0) {
				free(xt);
				return -1;
			}
			xt->byteoffset = value_int;
			check_tag_end("byteoffset");

		} else if (! strcmp(name, "bytecount")) {
			check_required_tag(3);
			get_tag_text();
			if (xml_parse_ull(&value_int, value) < 0) {
				free(xt);
				return -1;
			}
			xt->bytecount = value_int;
			check_tag_end("bytecount");

		} else if (idx_version >= IDX_VERSION_SPARSE && ! strcmp(name, "fileoffset")) {
			check_required_tag(4);
			get_tag_text();
			if (xml_parse_ull(&value_int, value) < 0) {
				free(xt);
				return -1;
			}
			xt->fileoffset = value_int;
			check_tag_end("fileoffset");

		} else
			ignore_unrecognized_tag();
	}

	/* For older index versions, set fileoffset at the end of the previous extent */
	if (idx_version < IDX_VERSION_SPARSE) {
		check_required_tag(4);
		if (TAILQ_EMPTY(&d->extentlist))
			xt->fileoffset = 0;
		else {
			xt_last = TAILQ_LAST(&d->extentlist, extent_struct);
			xt->fileoffset = xt_last->fileoffset + xt_last->bytecount;
		}
	}

	check_required_tags();

	/* Add extent to the extent list, performing appropriate reordering if necessary.
	 * Also make sure the new extent does not overlap with any existing extents. */
	if (TAILQ_EMPTY(&d->extentlist))
		TAILQ_INSERT_TAIL(&d->extentlist, xt, list);
	else {
		bool xt_used = false;
		TAILQ_FOREACH_REVERSE(xt_last, &d->extentlist, extent_struct, list) {
			if (xt_last->fileoffset + xt_last->bytecount <= xt->fileoffset) {
				TAILQ_INSERT_AFTER(&d->extentlist, xt_last, xt, list);
				xt_used = true;
				break;
			} else if (xt->fileoffset + xt->bytecount > xt_last->fileoffset) {
				/* Overlap error */
				ltfsmsg(LTFS_ERR, "17097E");
				free(xt);
				return -1;
			}
		}
		if (! xt_used)
			TAILQ_INSERT_HEAD(&d->extentlist, xt, list);
	}

	d->realsize += xt->bytecount;

	if (d->vol) {
		d->used_blocks += ((xt->byteoffset + xt->bytecount) / d->vol->label->blocksize);
		if ((xt->byteoffset + xt->bytecount) % d->vol->label->blocksize)
			d->used_blocks++;
	}

	return 0;
}

/**
 * Parse extended attributes for a file or directory.
 */
int _xml_parse_xattrs(xmlTextReaderPtr reader, struct dentry *d)
{
	declare_parser_vars("extendedattributes");
	declare_tracking_arrays(0, 0);

	/* get rid of unused variable warning */
	(void) value;

	while (true) {
		get_next_tag();

		if (! strcmp(name, "xattr")) {
			assert_not_empty();
			if (_xml_parse_one_xattr(reader, d) < 0)
				return -1;

		} else
			ignore_unrecognized_tag();
	}

	check_required_tags();
	return 0;
}

/**
 * Parse a single extended attribute, appending it to the xattr list of the given dentry.
 *
 * HPE MD 22.09.2017 Changes made to support SNIA 2.4 section 7.4 percent encoding 
 */
int _xml_parse_one_xattr(xmlTextReaderPtr reader, struct dentry *d)
{
	char *xattr_type;
    char *xattr_percent_encoded;
	struct xattr_info *xattr;
	declare_parser_vars("xattr");
	declare_tracking_arrays(2, 0);

	xattr = calloc(1, sizeof(struct xattr_info));
	if (! xattr) {
		ltfsmsg(LTFS_ERR, "10001E", __FUNCTION__);
		return -1;
	}

	while (true) {
		get_next_tag();

        if (!strcmp(name, "key")) {
            check_required_tag(0);

            xattr_percent_encoded = (char *)xmlTextReaderGetAttribute(reader, BAD_CAST "percentencoded");
            if (xattr_percent_encoded && !strcasecmp(xattr_percent_encoded, "true")) 
            {
               xattr->percentencoded = true;
               get_tag_text();
               if (xml_parse_xattrname(&xattr->percent_encoded_key, value, 1) < 0)
               {
                  free(xattr);
                  return -1;
               } 
               else 
               {
                   update_xattr_safe_name(xattr); // Need to update xattr->key with the decoded name.
               }  
            }
            else
            {
               get_tag_text();
               if (xml_parse_xattrname(&xattr->key, value, 0) < 0)
               {
                  free(xattr);
                  return -1;
               }
               else
               {
                  xattr->percent_encoded_key = NULL;
               }   
            }
            
            
            check_tag_end("key");

        } else if (! strcmp(name, "value")) {
			check_required_tag(1);

			xattr_type = (char *)xmlTextReaderGetAttribute(reader, BAD_CAST "type");
			if (xattr_type && strcmp(xattr_type, "text") && strcmp(xattr_type, "base64")) {
				ltfsmsg(LTFS_ERR, "17027E", xattr_type);
				free(xattr);
				return -1;
			}

			check_empty();
			if (empty == 0) {
				if (xml_scan_text(reader, &value) < 0) {
					free(xattr->key);
					free(xattr);
					return -1;
				}
				if (! xattr_type || ! strcmp(xattr_type, "text")) {
					xattr->value = strdup(value);
					if (! xattr->value) {
						ltfsmsg(LTFS_ERR, "10001E", __FUNCTION__);
						free(xattr->key);
						free(xattr);
						return -1;
					}
					xattr->size = strlen(value);
				} else { /* base64 */
					xattr->size = base64_decode(
						(const unsigned char *)value,
						strlen(value),
						(unsigned char **)(&xattr->value));
					if (xattr->size == 0) {
						ltfsmsg(LTFS_ERR, "17028E");
						free(xattr->key);
						free(xattr);
						return -1;
					}
				}
				if (strlen(value) > 0)
					check_tag_end("value");
			} else {
				xattr->value = NULL;
				xattr->size = 0;
			}

/* HPE : need to free using xmlFree to clean up properly */
#ifndef HPE_mingw_BUILD
			free(xattr_type);
#else
			xmlFree(xattr_type);
#endif	

		} else
			ignore_unrecognized_tag();
	}

	check_required_tags();
	TAILQ_INSERT_TAIL(&d->xattrlist, xattr, list);
	return 0;
}

/**
 * Parse a version number of the form X.Y from a string.
 * @param version_str String to parse
 * @param major Outputs the major version number X
 * @param minor Outputs the minor version number Y
 * @return 0 on success or a negative value on error.
 */
int _xml_parse_version(const char *version_str, int *version_int)
{
	const char *y_str, *z_str, *tmp;

	CHECK_ARG_NULL(version_str, -LTFS_NULL_ARG);
	CHECK_ARG_NULL(version_int, -LTFS_NULL_ARG);

	/* Legacy index version 1.0 */
	if (! strcmp(version_str, "1.0")) {
		*version_int = MAKE_LTFS_VERSION(1,0,0);
		return 0;
	}

	tmp = version_str;
	while (*tmp && *tmp >= '0' && *tmp <= '9')
		++tmp;
	if (tmp == version_str || *tmp != '.')
		return -LTFS_BAD_ARG;
	y_str = ++tmp;
	while (*tmp && *tmp >= '0' && *tmp <= '9')
		++tmp;
	if (tmp == y_str || *tmp != '.')
		return -LTFS_BAD_ARG;
	z_str = ++tmp;
	while (*tmp && *tmp >= '0' && *tmp <= '9')
		++tmp;
	if (tmp == z_str || *tmp != '\0')
		return -LTFS_BAD_ARG;

	*version_int = MAKE_LTFS_VERSION(atoi(version_str), atoi(y_str), atoi(z_str));
	return 0;
}

/**
 * Verify that a given string really does represent a partition (single character, a-z).
 */
int _xml_parse_partition(const char *val)
{
	CHECK_ARG_NULL(val, -LTFS_NULL_ARG);

	if (strlen(val) != 1 || val[0] < 'a' || val[0] > 'z') {
		ltfsmsg(LTFS_ERR, "17033E", val);
		return -1;
	}

	return 0;
}

/**
 * Scan a tape offset (a partition tag and a startblock tag) from an XML source.
 * @param reader the XML source
 * @param tag name of the tag containing the tape position. This function will read to the end
 *            of that tag, so it is not suitable for reading an extent list (which has other
 *            children that need to be parsed).
 * @param pos pointer to a structure where the offset will be stored
 * @return 0 on success or a negative value on error
 */
int _xml_scan_tapepos(xmlTextReaderPtr reader, const char *tag, struct tape_offset *pos)
{
	unsigned long long value_int;
	declare_parser_vars(tag);
	declare_tracking_arrays(2, 0);

	while (true) {
		get_next_tag();

		if (! strcmp(name, "partition")) {
			check_required_tag(0);
			get_tag_text();
			if (_xml_parse_partition(value) < 0)
				return -1;
			pos->partition = value[0];
			check_tag_end("partition");

		} else if (! strcmp(name, "startblock")) {
			check_required_tag(1);
			get_tag_text();
			if (xml_parse_ull(&value_int, value) < 0)
				return -1;
			pos->block = value_int;
			check_tag_end("startblock");

		} else
			ignore_unrecognized_tag();
	}

	check_required_tags();
	return 0;
}

int _xml_save_symlink_conflict( struct ltfs_index *idx, struct dentry *d)
{
	size_t c = idx->symerr_count + 1;
	struct dentry **err_d;

	err_d = realloc( idx->symlink_conflict, c * sizeof(size_t));
	if (! err_d) {
		ltfsmsg(LTFS_ERR, "10001E", __FUNCTION__);
		return -1;
	}
	err_d[c-1] = d;

	idx->symlink_conflict = err_d;
	idx->symerr_count = c;

	return 0;
}
