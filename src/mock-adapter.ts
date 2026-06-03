/**
 * Mock adapter for the OCCT WASM module.
 *
 * This module is loaded instead of the real compiled WASM binary when running
 * TypeScript tests in Node.js without a built WASM binary. It provides enough
 * of the NativeKernel interface to allow the TypeScript wrapper to be tested.
 *
 * The mock does NOT perform real geometry. It is deliberately minimal and only
 * exercises the JS/TS layer logic (parameter validation, error propagation, etc.).
 *
 * Real geometry regression tests require the compiled WASM binary and are skipped
 * when it is absent.
 */

import { KernelError } from './errors';

// ---------------------------------------------------------------------------
// Internal shape store
// ---------------------------------------------------------------------------

interface MockShape {
    kind: string;
    params: Record<string, unknown>;
}

interface MockImportResult {
    readStatus: string;
    transferStatus: string;
    rootCount: number;
    transferredRootCount: number;
    messageList: Array<{
        phase: 'load' | 'transfer' | 'heal' | 'validation';
        severity: 'info' | 'warning' | 'fail';
        text: string;
        entityNumber?: number;
    }>;
    shapeId?: number;
    isValid: boolean;
    wasValidBeforeHealing: boolean;
    healed: boolean;
}

interface MockProfileWire {
    segments: unknown[];
}

interface MockProfile {
    wires: MockProfileWire[];
}

interface MockSpatialWire {
    segments: unknown[];
}

interface MockRotationTransform {
    axisOrigin: [number, number, number];
    axisDirection: [number, number, number];
    angleDegrees: number;
}

interface MockShapeTransform {
    translation?: [number, number, number];
    rotation?: MockRotationTransform;
}

function isNumberArray(value: unknown, minimumLength: number): value is number[] {
    return Array.isArray(value)
        && value.length >= minimumLength
        && value.every((entry) => typeof entry === 'number' && Number.isFinite(entry));
}

function isPointArray(value: unknown, length: 2 | 3, minimumLength: number): value is number[][] {
    return Array.isArray(value)
        && value.length >= minimumLength
        && value.every((entry) => isPoint(entry, length));
}

function isPositiveInteger(value: unknown): value is number {
    return typeof value === 'number' && Number.isInteger(value) && value > 0;
}

function isPoint(value: unknown, length: 2 | 3): value is number[] {
    return Array.isArray(value)
        && value.length === length
        && value.every((entry) => typeof entry === 'number' && Number.isFinite(entry));
}

function isZeroVector(value: readonly number[]): boolean {
    return value.every((entry) => entry === 0);
}

// ---------------------------------------------------------------------------
// Mock native kernel class
// ---------------------------------------------------------------------------

export class MockNativeKernel {
    private _shapes = new Map<number, MockShape>();
    private _refCounts = new Map<number, number>();
    private _nextId = 1;

    private _defaultStableHashes(shapeKind: string, kind: 'face' | 'edge' | 'vertex', count: number): string[] {
        const prefix = kind === 'face' ? 'F' : kind === 'edge' ? 'E' : 'V';
        return Array.from({ length: count }, (_unused, index) => `${prefix}:mock_${shapeKind}_${index + 1}`);
    }

    private _stableHashes(shape: MockShape, kind: 'face' | 'edge' | 'vertex', count: number): string[] {
        const key = `${kind}StableHashes`;
        const candidate = shape.params[key];
        if (Array.isArray(candidate) && candidate.length === count && candidate.every((entry) => typeof entry === 'string')) {
            return [...candidate as string[]];
        }
        return this._defaultStableHashes(shape.kind, kind, count);
    }

    private _inheritStableHashes(shape: MockShape, resultKind: string, kind: 'face' | 'edge' | 'vertex'): string[] {
        const sourceAnalysis = this._analysis(shape);
        const resultAnalysis = this._analysis({ kind: resultKind, params: {} });
        const sourceCount = kind === 'face' ? sourceAnalysis.faceCount : kind === 'edge' ? sourceAnalysis.edgeCount : sourceAnalysis.vertexCount;
        const targetCount = kind === 'face' ? resultAnalysis.faceCount : kind === 'edge' ? resultAnalysis.edgeCount : resultAnalysis.vertexCount;
        const inherited = this._stableHashes(shape, kind, sourceCount);
        const prefix = kind === 'face' ? 'F' : kind === 'edge' ? 'E' : 'V';

        return Array.from({ length: targetCount }, (_unused, index) => inherited[index] ?? `${prefix}:mock_${resultKind}_derived_${index + 1}`);
    }

    private _boundsOverlap(
        a: { xMin: number; yMin: number; zMin: number; xMax: number; yMax: number; zMax: number },
        b: { xMin: number; yMin: number; zMin: number; xMax: number; yMax: number; zMax: number },
    ): boolean {
        return a.xMin <= b.xMax && a.xMax >= b.xMin
            && a.yMin <= b.yMax && a.yMax >= b.yMin
            && a.zMin <= b.zMax && a.zMax >= b.zMin;
    }

    private _intersectBounds(
        a: { xMin: number; yMin: number; zMin: number; xMax: number; yMax: number; zMax: number },
        b: { xMin: number; yMin: number; zMin: number; xMax: number; yMax: number; zMax: number },
    ): { xMin: number; yMin: number; zMin: number; xMax: number; yMax: number; zMax: number } {
        return {
            xMin: Math.max(a.xMin, b.xMin),
            yMin: Math.max(a.yMin, b.yMin),
            zMin: Math.max(a.zMin, b.zMin),
            xMax: Math.min(a.xMax, b.xMax),
            yMax: Math.min(a.yMax, b.yMax),
            zMax: Math.min(a.zMax, b.zMax),
        };
    }

    private _closestPointOnBounds(
        point: readonly [number, number, number],
        bounds: { xMin: number; yMin: number; zMin: number; xMax: number; yMax: number; zMax: number },
    ): [number, number, number] {
        return [
            Math.min(Math.max(point[0], bounds.xMin), bounds.xMax),
            Math.min(Math.max(point[1], bounds.yMin), bounds.yMax),
            Math.min(Math.max(point[2], bounds.zMin), bounds.zMax),
        ];
    }

    private _distanceBetweenBounds(
        a: { xMin: number; yMin: number; zMin: number; xMax: number; yMax: number; zMax: number },
        b: { xMin: number; yMin: number; zMin: number; xMax: number; yMax: number; zMax: number },
    ): { distance: number; pointOnA: [number, number, number]; pointOnB: [number, number, number] } {
        const pointOnA: [number, number, number] = [0, 0, 0];
        const pointOnB: [number, number, number] = [0, 0, 0];

        const assignAxis = (
            minA: number,
            maxA: number,
            minB: number,
            maxB: number,
            index: 0 | 1 | 2,
        ): number => {
            if (maxA < minB) {
                pointOnA[index] = maxA;
                pointOnB[index] = minB;
                return minB - maxA;
            }
            if (maxB < minA) {
                pointOnA[index] = minA;
                pointOnB[index] = maxB;
                return minA - maxB;
            }
            const overlap = Math.max(minA, minB);
            pointOnA[index] = overlap;
            pointOnB[index] = overlap;
            return 0;
        };

        const dx = assignAxis(a.xMin, a.xMax, b.xMin, b.xMax, 0);
        const dy = assignAxis(a.yMin, a.yMax, b.yMin, b.yMax, 1);
        const dz = assignAxis(a.zMin, a.zMax, b.zMin, b.zMax, 2);

        return {
            distance: Math.sqrt(dx * dx + dy * dy + dz * dz),
            pointOnA,
            pointOnB,
        };
    }

    private _boundingBox(shape: MockShape): { xMin: number; yMin: number; zMin: number; xMax: number; yMax: number; zMax: number } {
        if (shape.kind === 'box') {
            const dx = Number(shape.params.dx) || 1;
            const dy = Number(shape.params.dy) || 1;
            const dz = Number(shape.params.dz) || 1;
            return { xMin: 0, yMin: 0, zMin: 0, xMax: dx, yMax: dy, zMax: dz };
        }
        if (shape.kind === 'cylinder') {
            const radius = Number(shape.params.radius) || 1;
            const height = Number(shape.params.height) || 1;
            return { xMin: -radius, yMin: -radius, zMin: 0, xMax: radius, yMax: radius, zMax: height };
        }
        if (shape.kind === 'sphere') {
            const radius = Number(shape.params.radius) || 1;
            return { xMin: -radius, yMin: -radius, zMin: -radius, xMax: radius, yMax: radius, zMax: radius };
        }
        if (shape.kind === 'transform' && isPositiveInteger(shape.params.id)) {
            const sourceBounds = this._boundingBox(this._require(Number(shape.params.id)));
            const translation = Array.isArray((shape.params.transform as MockShapeTransform | undefined)?.translation)
                ? (shape.params.transform as MockShapeTransform).translation ?? [0, 0, 0]
                : [0, 0, 0];
            return {
                xMin: sourceBounds.xMin + translation[0],
                yMin: sourceBounds.yMin + translation[1],
                zMin: sourceBounds.zMin + translation[2],
                xMax: sourceBounds.xMax + translation[0],
                yMax: sourceBounds.yMax + translation[1],
                zMax: sourceBounds.zMax + translation[2],
            };
        }
        if (shape.kind === 'intersection' && isPositiveInteger(shape.params.id1) && isPositiveInteger(shape.params.id2)) {
            const boundsA = this._boundingBox(this._require(Number(shape.params.id1)));
            const boundsB = this._boundingBox(this._require(Number(shape.params.id2)));
            return this._intersectBounds(boundsA, boundsB);
        }
        return { xMin: 0, yMin: 0, zMin: 0, xMax: 1, yMax: 1, zMax: 1 };
    }

