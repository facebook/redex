AUTOMAKE_OPTIONS = foreign

SUBDIRS = . test

ACLOCAL_AMFLAGS = -I m4

# Disable some warnings so we can enable -Werror for the rest.
#
# Using -Wno- as that is explicitly specified as backwards-compatible, while
# I cannot find the same for the -Wno-error= form.

# attributes: unsupported CFI attribute warnings.
# comment: unhappy with use of "\" in comments, which we use for diagrams.
# unknown-warning-option: Clang doesn't follow GCC's "no-" backwards approach.
DISABLED_WARNINGS_HARD = \
  -Wno-attributes \
  -Wno-comment \
  -Wno-unknown-warning-option

# TODO: Minimize this list.
DISABLED_WARNINGS = \
  $(DISABLED_WARNINGS_HARD) \
  -Wno-class-memaccess \
  -Wno-deprecated-declarations \
  -Wno-format \
  -Wno-format-truncation \
  -Wno-format-zero-length \
  -Wno-invalid-offsetof \
  -Wno-maybe-uninitialized \
  -Wno-parentheses \
  -Wno-return-type \
  -Wno-shift-count-overflow \
  -Wno-sign-compare \
  -Wno-strict-aliasing \
  -Wno-stringop-overflow \
  -Wno-uninitialized \
  -Wno-unused-but-set-variable \
  -Wno-unused-function \
  -Wno-unused-private-field \
  -Wno-unused-result \
  -Wno-unused-variable

AM_CXXFLAGS = --std=gnu++17 -O3 -Wall -Werror $(DISABLED_WARNINGS) -g

include $(top_srcdir)/Makefile.inc

#
# Include paths
#
AM_CPPFLAGS = $(COMMON_INCLUDES)

noinst_LTLIBRARIES = libredex.la libopt.la

#
# build protobuf
#
if SET_PROTOBUF

proto_res_in = \
	proto/Resources.proto \
	proto/Configuration.proto

protores/%.pb.cc protores/%.pb.h: proto/%.proto | mdir ; $(PROTOC) --proto_path=$(dir $^) --cpp_out=$(top_srcdir)/protores $^
mdir: ; mkdir -p $(top_srcdir)/protores

proto_res_header_out = $(addprefix protores/, $(notdir ${proto_res_in:.proto=.pb.h}))
proto_res_body_out = $(addprefix protores/, $(notdir ${proto_res_in:.proto=.pb.cc}))
BUILT_SOURCES = $(proto_res_header_out)

endif

#
# libredex: the optimizer's guts
#

