---
id: interdex
title: Interdex
---

The Interdex Pass addresses the problem that the ordering of classes inside dexes
and their distribution between dexes (if the application has multiple dexes) does
not correspond to the order that they are accessed with during runtime if we use
the normal compilation tool chain.

Consider a dex with classes c0, c1, c2, ... . Let's assume the order in which
they are accessed during runtime is c0, c1, c2, ... .
The actual layout in the bytecode section might be
c1000, c291, c3, c705, ..., c0,..., c1 and so on

And of course the same issue happens if there are multiple dexes, with the
additional problem that classes might be in different dexes too.

Reordering classes to be in the same order as they are accessed at
runtime has several benefits:
- Less IO: IO happens in bigger chunks, the exact size of each chunk read being a function of OS and file system settings. Chunk sizes of 4KB are the minimum, with 128KB being possible with read ahead settings for common file systems. Many classes will be smaller than these chunk sizes and there will be IO for data that is not immediately used
- Less memory usage: Memory is allocated at page granularity (4KB). If most of the 4KB page is unused data, the memory usage of the process will be increased. On many Android systems memory is a critical resource and the OS is constantly struggling to free up memory. Inefficient usage of memory can make a whole system behave worse and increased memory usage by an application makes it more likely that the OS will have to kill it at some point to free up enough memory for other applications to run.
- Less page cache pollution: As mentioned under IO, IO is often performed in coarse chunks. Data read in is buffered in the page cache, which is a limited resource in many typical Android devices. Bringing in data that is not immediately used causes pollution of the page cache and potential eviction of useful data from the application or other applications.

# Generating input data

The flow for generating profiling data is:
- Establish a typical use case for your app. This might be a automated test or just the developer starting up the application and performing common interactions with it
- getting a heap dump from the running app
- parsing the heap dump with the provided script. This generates a text file with the classes loaded by the app and VM in the order that they were loaded

Note that it is very important that you have compiled the app that you're going
to be generating the heap dump with the same settings that you use in your
release build, otherwise the class ordering will not reflect reality.

It is equally important that the usage reflects a real-world scenario. Using
an overly simplistic test or startup scenario will not generate representative
data and will not lead to performance improvements.

Note that if you use proguard for obfuscation or another program to the same
end, you'll have to disable those steps for profiling. Obfuscation is not
guaranteed to be stable and makes mapping class names from one generated apk
to the next difficult.

## Step-by-Step on how to generate class list
Connect your device to your computer, so you can execute adb commands. You need to have root on your device to execute the dump heap command.
On your computer:

 // get the process if of your app
 ```
 adb shell ps | grep YOUR_APP_NAME | awk '{print $2}' > YOUR_PID ( if you don't have awk, the second value is the pid of your app)
 ```
 // dump the heap of your app. You WILL NEED ROOT for this step
 ```
 adb root
 adb shell am dumpheap YOUR_PID /data/local/tmp/SOMEDUMP.hprof
 ```
 // copy the heap to your host computer
 ```
 adb pull /data/local/tmp/SOMEDUMP.hprof YOUR_DIR_HERE/.
 ```
 // pass the heap dump to the python script for parsing and printing out the class list
 // Note that the script needs python 3
 ```
 redex/tools/hprof/dump_classes_from_hprof.py --hprof YOUR_DIR_HERE/SOMEDUMP.hprof > list_of_classes.txt
 ```

 If everything worked out, list_of_classes.txt will contain a large number of lines of the form foobar.class
 You'll note that many of the classes list are actually classes provided by the system and not from your app.
 This is ok, since the Interdex Pass will ignore any entries for which it cannot find the corresponding classes to in the apk.

 Note: you must have ```enum34``` installed for the script to work.

# Usage

To enable the Interdex pass for you application, add the following to your config file:

- add "InterDexPass" to passes
- add "coldstart\_classes": "list\_of\_classes.txt" to the config file

## Options

There are two flags that can be set to influence the behavior of the Interdex pass

- emit_canaries: This flag controls whether each secondary dex has
  a non-functional canary class added. Defaults to false.
  Enable this only if you explicitly know that you need it.

- static_prune: This flag controls whether Interdex attempts to remove classes
  that have no references to them from the rest of the set of classes in the pgo list.

# Measuring benefit

- Install an apk without interdex pass enabled
  ```
  adb shell ps | grep YOUR_APP_NAME | awk '{ print $2 }' > YOUR_PID
  adb shell dumpsys meminfo YOUR_PID
  ```
- Note how much memory your app uses and the .dex mmap row
- Do all the steps described above and rerun redex with the Interdex Pass enabled and using the profiling data you generated
- Install the apk with interdex enabled
- start the app and repeat the step to get the meminfo
- Note total memory usage and .dex mmap in particular
- Hopefully memory usage has gone down!

If you want performance measurements, you'll have to set up a test for app startup and run it on apks with and without the interdex pass applied.
