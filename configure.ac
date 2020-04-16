#                                               -*- Autoconf -*-
# Process this file with autoconf to produce a configure script.

AC_PREREQ([2.69])
AC_INIT([redex], [1.0], [not-valid-yet@fb.com])
AM_INIT_AUTOMAKE(redex, 1.0)

# clear out default cxx flags (was "-O2 -g") so that they don't override
# the flags defined in AM_CXXFLAGS
: ${CXXFLAGS=""}

# Checks for programs.
AC_PROG_CXX
AC_PROG_CC

LT_INIT

AM_INIT_AUTOMAKE([subdir-objects])

AC_CONFIG_MACRO_DIR([m4])

AM_PATH_PYTHON([3.0], [], [AC_MSG_ERROR([Redex requires python3])])

# Checks for libraries.
AX_PTHREAD
AX_BOOST_BASE([1.71.0], [], [AC_MSG_ERROR(
              [Please install boost >= 1.71 (including filesystem)])])
AX_BOOST_FILESYSTEM
AX_BOOST_SYSTEM
AX_BOOST_REGEX
AX_BOOST_PROGRAM_OPTIONS
AX_BOOST_IOSTREAMS
AX_BOOST_THREAD
AC_CHECK_LIB([z], [adler32], [], [AC_MSG_ERROR([Please install zlib])])
AC_CHECK_LIB([jsoncpp], [main], [], [AC_MSG_ERROR([Please install jsoncpp])])

# Checks for header files.
AC_CHECK_HEADERS([arpa/inet.h fcntl.h inttypes.h memory.h netinet/in.h stddef.h stdint.h stdlib.h string.h sys/time.h unistd.h])

# Checks for typedefs, structures, and compiler characteristics.
AC_CHECK_HEADER_STDBOOL
AC_C_INLINE
AC_TYPE_INT16_T
AC_TYPE_INT32_T
AC_TYPE_INT64_T
AC_TYPE_INT8_T
AC_TYPE_OFF_T
AC_TYPE_PID_T
AC_TYPE_SIZE_T
AC_TYPE_SSIZE_T
AC_TYPE_UINT16_T
AC_TYPE_UINT32_T
AC_TYPE_UINT64_T
AC_TYPE_UINT8_T

# Checks for library functions.
AC_FUNC_MALLOC
AC_FUNC_MMAP
AC_FUNC_REALLOC
AC_CHECK_FUNCS([clock_gettime gettimeofday memmove memset munmap regcomp strchr strdup strerror strrchr strstr strtol])

AC_CONFIG_FILES([
        Makefile
        test/Makefile
        test/integ/Makefile
        test/unit/Makefile])
AC_OUTPUT
