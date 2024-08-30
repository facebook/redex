include $(top_srcdir)/Makefile.inc
include $(top_srcdir)/test/Makefile.inc
include $(top_srcdir)/test/samples/Makefile.inc

DX = @DX@

ANDROID_SDK = @ANDROID_SDK@
ANDROID_PLATFORM_VERSION = @ANDROID_PLATFORM_VERSION@

AAPT2 = $(ANDROID_SDK)/build-tools/29.0.2/aapt2
ANDROID_JAR = $(ANDROID_SDK)/platforms/$(ANDROID_PLATFORM_VERSION)/android.jar

ARSC_RESOURCE_SOURCES = values/strings.xml
XML_RESOURCE_SOURCES =
MANIFEST_SOURCE = AndroidManifest.xml
JAVA_SOURCES = MainActivity.java
JAVA_PACKAGE = com/fb/strings

$(eval $(call make_apk,$(ARSC_RESOURCE_SOURCES),$(XML_RESOURCE_SOURCES),$(MANIFEST_SOURCE),$(JAVA_SOURCES),$(JAVA_PACKAGE)))
