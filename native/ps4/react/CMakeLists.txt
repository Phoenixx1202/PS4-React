# PS4 React starter: Embedded React + QuickJS + the bundled JSX app.

if(NOT ER_ROOT)
    message(FATAL_ERROR "PS4_REACT_WITH_REACT=ON requires -DER_ROOT=/path/to/embedded-react")
endif()
if(NOT QUICKJS_ROOT)
    message(FATAL_ERROR "PS4_REACT_WITH_REACT=ON requires -DQUICKJS_ROOT=/path/to/quickjs")
endif()
if(NOT EXISTS "${QUICKJS_ROOT}/quickjs.h")
    message(FATAL_ERROR "QUICKJS_ROOT does not contain quickjs.h: ${QUICKJS_ROOT}")
endif()
if(NOT EXISTS "${PS4_REACT_GENERATED_DIR}/app.bundle.js")
    message(FATAL_ERROR
        "Missing ${PS4_REACT_GENERATED_DIR}/app.bundle.js. Run tools/bundle_react.ps1 first.")
endif()
if(NOT EXISTS "${PS4_REACT_GENERATED_DIR}/assets.generated.c")
    message(FATAL_ERROR
        "Missing ${PS4_REACT_GENERATED_DIR}/assets.generated.c. Run tools/bundle_react.ps1 first.")
endif()
if(NOT EXISTS "${PS4_REACT_GENERATED_DIR}/sounds.generated.c")
    message(FATAL_ERROR
        "Missing ${PS4_REACT_GENERATED_DIR}/sounds.generated.c. Run tools/bundle_react.ps1 first.")
endif()

option(PS4_REACT_DEV_SERVER "Enable the TCP hot-reload receiver" OFF)

set(ER_BRIDGE_QUICKJS_SMOKE OFF CACHE BOOL "" FORCE)
set(ER_BRIDGE_QUICKJS_RUNTEST OFF CACHE BOOL "" FORCE)
set(ER_BRIDGE_QUICKJS_SPLITTEST OFF CACHE BOOL "" FORCE)
set(ER_BRIDGE_QUICKJS_GCTEST OFF CACHE BOOL "" FORCE)
set(ER_BRIDGE_QUICKJS_COMPILE OFF CACHE BOOL "" FORCE)
set(ER_BRIDGE_JS_USABLE_SIZE shim CACHE STRING "" FORCE)
set(FETCHCONTENT_SOURCE_DIR_QUICKJS "${QUICKJS_ROOT}" CACHE PATH "" FORCE)

if(NOT HARFBUZZ_SRC)
    set(HARFBUZZ_SRC "/workspace/harfbuzz/src")
endif()
if(NOT HARFBUZZ_LIB_DIR)
    set(HARFBUZZ_LIB_DIR "${CMAKE_SOURCE_DIR}/.build/harfbuzz")
endif()
include_directories(BEFORE "${CMAKE_CURRENT_SOURCE_DIR}/../compat")
add_subdirectory("${ER_ROOT}/bridges/quickjs" bridge)

# quickjs-libc is the CLI/OS module library; the React bridge only needs qjs.
# Its ttyGetWinSize uses TIOCGWINSZ, which OpenOrbis' libc does not define.
if(TARGET qjs-libc)
    set_target_properties(qjs-libc PROPERTIES EXCLUDE_FROM_ALL TRUE)
endif()
if(TARGET qjs_exe)
    set_target_properties(qjs_exe PROPERTIES EXCLUDE_FROM_ALL TRUE)
endif()
if(TARGET qjsc)
    set_target_properties(qjsc PROPERTIES EXCLUDE_FROM_ALL TRUE)
endif()

add_executable(ps4_react_starter
    react_host.cpp
    ../host_platform.cpp
    ../shared/package_api.cpp
    ../shared/save_data_api.cpp
    ../shared/ime_api.cpp
    ../shared/text_shaper.cpp
    ../third_party/sheenbidi/Source/SheenBidi.c
    ../audio_out.cpp
    ../shared/js_heap.cpp
    ../display_video.cpp
    ../system.cpp
    ../pad.cpp
    ../heap.cpp
    ../hui/core/input.cpp
    ../hui/audio/mixer.cpp
    ../shared/host_api.cpp
    ../shared/removals.cpp
    ../shared/sound_api.cpp
    ../shared/network.cpp
    ../shared/network_api.cpp
    ../shared/image_loader.cpp
    ../shared/image_api.cpp
    ../shared/archives.cpp
    ../shared/archive_api.cpp
    ../shared/archive_writer.cpp
    ../shared/archive_preflight_stub.cpp
    ../shared/rar5_password_stub.cpp
    ../network_platform.cpp
    ../archive_platform.cpp
    "${PS4_REACT_GENERATED_DIR}/assets.generated.c"
    "${PS4_REACT_GENERATED_DIR}/sounds.generated.c"
    "${PS4_REACT_GENERATED_DIR}/bundle.c"
)