libredex_la_SOURCES = \
	checkers/DexLimitsChecker.cpp \
	checkers/NoInitClassInstructionsChecker.cpp \
	liblocator/locator.cpp \
	libredex/AggregateException.cpp \
	libredex/AnalysisUsage.cpp \
	libredex/AnnoUtils.cpp \
	libredex/ApiLevelChecker.cpp \
	libredex/ApkResources.cpp \
	libredex/AssetManager.cpp \
	libredex/BalancedPartitioning.cpp \
	libredex/BigBlocks.cpp \
	libredex/BundleResources.cpp \
	libredex/CFGMutation.cpp \
	libredex/CallGraph.cpp \
	libredex/ClassHierarchy.cpp \
	libredex/ClassUtil.cpp \
	libredex/ClassChecker.cpp \
	libredex/ConcurrentContainers.cpp \
	libredex/ConfigFiles.cpp \
	libredex/Configurable.cpp \
	libredex/ControlFlow.cpp \
	libredex/Creators.cpp \
	libredex/Debug.cpp \
	libredex/DexAccess.cpp \
	libredex/DexAssessments.cpp \
	libredex/DexAnnotation.cpp \
	libredex/DexAsm.cpp \
	libredex/DexCallSite.cpp \
	libredex/DexClass.cpp \
	libredex/DexDebugInstruction.cpp \
	libredex/DexHasher.cpp \
	libredex/DexIdx.cpp \
	libredex/DexInstruction.cpp \
	libredex/DexLoader.cpp \
	libredex/DexMemberRefs.cpp \
	libredex/DexMethodHandle.cpp \
	libredex/DexOpcode.cpp \
	libredex/DexOutput.cpp \
	libredex/DexPosition.cpp \
	libredex/DexStats.cpp \
	libredex/DexStore.cpp \
	libredex/DexStoreUtil.cpp \
	libredex/DexTypeEnvironment.cpp \
	libredex/DexUtil.cpp \
	libredex/DexStoreUtil.cpp \
	libredex/DuplicateClasses.cpp \
	libredex/EditableCfgAdapter.cpp \
	libredex/FbjniMarker.cpp \
	libredex/FrameworkApi.cpp \
	libredex/FrequentlyUsedPointersCache.cpp \
	libredex/GlobalConfig.cpp \
	libredex/GraphVisualizer.cpp \
	libredex/HierarchyUtil.cpp \
	libredex/InitCollisionFinder.cpp \
	libredex/InlinerConfig.cpp \
	libredex/InstructionLowering.cpp \
	libredex/InteractiveDebugging.cpp \
	libredex/IODIMetadata.cpp \
	libredex/IRAssembler.cpp \
	libredex/IRCode.cpp \
	libredex/IRInstruction.cpp \
	libredex/IRList.cpp \
	libredex/IRMetaIO.cpp \
	libredex/IROpcode.cpp \
	libredex/IRTypeChecker.cpp \
	libredex/IRTypeChecker.cpp \
	libredex/JarLoader.cpp \
	libredex/JavaParserUtil.cpp \
	libredex/JsonWrapper.cpp \
	libredex/KeepReason.cpp \
	libredex/Match.cpp \
	libredex/MatchFlow.cpp \
	libredex/MatchFlowDetail.cpp \
	libredex/MethodDevirtualizer.cpp \
	libredex/MethodOverrideGraph.cpp \
	libredex/MethodProfiles.cpp \
	libredex/MethodSimilarityCompressionConsciousOrderer.cpp \
	libredex/MethodSimilarityGreedyOrderer.cpp \
	libredex/MethodUtil.cpp \
	libredex/MonitorCount.cpp \
	libredex/Mutators.cpp \
	libredex/Native.cpp \
	libredex/NativeNames.cpp \
	libredex/NoOptimizationsMatcher.cpp \
	libredex/NullnessDomain.cpp \
	libredex/OptData.cpp \
	libredex/Pass.cpp \
	libredex/PassManager.cpp \
	libredex/PassRegistry.cpp \
	libredex/PluginRegistry.cpp \
	libredex/PointsToSemantics.cpp \
	libredex/PointsToSemanticsUtils.cpp \
	libredex/PostLowering.cpp \
	libredex/PrintSeeds.cpp \
	libredex/ProguardConfiguration.cpp \
	libredex/ProguardLexer.cpp \
	libredex/ProguardLineRange.cpp \
	libredex/ProguardMap.cpp \
	libredex/ProguardMatcher.cpp \
	libredex/ProguardParser.cpp \
	libredex/ProguardPrintConfiguration.cpp \
	libredex/ProguardRegex.cpp \
	libredex/ProguardReporting.cpp \
	libredex/Purity.cpp \
	libredex/Reachability.cpp \
	libredex/ReachableClasses.cpp \
	libredex/RedexMappedFile.cpp \
	libredex/ReadMaybeMapped.cpp \
	libredex/RedexContext.cpp \
	libredex/RedexPropertiesManager.cpp \
	libredex/RedexPropertyChecker.cpp \
	libredex/RedexPropertyCheckerRegistry.cpp \
	libredex/RedexException.cpp \
	libredex/RedexOptions.cpp \
	libredex/RedexResources.cpp \
	libredex/ReflectionAnalysis.cpp \
	libredex/RefChecker.cpp \
	libredex/Resolver.cpp \
	libredex/ScopedMetrics.cpp \
	libredex/Show.cpp \
	libredex/SourceBlockConsistencyCheck.cpp \
	libredex/SourceBlocks.cpp \
	libredex/StringTreeSet.cpp \
	libredex/Timer.cpp \
	libredex/Trace.cpp \
	libredex/Transform.cpp \
	libredex/TypeInference.cpp \
	libredex/TypeSystem.cpp \
	libredex/TypeUtil.cpp \
	libredex/UnknownVirtuals.cpp \
	libredex/Vinfo.cpp \
	libredex/VirtualScope.cpp \
	libredex/Warning.cpp \
	libredex/WorkQueue.cpp \
	libresource/LocaleValue.cpp \
	libresource/LocaleData.cpp \
	libresource/ResourceTypes.cpp \
	libresource/Serialize.cpp \
	libresource/SharedBuffer.cpp \
	libresource/String16.cpp \
	libresource/String8.cpp \
	libresource/TypeWrappers.cpp \
	libresource/Unicode.cpp \
	libresource/VectorImpl.cpp \
	libresource/Visitor.cpp \
	service/api-levels/ApiLevelsUtils.cpp \
	service/class-merging/ApproximateShapeMerging.cpp \
	service/class-merging/ClassAssemblingUtils.cpp \
	service/class-merging/ClassMerging.cpp \
	service/class-merging/ConfigUtils.cpp \
	service/class-merging/MergeabilityCheck.cpp \
	service/class-merging/MergerType.cpp \
	service/class-merging/MergingStrategies.cpp \
	service/class-merging/ModelMerger.cpp \
	service/class-merging/ModelMethodMerger.cpp \
	service/class-merging/Model.cpp \
	service/class-merging/TypeTagUtils.cpp \
	service/class-splitting/ClassSplitting.cpp \
	service/cross-dex-ref-minimizer/CrossDexRefMinimizer.cpp \
	service/constant-propagation/ConstantEnvironment.cpp \
	service/constant-propagation/ConstantPropagationAnalysis.cpp \
	service/constant-propagation/ConstantPropagation.cpp \
	service/constant-propagation/ConstantPropagationTransform.cpp \
	service/constant-propagation/ConstantPropagationWholeProgramState.cpp \
	service/constant-propagation/ConstructorParams.cpp \
	service/constant-propagation/DefinitelyAssignedIFields.cpp \
	service/constant-propagation/IPConstantPropagationAnalysis.cpp \
	service/constant-propagation/ObjectDomain.cpp \
	service/constant-propagation/SignDomain.cpp \
	service/copy-propagation/AliasedRegisters.cpp \
	service/copy-propagation/CanonicalizeLocks.cpp \
	service/copy-propagation/CopyPropagation.cpp \
	service/cross-dex-ref-minimizer/CrossDexRefMinimizer.cpp \
	service/cse/CommonSubexpressionElimination.cpp \
	service/dataflow/LiveRange.cpp \
	service/dataflow/ConstantUses.cpp \
	service/dedup-blocks/DedupBlocks.cpp \
	service/dedup-blocks/DedupBlockValueNumbering.cpp \
	service/escape-analysis/BlamingAnalysis.cpp \
	service/escape-analysis/LocalPointersAnalysis.cpp \
	service/field-ops/FieldOpTracker.cpp \
	service/init-classes/InitClassPruner.cpp \
	service/init-classes/InitClassesWithSideEffects.cpp \
	service/kotlin-instance-rewrite/KotlinInstanceRewriter.cpp \
	service/local-dce/LocalDce.cpp \
	service/loop-info/LoopInfo.cpp \
	service/method-dedup/ConstantLifting.cpp \
	service/method-dedup/ConstantValue.cpp \
	service/method-dedup/MethodDedup.cpp \
	service/method-dedup/NormalizeConstructor.cpp \
	service/method-dedup/TypeTags.cpp \
	service/method-inliner/CallSiteSummaries.cpp \
	service/method-inliner/CFGInliner.cpp \
	service/method-inliner/ConstructorAnalysis.cpp \
	service/method-inliner/Deleter.cpp \
	service/method-inliner/Inliner.cpp \
	service/method-inliner/LegacyInliner.cpp \
	service/method-inliner/MethodInliner.cpp \
	service/method-inliner/ObjectInlinePlugin.cpp \
	service/method-inliner/RecursionPruner.cpp \
	service/method-merger/MethodMerger.cpp \
	service/method-outliner/OutliningProfileGuidanceImpl.cpp \
	service/reduce-boolean-branches/ReduceBooleanBranches.cpp \
	service/reference-update/MethodReference.cpp \
	service/reference-update/TypeReference.cpp \
	service/regalloc/GraphColoring.cpp \
	service/regalloc/Interference.cpp \
	service/regalloc/RegisterAllocation.cpp \
	service/regalloc/RegisterType.cpp \
	service/regalloc/Split.cpp \
	service/regalloc/VirtualRegistersFile.cpp \
	service/regalloc-fast/LinearScan.cpp \
	service/regalloc-fast/LiveInterval.cpp \
	service/resources/StaticIds.cpp \
	service/shrinker/Shrinker.cpp \
	service/switch-dispatch/SwitchDispatch.cpp \
	service/switch-partitioning/SwitchEquivFinder.cpp \
	service/switch-partitioning/SwitchMethodPartitioning.cpp \
	service/type-analysis/GlobalTypeAnalyzer.cpp \
	service/type-analysis/LocalTypeAnalyzer.cpp \
	service/type-analysis/WholeProgramState.cpp \
	service/type-analysis/TypeAnalysisTransform.cpp \
	service/type-analysis/TypeAnalysisRuntimeAssert.cpp \
	service/type-analysis/ResolveMethodRefs.cpp \
	service/type-string-rewriter/TypeStringRewriter.cpp \
	shared/DexDefs.cpp \
	shared/DexEncoding.cpp \
	shared/file-utils.cpp \
	util/CommandProfiling.cpp \
	util/JemallocUtil.cpp \
	util/Sha1.cpp

