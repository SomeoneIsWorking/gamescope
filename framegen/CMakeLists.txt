# framegen on its own: the frame pacer and the Lossless generator, for hosts outside Gamescope.
# Needs the Vulkan headers and SPIRV-Headers (the fork's thirdparty/SPIRV-Headers submodule, or
# FRAMEGEN_SPIRV_HEADERS_DIR pointing at another copy's include/spirv/unified1).
cmake_minimum_required(VERSION 3.20)
project(framegen LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

find_package(Vulkan REQUIRED)
set(FRAMEGEN_SPIRV_HEADERS_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../thirdparty/SPIRV-Headers/include/spirv/unified1"
    CACHE PATH "Directory holding spirv.hpp")
if(NOT EXISTS "${FRAMEGEN_SPIRV_HEADERS_DIR}/spirv.hpp")
    message(FATAL_ERROR "framegen needs spirv.hpp in FRAMEGEN_SPIRV_HEADERS_DIR (${FRAMEGEN_SPIRV_HEADERS_DIR})")
endif()

add_library(framegen STATIC
    src/ComputeImage.cpp
    src/Device.cpp
    src/FramePacer.cpp
    src/LosslessGenerator.cpp
    src/LosslessImages.cpp
    src/LosslessPipelines.cpp
    src/LosslessShaders.cpp
    src/PeResources.cpp
    src/SpirvInterface.cpp
)
add_library(framegen::framegen ALIAS framegen)
set_target_properties(framegen PROPERTIES POSITION_INDEPENDENT_CODE ON)
target_include_directories(framegen
    PUBLIC "$<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>" "$<INSTALL_INTERFACE:include>"
    PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/src" "${FRAMEGEN_SPIRV_HEADERS_DIR}")
target_link_libraries(framegen PUBLIC Vulkan::Headers)

include(GNUInstallDirs)
install(TARGETS framegen EXPORT framegenTargets ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR})
install(DIRECTORY include/framegen DESTINATION ${CMAKE_INSTALL_INCLUDEDIR})
install(FILES LICENSE DESTINATION ${CMAKE_INSTALL_DATADIR}/licenses/framegen)
install(EXPORT framegenTargets NAMESPACE framegen:: DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/framegen)
file(WRITE "${CMAKE_CURRENT_BINARY_DIR}/framegenConfig.cmake" [=[
include(CMakeFindDependencyMacro)
find_dependency(Vulkan)
include("${CMAKE_CURRENT_LIST_DIR}/framegenTargets.cmake")
]=])
install(FILES "${CMAKE_CURRENT_BINARY_DIR}/framegenConfig.cmake" DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/framegen)
