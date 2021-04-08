include $(top_srcdir)/Makefile.inc
include $(top_srcdir)/test/Makefile.inc

AM_CXXFLAGS = --std=gnu++14
AM_CPPFLAGS = $(COMMON_INCLUDES) $(COMMON_TEST_INCLUDES)

LDADD = $(COMMON_MOCK_TEST_LIBS)

# Need to set "dexfile" environment variable to point to the dex file ised in
# the test. We do this with a helper script and naming convention.
LOG_COMPILER = sh
AM_LOG_FLAGS = -c 'dexfile=$0-class.dex $0'

check_PROGRAMS = \
    ab_experiment_context_test \
    call_graph_test \
    clinit_side_effect_test \
    constant_propagation_test \
    constructor_dedup_test_test \
    copy_prop_test \
    dedup_blocks_test \
    dedup_vmethods_test \
    default_annotation_test \
    final_inline_analysis_test \
    global_type_analysis_test \
    instruction_sequence_outliner_test \
    iodi_test \
    ip_reflection_analysis_test \
    max_depth_test \
    method_override_graph_test \
    monotonic_fixpoint_test \
    peephole_test \
    points_to_semantics_test \
    propagation_test \
    pure_method_test \
    reachability_test \
    reflection_analysis_test \
    remove_unreachable_test \
    result_propagation_test \
    strip_debug_info_test \
    synth_test \
    type_analysis_transform_test \
    type_inference_test

TESTS = $(check_PROGRAMS)

ab_experiment_context_test_SOURCES = ABExperimentContextTest.cpp
EXTRA_ab_experiment_context_test_DEPENDENCIES = ab_experiment_context_test-class.dex

call_graph_test_SOURCES = CallGraphTest.cpp
EXTRA_call_graph_test_DEPENDENCIES = call_graph_test-class.dex

clinit_side_effect_test_SOURCES = ClinitSideEffect.cpp
EXTRA_clinit_side_effect_test_DEPENDENCIES = clinit_side_effect_test-class.dex

constant_propagation_test_SOURCES = ConstantPropagationTest.cpp
EXTRA_constant_propagation_test_DEPENDENCIES = constant_propagation_test-class.dex

constructor_dedup_test_test_SOURCES = ConstructorDedupTest.cpp
EXTRA_constructor_dedup_test_test_DEPENDENCIES = constructor_dedup_test_test-class.dex

copy_prop_test_SOURCES = CopyPropagationTest.cpp
EXTRA_copy_prop_test_DEPENDENCIES = copy_prop_test-class.dex

dedup_blocks_test_SOURCES = DedupBlocksTest.cpp
EXTRA_dedup_blocks_test_DEPENDENCIES = dedup_blocks_test-class.dex

dedup_vmethods_test_SOURCES = DedupVirtualMethodsTest.cpp
EXTRA_dedup_vmethods_test_DEPENDENCIES = dedup_vmethods_test-class.dex

default_annotation_test_SOURCES = DefaultAnnotation.cpp
EXTRA_default_annotation_test_DEPENDENCIES = default_annotation_test-class.dex

final_inline_analysis_test_SOURCES = FinalInlineAnalysisTest.cpp
EXTRA_final_inline_analysis_test_DEPENDENCIES = final_inline_analysis_test-class.dex

access_marking_test_SOURCES = AccessMarkingTest.cpp
EXTRA_access_marking_test_DEPENDENCIES = access_marking_test-class.dex

global_type_analysis_test_SOURCES = GlobalTypeAnalysisTest.cpp
EXTRA_global_type_analysis_test_DEPENDENCIES = global_type_analysis_test-class.dex

instruction_sequence_outliner_test_SOURCES = InstructionSequenceOutlinerTest.cpp
EXTRA_instruction_sequence_outliner_test_DEPENDENCIES = instruction_sequence_outliner_test-class.dex

iodi_test_SOURCES = IODI.cpp
EXTRA_iodi_test_DEPENDENCIES = iodi_test-class.dex

ip_reflection_analysis_test_SOURCES = IPReflectionAnalysisTest.cpp
EXTRA_ip_reflection_analysis_test_DEPENDENCIES = ip_reflection_analysis_test-class.dex

max_depth_test_SOURCES = MaxDepthAnalysisTest.cpp
EXTRA_max_depth_test_DEPENDENCIES = max_depth_test-class.dex

method_override_graph_test_SOURCES = MethodOverrideGraphTest.cpp
EXTRA_method_override_graph_test_DEPENDENCIES = method_override_graph_test-class.dex

monotonic_fixpoint_test_SOURCES = MonotonicFixpointTest.cpp
EXTRA_monotonic_fixpoint_test_DEPENDENCIES = monotonic_fixpoint_test-class.dex

peephole_test_SOURCES = PeepholeTest.cpp
EXTRA_peephole_test_DEPENDENCIES = peephole_test-class.dex

