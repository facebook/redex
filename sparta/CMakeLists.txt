# Copyright (c) Meta Platforms, Inc. and affiliates.
#
# This source code is licensed under the MIT license found in the
# LICENSE file in the root directory of this source tree.

cmake_minimum_required(VERSION 3.0.2)
project("sparta")

include(CTest)
set(CMAKE_MODULE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/cmake_modules" ${CMAKE_MODULE_PATH})
include(Commons)

if (NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE Release)
endif ()

set_common_cxx_flags_for_sparta()
add_dependent_packages_for_sparta()

include_directories(${Boost_INCLUDE_DIRS})

###################################################
# Add sparta interface library
###################################################
add_library(sparta INTERFACE)

target_include_directories(sparta INTERFACE
  $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
  $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>
)

target_link_libraries(sparta INTERFACE ${Boost_LIBRARIES})

add_library(sparta::sparta ALIAS sparta)

###################################################
# install and export
###################################################
# Must use GNUInstallDirs to install libraries into correct
# locations on all platforms.
include(GNUInstallDirs)

# 'make install' to the correct locations (provided by GNUInstallDirs).
install(TARGETS sparta EXPORT sparta_target
  ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR}
  LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
  RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}  # This is for Windows
)
install(DIRECTORY include/ DESTINATION ${CMAKE_INSTALL_INCLUDEDIR})

install(EXPORT sparta_target
  FILE sparta_target.cmake
  DESTINATION "${CMAKE_INSTALL_LIBDIR}/cmake/sparta"
  NAMESPACE sparta::
)

# This makes the project importable from the build directory
export(TARGETS sparta
  FILE sparta_target.cmake
  NAMESPACE sparta::
)

# Generate CMake config file that can be used in other projects
include(CMakePackageConfigHelpers)
configure_package_config_file(
  "${CMAKE_CURRENT_SOURCE_DIR}/cmake_modules/sparta-config.cmake.in"
  "${CMAKE_CURRENT_BINARY_DIR}/sparta-config.cmake"
  INSTALL_DESTINATION "${CMAKE_INSTALL_LIBDIR}/cmake/sparta"
)
install(FILES
  "${CMAKE_CURRENT_BINARY_DIR}/sparta-config.cmake"
  DESTINATION "${CMAKE_INSTALL_LIBDIR}/cmake/sparta"
)

###################################################
# test
###################################################
if (BUILD_TESTING)
  add_subdirectory(test)
endif()
