cmake_minimum_required(VERSION 3.25)
project(vision_sdk_check LANGUAGES C CXX)
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
if(MSVC)
    add_compile_options(/utf-8)
    add_compile_options("$<$<COMPILE_LANGUAGE:CXX>:/EHsc>")
endif()
get_filename_component(ENV_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/../../开发环境" ABSOLUTE)
set(SOURCES "${ENV_ROOT}/源码")

# Native TLS uses Windows Schannel; no OpenSSL or Anaconda dependency.
set(BUILD_SHARED_LIBS OFF CACHE BOOL "" FORCE)
set(BUILD_STATIC_LIBS ON CACHE BOOL "" FORCE)
set(BUILD_STATIC_CURL ON CACHE BOOL "" FORCE)
set(BUILD_CURL_EXE ON CACHE BOOL "" FORCE)
set(BUILD_TESTING OFF CACHE BOOL "" FORCE)
set(HTTP_ONLY ON CACHE BOOL "" FORCE)
set(CURL_USE_SCHANNEL ON CACHE BOOL "" FORCE)
set(CURL_USE_OPENSSL OFF CACHE BOOL "" FORCE)
set(CURL_USE_LIBPSL OFF CACHE BOOL "" FORCE)
set(CURL_USE_LIBSSH2 OFF CACHE BOOL "" FORCE)
set(CURL_USE_PKGCONFIG OFF CACHE BOOL "" FORCE)
set(CURL_ZLIB OFF CACHE STRING "" FORCE)
set(CURL_BROTLI OFF CACHE STRING "" FORCE)
set(CURL_ZSTD OFF CACHE STRING "" FORCE)
set(CURL_USE_LIBIDN2 OFF CACHE BOOL "" FORCE)
add_subdirectory("${SOURCES}/curl-8.22.0" curl-build)

add_library(vision_sqlite STATIC "${SOURCES}/sqlite-amalgamation-3530400/sqlite3.c")
target_include_directories(vision_sqlite PUBLIC "${SOURCES}/sqlite-amalgamation-3530400")
target_compile_definitions(vision_sqlite PRIVATE SQLITE_THREADSAFE=1)
set_target_properties(vision_sqlite PROPERTIES OUTPUT_NAME sqlite3)
install(TARGETS vision_sqlite ARCHIVE DESTINATION lib)
install(FILES "${SOURCES}/sqlite-amalgamation-3530400/sqlite3.h"
    "${SOURCES}/sqlite-amalgamation-3530400/sqlite3ext.h" DESTINATION include)
install(DIRECTORY "${SOURCES}/json-3.12.0/single_include/nlohmann" DESTINATION include)
install(DIRECTORY "${SOURCES}/spdlog-1.17.0/include/spdlog" DESTINATION include)
install(FILES "${SOURCES}/doctest-2.5.3/doctest/doctest.h" DESTINATION include/doctest)
install(FILES "${SOURCES}/cpp-httplib-0.56.0/httplib.h" DESTINATION include)
foreach(item IN ITEMS json-3.12.0 spdlog-1.17.0 doctest-2.5.3 cpp-httplib-0.56.0)
    file(GLOB license_files "${SOURCES}/${item}/LICENSE*" "${SOURCES}/${item}/COPYING*")
    install(FILES ${license_files} DESTINATION "licenses/${item}")
endforeach()

set(Qt6_DIR "${ENV_ROOT}/SDK/Qt/6.8.3/msvc2022_64/lib/cmake/Qt6")
find_package(Qt6 6.8.3 EXACT REQUIRED COMPONENTS Core Gui Widgets Network)
set(OpenCV_DIR "${ENV_ROOT}/SDK/OpenCV/opencv/build/x64/vc16/lib")
find_package(OpenCV 4.12 EXACT REQUIRED COMPONENTS core imgproc imgcodecs)
set(ORT_ROOT "${ENV_ROOT}/SDK/ONNXRuntime/onnxruntime-win-x64-1.30.0")
add_library(onnxruntime SHARED IMPORTED)
set_target_properties(onnxruntime PROPERTIES
    IMPORTED_IMPLIB "${ORT_ROOT}/lib/onnxruntime.lib"
    IMPORTED_LOCATION "${ORT_ROOT}/lib/onnxruntime.dll"
    INTERFACE_INCLUDE_DIRECTORIES "${ORT_ROOT}/include")

add_executable(sdk_smoke main.cpp)
target_include_directories(sdk_smoke PRIVATE
    "${SOURCES}/doctest-2.5.3"
    "${SOURCES}/json-3.12.0/single_include"
    "${SOURCES}/spdlog-1.17.0/include"
    "${SOURCES}/cpp-httplib-0.56.0")
target_link_libraries(sdk_smoke PRIVATE Qt6::Core Qt6::Gui Qt6::Widgets Qt6::Network
    ${OpenCV_LIBS} onnxruntime vision_sqlite CURL::libcurl ws2_32 crypt32)
add_custom_command(TARGET sdk_smoke POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
        "${ORT_ROOT}/lib/onnxruntime.dll"
        "${ORT_ROOT}/lib/onnxruntime_providers_shared.dll"
        "${ENV_ROOT}/SDK/OpenCV/opencv/build/x64/vc16/bin/opencv_world4120.dll"
        "$<TARGET_FILE_DIR:sdk_smoke>")
enable_testing()
add_test(NAME sdk_smoke COMMAND sdk_smoke)
set_tests_properties(sdk_smoke PROPERTIES TIMEOUT 30)
