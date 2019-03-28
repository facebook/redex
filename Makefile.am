AUTOMAKE_OPTIONS = foreign

SUBDIRS = . test

ACLOCAL_AMFLAGS = -I m4
AM_CXXFLAGS = --std=gnu++14 -O3 -Wall -g

#
# Include paths
#
AM_CPPFLAGS = \
	-I$(top_srcdir)/liblocator \
	-I$(top_srcdir)/libredex \
	-I$(top_srcdir)/libresource \
	-I$(top_srcdir)/libresource/android \
	-I$(top_srcdir)/libresource/androidfw \
	-I$(top_srcdir)/libresource/cutils \
	-I$(top_srcdir)/libresource/system \
	-I$(top_srcdir)/libresource/utils \
	-I$(top_srcdir)/opt \
	-I$(top_srcdir)/opt/access-marking \
	-I$(top_srcdir)/opt/add_redex_txt_to_apk \
	-I$(top_srcdir)/opt/analysis_ref_graph \
	-I$(top_srcdir)/opt/annoclasskill \
	-I$(top_srcdir)/opt/annokill \
	-I$(top_srcdir)/opt/basic-block \
	-I$(top_srcdir)/opt/branch-prefix-hoisting \
	-I$(top_srcdir)/opt/bridge \
	-I$(top_srcdir)/opt/check_breadcrumbs \
	-I$(top_srcdir)/opt/class-splitting \
	-I$(top_srcdir)/opt/constant-propagation \
	-I$(top_srcdir)/opt/dedup_blocks \
	-I$(top_srcdir)/opt/dedup-strings \
	-I$(top_srcdir)/opt/delinit \
	-I$(top_srcdir)/opt/delsuper \
	-I$(top_srcdir)/opt/final_inline \
	-I$(top_srcdir)/opt/inlineinit \
	-I$(top_srcdir)/opt/instrument \
	-I$(top_srcdir)/opt/interdex \
	-I$(top_srcdir)/opt/local-dce \
	-I$(top_srcdir)/opt/layout-reachability \
	-I$(top_srcdir)/opt/merge_interface \
	-I$(top_srcdir)/opt/obfuscate \
	-I$(top_srcdir)/opt/object-sensitive-dce \
	-I$(top_srcdir)/opt/optimize_enums \
	-I$(top_srcdir)/opt/original_name \
	-I$(top_srcdir)/opt/outliner \
	-I$(top_srcdir)/opt/peephole \
	-I$(top_srcdir)/opt/print-members \
	-I$(top_srcdir)/opt/rebindrefs \
	-I$(top_srcdir)/opt/redundant_move_elimination \
	-I$(top_srcdir)/opt/regalloc \
	-I$(top_srcdir)/opt/remove-builders \
	-I$(top_srcdir)/opt/remove-interfaces \
	-I$(top_srcdir)/opt/remove_redundant_check_casts \
	-I$(top_srcdir)/opt/remove-unreachable \
	-I$(top_srcdir)/opt/remove_empty_classes \
	-I$(top_srcdir)/opt/remove_gotos \
	-I$(top_srcdir)/opt/remove_unused_args \
	-I$(top_srcdir)/opt/renameclasses \
	-I$(top_srcdir)/opt/reorder-interfaces \
	-I$(top_srcdir)/opt/reduce-gotos \
	-I$(top_srcdir)/opt/result-propagation \
	-I$(top_srcdir)/opt/shorten-srcstrings \
	-I$(top_srcdir)/opt/methodinline \
	-I$(top_srcdir)/opt/singleimpl \
	-I$(top_srcdir)/opt/static-sink \
	-I$(top_srcdir)/opt/staticrelo \
	-I$(top_srcdir)/opt/string_concatenator \
	-I$(top_srcdir)/opt/string_simplification \
	-I$(top_srcdir)/opt/strip-debug-info \
	-I$(top_srcdir)/opt/synth \
	-I$(top_srcdir)/opt/test_cfg \
	-I$(top_srcdir)/opt/track_resources \
	-I$(top_srcdir)/opt/type-erasure \
	-I$(top_srcdir)/opt/unreferenced_interfaces \
	-I$(top_srcdir)/opt/unmark_proguard_keep \
	-I$(top_srcdir)/opt/unterface \
	-I$(top_srcdir)/opt/up-code-motion \
	-I$(top_srcdir)/opt/verifier \
	-I$(top_srcdir)/opt/virtual_scope \
	-I$(top_srcdir)/service/constant-propagation \
	-I$(top_srcdir)/service/dataflow \
	-I$(top_srcdir)/service/escape-analysis \
	-I$(top_srcdir)/service/method-dedup \
	-I$(top_srcdir)/service/method-merger \
	-I$(top_srcdir)/service/reference-update \
	-I$(top_srcdir)/service/switch-dispatch \
	-I$(top_srcdir)/service/switch-partitioning \
	-I$(top_srcdir)/shared \
	-I$(top_srcdir)/sparta/include \
	-I$(top_srcdir)/tools/common \
	-I$(top_srcdir)/tools/redexdump \
	-I$(top_srcdir)/util \
	-I/usr/include/jsoncpp

