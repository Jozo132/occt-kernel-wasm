/**
 * Shared public API surface for all published entry points.
 */

export type {
    BooleanParams,
    BoundingBox,
    BoxParams,
    ChamferParams,
    CylinderParams,
    DisposeParams,
    ExportStepParams,
    ExtrudeParams,
    FilletParams,
    ImportStepParams,
    Profile,
    ProfileSegment,
    ProfileSegmentArc,
    ProfileSegmentCircle,
    ProfileSegmentLine,
    RevolveParams,
    ShapeHandle,
    SphereParams,
    TessellateParams,
    TessellationResult,
    TopologyResult,
} from './types';

export { KernelError } from './errors';
export type { KernelErrorCode } from './errors';
export { OcctKernel } from './kernel';
export type { NativeKernel, WasmModule } from './kernel';