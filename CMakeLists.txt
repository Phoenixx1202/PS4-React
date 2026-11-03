cmake_minimum_required(VERSION 3.18)

# PS4 React — React runtime for PS4 homebrew.
#
# Configure with the OpenOrbis/PacBrew CMake toolchain:
#   cmake -S . -B build \
#     -DCMAKE_TOOLCHAIN_FILE=$OPENORBIS/cmake/ps4.cmake \
#     -DCMAKE_BUILD_TYPE=Release
#
# The toolchain file must be supplied on the command line: CMake reads it
# before this file is processed. The Windows helper in tools/build_ps4.ps1
# does that inside the PacBrew rootfs.

project(PS4React LANGUAGES C CXX)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

if(NOT DEFINED OPENORBIS)
    if(DEFINED ENV{OPENORBIS})
        set(OPENORBIS "$ENV{OPENORBIS}")
    else()
        set(OPENORBIS "/opt/pacbrew/ps4/openorbis")
    endif()
endif()

if(NOT EXISTS "${OPENORBIS}/link.x")
    message(FATAL_ERROR
        "OPENORBIS does not look like an OpenOrbis toolchain: ${OPENORBIS}\n"
        "Configure with -DCMAKE_TOOLCHAIN_FILE=$OPENORBIS/cmake/ps4.cmake")
endif()

# OpenOrbis' stock link.x drops .data.* / .init_array.* / .tdata.* / .tbss.*.
# Stremio PS4 proved that this leaves initialized globals outside the loaded
# image and crashes the title at launch. Build a project-local link.x instead.
find_program(PS4_PYTHON NAMES python3 python)
if(NOT PS4_PYTHON)
    message(FATAL_ERROR "python3 is required to prepare the PS4 linker script")
endif()
set(PS4_PROJECT_LINKER "${CMAKE_BINARY_DIR}/ps4-link.x")
execute_process(
    COMMAND "${PS4_PYTHON}" "${CMAKE_CURRENT_SOURCE_DIR}/tools/prepare_ps4_linker.py"
            "${OPENORBIS}/link.x" "${PS4_PROJECT_LINKER}"
    RESULT_VARIABLE PS4_LINKER_RESULT
    OUTPUT_VARIABLE PS4_LINKER_OUTPUT
    ERROR_VARIABLE PS4_LINKER_ERROR)
if(NOT PS4_LINKER_RESULT EQUAL 0)
    message(FATAL_ERROR "PS4 linker script preparation failed: ${PS4_LINKER_ERROR}")
endif()
string(REPLACE "--script ${OPENORBIS}/link.x" "--script ${PS4_PROJECT_LINKER}"
       CMAKE_EXE_LINKER_FLAGS "${CMAKE_EXE_LINKER_FLAGS}")
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS
    "${CMAKE_CURRENT_SOURCE_DIR}/tools/prepare_ps4_linker.py" "${OPENORBIS}/link.x")
message(STATUS "PS4 linker: ${PS4_PROJECT_LINKER}")

# OpenOrbis' crt1.o carries the local sceLibcHeapSize the loader reads. Its -1
# default gives a ~12.7 MiB heap, which only works for mini apps that inherit
# the shell heap. tools/build_ci.sh passes a copy patched to 512 MiB; use it in
# the executable link templates instead of the toolchain copy.
if(PS4_REACT_CRT1)
    if(NOT EXISTS "${PS4_REACT_CRT1}")
        message(FATAL_ERROR "PS4_REACT_CRT1 does not exist: ${PS4_REACT_CRT1}")
    endif()
    foreach(PS4_LANG C CXX)
        string(REPLACE "${OPENORBIS}/lib/crt1.o" "${PS4_REACT_CRT1}"
               CMAKE_${PS4_LANG}_LINK_EXECUTABLE "${CMAKE_${PS4_LANG}_LINK_EXECUTABLE}")
    endforeach()
    message(STATUS "PS4 crt1: ${PS4_REACT_CRT1}")
endif()

if(NOT PS4_REACT_GENERATED_DIR)
    set(PS4_REACT_GENERATED_DIR "${CMAKE_SOURCE_DIR}/.build/react")
endif()

add_subdirectory(native/ps4)
