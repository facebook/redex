SUBDIRS = . samples integ unit

check_LTLIBRARIES = libgtest.la libgtest_main.la libgmock_main.la

GOOGLETEST_MIN_VERSION = 1.14.0
GOOGLETEST_RELEASE_DIR = googletest-release-$(GOOGLETEST_MIN_VERSION)
GOOGLETEST_DIR = $(GOOGLETEST_RELEASE_DIR)/googletest
GOOGLEMOCK_DIR = $(GOOGLETEST_RELEASE_DIR)/googlemock

libgtest_la_CPPFLAGS = -I$(GOOGLETEST_DIR) -I$(GOOGLETEST_DIR)/include
libgtest_la_SOURCES = $(GOOGLETEST_DIR)/src/gtest-all.cc

libgtest_la_CPPFLAGS = -I$(GOOGLETEST_DIR) -I$(GOOGLETEST_DIR)/include
libgtest_la_SOURCES = $(GOOGLETEST_DIR)/src/gtest-all.cc

libgtest_main_la_CPPFLAGS = -I$(GOOGLETEST_DIR) -I$(GOOGLETEST_DIR)/include
libgtest_main_la_SOURCES = $(GOOGLETEST_DIR)/src/gtest_main.cc
libgtest_main_la_LIBADD = libgtest.la

libgmock_main_la_CPPFLAGS = -I$(GOOGLEMOCK_DIR) -I$(GOOGLEMOCK_DIR)/include -I$(GOOGLETEST_DIR)/include
libgmock_main_la_SOURCES = $(GOOGLEMOCK_DIR)/src/gmock-all.cc $(GOOGLEMOCK_DIR)/src/gmock_main.cc

# Hack: auto-download googletest.

GOOGLE_TEST_SOURCES = \
  $(GOOGLETEST_DIR)/src/gtest-all.cc \
  $(GOOGLETEST_DIR)/src/gtest_main.cc \
  $(GOOGLEMOCK_DIR)/src/gmock-all.cc \
  $(GOOGLEMOCK_DIR)/src/gmock_main.cc

BUILT_SOURCES = $(GOOGLE_TEST_SOURCES)

$(GOOGLE_TEST_SOURCES): $(GOOGLETEST_RELEASE_DIR)/setup_gtest

$(GOOGLETEST_RELEASE_DIR)/setup_gtest:
	curl https://codeload.github.com/google/googletest/tar.gz/v$(GOOGLETEST_MIN_VERSION) -o gtest.tar.gz
	rm -rf googletest-release-*
	mkdir $(GOOGLETEST_RELEASE_DIR)
	tar xvf gtest.tar.gz --strip-components=1 -C $(GOOGLETEST_RELEASE_DIR)
	touch $(GOOGLETEST_RELEASE_DIR)/setup_gtest

check_SCRIPTS = check_num_passes.sh
# _DEPENDENCIES does not seem to work for SCRIPTS
check_num_passes.sh: $(top_builddir)/redex-all

TESTS = check_num_passes.sh
