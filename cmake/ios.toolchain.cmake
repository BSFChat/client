# iOS CMake toolchain
set(CMAKE_SYSTEM_NAME iOS)
set(CMAKE_OSX_DEPLOYMENT_TARGET "16.0" CACHE STRING "Minimum iOS version")
set(CMAKE_XCODE_ATTRIBUTE_ONLY_ACTIVE_ARCH NO)

# Default to arm64 (physical devices). Simulator uses x86_64 or arm64 depending on host.
if(NOT CMAKE_OSX_ARCHITECTURES)
    set(CMAKE_OSX_ARCHITECTURES "arm64" CACHE STRING "Build architectures")
endif()