    private _analysis(shape: MockShape): {
        shapeType: string;
        solidCount: number;
        shellCount: number;
        wireCount: number;
        faceCount: number;
        edgeCount: number;
        vertexCount: number;
        boundingBox: { xMin: number; yMin: number; zMin: number; xMax: number; yMax: number; zMax: number };
        isValid: boolean;
        volume: number;
        surfaceArea: number;
        linearLength: number;
        centerOfMass: [number, number, number] | null;
        centerOfMassBasis: 'volume' | 'surface' | 'linear' | 'none';
    } {
        if (shape.kind === 'box') {
            const dx = Number(shape.params.dx) || 1;
            const dy = Number(shape.params.dy) || 1;
            const dz = Number(shape.params.dz) || 1;
            return {
                shapeType: 'solid',
                solidCount: 1,
                shellCount: 1,
                wireCount: 6,
                faceCount: 6,
                edgeCount: 12,
                vertexCount: 8,
                boundingBox: this._boundingBox(shape),
                isValid: shape.params.invalid !== true,
                volume: dx * dy * dz,
                surfaceArea: 2 * (dx * dy + dy * dz + dx * dz),
                linearLength: 4 * (dx + dy + dz),
                centerOfMass: [dx / 2, dy / 2, dz / 2],
                centerOfMassBasis: 'volume',
            };
        }
        if (shape.kind === 'cylinder') {
            const radius = Number(shape.params.radius) || 1;
            const height = Number(shape.params.height) || 1;
            return {
                shapeType: 'solid',
                solidCount: 1,
                shellCount: 1,
                wireCount: 3,
                faceCount: 3,
                edgeCount: 3,
                vertexCount: 2,
                boundingBox: this._boundingBox(shape),
                isValid: shape.params.invalid !== true,
                volume: Math.PI * radius * radius * height,
                surfaceArea: 2 * Math.PI * radius * (radius + height),
                linearLength: 4 * Math.PI * radius,
                centerOfMass: [0, 0, height / 2],
                centerOfMassBasis: 'volume',
            };
        }
        if (shape.kind === 'sphere') {
            const radius = Number(shape.params.radius) || 1;
            return {
                shapeType: 'solid',
                solidCount: 1,
                shellCount: 1,
                wireCount: 1,
                faceCount: 1,
                edgeCount: 0,
                vertexCount: 0,
                boundingBox: this._boundingBox(shape),
                isValid: shape.params.invalid !== true,
                volume: (4 / 3) * Math.PI * radius * radius * radius,
                surfaceArea: 4 * Math.PI * radius * radius,
                linearLength: 0,
                centerOfMass: [0, 0, 0],
                centerOfMassBasis: 'volume',
            };
        }
        if (shape.kind === 'transform' && isPositiveInteger(shape.params.id)) {
            const source = this._require(Number(shape.params.id));
            const analysis = this._analysis(source);
            return {
                ...analysis,
                boundingBox: this._boundingBox(shape),
            };
        }
        if (shape.kind === 'intersection') {
            return {
                shapeType: 'compound',
                solidCount: 0,
                shellCount: 0,
                wireCount: 1,
                faceCount: 0,
                edgeCount: Number(shape.params.edgeCount) || 4,
                vertexCount: Number(shape.params.vertexCount) || 4,
                boundingBox: this._boundingBox(shape),
                isValid: true,
                volume: 0,
                surfaceArea: 0,
                linearLength: 1,
                centerOfMass: null,
                centerOfMassBasis: 'none',
            };
        }
        return {
            shapeType: 'solid',
            solidCount: 1,
            shellCount: 1,
            wireCount: 6,
            faceCount: 6,
            edgeCount: 12,
            vertexCount: 8,
            boundingBox: this._boundingBox(shape),
            isValid: shape.params.invalid !== true,
            volume: 1,
            surfaceArea: 6,
            linearLength: 12,
            centerOfMass: [0.5, 0.5, 0.5],
            centerOfMassBasis: 'volume',
        };
    }

    private _store(kind: string, params: Record<string, unknown>): number {
        const id = this._nextId++;
        this._shapes.set(id, { kind, params });
        this._refCounts.set(id, 1);
        return id;
    }

    private _require(id: number): MockShape {
        const shape = this._shapes.get(id);
        if (!shape) {
            throw new KernelError('INVALID_HANDLE', `No shape with handle ${id}`);
        }
        return shape;
    }

    private _revisionInfo(id: number): Record<string, unknown> {
        const shape = this._require(id);
        if (typeof shape.params.revisionInfo === 'object' && shape.params.revisionInfo !== null) {
            return shape.params.revisionInfo as Record<string, unknown>;
        }

        const unresolved = ['union', 'subtract', 'intersect', 'fillet', 'chamfer', 'extrudeFeature', 'extrudeCutFeature', 'revolveFeature', 'revolveCutFeature', 'sweepFeature', 'sweepCutFeature', 'loftFeature', 'loftCutFeature'].includes(shape.kind);
        const retained = shape.kind === 'transform';
        const entityStatus = typeof shape.params.entityStatus === 'string'
            ? shape.params.entityStatus
            : unresolved ? 'unresolved' : retained ? 'retained' : 'generated';
        const identityStatus = typeof shape.params.identityStatus === 'string'
            ? shape.params.identityStatus
            : unresolved ? 'unresolved' : retained ? 'retained' : 'generated';
        const historyWarnings = Array.isArray(shape.params.historyWarnings)
            ? [...shape.params.historyWarnings as string[]]
            : unresolved ? ['Mock lineage is unresolved for this operation'] : [];
        return {
            revisionId: `rev_mock_${id}`,
            operationId: `op_mock_${id}`,
            sourceFeatureId: `op_mock_${id}`,
            operationType: shape.kind,
            operandRevisionIds: [],
            parameterHash: `P:mock_${shape.kind}`,
            topologyHash: `T:mock_${shape.kind}_${id}`,
            historySchemaVersion: 1,
            createdFromCheckpoint: false,
            entityStatus,
            identityStatus,
            historyWarnings,
            deletedEntities: [],
        };
    }

    private _validateRef(value: Record<string, unknown>, label: string): void {
        if (value.topoId === undefined && value.stableHash === undefined) {
            throw new KernelError('INVALID_PARAMS', `${label} must include topoId or stableHash`);
        }
        if (value.topoId !== undefined && !isPositiveInteger(value.topoId)) {
            throw new KernelError('INVALID_PARAMS', `${label}.topoId must be a positive integer`);
        }
        if (value.stableHash !== undefined && typeof value.stableHash !== 'string') {
            throw new KernelError('INVALID_PARAMS', `${label}.stableHash must be a string`);
        }
    }

    private _validateBlendSpec(spec: Record<string, unknown>, label: 'fillet' | 'chamfer'): void {
        if (spec.schemaVersion !== 1) {
            throw new KernelError('INVALID_PARAMS', `${label}.schemaVersion must be 1`);
        }
        if (spec.limits !== undefined) {
            throw new KernelError('INVALID_PARAMS', JSON.stringify({ phase: 'validation', operation: `${label}Edges`, path: `${label}.limits`, unsupportedFeature: `${label}.partialEdge` }));
        }
        if (spec.tangentPropagation === false) {
            throw new KernelError('INVALID_PARAMS', JSON.stringify({ phase: 'validation', operation: `${label}Edges`, path: `${label}.tangentPropagation`, unsupportedFeature: `${label}.nonPropagatingEdges` }));
        }
        if (spec.cornerMode !== undefined && spec.cornerMode !== 'rollingBall') {
            throw new KernelError('INVALID_PARAMS', JSON.stringify({ phase: 'validation', operation: `${label}Edges`, path: `${label}.cornerMode`, unsupportedFeature: `${label}.cornerModes` }));
        }

        if (Array.isArray(spec.edges)) {
            for (const [index, entry] of spec.edges.entries()) {
                const object = entry as Record<string, unknown>;
                if (object.limits !== undefined) {
                    throw new KernelError('INVALID_PARAMS', JSON.stringify({ phase: 'validation', operation: `${label}Edges`, path: `${label}.edges[${index}].limits`, unsupportedFeature: `${label}.partialEdge` }));
                }
                if (object.tangentPropagation === false) {
                    throw new KernelError('INVALID_PARAMS', JSON.stringify({ phase: 'validation', operation: `${label}Edges`, path: `${label}.edges[${index}].tangentPropagation`, unsupportedFeature: `${label}.nonPropagatingEdges` }));
                }
                const ref = (object.edge ?? object.edgeRef ?? object) as Record<string, unknown>;
                this._validateRef(ref, `${label}.edges[${index}]`);
            }
        }
    }

    private _blendResult(shapeId: number, kind: 'filletFace' | 'chamferFace', edgeCount: number): string {
        const revision = this._revisionInfo(shapeId);
        const topology = JSON.parse(this.getTopology(shapeId)) as { faces?: Array<{ id?: number; stableHash?: string }> };
        const topologyFaces = Array.isArray(topology.faces) ? topology.faces : [];
        const blendFaces = Array.from({ length: edgeCount }, (_, index) => {
            const finalFace = topologyFaces.length > 0 ? topologyFaces[index % topologyFaces.length] : undefined;
            const finalOutputFaceRef = finalFace !== undefined && typeof finalFace.id === 'number'
                ? {
                    stableHash: finalFace.stableHash,
                    topoFaceId: finalFace.id,
                }
                : undefined;
            return {
                kind,
                stableHash: finalOutputFaceRef?.stableHash ?? null,
                topoFaceId: finalOutputFaceRef?.topoFaceId,
                finalOutputFaceRef,
                sourceEdge: { topoId: index + 1, stableHash: `E:mock_source_${index + 1}` },
                tangentChainEdgeRefs: [{ topoId: index + 1, stableHash: `E:mock_source_${index + 1}` }],
                usedParameters: {},
                supportingFaceIds: [1, 2],
                terminalCapIds: [],
                terminalCondition: 'unresolved',
            };
        });
        return JSON.stringify({
            shapeId,
            revision,
            topology,
            lineage: { generated: blendFaces, modified: [], retained: [], deleted: blendFaces.map((_, index) => `E:mock_source_${index + 1}`) },
            blendFaces,
            status: { isPartial: false, isClipped: false, isHealed: false, isExact: true },
        });
    }

    // -- Primitives --

    createBox(dx: number, dy: number, dz: number): number {
        if (dx <= 0 || dy <= 0 || dz <= 0) {
            throw new KernelError('INVALID_PARAMS', 'Box dimensions must be > 0');
        }
        return this._store('box', { dx, dy, dz });
    }

    createCylinder(radius: number, height: number): number {
        if (radius <= 0 || height <= 0) {
            throw new KernelError('INVALID_PARAMS', 'Cylinder radius and height must be > 0');
        }
        return this._store('cylinder', { radius, height });
    }

    createSphere(radius: number): number {
        if (radius <= 0) {
            throw new KernelError('INVALID_PARAMS', 'Sphere radius must be > 0');
        }
        return this._store('sphere', { radius });
    }

    private _parseProfile(profileJson: string): MockProfile {
        const profile = JSON.parse(profileJson) as Partial<MockProfile>;
        if (!Array.isArray(profile.wires) || profile.wires.length === 0) {
            throw new KernelError('INVALID_PARAMS', 'Profile must include at least one wire');
        }
        for (const [wireIndex, wire] of profile.wires.entries()) {
            if (!wire || !Array.isArray(wire.segments) || wire.segments.length === 0) {
                throw new KernelError('INVALID_PARAMS', `Profile wire ${wireIndex} must include segments`);
            }
            for (const [segmentIndex, segment] of wire.segments.entries()) {
                this._validateProfileSegment(segment, `profile.wires[${wireIndex}].segments[${segmentIndex}]`);
            }
        }
        return profile as MockProfile;
    }

    private _parseSpatialWire(wireJson: string, label: string): MockSpatialWire {
        const wire = JSON.parse(wireJson) as Partial<MockSpatialWire>;
        if (!Array.isArray(wire.segments) || wire.segments.length === 0) {
            throw new KernelError('INVALID_PARAMS', `${label} must include at least one segment`);
        }
        for (const [segmentIndex, segment] of wire.segments.entries()) {
            this._validateSpatialSegment(segment, `${label}.segments[${segmentIndex}]`);
        }
        return wire as MockSpatialWire;
    }

    private _requireSingleWireProfile(profile: MockProfile, label: string): MockProfile {
        if (profile.wires.length !== 1) {
            throw new KernelError('INVALID_PARAMS', `${label} must contain exactly one closed wire for this operation`);
        }
        return profile;
    }

