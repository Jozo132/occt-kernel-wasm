export type SketchId = number;
export type EntityId = number;
export type ConstraintId = number;
export type Point2 = readonly [number, number];
export type Point3 = readonly [number, number, number];
export type ScalarSource = number | string;

export type LengthUnit = 'model' | 'mm' | 'cm' | 'm' | 'in';
export type ReferenceMode = 'normal' | 'construction' | 'external' | 'derived';
export type DrivingState = 'driving' | 'driven' | 'soft' | 'suppressed';
export type ConstraintPriority = 'required' | 'high' | 'normal' | 'low';
export type SolveMode = 'authoring' | 'drag' | 'diagnostic';
export type SolveAlgorithm = 'auto' | 'gauss-seidel' | 'newton' | 'gauss-newton' | 'lm' | 'dogleg' | 'bfgs' | 'lbfgs' | 'sqp';
export type DiagnosticSeverity = 'info' | 'warning' | 'error';
export type SketchStatus = 'underdefined' | 'fully-defined' | 'overdefined' | 'inconsistent' | 'failed';
export type ExternalReferenceKind = 'vertex' | 'edge' | 'face' | 'axis' | 'plane';

export interface SketchHandle {
    readonly id: SketchId;
    readonly sessionId: string;
}

export interface SketchPlaneFrame {
    readonly origin: Point3;
    readonly normal: Point3;
    readonly xAxis: Point3;
    readonly yAxis: Point3;
    readonly unit?: LengthUnit;
}

export interface SketchCreateParams {
    readonly name?: string;
    readonly plane: SketchPlaneFrame;
}

export interface BaseEntitySpec {
    readonly name?: string;
    readonly construction?: boolean;
    readonly visible?: boolean;
    readonly suppressed?: boolean;
    readonly referenceMode?: ReferenceMode;
}

export interface Point2DEntitySpec extends BaseEntitySpec {
    readonly kind: 'point';
    readonly x: number;
    readonly y: number;
    readonly fixed?: boolean;
}

export interface LineSegment2DEntitySpec extends BaseEntitySpec {
    readonly kind: 'line-segment';
    readonly start: EntityId;
    readonly end: EntityId;
}

export interface InfiniteLine2DEntitySpec extends BaseEntitySpec {
    readonly kind: 'infinite-line';
    readonly anchor: Point2;
    readonly angleRadians?: number;
    readonly angleDegrees?: number;
}

export interface Circle2DEntitySpec extends BaseEntitySpec {
    readonly kind: 'circle';
    readonly center: EntityId;
    readonly radius: ScalarSource;
}

export interface Arc2DEntitySpec extends BaseEntitySpec {
    readonly kind: 'arc';
    readonly center: EntityId;
    readonly radius: ScalarSource;
    readonly startPoint?: EntityId;
    readonly endPoint?: EntityId;
    readonly startRadians?: number;
    readonly startDegrees?: number;
    readonly sweepRadians?: number;
    readonly sweepDegrees?: number;
}

export interface Ellipse2DEntitySpec extends BaseEntitySpec {
    readonly kind: 'ellipse';
    readonly center: EntityId;
    readonly majorRadius: ScalarSource;
    readonly minorRadius: ScalarSource;
    readonly angleRadians?: number;
    readonly angleDegrees?: number;
}

export interface EllipticalArc2DEntitySpec extends BaseEntitySpec {
    readonly kind: 'elliptical-arc';
    readonly center: EntityId;
    readonly majorRadius: ScalarSource;
    readonly minorRadius: ScalarSource;
    readonly angleRadians?: number;
    readonly angleDegrees?: number;
    readonly startParameter: number;
    readonly endParameter: number;
}

export interface Spline2DEntitySpec extends BaseEntitySpec {
    readonly kind: 'bspline';
    readonly controlPoints: readonly EntityId[];
    readonly degree: number;
    readonly knots: readonly number[];
    readonly multiplicities: readonly number[];
    readonly periodic?: boolean;
    readonly weights?: readonly number[];
}

export interface Polyline2DEntitySpec extends BaseEntitySpec {
    readonly kind: 'polyline';
    readonly points: readonly EntityId[];
    readonly closed?: boolean;
}

export interface CoordinateSystem2DEntitySpec extends BaseEntitySpec {
    readonly kind: 'coordinate-system';
    readonly origin: Point2;
    readonly angleRadians?: number;
    readonly angleDegrees?: number;
}

