cmake_minimum_required(VERSION 3.20)

project(occt-kernel-wasm
    VERSION 1.0.0
    LANGUAGES CXX
    DESCRIPTION "Reusable OCCT-based CAD kernel for WebAssembly"
)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(NOT CMAKE_CONFIGURATION_TYPES)
    set_property(CACHE CMAKE_BUILD_TYPE PROPERTY STRINGS Debug Release RelWithDebInfo MinSizeRel Fast)
endif()

# ---------------------------------------------------------------------------
# Options
# ---------------------------------------------------------------------------

option(OCCT_KERNEL_WASM   "Build the Emscripten WASM target"    OFF)
option(OCCT_KERNEL_NATIVE "Build the native shared-library target" OFF)
option(SKETCH_TOOLKIT_WASM "Build the sketch toolkit Emscripten WASM target" OFF)
option(OCCT_KERNEL_USE_MINIMAL_OCCT_LINK "Use a narrower OCCT link set for kernel targets" ON)
option(OCCT_KERNEL_ENABLE_PTHREADS "Build the Emscripten kernel with pthread support" OFF)

set(OCCT_KERNEL_ARTIFACT_BASENAME "occt-kernel"
    CACHE STRING "Basename for emitted JS/WASM kernel artifacts")
set(SKETCH_TOOLKIT_ARTIFACT_BASENAME "sketch-toolkit.wasm"
    CACHE STRING "Basename for emitted JS/WASM sketch toolkit artifacts")
set(OCCT_KERNEL_PTHREAD_POOL_SIZE "4"
    CACHE STRING "Default pthread pool size for the Emscripten kernel")

set(OCCT_KERNEL_REQUIRED_COMPONENTS
    FoundationClasses
    ModelingData
    ModelingAlgorithms
    DataExchange
)

set(OCCT_KERNEL_MINIMAL_OCCT_LIBRARIES
    TKernel
    TKMath
    TKG2d
    TKG3d
    TKGeomBase
    TKBRep
    TKGeomAlgo
    TKTopAlgo
    TKPrim
    TKBO
    TKBool
    TKFeat
    TKFillet
    TKOffset
    TKShHealing
    TKMesh
    TKXSBase
    TKDESTEP
)

# ---------------------------------------------------------------------------
# OCCT dependency
# ---------------------------------------------------------------------------

# The caller may set OCCT_ROOT to either the OCCT install prefix or directly to
# the directory that contains OCCTConfig.cmake / OpenCASCADEConfig.cmake.
# When building with Emscripten, this should point to the Emscripten build of OCCT.
# When building natively, this should point to the host build of OCCT.

if(DEFINED OCCT_ROOT)
    list(APPEND CMAKE_PREFIX_PATH "${OCCT_ROOT}")
    list(APPEND CMAKE_PREFIX_PATH
        "${OCCT_ROOT}/lib/cmake/opencascade"
        "${OCCT_ROOT}/lib64/cmake/opencascade"
    )
    if(EXISTS "${OCCT_ROOT}/lib/cmake/opencascade/OpenCASCADEConfig.cmake")
        set(OpenCASCADE_DIR "${OCCT_ROOT}/lib/cmake/opencascade")
    elseif(EXISTS "${OCCT_ROOT}/lib64/cmake/opencascade/OpenCASCADEConfig.cmake")
        set(OpenCASCADE_DIR "${OCCT_ROOT}/lib64/cmake/opencascade")
    endif()
endif()

find_package(OpenCASCADE QUIET
    COMPONENTS ${OCCT_KERNEL_REQUIRED_COMPONENTS}
)

if(NOT OpenCASCADE_FOUND)
    find_package(OpenCASCADE QUIET)
endif()

if(NOT OpenCASCADE_FOUND)
    # Try alternative package name used by some OCCT builds
    find_package(OCCT QUIET
        COMPONENTS ${OCCT_KERNEL_REQUIRED_COMPONENTS}
    )
    if(NOT OCCT_FOUND)
        find_package(OCCT QUIET)
    endif()
    if(OCCT_FOUND)
        set(OpenCASCADE_FOUND TRUE)
        set(OpenCASCADE_INCLUDE_DIR ${OCCT_INCLUDE_DIR})
        set(OpenCASCADE_LIBRARIES   ${OCCT_LIBRARIES})
    endif()
endif()

if(NOT OpenCASCADE_FOUND)
    message(WARNING
        "OCCT not found. Set -DOCCT_ROOT=<path> to the installed OCCT directory.\n"
        "To build OCCT for Emscripten: run scripts/build-occt.sh first.\n"
        "The build will continue but the kernel target cannot be compiled."
    )
endif()