    private _validateProfileSegment(segment: unknown, label: string): void {
        const candidate = segment as Record<string, unknown>;
        if (!candidate || typeof candidate.type !== 'string') {
            throw new KernelError('INVALID_PARAMS', `${label}.type must be a string`);
        }

        switch (candidate.type) {
            case 'line':
                if (!isPoint(candidate.start, 2) || !isPoint(candidate.end, 2)) {
                    throw new KernelError('INVALID_PARAMS', `${label} must include start and end points`);
                }
                break;
            case 'arc':
                if (!isPoint(candidate.start, 2) || !isPoint(candidate.mid, 2) || !isPoint(candidate.end, 2)) {
                    throw new KernelError('INVALID_PARAMS', `${label} must include start, mid, and end points`);
                }
                break;
            case 'circle':
                if (!isPoint(candidate.centre, 2) || typeof candidate.radius !== 'number' || candidate.radius <= 0) {
                    throw new KernelError('INVALID_PARAMS', `${label} must include a centre point and positive radius`);
                }
                break;
            case 'bezier':
                if (!isPointArray(candidate.controlPoints, 2, 2)) {
                    throw new KernelError('INVALID_PARAMS', `${label}.controlPoints must contain at least two 2D points`);
                }
                break;
            case 'bspline': {
                if (!isPointArray(candidate.controlPoints, 2, 2)) {
                    throw new KernelError('INVALID_PARAMS', `${label}.controlPoints must contain at least two 2D points`);
                }
                if (!isPositiveInteger(candidate.degree)) {
                    throw new KernelError('INVALID_PARAMS', `${label}.degree must be a positive integer`);
                }
                if (!isNumberArray(candidate.knots, 2)) {
                    throw new KernelError('INVALID_PARAMS', `${label}.knots must contain at least two finite numbers`);
                }
                if (!Array.isArray(candidate.multiplicities) || candidate.multiplicities.length !== candidate.knots.length || !candidate.multiplicities.every(isPositiveInteger)) {
                    throw new KernelError('INVALID_PARAMS', `${label}.multiplicities must be positive integers matching the knot count`);
                }
                for (let index = 1; index < candidate.knots.length; index += 1) {
                    if (candidate.knots[index] <= candidate.knots[index - 1]) {
                        throw new KernelError('INVALID_PARAMS', `${label}.knots must be strictly increasing`);
                    }
                }
                const multiplicitySum = (candidate.multiplicities as number[]).reduce((sum, value) => sum + value, 0);
                if (multiplicitySum - (candidate.degree as number) - 1 !== candidate.controlPoints.length) {
                    throw new KernelError('INVALID_PARAMS', `${label} has inconsistent controlPoints, degree, and multiplicities`);
                }
                break;
            }
            default:
                throw new KernelError('INVALID_PARAMS', `${label}.type is not supported`);
        }
    }

    private _validateSpatialSegment(segment: unknown, label: string): void {
        const candidate = segment as Record<string, unknown>;
        if (!candidate || typeof candidate.type !== 'string') {
            throw new KernelError('INVALID_PARAMS', `${label}.type must be a string`);
        }

        switch (candidate.type) {
            case 'line':
                if (!isPoint(candidate.start, 3) || !isPoint(candidate.end, 3)) {
                    throw new KernelError('INVALID_PARAMS', `${label} must include 3D start and end points`);
                }
                break;
            case 'arc':
                if (!isPoint(candidate.start, 3) || !isPoint(candidate.mid, 3) || !isPoint(candidate.end, 3)) {
                    throw new KernelError('INVALID_PARAMS', `${label} must include 3D start, mid, and end points`);
                }
                break;
            case 'circle':
                if (!isPoint(candidate.center, 3) || !isPoint(candidate.normal, 3)) {
                    throw new KernelError('INVALID_PARAMS', `${label} must include a 3D center and normal`);
                }
                if (isZeroVector(candidate.normal as number[])) {
                    throw new KernelError('INVALID_PARAMS', `${label}.normal must not be the zero vector`);
                }
                if (typeof candidate.radius !== 'number' || !Number.isFinite(candidate.radius) || candidate.radius <= 0) {
                    throw new KernelError('INVALID_PARAMS', `${label}.radius must be > 0`);
                }
                if (candidate.xDirection !== undefined && (!isPoint(candidate.xDirection, 3) || isZeroVector(candidate.xDirection as number[]))) {
                    throw new KernelError('INVALID_PARAMS', `${label}.xDirection must be a non-zero 3-element array`);
                }
                break;
            case 'bezier':
                if (!isPointArray(candidate.controlPoints, 3, 2)) {
                    throw new KernelError('INVALID_PARAMS', `${label}.controlPoints must contain at least two 3D points`);
                }
                break;
            case 'bspline': {
                if (!isPointArray(candidate.controlPoints, 3, 2)) {
                    throw new KernelError('INVALID_PARAMS', `${label}.controlPoints must contain at least two 3D points`);
                }
                if (!isPositiveInteger(candidate.degree)) {
                    throw new KernelError('INVALID_PARAMS', `${label}.degree must be a positive integer`);
                }
                if (!isNumberArray(candidate.knots, 2)) {
                    throw new KernelError('INVALID_PARAMS', `${label}.knots must contain at least two finite numbers`);
                }
                if (!Array.isArray(candidate.multiplicities) || candidate.multiplicities.length !== candidate.knots.length || !candidate.multiplicities.every(isPositiveInteger)) {
                    throw new KernelError('INVALID_PARAMS', `${label}.multiplicities must be positive integers matching the knot count`);
                }
                for (let index = 1; index < candidate.knots.length; index += 1) {
                    if (candidate.knots[index] <= candidate.knots[index - 1]) {
                        throw new KernelError('INVALID_PARAMS', `${label}.knots must be strictly increasing`);
                    }
                }
                const multiplicitySum = (candidate.multiplicities as number[]).reduce((sum, value) => sum + value, 0);
                if (multiplicitySum - (candidate.degree as number) - 1 !== candidate.controlPoints.length) {
                    throw new KernelError('INVALID_PARAMS', `${label} has inconsistent controlPoints, degree, and multiplicities`);
                }
                break;
            }
            default:
                throw new KernelError('INVALID_PARAMS', `${label}.type is not supported`);
        }
    }

    private _parseExtrudeOptions(optionsJson: string): Record<string, unknown> {
        const options = JSON.parse(optionsJson) as Record<string, unknown>;
        const hasHeight = typeof options.height === 'number';
        const hasVector = isPoint(options.vector, 3);

        if (hasHeight === hasVector) {
            throw new KernelError('INVALID_PARAMS', "Extrude options must specify exactly one of 'height' or 'vector'");
        }
        if (hasHeight && (options.height as number) <= 0) {
            throw new KernelError('INVALID_PARAMS', 'Extrusion height must be > 0');
        }
        if (hasVector && isZeroVector(options.vector as number[])) {
            throw new KernelError('INVALID_PARAMS', 'Extrusion vector must not be the zero vector');
        }

        if (options.plane !== undefined) {
            const plane = options.plane as Record<string, unknown>;
            if (!isPoint(plane.origin, 3) || !isPoint(plane.normal, 3) || !isPoint(plane.xDirection, 3)) {
                throw new KernelError('INVALID_PARAMS', 'Plane must include origin, normal, and xDirection');
            }
            if (isZeroVector(plane.normal as number[]) || isZeroVector(plane.xDirection as number[])) {
                throw new KernelError('INVALID_PARAMS', 'Plane vectors must not be zero');
            }
        }

        return options;
    }

    private _parseExtrudeSpec(specJson: string): Record<string, unknown> {
        const spec = JSON.parse(specJson) as Record<string, unknown>;
        if (spec.schemaVersion !== 1) {
            throw new KernelError('INVALID_PARAMS', 'extrude spec.schemaVersion must be 1');
        }

        const allowUnknownFields = spec.allowUnknownFields === true;
        if (!allowUnknownFields) {
            const allowedKeys = new Set([
                'schemaVersion',
                'allowUnknownFields',
                'unit',
                'plane',
                'direction',
                'reverseDirection',
                'draftAngleRadians',
                'draftAngleDegrees',
                'extent',
                'metadata',
            ]);
            for (const key of Object.keys(spec)) {
                if (!allowedKeys.has(key)) {
                    throw new KernelError('INVALID_PARAMS', JSON.stringify({
                        phase: 'validation',
                        operation: 'extrudeProfile',
                        path: `extrude.${key}`,
                        reason: 'Unknown field is not allowed for this schema version',
                        unsupportedFeature: 'unknownField',
                    }));
                }
            }
        }

        if (spec.unit !== undefined) {
            const unit = spec.unit as Record<string, unknown>;
            if (unit.length !== undefined && unit.length !== 'model') {
                throw new KernelError('INVALID_PARAMS', 'extrude spec.unit.length must be model');
            }
            if (unit.angle !== undefined && unit.angle !== 'radians' && unit.angle !== 'degrees') {
                throw new KernelError('INVALID_PARAMS', 'extrude spec.unit.angle must be radians or degrees');
            }
        }

        if (spec.plane !== undefined) {
            const plane = spec.plane as Record<string, unknown>;
            if (!isPoint(plane.origin, 3) || !isPoint(plane.normal, 3) || !isPoint(plane.xDirection, 3)) {
                throw new KernelError('INVALID_PARAMS', 'Plane must include origin, normal, and xDirection');
            }
            if (isZeroVector(plane.normal as number[]) || isZeroVector(plane.xDirection as number[])) {
                throw new KernelError('INVALID_PARAMS', 'Plane vectors must not be zero');
            }
        }

        if (spec.direction !== undefined) {
            if (!isPoint(spec.direction, 3) || isZeroVector(spec.direction as number[])) {
                throw new KernelError('INVALID_PARAMS', 'extrude spec.direction must be a non-zero 3-element array');
            }
        }

        if (spec.reverseDirection !== undefined && typeof spec.reverseDirection !== 'boolean') {
            throw new KernelError('INVALID_PARAMS', 'extrude spec.reverseDirection must be a boolean');
        }

        const hasAngleRadians = typeof spec.draftAngleRadians === 'number';
        const hasAngleDegrees = typeof spec.draftAngleDegrees === 'number';
        if (hasAngleRadians && hasAngleDegrees) {
            throw new KernelError('INVALID_PARAMS', 'extrude spec must not specify both draftAngleRadians and draftAngleDegrees');
        }
        if (hasAngleRadians && Math.abs(spec.draftAngleRadians as number) >= Math.PI / 2) {
            throw new KernelError('INVALID_PARAMS', 'extrude spec.draftAngleRadians must be in (-pi/2, pi/2)');
        }
        if (hasAngleDegrees && Math.abs(spec.draftAngleDegrees as number) >= 90) {
            throw new KernelError('INVALID_PARAMS', 'extrude spec.draftAngleDegrees must be in (-90, 90)');
        }

        const extent = spec.extent as Record<string, unknown> | undefined;
        if (!extent || typeof extent.type !== 'string') {
            throw new KernelError('INVALID_PARAMS', 'extrude spec.extent must be provided');
        }

        const validateSurface = (label: string, surface: Record<string, unknown> | undefined): void => {
            if (!surface || typeof surface !== 'object') {
                throw new KernelError('INVALID_PARAMS', `${label} must be an object`);
            }
            if (surface.shapeId !== undefined) {
                if (!isPositiveInteger(surface.shapeId)) {
                    throw new KernelError('INVALID_PARAMS', `${label}.shapeId must be a positive integer`);
                }
                this._require(surface.shapeId as number);
            }
            this._validateRef((surface.face ?? {}) as Record<string, unknown>, `${label}.face`);
        };

        switch (extent.type) {
            case 'blind':
                if (typeof extent.distance !== 'number' || !Number.isFinite(extent.distance) || extent.distance <= 0) {
                    throw new KernelError('INVALID_PARAMS', 'extrude spec.extent.distance must be > 0');
                }
                break;
            case 'upToNext':
            case 'throughAll':
                break;
            case 'upToSurface':
                validateSurface('extrude spec.extent.surface', extent.surface as Record<string, unknown> | undefined);
                break;
            case 'offsetFromSurface':
                validateSurface('extrude spec.extent.surface', extent.surface as Record<string, unknown> | undefined);
                if (typeof extent.offset !== 'number' || !Number.isFinite(extent.offset) || extent.offset <= 0) {
                    throw new KernelError('INVALID_PARAMS', 'extrude spec.extent.offset must be > 0');
                }
                break;
            default:
                throw new KernelError('INVALID_PARAMS', `extrude spec.extent.type '${extent.type}' is not supported`);
        }

        if (hasAngleRadians || hasAngleDegrees) {
            const planeNormal = spec.plane !== undefined ? ((spec.plane as Record<string, unknown>).normal as number[] | undefined) : undefined;
            const direction = (spec.direction as number[] | undefined) ?? planeNormal;
            if (direction !== undefined && planeNormal !== undefined) {
                const cross = [
                    planeNormal[1] * direction[2] - planeNormal[2] * direction[1],
                    planeNormal[2] * direction[0] - planeNormal[0] * direction[2],
                    planeNormal[0] * direction[1] - planeNormal[1] * direction[0],
                ];
                if (!isZeroVector(cross)) {
                    throw new KernelError('INVALID_PARAMS', 'Draft extrusion only supports direction aligned with the sketch plane normal');
                }
            }
        }

        return spec;
    }

