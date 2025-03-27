/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

package com.facebook.redex.examples.proguardexample

import android.os.Bundle
import androidx.activity.ComponentActivity
import androidx.activity.compose.setContent
import androidx.activity.enableEdgeToEdge
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.foundation.layout.padding
import androidx.compose.material3.Scaffold
import androidx.compose.material3.Text
import androidx.compose.ui.Modifier
import com.facebook.redex.examples.proguardexample.ui.theme.MyApplicationTheme

class MainActivity : ComponentActivity() {
  override fun onCreate(savedInstanceState: Bundle?) {
    super.onCreate(savedInstanceState)
    enableEdgeToEdge()

    val alphaObject = Alpha()
    val ltuae = alphaObject.doubleWombat()

    val text =
        try {
          val greek = Class.forName("com.facebook.redex.examples.proguardexample.Greek")
          if (greek.isInstance(alphaObject)) {
            "Alpha is an instance of Greek"
          } else {
            "Alpha is not an instance of Greek"
          }
        } catch (e: ClassNotFoundException) {
          "ERROR: Greek interface not found"
        }

    setContent {
      MyApplicationTheme {
        Scaffold(modifier = Modifier.fillMaxSize()) { innerPadding ->
          Column(modifier = Modifier.padding(innerPadding)) {
            Text(text = "The answer is $ltuae!")
            Text(text = text)
          }
        }
      }
    }
  }
}