libredex_la_LIBADD = \
	$(BOOST_FILESYSTEM_LIB) \
	$(BOOST_SYSTEM_LIB) \
	$(BOOST_REGEX_LIB) \
	$(BOOST_IOSTREAMS_LIB) \
	$(BOOST_THREAD_LIB) \
	-lpthread

if SET_PROTOBUF
libredex_la_SOURCES += \
	protores/Resources.pb.cc \
	protores/Configuration.pb.cc

libredex_la_LIBADD += \
	$(LIBPROTOBUF_LIBS)
endif

#
# libopt: the optimization passes
#

# Autoconf + libtool does not handle whole-archive well, and it is not
# supported on MacOS ("-all_load"). But we would need this for pass
# registration. Instead, share sources and create libopt for the tests.

libopt_la_SOURCES = \
	checkers/HasSourceBlocksChecker.cpp \
	checkers/NoInitClassInstructionsChecker.cpp \
	checkers/NoSpuriousGetClassCallsChecker.cpp \
	checkers/RenameClassChecker.cpp \
	analysis/max-depth/MaxDepthAnalysis.cpp \
	analysis/ip-reflection-analysis/IPReflectionAnalysis.cpp \
	opt/access-marking/AccessMarking.cpp \
	opt/annokill/AnnoKill.cpp \
	opt/analyze-pure-method/PureMethods.cpp \
	opt/app_module_usage/AppModuleUsage.cpp \
	opt/art-profile-writer/ArtProfileWriterPass.cpp \
	opt/basic-block/BasicBlockProfile.cpp \
	opt/builder_pattern/BuilderAnalysis.cpp \
	opt/builder_pattern/BuilderTransform.cpp \
	opt/builder_pattern/RemoveBuilderPattern.cpp \
	opt/branch-prefix-hoisting/BranchPrefixHoisting.cpp \
	opt/check_breadcrumbs/CheckBreadcrumbs.cpp \
	opt/check-recursion/CheckRecursion.cpp \
	opt/class-merging/AnonymousClassMergingPass.cpp \
	opt/class-merging/ClassMergingPass.cpp \
	opt/class-merging/IntraDexClassMergingPass.cpp \
	opt/class-merging/ModelSpecGenerator.cpp \
	opt/class-splitting/ClassSplittingPass.cpp \
	opt/const-class-branches/TransformConstClassBranches.cpp \
	opt/constant-propagation/ConstantPropagationPass.cpp \
	opt/constant-propagation/ConstantPropagationRuntimeAssert.cpp \
	opt/constant-propagation/IPConstantPropagation.cpp \
	opt/copy-propagation/CopyPropagationPass.cpp \
	opt/cse/CommonSubexpressionEliminationPass.cpp \
	opt/dedup_blocks/DedupBlocksPass.cpp \
	opt/dedup_resources/DedupResources.cpp \
	opt/dedup_resources/murmur_hash.cpp \
	opt/dedup-strings/DedupStrings.cpp \
	opt/delinit/DelInit.cpp \
	opt/delsuper/DelSuper.cpp \
	opt/evaluate_type_checks/EvaluateTypeChecks.cpp \
	opt/final_inline/FinalInline.cpp \
	opt/final_inline/FinalInlineV2.cpp \
	opt/init-classes/InitClassLoweringPass.cpp \
	opt/insert-source-blocks/InsertSourceBlocks.cpp \
	opt/instrument/BlockInstrument.cpp \
	opt/instrument/Instrument.cpp \
	opt/interdex/DexStructure.cpp \
	opt/interdex/InterDex.cpp \
	opt/interdex/InterDexPass.cpp \
	opt/interdex/InterDexReshufflePass.cpp \
	opt/interdex/SortRemainingClassesPass.cpp \
	opt/kotlin-lambda/RewriteKotlinSingletonInstance.cpp \
	opt/kotlin-lambda/KotlinObjectInliner.cpp \
	opt/layout-reachability/LayoutReachabilityPass.cpp \
	opt/local-dce/LocalDcePass.cpp \
	opt/merge_interface/MergeInterface.cpp \
	opt/nullcheck_conversion/IntrinsifyNullChecksPass.cpp \
	opt/nullcheck_conversion/MaterializeNullChecksPass.cpp \
	opt/obfuscate/Obfuscate.cpp \
	opt/obfuscate/ObfuscateUtils.cpp \
	opt/obfuscate/VirtualRenamer.cpp \
	opt/obfuscate_resources/ObfuscateResourcesPass.cpp \
	opt/object-sensitive-dce/ObjectSensitiveDcePass.cpp \
	opt/object-sensitive-dce/SideEffectSummary.cpp \
	opt/object-sensitive-dce/UsedVarsAnalysis.cpp \
	opt/optimize_enums/EnumClinitAnalysis.cpp \
	opt/optimize_enums/EnumConfig.cpp \
	opt/optimize_enums/EnumAnalyzeGeneratedMethods.cpp \
	opt/optimize_enums/EnumTransformer.cpp \
	opt/optimize_enums/EnumUpcastAnalysis.cpp \
	opt/optimize_enums/OptimizeEnumsAnalysis.cpp \
	opt/optimize_enums/OptimizeEnums.cpp \
	opt/optimize_enums/OptimizeEnumsUnmap.cpp \
	opt/original_name/OriginalNamePass.cpp \
	opt/partial-application/PartialApplication.cpp \
	opt/peephole/Peephole.cpp \
	opt/peephole/RedundantCheckCastRemover.cpp \
	opt/print-kotlin-stats/PrintKotlinStats.cpp \
	opt/print-members/PrintMembers.cpp \
  opt/reachable-natives/ReachableNatives.cpp \
	opt/rebindrefs/ReBindRefs.cpp \
	opt/regalloc/RegAlloc.cpp \
	opt/regalloc-fast/FastRegAlloc.cpp \
	opt/remove-apilevel-checks/RemoveApiLevelChecks.cpp \
	opt/remove-builders/RemoveBuilders.cpp \
	opt/remove-builders/RemoveBuildersHelper.cpp \
	opt/remove-interfaces/RemoveInterfacePass.cpp \
	opt/remove-recursive-locks/RemoveRecursiveLocks.cpp \
	opt/remove_redundant_check_casts/CheckCastAnalysis.cpp \
	opt/remove_redundant_check_casts/CheckCastTransform.cpp \
	opt/remove_redundant_check_casts/RemoveRedundantCheckCasts.cpp \
	opt/remove-uninstantiables/RemoveUninstantiablesPass.cpp \
	opt/remove-unreachable/RemoveUnreachable.cpp \
	opt/remove-unreachable/TypeAnalysisAwareRemoveUnreachable.cpp \
	opt/remove-nullcheck-string-arg/RemoveNullcheckStringArg.cpp \
	opt/remove-unused-fields/RemoveUnusedFields.cpp \
	opt/remove_empty_classes/RemoveEmptyClasses.cpp \
	opt/remove-unused-args/RemoveUnusedArgs.cpp \
	opt/renameclasses/RenameClassesV2.cpp \
	opt/reorder-interfaces-decl/ReorderInterfacesDecl.cpp \
	opt/reduce-array-literals/ReduceArrayLiterals.cpp \
	opt/reduce-boolean-branches/ReduceBooleanBranchesPass.cpp \
	opt/reduce-gotos/ReduceGotos.cpp \
	opt/resolve-refs/ResolveRefsPass.cpp \
	opt/resolve-refs/ExternalRefsManglingPass.cpp \
	opt/resolve-refs/SpecializeRtype.cpp \
	opt/result-propagation/ResultPropagation.cpp \
	opt/resolve-proguard-values/ResolveProguardAssumeValues.cpp \
	opt/shorten-srcstrings/Shorten.cpp \
	opt/shrinker/ShrinkerPass.cpp \
	opt/make-public/MakePublicPass.cpp \
	opt/methodinline/IntraDexInlinePass.cpp \
	opt/methodinline/LocalMethodInlinePass.cpp \
	opt/methodinline/MethodInlinePass.cpp \
	opt/methodinline/PerfMethodInlinePass.cpp \
	opt/methodinline/BridgeSynthInlinePass.cpp \
	opt/outliner/OutlinerTypeAnalysis.cpp \
	opt/outliner/InstructionSequenceOutliner.cpp \
	opt/singleimpl/SingleImpl.cpp \
	opt/singleimpl/SingleImplAnalyze.cpp \
	opt/singleimpl/SingleImplOptimize.cpp \
	opt/singleimpl/SingleImplStats.cpp \
	opt/split_huge_switches/SplitHugeSwitchPass.cpp \
	opt/split_resource_tables/SplitResourceTables.cpp \
	opt/object-escape-analysis/ExpandableMethodParams.cpp \
	opt/object-escape-analysis/ObjectEscapeAnalysis.cpp \
	opt/staticrelo/StaticReloV2.cpp \
	opt/string_concatenator/StringConcatenator.cpp \
	opt/stringbuilder-outliner/StringBuilderOutliner.cpp \
	opt/strip-debug-info/StripDebugInfo.cpp \
	opt/test_cfg/TestCFG.cpp \
	opt/track_resources/TrackResources.cpp \
	opt/optimize_resources/OptimizeResources.cpp \
	opt/throw-propagation/ThrowPropagationPass.cpp \
	opt/type-analysis/CallGraphFileGenerationPass.cpp \
	opt/type-analysis/GlobalTypeAnalysisPass.cpp \
	opt/type-analysis/TypeAnalysisCallGraphGenerationPass.cpp \
	opt/unreferenced_interfaces/UnreferencedInterfaces.cpp \
	opt/unmark_proguard_keep/UnmarkProguardKeep.cpp \
	opt/up-code-motion/UpCodeMotion.cpp \
	opt/uses-names/UsesNames.cpp \
	opt/verifier/Verifier.cpp \
	opt/vertical_merging/VerticalMerging.cpp \
	opt/virtual_merging/DedupVirtualMethods.cpp \
	opt/virtual_merging/VirtualMerging.cpp \
	opt/virtual_scope/MethodDevirtualizationPass.cpp

