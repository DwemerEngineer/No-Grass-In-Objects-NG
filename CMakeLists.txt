cmake_minimum_required(VERSION 3.28)
message("Using toolchain file ${CMAKE_TOOLCHAIN_FILE}.")

project(
  NGIO-NG
  VERSION 2.0.1
  LANGUAGES CXX
)

option(COPY_BUILD "Copy the build output to the Skyrim directory." FALSE)
option(BUILD_ZIP "Create a 7z archive." TRUE)
option(NGIO_WAIT_FOR_DEBUGGER "Wait for a debugger when loading a Debug build." OFF)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_INTERPROCEDURAL_OPTIMIZATION ON)
set(CMAKE_INTERPROCEDURAL_OPTIMIZATION_DEBUG OFF)
set(CMAKE_OPTIMIZE_DEPENDENCIES ON)
set(SKSE_SUPPORT_XBYAK ON)

include(GNUInstallDirs)

configure_file(
  ${CMAKE_CURRENT_SOURCE_DIR}/cmake/Version.h.in
  ${CMAKE_CURRENT_BINARY_DIR}/include/Version.h
  @ONLY
)

configure_file(
  ${CMAKE_CURRENT_SOURCE_DIR}/cmake/version.rc.in
  ${CMAKE_CURRENT_BINARY_DIR}/version.rc
  @ONLY
)

set(sources
  src/PCH.cpp
  src/Main.cpp
  src/AddCellGrassTask.cpp
  src/CliffSurfaces.cpp
  src/Config.cpp
  src/ObjectHandler.cpp
  src/Events.cpp
  src/Hooks.cpp
  src/DistantGrass.cpp
  src/GrassCellCreation.cpp
  src/GrassPatchCreation.cpp
  src/GidFileCache.cpp
  src/PhysicsRaycast.cpp
  src/RaycastHelper.cpp
  src/Profiler.cpp
  src/Util.cpp
)
list(TRANSFORM sources PREPEND "${CMAKE_CURRENT_SOURCE_DIR}/")
list(APPEND sources "${CMAKE_CURRENT_BINARY_DIR}/version.rc")
file(GLOB_RECURSE headers CONFIGURE_DEPENDS
  "${CMAKE_CURRENT_SOURCE_DIR}/include/GrassControl/*.h"
  "${CMAKE_CURRENT_SOURCE_DIR}/include/GrassControl/*.hpp"
)

source_group(
  TREE ${CMAKE_CURRENT_SOURCE_DIR}
  FILES ${headers} ${sources}
)

set(BUILD_TESTS OFF)
add_subdirectory(external/CommonLibSSE)
include(external/CommonLibSSE/cmake/CommonLibSSE.cmake)

add_commonlibsse_plugin(
  ${PROJECT_NAME}
  AUTHOR DwemerEngineer
  SOURCES ${headers} ${sources}
)

add_library(${PROJECT_NAME}::${PROJECT_NAME} ALIAS ${PROJECT_NAME})

find_path(SIMPLEINI_INCLUDE_DIRS "SimpleIni.h" REQUIRED)
find_path(CLIBUTIL_INCLUDE_DIRS "CLibUtil/string.hpp" REQUIRED)
find_package(TBB CONFIG REQUIRED COMPONENTS tbb)
get_target_property(TBB_LIBRARY_TYPE TBB::tbb TYPE)
if(NOT TBB_LIBRARY_TYPE STREQUAL "STATIC_LIBRARY")
  message(FATAL_ERROR "NGIO requires static oneTBB to avoid runtime DLL conflicts with other SKSE plugins.")
endif()

find_path(GLM_INCLUDE_DIRS "glm/glm.hpp" REQUIRED)
target_include_directories(${PROJECT_NAME} PRIVATE ${GLM_INCLUDE_DIRS})

target_link_libraries(
  ${PROJECT_NAME}
  PRIVATE
  TBB::tbb
)
target_include_directories(
  ${PROJECT_NAME}
  PRIVATE
  $<BUILD_INTERFACE:${CMAKE_CURRENT_BINARY_DIR}/include>
  ${SIMPLEINI_INCLUDE_DIRS}
  ${CLIBUTIL_INCLUDE_DIRS}
  PUBLIC
  $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
)

target_precompile_headers(
  ${PROJECT_NAME}
  PRIVATE
  include/PCH.h
)

# Recover understood C++ failures; hardware faults must reach the crash handler.
target_compile_options(${PROJECT_NAME} PRIVATE "$<$<CXX_COMPILER_ID:MSVC>:/EHsc;/GR->")
if(NGIO_WAIT_FOR_DEBUGGER)
  target_compile_definitions(${PROJECT_NAME} PRIVATE $<$<CONFIG:Debug>:NGIO_WAIT_FOR_DEBUGGER>)
endif()

install(TARGETS ${PROJECT_NAME} RUNTIME DESTINATION SKSE/Plugins)
include(cmake/packaging.cmake)

if(MSVC_VERSION GREATER_EQUAL 1936 AND MSVC_IDE) # 17.6+
	# Disable the STL module build enabled implicitly by /std:c++latest.
	# Set the MSBuild property directly: https://gitlab.kitware.com/cmake/cmake/-/issues/24922
	file(CONFIGURE OUTPUT "${CMAKE_BINARY_DIR}/Directory.Build.props" CONTENT [==[
<Project>
  <ItemDefinitionGroup>
    <ClCompile>
      <BuildStlModules>false</BuildStlModules>
    </ClCompile>
  </ItemDefinitionGroup>
</Project>
]==] @ONLY)
endif()
