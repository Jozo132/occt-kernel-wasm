/**
 * @file wire_builder.h
 * @brief Converts a JSON 3-D wire descriptor into an OCCT wire.
 */

#pragma once

#include <string>

#include <TopoDS_Wire.hxx>

namespace occt_kernel {

/**
 * Parse a JSON spatial-wire descriptor and return an OCCT wire.
 *
 * Supported segment types mirror the TypeScript `SpatialCurveSegment` union:
 * `line`, `arc`, `circle`, `bezier`, and `bspline`.
 *
 * @throws std::runtime_error on parse failure, invalid segment contracts,
 *         or an open wire when `requireClosed` is true.
 */
TopoDS_Wire buildSpatialWireFromJson(const std::string& wireJson, bool requireClosed = false);

} // namespace occt_kernel