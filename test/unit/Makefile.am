include $(top_srcdir)/Makefile.inc
include $(top_srcdir)/test/Makefile.inc

AM_CXXFLAGS = --std=gnu++14
AM_CPPFLAGS = $(COMMON_INCLUDES) $(COMMON_TEST_INCLUDES)

LDADD = $(COMMON_TEST_LIBS)

# These need things in the environment.
# XFAIL_TESTS = stringbuilder_outline_test throw_propagation_test

# CircleCI shows XFAIL as red. Automake does not allow to $(filter). So for
# now remove the XFAIL_TESTS entries explicitly from here.

check_PROGRAMS = \
    aliased_registers_test \
    analysis_usage_test \
    array_propagation_test \
    blaming_escape_test \
    boxed_boolean_propagation_test \
    branch_prefix_hoisting_test \
    cfg_inliner_test \
    cfg_mutation_test \
    check_breadcrumbs_test \
    check_cast_analysis_test \
    concurrent_containers_test \
    configurable_test \
    constructor_analysis_test \
    control_flow_test \
    copy_propagation_test \
    cse_test \
    creators_test \
    debug_info_test \
    debug_test \
    dedup_blocks_test \
    dex_class_test \
    dex_instruction_test \
    dex_loader_test \
    dex_mutate_test \
    dex_output_test \
    dex_store_test \
    dex_type_environment_test \
    dex_util_test \
    dominators_test \
    ev_arg_test \
    ev_write_test \
    evaluate_type_checks_test \
    exception_test \
    extract_native_test \
    fbjni_marker_test \
    final_inline_test \
    final_inline_v2_test \
    fp_ev_test \
    global_type_analysis_test \
    graph_util_test \
    hierarchy_util_test \
    interprocedural_constant_propagation_test \
    intraprocedural_constant_propagation_test \
    ir_assembler_test \
    ir_code_test \
    ir_instruction_test \
    ir_list_test \
    ir_typechecker_test \
    java_parser_util_test \
    literals_test \
    live_range_test \
    local_dce_test \
    local_pointers_test \
    loop_info_test \
    loosen_access_modifier_test \
    match_flow_test \
    match_test \
    method_inline_test \
    method_merger_test \
    monitor_count_test \
    mutf8_compare_test \
    leb_test \
    native_test \
    native_names_test \
    null_propagation_test \
    object_inliner_test \
    object_propagation_test \
    optimize_enums_test \
    outliner_type_analysis_test \
    partial_pass_test \
    peephole_test \
    proguard_lexer_test \
    proguard_map_test \
    proguard_parser_test \
    proguard_regex_test \
    pure_analysis_test \
    random_forest_test \
    reaching_definitions_test \
    reduce_array_literals_test \
    reduce_gotos_test \
    reflection_analysis_test \
    reg_alloc_test \
    registers_test \
    remove_apilevel_checks_test \
    remove_gotos_test \
    remove_null_check_string_arg_test \
    remove_recursive_locks_test \
    remove_redundant_check_casts_test \
    remove_uninstantiables_test \
    remove_unused_args_test \
    renamer_test \
    resolver_test \
    resolve_proguard_value_test \
    result_propagation_test \
    side_effects_summary_test \
    signed_constant_propagation_test \
    source_blocks_test \
    split_huge_switch_test \
    static_relo_v2_test \
    strip_debug_info_test \
    switch_dispatch_test \
    timer_test \
    trace_multithreading_test \
    true_virtuals_test \
    type_analysis_transform_test \
    type_inference_test \
    type_ref_updater_test \
    type_reference_test \
    type_system_test \
    type_util_test \
    up_code_motion_test \
    used_vars_test \
    virt_scope_test \
    virtual_merging_test \
    walkers_test \
    work_queue_test \
    xstorerefs_test

aliased_registers_test_SOURCES = AliasedRegistersTest.cpp

analysis_usage_test_SOURCES = AnalysisUsageTest.cpp