    private _parseRevolveOptions(optionsJson: string): Record<string, unknown> {
        const options = JSON.parse(optionsJson) as Record<string, unknown>;
        if (typeof options.angleDegrees !== 'number' || options.angleDegrees <= 0 || options.angleDegrees > 360) {
            throw new KernelError('INVALID_PARAMS', 'Revolution angle must be in (0, 360]');
        }
        if (options.axisOrigin !== undefined && !isPoint(options.axisOrigin, 3)) {
            throw new KernelError('INVALID_PARAMS', 'axisOrigin must be a 3-element array');
        }
        if (options.axisDirection !== undefined) {
            if (!isPoint(options.axisDirection, 3)) {
                throw new KernelError('INVALID_PARAMS', 'axisDirection must be a 3-element array');
            }
            if (isZeroVector(options.axisDirection as number[])) {
                throw new KernelError('INVALID_PARAMS', 'axisDirection must not be the zero vector');
            }
        }
        return options;
    }

    private _parseRevolveSpec(specJson: string): Record<string, unknown> {
        const spec = JSON.parse(specJson) as Record<string, unknown>;
        if (spec.schemaVersion !== 1) {
            throw new KernelError('INVALID_PARAMS', 'revolve spec.schemaVersion must be 1');
        }

        const allowUnknownFields = spec.allowUnknownFields === true;
        if (!allowUnknownFields) {
            const allowedKeys = new Set([
                'schemaVersion',
                'allowUnknownFields',
                'unit',
                'plane',
                'axisOrigin',
                'axisDirection',
                'reverseDirection',
                'slidingEdges',
                'extent',
                'metadata',
            ]);
            for (const key of Object.keys(spec)) {
                if (!allowedKeys.has(key)) {
                    throw new KernelError('INVALID_PARAMS', JSON.stringify({
                        phase: 'validation',
                        operation: 'revolveProfile',
                        path: `revolve.${key}`,
                        reason: 'Unknown field is not allowed for this schema version',
                        unsupportedFeature: 'unknownField',
                    }));
                }
            }
        }

        if (spec.unit !== undefined) {
            const unit = spec.unit as Record<string, unknown>;
            if (unit.length !== undefined && unit.length !== 'model') {
                throw new KernelError('INVALID_PARAMS', 'revolve spec.unit.length must be model');
            }
            if (unit.angle !== undefined && unit.angle !== 'radians' && unit.angle !== 'degrees') {
                throw new KernelError('INVALID_PARAMS', 'revolve spec.unit.angle must be radians or degrees');
            }
        }

        if (spec.plane !== undefined) {
            const plane = spec.plane as Record<string, unknown>;
            if (!isPoint(plane.origin, 3) || !isPoint(plane.normal, 3) || !isPoint(plane.xDirection, 3)) {
                throw new KernelError('INVALID_PARAMS', 'Plane must include origin, normal, and xDirection');
            }
            if (isZeroVector(plane.normal as number[]) || isZeroVector(plane.xDirection as number[])) {
                throw new KernelError('INVALID_PARAMS', 'Plane vectors must not be zero');
            }
        }

        if (spec.axisOrigin !== undefined && !isPoint(spec.axisOrigin, 3)) {
            throw new KernelError('INVALID_PARAMS', 'revolve spec.axisOrigin must be a 3-element array');
        }
        if (spec.axisDirection !== undefined) {
            if (!isPoint(spec.axisDirection, 3) || isZeroVector(spec.axisDirection as number[])) {
                throw new KernelError('INVALID_PARAMS', 'revolve spec.axisDirection must be a non-zero 3-element array');
            }
        }
        if (spec.reverseDirection !== undefined && typeof spec.reverseDirection !== 'boolean') {
            throw new KernelError('INVALID_PARAMS', 'revolve spec.reverseDirection must be a boolean');
        }

        if (spec.slidingEdges !== undefined) {
            if (!Array.isArray(spec.slidingEdges)) {
                throw new KernelError('INVALID_PARAMS', 'revolve spec.slidingEdges must be an array');
            }
            for (const [index, entry] of (spec.slidingEdges as Record<string, unknown>[]).entries()) {
                if (!isPositiveInteger(entry.profileEdgeIndex)) {
                    throw new KernelError('INVALID_PARAMS', `revolve spec.slidingEdges[${index}].profileEdgeIndex must be a positive integer`);
                }
                this._validateRef((entry.face ?? {}) as Record<string, unknown>, `revolve spec.slidingEdges[${index}].face`);
            }
        }

        const validateSurface = (label: string, surface: Record<string, unknown> | undefined): void => {
            if (!surface || typeof surface !== 'object') {
                throw new KernelError('INVALID_PARAMS', `${label} must be an object`);
            }
            if (surface.shapeId !== undefined) {
                if (!isPositiveInteger(surface.shapeId)) {
                    throw new KernelError('INVALID_PARAMS', `${label}.shapeId must be a positive integer`);
                }
                this._require(surface.shapeId as number);
            }
            this._validateRef((surface.face ?? {}) as Record<string, unknown>, `${label}.face`);
        };

        const validateAngle = (label: string, value: Record<string, unknown>): void => {
            const hasRadians = typeof value.angleRadians === 'number';
            const hasDegrees = typeof value.angleDegrees === 'number';
            if (hasRadians && hasDegrees) {
                throw new KernelError('INVALID_PARAMS', `${label} must not specify both angleRadians and angleDegrees`);
            }
            if (!hasRadians && !hasDegrees) {
                throw new KernelError('INVALID_PARAMS', `${label} must specify angleRadians or angleDegrees`);
            }
            if (hasRadians && (!(Number.isFinite(value.angleRadians)) || value.angleRadians === 0 || Math.abs(value.angleRadians as number) > Math.PI * 2)) {
                throw new KernelError('INVALID_PARAMS', `${label}.angleRadians must be in [-2pi, 2pi] excluding 0`);
            }
            if (hasDegrees && (!(Number.isFinite(value.angleDegrees)) || value.angleDegrees === 0 || Math.abs(value.angleDegrees as number) > 360)) {
                throw new KernelError('INVALID_PARAMS', `${label}.angleDegrees must be in [-360, 360] excluding 0`);
            }
        };

        const extent = spec.extent as Record<string, unknown> | undefined;
        if (!extent || typeof extent.type !== 'string') {
            throw new KernelError('INVALID_PARAMS', 'revolve spec.extent must be provided');
        }

        switch (extent.type) {
            case 'angle':
                validateAngle('revolve spec.extent', extent);
                break;
            case 'upToSurface':
                validateSurface('revolve spec.extent.surface', extent.surface as Record<string, unknown> | undefined);
                break;
            case 'fromSurfaceToSurface':
                validateSurface('revolve spec.extent.fromSurface', extent.fromSurface as Record<string, unknown> | undefined);
                validateSurface('revolve spec.extent.untilSurface', extent.untilSurface as Record<string, unknown> | undefined);
                break;
            case 'throughAll':
                break;
            case 'upToSurfaceAtAngle':
                validateSurface('revolve spec.extent.surface', extent.surface as Record<string, unknown> | undefined);
                validateAngle('revolve spec.extent', extent);
                break;
            default:
                throw new KernelError('INVALID_PARAMS', `revolve spec.extent.type '${extent.type}' is not supported`);
        }

        return spec;
    }

    private _parseSweepSpec(specJson: string): Record<string, unknown> {
        const spec = JSON.parse(specJson) as Record<string, unknown>;
        if (spec.schemaVersion !== 1) {
            throw new KernelError('INVALID_PARAMS', 'sweep spec.schemaVersion must be 1');
        }

        if (typeof spec.spineJson !== 'string') {
            throw new KernelError('INVALID_PARAMS', 'sweep spec.spineJson must be a string');
        }
        this._parseSpatialWire(spec.spineJson, 'sweep spec.spine');

        if (spec.cut === true && spec.solid === false) {
            throw new KernelError('INVALID_PARAMS', 'sweep cut operations require spec.solid !== false');
        }

        if (spec.plane !== undefined) {
            const plane = spec.plane as Record<string, unknown>;
            if (!isPoint(plane.origin, 3) || !isPoint(plane.normal, 3) || !isPoint(plane.xDirection, 3)) {
                throw new KernelError('INVALID_PARAMS', 'Plane must include origin, normal, and xDirection');
            }
        }

        if (spec.trihedronMode !== undefined) {
            const trihedronMode = spec.trihedronMode as Record<string, unknown>;
            switch (trihedronMode.type) {
                case 'correctedFrenet':
                case 'frenet':
                case 'discrete':
                    break;
                case 'fixedTrihedron': {
                    const frame = trihedronMode.frame as Record<string, unknown> | undefined;
                    if (!frame || !isPoint(frame.origin, 3) || !isPoint(frame.normal, 3) || !isPoint(frame.xDirection, 3)) {
                        throw new KernelError('INVALID_PARAMS', 'sweep spec.trihedronMode.frame must define origin, normal, and xDirection');
                    }
                    break;
                }
                case 'fixedBinormal':
                    if (!isPoint(trihedronMode.binormal, 3) || isZeroVector(trihedronMode.binormal as number[])) {
                        throw new KernelError('INVALID_PARAMS', 'sweep spec.trihedronMode.binormal must be a non-zero 3-element array');
                    }
                    break;
                case 'auxiliarySpine':
                    if (typeof trihedronMode.spineJson !== 'string') {
                        throw new KernelError('INVALID_PARAMS', 'sweep spec.trihedronMode.spineJson must be a string');
                    }
                    this._parseSpatialWire(trihedronMode.spineJson, 'sweep spec.trihedronMode.spine');
                    break;
                default:
                    throw new KernelError('INVALID_PARAMS', 'sweep spec.trihedronMode.type is not supported');
            }
        }

        if (spec.tolerance !== undefined) {
            const tolerance = spec.tolerance as Record<string, unknown>;
            for (const key of ['tol3d', 'boundTol', 'angularTol'] as const) {
                const value = tolerance[key];
                if (value !== undefined && (typeof value !== 'number' || !Number.isFinite(value) || value <= 0)) {
                    throw new KernelError('INVALID_PARAMS', `sweep spec.tolerance.${key} must be > 0`);
                }
            }
        }

        if (spec.maxDegree !== undefined && !isPositiveInteger(spec.maxDegree)) {
            throw new KernelError('INVALID_PARAMS', 'sweep spec.maxDegree must be a positive integer');
        }
        if (spec.maxSegments !== undefined && !isPositiveInteger(spec.maxSegments)) {
            throw new KernelError('INVALID_PARAMS', 'sweep spec.maxSegments must be a positive integer');
        }

        return spec;
    }

