cmake_minimum_required(VERSION 3.20)
project(OpenITC LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE Release)
endif()

find_package(PkgConfig REQUIRED)
pkg_check_modules(RTLSDR REQUIRED IMPORTED_TARGET librtlsdr)
pkg_check_modules(LIBZMQ REQUIRED IMPORTED_TARGET libzmq)

add_executable(openitc
  src/main.cpp
  src/rtl_source.cpp
  src/dsp.cpp
  src/decode.cpp
  src/itc.cpp
  src/parse.cpp
  src/repository.cpp
  src/engine.cpp
  src/native.cpp
)

target_link_libraries(openitc PRIVATE PkgConfig::RTLSDR PkgConfig::LIBZMQ ws2_32)
target_compile_options(openitc PRIVATE -Wall -Wextra)
