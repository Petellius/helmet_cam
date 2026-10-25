# Helmet Cam: a Door Kickers 2 plugin (dk2ml) that shows a first-person picture-in-picture from an operator's head. The
# project name is the DLL name, the mod folder tools\install.ps1 and tools\package.ps1 create, and the loader's
# settings folder (GetConfigDir).
cmake_minimum_required(VERSION 3.20)
project(helmet_cam VERSION 0.1.0 LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
# Static CRT: players need no VC++ redistributable. Build Release for the Workshop: Debug lays out std:: types
# differently from the game, and its CRT checks slow every frame.
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")

if(NOT CMAKE_SIZEOF_VOID_P EQUAL 8)
    message(FATAL_ERROR "Door Kickers 2 is 64-bit; configure with an x64 toolchain")
endif()

# dk2ml.h/.hpp: dk2ml\ in the template zip, or the folder in DK2ML_INCLUDE.
set(DK2ML_INCLUDE "${CMAKE_CURRENT_SOURCE_DIR}/dk2ml" CACHE PATH "folder with dk2ml.h and dk2ml.hpp")
if(NOT EXISTS "${DK2ML_INCLUDE}/dk2ml.hpp")
    message(FATAL_ERROR "dk2ml.h/dk2ml.hpp not found in ${DK2ML_INCLUDE}: they come with the template zip in dk2ml\\, or set DK2ML_INCLUDE")
endif()

add_library(${PROJECT_NAME} SHARED src/Plugin.cpp)
target_include_directories(${PROJECT_NAME} PRIVATE "${DK2ML_INCLUDE}")
target_compile_definitions(${PROJECT_NAME} PRIVATE UNICODE _UNICODE NOMINMAX)
# opengl32: the GL 1.1 entry points (textures, glGet*). The framebuffer functions are the game's own pointers.
target_link_libraries(${PROJECT_NAME} PRIVATE user32 opengl32)