array_propagation_test_SOURCES = constant-propagation/ArrayPropagationTest.cpp
array_propagation_test_CPPFLAGS = $(COMMON_INCLUDES) $(COMMON_TEST_INCLUDES) -I$(top_srcdir)/sparta/test

blaming_escape_test_SOURCES = BlamingEscapeTest.cpp
blaming_escape_test_LDADD = $(COMMON_MOCK_TEST_LIBS)

boxed_boolean_propagation_test_SOURCES = constant-propagation/BoxedBooleanPropagationTest.cpp

branch_prefix_hoisting_test_SOURCES = BranchPrefixHoistingTest.cpp ScopeHelper.cpp

cfg_inliner_test_SOURCES = CFGInlinerTest.cpp
cfg_inliner_test_LDADD = $(COMMON_MOCK_TEST_LIBS)

cfg_mutation_test_SOURCES = CFGMutationTest.cpp
cfg_mutation_test_LDADD = $(COMMON_MOCK_TEST_LIBS)

check_breadcrumbs_test_SOURCES = CheckBreadCrumbsTest.cpp

check_cast_analysis_test_SOURCES = CheckCastAnalysisTest.cpp

concurrent_containers_test_SOURCES = ConcurrentContainersTest.cpp

configurable_test_SOURCES = ConfigurableTest.cpp
configurable_test_LDADD = $(COMMON_MOCK_TEST_LIBS)

constructor_analysis_test_SOURCES = ConstructorAnalysisTest.cpp

control_flow_test_SOURCES = ControlFlowTest.cpp
control_flow_test_LDADD = $(COMMON_MOCK_TEST_LIBS)

copy_propagation_test_SOURCES = CopyPropagationTest.cpp

cse_test_SOURCES = CommonSubexpressionEliminationTest.cpp
cse_test_LDADD = $(COMMON_MOCK_TEST_LIBS)

creators_test_SOURCES = CreatorsTest.cpp

debug_info_test_SOURCES = DebugInfoTest.cpp

debug_test_SOURCES = DebugTest.cpp

dedup_blocks_test_SOURCES = DedupBlocksTest.cpp VirtScopeHelper.cpp ScopeHelper.cpp

dex_class_test_SOURCES = DexClassTest.cpp

dex_instruction_test_SOURCES = DexInstructionTest.cpp

dex_loader_test_SOURCES = DexLoaderTest.cpp

dex_mutate_test_SOURCES = DexMutateTest.cpp

dex_output_test_SOURCES = DexOutputTest.cpp

dex_store_test_SOURCES = DexStoreTest.cpp

dex_type_environment_test_SOURCES = type-analysis/DexTypeEnvironmentTest.cpp

dex_util_test_SOURCES = DexUtilTest.cpp

dominators_test_SOURCES = DominatorsTest.cpp

ev_arg_test_SOURCES = EvArgTest.cpp

ev_write_test_SOURCES = EvWriteTest.cpp

evaluate_type_checks_test_SOURCES = EvaluateTypeChecksTest.cpp

exception_test_SOURCES = RedexExceptionTest.cpp

extract_native_test_SOURCES = ExtractNativeTest.cpp

fbjni_marker_test_SOURCES = FbjniMarkerTest.cpp

final_inline_test_SOURCES = FinalInlineTest.cpp

final_inline_v2_test_SOURCES = FinalInlineV2Test.cpp

fp_ev_test_SOURCES = FpEvTest.cpp

global_type_analysis_test_SOURCES = type-analysis/GlobalTypeAnalysisTest.cpp

graph_util_test_SOURCES = GraphUtilTest.cpp

hierarchy_util_test_SOURCES = HierarchyUtilTest.cpp
hierarchy_util_test_LDADD = $(COMMON_MOCK_TEST_LIBS)

interprocedural_constant_propagation_test_SOURCES = constant-propagation/IPConstantPropagationTest.cpp

intraprocedural_constant_propagation_test_SOURCES = constant-propagation/ConstantPropagationTest.cpp

ir_assembler_test_SOURCES = IRAssemblerTest.cpp

ir_code_test_SOURCES = IRCodeTest.cpp

