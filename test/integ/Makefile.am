# TODO: finish including all the tests
# TODO: Include all redex source cpp files

AM_CXXFLAGS = --std=gnu++14
AM_CPPFLAGS = \
	-I$(top_srcdir)/test/googletest-release-1.7.0/include \
	$(REDEX_INCLUDE_FLAGS)

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

JAVA_BIN = `/usr/libexec/java_home -v 1.7`/bin

synth-test-class.jar: Alpha.java SynthTest.java
	mkdir -p synth-test-class
	$(JAVA_BIN)/javac -version
	$(JAVA_BIN)/javac -d synth-test-class $^
	$(JAVA_BIN)/jar cf $@ -C synth-test-class .

synth-test-class.dex: synth-test-class.jar
	dx --dex --output=$@ $^

empty-classes-test-class.jar: EmptyClasses.java EmptyClassesTest.java
	mkdir -p empty-classes-test-class
	$(JAVA_BIN)/javac -d empty-classes-test-class $^
	$(JAVA_BIN)/jar cf $@ -C empty-classes-test-class .

empty-classes-test-class.dex: empty-classes-test-class.jar
	dx --dex --output=$@ $^

propagation-test-class.jar: Propagation.java
	mkdir -p propagation-test-class
	$(JAVA_BIN)/javac -d propagation-test-class $^
	$(JAVA_BIN)/jar cf $@ -C propagation-test-class .

propagation-test-class.dex: propagation-test-class.jar
	dx --dex --output=$@ $^

constant-propagation-test-class.jar: ConstantPropagation.java
	mkdir -p constant-propagation-test-class
	$(JAVA_BIN)/javac -d constant-propagation-test-class $^
	$(JAVA_BIN)/jar cf $@ -C constant-propagation-test-class .

constant-propagation-test-class.dex: constant-propagation-test-class.jar
	dx --dex --output=$@ $^

clean-local:
	rm -rf *.dex
	rm -rf *.jar