points_to_semantics_test_SOURCES = PointsToSemanticsTest.cpp
EXTRA_points_to_semantics_test_DEPENDENCIES = points_to_semantics_test-class.dex

propagation_test_SOURCES = PropagationTest.cpp
EXTRA_propagation_test_DEPENDENCIES = propagation_test-class.dex

pure_method_test_SOURCES = PureFunctionTest.cpp
EXTRA_pure_method_test_DEPENDENCIES = pure_method_test-class.dex

reachability_test_SOURCES = ReachabilityTest.cpp
EXTRA_reachability_test_DEPENDENCIES = reachability_test-class.dex

reflection_analysis_test_SOURCES = ReflectionAnalysisTest.cpp
EXTRA_reflection_analysis_test_DEPENDENCIES = reflection_analysis_test-class.dex

remove_unreachable_test_SOURCES = RemoveUnreachableTest.cpp
EXTRA_remove_unreachable_test_DEPENDENCIES = remove_unreachable_test-class.dex

result_propagation_test_SOURCES = ResultPropagationTest.cpp
EXTRA_result_propagation_test_DEPENDENCIES = result_propagation_test-class.dex

strip_debug_info_test_SOURCES = StripDebugInfoTest.cpp
EXTRA_strip_debug_info_test_DEPENDENCIES = strip_debug_info_test-class.dex

synth_test_SOURCES = SynthTest.cpp
EXTRA_synth_test_DEPENDENCIES = synth_test-class.dex

type_analysis_transform_test_SOURCES = TypeAnalysisTransformTest.cpp
EXTRA_type_analysis_transform_test_DEPENDENCIES = type_analysis_transform_test-class.dex

type_inference_test_SOURCES = TypeInferenceTest.cpp
EXTRA_type_inference_test_DEPENDENCIES = type_inference_test-class.dex

# Note: Should switch to d8, instead.
javac_source_target = -source 1.7 -target 1.7

define create_jar
	mkdir -p $@.tmp
	$(JAVAC) $(javac_source_target) -d $@.tmp $^
	jar cf $@ -C $@.tmp .
endef

SUFFIXES = .dex

.jar.dex:
	$(DX) --dex --output=$@ $^

ab_experiment_context_test-class.jar: ABExperimentContextTest.java
	$(create_jar)

call_graph_test-class.jar: CallGraphTest.java
	$(create_jar)

clinit_side_effect_test-class.jar: ClinitSideEffect.java
	$(create_jar)

constant_propagation_test-class.jar: ConstantPropagation.java
	$(create_jar)

constructor_dedup_test_test-class.jar: ConstructorDedupTest.java
	$(create_jar)

copy_prop_test-class.jar: CopyPropagationTest.java
	$(create_jar)

dedup_blocks_test-class.jar: DedupBlocksTest.java
	$(create_jar)

dedup_vmethods_test-class.jar: DedupVirtualMethodsTest.java
	$(create_jar)

default_annotation_test-class.jar: DefaultAnnotationTest.java
	$(create_jar)

final_inline_analysis_test-class.jar: FinalInlineAnalysisTest.java
	$(create_jar)

global_type_analysis_test-class.jar: GlobalTypeAnalysisTest.java
	$(create_jar)

instruction_sequence_outliner_test-class.jar: InstructionSequenceOutlinerTest.java
	$(create_jar)

iodi_test-class.jar: IODI.java
	$(create_jar)

ip_reflection_analysis_test-class.jar: IPReflectionAnalysisTest.java
	$(create_jar)

max_depth_test-class.jar: MaxDepthAnalysisTest.java
	$(create_jar)

method_override_graph_test-class.jar: MethodOverrideGraphTest.java
	$(create_jar)

monotonic_fixpoint_test-class.jar: MonotonicFixpoint.java
	$(create_jar)

peephole_test-class.jar: Peephole.java
	$(create_jar)

points_to_semantics_test-class.jar: PointsToSemantics.java
	$(create_jar)

propagation_test-class.jar: Propagation.java
	$(create_jar)

pure_method_test-class.jar: PureFunctionTest.java
	$(create_jar)

reachability_test-class.jar: RemoveUnreachableTest.java
	$(create_jar)

reflection_analysis_test-class.jar: ReflectionAnalysis.java
	$(create_jar)

remove_unreachable_test-class.jar: RemoveUnreachableTest.java
	$(create_jar)

result_propagation_test-class.jar: ResultPropagation.java
	$(create_jar)

strip_debug_info_test-class.jar: StripDebugInfo.java
	$(create_jar)

synth_test-class.jar: Alpha.java SynthTest.java
	$(create_jar)

type_analysis_transform_test-class.jar: TypeAnalysisTransformTest.java
	$(create_jar)

type_inference_test-class.jar: TypeInferenceTest.java
	$(create_jar)
