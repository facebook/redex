SUBDIRS = . integ unit

check_LTLIBRARIES = libgtest.la libgtest_main.la libgmock_main.la

libgtest_la_CPPFLAGS = -Igoogletest-release-1.8.0/googletest -Igoogletest-release-1.8.0/googletest/src -Igoogletest-release-1.8.0/googletest/include
libgtest_la_SOURCES = googletest-release-1.8.0/googletest/src/gtest-all.cc

libgtest_main_la_CPPFLAGS = -Igoogletest-release-1.8.0/googletest -Igoogletest-release-1.8.0/googletest/src -Igoogletest-release-1.8.0/googletest/include
libgtest_main_la_SOURCES = googletest-release-1.8.0/googletest/src/gtest_main.cc
libgtest_main_la_LIBADD = libgtest.la

libgmock_main_la_CPPFLAGS = -Igoogletest-release-1.8.0/googlemock -Igoogletest-release-1.8.0/googlemock/src -Igoogletest-release-1.8.0/googlemock/include -Igoogletest-release-1.8.0/googletest/include
libgmock_main_la_SOURCES = googletest-release-1.8.0/googlemock/src/gmock-all.cc googletest-release-1.8.0/googlemock/src/gmock_main.cc

# Hack: auto-download googletest.

googletest-release-1.8.0/googletest/src/gtest-all.cc \
googletest-release-1.8.0/googletest/src/gtest_main.cc \
googletest-release-1.8.0/googlemock/src/gmock-all.cc \
googletest-release-1.8.0/googlemock/src/gmock_main.cc: setup_gtest

.PHONY: setup_gtest
setup_gtest:
	./setup.sh

check_SCRIPTS = check_num_passes.sh
# _DEPENDENCIES does not seem to work for SCRIPTS
check_num_passes.sh: $(top_srcdir)/redex-all

TESTS = check_num_passes.sh
