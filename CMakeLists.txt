cmake_minimum_required(VERSION 3.20)

# Enable ASan and UBSan by default (pass in -DENABLE_ASAN_UBSAN=OFF to disable
# it for release builds).
option(ENABLE_ASAN_UBSAN "Enable sanitizers in Debug (ASan only with MSVC)" ON)

# Disable coverage by default (pass in -DENABLE_COVERAGE=ON to enable it for test builds).
option(ENABLE_COVERAGE "Enable LLVM source coverage" OFF)

# Use vcpkg's toolchain file so that vcpkg can work. If -DCMAKE_TOOLCHAIN_FILE
# was not passed in, default to the directory of the vcpkg executable in PATH.
if(NOT DEFINED CMAKE_TOOLCHAIN_FILE)
    find_program(amj_vcpkg_executable vcpkg REQUIRED)
    get_filename_component(amj_vcpkg_directory "${amj_vcpkg_executable}" DIRECTORY)
    set(CMAKE_TOOLCHAIN_FILE "${amj_vcpkg_directory}/scripts/buildsystems/vcpkg.cmake" CACHE FILEPATH "vcpkg toolchain")
endif()

# Tell LLVM clang where to find macOS system headers so clangd and clang-tidy
# can work. It works by setting the CMake-native CMAKE_OSX_SYSROOT variable on
# macOS.
#
# Must be configured before project() compiler detection so that compiler
# detection and compilation use the same SDK.
if(CMAKE_HOST_SYSTEM_NAME STREQUAL "Darwin" AND NOT CMAKE_OSX_SYSROOT)
    execute_process(COMMAND xcrun --sdk macosx --show-sdk-path OUTPUT_VARIABLE amj_macos_sdk OUTPUT_STRIP_TRAILING_WHITESPACE COMMAND_ERROR_IS_FATAL ANY)
    if(NOT amj_sdk_result EQUAL 0)
        message(FATAL_ERROR "Could not locate the macOS SDK; check xcode-select")
    endif()
    set(CMAKE_OSX_SYSROOT "${amj_macos_sdk}" CACHE STRING "macOS SDK" FORCE)
endif()

project(AutomateMyJob VERSION 0.1.0 LANGUAGES C CXX)
set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# Tell LLVM clang-cl where to find Windows system headers so clangd and
# clang-tidy can work. It works by passing the INCLUDE environment variable
# (configured by vcvars64.bat) to include_directories().
#
# Must be configured after project() compiler detection so that we know whether
# the compiler is MSVC (LLVM's clang-cl counts as MSVC).
if(MSVC)
    if("$ENV{INCLUDE}" STREQUAL "")
        message(FATAL_ERROR "Use scripts/build.ps1 to initialize the MSVC environment")
    endif()
    include_directories(SYSTEM $ENV{INCLUDE})
endif()

# Use CMake's built-in Threads module for threading.
find_package(Threads REQUIRED)

# Stack traces.
find_package(cpptrace REQUIRED)

# JavaScript engine.
find_package(qjs REQUIRED)

# Native Fetch URL parsing and HTTPS transport.
find_package(ada CONFIG REQUIRED)
find_package(httplib CONFIG REQUIRED)
find_package(OpenSSL REQUIRED)

# Select MSVC C/C++ runtime linkage; CMake generates the compiler flags.
#
# set(CMAKE_MSVC_RUNTIME_LIBRARY MultiThreadedDebugDLL) -> /MDd (dynamic debug
# runtime, matching prebuilt Qt).
#
# set(CMAKE_MSVC_RUNTIME_LIBRARY MultiThreaded) -> /MT (static runtime, no MSVC
# runtime DLLs to ship).
if(MSVC)
    set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:DebugDLL>")
    if(ENABLE_ASAN_UBSAN)
        # ASan is incompatible with /RTC checks and /ZI (Edit and Continue).
        foreach(amj_language C CXX)
            string(REGEX REPLACE "/RTC[^ ]*" "" CMAKE_${amj_language}_FLAGS_DEBUG "${CMAKE_${amj_language}_FLAGS_DEBUG}")
            string(REPLACE "/ZI" "/Zi" CMAKE_${amj_language}_FLAGS_DEBUG "${CMAKE_${amj_language}_FLAGS_DEBUG}")
        endforeach()
    endif()
endif()

# BUILD_TESTING defaults to ON; pass -DBUILD_TESTING=OFF to disable testing.
include(CTest)

set(amj_libraries)
set(amj_executables)
set(amj_test_libraries)
set(amj_test_sources)
set(amj_test_filters)

## == src/errors == ##