ir_instruction_test_SOURCES = IRInstructionTest.cpp OpcodeList.cpp

ir_list_test_SOURCES = IRListTest.cpp
ir_list_test_LDADD = $(COMMON_MOCK_TEST_LIBS)

ir_typechecker_test_SOURCES = IRTypeCheckerTest.cpp
ir_typechecker_test_LDADD = $(COMMON_MOCK_TEST_LIBS)

java_parser_util_test_SOURCES = JavaParserUtilTest.cpp
java_parser_util_test_LDADD = $(COMMON_MOCK_TEST_LIBS)

literals_test_SOURCES = LiteralsTest.cpp

live_range_test_SOURCES = LiveRangeTest.cpp
live_range_test_LDADD = $(COMMON_MOCK_TEST_LIBS)

local_dce_test_SOURCES = LocalDceTest.cpp ScopeHelper.cpp

local_pointers_test_SOURCES = LocalPointersTest.cpp
local_pointers_test_LDADD = $(COMMON_MOCK_TEST_LIBS)

loop_info_test_SOURCES = LoopInfoTest.cpp
loop_info_test_LDADD = $(COMMON_MOCK_TEST_LIBS)

loosen_access_modifier_test_SOURCES = LoosenAccessModifierTest.cpp

match_test_SOURCES = MatchTest.cpp

match_flow_test_SOURCES = MatchFlowTest.cpp
match_flow_test_LDADD = $(COMMON_MOCK_TEST_LIBS)

method_inline_test_SOURCES = MethodInlineTest.cpp

method_merger_test_SOURCES = MethodMergerTest.cpp

monitor_count_test_SOURCES = MonitorCountTest.cpp

mutf8_compare_test_SOURCES = Mutf8CompareTest.cpp

leb_test_SOURCES = LebTest.cpp

native_test_SOURCES = NativeTest.cpp

native_names_test_SOURCES = NativeNamesTest.cpp

null_propagation_test_SOURCES = constant-propagation/NullPropagationTest.cpp

object_inliner_test_SOURCES = ObjectInlinerTest.cpp
object_inliner_test_LDADD = $(COMMON_MOCK_TEST_LIBS)

object_propagation_test_SOURCES = constant-propagation/ObjectPropagationTest.cpp
object_propagation_test_CPPFLAGS = $(COMMON_INCLUDES) $(COMMON_TEST_INCLUDES) -I$(top_srcdir)/sparta/test

optimize_enums_test_SOURCES = OptimizeEnumsTest.cpp
optimize_enums_test_LDADD = $(COMMON_MOCK_TEST_LIBS)

outliner_type_analysis_test_SOURCES = OutlinerTypeAnalysisTest.cpp

partial_pass_test_SOURCES = PartialPassTest.cpp

peephole_test_SOURCES = PeepholeTest.cpp

proguard_lexer_test_SOURCES = ProguardLexerTest.cpp

proguard_map_test_SOURCES = ProguardMapTest.cpp
proguard_map_test_LDADD = $(COMMON_MOCK_TEST_LIBS)

proguard_parser_test_SOURCES = ProguardParserTest.cpp

proguard_regex_test_SOURCES = ProguardRegexTest.cpp

pure_analysis_test_SOURCES = PureAnalysisTest.cpp
pure_analysis_test_LDADD = $(COMMON_MOCK_TEST_LIBS)

random_forest_test_SOURCES = RandomForestTest.cpp

reaching_definitions_test_SOURCES = ReachingDefinitionsTest.cpp

reduce_array_literals_test_SOURCES = ReduceArrayLiteralsTest.cpp
reduce_array_literals_test_LDADD = $(COMMON_MOCK_TEST_LIBS)

reduce_gotos_test_SOURCES = ReduceGotosTest.cpp
reduce_gotos_test_LDADD = $(COMMON_MOCK_TEST_LIBS)

reflection_analysis_test_SOURCES = ReflectionAnalysisTest.cpp
reflection_analysis_test_LDADD = $(COMMON_MOCK_TEST_LIBS)

