# Copyright (c) Meta Platforms, Inc. and affiliates.
#
# This source code is licensed under the MIT license found in the
# LICENSE file in the root directory of this source tree.

file(GLOB test "*.cpp")

foreach(testfile ${test})
  # ${testfile} is in the format of SomeTest.cpp
  string(REPLACE ".cpp" "_test" no_ext_name ${testfile})
  # ${no_ext_name} is in the format of SomeTest_test
  get_filename_component(test_bin ${no_ext_name} NAME)
  # ${test_bin} is in the format of SomeTest_test
  add_executable(${test_bin} ${testfile})
  target_link_libraries(${test_bin} PRIVATE sparta gmock_main)
  add_test(NAME ${testfile} COMMAND ${test_bin})
endforeach()
