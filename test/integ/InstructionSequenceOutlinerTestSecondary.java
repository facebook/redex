/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

package com.facebook.redextest;

class InstructionSequenceOutlinerTestSecondary {

    // Must mirror the primary class exactly: the outliner only reuses an
    // outlined method across dexes when the instruction sequences match.

    public void secondary1() {
      InstructionSequenceOutlinerTest.println(
          InstructionSequenceOutlinerTest.SA,
          InstructionSequenceOutlinerTest.SB,
          InstructionSequenceOutlinerTest.SC);
      InstructionSequenceOutlinerTest.println(
          InstructionSequenceOutlinerTest.SD,
          InstructionSequenceOutlinerTest.SE,
          InstructionSequenceOutlinerTest.SF);
      InstructionSequenceOutlinerTest.println(
          InstructionSequenceOutlinerTest.SG,
          InstructionSequenceOutlinerTest.SH,
          InstructionSequenceOutlinerTest.SI);
      InstructionSequenceOutlinerTest.println(
          InstructionSequenceOutlinerTest.SJ,
          InstructionSequenceOutlinerTest.SK,
          InstructionSequenceOutlinerTest.SL);
      InstructionSequenceOutlinerTest.println(
          InstructionSequenceOutlinerTest.SM,
          InstructionSequenceOutlinerTest.SN,
          InstructionSequenceOutlinerTest.SO);
    }

    public void secondary2() {
      InstructionSequenceOutlinerTest.println(
          InstructionSequenceOutlinerTest.SA,
          InstructionSequenceOutlinerTest.SB,
          InstructionSequenceOutlinerTest.SC);
      InstructionSequenceOutlinerTest.println(
          InstructionSequenceOutlinerTest.SD,
          InstructionSequenceOutlinerTest.SE,
          InstructionSequenceOutlinerTest.SF);
      InstructionSequenceOutlinerTest.println(
          InstructionSequenceOutlinerTest.SG,
          InstructionSequenceOutlinerTest.SH,
          InstructionSequenceOutlinerTest.SI);
      InstructionSequenceOutlinerTest.println(
          InstructionSequenceOutlinerTest.SJ,
          InstructionSequenceOutlinerTest.SK,
          InstructionSequenceOutlinerTest.SL);
      InstructionSequenceOutlinerTest.println(
          InstructionSequenceOutlinerTest.SM,
          InstructionSequenceOutlinerTest.SN,
          InstructionSequenceOutlinerTest.SO);
    }
}
