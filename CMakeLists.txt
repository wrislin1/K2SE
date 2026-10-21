cmake_minimum_required(VERSION 3.21)

# CMP0091: let CMAKE_MSVC_RUNTIME_LIBRARY select the CRT (needed for a static CRT).
cmake_policy(SET CMP0091 NEW)

project(K2SE VERSION 0.2.0 LANGUAGES CXX)

# --- hard 32-bit gate -------------------------------------------------------
# swkotor2.exe is a 32-bit process. A 64-bit build would silently produce a DLL
# the game can never load.
if(NOT CMAKE_SIZEOF_VOID_P EQUAL 4)
    message(FATAL_ERROR
        "K2SE must be built as 32-bit (x86).\n"
        "Configure with:  cmake -S . -B build -A Win32")
endif()

if(NOT MSVC)
    message(FATAL_ERROR "K2SE targets MSVC (Visual Studio 2022). Other toolchains are untested.")
endif()

# --- generated proxy sources ------------------------------------------------
# build/generate_proxy.py parses the real SysWOW64\version.dll export table and
# emits proxy_version_generated.cpp + version.def. Run it before configuring:
#     python build\generate_proxy.py
set(GENERATED_PROXY "${CMAKE_CURRENT_SOURCE_DIR}/src/proxy_version_generated.cpp")
set(GENERATED_DEF   "${CMAKE_CURRENT_SOURCE_DIR}/src/version.def")

if(NOT EXISTS "${GENERATED_PROXY}" OR NOT EXISTS "${GENERATED_DEF}")
    message(FATAL_ERROR
        "Generated proxy sources are missing.\n"
        "Run:  python build\\generate_proxy.py\n"
        "Expected: ${GENERATED_PROXY}")
endif()

add_library(k2se SHARED
    src/dllmain.cpp
    src/log.cpp
    src/fingerprint.cpp
    src/vm.cpp
    src/routines.cpp
    src/vmstack.cpp
    src/exostring.cpp
    src/gameobj.cpp
    src/glhook.cpp
    src/config.cpp
    src/input.cpp
    src/callsite.cpp
    src/player.cpp
    src/anim.cpp
    src/movement.cpp
    src/fov.cpp
    src/camera.cpp
    src/fpcam.cpp
    src/fpview.cpp
    src/console.cpp
    src/render.cpp
    src/spawner.cpp
    src/npcvariety.cpp
    src/lk2_proto.cpp
    src/lk2_mailbox.cpp
    src/lk2_meter.cpp
    src/bridge.cpp
    "${GENERATED_PROXY}"
)

target_include_directories(k2se PRIVATE src)

set_target_properties(k2se PROPERTIES
    OUTPUT_NAME "version"      # the proxy slot; see DESIGN.md 3.1
    PREFIX ""                  # -> version.dll, not libversion.dll
    CXX_STANDARD 17
    CXX_STANDARD_REQUIRED ON
)

# Static CRT: the shipped DLL must import only KERNEL32, so a user without the
# VC++ redistributable can still load it.
set_property(TARGET k2se PROPERTY MSVC_RUNTIME_LIBRARY "MultiThreaded")

target_compile_definitions(k2se PRIVATE
    WIN32_LEAN_AND_MEAN
    NOMINMAX
    _CRT_SECURE_NO_WARNINGS
    K2SE_VERSION_MAJOR=${PROJECT_VERSION_MAJOR}
    K2SE_VERSION_MINOR=${PROJECT_VERSION_MINOR}
    K2SE_VERSION_PATCH=${PROJECT_VERSION_PATCH}
)

target_compile_options(k2se PRIVATE /W4 /permissive- /GS- /Gy)

# /DEF supplies the forwarding export names (GetFileVersionInfoA = stub, ...).
# /NODEFAULTLIB is NOT used: the static CRT still provides memcpy etc.
target_link_options(k2se PRIVATE
    "/DEF:${GENERATED_DEF}"
    /OPT:REF
    /OPT:ICF
)

target_link_libraries(k2se PRIVATE kernel32)

# Explicit native integration target; never linked into the proxy DLL.
add_executable(lk2_log_native_test EXCLUDE_FROM_ALL
    tests/lk2_log_native_test.cpp src/lk2_meter.cpp src/lk2_proto.cpp src/lk2_mailbox.cpp)
set_property(TARGET lk2_log_native_test PROPERTY MSVC_RUNTIME_LIBRARY "MultiThreaded")
set_property(TARGET lk2_log_native_test PROPERTY CXX_STANDARD 17)
target_compile_definitions(lk2_log_native_test PRIVATE
    WIN32_LEAN_AND_MEAN NOMINMAX _CRT_SECURE_NO_WARNINGS
    K2SE_VERSION_MAJOR=${PROJECT_VERSION_MAJOR}
    K2SE_VERSION_MINOR=${PROJECT_VERSION_MINOR}
    K2SE_VERSION_PATCH=${PROJECT_VERSION_PATCH})
target_compile_options(lk2_log_native_test PRIVATE /W4 /permissive- /EHsc)
target_link_libraries(lk2_log_native_test PRIVATE kernel32)

# --- post-build sanity ------------------------------------------------------
add_custom_command(TARGET k2se POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E echo "K2SE built: $<TARGET_FILE:k2se>"
    COMMAND ${CMAKE_COMMAND} -E echo "Verify the DLL before shipping it:"
    COMMAND ${CMAKE_COMMAND} -E echo "    python tools/check_dll.py \"$<TARGET_FILE:k2se>\""
)