libopt_la_LIBADD = \
    libredex.la \
	$(BOOST_FILESYSTEM_LIB) \
	$(BOOST_SYSTEM_LIB) \
	$(BOOST_REGEX_LIB) \
	$(BOOST_IOSTREAMS_LIB) \
	$(BOOST_THREAD_LIB) \
	-lpthread

#
# redex-all: the main executable
#
bin_PROGRAMS = redexdump
noinst_PROGRAMS = redex-all

redex_all_SOURCES = \
    $(libopt_la_SOURCES) \
	tools/common/ToolsCommon.cpp \
	tools/redex-all/main.cpp

# Workaround for not using libopt.
redex_all_CPPFLAGS = $(AM_CPPFLAGS)

redex_all_LDADD = \
	libredex.la \
	$(BOOST_FILESYSTEM_LIB) \
	$(BOOST_SYSTEM_LIB) \
	$(BOOST_REGEX_LIB) \
	$(BOOST_IOSTREAMS_LIB) \
	$(BOOST_PROGRAM_OPTIONS_LIB) \
	$(BOOST_THREAD_LIB) \
	-lpthread \
	-ldl

if SET_PROTOBUF
redex_all_LDADD += \
	$(LIBPROTOBUF_LIBS)
endif

redex_all_LDFLAGS = \
	-rdynamic # function names in stack traces

