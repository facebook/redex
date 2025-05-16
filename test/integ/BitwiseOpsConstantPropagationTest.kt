/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

@Suppress("HexColorValueUsage", "ClassNameDoesNotMatchFileName")
class TestRGBA {
  /** Extracts the green component from a 32-bit RGBA value. */
  inline fun extractGreenFromRGBA256(rgba: Int): Int {
    return (rgba ushr 16) and 0xFF
  }

  // Test >>>, |, & on int.
  public fun mainExtractGreen(baseColor: Int) {
    var rgba256 = baseColor or 0x00F10000
    rgba256 = extractGreenFromRGBA256(rgba256)
    if ((rgba256 and 0xF0) != 0) {
      print("8-bit deep green")
    } else {
      // This branch should be optimized out.
      print("8-bit light green")
    }
  }

  /** Extracts the green component from a 64-bit RGBA value. */
  inline fun extractGreenFromRGBA512(rgba: Long): Long {
    return (rgba ushr 32) and 0xFFFF
  }

  // Test >>>, |, & on long.
  public fun mainExtractGreen(baseColor: Long) {
    var rgba512: Long = baseColor or 0x0000010000000000L
    rgba512 = extractGreenFromRGBA512(rgba512)
    if ((rgba512 and 0xFF00L) != 0L) {
      print("16-bit deep green")
    } else {
      // This branch should be optimized out.
      print("16-bit light green")
    }
  }

  /** Tells if rgba has any non-red component has all. */
  inline fun hasNonRed(rgba: Int): Boolean {
    return (rgba shl 8) != 0
  }

  // Test <<, & on int.
  public fun mainHasNonRed(baseColor: Int) {
    val onlyLowerRed = baseColor and 0x0F000000
    if (hasNonRed(onlyLowerRed)) {
      // This branch should be optimized out.
      print("int onlyLowerRed has non-red")
    } else {
      print("int onlyLowerRed has no non-red")
    }

    val onlyLowerAlpha = baseColor and 0xF
    // Neither branch should be optimized out.
    if (hasNonRed(onlyLowerAlpha)) {
      print("int onlyLowerAlpha has non-red")
    } else {
      print("int onlyLowerAlpha has no non-red")
    }
  }

  /** Tells if rgba has any non-red component has all. */
  inline fun hasNonRed(rgba: Long): Boolean {
    return (rgba shl 16) != 0L
  }

  // Test <<, & on long.
  public fun mainHasNonRed(baseColor: Long) {
    val onlyLowerRed = baseColor and 0x00FF000000000000L
    if (hasNonRed(onlyLowerRed)) {
      // This branch should be optimized out.
      print("long onlyLowerRed has non-red")
    } else {
      print("long onlyLowerRed has no non-red")
    }

    val onlyLowerAlpha = baseColor and 0xFFL
    // Neither branch should be optimized out.
    if (hasNonRed(onlyLowerAlpha)) {
      print("long onlyLowerAlpha has non-red")
    } else {
      print("long onlyLowerAlpha has no non-red")
    }
  }

  inline fun invert(rgba: Int): Int {
    return rgba.inv()
  }

  inline fun invertAlpha(rgba: Int): Int {
    return rgba xor 0xFF
  }

  // Test ~, ^, & on int
  public fun mainInvert(baseColor: Int) {
    val alphaless = baseColor and 0xFF.inv()
    var inverted = invert(alphaless) // inverted lowest 8 bits are 1s
    if (inverted == 0) {
      // This branch should be optimized out
      print("int alphaless inverted is zero")
    } else {
      print("int alphaless inverted is not zero")
    }

    // Neither branch should be optimized out
    if (inverted == 0xFF) {
      print("int alphaless inverted is 0xFF")
    } else {
      print("int alphaless inverted is not 0xFF")
    }

    inverted = invertAlpha(inverted) // inverted lowest 8 bits are 0s
    // Neither branch should be optimized out
    if (inverted == 0) {
      print("int alphaless inverted twice is zero")
    } else {
      print("int alphaless inverted twice is not zero")
    }

    if (inverted == 0xFF) {
      // This branch should be optimized out
      print("int alphaless inverted twice is 0xFF")
    } else {
      print("int alphaless inverted twice is not 0xFF")
    }
  }

  inline fun invert(rgba: Long): Long {
    return rgba.inv()
  }

  inline fun invertAlpha(rgba: Long): Long {
    return rgba xor 0xFFFFL
  }

  // Test ~, & on long
  public fun mainInvert(baseColor: Long) {
    val alphaless = baseColor and 0xFFFFL.inv()
    var inverted = invert(alphaless) // inverted lowest 16 bits are 1s
    if (inverted == 0L) {
      // This branch should be optimized out
      print("long alphaless inverted is zero")
    } else {
      print("long alphaless inverted is not zero")
    }

    // Neither branch should be optimized out
    if (inverted == 0xFFFFL) {
      print("long alphaless inverted is 0xFFFF")
    } else {
      print("long alphaless inverted is not 0xFFFF")
    }

    inverted = invertAlpha(inverted) // inverted lowest 16 bits are 0s
    // Neither branch should be optimized out
    if (inverted == 0L) {
      print("long alphaless inverted twice is zero")
    } else {
      print("long alphaless inverted twice is not zero")
    }

    if (inverted == 0xFFL) {
      // This branch should be optimized out
      print("long alphaless inverted twice is 0xFFFF")
    } else {
      print("long alphaless inverted twice is not 0xFFFF")
    }
  }
}
