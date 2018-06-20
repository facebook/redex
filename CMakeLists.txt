cmake_minimum_required(VERSION 3.0.2)
project("Redex")

set(CMAKE_MODULE_PATH "${CMAKE_SOURCE_DIR}/cmake_modules" ${CMAKE_MODULE_PATH})
include(Commons)

if (NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Release)
endif ()

set_common_cxx_flags_for_redex()
add_dependent_packages_for_redex()

file(GLOB includes
        "libredex"
        "service/*"
        "opt/*"
        "util"
        "liblocator"
        "libresource"
        "shared"
        )

include_directories(
        ${Boost_INCLUDE_DIRS}
        ${JSONCPP_INCLUDE_DIRS}
        ${ZLIB_INCLUDE_DIRS}
        ${includes})

file(GLOB_RECURSE redex_srcs
        "libredex/*.cpp"
        "libredex/*.h"
        "service/*.cpp"
        "service/*.h"
        "opt/*.cpp"
        "opt/*.h"
        "util/Sha1.*"
        "shared/*.cpp"
        "shared/*.h"
        "liblocator/*.cpp"
        "liblocator/*.h"
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
        )

add_executable(redex-all ${redex_all_srcs})

target_link_libraries(redex-all
        ${Boost_LIBRARIES}
        ${JSONCPP_LIBRARY}
        ${ZLIB_LIBRARIES}
        redex
        resource
        )

target_compile_definitions(redex-all PRIVATE)

set_link_whole(redex-all redex)
