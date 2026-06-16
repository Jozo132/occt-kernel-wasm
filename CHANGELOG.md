# Changelog

## Unreleased

- Added `getPlanarFaceWires()` to expose exact trimmed planar-face loops as both local 2-D plane curves and world-space 3-D edge curves for CAM, profile recovery, and other exact boundary workflows.

## 1.1.0 - 2026-06-04

- Added a generic `previewFeature()` API for non-committing live feature previews across exact feature, boolean, and transform operations.
- Added lightweight tessellation controls and selected-face tessellation to reduce preview mesh cost.
- Improved native exact blend diagnostics, including structured chamfer/fillet failure reporting and more stable regression coverage for WASM integration tests.