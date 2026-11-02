# PS4 React host.
#
# The engine libraries and the React starter are the only targets: a build
# produces one launch folder and one PKG.

if(NOT ER_ROOT)
    message(FATAL_ERROR "Set -DER_ROOT=/path/to/embedded-react")
endif()
if(NOT EXISTS "${ER_ROOT}/engine/CMakeLists.txt")
    message(FATAL_ERROR "ER_ROOT does not contain engine/CMakeLists.txt: ${ER_ROOT}")
endif()

add_subdirectory(engine)
add_subdirectory(react)
