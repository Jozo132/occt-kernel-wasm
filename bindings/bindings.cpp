/**
 * @file bindings.cpp
 * @brief Emscripten embind boundary between the OcctKernel C++ class and JavaScript.
 *
 * This file is compiled only when targeting WebAssembly with Emscripten.
 * It registers the OcctKernel class and all its methods with the embind runtime.
 *
 * Design: all types that cross the boundary are primitive C++ types or std::string.
 * No OCCT classes are exposed to JavaScript.
 */

#include <emscripten/bind.h>
#include "../cpp/kernel.h"

using namespace emscripten;
using namespace occt_kernel;

EMSCRIPTEN_BINDINGS(occt_kernel_module) {
    class_<OcctKernel>("OcctKernel")
        .constructor()

        // Primitives
        .function("createBox",      &OcctKernel::createBox)
        .function("createCylinder", &OcctKernel::createCylinder)
        .function("createSphere",   &OcctKernel::createSphere)

        // Sketch-based features
        .function("extrudeProfile", &OcctKernel::extrudeProfile)
        .function("revolveProfile", &OcctKernel::revolveProfile)

        // Booleans
        .function("booleanUnion",      &OcctKernel::booleanUnion)
        .function("booleanSubtract",   &OcctKernel::booleanSubtract)
        .function("booleanIntersect",  &OcctKernel::booleanIntersect)

        // Modifiers
        .function("filletEdges",  &OcctKernel::filletEdges)
        .function("chamferEdges", &OcctKernel::chamferEdges)
        .function("transformShape", &OcctKernel::transformShape)

        // Queries
        .function("getTopology",    &OcctKernel::getTopology)
        .function("checkValidity",  &OcctKernel::checkValidity)

        // Tessellation
        .function("tessellate", &OcctKernel::tessellate)

        // Import / export
        .function("importStep", &OcctKernel::importStep)
        .function("importStepDetailed", &OcctKernel::importStepDetailed)
        .function("exportStep", &OcctKernel::exportStep)

        // Memory
        .function("disposeShape", &OcctKernel::disposeShape)
        ;
}