redexdump_SOURCES = \
	tools/redexdump/DumpTables.cpp \
	tools/redexdump/PrintUtil.cpp \
	tools/redexdump/RedexDump.cpp \
	tools/common/DexCommon.cpp \
	tools/common/Formatters.cpp

redexdump_LDADD = \
	libredex.la \
	$(BOOST_FILESYSTEM_LIB) \
	$(BOOST_SYSTEM_LIB) \
	$(BOOST_REGEX_LIB) \
	$(BOOST_THREAD_LIB) \
	-lpthread \
	-ldl

#
# redex: Python driver script
#
bin_SCRIPTS = redex apkutil
CLEANFILES = redex

# Embedded API level files.
GEN_API_LEVELS_SCRIPT = gen_packed_apilevels.py
GENERATED_API_LEVELS_MODULE = generated_apilevels.py
$(GENERATED_API_LEVELS_MODULE): $(GEN_API_LEVELS_SCRIPT) service/api-levels/framework_classes_api_*.txt
	python3 $(GEN_API_LEVELS_SCRIPT) -o $@ service/api-levels/framework_classes_api_*.txt

PYTHON_SRCS := redex.py \
	pyredex/__init__.py \
	pyredex/logger.py \
	pyredex/unpacker.py \
	pyredex/utils.py \
	$(GENERATED_API_LEVELS_MODULE)

redex: redex-all $(PYTHON_SRCS)
	$(srcdir)/bundle-redex.sh
