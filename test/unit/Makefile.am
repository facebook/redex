include $(top_srcdir)/Makefile.inc
include $(top_srcdir)/test/Makefile.inc

AM_CXXFLAGS = --std=gnu++14
AM_CPPFLAGS = $(COMMON_INCLUDES) $(COMMON_TEST_INCLUDES)

LDADD = $(COMMON_TEST_LIBS)

TESTS = \
	ev_arg_test \
	extract_native_test \
	fp_ev_test \
	proguard_map_test

ev_arg_test_SOURCES = EvArgTest.cpp

extract_native_test_SOURCES = ExtractNativeTest.cpp

fp_ev_test_SOURCES = FpEvTest.cpp

proguard_map_test_SOURCES = ProguardMapTest.cpp
proguard_map_test_LDADD = $(COMMON_MOCK_TEST_LIBS)

check_PROGRAMS = $(TESTS)