set(OCCT_KERNEL_EXTRA_INCLUDE_DIRS)
if(DEFINED OCCT_ROOT)
    set(OCCT_KERNEL_OCCT_BUILD_INCLUDE_DIR "${OCCT_ROOT}/../b/include/opencascade")
    if(EXISTS "${OCCT_KERNEL_OCCT_BUILD_INCLUDE_DIR}/BRepFeat_MakePrism.hxx")
        list(APPEND OCCT_KERNEL_EXTRA_INCLUDE_DIRS "${OCCT_KERNEL_OCCT_BUILD_INCLUDE_DIR}")
    endif()
endif()

# ---------------------------------------------------------------------------
# Source lists
# ---------------------------------------------------------------------------

set(KERNEL_SOURCES
    cpp/kernel.cpp
    cpp/profile_builder.cpp
    cpp/wire_builder.cpp
)

set(BINDING_SOURCES
    bindings/bindings.cpp
)

set(SKETCH_TOOLKIT_SOURCES
    cpp/sketch_toolkit.cpp
    bindings/sketch_bindings.cpp
)

# ---------------------------------------------------------------------------
# WASM target
# ---------------------------------------------------------------------------

if(OCCT_KERNEL_WASM)
    if(NOT EMSCRIPTEN)
        message(FATAL_ERROR "OCCT_KERNEL_WASM requires the Emscripten toolchain. "
                            "Set -DCMAKE_TOOLCHAIN_FILE=<emscripten>/cmake/Modules/Platform/Emscripten.cmake")
    endif()

    set(OCCT_KERNEL_LINK_LIBRARIES ${OpenCASCADE_LIBRARIES})
    if(OCCT_KERNEL_USE_MINIMAL_OCCT_LINK AND DEFINED OCCT_ROOT)
        set(OCCT_KERNEL_LINK_LIBRARIES ${OCCT_KERNEL_MINIMAL_OCCT_LIBRARIES})
        message(STATUS "Using minimal OCCT link set for occt-kernel WASM target")
    endif()

    add_executable(occt-kernel ${KERNEL_SOURCES} ${BINDING_SOURCES})

    target_include_directories(occt-kernel
        PRIVATE
            ${CMAKE_CURRENT_SOURCE_DIR}/cpp
            ${OpenCASCADE_INCLUDE_DIR}
            ${OCCT_KERNEL_EXTRA_INCLUDE_DIRS}
    )

    target_link_libraries(occt-kernel
        PRIVATE ${OCCT_KERNEL_LINK_LIBRARIES}
    )

    # Ensure the linker can find OCCT static libraries by path
    if(DEFINED OCCT_ROOT)
        target_link_directories(occt-kernel PRIVATE "${OCCT_ROOT}/lib")
    endif()

    # Keep longjmp handling on the JS runtime path so OCCT exception support does
    # not resolve to wasm SjLj helper symbols that are incompatible with
    # DISABLE_EXCEPTION_CATCHING=0 on newer distro-packaged Emscripten builds.
    set(EM_COMMON_FLAGS
        "-fexceptions"
        "-sDISABLE_EXCEPTION_CATCHING=0"
        "-sSUPPORT_LONGJMP=emscripten"
    )

    # Emscripten link flags
    set(EM_LINK_FLAGS
        # Use Emscripten embind
        "--bind"
        # C++ exceptions (required for OCCT Standard_Failure propagation)
        # Modularise the JS output so it can be imported as an ES module or CJS
        "-sMODULARIZE=1"
        "-sEXPORT_NAME=createOcctKernelModule"
        "-sENVIRONMENT=web,node"
        # Memory
        "-sINITIAL_MEMORY=128MB"
        "-sALLOW_MEMORY_GROWTH=1"
        "-sMAXIMUM_MEMORY=2GB"
        # Filesystem (needed for STEP import/export via temp files)
        "-sFORCE_FILESYSTEM=1"
        # Stack size
        "-sTOTAL_STACK=8MB"
        # Exports
        "-sEXPORTED_RUNTIME_METHODS=[ccall,cwrap,FS]"
        # Production: strip assertions
        "$<$<CONFIG:Release>:-sASSERTIONS=0>"
        # Fast local iteration: keep native semantics, skip expensive optimization work
        "$<$<CONFIG:Fast>:-O0>"
        "$<$<CONFIG:Fast>:-sASSERTIONS=0>"
        # Debug: keep assertions and source maps
        "$<$<CONFIG:Debug>:-sASSERTIONS=2>"
        "$<$<CONFIG:Debug>:-gsource-map>"
    )

    foreach(flag IN LISTS EM_COMMON_FLAGS)
        target_compile_options(occt-kernel PRIVATE "SHELL:${flag}")
    endforeach()

    # Apply as target link options
    foreach(flag IN LISTS EM_COMMON_FLAGS EM_LINK_FLAGS)
        target_link_options(occt-kernel PRIVATE "SHELL:${flag}")
    endforeach()

    if(OCCT_KERNEL_ENABLE_PTHREADS)
        target_compile_options(occt-kernel PRIVATE -pthread)
        target_link_options(occt-kernel PRIVATE
            "SHELL:-pthread"
            "SHELL:-sPTHREAD_POOL_SIZE=${OCCT_KERNEL_PTHREAD_POOL_SIZE}"
        )
    endif()

    target_compile_options(occt-kernel
        PRIVATE
            "$<$<CONFIG:Release>:-O3>"
            "$<$<CONFIG:Fast>:-O0>"
            "$<$<CONFIG:Debug>:-O0;-g>"
    )

    # Output goes to dist/
    set_target_properties(occt-kernel PROPERTIES
        RUNTIME_OUTPUT_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}/dist"
        OUTPUT_NAME              "${OCCT_KERNEL_ARTIFACT_BASENAME}"
        SUFFIX                   ".js"
    )
