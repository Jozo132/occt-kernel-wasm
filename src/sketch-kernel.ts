import { KernelError, parseNativeError } from './errors';
import { API_VERSION, LIBRARY_VERSION } from './version';
import type {
    Arc2DEntitySpec,
    Circle2DEntitySpec,
    ConstraintId,
    ConstraintSpec,
    DofSummary,
    DiagnosticItem,
    DiagnosticReport,
    DrivenDimensionValue,
    DrivingState,
    EntityId,
    EntitySpec,
    Point2DEntitySpec,
    ScalarSource,
    SketchCreateParams,
    SketchHandle,
    SketchPlaneFrame,
    SketchSnapshot,
    SketchSolveOptions,
    SketchSolveResult,
    SketchStatus,
    SketchToolkitVersionInfo,
    SolveAlgorithm,
    StoredConstraint,
    StoredEntity,
} from './sketch-types';

export interface NativeSketchToolkit {
    getSessionId?: () => string;
    getLastError?: () => string;
    clearLastError?: () => void;
    createSketch(createParamsJson: string): number;
    disposeSketch?: (sketchId: number) => void;
    addEntity(sketchId: number, entitySpecJson: string): number;
    removeEntity?: (sketchId: number, entityId: number) => void;
    addConstraint(sketchId: number, constraintSpecJson: string): number;
    removeConstraint?: (sketchId: number, constraintId: number) => void;
    setParameter?: (sketchId: number, name: string, valueJson: string) => void;
    solveSketch?: (sketchId: number, optionsJson: string) => string;
    getSketchSnapshot(sketchId: number): string;
}

export interface SketchToolkitWasmModule {
    SketchToolkitNative: new () => NativeSketchToolkit;
}

interface NativeSketchSnapshot {
    readonly sessionId?: string;
    readonly id: number;
    readonly createParams?: SketchCreateParams;
    readonly entities: ReadonlyArray<{ readonly id: number; readonly spec: EntitySpec }>;
    readonly constraints: ReadonlyArray<{ readonly id: number; readonly spec: ConstraintSpec }>;
    readonly parameters?: Readonly<Record<string, ScalarSource>>;
}

interface NativeSketchSolvePayload {
    readonly converged?: boolean;
    readonly status?: SketchStatus;
    readonly algorithm?: SolveAlgorithm;
    readonly iterations?: number;
    readonly maxScaledResidual?: number;
    readonly diagnostics?: DiagnosticReport;
}

let sketchToolkitSessionCounter = 0;

function createSessionId(): string {
    sketchToolkitSessionCounter += 1;
    return `sketch_toolkit_${Date.now().toString(36)}_${sketchToolkitSessionCounter.toString(36)}`;
}

function wrapNativeCall<T>(native: NativeSketchToolkit, action: () => T): T {
    native.clearLastError?.();
    try {
        return action();
    } catch (error) {
        const reported = native.getLastError?.();
        if (reported) {
            throw parseNativeError(reported);
        }
        throw parseNativeError(error);
    }
}

function normalizeEntitySpec(spec: EntitySpec): EntitySpec {
    if (spec.kind === 'arc') {
        const startRadians = spec.startRadians ?? (typeof spec.startDegrees === 'number' ? (spec.startDegrees * Math.PI) / 180 : 0);
        const sweepRadians = spec.sweepRadians ?? (typeof spec.sweepDegrees === 'number' ? (spec.sweepDegrees * Math.PI) / 180 : 0);
        return { ...spec, startRadians, sweepRadians };
    }

    if (spec.kind === 'ellipse' || spec.kind === 'elliptical-arc') {
        const angleRadians = spec.angleRadians ?? (typeof spec.angleDegrees === 'number' ? (spec.angleDegrees * Math.PI) / 180 : 0);
        return { ...spec, angleRadians };
    }

    if (spec.kind === 'infinite-line' || spec.kind === 'coordinate-system') {
        const angleRadians = spec.angleRadians ?? (typeof spec.angleDegrees === 'number' ? (spec.angleDegrees * Math.PI) / 180 : 0);
        return { ...spec, angleRadians };
    }

    return spec;
}