export interface ExternalReferenceEntitySpec extends BaseEntitySpec {
    readonly kind: 'external-reference';
    readonly referenceKind: ExternalReferenceKind;
    readonly token: string;
}

export type EntitySpec =
    | Point2DEntitySpec
    | LineSegment2DEntitySpec
    | InfiniteLine2DEntitySpec
    | Circle2DEntitySpec
    | Arc2DEntitySpec
    | Ellipse2DEntitySpec
    | EllipticalArc2DEntitySpec
    | Spline2DEntitySpec
    | Polyline2DEntitySpec
    | CoordinateSystem2DEntitySpec
    | ExternalReferenceEntitySpec;

export interface BaseConstraintSpec {
    readonly name?: string;
    readonly drivingState?: DrivingState;
    readonly priority?: ConstraintPriority;
}

export interface CoincidentConstraintSpec extends BaseConstraintSpec {
    readonly kind: 'coincident';
    readonly pointA: EntityId;
    readonly pointB: EntityId;
}

export interface DistancePointPointConstraintSpec extends BaseConstraintSpec {
    readonly kind: 'distance-point-point';
    readonly pointA: EntityId;
    readonly pointB: EntityId;
    readonly value: ScalarSource;
}

export interface DistancePointLineConstraintSpec extends BaseConstraintSpec {
    readonly kind: 'distance-point-line';
    readonly point: EntityId;
    readonly line: EntityId;
    readonly value: ScalarSource;
}

export interface DistanceLineLineConstraintSpec extends BaseConstraintSpec {
    readonly kind: 'distance-line-line';
    readonly lineA: EntityId;
    readonly lineB: EntityId;
    readonly value: ScalarSource;
}

export interface UnaryEntityConstraintSpec extends BaseConstraintSpec {
    readonly kind: 'horizontal' | 'vertical' | 'fix' | 'radius' | 'diameter';
    readonly entity: EntityId;
    readonly value?: ScalarSource;
}

export interface BinaryEntityConstraintSpec extends BaseConstraintSpec {
    readonly kind:
        | 'parallel'
        | 'perpendicular'
        | 'tangent'
        | 'equal-length'
        | 'equal-radius'
        | 'concentric'
        | 'midpoint'
        | 'symmetry'
        | 'curve-tangent'
        | 'curve-curvature-continuity';
    readonly entityA: EntityId;
    readonly entityB: EntityId;
    readonly helperEntity?: EntityId;
}

export interface AngleConstraintSpec extends BaseConstraintSpec {
    readonly kind: 'angle';
    readonly lineA: EntityId;
    readonly lineB: EntityId;
    readonly value: ScalarSource;
}

export interface PointOnEntityConstraintSpec extends BaseConstraintSpec {
    readonly kind: 'point-on-curve' | 'point-on-circle' | 'point-on-arc' | 'point-on-ellipse';
    readonly point: EntityId;
    readonly entity: EntityId;
}

export interface AlgebraicConstraintSpec extends BaseConstraintSpec {
    readonly kind: 'algebraic';
    readonly expression: string;
}

export interface NamedParameterConstraintSpec extends BaseConstraintSpec {
    readonly kind: 'named-parameter';
    readonly parameter: string;
    readonly expression: ScalarSource;
}

export type ConstraintSpec =
    | CoincidentConstraintSpec
    | DistancePointPointConstraintSpec
    | DistancePointLineConstraintSpec
    | DistanceLineLineConstraintSpec
    | UnaryEntityConstraintSpec
    | BinaryEntityConstraintSpec
    | AngleConstraintSpec
    | PointOnEntityConstraintSpec
    | AlgebraicConstraintSpec
    | NamedParameterConstraintSpec;

export interface TemporaryConstraintSpec {
    readonly kind:
        | 'drag-anchor'
        | 'snap-grid'
        | 'snap-point'
        | 'snap-line'
        | 'horizontal-inference'
        | 'vertical-inference'
        | 'tangent-inference'
        | 'coincident-preview'
        | 'alignment-preview'
        | 'temporary-fix';
    readonly weight?: number;
    readonly hard?: boolean;
    readonly entity?: EntityId;
    readonly target?: Point2;
}