reg_alloc_test_SOURCES = RegAllocTest.cpp OpcodeList.cpp
reg_alloc_test_LDADD = $(COMMON_MOCK_TEST_LIBS)

registers_test_SOURCES = RegistersTest.cpp OpcodeList.cpp

remove_apilevel_checks_test_SOURCES = RemoveApiLevelChecksTest.cpp
remove_apilevel_checks_test_LDADD = $(COMMON_MOCK_TEST_LIBS)

remove_gotos_test_SOURCES = RemoveGotosTest.cpp VirtScopeHelper.cpp ScopeHelper.cpp

remove_null_check_string_arg_test_SOURCES = RemoveNullCheckStringArgTest.cpp ScopeHelper.cpp

remove_recursive_locks_test_SOURCES = RemoveRecursiveLocksTest.cpp

remove_redundant_check_casts_test_SOURCES = RemoveRedundantCheckCastsTest.cpp VirtScopeHelper.cpp ScopeHelper.cpp

remove_uninstantiables_test_SOURCES = RemoveUninstantiablesTest.cpp ScopeHelper.cpp
remove_uninstantiables_test_LDADD = $(COMMON_MOCK_TEST_LIBS)

remove_unused_args_test_SOURCES = RemoveUnusedArgsTest.cpp ScopeHelper.cpp
remove_unused_args_test_LDADD = $(COMMON_MOCK_TEST_LIBS)

renamer_test_SOURCES = RenamerTest.cpp VirtScopeHelper.cpp ScopeHelper.cpp

resolver_test_SOURCES = ResolverTest.cpp
resolve_proguard_value_test_SOURCES = ResolveProguardAssumeValuesTest.cpp ScopeHelper.cpp

result_propagation_test_SOURCES = ResultPropagationTest.cpp
result_propagation_test_LDADD = $(COMMON_MOCK_TEST_LIBS)

side_effects_summary_test_SOURCES = object-sensitive-dce/SideEffectSummaryTest.cpp
side_effects_summary_test_LDADD = $(COMMON_MOCK_TEST_LIBS)

signed_constant_propagation_test_SOURCES = constant-propagation/SignedConstantPropagationTest.cpp
signed_constant_propagation_test_CPPFLAGS = $(COMMON_INCLUDES) $(COMMON_TEST_INCLUDES) -I$(top_srcdir)/sparta/test

source_blocks_test_SOURCES = SourceBlocksTest.cpp

split_huge_switch_test_SOURCES = SplitHugeSwitchTest.cpp

static_relo_v2_test_SOURCES = StaticReloV2Test.cpp

# stringbuilder_outline_test_SOURCES = StringBuilderOutlinerTest.cpp

strip_debug_info_test_SOURCES = StripDebugInfoTest.cpp

switch_dispatch_test_SOURCES = SwitchDispatchTest.cpp

# throw_propagation_test_SOURCES = ThrowPropagationTest.cpp
# throw_propagation_test_LDADD = $(COMMON_MOCK_TEST_LIBS)

timer_test_SOURCES = TimerTest.cpp

trace_multithreading_test_SOURCES = TraceMultithreadingTest.cpp
trace_multithreading_test_LDADD = $(COMMON_MOCK_TEST_LIBS)

true_virtuals_test_SOURCES = TrueVirtualsTest.cpp ScopeHelper.cpp
true_virtuals_test_LDADD = $(COMMON_MOCK_TEST_LIBS)

type_analysis_transform_test_SOURCES = type-analysis/TypeAnalysisTransformTest.cpp

type_inference_test_SOURCES = TypeInferenceTest.cpp
type_inference_test_LDADD = $(COMMON_MOCK_TEST_LIBS)

type_ref_updater_test_SOURCES = TypeRefUpdaterTest.cpp

type_reference_test_SOURCES = TypeReferenceTest.cpp

type_system_test_SOURCES = TypeSystemTest.cpp ScopeHelper.cpp
type_system_test_LDADD = $(COMMON_MOCK_TEST_LIBS)

type_util_test_SOURCES = TypeUtilTest.cpp

