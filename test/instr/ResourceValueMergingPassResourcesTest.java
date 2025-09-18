/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

package com.facebook.redextest;

import static org.junit.Assert.assertEquals;
import static org.junit.Assert.assertTrue;

import android.content.Context;
import android.content.res.Resources;
import android.content.res.TypedArray;
import android.util.TypedValue;
import androidx.test.platform.app.InstrumentationRegistry;
import org.junit.Before;
import org.junit.Test;

public class ResourceValueMergingPassResourcesTest {

  private Resources resources;
  private Resources.Theme theme;

  private static final int ATTR_ELEVATION = 0x10100d4;
  private static final int ATTR_PADDING = 0x10100d5;
  private static final int ATTR_LAYOUT_MARGIN = 0x10100f6;
  private static final int ATTR_TEXT_COLOR = 0x1010098;
  private static final int ATTR_COLOR_PRIMARY = 0x1010433;
  private static final int ATTR_COLOR_ACCENT = 0x1010435;
  private static final int ATTR_WINDOW_ACTION_BAR = 0x1010056;
  private static final int ATTR_WINDOW_NO_TITLE = 0x10102cd;

  @Before
  public void setUp() {
    Context context = InstrumentationRegistry.getInstrumentation().getTargetContext();
    resources = context.getResources();
    theme = resources.newTheme();
  }

  @Test
  public void testStyleAttributesWithCorrectValues() {
    checkStyleAttribute("CardCompact", ATTR_PADDING, "8px");
    checkStyleAttribute("CardCompact", ATTR_LAYOUT_MARGIN, "4px");
    checkStyleAttribute("CardHighlight1", ATTR_PADDING, "24px");
    checkStyleAttribute("CardHighlight2", ATTR_LAYOUT_MARGIN, "8px");
    checkStyleAttribute("AppTheme.Light", ATTR_TEXT_COLOR, "#212121");
    checkStyleAttribute("AppTheme.Light.Blue", ATTR_COLOR_PRIMARY, "#2196F3");
    checkStyleAttribute("AppTheme.Light.Blue", ATTR_COLOR_ACCENT, "#03A9F4");
  }

  private void checkStyleAttribute(String styleName, int attributeId, String expectedValue) {
    try {
      int styleId = resources.getIdentifier(styleName, "style", "com.fb.styles");
      if (styleId == 0) {
        if (expectedValue != null) {
          assertTrue("Style " + styleName + " not found", false);
        }
        return;
      }

      theme.applyStyle(styleId, true);
      TypedArray ta = theme.obtainStyledAttributes(styleId, new int[]{attributeId});
      TypedValue tv = new TypedValue();
      boolean hasValue = ta.getValue(0, tv);
      String attrName = getAttributeName(attributeId);

      if (hasValue) {
        if (tv.type == TypedValue.TYPE_DIMENSION) {
          float value = ta.getDimension(0, 0);
          float expectedPx = Float.parseFloat(expectedValue.replaceAll("[^\\d.]", ""));
          assertTrue("Style " + styleName + " attribute " + attrName +
                    " value " + value + "px doesn't match expected " + expectedPx + "px",
                    Math.abs(value - expectedPx) < 0.5f);
        } else if (tv.type == TypedValue.TYPE_INT_BOOLEAN) {
          boolean value = ta.getBoolean(0, false);
          assertEquals("Style " + styleName + " attribute " + attrName + " value doesn't match",
                      expectedValue, String.valueOf(value));
        } else if (tv.type >= TypedValue.TYPE_FIRST_COLOR_INT && tv.type <= TypedValue.TYPE_LAST_COLOR_INT) {
          int color = ta.getColor(0, 0);
          String actualValue = String.format("#%08X", color).toLowerCase();
          if (actualValue.startsWith("#ff")) {
            actualValue = "#" + actualValue.substring(3);
          }

          String normalizedExpected = expectedValue.toLowerCase();
          if (normalizedExpected.startsWith("#ff")) {
            normalizedExpected = "#" + normalizedExpected.substring(3);
          }

          assertEquals("Style " + styleName + " attribute " + attrName + " color doesn't match",
                      normalizedExpected, actualValue);
        } else {
          String actualValue = ta.getString(0);
          assertEquals("Style " + styleName + " attribute " + attrName + " value doesn't match",
                      expectedValue, actualValue);
        }
      } else if (expectedValue != null) {
        assertTrue("Style " + styleName + " attribute " + attrName +
                  " does not exist but was expected to have value: " + expectedValue, false);
      }

      ta.recycle();
    } catch (Resources.NotFoundException e) {
      if (expectedValue != null) {
        assertTrue("Style " + styleName + " not found", false);
      }
    }
  }

  private String getAttributeName(int attributeId) {
    switch (attributeId) {
      case ATTR_ELEVATION: return "android:elevation";
      case ATTR_PADDING: return "android:padding";
      case ATTR_LAYOUT_MARGIN: return "android:layout_margin";
      case ATTR_TEXT_COLOR: return "android:textColor";
      case ATTR_COLOR_PRIMARY: return "android:colorPrimary";
      case ATTR_COLOR_ACCENT: return "android:colorAccent";
      case ATTR_WINDOW_ACTION_BAR: return "android:windowActionBar";
      case ATTR_WINDOW_NO_TITLE: return "android:windowNoTitle";
      default: return "0x" + Integer.toHexString(attributeId);
    }
  }
}
