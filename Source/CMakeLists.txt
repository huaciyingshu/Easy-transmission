cmake_minimum_required(VERSION 3.16)
project(Easy-transmission LANGUAGES C CXX)

if(NOT WIN32 OR NOT CMAKE_SIZEOF_VOID_P EQUAL 8)
    message(FATAL_ERROR "Easy-transmission requires 64-bit Windows 10 or later")
endif()

set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
find_package(Threads REQUIRED)

set(vendor_dir "${CMAKE_CURRENT_SOURCE_DIR}/third_party")
set(generated_dir "${CMAKE_CURRENT_BINARY_DIR}/generated")
set(public_dir "${CMAKE_CURRENT_SOURCE_DIR}/public")
set(assets_header "${generated_dir}/assets.hpp")

# miniz's upstream build generates this macro for its selected linkage.
file(GENERATE OUTPUT "${generated_dir}/miniz_export.h" CONTENT "#pragma once\n#define MINIZ_EXPORT\n")

add_custom_command(
    OUTPUT "${assets_header}"
    COMMAND "${CMAKE_COMMAND}" -E make_directory "${generated_dir}"
    COMMAND "${CMAKE_COMMAND}" "-DPUBLIC_DIR=${public_dir}" "-DOUTPUT=${assets_header}"
            -P "${CMAKE_CURRENT_SOURCE_DIR}/cmake/EmbedAssets.cmake"
    DEPENDS
        "${public_dir}/index.html"
        "${public_dir}/app.js"
        "${public_dir}/style.css"
        "${CMAKE_CURRENT_SOURCE_DIR}/cmake/EmbedAssets.cmake"
    VERBATIM
)

add_executable(Easy-transmission
    "${CMAKE_CURRENT_SOURCE_DIR}/app.manifest"
    src/main.cpp
    src/network.cpp
    src/firewall.cpp
    src/transfer.cpp
    src/formats.cpp
    src/native_files.cpp
    "${assets_header}"
    "${vendor_dir}/qrcodegen/qrcodegen.cpp"
    "${vendor_dir}/miniz/miniz.c"
    "${vendor_dir}/miniz/miniz_tdef.c"
    "${vendor_dir}/miniz/miniz_tinfl.c"
    "${vendor_dir}/miniz/miniz_zip.c"
)
set_target_properties(Easy-transmission PROPERTIES
    CXX_STANDARD 17
    CXX_STANDARD_REQUIRED YES
    CXX_EXTENSIONS NO
)
if(MSVC)
    target_link_options(Easy-transmission PRIVATE "/MANIFEST:EMBED" "/MANIFESTINPUT:${CMAKE_CURRENT_SOURCE_DIR}/app.manifest")
endif()
target_include_directories(Easy-transmission PRIVATE
    "${generated_dir}"
    "${vendor_dir}/cpp-httplib"
    "${vendor_dir}/json"
    "${vendor_dir}/qrcodegen"
    "${vendor_dir}/miniz"
)
target_link_libraries(Easy-transmission PRIVATE Threads::Threads ws2_32 iphlpapi shell32 ole32 oleaut32)
target_compile_definitions(Easy-transmission PRIVATE _WIN32_WINNT=0x0A00 _CRT_SECURE_NO_WARNINGS)
if(MSVC)
    target_compile_options(Easy-transmission PRIVATE /utf-8 /bigobj)
endif()
