/**
 * Copyright (c) 2016-present, Facebook, Inc.
 * All rights reserved.
 *
 * This source code is licensed under the BSD-style license found in the
 * LICENSE file in the root directory of this source tree. An additional grant
 * of patent rights can be found in the PATENTS file in the same directory.
 */

package com.facebook.redex.test.instr;

import static org.fest.assertions.api.Assertions.*;

import java.lang.reflect.InvocationTargetException;
import java.lang.reflect.Method;

import static org.fest.assertions.api.Assertions.*;

import org.junit.Before;
import org.junit.Test;

interface IForceRename {

}

class CantRename {
}

class ForceRename implements IForceRename {
}

public class RenameClassesV2Test {

  @Test
  public void testRenames() {
    // Make sure CantRename's name is not changed
    assertThat(CantRename.class.getName()).isEqualTo(
        "com_facebook_redex_test_instr_CantRename".replace('_', '.'));
    // Make sure ForceRename's name /is/ changed
    assertThat(ForceRename.class.getName()).isNotEqualTo(
        "com_facebook_redex_test_instr_ForceRename".replace('_', '.'));
    // Make sure, specifically, that it seems to abide the renamer's conventions
    assertThat(ForceRename.class.getPackage().getName()).isEqualTo("X");
  }
}
