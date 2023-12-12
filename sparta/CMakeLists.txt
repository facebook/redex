# Copyright (c) Meta Platforms, Inc. and affiliates.
#
# This source code is licensed under the MIT license found in the
# LICENSE file in the root directory of this source tree.

cmake_minimum_required(VERSION 3.0.2)
project("sparta")

set(CMAKE_MODULE_PATH "${CMAKE_SOURCE_DIR}/cmake_modules" ${CMAKE_MODULE_PATH})
include(Commons)

if (NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Release)
endif ()

set_common_cxx_flags_for_sparta()
add_dependent_packages_for_sparta()

include_directories(
        ${Boost_INCLUDE_DIRS}
        )

###################################################
# Add sparta interface library
###################################################
add_library(sparta INTERFACE)

target_include_directories(sparta INTERFACE
      $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
      $<INSTALL_INTERFACE:include>
      )

target_link_libraries(sparta INTERFACE ${Boost_LIBRARIES})

###################################################
# install and export
###################################################
# Must use GNUInstallDirs to install libraries into correct
# locations on all platforms.
include(GNUInstallDirs)

# 'make install' to the correct locations (provided by GNUInstallDirs).
install(TARGETS sparta EXPORT sparta_target
    ARCHIVE  DESTINATION ${CMAKE_INSTALL_LIBDIR}
    LIBRARY  DESTINATION ${CMAKE_INSTALL_LIBDIR}
    RUNTIME  DESTINATION ${CMAKE_INSTALL_BINDIR})  # This is for Windows
install(DIRECTORY include/ DESTINATION ${CMAKE_INSTALL_INCLUDEDIR})

install(EXPORT sparta_target DESTINATION cmake)

# This makes the project importable from the build directory
export(TARGETS sparta FILE sparta_target.cmake)

###################################################
# test
###################################################
file(GLOB test
        "test/*.cpp"
        )

include(CTest)
# ${test} contains all paths to the test cpps
foreach(testfile ${test})
    # ${testfile} is in the format of test/SomeTest.cpp
    string(REPLACE ".cpp" "_test" no_ext_name ${testfile})
    # ${no_ext_name} is in the format of test/SomeTest_test
    get_filename_component(test_bin ${no_ext_name} NAME)
    # ${test_bin} is in the format of SomeTest_test
    add_executable(${test_bin} ${testfile})
    target_link_libraries(${test_bin} PRIVATE sparta gmock_main)
    add_test(NAME ${testfile} COMMAND ${test_bin})
endforeach()

