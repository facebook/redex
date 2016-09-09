/**
 * Copyright (c) 2016-present, Facebook, Inc.
 * All rights reserved.
 *
 * This source code is licensed under the BSD-style license found in the
 * LICENSE file in the root directory of this source tree. An additional grant
 * of patent rights can be found in the PATENTS file in the same directory.
 */

#include <vector>

#include "Pass.h"

#include "AnnoClassKill.h"
#include "AnnoKill.h"
#include "Bridge.h"
#include "ConstantPropagation.h"
#include "DelInit.h"
#include "DelSuper.h"
#include "FinalInline.h"
#include "InterDex.h"
#include "LocalDce.h"
#include "Peephole.h"
#include "ReBindRefs.h"
#include "RemoveEmptyClasses.h"
#include "RemoveUnreachable.h"
#include "RenameClasses.h"
#include "Shorten.h"
#include "SimpleInline.h"
#include "SingleImpl.h"
#include "StaticRelo.h"
#include "StaticSink.h"
#include "Synth.h"
#include "Unterface.h"

std::vector<Pass*> create_passes() {
  return std::vector<Pass*>({
    new AnnoClassKillPass(),
    new AnnoKillPass(),
    new BridgePass(),
    new ConstantPropagationPass(),
    new DelInitPass(),
    new DelSuperPass(),
    new FinalInlinePass(),
    new InterDexPass(),
    new LocalDcePass(),
    new PeepholePass(),
    new ReBindRefsPass(),
    new RemoveEmptyClassesPass(),
    new RemoveUnreachablePass(),
    new RenameClassesPass(),
    new ShortenSrcStringsPass(),
    new SimpleInlinePass(),
    new SingleImplPass(),
    new StaticSinkPass(),
    new StaticReloPass(),
    new SynthPass(),
    new UnterfacePass(),
  });
}
