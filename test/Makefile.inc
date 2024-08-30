# TODO: Unify with Makefile.am (or maybe autodetect)
GOOGLETEST_MIN_VERSION = 1.10.0
GOOGLETEST_DIR = googletest-release-$(GOOGLETEST_MIN_VERSION)/googletest
GOOGLEMOCK_DIR = googletest-release-$(GOOGLETEST_MIN_VERSION)/googlemock

#
# Include paths
#
COMMON_TEST_INCLUDES = \
    -I$(top_srcdir)/test/common \
    -I$(top_builddir)/test/$(GOOGLEMOCK_DIR)/include \
    -I$(top_builddir)/test/$(GOOGLETEST_DIR)/include

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
	$(top_builddir)/test/libgtest_main.la

COMMON_MOCK_TEST_LIBS = \
    $(COMMON_BASE_TEST_LIBS) \
	$(top_builddir)/test/libgtest.la \
	$(top_builddir)/test/libgmock_main.la

# By default use PATH to find tools.
JAVAC = javac
