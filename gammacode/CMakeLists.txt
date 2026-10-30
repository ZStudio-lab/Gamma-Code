cmake_minimum_required(VERSION 3.25)

project(GammaCode VERSION 0.1.0 LANGUAGES CXX)

# RC for gamma.rc (app icon + version info compiled into gamma.exe).
if(WIN32)
  enable_language(RC)
endif()

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# --- third-party dependencies (everything else is fetched) -------------------
include(FetchContent)

FetchContent_Declare(
  json
  GIT_REPOSITORY https://github.com/nlohmann/json.git
  GIT_TAG        v3.11.3
  GIT_SHALLOW    TRUE
)
FetchContent_Declare(
  replxx
  GIT_REPOSITORY https://github.com/AmokHuginnsson/replxx.git
  GIT_TAG        master # BSD-licensed line editor, works on Windows
  GIT_SHALLOW    TRUE
)

# libcurl: prefer the system package (libcurl-dev); fall back to building
# curl from source so a clean machine needs nothing beyond a compiler.
find_package(CURL)
if(NOT CURL_FOUND)
  message(STATUS "system libcurl not found -- building curl via FetchContent")
  set(CURL_ENABLE_SSL OFF CACHE BOOL "" FORCE) # http://127.0.0.1 is all we need
  set(HTTP_ONLY        ON  CACHE BOOL "" FORCE)
  set(CURL_USE_LIBSSH2 OFF CACHE BOOL "" FORCE)
  set(BUILD_CURL_EXE   OFF CACHE BOOL "" FORCE)
  set(BUILD_SHARED_LIBS OFF CACHE BOOL "" FORCE)
  set(BUILD_TESTING    OFF CACHE BOOL "" FORCE)
  FetchContent_Declare(
    curl
    GIT_REPOSITORY https://github.com/curl/curl.git
    GIT_TAG        curl-8_11_1
    GIT_SHALLOW    TRUE
  )
  FetchContent_MakeAvailable(curl)
  if(NOT TARGET CURL::libcurl)
    add_library(CURL::libcurl ALIAS libcurl)
  endif()
endif()

FetchContent_MakeAvailable(json replxx)

# --- gamma executable ---------------------------------------------------------
add_executable(gamma
  main.cpp
  src/config/config.cpp
  src/config/sessions.cpp
  src/ollama/client.cpp
  src/agent/messages.cpp
  src/agent/formatter.cpp
  src/agent/loop.cpp
  src/tools/registry.cpp
  src/tools/fs.cpp
  src/tools/shell.cpp
  src/ui/render.cpp
  src/ui/launcher.cpp
)

if(WIN32)
  target_sources(gamma PRIVATE gamma.rc)
endif()

target_include_directories(gamma PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)
target_compile_definitions(gamma PRIVATE GAMMA_VERSION="${PROJECT_VERSION}")

target_link_libraries(gamma PRIVATE
  CURL::libcurl
  nlohmann_json::nlohmann_json
  replxx
)

if(MSVC)
  target_compile_options(gamma PRIVATE /W4 /WX)
else()
  target_compile_options(gamma PRIVATE -Wall -Wextra -Werror)
endif()

# --- tests (filled in at step 5) ----------------------------------------------
include(CTest)
