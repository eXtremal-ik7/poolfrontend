include("cmake/HunterGate.cmake")
HunterGate(
    URL "https://github.com/eXtremal-ik7/hunter/archive/v0.23.260-with-fb.zip"
    SHA1 "6b07ca070eae553de0ae4d160a8f916983cee468"
)

cmake_minimum_required(VERSION 3.11)
project(poolfrontend)

set (CMAKE_CXX_STANDARD 17)
option(SANITIZER_ENABLED "Build with address sanitizer" OFF)
option(GPROF_ENABLED "Build with GNU profiler (use gprof ./exename -p > out.txt)" OFF)

if (SANITIZER_ENABLED)
  set (CMAKE_C_FLAGS "${CMAKE_C_FLAGS} -fsanitize=address")
  set (CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -fsanitize=address")
endif()

if (GPROF_ENABLED)
  set (CMAKE_C_FLAGS "${CMAKE_C_FLAGS} -pg")
  set (CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -pg")
endif()

set (CMAKE_MODULE_PATH ${CMAKE_MODULE_PATH}
  ${CMAKE_CURRENT_SOURCE_DIR}/cmake
)

set(CMAKE_DEBUG_POSTFIX d)
add_definitions(-DTBB_USE_EXCEPTIONS=0)

if(MSVC)
  add_definitions(
    -D_CRT_SECURE_NO_DEPRECATE
    -D_CRT_SECURE_NO_WARNINGS
    -D_CRT_NONSTDC_NO_DEPRECATE
    -D__STDC_LIMIT_MACROS
    -D__STDC_FORMAT_MACROS
    -DNOMINMAX
    -DNOGDI
  )

  set (CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} /GR-")
  include_directories(${CMAKE_SOURCE_DIR}/msvc/include)
else(MSVC)
  set (CMAKE_C_FLAGS "${CMAKE_C_FLAGS} -Wall -Wextra")
  set (CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -Wall -Wextra -fno-rtti")
  set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -mcx16")
endif(MSVC)

include(${CMAKE_SOURCE_DIR}/cmake/ProjectPoolcore.cmake)

find_package(OpenSSL REQUIRED)
find_package(RapidJSON CONFIG REQUIRED)
find_package(RocksDB CONFIG REQUIRED)
find_package(Protobuf CONFIG REQUIRED)

if ("${CMAKE_CXX_COMPILER_ID}" STREQUAL "GNU")
  set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -pthread")
endif()

include_directories(
  ${CMAKE_SOURCE_DIR}/include
  ${poolcore_SOURCE_DIR}/src/include
  ${poolcore_BINARY_DIR}
  ${libp2p_SOURCE_DIR}/include
  ${libp2p_BINARY_DIR}/include
)

add_executable(poolfrontend 
  config.cpp
  main.cpp
  http.cpp
)

target_link_libraries(poolfrontend
  poolcore
  poolinstances
  blockmaker
  poolcommon
  loguru
  asyncio-0.5
  asyncioextras-0.5
  p2putils
  RapidJSON::rapidjson
  OpenSSL::SSL
  OpenSSL::Crypto
  RocksDB::rocksdb
  protobuf::libprotobuf
)

# OS-specific configuration
if (WIN32)
  target_link_libraries(poolfrontend ws2_32 mswsock)
elseif(CMAKE_SYSTEM_NAME STREQUAL "Linux")
  target_link_libraries(poolfrontend dl)
endif()

# Compiler-specific configuration
if (CMAKE_CXX_COMPILER_ID STREQUAL "GNU")
  target_link_libraries(poolfrontend
    atomic
    pthread
    stdc++fs
  )
endif()