function toStoredEntity(entry: { readonly id: number; readonly spec: EntitySpec }): StoredEntity {
    return { id: entry.id, ...normalizeEntitySpec(entry.spec) };
}

function toStoredConstraint(entry: { readonly id: number; readonly spec: ConstraintSpec }): StoredConstraint {
    return { id: entry.id, ...entry.spec };
}

function getScalarValue(value: unknown): ScalarSource | undefined {
    return typeof value === 'number' || typeof value === 'string' ? value : undefined;
}

function structuralDofForEntity(entity: StoredEntity): number {
    switch (entity.kind) {
        case 'point':
            return entity.fixed ? 0 : 2;
        case 'line-segment':
            return 0;
        case 'infinite-line':
            return 3;
        case 'circle':
            return 1;
        case 'arc':
            return typeof entity.startPoint === 'number' && typeof entity.endPoint === 'number' ? 1 : 3;
        case 'ellipse':
            return 3;
        case 'elliptical-arc':
            return 5;
        case 'bspline':
            return entity.controlPoints.length * 2 + (entity.weights?.length ?? 0);
        case 'polyline':
            return 0;
        case 'coordinate-system':
            return 3;
        case 'external-reference':
            return 0;
    }
}

function isDrivenConstraint(constraint: StoredConstraint): boolean {
    return constraint.drivingState === 'driven';
}

function structuralRankForConstraint(constraint: StoredConstraint): number {
    switch (constraint.kind) {
        case 'fix':
        case 'coincident':
            return 2;
        default:
            return 1;
    }
}

function implicitStructuralRankForEntity(entity: StoredEntity): number {
    if (entity.kind !== 'arc') {
        return 0;
    }

    let rank = 0;
    if (typeof entity.startPoint === 'number') {
        rank += 1;
    }
    if (typeof entity.endPoint === 'number') {
        rank += 1;
    }
    return rank;
}

function summarizeDof(entities: readonly StoredEntity[], constraints: readonly StoredConstraint[]): DofSummary {
    let structuralDof = 0;
    let drivingConstraintCount = 0;
    for (const entity of entities) {
        structuralDof += structuralDofForEntity(entity);
        drivingConstraintCount += implicitStructuralRankForEntity(entity);
    }

    let drivenConstraintCount = 0;
    for (const constraint of constraints) {
        if (isDrivenConstraint(constraint)) {
            drivenConstraintCount += structuralRankForConstraint(constraint);
        } else if (constraint.drivingState !== 'suppressed') {
            drivingConstraintCount += structuralRankForConstraint(constraint);
        }
    }

    return {
        structuralDof,
        drivingConstraintCount,
        drivenConstraintCount,
    };
}

function deriveStatus(dof: DofSummary): SketchStatus {
    if (dof.drivingConstraintCount === 0) {
        return 'underdefined';
    }
    if (dof.structuralDof > dof.drivingConstraintCount) {
        return 'underdefined';
    }
    if (dof.structuralDof === dof.drivingConstraintCount) {
        return 'fully-defined';
    }
    return 'overdefined';
}

function buildDiagnostics(status: SketchStatus, dof: DofSummary): DiagnosticReport {
    const items: DiagnosticItem[] = [
        {
            code: 'PHASE1_SCAFFOLD',
            severity: 'info',
            message: 'Phase 1 sketch-toolkit scaffold only: native storage and structural analysis are available, but nonlinear solving is not implemented yet.',
            suggestedAction: 'Next step: compile entities and constraints into residual and Jacobian blocks in the sketch-toolkit WASM target.',
        },
    ];

    if (status === 'underdefined') {
        items.push({
            code: 'STRUCTURAL_UNDERDEFINED',
            severity: 'warning',
            message: 'Structural analysis indicates the sketch still has free degrees of freedom.',
            suggestedAction: 'Add driving dimensions, locks, or geometric constraints before expecting deterministic solves.',
        });
    } else if (status === 'overdefined') {
        items.push({
            code: 'STRUCTURAL_OVERDEFINED',
            severity: 'warning',
            message: 'Structural analysis indicates there are more driving constraints than estimated scalar degrees of freedom.',
            suggestedAction: 'Demote some constraints to driven or remove redundant driving dimensions before the full nonlinear solver lands.',
        });
    }

    return {
        state: status,
        dof,
        items,
        freeDirections: status === 'underdefined'
            ? [{ label: 'structural-free-motion', entityIds: [] }]
            : [],
    };
}

