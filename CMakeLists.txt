cmake_minimum_required(VERSION 3.0.2)
project("Redex")

if (NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Release)
endif ()

set(CMAKE_CXX_STANDARD 14)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_MODULE_PATH "${CMAKE_SOURCE_DIR}/cmake_modules" ${CMAKE_MODULE_PATH})

if (MSVC)
    set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} /EHsc /W1 /D_SCL_SECURE_NO_WARNINGS")
    set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} /EHsc /W1 /D_SCL_SECURE_NO_WARNINGS")
    set(CMAKE_CXX_FLAGS_RELEASE "${CMAKE_CXX_FLAGS_RELEASE} /MT")
    set(CMAKE_CXX_FLAGS_DEBUG "${CMAKE_CXX_FLAGS_DEBUG} /MTd")
else ()
    set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -Wall -rdynamic")
    set(CMAKE_CXX_FLAGS_RELEASE "-O3")
    set(CMAKE_CXX_FLAGS_DEBUG "-g")
endif ()

function(print_dirs var name)
    foreach (path ${var})
        message(STATUS "${name}: " ${path})
    endforeach ()
endfunction()

find_package(Boost 1.56.0 REQUIRED COMPONENTS regex filesystem program_options iostreams)
print_dirs("${Boost_INCLUDE_DIRS}" "Boost_INCLUDE_DIRS")
print_dirs("${Boost_LIBRARIES}" "Boost_LIBRARIES")

set(Boost_USE_STATIC_LIBS ON)
set(Boost_USE_STATIC_RUNTIME ON)
set(Boost_USE_MULTITHREADED ON)

find_package(JsonCpp 0.10.5 REQUIRED)
print_dirs("${JSONCPP_INCLUDE_DIRS}" "JSONCPP_INCLUDE_DIRS")
print_dirs("${JSONCPP_LIBRARY}" "JSONCPP_LIBRARY")

if (NOT MSVC)
    set(JSONCPP_INCLUDE_DIRS "${JSONCPP_INCLUDE_DIRS}/jsoncpp")
endif ()

find_package(ZLIB REQUIRED)
print_dirs("${ZLIB_INCLUDE_DIRS}" "ZLIB_INCLUDE_DIRS")
print_dirs("${ZLIB_LIBRARIES}" "ZLIB_LIBRARIES")

file(GLOB includes
        "libredex"
        "opt/*"
        "util"
        "liblocator"
        "libresource"
        )

include_directories(
        ${Boost_INCLUDE_DIRS}
        ${JSONCPP_INCLUDE_DIRS}
        ${ZLIB_INCLUDE_DIRS}
        ${includes})

file(GLOB_RECURSE redex_srcs
        "libredex/*.cpp"
        "libredex/*.h"
        "opt/*.cpp"
        "opt/*.h"
        "util/Sha1.*"
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

file(GLOB_RECURSE redex_all_srcs
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

set_link_whole(redex-all redex)

if (EXISTS ${CMAKE_CURRENT_SOURCE_DIR}/facebook)
    add_subdirectory(${CMAKE_CURRENT_SOURCE_DIR}/facebook/mortirolo)
endif ()
