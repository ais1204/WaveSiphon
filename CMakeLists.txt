cmake_minimum_required(VERSION 3.20)
project(WaveSiphon VERSION 1.0.0 LANGUAGES CXX RC)

option(WAVESIPHON_BUILD_TESTS "Build the WAV writer test" OFF)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
# Static CRT: a single .exe with no runtime to install.
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")

set(WAVESIPHON_DEFINES UNICODE _UNICODE NOMINMAX _WIN32_WINNT=0x0A00 WINVER=0x0A00)

add_executable(WaveSiphon WIN32
    src/main.cpp
    src/recorder.cpp
    src/wav_writer.cpp
    src/audio_apps.cpp
    src/WaveSiphon.rc
)
target_include_directories(WaveSiphon PRIVATE src)
target_compile_definitions(WaveSiphon PRIVATE ${WAVESIPHON_DEFINES})
target_link_libraries(WaveSiphon PRIVATE ole32 uuid comctl32 shell32 version avrt user32 gdi32)

if(MSVC)
    target_compile_options(WaveSiphon PRIVATE /utf-8 /W4 /permissive-)
    # The manifest is embedded through WaveSiphon.rc.
    target_link_options(WaveSiphon PRIVATE /MANIFEST:NO)
else()
    target_compile_options(WaveSiphon PRIVATE -Wall -Wextra -municode)
    target_link_options(WaveSiphon PRIVATE -municode -static -s)
endif()

if(WAVESIPHON_BUILD_TESTS)
    add_executable(wav_writer_test tests/wav_writer_test.cpp src/wav_writer.cpp)
    target_compile_definitions(wav_writer_test PRIVATE ${WAVESIPHON_DEFINES})
    if(MSVC)
        target_compile_options(wav_writer_test PRIVATE /utf-8 /W4 /permissive-)
    else()
        target_compile_options(wav_writer_test PRIVATE -municode)
        target_link_options(wav_writer_test PRIVATE -municode -static)
    endif()
endif()