#
# libredex: the optimizer's guts
#
noinst_LTLIBRARIES = libredex.la

libredex_la_SOURCES = \
	liblocator/locator.cpp \
	libredex/AnnoUtils.cpp \
	libredex/ApiLevelChecker.cpp \
	libredex/ApkManager.cpp \
	libredex/CallGraph.cpp \
	libredex/CFGInliner.cpp \
	libredex/ClassHierarchy.cpp \
	libredex/ConfigFiles.cpp \
	libredex/Creators.cpp \
	libredex/ControlFlow.cpp \
	libredex/Debug.cpp \
	libredex/DexAnnotation.cpp \
	libredex/DexClass.cpp \
	libredex/DexDebugInstruction.cpp \
	libredex/DexEncoding.cpp \
	libredex/DexIdx.cpp \
	libredex/DexLoader.cpp \
	libredex/DexInstruction.cpp \
	libredex/DexMemberRefs.cpp \
	libredex/DexOpcode.cpp \
	libredex/DexOutput.cpp \
	libredex/DexPosition.cpp \
	libredex/DexStore.cpp \
	libredex/DexUtil.cpp \
	libredex/DexStoreUtil.cpp \
	libredex/EditableCfgAdapter.cpp \
	libredex/FieldOpTracker.cpp \
	libredex/ImmutableSubcomponentAnalyzer.cpp \
	libredex/Inliner.cpp \
	libredex/InstructionLowering.cpp \
	libredex/IODIMetadata.cpp \
	libredex/IRAssembler.cpp \
	libredex/IRCode.cpp \
	libredex/IRInstruction.cpp \
	libredex/IRList.cpp \
	libredex/IRMetaIO.cpp \
	libredex/IROpcode.cpp \
	libredex/IRTypeChecker.cpp \
	libredex/JarLoader.cpp \
	libredex/KeepReason.cpp \
	libredex/Match.cpp \
	libredex/MethodDevirtualizer.cpp \
	libredex/MethodOverrideGraph.cpp \
	libredex/Mutators.cpp \
	libredex/NoOptimizationsMatcher.cpp \
	libredex/OptData.cpp \
	libredex/PassManager.cpp \
	libredex/PassRegistry.cpp \
	libredex/PluginRegistry.cpp \
	libredex/PointsToSemantics.cpp \
	libredex/PointsToSemanticsUtils.cpp \
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
	libredex/Reachability.cpp \
	libredex/ReachableClasses.cpp \
	libredex/RedexContext.cpp \
	libredex/RedexResources.cpp \
	libredex/Resolver.cpp \
	libredex/Show.cpp \
	libredex/ReflectionAnalysis.cpp \
	libredex/Timer.cpp \
	libredex/Trace.cpp \
	libredex/Transform.cpp \
	libredex/IRTypeChecker.cpp \
	libredex/TypeSystem.cpp \
	libredex/Vinfo.cpp \
	libredex/VirtualScope.cpp \
	libredex/Warning.cpp \
	libresource/FileMap.cpp \
	libresource/ResourceTypes.cpp \
	libresource/Serialize.cpp \
	libresource/SharedBuffer.cpp \
	libresource/Static.cpp \
	libresource/String16.cpp \
	libresource/String8.cpp \
	libresource/TypeWrappers.cpp \
	libresource/Unicode.cpp \
	libresource/VectorImpl.cpp \
	shared/DexDefs.cpp \
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