export interface SketchSolveOptions {
    readonly mode?: SolveMode;
    readonly algorithm?: SolveAlgorithm;
    readonly maxIterations?: number;
    readonly residualTolerance?: number;
    readonly stepTolerance?: number;
    readonly temporaryConstraints?: readonly TemporaryConstraintSpec[];
}

export interface DofSummary {
    readonly structuralDof: number;
    readonly drivingConstraintCount: number;
    readonly drivenConstraintCount: number;
}

export interface NullspaceDirection {
    readonly label: string;
    readonly entityIds: readonly EntityId[];
}

export interface DiagnosticItem {
    readonly code: string;
    readonly severity: DiagnosticSeverity;
    readonly message: string;
    readonly entityIds?: readonly EntityId[];
    readonly constraintIds?: readonly ConstraintId[];
    readonly suggestedAction?: string;
}

export interface DiagnosticReport {
    readonly state: SketchStatus;
    readonly dof: DofSummary;
    readonly items: readonly DiagnosticItem[];
    readonly freeDirections: readonly NullspaceDirection[];
}

export interface DrivenDimensionValue {
    readonly constraintId: ConstraintId;
    readonly value?: ScalarSource;
    readonly kind?: string;
    readonly name?: string;
    readonly drivingState?: DrivingState;
}

export type SmartDimensionKind = 'distance' | 'dx' | 'dy' | 'angle' | 'radius' | 'diameter';
export type SmartSegmentEndpointKey = 'p1' | 'p2';

export interface SmartDimensionEntityLikeBase {
    readonly id?: EntityId;
}

export interface SmartPointLike extends SmartDimensionEntityLikeBase {
    readonly type: 'point';
    readonly x: number;
    readonly y: number;
}

export interface SmartSegmentLike extends SmartDimensionEntityLikeBase {
    readonly type: 'segment';
    readonly x1: number;
    readonly y1: number;
    readonly x2: number;
    readonly y2: number;
    readonly midX?: number;
    readonly midY?: number;
    readonly p1?: { readonly x: number; readonly y: number };
    readonly p2?: { readonly x: number; readonly y: number };
}

export interface SmartCurveLike extends SmartDimensionEntityLikeBase {
    readonly type: 'circle' | 'arc';
    readonly cx: number;
    readonly cy: number;
    readonly radius: number;
    readonly sweepAngle?: number;
}

export type SmartDimensionEntityLike = SmartPointLike | SmartSegmentLike | SmartCurveLike;

export interface SmartSegmentAngleInfo {
    readonly vx: number;
    readonly vy: number;
    readonly startAngle: number;
    readonly sweep: number;
    readonly angleEndpointAKey: SmartSegmentEndpointKey | null;
    readonly angleEndpointBKey: SmartSegmentEndpointKey | null;
}

export interface SmartDimensionCandidate {
    readonly dimType: SmartDimensionKind;
    readonly label?: string;
    readonly x1: number;
    readonly y1: number;
    readonly x2: number;
    readonly y2: number;
    readonly angleStart?: number;
    readonly angleSweep?: number;
    readonly angleEndpointAKey?: SmartSegmentEndpointKey | null;
    readonly angleEndpointBKey?: SmartSegmentEndpointKey | null;
    readonly sourceA?: SmartDimensionEntityLike;
    readonly sourceB?: SmartDimensionEntityLike | null;
}

export interface SketchSolveResult {
    readonly converged: boolean;
    readonly status: SketchStatus;
    readonly algorithm: SolveAlgorithm;
    readonly iterations: number;
    readonly maxScaledResidual: number;
    readonly diagnostics: DiagnosticReport;
    readonly drivenDimensions: readonly DrivenDimensionValue[];
}

export type StoredEntity = EntitySpec & {
    readonly id: EntityId;
};

export type StoredConstraint = ConstraintSpec & {
    readonly id: ConstraintId;
};

export interface SketchSnapshot {
    readonly handle: SketchHandle;
    readonly name: string;
    readonly plane: SketchPlaneFrame;
    readonly entities: readonly StoredEntity[];
    readonly constraints: readonly StoredConstraint[];
    readonly parameters: Readonly<Record<string, ScalarSource>>;
}

export interface SketchToolkitVersionInfo {
    readonly libraryVersion: string;
    readonly apiVersion: number;
    readonly target: 'sketch-toolkit';
    readonly status: 'phase1-scaffold';
}
