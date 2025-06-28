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

AM_PATH_PYTHON([3.9], [], [AC_MSG_ERROR([Redex requires python >= 3.9])])

# Checks for libraries.
AX_PTHREAD
AX_BOOST_BASE([1.74.0], [], [AC_MSG_ERROR(
              [Please install boost >= 1.74 (including filesystem)])])
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
        [Enable the protobuf for AppBundle build])]
)

AS_IF([test "x$enable_protobuf" = "xyes"], [
    # user enabled protobuf
    # check if protobuf is installed
    AC_DEFINE(HAS_PROTOBUF)

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

# Check for Android SDK (for tests).
AC_ARG_WITH([android-sdk],
    [AS_HELP_STRING([--with-android-sdk=/path/to/android-sdk],
        [Location of the Android SDK, for testing.])],
    [ANDROID_HOME="$withval"],
    [NO_ANDROID_HOME="no"]
    [ AS_IF([test "x${PROTOC}" == "x"],
        [AC_PATH_PROG([PROTOC], [protoc], [no])])
    ]
)
# Look for dx & android.jar.
AS_IF([test "x$NO_ANDROID_HOME" = "xno"],
    [],
    [
        # Do not assume a totally new SDK. Try platform 29.
        AC_PATH_PROG(
            DX,
            dx,
            no,
            "$ANDROID_HOME/build-tools/29.0.2:$PATH"
        )
        AS_IF([test "x$DX" = "xno"],
            [AC_MSG_ERROR([--with-android-sdk option was specified but does not seem to point at a valid Android SDK installation])]
            []
        )
        AC_CHECK_FILE(
            "$ANDROID_HOME/platforms/android-29/android.jar",
            [
                AC_SUBST(ANDROID_JAR,"$ANDROID_HOME/platforms/android-29/android.jar")
                AC_SUBST(ANDROID_SDK,"$ANDROID_HOME")
                AC_SUBST(ANDROID_PLATFORM_VERSION,"android-29")
            ],
            [AC_MSG_ERROR([--with-android-sdk option was specified but does not seem to point at a valid Android SDK installation])]
        )
    ]
)

# check whether user enabled tests
AC_ARG_ENABLE([tests],
    [AS_HELP_STRING([--disable-tests],
        [Disable running tests])]
)
AS_IF([test "x$enable_tests" != "xno"], [
    # user enabled tests
    # check if googletest is installed

    # allow users to specify the path to googletest
    # --with-googletest

    AC_ARG_WITH([googletest],
        [AS_HELP_STRING([--with-googletest=/path/to/googletest],
            [Location to googletest installation dir.])],
        [
            GOOGLETEST_DIR="$withval"
            GOOGLETEST_INCLUDE_DIR="$GOOGLETEST_DIR/include"
            GOOGLETEST_LIB_DIR="$GOOGLETEST_DIR/lib"
        ]
    )
    AC_LANG_PUSH([C++])
    save_CXXFLAGS="$CXXFLAGS"
    CXXFLAGS=
    if test "x$GOOGLETEST_INCLUDE_DIR" != x ; then
        CXXFLAGS="-I$GOOGLETEST_INCLUDE_DIR"
    fi
    save_CPPFLAGS="$CPPFLAGS"
    CPPFLAGS="$CXXFLAGS"
    AC_CHECK_HEADERS(
        [gtest/gtest.h gmock/gmock.h],
        [AC_SUBST(GOOGLETEST_INCLUDE_DIR,"$GOOGLETEST_INCLUDE_DIR")],
        [AC_MSG_ERROR([googletest not detected or --with-googletest does not seem to point at a valid GoogleTest installation with the header files.])])
    CXXFLAGS="$save_CXXFLAGS"
    CPPFLAGS="$save_CPPFLAGS"

    save_LDFLAGS="$LDFLAGS"
    LDFLAGS=
    if test "x$GOOGLETEST_LIB_DIR" != x ; then
        LDFLAGS="-L$GOOGLETEST_LIB_DIR"
    fi
    save_LIBS="$LIBS"
    LIBS=
    AC_CHECK_LIB(gtest_main, main,
        [AC_SUBST(GOOGLETEST_TEST_LIBS,"$LDFLAGS -lgtest_main -lgtest")],
        [AC_MSG_ERROR([googletest not detected or --with-googletest does not seem to point at a valid GoogleTest installation with gtest_main.])])
    AC_CHECK_LIB(gmock_main, main,
        [AC_SUBST(GOOGLETEST_MOCK_LIBS,"$LDFLAGS -lgmock_main -lgmock -lgtest")],
        [AC_MSG_ERROR([googletest not detected or --with-googletest does not seem to point at a valid GoogleTest installation with gmock_main.])])
    LDFLAGS="$save_LDFLAGS"
    LIBS="$save_LIBS"
    AC_LANG_POP([C++])

    # Test above may lead to a false negative because the headers may be present
    # in a standard directory but not the specified googletest path.
    if test "x$GOOGLETEST_DIR" != x ; then
        AC_CHECK_FILES(
            [$GOOGLETEST_INCLUDE_DIR/gtest/gtest.h
             $GOOGLETEST_INCLUDE_DIR/gmock/gmock.h
             $GOOGLETEST_LIB_DIR/libgtest_main.a
             $GOOGLETEST_LIB_DIR/libgtest.a
             $GOOGLETEST_LIB_DIR/libgmock_main.a
             $GOOGLETEST_LIB_DIR/libgmock.a],
            [],
            [AC_MSG_ERROR([--with-googletest is specified but does not seem to point at a valid GoogleTest installation with necessary header and library files])])
    fi
])

# check whether user enabled kotlin tests
AC_ARG_ENABLE([kotlin-tests],
    [AS_HELP_STRING([--disable-kotlin-tests],
        [Disable running kotlin tests])]
)
AM_CONDITIONAL([RUN_KOTLIN_TESTS],[test "x$enable_kotlin_tests" != "xno"])
AS_IF([test "x$enable_kotlin_tests" != "xno"], [
    # user enabled kotlin tests
    # check if kotlinc is installed

    # allow users to specify the path to kotlinc
    # --with-kotlinc

    AC_ARG_WITH([kotlinc],
        [AS_HELP_STRING([--with-kotlinc=/path/to/kotlinc],
            [Location of the Kotlin compiler.])],
        [KOTLINC="$withval"],
        [ AS_IF([test "x${KOTLINC}" == "x"],
            [AC_PATH_PROG([KOTLINC], [kotlinc], [no_kotlinc])])
        ]
    )
    AS_IF([test "${KOTLINC}" == "no_kotlinc"], [AC_MSG_ERROR([Kotlin compiler kotlinc not found.])])
])

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
        test/unit/Makefile
        test/samples/Makefile
        test/samples/more_resources/Makefile
        test/samples/strings/Makefile
        ])
AC_OUTPUT
