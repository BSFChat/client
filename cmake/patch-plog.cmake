# Patch outdated cmake_minimum_required in libdatachannel's vendored dependencies
# for CMake 3.31+ compatibility (plog, usrsctp, libjuice, libsrtp)
file(GLOB_RECURSE _cmake_files "deps/*/CMakeLists.txt")
foreach(_file ${_cmake_files})
    file(READ "${_file}" _content)
    if(_content MATCHES "cmake_minimum_required\\(VERSION [0-2]\\.")
        string(REGEX REPLACE
            "cmake_minimum_required\\(VERSION [0-9.]+"
            "cmake_minimum_required(VERSION 3.5"
            _content "${_content}")
        file(WRITE "${_file}" "${_content}")
        message(STATUS "Patched cmake_minimum_required in ${_file}")
    endif()
endforeach()
