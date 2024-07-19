include $(top_srcdir)/Makefile.inc
include $(top_srcdir)/test/Makefile.inc
include $(top_srcdir)/test/samples/Makefile.inc

DX = @DX@

ANDROID_SDK = @ANDROID_SDK@
ANDROID_PLATFORM_VERSION = @ANDROID_PLATFORM_VERSION@

AAPT2 = $(ANDROID_SDK)/build-tools/29.0.2/aapt2
ANDROID_JAR = $(ANDROID_SDK)/platforms/$(ANDROID_PLATFORM_VERSION)/android.jar

ARSC_RESOURCE_SOURCES = values/values.xml values-night/values.xml
XML_RESOURCE_SOURCES = layout/activity_main.xml layout/class_names.xml layout/test_views.xml
MANIFEST_SOURCE = AndroidManifest.xml
JAVA_SOURCES = MainActivity.java
JAVA_PACKAGE = com/fb/resources

$(eval $(call make_apk,$(ARSC_RESOURCE_SOURCES),$(XML_RESOURCE_SOURCES),$(MANIFEST_SOURCE),$(JAVA_SOURCES),$(JAVA_PACKAGE)))
