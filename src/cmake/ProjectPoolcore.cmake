include(FetchContent)

FetchContent_Declare(
  poolcore
  GIT_REPOSITORY https://github.com/eXtremal-ik7/poolcore.git
  GIT_TAG        version/0.4
  GIT_SHALLOW    1
  SOURCE_DIR     ${CMAKE_SOURCE_DIR}/../dependencies/poolcore
)

FetchContent_GetProperties(poolcore)
if (NOT poolcore_POPULATED)
  FetchContent_Populate(poolcore)
  add_subdirectory(${poolcore_SOURCE_DIR}/src ${poolcore_BINARY_DIR} EXCLUDE_FROM_ALL)
endif()