endif()

# ---------------------------------------------------------------------------
# Sketch toolkit WASM target
# ---------------------------------------------------------------------------

if(SKETCH_TOOLKIT_WASM)
    if(NOT EMSCRIPTEN)
        message(FATAL_ERROR "SKETCH_TOOLKIT_WASM requires the Emscripten toolchain. "
                            "Set -DCMAKE_TOOLCHAIN_FILE=<emscripten>/cmake/Modules/Platform/Emscripten.cmake")
    endif()

    add_executable(sketch-toolkit ${SKETCH_TOOLKIT_SOURCES})

    target_include_directories(sketch-toolkit
        PRIVATE
            ${CMAKE_CURRENT_SOURCE_DIR}/cpp
    )

    set(SKETCH_TOOLKIT_LINK_FLAGS
        "--bind"
        "-sMODULARIZE=1"
        "-sEXPORT_NAME=createSketchToolkitModule"
        "-sENVIRONMENT=web,node"
        "-sINITIAL_MEMORY=64MB"
        "-sALLOW_MEMORY_GROWTH=1"
        "-sMAXIMUM_MEMORY=512MB"
        "-sFILESYSTEM=0"
        "-sEXPORTED_RUNTIME_METHODS=[ccall,cwrap]"
        "$<$<CONFIG:Release>:-sASSERTIONS=0>"
        "$<$<CONFIG:Fast>:-O0>"
        "$<$<CONFIG:Fast>:-sASSERTIONS=0>"
        "$<$<CONFIG:Debug>:-sASSERTIONS=2>"
        "$<$<CONFIG:Debug>:-gsource-map>"
    )

    foreach(flag IN LISTS EM_COMMON_FLAGS)
        target_compile_options(sketch-toolkit PRIVATE "SHELL:${flag}")
    endforeach()

    foreach(flag IN LISTS EM_COMMON_FLAGS SKETCH_TOOLKIT_LINK_FLAGS)
        target_link_options(sketch-toolkit PRIVATE "SHELL:${flag}")
    endforeach()

    target_compile_options(sketch-toolkit
        PRIVATE
            "$<$<CONFIG:Release>:-O3>"
            "$<$<CONFIG:Fast>:-O0>"
            "$<$<CONFIG:Debug>:-O0;-g>"
    )

    set_target_properties(sketch-toolkit PROPERTIES
        RUNTIME_OUTPUT_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}/dist"
        OUTPUT_NAME              "${SKETCH_TOOLKIT_ARTIFACT_BASENAME}"
        SUFFIX                   ".js"
    )
endif()

# ---------------------------------------------------------------------------
# Native target (for local dev / geometry tests without browser)
# ---------------------------------------------------------------------------

if(OCCT_KERNEL_NATIVE AND OpenCASCADE_FOUND)
    add_library(occt-kernel-native SHARED ${KERNEL_SOURCES})

    target_include_directories(occt-kernel-native
        PRIVATE
            ${CMAKE_CURRENT_SOURCE_DIR}/cpp
            ${OpenCASCADE_INCLUDE_DIR}
            ${OCCT_KERNEL_EXTRA_INCLUDE_DIRS}
    )

    target_link_libraries(occt-kernel-native
        PRIVATE ${OpenCASCADE_LIBRARIES}
    )

    set_target_properties(occt-kernel-native PROPERTIES
        LIBRARY_OUTPUT_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}/build-native"
        OUTPUT_NAME              "occt-kernel"
    )
endif()
