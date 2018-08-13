AUTOMAKE_OPTIONS = foreign

SUBDIRS = . test

ACLOCAL_AMFLAGS = -I m4
AM_CXXFLAGS = --std=gnu++14 -O3 -Wall -g

export REDEX_INCLUDE_FLAGS = \
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
	-I$(top_srcdir)/opt/bridge \
	-I$(top_srcdir)/opt/check_breadcrumbs \
	-I$(top_srcdir)/opt/constant-propagation \
	-I$(top_srcdir)/opt/dead-code-elimination \
	-I$(top_srcdir)/opt/dedup_blocks \
	-I$(top_srcdir)/opt/delinit \
	-I$(top_srcdir)/opt/delsuper \
	-I$(top_srcdir)/opt/final_inline \
	-I$(top_srcdir)/opt/hotness-score \
	-I$(top_srcdir)/opt/inlineinit \
	-I$(top_srcdir)/opt/instrument \
	-I$(top_srcdir)/opt/interdex \
	-I$(top_srcdir)/opt/local-dce \
	-I$(top_srcdir)/opt/obfuscate \
	-I$(top_srcdir)/opt/original_name \
	-I$(top_srcdir)/opt/outliner \
	-I$(top_srcdir)/opt/peephole \
	-I$(top_srcdir)/opt/print-members \
	-I$(top_srcdir)/opt/rebindrefs \
	-I$(top_srcdir)/opt/redundant_move_elimination \
	-I$(top_srcdir)/opt/regalloc \
	-I$(top_srcdir)/opt/remove-builders \
	-I$(top_srcdir)/opt/remove-unreachable \
	-I$(top_srcdir)/opt/remove_empty_classes \
	-I$(top_srcdir)/opt/remove_gotos \
	-I$(top_srcdir)/opt/remove_unused_args \
	-I$(top_srcdir)/opt/renameclasses \
	-I$(top_srcdir)/opt/reorder-interfaces \
	-I$(top_srcdir)/opt/shorten-srcstrings \
	-I$(top_srcdir)/opt/simpleinline \
	-I$(top_srcdir)/opt/singleimpl \
	-I$(top_srcdir)/opt/static-sink \
	-I$(top_srcdir)/opt/staticrelo \
	-I$(top_srcdir)/opt/string_concatenator \
	-I$(top_srcdir)/opt/string_simplification \
	-I$(top_srcdir)/opt/strip-debug-info \
	-I$(top_srcdir)/opt/synth \
	-I$(top_srcdir)/opt/test_cfg \
	-I$(top_srcdir)/opt/track_resources \
	-I$(top_srcdir)/opt/unreferenced_interfaces \
	-I$(top_srcdir)/opt/unterface \
	-I$(top_srcdir)/opt/verifier \
	-I$(top_srcdir)/opt/virtual_scope \
	-I$(top_srcdir)/service/constant-propagation \
	-I$(top_srcdir)/service/dataflow \
	-I$(top_srcdir)/service/escape-analysis \
	-I$(top_srcdir)/service/method-dedup \
	-I$(top_srcdir)/service/reference-update \
	-I$(top_srcdir)/service/switch-dispatch \
	-I$(top_srcdir)/shared \
	-I$(top_srcdir)/sparta/include \
	-I$(top_srcdir)/tools/common \
	-I$(top_srcdir)/tools/redexdump \
	-I$(top_srcdir)/util \
	-I/usr/include/jsoncpp

#
# Include paths
#
AM_CPPFLAGS = $(REDEX_INCLUDE_FLAGS)

#
# libredex: the optimizer's guts
#
noinst_LTLIBRARIES = libredex.la