function isDrivingConstraint(constraint: StoredConstraint): boolean {
    return constraint.drivingState !== 'suppressed' && !isDrivenConstraint(constraint);
}

function buildDrivingDimensionConversionDiagnostic(snapshot: SketchSnapshot, status: SketchStatus): DiagnosticItem | undefined {
    if (status !== 'overdefined') {
        return undefined;
    }

    const candidates: StoredConstraint[] = [];
    const seenNames = new Set<string>();

    for (const constraint of snapshot.constraints) {
        if (!isDrivingConstraint(constraint)) {
            continue;
        }

        const name = typeof constraint.name === 'string' ? constraint.name.trim() : '';
        if (!name.startsWith('dimension:') || seenNames.has(name)) {
            continue;
        }

        seenNames.add(name);
        candidates.push(constraint);
    }

    if (candidates.length === 0) {
        return undefined;
    }

    const labels = candidates.map((constraint) => constraint.name as string);
    const listed = labels.slice(0, 3).join(', ');
    const truncated = labels.length > 3 ? ', ...' : '';

    return {
        code: 'DRIVING_DIMENSION_CONVERSION_CANDIDATES',
        severity: 'warning',
        message: `Driving dimensions are likely redundancy candidates in the current overdefined sketch: ${listed}${truncated}.`,
        constraintIds: candidates.map((constraint) => constraint.id),
        suggestedAction: 'Convert one of the listed dimensions to driven or remove another redundant driver before expecting a stable fully-defined solve.',
    };
}

function appendDerivedDiagnostics(diagnostics: DiagnosticReport, snapshot: SketchSnapshot, status: SketchStatus): DiagnosticReport {
    const drivingDimensionDiagnostic = buildDrivingDimensionConversionDiagnostic(snapshot, status);
    if (!drivingDimensionDiagnostic || diagnostics.items.some((item) => item.code === drivingDimensionDiagnostic.code)) {
        return diagnostics;
    }

    return {
        ...diagnostics,
        state: status,
        items: [...diagnostics.items, drivingDimensionDiagnostic],
    };
}

function findEntityById(entities: readonly StoredEntity[], entityId: EntityId): StoredEntity | undefined {
    return entities.find((entity) => entity.id === entityId);
}

function findPointById(entities: readonly StoredEntity[], pointId: EntityId): Extract<StoredEntity, { kind: 'point' }> | undefined {
    const entity = findEntityById(entities, pointId);
    return entity?.kind === 'point' ? entity : undefined;
}

function findLineSegmentById(entities: readonly StoredEntity[], lineId: EntityId): Extract<StoredEntity, { kind: 'line-segment' }> | undefined {
    const entity = findEntityById(entities, lineId);
    return entity?.kind === 'line-segment' ? entity : undefined;
}

function findCurveById(entities: readonly StoredEntity[], curveId: EntityId): Extract<StoredEntity, { kind: 'circle' | 'arc' }> | undefined {
    const entity = findEntityById(entities, curveId);
    return entity?.kind === 'circle' || entity?.kind === 'arc' ? entity : undefined;
}

function pointDistance(pointA: Extract<StoredEntity, { kind: 'point' }>, pointB: Extract<StoredEntity, { kind: 'point' }>): number {
    return Math.hypot(pointB.x - pointA.x, pointB.y - pointA.y);
}

function pointLineDistance(
    point: Extract<StoredEntity, { kind: 'point' }>,
    line: Extract<StoredEntity, { kind: 'line-segment' }>,
    entities: readonly StoredEntity[],
): number | undefined {
    const start = findPointById(entities, line.start);
    const end = findPointById(entities, line.end);
    if (!start || !end) {
        return undefined;
    }
    const dx = end.x - start.x;
    const dy = end.y - start.y;
    const len = Math.hypot(dx, dy);
    if (len <= 1.0e-12) {
        return undefined;
    }
    return Math.abs(dx * (start.y - point.y) - dy * (start.x - point.x)) / len;
}