#
# redex-all: the main executable
#
bin_PROGRAMS = redexdump
noinst_PROGRAMS = redex-all

redex_all_SOURCES = \
	libredex/DexAsm.cpp \
	opt/access-marking/AccessMarking.cpp \
	opt/add_redex_txt_to_apk/AddRedexTxtToApk.cpp \
	opt/analysis_ref_graph/ReferenceGraphCreator.cpp \
	opt/annokill/AnnoKill.cpp \
	opt/basic-block/BasicBlockProfile.cpp \
	opt/branch-prefix-hoisting/BranchPrefixHoisting.cpp \
	opt/bridge/Bridge.cpp \
	opt/check_breadcrumbs/CheckBreadcrumbs.cpp \
	opt/class-splitting/ClassSplitting.cpp \
	opt/constant-propagation/ConstantPropagation.cpp \
	opt/constant-propagation/ConstantPropagationRuntimeAssert.cpp \
	opt/constant-propagation/ConstantPropagationTransform.cpp \
	opt/constant-propagation/IPConstantPropagation.cpp \
	opt/copy-propagation/AliasedRegisters.cpp \
	opt/copy-propagation/CopyPropagationPass.cpp \
	opt/dedup_blocks/DedupBlocksPass.cpp \
	opt/dedup-strings/DedupStrings.cpp \
	opt/delinit/DelInit.cpp \
	opt/delsuper/DelSuper.cpp \
	opt/final_inline/FinalInline.cpp \
	opt/final_inline/FinalInlineV2.cpp \
	opt/inlineinit/InlineInit.cpp \
	opt/instrument/Instrument.cpp \
	opt/interdex/CrossDexRefMinimizer.cpp \
	opt/interdex/DexStructure.cpp \
	opt/interdex/InterDex.cpp \
	opt/interdex/InterDexPass.cpp \
	opt/layout-reachability/LayoutReachabilityPass.cpp \
	opt/local-dce/LocalDce.cpp \
	opt/merge_interface/MergeInterface.cpp \
	opt/obfuscate/Obfuscate.cpp \
	opt/obfuscate/ObfuscateUtils.cpp \
	opt/obfuscate/VirtualRenamer.cpp \
	opt/object-sensitive-dce/ObjectSensitiveDcePass.cpp \
	opt/object-sensitive-dce/SideEffectSummary.cpp \
	opt/object-sensitive-dce/UsedVarsAnalysis.cpp \
	opt/optimize_enums/EnumClinitAnalysis.cpp \
	opt/optimize_enums/EnumInSwitch.cpp \
	opt/optimize_enums/EnumTransformer.cpp \
	opt/optimize_enums/EnumUpcastAnalysis.cpp \
	opt/optimize_enums/OptimizeEnumsAnalysis.cpp \
	opt/optimize_enums/OptimizeEnums.cpp \
	opt/optimize_enums/OptimizeEnumsGeneratedAnalysis.cpp \
	opt/original_name/OriginalNamePass.cpp \
	opt/outliner/Outliner.cpp \
	opt/peephole/Peephole.cpp \
	opt/peephole/RedundantCheckCastRemover.cpp \
	opt/print-members/PrintMembers.cpp \
	opt/rebindrefs/ReBindRefs.cpp \
	opt/regalloc/GraphColoring.cpp \
	opt/regalloc/Interference.cpp \
	opt/regalloc/RegAlloc.cpp \
	opt/regalloc/RegisterType.cpp \
	opt/regalloc/Split.cpp \
	opt/regalloc/VirtualRegistersFile.cpp \
	opt/remove-builders/RemoveBuilders.cpp \
	opt/remove-builders/RemoveBuildersHelper.cpp \
	opt/remove-interfaces/RemoveInterfacePass.cpp \
	opt/remove_redundant_check_casts/CheckCastAnalysis.cpp \
	opt/remove_redundant_check_casts/RemoveRedundantCheckCasts.cpp \
	opt/remove-unreachable/RemoveUnreachable.cpp \
	opt/remove-unread-fields/RemoveUnreadFields.cpp \
	opt/remove_empty_classes/RemoveEmptyClasses.cpp \
	opt/remove_gotos/RemoveGotos.cpp \
	opt/remove_unused_args/RemoveUnusedArgs.cpp \
	opt/renameclasses/RenameClasses.cpp \
	opt/renameclasses/RenameClassesV2.cpp \
	opt/reorder-interfaces/ReorderInterfaces.cpp \
	opt/reduce-gotos/ReduceGotos.cpp \
	opt/result-propagation/ResultPropagation.cpp \
	opt/shorten-srcstrings/Shorten.cpp \
	opt/methodinline/Deleter.cpp \
	opt/methodinline/MethodInline.cpp \
	opt/singleimpl/SingleImpl.cpp \
	opt/singleimpl/SingleImplAnalyze.cpp \
	opt/singleimpl/SingleImplOptimize.cpp \
	opt/singleimpl/SingleImplStats.cpp \
	opt/singleimpl/SingleImplUtil.cpp \
	opt/simplify_cfg/SimplifyCFG.cpp \
	opt/static-sink/StaticSink.cpp \
	opt/staticrelo/StaticRelo.cpp \
	opt/staticrelo/StaticReloV2.cpp \
	opt/string_concatenator/StringConcatenator.cpp \
	opt/string_simplification/StringIterator.cpp \
	opt/string_simplification/StringSimplification.cpp \
	opt/strip-debug-info/StripDebugInfo.cpp \
	opt/synth/Synth.cpp \
	opt/test_cfg/TestCFG.cpp \
	opt/track_resources/TrackResources.cpp \
	opt/type-erasure/ApproximateShapeMerging.cpp \
	opt/type-erasure/ClassAssemblingUtils.cpp \
	opt/type-erasure/MergerType.cpp \
	opt/type-erasure/ModelMethodMerger.cpp \
	opt/type-erasure/Model.cpp \
	opt/type-erasure/ModelMerger.cpp \
	opt/type-erasure/TypeErasureInterDexPlugin.cpp \
	opt/type-erasure/TypeErasurePass.cpp \
	opt/type-erasure/TypeTagUtils.cpp \
	opt/unreferenced_interfaces/UnreferencedInterfaces.cpp \
	opt/unmark_proguard_keep/UnmarkProguardKeep.cpp \
	opt/up-code-motion/UpCodeMotion.cpp \
	opt/verifier/Verifier.cpp \
	opt/virtual_scope/MethodDevirtualizationPass.cpp \
	service/constant-propagation/ConstantEnvironment.cpp \
	service/constant-propagation/ConstantPropagationAnalysis.cpp \
	service/constant-propagation/ConstantPropagationWholeProgramState.cpp \
	service/constant-propagation/IPConstantPropagationAnalysis.cpp \
	service/constant-propagation/ObjectDomain.cpp \
	service/constant-propagation/SignDomain.cpp \
	service/dataflow/LiveRange.cpp \
	service/escape-analysis/LocalPointersAnalysis.cpp \
	service/method-dedup/ConstantLifting.cpp \
	service/method-dedup/ConstantValue.cpp \
	service/method-dedup/MethodDedup.cpp \
	service/method-dedup/TypeTags.cpp \
	service/method-merger/MethodMerger.cpp \
	service/reference-update/MethodReference.cpp \
	service/reference-update/TypeReference.cpp \
	service/switch-dispatch/SwitchDispatch.cpp \
	service/switch-partitioning/SwitchEquivFinder.cpp \
	service/switch-partitioning/SwitchMethodPartitioning.cpp \
	tools/common/ToolsCommon.cpp \
	tools/redex-all/main.cpp

redex_all_LDADD = \
	libredex.la \
	$(BOOST_FILESYSTEM_LIB) \
	$(BOOST_SYSTEM_LIB) \
	$(BOOST_REGEX_LIB) \
	$(BOOST_PROGRAM_OPTIONS_LIB) \
	$(BOOST_THREAD_LIB) \
	-lpthread \
	-ldl

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

PYTHON_SRCS := redex.py \
	pyredex/__init__.py \
	pyredex/logger.py \
	pyredex/unpacker.py \
	pyredex/utils.py

redex: redex-all $(PYTHON_SRCS)
	$(srcdir)/bundle-redex.sh