add_library(amj_errors STATIC src/errors/errors.cpp)
target_link_libraries(amj_errors PRIVATE cpptrace::cpptrace)
list(APPEND amj_libraries amj_errors)
if(BUILD_TESTING)
    list(APPEND amj_test_libraries amj_errors)
    list(APPEND amj_test_sources src/errors/errors_test.cpp)
endif()

## == src/context == ##

add_library(amj_context STATIC src/context/context.cpp)
list(APPEND amj_libraries amj_context)
target_link_libraries(amj_context PUBLIC amj_errors PRIVATE Threads::Threads)
if(BUILD_TESTING)
    list(APPEND amj_test_libraries amj_context Threads::Threads)
    list(APPEND amj_test_sources src/context/context_test.cpp)
endif()

## == src/keyring == ##

add_library(amj_keyring STATIC src/keyring/keyring.cpp)
list(APPEND amj_libraries amj_keyring)
target_link_libraries(amj_keyring PUBLIC amj_errors)
if(BUILD_TESTING)
    list(APPEND amj_test_libraries amj_keyring)
    list(APPEND amj_test_sources src/keyring/keyring_test.cpp)
    # Serialize native-store integration tests without blocking unrelated tests.
    list(APPEND amj_test_filters "KeyringIntegration.*")
    set("amj_test_properties_KeyringIntegration.*" RESOURCE_LOCK keyring)
endif()

if(CMAKE_SYSTEM_NAME STREQUAL "Windows")
    target_sources(amj_keyring PRIVATE src/keyring/keyring_windows.cpp)
    target_link_libraries(amj_keyring PRIVATE advapi32 crypt32)
    target_compile_definitions(amj_keyring PRIVATE NOMINMAX)
elseif(CMAKE_SYSTEM_NAME STREQUAL "Darwin")
    target_sources(amj_keyring PRIVATE src/keyring/keyring_darwin.cpp)
    find_library(AMJ_KEYRING_COREFOUNDATION CoreFoundation REQUIRED)
    find_library(AMJ_KEYRING_SECURITY Security REQUIRED)
    target_link_libraries(amj_keyring PRIVATE ${AMJ_KEYRING_COREFOUNDATION} ${AMJ_KEYRING_SECURITY})
elseif(CMAKE_SYSTEM_NAME STREQUAL "Linux")
    target_sources(amj_keyring PRIVATE src/keyring/keyring_linux.cpp)
    find_package(PkgConfig REQUIRED)
    pkg_check_modules(AMJ_KEYRING_LIBSECRET REQUIRED IMPORTED_TARGET libsecret-1)
    target_link_libraries(amj_keyring PRIVATE PkgConfig::AMJ_KEYRING_LIBSECRET)
else()
    message(FATAL_ERROR "Unsupported keyring platform")
endif()

## == src/javascript == ##

# File-by-file dependency order: javascript -> text -> URL -> scheduler -> timeout -> fetch.
add_library(amj_javascript STATIC
    src/javascript/javascript.hpp
    src/javascript/javascript.cpp
    src/javascript/text.js
    src/javascript/global.d.ts
    src/javascript/url_impl.cpp
    src/javascript/url.js
    src/javascript/url_impl.d.ts
    src/javascript/scheduler.cpp
    src/javascript/timeout_impl.cpp
    src/javascript/timeout_impl.d.ts
    src/javascript/timeout.js
    src/javascript/fetch_impl.cpp
    src/javascript/fetch.js
    src/javascript/fetch_impl.d.ts
)
list(APPEND amj_libraries amj_javascript)
target_link_libraries(amj_javascript PUBLIC amj_errors qjs PRIVATE Threads::Threads ada::ada httplib::httplib OpenSSL::SSL OpenSSL::Crypto)
# Keep the JS modules alongside the build without evaluating them globally.
configure_file(src/javascript/url.js javascript/url.js COPYONLY)
configure_file(src/javascript/text.js javascript/text.js COPYONLY)
configure_file(src/javascript/fetch.js javascript/fetch.js COPYONLY)
configure_file(src/javascript/timeout.js javascript/timeout.js COPYONLY)
configure_file(src/javascript/global.d.ts javascript/global.d.ts COPYONLY)
configure_file(src/javascript/url_impl.d.ts javascript/url_impl.d.ts COPYONLY)
configure_file(src/javascript/timeout_impl.d.ts javascript/timeout_impl.d.ts COPYONLY)
configure_file(src/javascript/fetch_impl.d.ts javascript/fetch_impl.d.ts COPYONLY)

## == Tests == ##