up_code_motion_test_SOURCES = UpCodeMotionTest.cpp
up_code_motion_test_LDADD = $(COMMON_MOCK_TEST_LIBS)

used_vars_test_SOURCES = object-sensitive-dce/UsedVarsTest.cpp
used_vars_test_LDADD = $(COMMON_MOCK_TEST_LIBS)

virt_scope_test_SOURCES = VirtScopeTest.cpp VirtScopeHelper.cpp ScopeHelper.cpp
virt_scope_test_LDADD = $(COMMON_MOCK_TEST_LIBS)

virtual_merging_test_SOURCES = VirtualMergingTest.cpp

walkers_test_SOURCES = WalkersTest.cpp
walkers_test_LDADD = $(COMMON_MOCK_TEST_LIBS)

work_queue_test_SOURCES = WorkQueueTest.cpp

xstorerefs_test_SOURCES = XStoreRefsTest.cpp


TESTS = \
    aliased_registers_test \
    analysis_usage_test \
    array_propagation_test \
    blaming_escape_test \
    boxed_boolean_propagation_test \
    branch_prefix_hoisting_test \
    cfg_inliner_test \
    cfg_mutation_test \
    check_breadcrumbs_test \
    check_cast_analysis_test \
    concurrent_containers_test \
    configurable_test \
    constructor_analysis_test \
    control_flow_test \
    copy_propagation_test \
    cse_test \
    creators_test \
    debug_info_test \
    debug_test \
    dedup_blocks_test \
    dex_class_test \
    dex_instruction_test \
    dex_loader_test \
    dex_mutate_test \
    dex_output_test \
    dex_store_test \
    dex_type_environment_test \
    dex_util_test \
    dominators_test \
    ev_arg_test \
    ev_write_test \
    evaluate_type_checks_test \
    exception_test \
    extract_native_test \
    final_inline_test \
    final_inline_v2_test \
    fp_ev_test \
    global_type_analysis_test \
    graph_util_test \
    hierarchy_util_test \
    interprocedural_constant_propagation_test \
    intraprocedural_constant_propagation_test \
    ir_assembler_test \
    ir_code_test \
    ir_instruction_test \
    ir_list_test \
    ir_typechecker_test \
    java_parser_util_test \
    literals_test \
    live_range_test \
    local_dce_test \
    local_pointers_test \
    loop_info_test \
    loosen_access_modifier_test \
    match_flow_test \
    match_test \
    method_inline_test \
    method_merger_test \
    monitor_count_test \
    mutf8_compare_test \
    leb_test \
    null_propagation_test \
    object_inliner_test \
    object_propagation_test \
    optimize_enums_test \
    outliner_type_analysis_test \
    partial_pass_test \
    peephole_test \
    proguard_lexer_test \
    proguard_map_test \
    proguard_parser_test \
    proguard_regex_test \
    pure_analysis_test \
    random_forest_test \
    reaching_definitions_test \
    reduce_array_literals_test \
    reduce_gotos_test \
    reflection_analysis_test \
    reg_alloc_test \
    registers_test \
    remove_apilevel_checks_test \
    remove_gotos_test \
    remove_null_check_string_arg_test \
    remove_recursive_locks_test \
    remove_redundant_check_casts_test \
    remove_uninstantiables_test \
    remove_unused_args_test \
    renamer_test \
    resolver_test \
    resolve_proguard_value_test \
    result_propagation_test \
    side_effects_summary_test \
    signed_constant_propagation_test \
    source_blocks_test \
    split_huge_switch_test \
    static_relo_v2_test \
    strip_debug_info_test \
    switch_dispatch_test \
    timer_test \
    trace_multithreading_test \
    true_virtuals_test \
    type_analysis_transform_test \
    type_inference_test \
    type_ref_updater_test \
    type_reference_test \
    type_system_test \
    type_util_test \
    up_code_motion_test \
    used_vars_test \
    virt_scope_test \
    virtual_merging_test \
    walkers_test \
    work_queue_test \
    xstorerefs_test
