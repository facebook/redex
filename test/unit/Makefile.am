# TODO: finish including all the tests
# TODO: Include all redex source cpp files

AM_CXXFLAGS = --std=gnu++14
AM_CPPFLAGS = \
	-I$(top_srcdir)/test/googletest-release-1.7.0/include \
	$(REDEX_INCLUDE_FLAGS)

TESTS = \
	config_parser_test \
	ev_arg_test \
	extract_native_test \
	fp_ev_test \
	proguard_map_test

TEST_LIBS = $(top_builddir)/test/libgtest_main.la $(top_builddir)/libredex.la

config_parser_test_SOURCES = ConfigParserTest.cpp
config_parser_test_LDADD = $(TEST_LIBS)

ev_arg_test_SOURCES = EvArgTest.cpp
ev_arg_test_LDADD = $(TEST_LIBS)

extract_native_test_SOURCES = ExtractNativeTest.cpp
extract_native_test_LDADD = $(TEST_LIBS)

fp_ev_test_SOURCES = FpEvTest.cpp
fp_ev_test_LDADD = $(TEST_LIBS)

proguard_map_test_SOURCES = ProguardMapTest.cpp
proguard_map_test_LDADD = $(TEST_LIBS)

check_PROGRAMS = $(TESTS)