    private _parseLoftSections(sectionsJson: string): Record<string, unknown>[] {
        const sections = JSON.parse(sectionsJson) as Record<string, unknown>[];
        if (!Array.isArray(sections) || sections.length < 2) {
            throw new KernelError('INVALID_PARAMS', 'loft sections must contain at least two entries');
        }

        for (const [index, section] of sections.entries()) {
            switch (section.type) {
                case 'profile':
                    if (typeof section.profileJson !== 'string') {
                        throw new KernelError('INVALID_PARAMS', `loft sections[${index}].profileJson must be a string`);
                    }
                    this._requireSingleWireProfile(this._parseProfile(section.profileJson as string), `loft sections[${index}].profile`);
                    if (section.plane !== undefined) {
                        const plane = section.plane as Record<string, unknown>;
                        if (!isPoint(plane.origin, 3) || !isPoint(plane.normal, 3) || !isPoint(plane.xDirection, 3)) {
                            throw new KernelError('INVALID_PARAMS', `loft sections[${index}].plane must define origin, normal, and xDirection`);
                        }
                    }
                    break;
                case 'wire':
                    if (typeof section.wireJson !== 'string') {
                        throw new KernelError('INVALID_PARAMS', `loft sections[${index}].wireJson must be a string`);
                    }
                    this._parseSpatialWire(section.wireJson as string, `loft sections[${index}].wire`);
                    break;
                case 'point':
                    if (!isPoint(section.point, 3)) {
                        throw new KernelError('INVALID_PARAMS', `loft sections[${index}].point must be a 3-element array`);
                    }
                    break;
                default:
                    throw new KernelError('INVALID_PARAMS', `loft sections[${index}].type is not supported`);
            }
        }

        return sections;
    }

    private _parseLoftSpec(specJson: string): Record<string, unknown> {
        const spec = JSON.parse(specJson) as Record<string, unknown>;
        if (spec.schemaVersion !== 1) {
            throw new KernelError('INVALID_PARAMS', 'loft spec.schemaVersion must be 1');
        }

        if (spec.cut === true && spec.solid === false) {
            throw new KernelError('INVALID_PARAMS', 'loft cut operations require spec.solid !== false');
        }

        if (spec.pres3d !== undefined && (typeof spec.pres3d !== 'number' || !Number.isFinite(spec.pres3d) || spec.pres3d <= 0)) {
            throw new KernelError('INVALID_PARAMS', 'loft spec.pres3d must be > 0');
        }

        if (spec.maxDegree !== undefined && !isPositiveInteger(spec.maxDegree)) {
            throw new KernelError('INVALID_PARAMS', 'loft spec.maxDegree must be a positive integer');
        }

        if (spec.criteriumWeight !== undefined) {
            const weights = spec.criteriumWeight as Record<string, unknown>;
            for (const key of ['w1', 'w2', 'w3'] as const) {
                const value = weights[key];
                if (typeof value !== 'number' || !Number.isFinite(value) || value <= 0) {
                    throw new KernelError('INVALID_PARAMS', `loft spec.criteriumWeight.${key} must be > 0`);
                }
            }
        }

        return spec;
    }

    private _parseShapeTransform(transformJson: string): MockShapeTransform {
        const transform = JSON.parse(transformJson) as MockShapeTransform;
        if (transform.translation !== undefined) {
            if (!isPoint(transform.translation, 3)) {
                throw new KernelError('INVALID_PARAMS', 'translation must be a 3-element array');
            }
        }
        if (transform.rotation !== undefined) {
            if (!isPoint(transform.rotation.axisOrigin, 3) || !isPoint(transform.rotation.axisDirection, 3)) {
                throw new KernelError('INVALID_PARAMS', 'rotation axis must be defined by 3-element arrays');
            }
            if (isZeroVector(transform.rotation.axisDirection)) {
                throw new KernelError('INVALID_PARAMS', 'rotation axisDirection must not be the zero vector');
            }
            if (typeof transform.rotation.angleDegrees !== 'number' || !Number.isFinite(transform.rotation.angleDegrees)) {
                throw new KernelError('INVALID_PARAMS', 'rotation angleDegrees must be finite');
            }
        }
        if (transform.translation === undefined && transform.rotation === undefined) {
            throw new KernelError('INVALID_PARAMS', 'Transform must specify translation and/or rotation');
        }
        return transform;
    }

    // -- Sketch-based features --

    extrudeProfile(profileJson: string, optionsJson: string): number {
        const profile = this._parseProfile(profileJson);
        const options = this._parseExtrudeOptions(optionsJson);
        return this._store('extrude', { profile, options });
    }

    extrudeProfileWithSpec(id: number, profileJson: string, specJson: string): number {
        this._require(id);
        const profile = this._parseProfile(profileJson);
        const spec = this._parseExtrudeSpec(specJson);
        return this._store('extrudeFeature', { id, profile, spec });
    }

    extrudeCutProfileWithSpec(id: number, profileJson: string, specJson: string): number {
        this._require(id);
        const profile = this._parseProfile(profileJson);
        const spec = this._parseExtrudeSpec(specJson);
        return this._store('extrudeCutFeature', { id, profile, spec });
    }

    revolveProfile(profileJson: string, optionsJson: string): number {
        const profile = this._parseProfile(profileJson);
        const options = this._parseRevolveOptions(optionsJson);
        return this._store('revolve', { profile, options });
    }

    revolveProfileWithSpec(id: number, profileJson: string, specJson: string): number {
        this._require(id);
        const profile = this._parseProfile(profileJson);
        const spec = this._parseRevolveSpec(specJson);
        return this._store('revolveFeature', { id, profile, spec });
    }

    revolveCutProfileWithSpec(id: number, profileJson: string, specJson: string): number {
        this._require(id);
        const profile = this._parseProfile(profileJson);
        const spec = this._parseRevolveSpec(specJson);
        return this._store('revolveCutFeature', { id, profile, spec });
    }

    sweepProfileWithSpec(id: number, profileJson: string, specJson: string): number {
        this._require(id);
        const profile = this._requireSingleWireProfile(this._parseProfile(profileJson), 'profile');
        const spec = this._parseSweepSpec(specJson);
        return this._store(spec.cut === true ? 'sweepCutFeature' : 'sweepFeature', { id, profile, spec });
    }

    loftWithSpec(id: number, sectionsJson: string, specJson: string): number {
        this._require(id);
        const sections = this._parseLoftSections(sectionsJson);
        const spec = this._parseLoftSpec(specJson);
        return this._store(spec.cut === true ? 'loftCutFeature' : 'loftFeature', { id, sections, spec });
    }

    // -- Booleans --

    booleanUnion(id1: number, id2: number): number {
        const source = this._require(id1);
        this._require(id2);
        return this._store('union', {
            id1,
            id2,
            faceStableHashes: this._inheritStableHashes(source, 'union', 'face'),
            edgeStableHashes: this._inheritStableHashes(source, 'union', 'edge'),
            vertexStableHashes: this._inheritStableHashes(source, 'union', 'vertex'),
            identityStatus: 'resolved',
            historyWarnings: [],
        });
    }

    booleanSubtract(id1: number, id2: number): number {
        const source = this._require(id1);
        this._require(id2);
        return this._store('subtract', {
            id1,
            id2,
            faceStableHashes: this._inheritStableHashes(source, 'subtract', 'face'),
            edgeStableHashes: this._inheritStableHashes(source, 'subtract', 'edge'),
            vertexStableHashes: this._inheritStableHashes(source, 'subtract', 'vertex'),
            identityStatus: 'resolved',
            historyWarnings: [],
        });
    }

    booleanIntersect(id1: number, id2: number): number {
        const source = this._require(id1);
        this._require(id2);
        return this._store('intersect', {
            id1,
            id2,
            faceStableHashes: this._inheritStableHashes(source, 'intersect', 'face'),
            edgeStableHashes: this._inheritStableHashes(source, 'intersect', 'edge'),
            vertexStableHashes: this._inheritStableHashes(source, 'intersect', 'vertex'),
            identityStatus: 'resolved',
            historyWarnings: [],
        });
    }

    // -- Modifiers --

    filletEdges(id: number, radius: number): number {
        const source = this._require(id);
        if (radius <= 0) {
            throw new KernelError('INVALID_PARAMS', 'Fillet radius must be > 0');
        }
        return this._store('fillet', {
            id,
            radius,
            faceStableHashes: this._inheritStableHashes(source, 'fillet', 'face'),
            edgeStableHashes: this._inheritStableHashes(source, 'fillet', 'edge'),
            vertexStableHashes: this._inheritStableHashes(source, 'fillet', 'vertex'),
            identityStatus: 'resolved',
            historyWarnings: [],
        });
    }

    chamferEdges(id: number, distance: number): number {
        const source = this._require(id);
        if (distance <= 0) {
            throw new KernelError('INVALID_PARAMS', 'Chamfer distance must be > 0');
        }
        return this._store('chamfer', {
            id,
            distance,
            faceStableHashes: this._inheritStableHashes(source, 'chamfer', 'face'),
            edgeStableHashes: this._inheritStableHashes(source, 'chamfer', 'edge'),
            vertexStableHashes: this._inheritStableHashes(source, 'chamfer', 'vertex'),
            identityStatus: 'resolved',
            historyWarnings: [],
        });
    }

    filletEdgesWithSpec(id: number, specJson: string): string {
        const source = this._require(id);
        const spec = JSON.parse(specJson) as Record<string, unknown>;
        this._validateBlendSpec(spec, 'fillet');
        const edgeCount = Array.isArray(spec.edges) ? Math.max(spec.edges.length, 1) : 12;
        const shapeId = this._store('fillet', {
            id,
            spec,
            faceStableHashes: this._inheritStableHashes(source, 'fillet', 'face'),
            edgeStableHashes: this._inheritStableHashes(source, 'fillet', 'edge'),
            vertexStableHashes: this._inheritStableHashes(source, 'fillet', 'vertex'),
            identityStatus: 'resolved',
            historyWarnings: [],
        });
        return this._blendResult(shapeId, 'filletFace', edgeCount);
    }

