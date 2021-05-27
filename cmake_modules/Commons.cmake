# Copyright (c) Facebook, Inc. and its affiliates.
#
# This source code is licensed under the MIT license found in the
# LICENSE file in the root directory of this source tree.

function(print_dirs var name)
    foreach (path ${var})
        message(STATUS "${name}: " ${path})
    endforeach ()
endfunction()

macro(set_common_cxx_flags_for_redex)
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
        if (UNIX AND NOT APPLE AND ENABLE_STATIC)
            set(CMAKE_EXE_LINKER_FLAGS "${CMAKE_EXE_LINKER_FLAGS} -static-libgcc -static-libstdc++")
        elseif (MINGW)
            set(CMAKE_EXE_LINKER_FLAGS "${CMAKE_EXE_LINKER_FLAGS} -static -static-libgcc -static-libstdc++")
        endif ()

        set(CMAKE_CXX_FLAGS_DEBUG "${CMAKE_CXX_FLAGS_DEBUG} -g -Wall -pthread")
        set(COMMON_CXX_FLAGS_NODBG "-O3 -UNDEBUG")
        set(CMAKE_CXX_FLAGS_RELEASE "${CMAKE_CXX_FLAGS_RELEASE} ${COMMON_CXX_FLAGS_NODBG} -pthread")
        set(CMAKE_CXX_FLAGS_RELWITHDEBINFO "${CMAKE_CXX_FLAGS_RELWITHDEBINFO} ${COMMON_CXX_FLAGS_NODBG} -pthread")
        set(CMAKE_CXX_FLAGS_MINSIZEREL "${CMAKE_CXX_FLAGS_MINSIZEREL} ${COMMON_CXX_FLAGS_NODBG} -pthread")
    endif ()
endmacro()

macro(add_dependent_packages_for_redex)

    message("-- ENABLE_STATIC ${ENABLE_STATIC}")

    if(ENABLE_STATIC)
        set(Boost_USE_STATIC_LIBS ON)
        if(NOT APPLE)
            set(Boost_USE_STATIC_RUNTIME ON)
        endif()
        set(Boost_USE_MULTITHREADED ON)
    endif()

    find_package(Boost 1.71.0 REQUIRED COMPONENTS system regex filesystem program_options iostreams thread)
    print_dirs("${Boost_INCLUDE_DIRS}" "Boost_INCLUDE_DIRS")
    print_dirs("${Boost_LIBRARIES}" "Boost_LIBRARIES")

    find_package(JsonCpp 0.10.5 REQUIRED)
    print_dirs("${JSONCPP_INCLUDE_DIRS}" "JSONCPP_INCLUDE_DIRS")

    if(ENABLE_STATIC)
        if (MINGW AND (EXISTS "${JSONCPP_INCLUDE_DIRS}/../lib/libjsoncpp.a"))
            # MSYS's JSONCPP only gets detected as shared, leaving the value
            # above empty. Hardcode as a workaround.
            set(REDEX_JSONCPP_LIBRARY ${JSONCPP_INCLUDE_DIRS}/../lib/libjsoncpp.a)
        elseif (JSONCPP_LIBRARY_STATIC)
            set(REDEX_JSONCPP_LIBRARY ${JSONCPP_LIBRARY_STATIC})
        else ()
            message(FATAL_ERROR "Could not find a static library for JsonCpp.")
        endif()
    else()
        set(REDEX_JSONCPP_LIBRARY ${JSONCPP_LIBRARY})
    endif()

    if (APPLE AND (NOT ZLIB_HOME))
        #Static library is not installed on default path in MacOS because it conflicts with Xcode Version
        set(ZLIB_HOME "/usr/local/opt/zlib/")
    endif ()

    find_package(Zlib REQUIRED)

    print_dirs(${ZLIB_STATIC_LIB} "ZLIB_STATIC_LIB")
    print_dirs(${ZLIB_SHARED_LIB} "ZLIB_SHARED_LIB")

    if (ENABLE_STATIC)
        set(REDEX_ZLIB_LIBRARY ${ZLIB_STATIC_LIB})
    else ()
        set(REDEX_ZLIB_LIBRARY ${ZLIB_SHARED_LIB})
    endif ()

    print_dirs("${ZLIB_INCLUDE_DIRS}" "ZLIB_INCLUDE_DIRS")
    print_dirs("${REDEX_ZLIB_LIBRARY}" "REDEX_ZLIB_LIBRARY")

endmacro()

function(set_link_whole target_name lib_name)
    set(libpath "${LIBRARY_OUTPUT_DIRECTORY}${CMAKE_STATIC_LIBRARY_PREFIX}${lib_name}${CMAKE_STATIC_LIBRARY_SUFFIX}")
    message(STATUS "${target_name} will link ${libpath} wholly")
    string(TOLOWER ${CMAKE_CXX_COMPILER_ID} compiler_id)
    if (${compiler_id} MATCHES ".*clang")
        if (APPLE)
            set_property(TARGET ${target_name} APPEND_STRING PROPERTY LINK_FLAGS "-Wl,-force_load ${libpath} ")
        else ()
            # On non-Apple platforms, we always assume that the GNU Linker is used
            set_property(TARGET ${target_name} APPEND_STRING PROPERTY LINK_FLAGS "-Wl,--whole-archive ${libpath} -Wl,--no-whole-archive")
        endif ()
    elseif (${compiler_id} STREQUAL "gnu")
        set_property(TARGET ${target_name} APPEND_STRING PROPERTY LINK_FLAGS "-Wl,--whole-archive ${libpath} -Wl,--no-whole-archive")
    elseif (${compiler_id} STREQUAL "msvc")
        set_property(TARGET ${target_name} APPEND_STRING PROPERTY LINK_FLAGS "/WHOLEARCHIVE:${libpath} ")
    else ()
        message(WARNING "Unknown compiler: skipping whole link option: " ${compiler_id})
    endif ()
endfunction()