export LIBREDEX_CPP_FILES = \
	$(top_srcdir)/liblocator/locator.cpp \
	$(top_srcdir)/libredex/AnnoUtils.cpp \
	$(top_srcdir)/libredex/ApkManager.cpp \
	$(top_srcdir)/libredex/CallGraph.cpp \
	$(top_srcdir)/libredex/ClassHierarchy.cpp \
	$(top_srcdir)/libredex/ConfigFiles.cpp \
	$(top_srcdir)/libredex/Creators.cpp \
	$(top_srcdir)/libredex/ControlFlow.cpp \
	$(top_srcdir)/libredex/Debug.cpp \
	$(top_srcdir)/libredex/DexAnnotation.cpp \
	$(top_srcdir)/libredex/DexClass.cpp \
	$(top_srcdir)/libredex/DexDebugInstruction.cpp \
	$(top_srcdir)/libredex/DexEncoding.cpp \
	$(top_srcdir)/libredex/DexIdx.cpp \
	$(top_srcdir)/libredex/DexLoader.cpp \
	$(top_srcdir)/libredex/DexInstruction.cpp \
	$(top_srcdir)/libredex/DexMemberRefs.cpp \
	$(top_srcdir)/libredex/DexOpcode.cpp \
	$(top_srcdir)/libredex/DexOutput.cpp \
	$(top_srcdir)/libredex/DexPosition.cpp \
	$(top_srcdir)/libredex/DexStore.cpp \
	$(top_srcdir)/libredex/DexUtil.cpp \
	$(top_srcdir)/libredex/FieldOpTracker.cpp \
	$(top_srcdir)/libredex/ImmutableSubcomponentAnalyzer.cpp \
	$(top_srcdir)/libredex/Inliner.cpp \
	$(top_srcdir)/libredex/InstructionLowering.cpp \
	$(top_srcdir)/libredex/IRAssembler.cpp \
	$(top_srcdir)/libredex/IRCode.cpp \
	$(top_srcdir)/libredex/IRInstruction.cpp \
	$(top_srcdir)/libredex/IRList.cpp \
	$(top_srcdir)/libredex/IROpcode.cpp \
	$(top_srcdir)/libredex/IRTypeChecker.cpp \
	$(top_srcdir)/libredex/JarLoader.cpp \
	$(top_srcdir)/libredex/Match.cpp \
	$(top_srcdir)/libredex/MethodDevirtualizer.cpp \
	$(top_srcdir)/libredex/Mutators.cpp \
	$(top_srcdir)/libredex/OptData.cpp \
	$(top_srcdir)/libredex/PassManager.cpp \
	$(top_srcdir)/libredex/PassRegistry.cpp \
	$(top_srcdir)/libredex/PluginRegistry.cpp \
	$(top_srcdir)/libredex/PointsToSemantics.cpp \
	$(top_srcdir)/libredex/PointsToSemanticsUtils.cpp \
	$(top_srcdir)/libredex/PrintSeeds.cpp \
	$(top_srcdir)/libredex/ProguardLexer.cpp \
	$(top_srcdir)/libredex/ProguardMap.cpp \
	$(top_srcdir)/libredex/ProguardMatcher.cpp \
	$(top_srcdir)/libredex/ProguardParser.cpp \
	$(top_srcdir)/libredex/ProguardPrintConfiguration.cpp \
	$(top_srcdir)/libredex/ProguardRegex.cpp \
	$(top_srcdir)/libredex/ProguardReporting.cpp \
	$(top_srcdir)/libredex/ReachableClasses.cpp \
	$(top_srcdir)/libredex/ReachableObjects.cpp \
	$(top_srcdir)/libredex/RedexContext.cpp \
	$(top_srcdir)/libredex/Resolver.cpp \
	$(top_srcdir)/libredex/Show.cpp \
	$(top_srcdir)/libredex/SimpleReflectionAnalysis.cpp \
	$(top_srcdir)/libredex/Timer.cpp \
	$(top_srcdir)/libredex/Trace.cpp \
	$(top_srcdir)/libredex/Transform.cpp \
	$(top_srcdir)/libredex/IRTypeChecker.cpp \
	$(top_srcdir)/libredex/TypeSystem.cpp \
	$(top_srcdir)/libredex/Vinfo.cpp \
	$(top_srcdir)/libredex/VirtualScope.cpp \
	$(top_srcdir)/libredex/Warning.cpp \
	$(top_srcdir)/libresource/FileMap.cpp \
	$(top_srcdir)/libresource/RedexResources.cpp \
	$(top_srcdir)/libresource/ResourceTypes.cpp \
	$(top_srcdir)/libresource/Serialize.cpp \
	$(top_srcdir)/libresource/SharedBuffer.cpp \
	$(top_srcdir)/libresource/Static.cpp \
	$(top_srcdir)/libresource/String16.cpp \
	$(top_srcdir)/libresource/String8.cpp \
	$(top_srcdir)/libresource/TypeWrappers.cpp \
	$(top_srcdir)/libresource/Unicode.cpp \
	$(top_srcdir)/libresource/VectorImpl.cpp \
	$(top_srcdir)/shared/DexDefs.cpp \
	$(top_srcdir)/shared/file-utils.cpp \
	$(top_srcdir)/util/CommandProfiling.cpp \
	$(top_srcdir)/util/JemallocUtil.cpp \
	$(top_srcdir)/util/Sha1.cpp

