/**
 * @file sketch_bindings.cpp
 * @brief Emscripten embind boundary for the sketch-toolkit WASM target.
 */

#include <emscripten/bind.h>

#include "../cpp/sketch_toolkit.h"

using namespace emscripten;
using namespace occt_kernel;

EMSCRIPTEN_BINDINGS(sketch_toolkit_module) {
    class_<SketchToolkitNative>("SketchToolkitNative")
        .constructor()
        .function("getSessionId", &SketchToolkitNative::getSessionId)
        .function("createSketch", &SketchToolkitNative::createSketch)
        .function("disposeSketch", &SketchToolkitNative::disposeSketch)
        .function("addEntity", &SketchToolkitNative::addEntity)
        .function("removeEntity", &SketchToolkitNative::removeEntity)
        .function("addConstraint", &SketchToolkitNative::addConstraint)
        .function("removeConstraint", &SketchToolkitNative::removeConstraint)
        .function("setParameter", &SketchToolkitNative::setParameter)
        .function("solveSketch", &SketchToolkitNative::solveSketch)
        .function("getSketchSnapshot", &SketchToolkitNative::getSketchSnapshot)
        .function("getLastError", &SketchToolkitNative::getLastError)
        .function("clearLastError", &SketchToolkitNative::clearLastError);
}