function lineLineDistance(
    lineA: Extract<StoredEntity, { kind: 'line-segment' }>,
    lineB: Extract<StoredEntity, { kind: 'line-segment' }>,
    entities: readonly StoredEntity[],
): number | undefined {
    const anchor = findPointById(entities, lineA.start);
    if (!anchor) {
        return undefined;
    }
    return pointLineDistance(anchor, lineB, entities);
}

function lineAngle(
    lineA: Extract<StoredEntity, { kind: 'line-segment' }>,
    lineB: Extract<StoredEntity, { kind: 'line-segment' }>,
    entities: readonly StoredEntity[],
): number | undefined {
    const a0 = findPointById(entities, lineA.start);
    const a1 = findPointById(entities, lineA.end);
    const b0 = findPointById(entities, lineB.start);
    const b1 = findPointById(entities, lineB.end);
    if (!a0 || !a1 || !b0 || !b1) {
        return undefined;
    }
    const angleA = Math.atan2(a1.y - a0.y, a1.x - a0.x);
    const angleB = Math.atan2(b1.y - b0.y, b1.x - b0.x);
    let diff = angleB - angleA;
    while (diff > Math.PI) {
        diff -= 2 * Math.PI;
    }
    while (diff < -Math.PI) {
        diff += 2 * Math.PI;
    }
    return diff;
}

function withMeasuredDrivenValue(constraint: StoredConstraint, value: ScalarSource | undefined): DrivenDimensionValue {
    return {
        constraintId: constraint.id,
        ...(typeof value !== 'undefined' ? { value } : {}),
        ...(typeof constraint.kind === 'string' ? { kind: constraint.kind } : {}),
        ...(typeof constraint.name === 'string' ? { name: constraint.name } : {}),
        ...(typeof constraint.drivingState === 'string' ? { drivingState: constraint.drivingState as DrivingState } : {}),
    };
}

function measureDrivenConstraint(constraint: StoredConstraint, entities: readonly StoredEntity[]): ScalarSource | undefined {
    switch (constraint.kind) {
        case 'distance-point-point': {
            const pointA = findPointById(entities, constraint.pointA);
            const pointB = findPointById(entities, constraint.pointB);
            if (!pointA || !pointB) {
                return undefined;
            }
            return pointDistance(pointA, pointB);
        }
        case 'distance-point-line': {
            const point = findPointById(entities, constraint.point);
            const line = findLineSegmentById(entities, constraint.line);
            if (!point || !line) {
                return undefined;
            }
            return pointLineDistance(point, line, entities);
        }
        case 'distance-line-line': {
            const lineA = findLineSegmentById(entities, constraint.lineA);
            const lineB = findLineSegmentById(entities, constraint.lineB);
            if (!lineA || !lineB) {
                return undefined;
            }
            return lineLineDistance(lineA, lineB, entities);
        }
        case 'radius': {
            const curve = findCurveById(entities, constraint.entity);
            if (!curve) {
                return undefined;
            }
            const radius = typeof curve.radius === 'number' ? curve.radius : Number(curve.radius);
            return Number.isFinite(radius) ? radius : undefined;
        }
        case 'diameter': {
            const curve = findCurveById(entities, constraint.entity);
            if (!curve) {
                return undefined;
            }
            const radius = typeof curve.radius === 'number' ? curve.radius : Number(curve.radius);
            return Number.isFinite(radius) ? radius * 2 : undefined;
        }
        case 'angle': {
            const lineA = findLineSegmentById(entities, constraint.lineA);
            const lineB = findLineSegmentById(entities, constraint.lineB);
            if (!lineA || !lineB) {
                return undefined;
            }
            return lineAngle(lineA, lineB, entities);
        }
        default:
            return getScalarValue((constraint as unknown as Record<string, unknown>).value);
    }
}

function buildMeasuredDrivenDimensions(snapshot: SketchSnapshot): readonly DrivenDimensionValue[] {
    return snapshot.constraints
        .filter((constraint) => isDrivenConstraint(constraint))
        .map((constraint) => withMeasuredDrivenValue(constraint, measureDrivenConstraint(constraint, snapshot.entities)));
}

