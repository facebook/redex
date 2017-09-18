AM_CXXFLAGS = --std=gnu++14
AM_CPPFLAGS = \
	-I$(top_srcdir)/test/googletest-release-1.7.0/include \
	-I$(top_srcdir)/liblocator \
	-I$(top_srcdir)/libredex \
	-I$(top_srcdir)/libresource \
	-I$(top_srcdir)/libresource/android \
	-I$(top_srcdir)/libresource/androidfw \
	-I$(top_srcdir)/libresource/cutils \
	-I$(top_srcdir)/libresource/system \
	-I$(top_srcdir)/libresource/utils \
	-I$(top_srcdir)/opt \
	-I$(top_srcdir)/opt/all-static \
	-I$(top_srcdir)/opt/annokill \
	-I$(top_srcdir)/opt/bridge \
	-I$(top_srcdir)/opt/constant_propagation \
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
	-I$(top_srcdir)/opt/track_resources \
	-I$(top_srcdir)/opt/unterface \
	-I$(top_srcdir)/tools/redex-all \
	-I$(top_srcdir)/util \
	-I/usr/include/jsoncpp

TESTS = \
	synth_test \
	empty_classes_test \
	propagation_test \
	constant_propagation_test

TEST_LIBS = $(top_builddir)/test/libgtest_main.la $(top_builddir)/libredex.la

synth_test_SOURCES = SynthTest.cpp
synth_test_LDADD = $(TEST_LIBS)
EXTRA_synth_test_DEPENDENCIES = synth-test-class.dex

empty_classes_test_SOURCES = EmptyClassesTest.cpp
empty_classes_test_LDADD = $(TEST_LIBS)
EXTRA_empty_classes_test_DEPENDENCIES = empty-classes-test-class.dex

propagation_test_SOURCES = PropagationTest.cpp
propagation_test_LDADD = $(TEST_LIBS)
EXTRA_propagation_test_DEPENDENCIES = propagation-test-class.dex

constant_propagation_test_SOURCES = ConstantPropagationTest.cpp
constant_propagation_test_LDADD = $(TEST_LIBS)
EXTRA_constant_propagation_test_DEPENDENCIES = constant-propagation-test-class.dex

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

propagation-test-class.jar: Propagation.java
	mkdir -p propagation-test-class
	javac -d propagation-test-class $^
	jar cf $@ -C propagation-test-class .

propagation-test-class.dex: propagation-test-class.jar
	dx --dex --output=$@ $^

constant-propagation-test-class.jar: ConstantPropagation.java
	mkdir -p constant-propagation-test-class
	javac -d constant-propagation-test-class $^
	jar cf $@ -C constant-propagation-test-class .

constant-propagation-test-class.dex: constant-propagation-test-class.jar
	dx --dex --output=$@ $^
