AUTOMAKE_OPTIONS = foreign

SUBDIRS = third-party/folly/folly . test

ACLOCAL_AMFLAGS = -I m4
AM_CXXFLAGS = --std=gnu++11 -O3 -Wall -g

#
# Include paths
#
AM_CPPFLAGS = \
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
	-I$(top_srcdir)/third-party/folly \
	-I$(top_srcdir)/util

#
# libredex: the optimizer's guts
#
noinst_LTLIBRARIES = libredex.la

libredex_la_SOURCES = \
	configparser/keeprules.cc \
	configparser/generated_files/parser.cc \
	configparser/generated_files/tokenizer.cc \
	liblocator/locator.cpp \
	libredex/Creators.cpp \
	libredex/Debug.cpp \
	libredex/DexAnnotation.cpp \
	libredex/DexClass.cpp \
	libredex/DexDebugOpcode.cpp \
	libredex/DexIdx.cpp \
	libredex/DexLoader.cpp \
	libredex/DexOpcode.cpp \
	libredex/DexOutput.cpp \
	libredex/DexUtil.cpp \
	libredex/JarLoader.cpp \
	libredex/PassManager.cpp \
	libredex/PgoFiles.cpp \
	libredex/ProguardLoader.cpp \
	libredex/ProguardMap.cpp \
	libredex/ReachableClasses.cpp \
	libredex/RedexContext.cpp \
	libredex/RegAlloc.cpp \
	libredex/Resolver.cpp \
	libredex/Show.cpp \
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
	opt/annoclasskill/AnnoClassKill.cpp \
	opt/annokill/AnnoKill.cpp \
	opt/bridge/Bridge.cpp \
	opt/delinit/DelInit.cpp \
	opt/delsuper/DelSuper.cpp \
	opt/final_inline/FinalInline.cpp \
	opt/interdex/InterDex.cpp \
	opt/local-dce/LocalDce.cpp \
	opt/peephole/Peephole.cpp \
	opt/rebindrefs/ReBindRefs.cpp \
	opt/remove_empty_classes/RemoveEmptyClasses.cpp \
	opt/renameclasses/RenameClasses.cpp \
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
	opt/unterface/Unterface.cpp \
	opt/unterface/UnterfaceOpt.cpp \
	util/Sha1.cpp

libredex_la_LIBADD = \
	-lboost_filesystem \
	-lboost_system \
	-lpthread \
	third-party/folly/folly/libfolly.la

#
# redex-all: the main executable
#
bin_PROGRAMS = redex-all

redex_all_SOURCES = \
	tools/redex-all/main.cpp \
	tools/redex-all/Passes.cpp

redex_all_LDADD = libredex.la

#
# redex: Python driver script
#
bin_SCRIPTS = redex
CLEANFILES = $(bin_SCRIPTS)
EXTRA_DIST = redex.py

redex: redex.py Makefile
	cp $(srcdir)/redex.py redex
	chmod +x redex

