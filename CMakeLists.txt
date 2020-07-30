# Copyright (c) Facebook, Inc. and its affiliates.
#
# This source code is licensed under the MIT license found in the
# LICENSE file in the root directory of this source tree.

cmake_minimum_required(VERSION 3.0.2)
project("Redex")

set(CMAKE_MODULE_PATH "${CMAKE_SOURCE_DIR}/cmake_modules" ${CMAKE_MODULE_PATH})
include(Commons)

if (NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Release)
endif ()

if(NOT BUILD_TYPE)
    set(BUILD_TYPE Shared)
endif ()
if (MINGW)
    set(BUILD_TYPE Static)
endif ()

if(BUILD_TYPE STREQUAL Static)
    set(ENABLE_STATIC ON CACHE BOOL "" FORCE)
    if (MINGW)
        set(STATIC_LINK_FLAG "-static")
    else (MINGW)
        set(STATIC_LINK_FLAG "")
    endif (MINGW)
elseif (BUILD_TYPE STREQUAL Shared)
    set(ENABLE_STATIC OFF CACHE BOOL "" FORCE)
    set(STATIC_LINK_FLAG "")
endif ()

set_common_cxx_flags_for_redex()
add_dependent_packages_for_redex()

file(GLOB includes
        "analysis"
        "libredex"
        "service/*"
        "opt/*"
        "util"
        "liblocator"
        "libresource"
        "shared"
        "sparta/include"
        "tools/common"
        )

include_directories(
        ${Boost_INCLUDE_DIRS}
        ${JSONCPP_INCLUDE_DIRS}
        ${ZLIB_INCLUDE_DIRS}
        ${includes})

file(GLOB_RECURSE redex_srcs
        "analysis/max-depth/*.cpp"
        "analysis/max-depth/*.h"
        "analysis/ip-reflection-analysis/*.cpp"
        "analysis/ip-reflection-analysis/*.h"
        "libredex/*.cpp"
        "libredex/*.h"
        "service/*.cpp"
        "service/*.h"
        "opt/*.cpp"
        "opt/*.h"
        "util/CommandProfiling.cpp"
        "util/CommandProfiling.h"
        "util/JemallocUtil.cpp"
        "util/JemallocUtil.h"
        "util/Sha1.cpp"
        "util/Sha1.h"
        "shared/DexDefs.cpp"
        "shared/DexDefs.h"
        "shared/file-utils.cpp"
        "shared/file-utils.h"
        "liblocator/locator.cpp"
        "liblocator/locator.h"
        )

add_library(redex STATIC ${redex_srcs})

file(GLOB_RECURSE tool_srcs
        "tools/tool/*.cpp"
        "tools/tool/*.h"
        )

add_library(tool STATIC ${tool_srcs})

file(GLOB_RECURSE resource_srcs
        "libresource/*.cpp"
        "libresource/*.h"
        )

add_library(resource STATIC ${resource_srcs})

file(GLOB redex_all_srcs
        "tools/redex-all/*.cpp"
        "tools/redex-all/*.h"
        "tools/common/ToolsCommon.cpp"
        "tools/common/ToolsCommon.h"
        )

add_executable(redex-all ${redex_all_srcs})

if (MINGW)
    set(MINGW_EXTRA_LIBS -Wl,-Bstatic ws2_32)
else (MINGW)
    set(MINGW_EXTRA_LIBS "")
endif (MINGW)

target_link_libraries(redex-all
        ${STATIC_LINK_FLAG}
        ${Boost_LIBRARIES}
        ${REDEX_JSONCPP_LIBRARY}
        ${REDEX_ZLIB_LIBRARY}
        ${CMAKE_DL_LIBS}
        redex
        resource
        ${MINGW_EXTRA_LIBS}
        )

target_compile_definitions(redex-all PRIVATE)

set_link_whole(redex-all redex)
