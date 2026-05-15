/**
 * @file profile_builder.h
 * @brief Converts a JSON 2-D profile descriptor into OCCT wire / face objects.
 *
 * Supported segment types:
 *   "line"   – start/end points
 *   "arc"    – start/mid/end points (3-point arc)
 *   "circle" – centre + radius (full circle → single edge wire)
 */

#pragma once

#include <string>

#include <TopoDS_Face.hxx>
#include <TopoDS_Wire.hxx>

namespace occt_kernel {

/**
 * Parse a JSON profile descriptor and return a planar OCCT face in the XY plane.
 *
 * The profile JSON schema mirrors the TypeScript `Profile` type:
 * ```json
 * {
 *   "wires": [
 *     {
 *       "segments": [
 *         { "type": "line",   "start": [0,0], "end": [10,0] },
 *         { "type": "arc",    "start": [10,0], "mid": [15,5], "end": [10,10] },
 *         { "type": "circle", "centre": [0,0], "radius": 5 }
 *       ]
 *     }
 *   ]
 * }
 * ```
 *
 * Backward compatibility: `{ "segments": [...] }` is still treated as a
 * single-wire profile, and `{ "outer": ..., "holes": [...] }` is supported.
 *
 * @throws std::runtime_error on parse failure or open wire.
 */
TopoDS_Face buildFaceFromProfile(const std::string& profileJson);

/**
 * Same as buildFaceFromProfile but returns the wire without filling.
 * Useful for revolution where only the wire is needed.
 */
TopoDS_Wire buildWireFromProfile(const std::string& profileJson);

} // namespace occt_kernel