function parseSnapshot(rawSnapshot: string): NativeSketchSnapshot {
    try {
        return JSON.parse(rawSnapshot) as NativeSketchSnapshot;
    } catch (error) {
        const detail = error instanceof Error ? error.message : String(error);
        throw new KernelError('OPERATION_FAILED', `Native sketch toolkit returned invalid snapshot JSON: ${detail}`);
    }
}

function parseNativeSolvePayload(rawPayload: string): NativeSketchSolvePayload {
    try {
        return JSON.parse(rawPayload) as NativeSketchSolvePayload;
    } catch (error) {
        const detail = error instanceof Error ? error.message : String(error);
        throw new KernelError('OPERATION_FAILED', `Native sketch toolkit returned invalid solve JSON: ${detail}`);
    }
}

export class SketchToolkit {
    private readonly native: NativeSketchToolkit;
    private readonly sessionId: string;

    constructor(module: SketchToolkitWasmModule) {
        this.native = new module.SketchToolkitNative();
        const nativeSessionId = this.native.getSessionId?.();
        this.sessionId = typeof nativeSessionId === 'string' && nativeSessionId.length > 0
            ? nativeSessionId
            : createSessionId();
    }

    createSketch(params: SketchCreateParams): SketchHandle {
        const id = wrapNativeCall(this.native, () => this.native.createSketch(JSON.stringify(params)));
        if (!Number.isInteger(id) || id <= 0) {
            throw new KernelError('OPERATION_FAILED', `Native sketch toolkit returned an invalid sketch id: ${String(id)}`);
        }
        return Object.freeze({ id, sessionId: this.sessionId });
    }

    disposeSketch(handle: SketchHandle): void {
        const sketchId = this.resolveSketchId(handle, 'sketch');
        wrapNativeCall(this.native, () => this.native.disposeSketch?.(sketchId));
    }

    addEntity(handle: SketchHandle, spec: EntitySpec): EntityId {
        const sketchId = this.resolveSketchId(handle, 'sketch');
        const entityId = wrapNativeCall(this.native, () => this.native.addEntity(sketchId, JSON.stringify(normalizeEntitySpec(spec))));
        if (!Number.isInteger(entityId) || entityId <= 0) {
            throw new KernelError('OPERATION_FAILED', `Native sketch toolkit returned an invalid entity id: ${String(entityId)}`);
        }
        return entityId;
    }

    addPoint(handle: SketchHandle, spec: Omit<Point2DEntitySpec, 'kind'>): EntityId {
        return this.addEntity(handle, { kind: 'point', ...spec });
    }

    addLineSegment(handle: SketchHandle, spec: Omit<Extract<EntitySpec, { kind: 'line-segment' }>, 'kind'>): EntityId {
        return this.addEntity(handle, { kind: 'line-segment', ...spec });
    }

    addCircle(handle: SketchHandle, spec: Omit<Circle2DEntitySpec, 'kind'>): EntityId {
        return this.addEntity(handle, { kind: 'circle', ...spec });
    }

    addArc(handle: SketchHandle, spec: Omit<Arc2DEntitySpec, 'kind'>): EntityId {
        return this.addEntity(handle, { kind: 'arc', ...spec });
    }

    removeEntity(handle: SketchHandle, entityId: EntityId): void {
        const sketchId = this.resolveSketchId(handle, 'sketch');
        wrapNativeCall(this.native, () => this.native.removeEntity?.(sketchId, entityId));
    }

    addConstraint(handle: SketchHandle, spec: ConstraintSpec): ConstraintId {
        const sketchId = this.resolveSketchId(handle, 'sketch');
        const constraintId = wrapNativeCall(this.native, () => this.native.addConstraint(sketchId, JSON.stringify(spec)));
        if (!Number.isInteger(constraintId) || constraintId <= 0) {
            throw new KernelError('OPERATION_FAILED', `Native sketch toolkit returned an invalid constraint id: ${String(constraintId)}`);
        }
        return constraintId;
    }

