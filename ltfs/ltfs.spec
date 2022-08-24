# RPM specification file for HPE SOS.
Summary: HPE StoreOpen Software (HPE-SOS) - Version 3.5.0
Name: HPE-SOS		
Version: 3.5.0
Release: 12
Group: Util
License: LGPL	
Vendor: HPE
Source0: HPE_LTFS_3.5.0_BUILD12
Prereq: /sbin/ldconfig, /usr/bin/awk
Requires:  fuse >= 2.8.4
Requires:  libxml2 >= 2.6.16
Requires:  libicu >= 3.6
Requires:  e2fsprogs >= 1.36
BuildRoot: /tmp/rpm/%{name}-%{version}

%define _unpackaged_files_terminate_build       0
%define         _sysconf /etc
%define         _prefix  /usr/local
%define 	_processname ltfs
%define 	_mkltfsprocessname mkltfs
%define 	_unltfsprocessname unltfs
%define 	_ltfsckprocessname ltfsck
%define         __prelink_undo_cmd     %{nil}
#%undefine __prelink_undo_cmd
#AutoReqProv: no

%description
The HPE LTFS software application is an open-source tape file system
implemented on dual partition tape drives.

%prep
%setup -q
echo $RPM_BUILD_ROOT

%build
rm -rf $RPM_BUILD_ROOT
#./configure
./configure --prefix=%{_prefix} --libdir=%{_libdir}
make

%pre
LTFS_PID=`ps ax | grep -v grep | grep -v rpm | grep -E '(^|\s)%{_processname}($|\s)' | awk '{print $1}' | tr '\n' ' '`
MKLTFS_PID=`ps ax | grep -v grep | grep -v rpm | grep -E '(^|\s)%{_mkltfsprocessname}($|\s)' | awk '{print $1}' | tr '\n' ' '`
UNLTFS_PID=`ps ax | grep -v grep | grep -v rpm | grep -E '(^|\s)%{_unltfsprocessname}($|\s)' | awk '{print $1}' | tr '\n' ' '`
LTFSCK_PID=`ps ax | grep -v grep | grep -v rpm | grep -E '(^|\s)%{_ltfsckprocessname}($|\s)' | awk '{print $1}' | tr '\n' ' '`
if [ ! -z "$LTFS_PID" ] || [ ! -z "$MKLTFS_PID" ] || [ ! -z "$UNLTFS_PID" ] || [ ! -z "$LTFSCK_PID" ]; then
    echo
    echo "Error: please unmount all LTFS instances or utilities( PID: $LTFS_PID $MKLTFS_PID $UNLTFS_PID $LTFSCK_PID)  before installing this RPM. Please refer the user guide for more information."
    echo
    exit 1
fi

%preun
LTFS_PID=`ps ax | grep -v grep | grep -v rpm | grep -E '(^|\s)%{_processname}($|\s)' | awk '{print $1}' | tr '\n' ' '`
MKLTFS_PID=`ps ax | grep -v grep | grep -v rpm | grep -E '(^|\s)%{_mkltfsprocessname}($|\s)' | awk '{print $1}' | tr '\n' ' '`
UNLTFS_PID=`ps ax | grep -v grep | grep -v rpm | grep -E '(^|\s)%{_unltfsprocessname}($|\s)' | awk '{print $1}' | tr '\n' ' '`
LTFSCK_PID=`ps ax | grep -v grep | grep -v rpm | grep -E '(^|\s)%{_ltfsckprocessname}($|\s)' | awk '{print $1}' | tr '\n' ' '`
if [ ! -z "$LTFS_PID" ] || [ ! -z "$MKLTFS_PID" ] || [ ! -z "$UNLTFS_PID" ] || [ ! -z "$LTFSCK_PID" ]; then
    echo
    echo "Error: please unmount all LTFS instances or utilities( PID: $LTFS_PID $MKLTFS_PID $UNLTFS_PID $LTFSCK_PID)  before uninstalling this RPM. Please refer the user guide for more information."
    echo
    exit 1
fi

%pretrans

%posttrans
if [ -s /usr/local/lib/ltfs ] && [ %{_libdir} != "/usr/local/lib" ]
then
    rm -rf /usr/local/lib/*ltfs*
fi
/sbin/ldconfig

%install
[ -d $RPM_BUILD_ROOT ] && rm -rf $RPM_BUILD_ROOT;
make sysconfdir=$RPM_BUILD_ROOT install DESTDIR=$RPM_BUILD_ROOT
mkdir -p $RPM_BUILD_ROOT%{_prefix}%{_sysconf}
mkdir -p $RPM_BUILD_ROOT%{_sysconf}/ld.so.conf.d
cp $RPM_BUILD_ROOT/ltfs.conf.local $RPM_BUILD_ROOT%{_prefix}%{_sysconf}
cp $RPM_BUILD_ROOT$RPM_BUILD_ROOT/ltfs.conf $RPM_BUILD_ROOT%{_prefix}%{_sysconf}
# echo "/usr/local/lib" > $RPM_BUILD_ROOT%{_sysconf}/ld.so.conf.d/%{name}.conf
# echo "/usr/local/lib64" >> $RPM_BUILD_ROOT%{_sysconf}/ld.so.conf.d/%{name}.conf
echo "%{_libdir}" > $RPM_BUILD_ROOT%{_sysconf}/ld.so.conf.d/%{name}.conf
cp %{_prefix}/bin/ltfscopy $RPM_BUILD_ROOT%{_prefix}/bin/
cp %{_prefix}/bin/ltfslock $RPM_BUILD_ROOT%{_prefix}/bin/
cp %{_prefix}/bin/latte $RPM_BUILD_ROOT%{_prefix}/bin/

%post
/sbin/ldconfig

%postun
/sbin/ldconfig

%clean
[ -d $RPM_BUILD_ROOT ] && rm -rf $RPM_BUILD_ROOT;

%files
%defattr(-,root,root)
%{_prefix}/bin/ltfs
%{_prefix}/bin/ltfsck
%{_prefix}/bin/mkltfs
%{_prefix}/bin/unltfs
%{_prefix}/bin/ltfscopy
%{_prefix}/bin/ltfslock
%{_prefix}/bin/latte
%{_libdir}/libltfs.a
%{_libdir}/libltfs.la
%{_libdir}/libltfs.so
%{_libdir}/libltfs.so.0
%{_libdir}/libltfs.so.0.0.0
%{_libdir}/ltfs/libdriver-ltotape.so
%{_libdir}/ltfs/libiosched-fcfs.so
%{_libdir}/ltfs/libiosched-unified.so
%{_libdir}/ltfs/libkmi-flatfile.so
%{_libdir}/ltfs/libkmi-simple.so
%{_prefix}/etc/ltfs.conf
%{_prefix}/etc/ltfs.conf.local
%config /etc/ld.so.conf.d/%{name}.conf

%changelog
* Thu May 25 2017 Martind <martin.dyer@hpe.com>
- update to 3.3.0

* Wed May 11 2015 Murali <murali.vuppalapati@hpe.com>
- update to 3.2.0
