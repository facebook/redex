#
# Include paths
#
GOOGLETEST_INCLUDE_DIR = @GOOGLETEST_INCLUDE_DIR@
GOOGLETEST_TEST_LIBS = @GOOGLETEST_TEST_LIBS@
GOOGLETEST_MOCK_LIBS = @GOOGLETEST_MOCK_LIBS@

COMMON_TEST_INCLUDES = \
    -I$(top_srcdir)/test/common \
    -I$(GOOGLETEST_INCLUDE_DIR)

COMMON_BASE_TEST_LIBS = \
	$(top_builddir)/libredex.la \
	$(top_builddir)/libopt.la \
	$(BOOST_FILESYSTEM_LIB) \
	$(BOOST_SYSTEM_LIB) \
	$(BOOST_REGEX_LIB) \
	$(BOOST_PROGRAM_OPTIONS_LIB) \
	$(BOOST_THREAD_LIB) \
	-lpthread \
	-ldl

# Libs.
COMMON_TEST_LIBS = \
    $(COMMON_BASE_TEST_LIBS) \
    $(GOOGLETEST_TEST_LIBS)

COMMON_MOCK_TEST_LIBS = \
    $(COMMON_BASE_TEST_LIBS) \
    $(GOOGLETEST_MOCK_LIBS)

# By default use PATH to find tools.
JAVAC = javac
