# Patch plog's outdated cmake_minimum_required for CMake 3.31+ compatibility
file(GLOB_RECURSE _plog_files "deps/plog/CMakeLists.txt")
foreach(_file ${_plog_files})
    file(READ "${_file}" _content)
    string(REGEX REPLACE
        "cmake_minimum_required\\(VERSION [0-9.]+"
        "cmake_minimum_required(VERSION 3.5"
        _content "${_content}")
    file(WRITE "${_file}" "${_content}")
endforeach()
