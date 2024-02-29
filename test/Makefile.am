SUBDIRS = . integ unit

check_LTLIBRARIES = libgtest.la libgtest_main.la libgmock_main.la

# Note: Use at least 1.10 as we need googlemock, too.
GOOGLETEST_MIN_VERSION = 1.10.0

libgtest_la_CPPFLAGS = -I$(srcdir)/googletest-release-$(GOOGLETEST_MIN_VERSION)/googletest -I$(srcdir)/googletest-release-$(GOOGLETEST_MIN_VERSION)/googletest/include
libgtest_la_SOURCES = googletest-release-$(GOOGLETEST_MIN_VERSION)/googletest/src/gtest-all.cc

libgtest_main_la_CPPFLAGS = -I$(srcdir)/googletest-release-$(GOOGLETEST_MIN_VERSION)/googletest -I$(srcdir)/googletest-release-$(GOOGLETEST_MIN_VERSION)/googletest/include
libgtest_main_la_SOURCES = googletest-release-$(GOOGLETEST_MIN_VERSION)/googletest/src/gtest_main.cc
libgtest_main_la_LIBADD = libgtest.la

libgmock_main_la_CPPFLAGS = -I$(srcdir)/googletest-release-$(GOOGLETEST_MIN_VERSION)/googlemock -I$(srcdir)/googletest-release-$(GOOGLETEST_MIN_VERSION)/googlemock/include -I$(srcdir)/googletest-release-$(GOOGLETEST_MIN_VERSION)/googletest/include
libgmock_main_la_SOURCES = googletest-release-$(GOOGLETEST_MIN_VERSION)/googlemock/src/gmock-all.cc googletest-release-$(GOOGLETEST_MIN_VERSION)/googlemock/src/gmock_main.cc

# Hack: auto-download googletest.

googletest-release-$(GOOGLETEST_MIN_VERSION)/googletest/src/gtest-all.cc \
googletest-release-$(GOOGLETEST_MIN_VERSION)/googletest/src/gtest_main.cc \
googletest-release-$(GOOGLETEST_MIN_VERSION)/googlemock/src/gmock-all.cc \
googletest-release-$(GOOGLETEST_MIN_VERSION)/googlemock/src/gmock_main.cc: setup_gtest

setup_gtest:
	curl https://codeload.github.com/google/googletest/zip/release-$(GOOGLETEST_MIN_VERSION) -o gtest-$(GOOGLETEST_MIN_VERSION).zip
	unzip -o gtest-$(GOOGLETEST_MIN_VERSION).zip
	touch setup_gtest

check_SCRIPTS = check_num_passes.sh
# _DEPENDENCIES does not seem to work for SCRIPTS
check_num_passes.sh: $(top_builddir)/redex-all

TESTS = check_num_passes.sh
