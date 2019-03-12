---
id: usage
title: Usage
---

To use ReDex, first build your app and find the APK for it.  Then run:
```
redex path/to/your.apk -o path/to/output.apk
```

If you want some statistics about each pass, you can turn on tracing:
```
export TRACE=1
```

The result `output.apk` should be smaller and faster than the
input.  Enjoy!