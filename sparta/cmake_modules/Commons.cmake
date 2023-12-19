# Copyright (c) Meta Platforms, Inc. and affiliates.
#
# This source code is licensed under the MIT license found in the
# LICENSE file in the root directory of this source tree.

function(print_dirs var name)
  foreach (path ${var})
    message(STATUS "${name}: " ${path})
  endforeach ()
endfunction()

macro(set_common_cxx_flags_for_sparta)
  message(STATUS "Using C++17")
  set(CMAKE_CXX_STANDARD 17)
  set(CMAKE_CXX_STANDARD_REQUIRED ON)
  set(CMAKE_CXX_EXTENSIONS OFF)
  if (MSVC)
    set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} /EHsc /W1 /D_SCL_SECURE_NO_WARNINGS")
    set(CMAKE_CXX_FLAGS_DEBUG "${CMAKE_CXX_FLAGS_DEBUG} /MTd")
    set(CMAKE_CXX_FLAGS_RELEASE "${CMAKE_CXX_FLAGS_RELEASE} /MT")
    set(CMAKE_CXX_FLAGS_RELWITHDEBINFO "${CMAKE_CXX_FLAGS_RELWITHDEBINFO} /MT")
    set(CMAKE_CXX_FLAGS_MINSIZEREL "${CMAKE_CXX_FLAGS_MINSIZEREL} /MT")
  else ()
    set(CMAKE_CXX_FLAGS_DEBUG "${CMAKE_CXX_FLAGS_DEBUG} -g -Wall")
    set(COMMON_CXX_FLAGS_NODBG, "-O3")
    set(CMAKE_CXX_FLAGS_RELEASE "${CMAKE_CXX_FLAGS_RELEASE} ${COMMON_CXX_FLAGS_NODBG}")
    set(CMAKE_CXX_FLAGS_RELWITHDEBINFO "${CMAKE_CXX_FLAGS_RELWITHDEBINFO} ${COMMON_CXX_FLAGS_NODBG}")
    set(CMAKE_CXX_FLAGS_MINSIZEREL "${CMAKE_CXX_FLAGS_MINSIZEREL} ${COMMON_CXX_FLAGS_NODBG}")
  endif ()
endmacro()

macro(add_dependent_packages_for_sparta)
  find_package(Boost 1.71.0 REQUIRED COMPONENTS thread)
  print_dirs("${Boost_INCLUDE_DIRS}" "Boost_INCLUDE_DIRS")
  print_dirs("${Boost_LIBRARIES}" "Boost_LIBRARIES")

  set(Boost_USE_STATIC_LIBS ON)
  set(Boost_USE_STATIC_RUNTIME ON)
  set(Boost_USE_MULTITHREADED ON)

  project(googletest-download NONE)

  # Download and unpack googletest at configure time
  configure_file(cmake_modules/gtest.cmake.in googletest-download/CMakeLists.txt)
  execute_process(COMMAND ${CMAKE_COMMAND} -G "${CMAKE_GENERATOR}" .
    RESULT_VARIABLE result
    WORKING_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR}/googletest-download
  )
  if(result)
    message(FATAL_ERROR "CMake step for googletest failed: ${result}")
  endif()
  execute_process(COMMAND ${CMAKE_COMMAND} --build .
    RESULT_VARIABLE result
    WORKING_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR}/googletest-download
  )
  if(result)
    message(FATAL_ERROR "Build step for googletest failed: ${result}")
  endif()

  # Prevent overriding the parent project's compiler/linker
  # settings on Windows
  set(gtest_force_shared_crt ON CACHE BOOL "" FORCE)

  # Add googletest directly to our build. This defines
  # the gtest and gtest_main targets.
  add_subdirectory(
    ${CMAKE_CURRENT_BINARY_DIR}/googletest-src
    ${CMAKE_CURRENT_BINARY_DIR}/googletest-build
    EXCLUDE_FROM_ALL
  )

  # The gtest/gtest_main targets carry header search path
  # dependencies automatically when using CMake 2.8.11 or
  # later. Otherwise we have to add them here ourselves.
  if (CMAKE_VERSION VERSION_LESS 2.8.11)
    include_directories("${gtest_SOURCE_DIR}/include")
  endif()
endmacro()

function(set_link_whole target_name lib_name)
  set(libpath "${LIBRARY_OUTPUT_DIRECTORY}${CMAKE_STATIC_LIBRARY_PREFIX}${lib_name}${CMAKE_STATIC_LIBRARY_SUFFIX}")
  message(STATUS "${target_name} will link ${libpath} wholly")
  string(TOLOWER ${CMAKE_CXX_COMPILER_ID} compiler_id)
  if (${compiler_id} MATCHES ".*clang")
    set_property(TARGET ${target_name} APPEND_STRING PROPERTY LINK_FLAGS "-Wl,-force_load ${libpath} ")
  elseif (${compiler_id} STREQUAL "gnu")
    set_property(TARGET ${target_name} APPEND_STRING PROPERTY LINK_FLAGS "-Wl,--whole-archive ${libpath} ")
  elseif (${compiler_id} STREQUAL "msvc")
    set_property(TARGET ${target_name} APPEND_STRING PROPERTY LINK_FLAGS "/WHOLEARCHIVE:${libpath} ")
  else ()
    message(WARNING "Unknown compiler: skipping whole link option: " ${compiler_id})
  endif ()
endfunction()
