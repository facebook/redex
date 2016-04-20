DEXDUMP = $(ANDROID_TOOLS)/dexdump
REDEX = ../../../../redex
INPUT_APK = synth-example-release

.PHONY:	all clean

all:	$(INPUT_APK)-redex.apk classes.dump classes-redex.dump

clean:
	rm -rf $(INPUT_APK)-redex.apk classes.dump classes-redex.dump

KEYSTORE = $(HOME)/.android/debug.keystore
ALIAS = androiddebugkey
KEYPASS = android

$(INPUT_APK)-redex.apk:	$(INPUT_APK).apk
			$(REDEX) --sign -s $(KEYSTORE) -a $(ALIAS) -p $(KEYPASS) -o $@ $< 

uninstall:	
		adb uninstall com.facebook.redex.examples.synth

install:	
		adb install -r $(INPUT_APK)-redex.apk

run:		
	    adb shell am start -n com.facebook.redex.examples.synth/com.facebook.redex.examples.synth.MainActivity


classes.dex:        $(INPUT_APK).apk
			unzip -p $< classes.dex > $@

classes-redex.dex:	$(INPUT_APK)-redex.apk
			unzip -p $< classes.dex > $@

%.dump:		%.dex
		$(DEXDUMP) -d -f -h $< > $@	
