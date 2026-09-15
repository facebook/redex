/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

import java.lang.annotation.Retention;
import java.lang.annotation.RetentionPolicy;

// Nothing calls value(), so the marker leaves the accessor unmarked, `sweep`
// drops it from the annotation class, and `sweep_annotation_elements` then
// rewrites every annotation that names it -- destroying the DexAnnotation the
// live reachability graph holds an ANNO node for. That is the sequence the
// post-sweep dump used to read freed memory on.
@Retention(RetentionPolicy.CLASS)
@interface PlumbingAnno {
  Class<?> value();
}

class PlumbingAnnoValue {}

// Retained by the keep rule, so the marker visits it and records an ANNO node
// for its annotation.
@PlumbingAnno(PlumbingAnnoValue.class)
public class RemovedReachabilityGraphPlumbingTest {
  public void annotatedEntry() {}
}