    removeConstraint(handle: SketchHandle, constraintId: ConstraintId): void {
        const sketchId = this.resolveSketchId(handle, 'sketch');
        wrapNativeCall(this.native, () => this.native.removeConstraint?.(sketchId, constraintId));
    }

    setParameter(handle: SketchHandle, name: string, value: ScalarSource): void {
        const sketchId = this.resolveSketchId(handle, 'sketch');
        wrapNativeCall(this.native, () => this.native.setParameter?.(sketchId, name, JSON.stringify(value)));
    }

    getSketchSnapshot(handle: SketchHandle): SketchSnapshot {
        const sketchId = this.resolveSketchId(handle, 'sketch');
        const rawSnapshot = wrapNativeCall(this.native, () => this.native.getSketchSnapshot(sketchId));
        const parsed = parseSnapshot(rawSnapshot);
        const createParams = parsed.createParams;

        return {
            handle,
            name: createParams?.name ?? `Sketch${handle.id}`,
            plane: createParams?.plane ?? defaultPlane(),
            entities: parsed.entities.map(toStoredEntity),
            constraints: parsed.constraints.map(toStoredConstraint),
            parameters: Object.freeze({ ...(parsed.parameters ?? {}) }),
        };
    }

    solveSketch(handle: SketchHandle, options: SketchSolveOptions = {}): SketchSolveResult {
        if (this.native.solveSketch) {
            const sketchId = this.resolveSketchId(handle, 'sketch');
            const rawPayload = wrapNativeCall(this.native, () => this.native.solveSketch?.(sketchId, JSON.stringify(options)) ?? '');
            const parsed = parseNativeSolvePayload(rawPayload);
            const snapshot = this.getSketchSnapshot(handle);
            const dof = parsed.diagnostics?.dof ?? summarizeDof(snapshot.entities, snapshot.constraints);
            const status = parsed.status ?? parsed.diagnostics?.state ?? deriveStatus(dof);
            const diagnostics = appendDerivedDiagnostics(parsed.diagnostics ?? buildDiagnostics(status, dof), snapshot, status);

            return {
                converged: parsed.converged ?? false,
                status,
                algorithm: parsed.algorithm ?? options.algorithm ?? 'auto',
                iterations: parsed.iterations ?? 0,
                maxScaledResidual: parsed.maxScaledResidual ?? 0,
                diagnostics,
                drivenDimensions: buildMeasuredDrivenDimensions(snapshot),
            };
        }

        const snapshot = this.getSketchSnapshot(handle);
        const dof = summarizeDof(snapshot.entities, snapshot.constraints);
        const status = deriveStatus(dof);
        const algorithm: SolveAlgorithm = options.algorithm ?? 'auto';
        const diagnostics = appendDerivedDiagnostics(buildDiagnostics(status, dof), snapshot, status);

        return {
            converged: false,
            status,
            algorithm,
            iterations: 0,
            maxScaledResidual: 0,
            diagnostics,
            drivenDimensions: buildMeasuredDrivenDimensions(snapshot),
        };
    }

    getVersionInfo(): SketchToolkitVersionInfo {
        return {
            libraryVersion: LIBRARY_VERSION,
            apiVersion: API_VERSION,
            target: 'sketch-toolkit',
            status: 'phase1-scaffold',
        };
    }

    private resolveSketchId(handle: SketchHandle, name: string): number {
        if (!Number.isInteger(handle.id) || handle.id <= 0) {
            throw new KernelError('INVALID_HANDLE', `${name} must contain a positive integer id`);
        }
        if (typeof handle.sessionId !== 'string' || handle.sessionId.length === 0) {
            throw new KernelError('INVALID_HANDLE', `${name} was not created by a valid sketch toolkit session`);
        }
        if (handle.sessionId !== this.sessionId) {
            throw new KernelError('SESSION_MISMATCH', `${name} belongs to sketch toolkit session ${handle.sessionId}, not ${this.sessionId}`);
        }
        return handle.id;
    }
}

function defaultPlane(): SketchPlaneFrame {
    return {
        origin: [0, 0, 0],
        normal: [0, 0, 1],
        xAxis: [1, 0, 0],
        yAxis: [0, 1, 0],
    };
}
