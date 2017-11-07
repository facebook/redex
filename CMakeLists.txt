cmake_minimum_required (VERSION 3.0.2)
project (redex)

if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE Release)
endif()

set(CMAKE_CXX_STANDARD 14)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_MODULE_PATH "${CMAKE_SOURCE_DIR}/cmake_modules" ${CMAKE_MODULE_PATH})
set(CMAKE_CXX_FLAGS "-Wall")
set(CMAKE_CXX_FLAGS_RELEASE "-O3")
set(CMAKE_CXX_FLAGS_DEBUG "-g")

find_package(Boost 1.56.0 REQUIRED COMPONENTS regex filesystem program_options iostreams)
find_package(JsonCpp 0.10.5 REQUIRED)
find_package(ZLIB REQUIRED)
find_package(Threads REQUIRED)

file(GLOB_RECURSE src
    "liblocator/*.cpp"
    "libredex/*.cpp"
    "libresource/*.cpp"
    "opt/*.cpp"
    "util/Sha1.cpp"
    "tools/redex-all/*.cpp"
)

file(GLOB include
  "opt/*"
  "liblocator"
  "libredex"
  "libresource"
  "util"
)

add_executable (redex ${src})
include_directories(${Boost_INCLUDE_DIRS} ${ZLIB_INCLUDE_DIRS} ${JSONCPP_INCLUDE_DIRS}/jsoncpp ${include})
target_link_libraries(redex ${CMAKE_THREAD_LIBS_INIT} ${JSONCPP_LIBRARY} ${ZLIB_LIBRARIES} ${Boost_LIBRARIES})
target_compile_definitions(redex PRIVATE)
