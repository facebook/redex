---
id: configuring
title: Configuring ReDex
sidebar_position: 2
---

ReDex can be configured to run different optimizations or to alter the behavior
of existing optimizations.  While this isn't always necessary, it's helpful to
be able to tweak settings to get the best results.

# A Simple Example

A starting point for configuration is to use a configuration similar to the
default config that's baked in to the `redex` binary.  You can find this config
in [config/default.config](https://github.com/facebook/redex/blob/master/config/default.config).  This is what it looks like:

```
{
"redex" : {
  "passes" : [
    "ReBindRefsPass",
    "BridgeSynthInlinePass",
    "FinalInlinePassV2",
    "DelSuperPass",
    "SingleImplPass",
    "MethodInlinePass",
    "StaticReloPassV2",
    "RemoveUnreachablePass",
    "RegAllocPass"
  ]
}
}
```

Name this file default.config, and invoke `redex` with it:

```
% redex.py -c default.config -o tmp/output.apk input.apk
```

This will do... exactly what redex does without the config.  Not so exciting, is
it?  Let's move on to something more advanced.

# Selecting Optimization Passes

Changing the set of optimizations ReDex runs is easy; just add (or remove) the
pass name from the redex.passes list.  For example, let's say you want to remove
the StaticReloPassV2 optimization while you're debugging something.  Just use
this config:

```
{
"redex" : {
  "passes" : [
    "ReBindRefsPass",
    "BridgeSynthInlinePass",
    "FinalInlinePassV2",
    "DelSuperPass",
    "SingleImplPass",
    "MethodInlinePass",
    "RemoveUnreachablePass",
    "RegAllocPass"
  ]
}
}
```

# Configuring optimization behavior

Each optimization pass has some configurable parameters that are specific to
that pass.  These are often blocklists (or allowlists) indicating what code the
optimization should leave alone (for blocklists) or what code should be
optimized (for allowlists).

A simple example is ReachableNativesPass.  This pass reports which methods are
reachable from native code.  You can tell it where to write that report by adding
a config entry:

```
"redex" : {
  "passes" : [
    "ReachableNativesPass",
    "RegAllocPass"
  ]
},
"ReachableNativesPass" : {
  "output_file_name" : "redex-reachable-natives.txt"
}
```

Options for each pass are documented with that pass.

# Global options

* `redex.passes`
   **Type**: array of strings
   A list of passes to be run in the specified order.

* `coldstart_classes`
   **Type**: string
   Path to a file containing a list of class names in the order they are used
   for cold start.  Example format:
   ```
   com/foo/Bar.class
   com/foo/Baz.class
   ...
   com/foo/Quux.class
   ```

* `proguard_map`
   **Type**: string
   Path to a file containing ProGuard's mapping of unobfuscated
   class/field/method names to obfuscated names.  This option is useful if you
   are running ReDex after ProGuard, so that ReDex will properly understand
   obfuscated names.

# Complete configuration

To see the (almost) complete list of configuration parameters, run
```
redex-all --reflect-config
```
(Note that you run `redex-all` here, not the `redex.py` wrapper!)

This emits a JSON document detailing parameters, their types, defaults and
possibly documentation.