libredex_la_SOURCES = $(LIBREDEX_CPP_FILES)

#
# redex-all: the main executable
#
bin_PROGRAMS = redexdump
noinst_PROGRAMS = redex-all

export REDEX_OPT_CPP_FILES = \
	$(top_srcdir)/libredex/DexAsm.cpp \
	$(top_srcdir)/opt/access-marking/AccessMarking.cpp \
	$(top_srcdir)/opt/add_redex_txt_to_apk/AddRedexTxtToApk.cpp \
	$(top_srcdir)/opt/analysis_ref_graph/ReferenceGraphCreator.cpp \
	$(top_srcdir)/opt/annokill/AnnoKill.cpp \
	$(top_srcdir)/opt/basic-block/BasicBlockProfile.cpp \
	$(top_srcdir)/opt/bridge/Bridge.cpp \
	$(top_srcdir)/opt/check_breadcrumbs/CheckBreadcrumbs.cpp \
	$(top_srcdir)/opt/constant-propagation/ConstantPropagation.cpp \
	$(top_srcdir)/opt/constant-propagation/ConstantPropagationRuntimeAssert.cpp \
	$(top_srcdir)/opt/constant-propagation/ConstantPropagationTransform.cpp \
	$(top_srcdir)/opt/constant-propagation/IPConstantPropagation.cpp \
	$(top_srcdir)/opt/copy-propagation/AliasedRegisters.cpp \
	$(top_srcdir)/opt/copy-propagation/CopyPropagationPass.cpp \
	$(top_srcdir)/opt/dead-code-elimination/DeadCodeEliminationPass.cpp \
	$(top_srcdir)/opt/dead-code-elimination/SideEffectSummary.cpp \
	$(top_srcdir)/opt/dead-code-elimination/UsedVarsAnalysis.cpp \
	$(top_srcdir)/opt/dedup_blocks/DedupBlocksPass.cpp \
	$(top_srcdir)/opt/delinit/DelInit.cpp \
	$(top_srcdir)/opt/delsuper/DelSuper.cpp \
	$(top_srcdir)/opt/final_inline/FinalInline.cpp \
	$(top_srcdir)/opt/final_inline/FinalInlineV2.cpp \
	$(top_srcdir)/opt/hotness-score/HotnessScore.cpp \
	$(top_srcdir)/opt/inlineinit/InlineInit.cpp \
	$(top_srcdir)/opt/instrument/Instrument.cpp \
	$(top_srcdir)/opt/interdex/DexStructure.cpp \
	$(top_srcdir)/opt/interdex/InterDex.cpp \
	$(top_srcdir)/opt/interdex/InterDexPass.cpp \
	$(top_srcdir)/opt/local-dce/LocalDce.cpp \
	$(top_srcdir)/opt/obfuscate/Obfuscate.cpp \
	$(top_srcdir)/opt/obfuscate/ObfuscateUtils.cpp \
	$(top_srcdir)/opt/obfuscate/VirtualRenamer.cpp \
	$(top_srcdir)/opt/original_name/OriginalNamePass.cpp \
	$(top_srcdir)/opt/outliner/Outliner.cpp \
	$(top_srcdir)/opt/peephole/Peephole.cpp \
	$(top_srcdir)/opt/peephole/RedundantCheckCastRemover.cpp \
	$(top_srcdir)/opt/print-members/PrintMembers.cpp \
	$(top_srcdir)/opt/reachability_graph/ReachabilityGraphPrinter.cpp \
	$(top_srcdir)/opt/rebindrefs/ReBindRefs.cpp \
	$(top_srcdir)/opt/regalloc/GraphColoring.cpp \
	$(top_srcdir)/opt/regalloc/Interference.cpp \
	$(top_srcdir)/opt/regalloc/RegAlloc.cpp \
	$(top_srcdir)/opt/regalloc/RegisterType.cpp \
	$(top_srcdir)/opt/regalloc/Split.cpp \
	$(top_srcdir)/opt/regalloc/VirtualRegistersFile.cpp \
	$(top_srcdir)/opt/remove-builders/RemoveBuilders.cpp \
	$(top_srcdir)/opt/remove-builders/RemoveBuildersHelper.cpp \
	$(top_srcdir)/opt/remove-unreachable/RemoveUnreachable.cpp \
	$(top_srcdir)/opt/remove-unread-fields/RemoveUnreadFields.cpp \
	$(top_srcdir)/opt/remove_empty_classes/RemoveEmptyClasses.cpp \
	$(top_srcdir)/opt/remove_gotos/RemoveGotos.cpp \
	$(top_srcdir)/opt/remove_unused_args/RemoveUnusedArgs.cpp \
	$(top_srcdir)/opt/renameclasses/RenameClasses.cpp \
	$(top_srcdir)/opt/renameclasses/RenameClassesV2.cpp \
	$(top_srcdir)/opt/reorder-interfaces/ReorderInterfaces.cpp \
	$(top_srcdir)/opt/shorten-srcstrings/Shorten.cpp \
	$(top_srcdir)/opt/simpleinline/Deleter.cpp \
	$(top_srcdir)/opt/simpleinline/SimpleInline.cpp \
	$(top_srcdir)/opt/singleimpl/SingleImpl.cpp \
	$(top_srcdir)/opt/singleimpl/SingleImplAnalyze.cpp \
	$(top_srcdir)/opt/singleimpl/SingleImplOptimize.cpp \
	$(top_srcdir)/opt/singleimpl/SingleImplStats.cpp \
	$(top_srcdir)/opt/singleimpl/SingleImplUtil.cpp \
	$(top_srcdir)/opt/simplify_cfg/SimplifyCFG.cpp \
	$(top_srcdir)/opt/static-sink/StaticSink.cpp \
	$(top_srcdir)/opt/staticrelo/StaticRelo.cpp \
	$(top_srcdir)/opt/staticrelo/StaticReloV2.cpp \
	$(top_srcdir)/opt/string_concatenator/StringConcatenator.cpp \
	$(top_srcdir)/opt/string_simplification/StringIterator.cpp \
	$(top_srcdir)/opt/string_simplification/StringSimplification.cpp \
	$(top_srcdir)/opt/strip-debug-info/StripDebugInfo.cpp \
	$(top_srcdir)/opt/synth/Synth.cpp \
	$(top_srcdir)/opt/test_cfg/TestCFG.cpp \
	$(top_srcdir)/opt/track_resources/TrackResources.cpp \
	$(top_srcdir)/opt/unreferenced_interfaces/UnreferencedInterfaces.cpp \
	$(top_srcdir)/opt/unterface/Unterface.cpp \
	$(top_srcdir)/opt/unterface/UnterfaceOpt.cpp \
	$(top_srcdir)/opt/verifier/Verifier.cpp \
	$(top_srcdir)/opt/virtual_scope/MethodDevirtualizationPass.cpp \
	$(top_srcdir)/service/constant-propagation/ConstantEnvironment.cpp \
	$(top_srcdir)/service/constant-propagation/ConstantPropagationAnalysis.cpp \
	$(top_srcdir)/service/constant-propagation/ConstantPropagationWholeProgramState.cpp \
	$(top_srcdir)/service/constant-propagation/IPConstantPropagationAnalysis.cpp \
	$(top_srcdir)/service/constant-propagation/ObjectDomain.cpp \
	$(top_srcdir)/service/constant-propagation/SignDomain.cpp \
	$(top_srcdir)/service/dataflow/LiveRange.cpp \
	$(top_srcdir)/service/escape-analysis/LocalPointersAnalysis.cpp \
	$(top_srcdir)/service/method-dedup/ConstantLifting.cpp \
	$(top_srcdir)/service/method-dedup/MethodDedup.cpp \
	$(top_srcdir)/service/method-dedup/TypeTags.cpp \
	$(top_srcdir)/service/reference-update/MethodReference.cpp \
	$(top_srcdir)/service/reference-update/TypeReference.cpp \
	$(top_srcdir)/service/switch-dispatch/SwitchDispatch.cpp \
	$(top_srcdir)/tools/redex-all/main.cpp

redex_all_SOURCES = $(REDEX_OPT_CPP_FILES)

export REDEX_LIBS = \
	$(BOOST_FILESYSTEM_LIB) \
	$(BOOST_SYSTEM_LIB) \
	$(BOOST_REGEX_LIB) \
	$(BOOST_IOSTREAMS_LIB) \
	$(BOOST_PROGRAM_OPTIONS_LIB) \
	$(BOOST_THREAD_LIB) \
	-lpthread

libredex_la_LIBADD = $(REDEX_LIBS)

redex_all_LDADD = \
	libredex.la \
	$(REDEX_LIBS) \
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

debug_print:
	echo $(AM_CPPFLAGS) >/dev/null
	echo ''
	echo $(libredex_la_SOURCES) >/dev/null
	echo ''
	echo $(redex_all_SOURCES) >/dev/null
	echo ''
	echo $(libredex_la_LIBADD) >/dev/null
