cmake_minimum_required(VERSION 3.21)
project(LoadingFlow LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Static CRT on purpose: the shipped DLL must not require the MSVC redistributable.
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")

find_package(CommonLibSSE CONFIG REQUIRED)
find_package(Threads REQUIRED)

# The in-game settings menu is optional and is NOT vendored here. To build it, obtain
# SKSEMenuFramework.h from https://github.com/QTR-Modding/SKSE-Menu-Framework-3 and drop the
# full (≈520 KB) header into include/.
option(WITH_MENU "Build the in-game settings menu (requires include/SKSEMenuFramework.h)" OFF)

add_library(${PROJECT_NAME} SHARED
    src/main.cpp
)

target_include_directories(${PROJECT_NAME} PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/include
)

if (WITH_MENU)
    target_compile_definitions(${PROJECT_NAME} PRIVATE
        LOADINGFLOW_WITH_MENU=1
        IMGUI_DEFINE_MATH_OPERATORS
    )
endif ()

target_link_libraries(${PROJECT_NAME} PRIVATE
    CommonLibSSE::CommonLibSSE
    Threads::Threads
)

if (MSVC)
    target_compile_options(${PROJECT_NAME} PRIVATE
        /utf-8
        /experimental:deterministic
    )
    # Keep absolute paths out of the shipped binary. CLIBSSE_SRC is optional: point it at a local
    # CommonLibSSE checkout to map that tree as well.
    target_compile_options(${PROJECT_NAME} PRIVATE "/pathmap:${CMAKE_SOURCE_DIR}=S:/proj")
    if (CLIBSSE_SRC)
        target_compile_options(${PROJECT_NAME} PRIVATE "/pathmap:${CLIBSSE_SRC}=S:/clib")
    endif ()

    set(CMAKE_MSVC_DEBUG_INFORMATION_FORMAT "$<IF:$<CONFIG:Debug>,ProgramDatabase,>")
    target_link_options(${PROJECT_NAME} PRIVATE /PDBALTPATH:%_PDB%)
endif ()

set_target_properties(${PROJECT_NAME} PROPERTIES
    PREFIX ""
    OUTPUT_NAME "LoadingFlow"
)
