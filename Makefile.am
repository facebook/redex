AUTOMAKE_OPTIONS = foreign

SUBDIRS = . test

ACLOCAL_AMFLAGS = -I m4
AM_CXXFLAGS = --std=gnu++11 -O3 -Wall -g

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
	-I$(top_srcdir)/opt/annoclasskill \
	-I$(top_srcdir)/opt/annokill \
	-I$(top_srcdir)/opt/bridge \
	-I$(top_srcdir)/opt/constant_propagation \
	-I$(top_srcdir)/opt/delinit \
	-I$(top_srcdir)/opt/delsuper \
	-I$(top_srcdir)/opt/final_inline \
	-I$(top_srcdir)/opt/access-marking \
	-I$(top_srcdir)/opt/interdex \
	-I$(top_srcdir)/opt/local-dce \
	-I$(top_srcdir)/opt/obfuscate \
	-I$(top_srcdir)/opt/peephole \
	-I$(top_srcdir)/opt/rebindrefs \
	-I$(top_srcdir)/opt/remove-unreachable \
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
	-I$(top_srcdir)/opt/verifier \
	-I$(top_srcdir)/util \
	-I$(top_srcdir)/tools/common \
	-I$(top_srcdir)/tools/redexdump \
	-I/usr/include/jsoncpp

#
# libredex: the optimizer's guts
#
noinst_LTLIBRARIES = libredex.la

libredex_la_SOURCES = \
	liblocator/locator.cpp \
	libredex/ConfigFiles.cpp \
	libredex/Creators.cpp \
	libredex/Debug.cpp \
	libredex/DexAnnotation.cpp \
	libredex/DexClass.cpp \
	libredex/DexDebugInstruction.cpp \
	libredex/DexIdx.cpp \
	libredex/DexLoader.cpp \
	libredex/DexInstruction.cpp \
	libredex/DexOutput.cpp \
	libredex/DexPosition.cpp \
	libredex/DexStore.cpp \
	libredex/DexUtil.cpp \
	libredex/JarLoader.cpp \
	libredex/Mutators.cpp \
	libredex/ObfuscateUtils.cpp \
	libredex/PassManager.cpp \
	libredex/PassRegistry.cpp \
	libredex/PrintSeeds.cpp \
	libredex/ProguardLexer.cpp \
	libredex/ProguardMap.cpp \
	libredex/ProguardMatcher.cpp \
	libredex/ProguardParser.cpp \
	libredex/ProguardPrintConfiguration.cpp \
	libredex/ProguardRegex.cpp \
	libredex/ProguardReporting.cpp \
	libredex/ReachableClasses.cpp \
	libredex/RedexContext.cpp \
	libredex/RegAlloc.cpp \
	libredex/Resolver.cpp \
	libredex/Show.cpp \
	libredex/Timer.cpp \
	libredex/Trace.cpp \
	libredex/Transform.cpp \
	libredex/Vinfo.cpp \
	libredex/Warning.cpp \
	libredex/WorkQueue.cpp \
	libresource/FileMap.cpp \
	libresource/RedexResources.cpp \
	libresource/ResourceTypes.cpp \
	libresource/SharedBuffer.cpp \
	libresource/Static.cpp \
	libresource/String16.cpp \
	libresource/String8.cpp \
	libresource/TypeWrappers.cpp \
	libresource/Unicode.cpp \
	libresource/VectorImpl.cpp \
	util/Sha1.cpp

libredex_la_LIBADD = \
	$(BOOST_FILESYSTEM_LIB) \
	$(BOOST_SYSTEM_LIB) \
	$(BOOST_REGEX_LIB) \
	-lpthread

#
# redex-all: the main executable
#
bin_PROGRAMS = redexdump
noinst_PROGRAMS = redex-all

redex_all_SOURCES = \
	opt/annoclasskill/AnnoClassKill.cpp \
	opt/annokill/AnnoKill.cpp \
	opt/bridge/Bridge.cpp \
	opt/constant_propagation/ConstantPropagation.cpp \
	opt/delinit/DelInit.cpp \
	opt/delsuper/DelSuper.cpp \
	opt/final_inline/FinalInline.cpp \
	opt/access-marking/AccessMarking.cpp \
	opt/interdex/InterDex.cpp \
	opt/local-dce/LocalDce.cpp \
	opt/obfuscate/Obfuscate.cpp \
	opt/peephole/Peephole.cpp \
	opt/rebindrefs/ReBindRefs.cpp \
	opt/remove_empty_classes/RemoveEmptyClasses.cpp \
	opt/remove-unreachable/RemoveUnreachable.cpp \
	opt/renameclasses/RenameClasses.cpp \
	opt/renameclasses/RenameClassesV2.cpp \
	opt/shorten-srcstrings/Shorten.cpp \
	opt/simpleinline/Deleter.cpp \
	opt/simpleinline/Devirtualizer.cpp \
	opt/simpleinline/InlineHelper.cpp \
	opt/simpleinline/SimpleInline.cpp \
	opt/singleimpl/SingleImpl.cpp \
	opt/singleimpl/SingleImplAnalyze.cpp \
	opt/singleimpl/SingleImplOptimize.cpp \
	opt/singleimpl/SingleImplStats.cpp \
	opt/singleimpl/SingleImplUtil.cpp \
	opt/static-sink/StaticSink.cpp \
	opt/staticrelo/StaticRelo.cpp \
	opt/synth/Synth.cpp \
	opt/track_resources/TrackResources.cpp \
	opt/unterface/Unterface.cpp \
	opt/unterface/UnterfaceOpt.cpp \
	opt/verifier/Verifier.cpp \
	opt/analysis_ref_graph/ReferenceGraphCreator.cpp \
	tools/redex-all/main.cpp

redex_all_LDADD = \
	libredex.la \
	$(BOOST_FILESYSTEM_LIB) \
	$(BOOST_SYSTEM_LIB) \
	$(BOOST_REGEX_LIB) \
	-lpthread


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
	-lpthread

#
# redex: Python driver script
#
bin_SCRIPTS = redex apkutil
CLEANFILES = redex

PYTHON_SRCS := redex.py \
	pyredex/__init__.py \
	pyredex/log.py \
	pyredex/unpacker.py \
	pyredex/utils.py

redex: redex-all $(PYTHON_SRCS)
	$(srcdir)/bundle-redex.sh
