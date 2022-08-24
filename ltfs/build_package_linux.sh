#! /bin/bash
#############################################################################
#
# FILE NAME:       build_package_linux.sh
#
# DESCRIPTION:     Automates build of LTFS for linux.
#
# AUTHOR:          Murali
#
#############################################################################
#
# (C) Copyright 2015, 2016 Hewlett Packard Enterprise Development LP
#
#############################################################################

LTFS_CODE_DIRECTORY=""
LTFS_NAME=""
LTFS_VERSION=""
LTFS_SOURCE_NAME=""
LTFS_RELEASE=""
LTFS_SPEC_NAME="ltfs.spec"

#RPM directories
RPM_SOURCES_DIR=""
RPM_SPECS_DIR=""
RPM_SRPMS_DIR=""
RPM_RPMS_DIR=""
RPM_ARCH_NAME=""

function initialize_global_variables() {

# Check whether rpm installed or not
    temp=`rpm -q rpm` 
    if [ $? -ne 0 ] ; then
        RET_VAL=$?
        echo -e "RPM not installed !!"
        exit $EXIT_VALUE
    fi

    if [[ "$temp" =~ "not installed" ]]; then
        echo "RPM Not Found";
    else
        echo "Initializing the RPM global variables...";
        RPM_SOURCES_DIR=`rpm -E %{_sourcedir}`
        RPM_SPECS_DIR=`rpm -E %{_specdir}`
        RPM_SRPMS_DIR=`rpm -E %{_srcrpmdir}`
        RPM_RPMS_DIR=`rpm -E %{_rpmdir}`
        RPM_ARCH_NAME=`uname -m`
        LTFS_CODE_DIRECTORY=`pwd`
        LTFS_NAME=`grep Name: $LTFS_SPEC_NAME | awk '{print $2}'`
        LTFS_VERSION=`grep Version: $LTFS_SPEC_NAME | awk '{print $2}'`
        LTFS_SOURCE_NAME=`grep Source0: $LTFS_SPEC_NAME | awk '{print $2}'`
        LTFS_RELEASE=`grep Release: $LTFS_SPEC_NAME | awk '{print $2}'`
    fi

}

function build_ltfs() {
   echo "Building ltfs..."
   cd ../
   temp_dir=`pwd`
   if [ -d "$temp_dir/$LTFS_NAME-$LTFS_VERSION" ]; then
       rm -rf $temp_dir/$LTFS_NAME-$LTFS_VERSION
   fi
   if [ -d "$temp_dir/$LTFS_SOURCE_NAME" ]; then
       rm -rf $temp_dir/$LTFS_SOURCE_NAME
   fi
   cp -rf $LTFS_CODE_DIRECTORY $LTFS_NAME-$LTFS_VERSION 
   tar -zcvf $LTFS_SOURCE_NAME $LTFS_NAME-$LTFS_VERSION
   mkdir -p $RPM_SOURCES_DIR
   cp -f $LTFS_SOURCE_NAME $RPM_SOURCES_DIR

   cd $LTFS_CODE_DIRECTORY
   rpmbuild -ba $LTFS_SPEC_NAME
   if [ $? -ne 0 ] ; then
       RET_VAL=$?
       echo -e "RPM build failed !!"
       exit $EXIT_VALUE
   fi

}

function copy_ltfs_build() {
   echo "Copying the ltfs binary rpm..."
   if [ -d "$LTFS_CODE_DIRECTORY/build" ]; then
       rm -rf $LTFS_CODE_DIRECTORY/build
   fi

   mkdir -p $LTFS_CODE_DIRECTORY/build

   cp $RPM_RPMS_DIR/$RPM_ARCH_NAME/$LTFS_NAME-$LTFS_VERSION-$LTFS_RELEASE.$RPM_ARCH_NAME.rpm $LTFS_CODE_DIRECTORY/build
   cp $RPM_SRPMS_DIR/$LTFS_NAME-$LTFS_VERSION-$LTFS_RELEASE.src.rpm $LTFS_CODE_DIRECTORY/build

}

function packaging_binaries() {
   echo "Producing binary tarball"
   cd $LTFS_CODE_DIRECTORY/build
   mkdir binary_package
   cd binary_package
   cp ../../doc/COPYING.LIB .
   cp ../../doc/INSTALLING.linux .
   cp ../../doc/README .
   cp ../$LTFS_NAME-$LTFS_VERSION-$LTFS_RELEASE.$RPM_ARCH_NAME.rpm .
   tar -cvzf $LTFS_NAME-$LTFS_VERSION-BINARIES-RHEL.$RPM_ARCH_NAME.tar.gz COPYING.LIB INSTALLING.linux README $LTFS_NAME-$LTFS_VERSION-$LTFS_RELEASE.$RPM_ARCH_NAME.rpm

}

function packaging_source() {
   echo "Producing source tarball"
   cd $LTFS_CODE_DIRECTORY/build
   mkdir source_package
   cd source_package
   svn export https://penrice.uk.rdlabs.hpecorp.net:18490/svn/StoreOpen/trunk/ltfs ltfs
   cp ../../doc/COPYING.LIB .
   cp ../../doc/BUILDING.linux .
   cp ../../doc/BUILDING.macosx .
   cp ../../doc/README .
   tar -cvzf $LTFS_NAME-$LTFS_VERSION-SOURCE.tar.gz COPYING.LIB BUILDING.linux BUILDING.macosx README ltfs 
}


function Main() {
    initialize_global_variables
    build_ltfs
    copy_ltfs_build
    packaging_binaries
    packaging_source
}

Main $@

