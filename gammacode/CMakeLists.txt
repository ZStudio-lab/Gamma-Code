cmake_minimum_required(VERSION 3.25)

project(GammaCode VERSION 0.1.3 LANGUAGES CXX)

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
  # HTTPS is needed for exactly one thing: downloading the official Ollama
  # installer on first run. Everything else stays on http://127.0.0.1.
  set(CURL_ENABLE_SSL ON CACHE BOOL "" FORCE)
  if(WIN32)
    # Schannel is built into Windows — no OpenSSL dependency to ship.
    set(CURL_USE_SCHANNEL ON CACHE BOOL "" FORCE)
  endif()
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

# ── WebView2 SDK (native clickable launcher) ──────────────────────────────────
# The launcher window embeds Edge via WebView2; the loader is statically
# linked from the official NuGet package. If the download fails, gamma still
# builds and the console launcher is used instead.
if(WIN32)
  set(GAMMA_WEBVIEW2_DIR "${CMAKE_BINARY_DIR}/_deps/webview2")
  set(GAMMA_WEBVIEW2_HEADER
      "${GAMMA_WEBVIEW2_DIR}/build/native/include/WebView2.h")
  set(GAMMA_WEBVIEW2_LIB
      "${GAMMA_WEBVIEW2_DIR}/build/native/x64/WebView2LoaderStatic.lib")
  if(NOT EXISTS "${GAMMA_WEBVIEW2_HEADER}")
    set(GAMMA_WEBVIEW2_NUPKG "${GAMMA_WEBVIEW2_DIR}/Microsoft.Web.WebView2.nupkg")
    file(MAKE_DIRECTORY "${GAMMA_WEBVIEW2_DIR}")
    file(DOWNLOAD "https://www.nuget.org/api/v2/package/Microsoft.Web.WebView2"
         "${GAMMA_WEBVIEW2_NUPKG}" STATUS webview2_download)
    list(GET webview2_download 0 webview2_code)
    if(webview2_code EQUAL 0)
      file(ARCHIVE_EXTRACT INPUT "${GAMMA_WEBVIEW2_NUPKG}"
           DESTINATION "${GAMMA_WEBVIEW2_DIR}")
    else()
      message(STATUS "WebView2 SDK download failed — console launcher only")
    endif()
  endif()
  if(EXISTS "${GAMMA_WEBVIEW2_HEADER}" AND EXISTS "${GAMMA_WEBVIEW2_LIB}")
    if(NOT TARGET WebView2Loader)
      add_library(WebView2Loader STATIC IMPORTED GLOBAL)
      set_target_properties(WebView2Loader PROPERTIES IMPORTED_LOCATION
                            "${GAMMA_WEBVIEW2_LIB}")
    endif()
    message(STATUS "WebView2 SDK found — native clickable launcher enabled")
  else()
    message(STATUS "WebView2 SDK missing — console launcher only")
  endif()
endif()

# --- gamma executable ---------------------------------------------------------
add_executable(gamma
  main.cpp
  src/config/config.cpp
  src/config/sessions.cpp
  src/ollama/client.cpp
  src/setup/ollama_setup.cpp
  src/agent/messages.cpp
  src/agent/formatter.cpp
  src/agent/loop.cpp
  src/tools/registry.cpp
  src/tools/fs.cpp
  src/tools/shell.cpp
  src/tools/ask.cpp
  src/tools/web.cpp
  src/ui/render.cpp
  src/ui/theme.cpp
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

if(TARGET WebView2Loader)
  target_sources(gamma PRIVATE src/ui/webview_launcher.cpp)
  set_source_files_properties(src/ui/webview_launcher.cpp PROPERTIES
    COMPILE_DEFINITIONS "UNICODE;_UNICODE"             # IDC_ARROW & friends resolve W
    COMPILE_OPTIONS "-Wno-unknown-pragmas"             # WebView2.h ships MSVC pragmas
  )
  target_include_directories(gamma PRIVATE
    "${CMAKE_BINARY_DIR}/_deps/webview2/build/native/include")
  target_compile_definitions(gamma PRIVATE GAMMA_HAS_WEBVIEW=1
    GAMMA_WEBVIEW2_DLL_DIR="${CMAKE_BINARY_DIR}/_deps/webview2/build/native/x64")
  # The loader DLL is runtime-loaded (LoadLibrary + GetProcAddress): the
  # MSVC-built static lib uses the MSVC C++ ABI, which cannot link into a
  # MinGW binary. Shipping the DLL sidesteps the ABI entirely.
  add_custom_command(TARGET gamma POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "${CMAKE_BINARY_DIR}/_deps/webview2/build/native/x64/WebView2Loader.dll"
            $<TARGET_FILE_DIR:gamma>)
else()
  # stub source keeps the build green without the SDK
  target_sources(gamma PRIVATE src/ui/webview_stub.cpp)
endif()

if(MSVC)
  target_compile_options(gamma PRIVATE /W4 /WX)
else()
  target_compile_options(gamma PRIVATE -Wall -Wextra -Werror)
endif()

# --- tests (filled in at step 5) ----------------------------------------------
include(CTest)
