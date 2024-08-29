#                                               -*- Autoconf -*-
# Process this file with autoconf to produce a configure script.

AC_PREREQ([2.69])
AC_INIT([redex], [1.0], [not-valid-yet@fb.com])
AM_INIT_AUTOMAKE([subdir-objects])
# clear out default cxx flags (was "-O2 -g") so that they don't override
# the flags defined in AM_CXXFLAGS. add "-std" to work around gtest issue
# on macos.
: ${CXXFLAGS="-std=gnu++17"}

# Checks for programs.
AC_PROG_CXX
AC_PROG_CC

LT_INIT

AC_CONFIG_MACRO_DIR([m4])

AM_PATH_PYTHON([3.0], [], [AC_MSG_ERROR([Redex requires python3])])

# Checks for libraries.
AX_PTHREAD
AX_BOOST_BASE([1.71.0], [], [AC_MSG_ERROR(
              [Please install boost >= 1.71 (including filesystem)])])
AX_BOOST_FILESYSTEM
AX_BOOST_REGEX
AX_BOOST_PROGRAM_OPTIONS
AX_BOOST_IOSTREAMS
AX_BOOST_THREAD
AC_CHECK_LIB([z], [adler32], [], [AC_MSG_ERROR([Please install zlib])])
AC_CHECK_LIB([jsoncpp], [main], [], [AC_MSG_ERROR([Please install jsoncpp])])

# check whether user enabled protobuf
AC_ARG_ENABLE([protobuf],
    [AS_HELP_STRING([--enable-protobuf],
        [Enable the protobuf for AppBundle build])],
    [ AC_DEFINE(HAS_PROTOBUF) ]
)

AS_IF([test "x$enable_protobuf" = "xyes"], [
    # user enabled protobuf
    # check if protobuf is installed

    # proto compiler
    # allow users to specify the path to protobuf compiler
    # --with-protoc

    AC_ARG_WITH([protoc],
        [AS_HELP_STRING([--with-protoc=/path/to/protoc],
            [Location of the protobuf compiler.])],
        [PROTOC="$withval"],
        [ AS_IF([test "x${PROTOC}" == "x"],
            [AC_PATH_PROG([PROTOC], [protoc], [no])])
        ]
    )
    AS_IF([test "${PROTOC}" == "no"], [AC_MSG_ERROR([Protobuf compiler protoc not found.])])

    # protobuf libraries
    # allow users to specify the path to the protobuf libs
    # --with-protolib

    AC_ARG_WITH([protolib],
        [AS_HELP_STRING([--with-protolib=/path/to/protolibs],
            [Location of the protobuf lib dir.])],
        [ # protobuf lib path set by user
            LDFLAGS_ORIG=$LDFLAGS

            # test protobuf
            LDFLAGS="${LDFLAGS_ORIG} -L${withval}"
            AC_LANG_PUSH([C++])
            AC_CHECK_LIB([protobuf], [main], [
            # library found
                AC_SUBST([LIBPROTOBUF_LIBS], "-L${withval} -lprotobuf")],
                [AC_MSG_ERROR([Protobuf libraries not found for user specified path.])]
            )
            AC_LANG_POP([C++])
            # restore original LDFLAGS
            LDFLAGS=$LDFLAGS_ORIG
        ],
        [ # check default search path
            AC_CHECK_LIB([protobuf], [main], [
                AC_SUBST([LIBPROTOBUF_LIBS], "-lprotobuf")],
                [AC_MSG_ERROR([Protobuf libraries not found.])]
            )
        ]
    )

    # protobuf headers
    # allow users to specify the path to the protobuf headers
    # --with-protoheader

    AC_ARG_WITH([protoheader],
        [AS_HELP_STRING([--with-protoheader=/path/to/protoheaders],
            [Location of the protobuf include dir.])],
        [ # protobuf header path set by user
            CXXFLAGS_ORIG=$CXXFLAGS

            # test protobuf header
            CXXFLAGS="-std=gnu++17 ${CXXFLAGS_ORIG} -I${withval}"
            AC_LANG_PUSH([C++])
            AC_CHECK_HEADER([google/protobuf/io/coded_stream.h], [
                # library found
                AC_SUBST([PROTOBUF_CXXFLAGS], "-I${withval}")],
                [AC_MSG_ERROR([Protobuf headers not found for user specified path.])]
            )
            AC_LANG_POP([C++])
            # restore original CXXFLAGS
            CXXFLAGS=$CXXFLAGS_ORIG
        ],
        []
    )
])
AM_CONDITIONAL([SET_PROTOBUF],[test "x${enable_protobuf}" = "xyes"])


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
