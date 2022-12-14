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
** FILE NAME:       arch/filename_handling.h
**
** DESCRIPTION:     Prototypes for platform-specific filename handling functions.
**
** AUTHOR:          Takashi Ashida
**                  IBM Yamato, Japan
**                  ashida@jp.ibm.com
**
*************************************************************************************
**
**  (C) Copyright 2015 - 2017 Hewlett Packard Enterprise Development LP
**  10/13/17 Added support for SNIA 2.4
**
*************************************************************************************
*/

#ifndef filename_handling_h_
#define filename_handling_h_

#include "libltfs/ltfs.h"

void update_platform_safe_name(struct dentry* dentry, bool handle_invalid_char, struct ltfs_index *idx);
int ltfs_compare_names(const char *name1, const char *name2, int *result);
// HPE MD 22.09.2017 Added following functions for SNIA 2.4
void perform_name_percent_encoding(struct dentry* dentry);
void perform_xattr_percent_encoding(struct xattr_info* xattr);
void update_xattr_safe_name(struct xattr_info* xattr);

#endif /* filename_handling_h_ */
