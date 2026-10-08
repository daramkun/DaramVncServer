cmake_minimum_required(VERSION 3.25)
project(DaramVncServer LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

find_package(ZLIB REQUIRED)
find_package(zlib-ng CONFIG REQUIRED)
find_package(libjpeg-turbo CONFIG REQUIRED)
find_package(OpenSSL REQUIRED)

file(GLOB_RECURSE CORE_SOURCES CONFIGURE_DEPENDS src/*.cpp)
list(REMOVE_ITEM CORE_SOURCES ${CMAKE_CURRENT_SOURCE_DIR}/src/main.cpp)

add_library(daramcore STATIC ${CORE_SOURCES})
target_include_directories(daramcore PUBLIC src)
target_compile_definitions(daramcore PUBLIC WIN32_LEAN_AND_MEAN NOMINMAX UNICODE _UNICODE _WIN32_WINNT=0x0A00)
if(MSVC)
    target_compile_options(daramcore PUBLIC /W4 /permissive- /utf-8)
endif()
target_link_libraries(daramcore PUBLIC
    ZLIB::ZLIB zlib-ng::zlib libjpeg-turbo::turbojpeg-static OpenSSL::SSL OpenSSL::Crypto
    ws2_32 user32 gdi32 d3d11 dxgi bcrypt crypt32 advapi32 wtsapi32 userenv shell32
    mfplat mf mfuuid wmcodecdspuuid ole32 oleaut32 strmiids)

add_executable(daramvnc src/main.cpp)
target_link_libraries(daramvnc PRIVATE daramcore)

file(GLOB TEST_SOURCES CONFIGURE_DEPENDS tests/*.cpp)
if(TEST_SOURCES)
    add_executable(daramvnc_tests ${TEST_SOURCES})
    target_include_directories(daramvnc_tests PRIVATE tests)
    target_link_libraries(daramvnc_tests PRIVATE daramcore)
    enable_testing()
    add_test(NAME all COMMAND daramvnc_tests)
endif()

# Integration/benchmark client speaking RFB (with reference decoders) against a running server.
add_executable(daramvnc_client tools/vnc_client.cpp)
target_include_directories(daramvnc_client PRIVATE tests)
target_link_libraries(daramvnc_client PRIVATE daramcore winmm)

add_executable(animwin tools/animwin.cpp)
target_link_libraries(animwin PRIVATE user32 gdi32)

add_executable(bench_encode tools/bench_encode.cpp)
target_link_libraries(bench_encode PRIVATE daramcore)
