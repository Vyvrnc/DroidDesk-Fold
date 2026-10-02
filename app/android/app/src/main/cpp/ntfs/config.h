/*
 * config.h for the vendored libntfs-3g (DroidDesk's libdroiddesk_ntfs.so).
 *
 * Derived from what ./configure --disable-ntfs-3g --disable-ntfsprogs --disable-crypto
 * --without-uuid --without-hd --disable-plugins --disable-mtab (ntfs-3g 2026.9.28, Debian
 * trixie, glibc) produced, then cut down by hand to what libntfs-3g actually tests and what
 * both glibc and Android bionic (API 21+, arm64-v8a / armeabi-v7a / x86_64) provide:
 *   - no libintl.h, getmntent/hasmntopt/mntent.h (mtab is not used: the volume is a held USB
 *     disk, never a mounted kernel device), no floppy header (linux/hdreg.h is needed because
 *     linux/fs.h already defines HDIO_GETGEO);
 *   - no crypto (encrypted files stay unreadable), no plugins, no POSIX ACLs, no xattr mappings;
 *   - large files on 32-bit ABIs through _FILE_OFFSET_BITS=64.
 * Every target DroidDesk builds for is little-endian.
 */
#ifndef DROIDDESK_NTFS_CONFIG_H
#define DROIDDESK_NTFS_CONFIG_H

#define PACKAGE "ntfs-3g"
#define PACKAGE_NAME "ntfs-3g"
#define PACKAGE_TARNAME "ntfs-3g"
#define PACKAGE_VERSION "2026.9.28"
#define PACKAGE_STRING "ntfs-3g 2026.9.28"
#define PACKAGE_BUGREPORT "ntfs-3g-devel@lists.sf.net"
#define PACKAGE_URL ""
#define VERSION "2026.9.28"

#ifndef _GNU_SOURCE
#define _GNU_SOURCE 1
#endif
#ifndef _FILE_OFFSET_BITS
#define _FILE_OFFSET_BITS 64
#endif

#define DISABLE_PLUGINS 1
#define IGNORE_MTAB 1

#define STDC_HEADERS 1
#define HAVE_BYTESWAP_H 1
#define HAVE_CTYPE_H 1
#define HAVE_ENDIAN_H 1
#define HAVE_ERRNO_H 1
#define HAVE_FCNTL_H 1
#define HAVE_INTTYPES_H 1
#define HAVE_LIMITS_H 1
#define HAVE_LINUX_FS_H 1
#define HAVE_LINUX_HDREG_H 1
#define HAVE_LOCALE_H 1
#define HAVE_STDARG_H 1
#define HAVE_STDDEF_H 1
#define HAVE_STDINT_H 1
#define HAVE_STDIO_H 1
#define HAVE_STDLIB_H 1
#define HAVE_STRINGS_H 1
#define HAVE_STRING_H 1
#define HAVE_SYSLOG_H 1
#define HAVE_SYS_IOCTL_H 1
#define HAVE_SYS_PARAM_H 1
#define HAVE_SYS_STAT_H 1
#define HAVE_SYS_SYSMACROS_H 1
#define HAVE_SYS_TYPES_H 1
#define HAVE_TIME_H 1
#define HAVE_UNISTD_H 1
#define HAVE_WCHAR_H 1
#define MAJOR_IN_SYSMACROS 1

#define HAVE_CLOCK_GETTIME 1
#define HAVE_DAEMON 1
#define HAVE_FFS 1
#define HAVE_GETTIMEOFDAY 1
#define HAVE_MBSINIT 1
#define HAVE_REALPATH 1
#define HAVE_STRSEP 1

#define WORDS_LITTLEENDIAN 1

#endif
