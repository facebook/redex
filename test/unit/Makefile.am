AM_CXXFLAGS = --std=gnu++11
AM_CPPFLAGS = \
	-I$(top_srcdir)/test/gtest-1.7.0/include \
	-I$(top_srcdir)/configparser \
	-I$(top_srcdir)/include \
	-I$(top_srcdir)/liblocator \
	-I$(top_srcdir)/libresource \
	-I$(top_srcdir)/libresource/android \
	-I$(top_srcdir)/libresource/androidfw \
	-I$(top_srcdir)/libresource/cutils \
	-I$(top_srcdir)/libresource/system \
	-I$(top_srcdir)/libresource/utils \
	-I$(top_srcdir)/opt \
	-I$(top_srcdir)/opt/all-static \
	-I$(top_srcdir)/opt/annoclasskill \
	-I$(top_srcdir)/opt/annokill \
	-I$(top_srcdir)/opt/bridge \
	-I$(top_srcdir)/opt/delinit \
	-I$(top_srcdir)/opt/delsuper \
	-I$(top_srcdir)/opt/final_inline \
	-I$(top_srcdir)/opt/interdex \
	-I$(top_srcdir)/opt/local-dce \
	-I$(top_srcdir)/opt/peephole \
	-I$(top_srcdir)/opt/rebindrefs \
	-I$(top_srcdir)/opt/remove_empty_classes \
	-I$(top_srcdir)/opt/renameclasses \
	-I$(top_srcdir)/opt/shorten-srcstrings \
	-I$(top_srcdir)/opt/simpleinline \
	-I$(top_srcdir)/opt/singleimpl \
	-I$(top_srcdir)/opt/static-sink \
	-I$(top_srcdir)/opt/staticrelo \
	-I$(top_srcdir)/opt/synth \
	-I$(top_srcdir)/opt/unterface \
	-I$(top_srcdir)/tools/redex-all \
	-I$(top_srcdir)/third-party/folly \
	-I$(top_srcdir)/util

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