    chamferEdgesWithSpec(id: number, specJson: string): string {
        const source = this._require(id);
        const spec = JSON.parse(specJson) as Record<string, unknown>;
        this._validateBlendSpec(spec, 'chamfer');
        const edgeCount = Array.isArray(spec.edges) ? Math.max(spec.edges.length, 1) : 12;
        const shapeId = this._store('chamfer', {
            id,
            spec,
            faceStableHashes: this._inheritStableHashes(source, 'chamfer', 'face'),
            edgeStableHashes: this._inheritStableHashes(source, 'chamfer', 'edge'),
            vertexStableHashes: this._inheritStableHashes(source, 'chamfer', 'vertex'),
            identityStatus: 'resolved',
            historyWarnings: [],
        });
        return this._blendResult(shapeId, 'chamferFace', edgeCount);
    }

    transformShape(id: number, transformJson: string): number {
        const source = this._require(id);
        const transform = this._parseShapeTransform(transformJson);
        const analysis = this._analysis(source);
        return this._store('transform', {
            id,
            transform,
            faceStableHashes: this._stableHashes(source, 'face', analysis.faceCount),
            edgeStableHashes: this._stableHashes(source, 'edge', analysis.edgeCount),
            vertexStableHashes: this._stableHashes(source, 'vertex', analysis.vertexCount),
            identityStatus: 'retained',
            entityStatus: 'retained',
            historyWarnings: [],
        });
    }

    intersectShapes(id1: number, id2: number): string {
        const shapeA = this._require(id1);
        const shapeB = this._require(id2);
        const boundsA = this._boundingBox(shapeA);
        const boundsB = this._boundingBox(shapeB);

        if (!this._boundsOverlap(boundsA, boundsB)) {
            return JSON.stringify({ hasIntersection: false, edgeCount: 0, vertexCount: 0 });
        }

        const sectionShapeId = this._store('intersection', {
            id1,
            id2,
            edgeCount: 4,
            vertexCount: 4,
            edgeStableHashes: this._defaultStableHashes('intersection', 'edge', 4),
            vertexStableHashes: this._defaultStableHashes('intersection', 'vertex', 4),
            identityStatus: 'generated',
            historyWarnings: [],
        });

        return JSON.stringify({
            hasIntersection: true,
            edgeCount: 4,
            vertexCount: 4,
            sectionShapeId,
        });
    }

    // -- Queries --

    getKernelVersionInfo(): string {
        return JSON.stringify({
            kernelVersion: '8.0.0',
            kernelVersionMajor: 8,
            kernelVersionMinor: 0,
            kernelVersionMaintenance: 0,
            checkpointSchemaVersion: 1,
            operationSchemaVersion: 1,
        });
    }

    getTopology(id: number): string {
        const shape = this._require(id);
        const revision = this._revisionInfo(id);
        const analysis = this._analysis(shape);
        const faceStableHashes = this._stableHashes(shape, 'face', analysis.faceCount);
        const edgeStableHashes = this._stableHashes(shape, 'edge', analysis.edgeCount);
        const vertexStableHashes = this._stableHashes(shape, 'vertex', analysis.vertexCount);
        return JSON.stringify({
            ...revision,
            shapeType: analysis.shapeType,
            solidCount: analysis.solidCount,
            shellCount: analysis.shellCount,
            wireCount: analysis.wireCount,
            faceCount: analysis.faceCount,
            edgeCount: analysis.edgeCount,
            vertexCount: analysis.vertexCount,
            boundingBox: analysis.boundingBox,
            isValid: analysis.isValid,
            solids: Array.from({ length: analysis.solidCount }, (_, index) => ({
                id: index + 1,
                shellIds: [1],
                status: revision.entityStatus,
            })),
            shells: Array.from({ length: analysis.shellCount }, (_, index) => ({
                id: index + 1,
                solidIds: [1],
                faceIds: Array.from({ length: analysis.faceCount }, (_unused, faceIndex) => faceIndex + 1),
                status: revision.entityStatus,
            })),
            wires: Array.from({ length: analysis.wireCount }, (_, index) => ({
                id: index + 1,
                edgeIds: Array.from({ length: Math.min(4, analysis.edgeCount) }, (_unused, edgeIndex) => edgeIndex + 1),
                topoFaceIds: [Math.min(index + 1, analysis.faceCount)],
                status: revision.entityStatus,
            })),
            faces: Array.from({ length: analysis.faceCount }, (_, index) => ({
                id: index + 1,
                stableHash: faceStableHashes[index],
                role: 'unknown',
                sourceFeatureId: null,
                generatedFrom: [],
                modifiedFrom: [],
                retainedFrom: [],
                status: revision.entityStatus,
                shared: {},
            })),
            edges: Array.from({ length: analysis.edgeCount }, (_, index) => ({
                id: index + 1,
                stableHash: edgeStableHashes[index],
                topoFaceIds: Array.from({ length: Math.min(2, analysis.faceCount) }, (_unused, faceIndex) => faceIndex + 1),
                generatedFrom: [],
                modifiedFrom: [],
                retainedFrom: [],
                status: revision.entityStatus,
            })),
            vertices: Array.from({ length: analysis.vertexCount }, (_, index) => ({
                id: index + 1,
                stableHash: vertexStableHashes[index],
                status: revision.entityStatus,
            })),
            deletedEntities: revision.deletedEntities,
        });
    }

    analyzeShape(id: number): string {
        const shape = this._require(id);
        return JSON.stringify(this._analysis(shape));
    }

    classifyPointContainment(id: number, pointJson: string, tolerance: number): string {
        const shape = this._require(id);
        const point = JSON.parse(pointJson) as [number, number, number];
        const bounds = this._boundingBox(shape);
        let state: 'in' | 'out' | 'on' | 'unknown' = 'unknown';

        if (shape.kind === 'sphere') {
            const radius = Number(shape.params.radius) || 1;
            const distance = Math.sqrt(point[0] * point[0] + point[1] * point[1] + point[2] * point[2]);
            if (Math.abs(distance - radius) <= tolerance) {
                state = 'on';
            } else if (distance < radius) {
                state = 'in';
            } else {
                state = 'out';
            }
        } else {
            const insideX = point[0] > bounds.xMin + tolerance && point[0] < bounds.xMax - tolerance;
            const insideY = point[1] > bounds.yMin + tolerance && point[1] < bounds.yMax - tolerance;
            const insideZ = point[2] > bounds.zMin + tolerance && point[2] < bounds.zMax - tolerance;
            const onX = Math.abs(point[0] - bounds.xMin) <= tolerance || Math.abs(point[0] - bounds.xMax) <= tolerance;
            const onY = Math.abs(point[1] - bounds.yMin) <= tolerance || Math.abs(point[1] - bounds.yMax) <= tolerance;
            const onZ = Math.abs(point[2] - bounds.zMin) <= tolerance || Math.abs(point[2] - bounds.zMax) <= tolerance;
            const withinBounds = point[0] >= bounds.xMin - tolerance
                && point[0] <= bounds.xMax + tolerance
                && point[1] >= bounds.yMin - tolerance
                && point[1] <= bounds.yMax + tolerance
                && point[2] >= bounds.zMin - tolerance
                && point[2] <= bounds.zMax + tolerance;

            if (insideX && insideY && insideZ) {
                state = 'in';
            } else if (withinBounds && (onX || onY || onZ)) {
                state = 'on';
            } else {
                state = 'out';
            }
        }

        return JSON.stringify({
            point,
            tolerance,
            state,
            isInside: state === 'in' || state === 'on',
        });
    }

    findClosestPointOnShape(id: number, pointJson: string, tolerance: number): string {
        const shape = this._require(id);
        const point = JSON.parse(pointJson) as [number, number, number];
        const bounds = this._boundingBox(shape);
        const closestPoint = this._closestPointOnBounds(point, bounds);
        const dx = point[0] - closestPoint[0];
        const dy = point[1] - closestPoint[1];
        const dz = point[2] - closestPoint[2];
        const analysis = this._analysis(shape);

        return JSON.stringify({
            queryPoint: point,
            closestPoint,
            distance: Math.sqrt(dx * dx + dy * dy + dz * dz),
            solutionCount: 1,
            support: {
                kind: 'face',
                topoId: analysis.faceCount > 0 ? 1 : undefined,
                stableHash: analysis.faceCount > 0 ? this._stableHashes(shape, 'face', analysis.faceCount)[0] : undefined,
                uv: [0.5, 0.5],
            },
            tolerance,
        });
    }

    measureShapeDistance(id1: number, id2: number, tolerance: number): string {
        const shapeA = this._require(id1);
        const shapeB = this._require(id2);
        const boundsA = this._boundingBox(shapeA);
        const boundsB = this._boundingBox(shapeB);
        const closest = this._distanceBetweenBounds(boundsA, boundsB);
        const analysisA = this._analysis(shapeA);
        const analysisB = this._analysis(shapeB);

        return JSON.stringify({
            distance: closest.distance,
            clearance: closest.distance,
            innerSolution: false,
            isInContact: closest.distance <= tolerance,
            solutionCount: 1,
            solutions: [{
                pointOnA: closest.pointOnA,
                pointOnB: closest.pointOnB,
                supportOnA: {
                    kind: 'face',
                    topoId: analysisA.faceCount > 0 ? 1 : undefined,
                    stableHash: analysisA.faceCount > 0 ? this._stableHashes(shapeA, 'face', analysisA.faceCount)[0] : undefined,
                    uv: [0.5, 0.5],
                },
                supportOnB: {
                    kind: 'face',
                    topoId: analysisB.faceCount > 0 ? 1 : undefined,
                    stableHash: analysisB.faceCount > 0 ? this._stableHashes(shapeB, 'face', analysisB.faceCount)[0] : undefined,
                    uv: [0.5, 0.5],
                },
            }],
        });
    }

    getRevisionInfo(id: number): string {
        this._require(id);
        return JSON.stringify(this._revisionInfo(id));
    }

    resolveStableEntity(id: number, stableHash: string): string {
        const shape = this._require(id);
        const revision = this._revisionInfo(id);
        const analysis = this._analysis(shape);
        const faceIndex = this._stableHashes(shape, 'face', analysis.faceCount).indexOf(stableHash);
        if (faceIndex >= 0) {
            return JSON.stringify({
                found: true,
                status: 'active',
                kind: 'face',
                id: faceIndex + 1,
                stableHash,
                revisionId: revision.revisionId,
            });
        }
        const edgeIndex = this._stableHashes(shape, 'edge', analysis.edgeCount).indexOf(stableHash);
        if (edgeIndex >= 0) {
            return JSON.stringify({
                found: true,
                status: 'active',
                kind: 'edge',
                id: edgeIndex + 1,
                stableHash,
                revisionId: revision.revisionId,
            });
        }
        const vertexIndex = this._stableHashes(shape, 'vertex', analysis.vertexCount).indexOf(stableHash);
        if (vertexIndex >= 0) {
            return JSON.stringify({
                found: true,
                status: 'active',
                kind: 'vertex',
                id: vertexIndex + 1,
                stableHash,
                revisionId: revision.revisionId,
            });
        }
        return JSON.stringify({
            found: false,
            status: 'unresolved',
            stableHash,
            revisionId: revision.revisionId,
            message: `Stable entity is not present in mock ${shape.kind}`,
        });
    }

