cmake_minimum_required(VERSION 3.16)
project(sysLens VERSION 0.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)
if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE Release)
endif()

include(FetchContent)

# Prefer system packages (Arch: pacman -S ftxui nlohmann-json curl), otherwise fetch.
find_package(ftxui QUIET)
if(NOT ftxui_FOUND)
  FetchContent_Declare(ftxui
    GIT_REPOSITORY https://github.com/ArthurSonzogni/FTXUI.git
    GIT_TAG v5.0.0)
  set(FTXUI_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
  set(FTXUI_BUILD_DOCS OFF CACHE BOOL "" FORCE)
  set(FTXUI_BUILD_TESTS OFF CACHE BOOL "" FORCE)
  set(FTXUI_ENABLE_INSTALL OFF CACHE BOOL "" FORCE)
  FetchContent_MakeAvailable(ftxui)
endif()

find_package(nlohmann_json QUIET)
if(NOT nlohmann_json_FOUND)
  FetchContent_Declare(json
    GIT_REPOSITORY https://github.com/nlohmann/json.git
    GIT_TAG v3.11.3)
  FetchContent_MakeAvailable(json)
endif()

option(SYSLENS_FETCH_CURL "Build libcurl from source if the system one is missing (no TLS; dev/CI only)" OFF)
find_package(CURL QUIET)
if(NOT CURL_FOUND AND SYSLENS_FETCH_CURL)
  FetchContent_Declare(curl
    GIT_REPOSITORY https://github.com/curl/curl.git
    GIT_TAG curl-8_10_1)
  set(BUILD_CURL_EXE OFF CACHE BOOL "" FORCE)
  set(BUILD_SHARED_LIBS OFF CACHE BOOL "" FORCE)
  set(CURL_USE_LIBPSL OFF CACHE BOOL "" FORCE)
  set(CURL_USE_OPENSSL OFF CACHE BOOL "" FORCE)
  set(CURL_ENABLE_SSL OFF CACHE BOOL "" FORCE)
  set(CURL_DISABLE_LDAP ON CACHE BOOL "" FORCE)
  set(USE_LIBIDN2 OFF CACHE BOOL "" FORCE)
  FetchContent_MakeAvailable(curl)
  if(NOT TARGET CURL::libcurl)
    add_library(CURL::libcurl ALIAS libcurl)
  endif()
elseif(NOT CURL_FOUND)
  message(FATAL_ERROR "libcurl not found. On Arch: sudo pacman -S curl")
endif()

add_executable(sysLens
  src/main.cpp
  src/sysinfo.cpp
  src/llm.cpp
  src/analyze.cpp
  src/format.cpp
  src/errors.cpp
  src/tui.cpp)

target_include_directories(sysLens PRIVATE src)
target_compile_options(sysLens PRIVATE -Wall -Wextra)
target_link_libraries(sysLens PRIVATE
  ftxui::screen ftxui::dom ftxui::component
  nlohmann_json::nlohmann_json
  CURL::libcurl
  pthread)

install(TARGETS sysLens RUNTIME DESTINATION bin)