if(PS4_REACT_DEV_SERVER)
    target_sources(ps4_react_starter PRIVATE ../dev_server.cpp)
    target_compile_definitions(ps4_react_starter PRIVATE PS4_REACT_DEV_SERVER=1)
endif()

target_include_directories(ps4_react_starter PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}/.."
    "${CMAKE_CURRENT_SOURCE_DIR}/../shared"
    "${CMAKE_CURRENT_SOURCE_DIR}/../third_party"
    "${CMAKE_CURRENT_SOURCE_DIR}/../third_party/sheenbidi/Headers"
    "${CMAKE_CURRENT_SOURCE_DIR}/../third_party/sheenbidi/Source"
    "${HARFBUZZ_SRC}"
    "${CMAKE_CURRENT_SOURCE_DIR}/../hui"
    "${ER_ROOT}/engine/include"
    "${ER_ROOT}/engine/text"
    "${ER_ROOT}/bridges/quickjs"
    "${ER_ROOT}/backends/software"
    "${OPENORBIS}/usr/include"
    "${PS4_REACT_GENERATED_DIR}"
)

target_link_directories(ps4_react_starter PRIVATE "${OPENORBIS}/usr/lib")

target_compile_features(ps4_react_starter PRIVATE cxx_std_20)
target_compile_options(ps4_react_starter PRIVATE
    -O2
    -Wall
    -Wextra
    -Wno-unused-parameter
    $<$<COMPILE_LANGUAGE:CXX>:-fno-exceptions>
    $<$<COMPILE_LANGUAGE:CXX>:-fno-rtti>
)
set_source_files_properties(
    "${PS4_REACT_GENERATED_DIR}/assets.generated.c"
    "${PS4_REACT_GENERATED_DIR}/sounds.generated.c"
    PROPERTIES COMPILE_OPTIONS "-w")
set_source_files_properties(../third_party/sheenbidi/Source/SheenBidi.c
    PROPERTIES COMPILE_DEFINITIONS "SB_CONFIG_UNITY")

target_link_libraries(ps4_react_starter PRIVATE
    er-bridge-quickjs
    er-software
    embedded-react
    SceSysmodule
    SceAppInstUtil
    SceSaveData
    SceCommonDialog
    SceImeDialog
    SceVideoOut
    ScePad
    SceAudioOut
    SceNet
    SceNetCtl
    SceUserService
    SceSystemService
    curl
    mbedtls
    mbedx509
    mbedcrypto
    archive
    bz2
    png
    z
    "${HARFBUZZ_LIB_DIR}/libharfbuzz-raster.a"
    "${HARFBUZZ_LIB_DIR}/libharfbuzz.a"
    pthread
)

if(COMMAND add_self)
    add_self(ps4_react_starter)

    set(PS4_REACT_APP_DIR "${CMAKE_BINARY_DIR}/app-react")
    add_custom_target(ps4_react_starter_stage ALL
        COMMAND "${CMAKE_COMMAND}" -E make_directory "${PS4_REACT_APP_DIR}/sce_module"
        COMMAND "${CMAKE_COMMAND}" -E copy_if_different
                "${CMAKE_CURRENT_BINARY_DIR}/eboot.bin" "${PS4_REACT_APP_DIR}/eboot.bin"
        COMMAND "${CMAKE_COMMAND}" -E copy_if_different
                "${OPENORBIS}/samples/piglet/sce_module/libc.prx"
                "${PS4_REACT_APP_DIR}/sce_module/libc.prx"
        COMMAND "${CMAKE_COMMAND}" -E copy_if_different
                "${OPENORBIS}/samples/piglet/sce_module/libSceFios2.prx"
                "${PS4_REACT_APP_DIR}/sce_module/libSceFios2.prx"
        COMMAND "${CMAKE_COMMAND}" -E copy_if_different
                "${CMAKE_SOURCE_DIR}/assets/ca-bundle.crt"
                "${PS4_REACT_APP_DIR}/ca-bundle.crt"
        DEPENDS ps4_react_starter_self
        COMMENT "Staging the React launch folder in ${PS4_REACT_APP_DIR}"
        VERBATIM)
endif()