    mapEntitiesAcrossRevisions(fromRevisionId: string, toRevisionId: string, stableHashesJson: string): string {
        const stableHashes = JSON.parse(stableHashesJson) as string[];
        return JSON.stringify({
            fromRevisionId,
            toRevisionId,
            mappings: stableHashes.map((stableHash) => ({
                stableHash,
                status: 'mapped',
                mappedStableHash: stableHash,
            })),
        });
    }

    evaluateEdge(id: number, edgeRefJson: string, t: number): string {
        this._require(id);
        const edgeRef = JSON.parse(edgeRefJson) as Record<string, unknown>;
        this._validateRef(edgeRef, 'edgeRef');
        return JSON.stringify({
            edge: { topoId: edgeRef.topoId ?? 1, stableHash: edgeRef.stableHash ?? 'E:mock_edge_1' },
            curveType: 'line',
            parameter: t,
            normalizedParameter: t,
            domain: { first: 0, last: 1 },
            point: [t, 0, 0],
            tangent: [1, 0, 0],
        });
    }

    sampleEdge(id: number, edgeRefJson: string, optionsJson: string): string {
        this._require(id);
        const edgeRef = JSON.parse(edgeRefJson) as Record<string, unknown>;
        const options = JSON.parse(optionsJson) as Record<string, unknown>;
        this._validateRef(edgeRef, 'edgeRef');
        const count = typeof options.count === 'number' ? options.count : 16;
        const samples = Array.from({ length: count }, (_, index) => {
            const t = count === 1 ? 0 : index / (count - 1);
            return { parameter: t, normalizedParameter: t, point: [t, 0, 0], tangent: [1, 0, 0] };
        });
        return JSON.stringify({
            edge: { topoId: edgeRef.topoId ?? 1, stableHash: edgeRef.stableHash ?? 'E:mock_edge_1' },
            curveType: 'line',
            domain: { first: 0, last: 1 },
            samples,
        });
    }

    getEdgeCurve(id: number, edgeRefJson: string): string {
        this._require(id);
        const edgeRef = JSON.parse(edgeRefJson) as Record<string, unknown>;
        this._validateRef(edgeRef, 'edgeRef');
        return JSON.stringify({
            edge: { topoId: edgeRef.topoId ?? 1, stableHash: edgeRef.stableHash ?? 'E:mock_edge_1' },
            curveType: 'line',
            domain: { first: 0, last: 1 },
            startPoint: [0, 0, 0],
            endPoint: [1, 0, 0],
            line: { origin: [0, 0, 0], direction: [1, 0, 0] },
        });
    }

    evaluateFace(id: number, faceRefJson: string, u: number, v: number): string {
        this._require(id);
        const faceRef = JSON.parse(faceRefJson) as Record<string, unknown>;
        this._validateRef(faceRef, 'faceRef');
        return JSON.stringify({
            face: { topoId: faceRef.topoId ?? 1, stableHash: faceRef.stableHash ?? 'F:mock_face_1' },
            surfaceType: 'plane',
            uv: [u, v],
            normalizedUv: [u, v],
            domain: { u: [0, 1], v: [0, 1] },
            point: [u, v, 0],
            dU: [1, 0, 0],
            dV: [0, 1, 0],
            normal: [0, 0, 1],
        });
    }

    getOperationSchema(): string {
        return JSON.stringify({
            schemaVersion: 1,
            operations: {
                extrudeProfile: {
                    schemaVersion: 1,
                    nativeExact: true,
                    requiresMeshFallback: false,
                    supports: {
                        direction: true,
                        draft: true,
                        plane: true,
                        reverseDirection: true,
                        endConditions: ['blind', 'upToNext', 'throughAll', 'upToSurface', 'offsetFromSurface'],
                        surfaceTarget: true,
                        curvedSurfaceTarget: true,
                    },
                },
                extrudeCutProfile: {
                    schemaVersion: 1,
                    nativeExact: true,
                    requiresMeshFallback: false,
                    supports: {
                        direction: true,
                        draft: true,
                        plane: true,
                        reverseDirection: true,
                        endConditions: ['blind', 'upToNext', 'throughAll', 'upToSurface', 'offsetFromSurface'],
                        surfaceTarget: true,
                        curvedSurfaceTarget: true,
                    },
                },
                revolveProfile: {
                    schemaVersion: 1,
                    nativeExact: true,
                    requiresMeshFallback: false,
                    supports: {
                        plane: true,
                        axis: true,
                        reverseDirection: true,
                        signedAngle: true,
                        endConditions: ['angle', 'upToSurface', 'fromSurfaceToSurface', 'throughAll', 'upToSurfaceAtAngle'],
                        surfaceTarget: true,
                        curvedSurfaceTarget: true,
                        slidingEdges: true,
                    },
                },
                revolveCutProfile: {
                    schemaVersion: 1,
                    nativeExact: true,
                    requiresMeshFallback: false,
                    supports: {
                        plane: true,
                        axis: true,
                        reverseDirection: true,
                        signedAngle: true,
                        endConditions: ['angle', 'upToSurface', 'fromSurfaceToSurface', 'throughAll', 'upToSurfaceAtAngle'],
                        surfaceTarget: true,
                        curvedSurfaceTarget: true,
                        slidingEdges: true,
                    },
                },
                sweepProfile: {
                    schemaVersion: 1,
                    nativeExact: true,
                    requiresMeshFallback: false,
                    supports: {
                        cutBoolean: true,
                        plane: true,
                        spine: true,
                        trihedronModes: ['correctedFrenet', 'frenet', 'discrete', 'fixedTrihedron', 'fixedBinormal', 'auxiliarySpine'],
                        sectionWithContact: true,
                        sectionWithCorrection: true,
                        solid: true,
                        forceApproxC1: true,
                        transitionModes: ['transformed', 'rightCorner', 'roundCorner'],
                        tolerances: true,
                        maxDegree: true,
                        maxSegments: true,
                    },
                },
                loft: {
                    schemaVersion: 1,
                    nativeExact: true,
                    requiresMeshFallback: false,
                    supports: {
                        cutBoolean: true,
                        sectionKinds: ['profile', 'wire', 'point'],
                        solid: true,
                        ruled: true,
                        pres3d: true,
                        checkCompatibility: true,
                        smoothing: true,
                        parametrization: ['chordLength', 'centripetal', 'isoParametric'],
                        continuity: ['C0', 'G1', 'C1', 'G2', 'C2', 'C3', 'CN'],
                        criteriumWeight: true,
                        maxDegree: true,
                        mutableInput: true,
                    },
                },
                filletEdges: { schemaVersion: 1, nativeExact: true },
                chamferEdges: { schemaVersion: 1, nativeExact: true },
                getVersionInfo: { schemaVersion: 1, nativeExact: true, sessionScoped: true },
                analyzeShape: { schemaVersion: 1, nativeExact: true, pointContainment: true },
                classifyPointContainment: { schemaVersion: 1, nativeExact: true, states: ['in', 'out', 'on', 'unknown'] },
                intersectShapes: { schemaVersion: 1, nativeExact: true, returnsSectionShape: true },
                findClosestPointOnShape: { schemaVersion: 1, nativeExact: true, supportKinds: ['vertex', 'edge', 'face'] },
                measureShapeDistance: { schemaVersion: 1, nativeExact: true, multipleSolutions: true, supportKinds: ['vertex', 'edge', 'face'] },
                evaluateEdge: { schemaVersion: 1, nativeExact: true },
                sampleEdge: { schemaVersion: 1, nativeExact: true },
                getEdgeCurve: { schemaVersion: 1, nativeExact: true },
                evaluateFace: { schemaVersion: 1, nativeExact: true },
            },
        });
    }

    getCapabilities(): string {
        return JSON.stringify({
            featureEdgesV1: true,
            rawEdgeSegmentsV1: true,
            featurePreviewV1: true,
            tessellationOptionsV1: true,
            triangleNormalsV1: true,
            triangleFaceMappingV1: true,
            topologySubshapesV1: true,
            topologyHierarchyV1: true,
            geometricStableHashesV1: true,
            revisionInfoV1: true,
            entityResolutionV1: true,
            entityRemapV1: true,
            revisionRetentionV1: true,
            historyV1: true,
            stableNamingV1: true,
            checkpointV1: true,
            versionInfoV1: true,
            analysisV1: true,
            sessionHandlesV1: true,
            operations: {
                structuredSpecsV1: true,
                operationSchemaV1: true,
                nativeExactBlendOpsV1: true,
                exactSubshapeEvaluationV1: true,
            },
            extrudeProfile: {
                schemaVersion: 1,
                nativeExact: true,
                direction: true,
                draft: true,
                plane: true,
                reverseDirection: true,
                endConditions: ['blind', 'upToNext', 'throughAll', 'upToSurface', 'offsetFromSurface'],
                surfaceTarget: true,
                curvedSurfaceTarget: true,
            },
            extrudeCutProfile: {
                schemaVersion: 1,
                nativeExact: true,
                direction: true,
                draft: true,
                plane: true,
                reverseDirection: true,
                endConditions: ['blind', 'upToNext', 'throughAll', 'upToSurface', 'offsetFromSurface'],
                surfaceTarget: true,
                curvedSurfaceTarget: true,
            },
            revolveProfile: {
                schemaVersion: 1,
                nativeExact: true,
                plane: true,
                axis: true,
                reverseDirection: true,
                signedAngle: true,
                endConditions: ['angle', 'upToSurface', 'fromSurfaceToSurface', 'throughAll', 'upToSurfaceAtAngle'],
                surfaceTarget: true,
                curvedSurfaceTarget: true,
                slidingEdges: true,
            },
            revolveCutProfile: {
                schemaVersion: 1,
                nativeExact: true,
                plane: true,
                axis: true,
                reverseDirection: true,
                signedAngle: true,
                endConditions: ['angle', 'upToSurface', 'fromSurfaceToSurface', 'throughAll', 'upToSurfaceAtAngle'],
                surfaceTarget: true,
                curvedSurfaceTarget: true,
                slidingEdges: true,
            },
            sweepProfile: {
                schemaVersion: 1,
                nativeExact: true,
                cutBoolean: true,
                plane: true,
                spine: true,
                trihedronModes: ['correctedFrenet', 'frenet', 'discrete', 'fixedTrihedron', 'fixedBinormal', 'auxiliarySpine'],
                sectionWithContact: true,
                sectionWithCorrection: true,
                solid: true,
                forceApproxC1: true,
                transitionModes: ['transformed', 'rightCorner', 'roundCorner'],
                tolerances: true,
                maxDegree: true,
                maxSegments: true,
            },
            loft: {
                schemaVersion: 1,
                nativeExact: true,
                cutBoolean: true,
                sectionKinds: ['profile', 'wire', 'point'],
                solid: true,
                ruled: true,
                pres3d: true,
                checkCompatibility: true,
                smoothing: true,
                parametrization: ['chordLength', 'centripetal', 'isoParametric'],
                continuity: ['C0', 'G1', 'C1', 'G2', 'C2', 'C3', 'CN'],
                criteriumWeight: true,
                maxDegree: true,
                mutableInput: true,
            },
            fillet: {
                schemaVersion: 1,
                nativeExact: true,
                constantRadius: true,
                startEndRadius: true,
                stationRadii: true,
                lawRadius: ['constant', 'linear'],
                tangentPropagation: true,
                partialEdges: false,
                setbackCorners: false,
                blendShape: ['rational', 'quasiAngular', 'polynomial'],
                continuity: ['C0', 'C1', 'C2'],
                overflowModes: ['fail'],
            },
            chamfer: {
                schemaVersion: 1,
                nativeExact: true,
                symmetric: true,
                twoDistance: true,
                distanceAngle: true,
                referenceFace: true,
                tangentPropagation: true,
                partialEdges: false,
                setbackCorners: false,
                overflowModes: ['fail'],
            },
            subshapeEvaluation: {
                evaluateEdge: true,
                sampleEdge: true,
                getEdgeCurve: true,
                evaluateFace: true,
                parameterModes: ['normalized', 'native'],
            },
            analysis: {
                volume: true,
                surfaceArea: true,
                linearLength: true,
                boundingBox: true,
                centerOfMass: true,
                shapeValidity: true,
                pointContainment: true,
                shapeIntersection: true,
                closestPoint: true,
                shapeDistance: true,
            },
            runtime: {
                browser: true,
                worker: true,
                node: true,
            },
        });
    }

