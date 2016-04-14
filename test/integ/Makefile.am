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
	synth_test \
	empty_classes_test

TEST_LIBS = $(top_builddir)/test/libgtest_main.la $(top_builddir)/libredex.la

synth_test_SOURCES = SynthTest.cpp
synth_test_LDADD = $(TEST_LIBS)
EXTRA_synth_test_DEPENDENCIES = synth-test-class.dex

empty_classes_test_SOURCES = EmptyClassesTest.cpp
empty_classes_test_LDADD = $(TEST_LIBS)
EXTRA_empty_classes_test_DEPENDENCIES = empty-classes-test-class.dex

check_PROGRAMS = $(TESTS)

synth-test-class.jar: Alpha.java SynthTest.java
	mkdir -p synth-test-class
	javac -d synth-test-class $^
	jar cf $@ -C synth-test-class .

synth-test-class.dex: synth-test-class.jar
	dx --dex --output=$@ $^

empty-classes-test-class.jar: EmptyClasses.java EmptyClassesTest.java
	mkdir -p empty-classes-test-class
	javac -d empty-classes-test-class $^
	jar cf $@ -C empty-classes-test-class .

empty-classes-test-class.dex: empty-classes-test-class.jar
	dx --dex --output=$@ $^
