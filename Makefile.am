AUTOMAKE_OPTIONS = foreign

CXXFLAGS = --std=gnu++11 -O3 -Wall -g

SUBDIRS = third-party/folly/folly

AM_CPPFLAGS = -I$(srcdir)/third-party/folly/folly

bin_PROGRAMS = redex-all

bin_SCRIPTS = redex
CLEANFILES = $(bin_SCRIPTS)
EXTRA_DIST = redex.py

redex: redex.py Makefile
	cp $(srcdir)/redex.py redex
	chmod +x redex

includes = \
	configparser \
	include \
	liblocator \
	libresource \
	libresource/android \
	libresource/androidfw \
	libresource/cutils \
	libresource/system \
	libresource/utils \
	opt \
	opt/all-static \
	opt/annoclasskill \
	opt/annokill \
	opt/bridge \
	opt/delinit \
	opt/delsuper \
	opt/final_inline \
	opt/interdex \
	opt/local-dce \
	opt/peephole \
	opt/rebindrefs \
	opt/remove_empty_classes \
	opt/renameclasses \
	opt/shorten-srcstrings \
	opt/simpleinline \
	opt/singleimpl \
	opt/static-sink \
	opt/staticrelo \
	opt/synth \
	opt/unterface \
	tools/redex-all \
	util

redex_all_CPPFLAGS = $(foreach i, $(includes), -I$(top_srcdir)/$i)
redex_all_LDADD = -lboost_filesystem -lboost_system -lpthread -Lthird-party/folly/folly/.libs -lfolly

redex_all_SOURCES = \
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
	tools/redex-all/main.cpp \
	tools/redex-all/Passes.cpp \
	util/Sha1.cpp