    checkValidity(id: number): boolean {
        const shape = this._shapes.get(id);
        return shape !== undefined && shape.params.invalid !== true;
    }

    // -- Tessellation --

    tessellate(id: number, linearDeflection: number, angularDeflection: number): string {
        return this.tessellateWithOptions(id, linearDeflection, angularDeflection, '');
    }

    tessellateWithOptions(id: number, _linearDeflection: number, _angularDeflection: number, optionsJson: string): string {
        this._require(id);
        const options = optionsJson.length > 0 ? JSON.parse(optionsJson) as Record<string, unknown> : {};
        const includeMetadata = options.includeMetadata !== false;
        const includeTriangleNormals = options.includeTriangleNormals ?? includeMetadata;
        const includeTriangleTopoFaceIds = options.includeTriangleTopoFaceIds ?? includeMetadata;
        const includeTriangleFaceGroups = options.includeTriangleFaceGroups ?? includeMetadata;
        const includeTriangleStableHashes = options.includeTriangleStableHashes ?? includeMetadata;
        const includeFeatureEdges = options.includeFeatureEdges ?? includeMetadata;
        const includeRawEdgeSegments = options.includeRawEdgeSegments ?? includeMetadata;
        const hasFaceSubset = Array.isArray(options.faces) && options.faces.length > 0;
        const triangleTopoFaceId = hasFaceSubset && typeof (options.faces as Array<{ topoId?: unknown }>)[0]?.topoId === 'number'
            ? (options.faces as Array<{ topoId: number }>)[0].topoId
            : 1;
        // Minimal triangle (not geometrically meaningful — mock only)
        return JSON.stringify({
            positions: [0, 0, 0, 1, 0, 0, 0, 1, 0],
            normals:   [0, 0, 1, 0, 0, 1, 0, 0, 1],
            indices:   [0, 1, 2],
            ...(includeTriangleNormals ? { triangleNormals: [0, 0, 1] } : {}),
            ...(includeTriangleTopoFaceIds ? { triangleTopoFaceIds: [triangleTopoFaceId] } : {}),
            ...(includeTriangleFaceGroups ? { triangleFaceGroups: [triangleTopoFaceId] } : {}),
            ...(includeTriangleStableHashes ? { triangleStableHashes: ['F:mock_face_1'] } : {}),
            ...(includeFeatureEdges ? { featureEdges: [
                {
                    points: [[0, 0, 0], [1, 0, 0]],
                    isClosed: false,
                    chainId: 1,
                    faceIndices: [1, 2],
                    topoFaceIds: [1, 2],
                    isBoundary: false,
                    isSharp: true,
                    isSeam: false,
                    stableHash: 'E:mock_edge_1',
                },
            ] } : {}),
            ...(includeRawEdgeSegments ? { rawEdgeSegments: [0, 0, 0, 1, 0, 0] } : {}),
        });
    }

    // -- Import / export --

    importStep(content: string): number {
        const result = JSON.parse(this.importStepDetailed(content, false, false, false, false, 1e-6)) as MockImportResult;
        if (result.shapeId === undefined) {
            throw new KernelError('IMPORT_FAILED', result.messageList[0]?.text ?? 'STEP import failed');
        }
        return result.shapeId;
    }

    importStepDetailed(
        content: string,
        heal: boolean,
        sew: boolean,
        fixSameParameter: boolean,
        fixSolid: boolean,
        _sewingTolerance: number,
    ): string {
        const trimmed = typeof content === 'string' ? content.trim() : '';
        if (trimmed.length === 0) {
            return JSON.stringify({
                readStatus: 'IFSelect_RetError',
                transferStatus: 'FAILED',
                rootCount: 0,
                transferredRootCount: 0,
                messageList: [{ phase: 'load', severity: 'fail', text: 'STEP content is empty' }],
                isValid: false,
                wasValidBeforeHealing: false,
                healed: false,
            } satisfies MockImportResult);
        }

        if (!content.includes('ISO-10303')) {
            return JSON.stringify({
                readStatus: 'IFSelect_RetFail',
                transferStatus: 'FAILED',
                rootCount: 1,
                transferredRootCount: 0,
                messageList: [{ phase: 'load', severity: 'fail', text: 'Content does not appear to be a valid STEP file' }],
                isValid: false,
                wasValidBeforeHealing: false,
                healed: false,
            } satisfies MockImportResult);
        }

        const wasValidBeforeHealing = !content.includes('MOCK_INVALID_SHAPE');
        const healedShape = !wasValidBeforeHealing && (heal || sew || fixSameParameter || fixSolid);
        const isValid = wasValidBeforeHealing || healedShape;
        const shapeId = this._store('imported', {
            content: content.slice(0, 64),
            invalid: !isValid,
        });

        return JSON.stringify({
            readStatus: 'IFSelect_RetDone',
            transferStatus: 'DONE',
            rootCount: 1,
            transferredRootCount: 1,
            messageList: [
                ...(!wasValidBeforeHealing
                    ? [{ phase: 'validation' as const, severity: isValid ? 'info' as const : 'warning' as const, text: isValid ? 'Healing produced a valid shape' : 'Imported shape is not valid according to BRepCheck_Analyzer' }]
                    : []),
            ],
            shapeId,
            isValid,
            wasValidBeforeHealing,
            healed: healedShape,
        } satisfies MockImportResult);
    }

    importStepPackage(
        content: string,
        heal: boolean,
        sew: boolean,
        fixSameParameter: boolean,
        fixSolid: boolean,
        sewingTolerance: number,
        linearDeflection: number,
        angularDeflection: number,
    ): string {
        const detailedJson = this.importStepDetailed(content, heal, sew, fixSameParameter, fixSolid, sewingTolerance);
        const detailed = JSON.parse(detailedJson);

        if (detailed.shapeId !== undefined) {
            const shapeId = detailed.shapeId;
            const revision = this._revisionInfo(shapeId);
            const properties = JSON.parse(this.analyzeShape(shapeId));
            const checkpoint = JSON.parse(this.createCheckpoint(shapeId));
            const mesh = (linearDeflection > 0 && angularDeflection > 0)
                ? JSON.parse(this.tessellate(shapeId, linearDeflection, angularDeflection))
                : null;

            const topology = {
                solidCount: properties.solidCount ?? 0,
                shellCount: properties.shellCount ?? 0,
                wireCount: properties.wireCount ?? 0,
                faceCount: properties.faceCount ?? 0,
                edgeCount: properties.edgeCount ?? 0,
                vertexCount: properties.vertexCount ?? 0,
                isValid: properties.isValid ?? false,
            };

            return JSON.stringify({
                readStatus: detailed.readStatus,
                transferStatus: detailed.transferStatus,
                healed: detailed.healed,
                isValid: detailed.isValid,
                messageList: detailed.messageList,
                shapeId,
                revision: {
                    revisionId: revision.revisionId,
                    topologyHash: revision.topologyHash,
                },
                topology,
                properties,
                checkpoint,
                mesh,
            });
        }

        return JSON.stringify({
            readStatus: detailed.readStatus,
            transferStatus: detailed.transferStatus,
            healed: detailed.healed,
            isValid: detailed.isValid,
            messageList: detailed.messageList,
            shapeId: null,
        });
    }

    exportStep(id: number): string {
        this._require(id);
        return [
            'ISO-10303-21;',
            'HEADER;',
            "FILE_DESCRIPTION(('occt-kernel-wasm mock export'),'2;1');",
            "FILE_NAME('','',(''),(''),'','','');",
            "FILE_SCHEMA(('AUTOMOTIVE_DESIGN'));",
            'ENDSEC;',
            'DATA;',
            'ENDSEC;',
            'END-ISO-10303-21;',
        ].join('\n');
    }

    createCheckpoint(id: number): string {
        this._require(id);
        return JSON.stringify({
            checkpointSchemaVersion: 1,
            brep: `mock-brep:${id}`,
            revision: this._revisionInfo(id),
        });
    }

    hydrateCheckpoint(checkpointJson: string): number {
        let checkpoint: { revision?: Record<string, unknown> };
        try {
            checkpoint = JSON.parse(checkpointJson) as { revision?: Record<string, unknown> };
        } catch (error) {
            const message = error instanceof Error ? error.message : 'Checkpoint JSON is invalid';
            throw new KernelError('INVALID_CHECKPOINT', message);
        }
        const revisionInfo = {
            ...(checkpoint.revision ?? {}),
            createdFromCheckpoint: true,
        };
        return this._store('checkpoint', { revisionInfo });
    }

    // -- Memory --

    disposeShape(id: number): void {
        this._shapes.delete(id);
        this._refCounts.delete(id);
    }

    retainRevision(id: number): void {
        this._require(id);
        this._refCounts.set(id, (this._refCounts.get(id) ?? 1) + 1);
    }

    releaseRevision(id: number): boolean {
        this._require(id);
        const nextCount = (this._refCounts.get(id) ?? 1) - 1;
        if (nextCount > 0) {
            this._refCounts.set(id, nextCount);
            return false;
        }
        this.disposeShape(id);
        return true;
    }

    /** Returns the number of live shapes (for leak detection in tests). */
    liveShapeCount(): number {
        return this._shapes.size;
    }
}

// ---------------------------------------------------------------------------
// Module factory (mirrors the real Emscripten module factory signature)
// ---------------------------------------------------------------------------

export interface MockWasmModule {
    OcctKernel: new () => MockNativeKernel;
}

/** Creates a mock WASM module. Resolves immediately (no async IO needed). */
export default function createMockModule(): Promise<MockWasmModule> {
    return Promise.resolve({
        OcctKernel: MockNativeKernel,
    });
}
