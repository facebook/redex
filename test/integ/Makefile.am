include $(top_srcdir)/Makefile.inc
include $(top_srcdir)/test/Makefile.inc

AM_CXXFLAGS = --std=gnu++14
AM_CPPFLAGS = $(COMMON_INCLUDES) $(COMMON_TEST_INCLUDES)

LDADD = $(COMMON_TEST_LIBS)

# Need to set "dexfile" environment variable to point to the dex file ised in
# the test. We do this with a helper script and naming convention.
LOG_COMPILER = sh
AM_LOG_FLAGS = -c 'dexfile=$0-class.dex $0'

TESTS = \
	synth_test \
	propagation_test \
	constant_propagation_test

synth_test_SOURCES = SynthTest.cpp
EXTRA_synth_test_DEPENDENCIES = synth_test-class.dex

propagation_test_SOURCES = PropagationTest.cpp
EXTRA_propagation_test_DEPENDENCIES = propagation_test-class.dex

constant_propagation_test_SOURCES = ConstantPropagationTest.cpp
EXTRA_constant_propagation_test_DEPENDENCIES = constant_propagation_test-class.dex

check_PROGRAMS = $(TESTS)

# Note: Should switch to d8, instead.
javac_source_target = -source 1.7 -target 1.7

define create_jar
	mkdir -p $@.tmp
	javac $(javac_source_target) -d $@.tmp $^
	jar cf $@ -C $@.tmp .
endef

SUFFIXES = .dex

.jar.dex:
	dx --dex --output=$@ $^

synth_test-class.jar: Alpha.java SynthTest.java
	$(create_jar)

propagation_test-class.jar: Propagation.java
	$(create_jar)

constant_propagation_test-class.jar: ConstantPropagation.java
	$(create_jar)
