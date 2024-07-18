include $(top_srcdir)/Makefile.inc
include $(top_srcdir)/test/Makefile.inc
include $(top_srcdir)/test/samples/Makefile.inc

DX = @DX@

ANDROID_SDK = @ANDROID_SDK@
ANDROID_PLATFORM_VERSION = @ANDROID_PLATFORM_VERSION@

AAPT2 = $(ANDROID_SDK)/build-tools/29.0.2/aapt2
ANDROID_JAR = $(ANDROID_SDK)/platforms/android-$(ANDROID_PLATFORM_VERSION)/android.jar

$(eval $(call make_apk,values/strings.xml,,AndroidManifest.xml,MainActivity.java,com/fb/strings))
