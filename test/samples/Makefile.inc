define make_compiled_resource
compiled/$(2): res/$(1)
	mkdir -p compiled
	$$(AAPT2) compile -o compiled $$^
	test -f $$@

endef

define compile_java_source
build/$(2): src/$(1) build/com/facebook/R.class
	$$(JAVAC) -source 1.7 -target 1.7 -cp build:$$(ANDROID_JAR) -d build -sourcepath $$(src_dir)/src $$<

endef

# My Makefile-fu is weak. I don't know why variables are vanishing when doing an include.
# That's pretty annoying and makes the following function quite ugly.
#
# $1: res files (arsc)
#     a space-separated list of files that become arsc resources (values)
# $2: res files (xml)
#     a space-separated list of files that become xml resources (layouts)
# $3: manifest source
# $4: java sources
#     a space-separated list of files that are Java inputs
# $5: java package path (get rid of this?)
define make_apk

$(foreach res,$(1),$(call make_compiled_resource,$(res),$(subst .xml,,$(subst /,_,$(res))).arsc.flat))

$(foreach res,$(2),$(call make_compiled_resource,$(res),$(subst /,_,$(res)).flat))

linked.apk java/com/facebook/R.java: $(foreach res,$(1),compiled/$(subst .xml,,$(subst /,_,$(res))).arsc.flat) $(foreach res,$(2),compiled/$(subst /,_,$(res)).flat) $(3)
	mkdir -p java
	$$(AAPT2) link -o linked.apk --manifest $$(srcdir)/$(3) --java java --custom-package com.facebook -I $$(ANDROID_JAR) $(foreach res,$(1),compiled/$(subst .xml,,$(subst /,_,$(res))).arsc.flat) $(foreach res,$(2),compiled/$(subst /,_,$(res)).flat)

build/com/facebook/R.class: java/com/facebook/R.java
	mkdir -p build
	$$(JAVAC) -source 1.7 -target 1.7 -cp build -d build -sourcepath java $$^

$(foreach jsrc,$(4),$(call compile_java_source,$(jsrc),$(5)/$(subst .java,.class,$(jsrc))))

classes.jar: build/com/facebook/R.class $(foreach jsrc,$(4),build/$(5)/$(subst .java,.class,$(jsrc)))
	jar cf $$@ -C build .

classes.dex: classes.jar
	$$(DX) --dex --output=$$@ $$^

unsigned.apk: linked.apk classes.dex
	cp -f linked.apk unsigned.apk
	zip -ur unsigned.apk classes.dex

all-local: unsigned.apk

check-local: unsigned.apk

endef