if(BUILD_TESTING)
    find_package(GTest CONFIG REQUIRED)
    include(GoogleTest)

    # Tests are built explicitly by scripts/test.ps1, never by the default build.
    add_executable(amj_tests EXCLUDE_FROM_ALL ${amj_test_sources})
    list(APPEND amj_executables amj_tests)
    target_link_libraries(amj_tests PRIVATE ${amj_test_libraries} GTest::gtest_main)

    # Each section can register non-overlapping filters and their test properties.
    foreach(amj_filter IN LISTS amj_test_filters)
        gtest_discover_tests(amj_tests
            DISCOVERY_MODE PRE_TEST
            TEST_FILTER "${amj_filter}"
            PROPERTIES ${amj_test_properties_${amj_filter}}
        )
    endforeach()

    # Discover the remaining tests. A leading '-' excludes every registered
    # filter (joined with ':' in GoogleTest syntax), avoiding duplicate tests.
    if(amj_test_filters)
        list(JOIN amj_test_filters ":" amj_excluded_filters)
        gtest_discover_tests(amj_tests
            DISCOVERY_MODE PRE_TEST
            TEST_FILTER "-${amj_excluded_filters}"
        )
    else()
        gtest_discover_tests(amj_tests DISCOVERY_MODE PRE_TEST)
    endif()

    if(ENABLE_COVERAGE)
        if(NOT CMAKE_CXX_COMPILER_ID MATCHES "Clang")
            message(FATAL_ERROR "Coverage requires LLVM Clang; use scripts/test.ps1 -coverage")
        endif()
        if(MSVC)
            # clang-cl embeds the profile runtime library name in its object files.
            execute_process(COMMAND "${CMAKE_CXX_COMPILER}" --print-runtime-dir OUTPUT_VARIABLE amj_clang_runtime_directory OUTPUT_STRIP_TRAILING_WHITESPACE COMMAND_ERROR_IS_FATAL ANY)
        endif()
        foreach(amj_target IN ITEMS amj_tests ${amj_libraries})
            target_compile_options(${amj_target} PRIVATE -fprofile-instr-generate -fcoverage-mapping)
            if(MSVC)
                target_link_directories(${amj_target} PUBLIC "${amj_clang_runtime_directory}")
            else()
                target_link_options(${amj_target} PUBLIC -fprofile-instr-generate)
            endif()
        endforeach()
    endif()
endif()

## == Common build settings == ##

foreach(amj_target IN LISTS amj_libraries)
    target_include_directories(${amj_target} PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/src")
endforeach()

# Keep executables beside one another even with multi-configuration generators.
foreach(amj_target IN LISTS amj_executables)
    set_target_properties(${amj_target} PROPERTIES RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/$<0:>")
endforeach()

foreach(amj_target IN LISTS amj_libraries amj_executables)
    if(MSVC)
        target_compile_definitions(${amj_target} PRIVATE _CRT_SECURE_NO_WARNINGS)
        target_compile_options(${amj_target} PRIVATE /W4 /WX /permissive-)
    else()
        target_compile_options(${amj_target} PRIVATE -Wall -Wextra -Wpedantic -Wconversion -Wsign-conversion -Werror)
        if(CMAKE_SYSTEM_NAME STREQUAL "Darwin")
            foreach(amj_directory IN LISTS CMAKE_CXX_IMPLICIT_INCLUDE_DIRECTORIES)
                if(amj_directory MATCHES "/include/c\\+\\+/v1$")
                    target_compile_options(${amj_target} PRIVATE "-isystem${amj_directory}")
                endif()
            endforeach()
        endif()
    endif()
    if(ENABLE_ASAN_UBSAN)
        if(MSVC)
            target_compile_definitions(${amj_target} PRIVATE "$<$<CONFIG:Debug>:_DISABLE_STRING_ANNOTATION;_DISABLE_VECTOR_ANNOTATION;_DISABLE_OPTIONAL_ANNOTATION>")
            target_compile_options(${amj_target} PRIVATE "$<$<CONFIG:Debug>:/fsanitize=address;/Zi>")
            target_link_options(${amj_target} PRIVATE "$<$<CONFIG:Debug>:/INCREMENTAL:NO;/DEBUG>")
        elseif(CMAKE_CXX_COMPILER_ID MATCHES "Clang|GNU")
            target_compile_options(${amj_target} PRIVATE "$<$<CONFIG:Debug>:-fsanitize=address,undefined;-fsanitize-address-use-after-scope;-fno-omit-frame-pointer;-fno-sanitize-recover=undefined;-g>")
            target_link_options(${amj_target} PRIVATE "$<$<CONFIG:Debug>:-fsanitize=address,undefined;-fno-sanitize-recover=undefined>")
        else()
            message(FATAL_ERROR "Sanitizers require Clang, GCC, or MSVC")
        endif()
    endif()
endforeach()
