#!/bin/bash
buck install native/redex/test/instr:test_redex && adb shell am instrument -w com.facebook.redex.test.instr/android.support.test.runner.AndroidJUnitRunner
