const FULL_CIRCLE_EPS = 1.0e-6;
const PARALLEL_EPS = 0.01;
const ANGLE_RAY_EPS = 1.0e-9;

export type SmartDimensionKind = 'distance' | 'dx' | 'dy' | 'angle' | 'radius' | 'diameter';
export type SmartSegmentEndpointKey = 'p1' | 'p2';

export interface SmartPointLike {
    readonly type: 'point';
    readonly x: number;
    readonly y: number;
    readonly id?: number;
}

export interface SmartVertexLike {
    readonly x: number;
    readonly y: number;
}

export interface SmartSegmentLike {
    readonly type: 'segment';
    readonly x1: number;
    readonly y1: number;
    readonly x2: number;
    readonly y2: number;
    readonly midX?: number;
    readonly midY?: number;
    readonly p1?: SmartVertexLike;
    readonly p2?: SmartVertexLike;
    readonly id?: number;
}

export interface SmartCurveLike {
    readonly type: 'circle' | 'arc';
    readonly cx: number;
    readonly cy: number;
    readonly radius: number;
    readonly sweepAngle?: number;
    readonly id?: number;
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

interface SmartAngleInfoOptions {
    readonly endpointAKey?: SmartSegmentEndpointKey | null;
    readonly endpointBKey?: SmartSegmentEndpointKey | null;
}

interface SegmentRayCandidate {
    readonly endpointKey: SmartSegmentEndpointKey;
    readonly angle: number;
    readonly length: number;
}

function isPointLike(entity: SmartDimensionEntityLike | null | undefined): entity is SmartPointLike {
    return !!entity && entity.type === 'point';
}

function isSegmentLike(entity: SmartDimensionEntityLike | null | undefined): entity is SmartSegmentLike {
    return !!entity && entity.type === 'segment';
}

function isCurveLike(entity: SmartDimensionEntityLike | null | undefined): entity is SmartCurveLike {
    return !!entity && (entity.type === 'circle' || entity.type === 'arc');
}

function anchorX(entity: SmartDimensionEntityLike): number {
    if (isSegmentLike(entity)) {
        return Number(entity.x1);
    }
    if (isPointLike(entity)) {
        return Number(entity.x);
    }
    return Number(entity.cx);
}

function anchorY(entity: SmartDimensionEntityLike): number {
    if (isSegmentLike(entity)) {
        return Number(entity.y1);
    }
    if (isPointLike(entity)) {
        return Number(entity.y);
    }
    return Number(entity.cy);
}

function segmentMidX(segment: SmartSegmentLike): number {
    return Number.isFinite(segment.midX) ? Number(segment.midX) : (Number(segment.x1) + Number(segment.x2)) / 2;
}

function segmentMidY(segment: SmartSegmentLike): number {
    return Number.isFinite(segment.midY) ? Number(segment.midY) : (Number(segment.y1) + Number(segment.y2)) / 2;
}

function footOnLine(px: number, py: number, ax: number, ay: number, bx: number, by: number): { x: number; y: number } {
    const dx = bx - ax;
    const dy = by - ay;
    const lenSq = dx * dx + dy * dy;
    if (lenSq === 0) {
        return { x: ax, y: ay };
    }
    const t = ((px - ax) * dx + (py - ay) * dy) / lenSq;
    return { x: ax + t * dx, y: ay + t * dy };
}

function footOnSegment(px: number, py: number, ax: number, ay: number, bx: number, by: number): { x: number; y: number } {
    const dx = bx - ax;
    const dy = by - ay;
    const lenSq = dx * dx + dy * dy;
    if (lenSq === 0) {
        return { x: ax, y: ay };
    }
    let t = ((px - ax) * dx + (py - ay) * dy) / lenSq;
    t = Math.max(0, Math.min(1, t));
    return { x: ax + t * dx, y: ay + t * dy };
}

export function prefersDiameterByDefault(shape: SmartDimensionEntityLike | null | undefined): boolean {
    if (!isCurveLike(shape)) {
        return false;
    }
    if (shape.type === 'circle') {
        return true;
    }
    return Math.abs(Math.abs(Number(shape.sweepAngle ?? 0)) - Math.PI * 2) <= FULL_CIRCLE_EPS;
}

function normalizeAngle(angle: number): number {
    let value = angle;
    while (value > Math.PI) {
        value -= 2 * Math.PI;
    }
    while (value < -Math.PI) {
        value += 2 * Math.PI;
    }
    return value;
}

function lineIntersection(a1x: number, a1y: number, a2x: number, a2y: number, b1x: number, b1y: number, b2x: number, b2y: number): { x: number; y: number } {
    const dAx = a2x - a1x;
    const dAy = a2y - a1y;
    const dBx = b2x - b1x;
    const dBy = b2y - b1y;
    const denom = dAx * dBy - dAy * dBx;
    if (Math.abs(denom) < 1.0e-9) {
        return {
            x: (a1x + a2x + b1x + b2x) / 4,
            y: (a1y + a2y + b1y + b2y) / 4,
        };
    }
    const t = ((b1x - a1x) * dBy - (b1y - a1y) * dBx) / denom;
    return {
        x: a1x + t * dAx,
        y: a1y + t * dAy,
    };
}

function segmentRayCandidate(segment: SmartSegmentLike, vertex: { x: number; y: number }, endpointKey: SmartSegmentEndpointKey): SegmentRayCandidate | null {
    const endpoint = segment?.[endpointKey];
    if (!endpoint) {
        return null;
    }
    const dx = Number(endpoint.x) - vertex.x;
    const dy = Number(endpoint.y) - vertex.y;
    const length = Math.hypot(dx, dy);
    if (length <= ANGLE_RAY_EPS) {
        return null;
    }
    return {
        endpointKey,
        angle: Math.atan2(dy, dx),
        length,
    };
}

function compareAngleScores(left: readonly number[], right: readonly number[]): number {
    for (let index = 0; index < Math.max(left.length, right.length); index += 1) {
        const lhs = left[index] ?? 0;
        const rhs = right[index] ?? 0;
        if (lhs < rhs) {
            return -1;
        }
        if (lhs > rhs) {
            return 1;
        }
    }
    return 0;
}

function fallbackSegmentAngleInfo(segA: SmartSegmentLike, segB: SmartSegmentLike): SmartSegmentAngleInfo {
    const vertex = lineIntersection(segA.x1, segA.y1, segA.x2, segA.y2, segB.x1, segB.y1, segB.x2, segB.y2);
    const angleA = Math.atan2(segA.y2 - segA.y1, segA.x2 - segA.x1);
    const angleB = Math.atan2(segB.y2 - segB.y1, segB.x2 - segB.x1);
    return {
        vx: vertex.x,
        vy: vertex.y,
        startAngle: angleA,
        sweep: normalizeAngle(angleB - angleA),
        angleEndpointAKey: null,
        angleEndpointBKey: null,
    };
}

function areParallel(segA: SmartSegmentLike, segB: SmartSegmentLike): boolean {
    const dxA = segA.x2 - segA.x1;
    const dyA = segA.y2 - segA.y1;
    const dxB = segB.x2 - segB.x1;
    const dyB = segB.y2 - segB.y1;
    const lenA = Math.hypot(dxA, dyA) || 1.0e-9;
    const lenB = Math.hypot(dxB, dyB) || 1.0e-9;
    const cross = Math.abs(dxA * dyB - dyA * dxB) / (lenA * lenB);
    return cross < PARALLEL_EPS;
}

export function buildSmartSegmentAngleInfo(segA: SmartSegmentLike, segB: SmartSegmentLike, options: SmartAngleInfoOptions = {}): SmartSegmentAngleInfo {
    const vertex = lineIntersection(segA.x1, segA.y1, segA.x2, segA.y2, segB.x1, segB.y1, segB.x2, segB.y2);
    const candidateKeysA: SmartSegmentEndpointKey[] = options.endpointAKey ? [options.endpointAKey] : ['p1', 'p2'];
    const candidateKeysB: SmartSegmentEndpointKey[] = options.endpointBKey ? [options.endpointBKey] : ['p1', 'p2'];
    const candidatesA = candidateKeysA.map((key) => segmentRayCandidate(segA, vertex, key)).filter((candidate): candidate is SegmentRayCandidate => candidate !== null);
    const candidatesB = candidateKeysB.map((key) => segmentRayCandidate(segB, vertex, key)).filter((candidate): candidate is SegmentRayCandidate => candidate !== null);

    let best: (SmartSegmentAngleInfo & { readonly score: readonly number[] }) | null = null;
    for (const candidateA of candidatesA) {
        for (const candidateB of candidatesB) {
            const sweep = normalizeAngle(candidateB.angle - candidateA.angle);
            const score = [
                Math.abs(sweep),
                -(candidateA.length + candidateB.length),
                sweep < 0 ? 1 : 0,
            ] as const;
            if (!best || compareAngleScores(score, best.score) < 0) {
                best = {
                    vx: vertex.x,
                    vy: vertex.y,
                    startAngle: candidateA.angle,
                    sweep,
                    angleEndpointAKey: candidateA.endpointKey,
                    angleEndpointBKey: candidateB.endpointKey,
                    score,
                };
            }
        }
    }

    if (best) {
        return best;
    }
    return fallbackSegmentAngleInfo(segA, segB);
}

export function detectDimensionType(a: SmartDimensionEntityLike, b?: SmartDimensionEntityLike | null): SmartDimensionCandidate | null {
    if (!b) {
        if (isSegmentLike(a)) {
            return { dimType: 'distance', x1: a.x1, y1: a.y1, x2: a.x2, y2: a.y2 };
        }
        if (isCurveLike(a)) {
            const dimType = prefersDiameterByDefault(a) ? 'diameter' : 'radius';
            return { dimType, x1: a.cx, y1: a.cy, x2: a.cx + a.radius, y2: a.cy };
        }
        return null;
    }

    if (isPointLike(a) && isPointLike(b)) {
        return { dimType: 'distance', x1: a.x, y1: a.y, x2: b.x, y2: b.y };
    }

    if (isSegmentLike(a) && isSegmentLike(b)) {
        if (areParallel(a, b)) {
            const foot = footOnLine(segmentMidX(a), segmentMidY(a), b.x1, b.y1, b.x2, b.y2);
            return { dimType: 'distance', x1: segmentMidX(a), y1: segmentMidY(a), x2: foot.x, y2: foot.y };
        }
        const info = buildSmartSegmentAngleInfo(a, b);
        return {
            dimType: 'angle',
            x1: info.vx,
            y1: info.vy,
            x2: info.vx,
            y2: info.vy,
            angleStart: info.startAngle,
            angleSweep: info.sweep,
            angleEndpointAKey: info.angleEndpointAKey,
            angleEndpointBKey: info.angleEndpointBKey,
        };
    }

    if (isPointLike(a) && isSegmentLike(b)) {
        const foot = footOnSegment(a.x, a.y, b.x1, b.y1, b.x2, b.y2);
        return { dimType: 'distance', x1: a.x, y1: a.y, x2: foot.x, y2: foot.y };
    }
    if (isSegmentLike(a) && isPointLike(b)) {
        const foot = footOnSegment(b.x, b.y, a.x1, a.y1, a.x2, a.y2);
        return { dimType: 'distance', x1: b.x, y1: b.y, x2: foot.x, y2: foot.y };
    }

    if (isCurveLike(a) && isPointLike(b)) {
        return { dimType: 'distance', x1: a.cx, y1: a.cy, x2: b.x, y2: b.y };
    }
    if (isPointLike(a) && isCurveLike(b)) {
        return { dimType: 'distance', x1: a.x, y1: a.y, x2: b.cx, y2: b.cy };
    }

    if (isCurveLike(a) && isCurveLike(b)) {
        return { dimType: 'distance', x1: a.cx, y1: a.cy, x2: b.cx, y2: b.cy };
    }

    if (isSegmentLike(a) && isCurveLike(b)) {
        return { dimType: 'distance', x1: segmentMidX(a), y1: segmentMidY(a), x2: b.cx, y2: b.cy };
    }
    if (isCurveLike(a) && isSegmentLike(b)) {
        return { dimType: 'distance', x1: a.cx, y1: a.cy, x2: segmentMidX(b), y2: segmentMidY(b) };
    }

    return {
        dimType: 'distance',
        x1: anchorX(a),
        y1: anchorY(a),
        x2: anchorX(b),
        y2: anchorY(b),
    };
}

export function detectAllDimensionTypes(a: SmartDimensionEntityLike, b?: SmartDimensionEntityLike | null): SmartDimensionCandidate[] {
    const results: SmartDimensionCandidate[] = [];

    if (!b) {
        if (isSegmentLike(a)) {
            results.push({ dimType: 'distance', label: 'Length', x1: a.x1, y1: a.y1, x2: a.x2, y2: a.y2 });
            results.push({ dimType: 'dx', label: 'Horizontal (ΔX)', x1: a.x1, y1: a.y1, x2: a.x2, y2: a.y2 });
            results.push({ dimType: 'dy', label: 'Vertical (ΔY)', x1: a.x1, y1: a.y1, x2: a.x2, y2: a.y2 });
        }
        if (isCurveLike(a)) {
            const curveTypes: Array<{ dimType: SmartDimensionKind; label: string }> = prefersDiameterByDefault(a)
                ? [{ dimType: 'diameter', label: 'Diameter' }, { dimType: 'radius', label: 'Radius' }]
                : [{ dimType: 'radius', label: 'Radius' }, { dimType: 'diameter', label: 'Diameter' }];
            for (const curveType of curveTypes) {
                results.push({
                    ...curveType,
                    x1: a.cx,
                    y1: a.cy,
                    x2: a.cx + a.radius,
                    y2: a.cy,
                });
            }
        }
        return results;
    }

    if (isPointLike(a) && isPointLike(b)) {
        results.push({ dimType: 'distance', label: 'Distance', x1: a.x, y1: a.y, x2: b.x, y2: b.y });
        results.push({ dimType: 'dx', label: 'Horizontal (ΔX)', x1: a.x, y1: a.y, x2: b.x, y2: b.y });
        results.push({ dimType: 'dy', label: 'Vertical (ΔY)', x1: a.x, y1: a.y, x2: b.x, y2: b.y });
        return results;
    }

    if (isSegmentLike(a) && isSegmentLike(b)) {
        if (areParallel(a, b)) {
            const midX = segmentMidX(a);
            const midY = segmentMidY(a);
            const foot = footOnLine(midX, midY, b.x1, b.y1, b.x2, b.y2);
            results.push({ dimType: 'distance', label: 'Distance', x1: midX, y1: midY, x2: foot.x, y2: foot.y });
        } else {
            const info = buildSmartSegmentAngleInfo(a, b);
            results.push({
                dimType: 'angle',
                label: 'Angle',
                x1: info.vx,
                y1: info.vy,
                x2: info.vx,
                y2: info.vy,
                angleStart: info.startAngle,
                angleSweep: info.sweep,
                sourceA: a,
                sourceB: b,
                angleEndpointAKey: info.angleEndpointAKey,
                angleEndpointBKey: info.angleEndpointBKey,
            });
            const invertedInfo = buildSmartSegmentAngleInfo(b, a);
            results.push({
                dimType: 'angle',
                label: 'Angle (Inverted)',
                x1: invertedInfo.vx,
                y1: invertedInfo.vy,
                x2: invertedInfo.vx,
                y2: invertedInfo.vy,
                angleStart: invertedInfo.startAngle,
                angleSweep: invertedInfo.sweep,
                sourceA: b,
                sourceB: a,
                angleEndpointAKey: invertedInfo.angleEndpointAKey,
                angleEndpointBKey: invertedInfo.angleEndpointBKey,
            });
            const midX = segmentMidX(a);
            const midY = segmentMidY(a);
            const foot = footOnLine(midX, midY, b.x1, b.y1, b.x2, b.y2);
            results.push({ dimType: 'distance', label: 'Distance', x1: midX, y1: midY, x2: foot.x, y2: foot.y });
        }
        return results;
    }

    if (isPointLike(a) && isSegmentLike(b)) {
        const foot = footOnSegment(a.x, a.y, b.x1, b.y1, b.x2, b.y2);
        results.push({ dimType: 'distance', label: 'Distance', x1: a.x, y1: a.y, x2: foot.x, y2: foot.y });
        return results;
    }
    if (isSegmentLike(a) && isPointLike(b)) {
        const foot = footOnSegment(b.x, b.y, a.x1, a.y1, a.x2, a.y2);
        results.push({ dimType: 'distance', label: 'Distance', x1: b.x, y1: b.y, x2: foot.x, y2: foot.y });
        return results;
    }

    if (isCurveLike(a) && isPointLike(b)) {
        results.push({ dimType: 'distance', label: 'Distance (center)', x1: a.cx, y1: a.cy, x2: b.x, y2: b.y });
        return results;
    }
    if (isPointLike(a) && isCurveLike(b)) {
        results.push({ dimType: 'distance', label: 'Distance (center)', x1: a.x, y1: a.y, x2: b.cx, y2: b.cy });
        return results;
    }

    if (isCurveLike(a) && isCurveLike(b)) {
        results.push({ dimType: 'distance', label: 'Distance (centers)', x1: a.cx, y1: a.cy, x2: b.cx, y2: b.cy });
        return results;
    }

    if (isSegmentLike(a) && isCurveLike(b)) {
        results.push({ dimType: 'distance', label: 'Distance', x1: segmentMidX(a), y1: segmentMidY(a), x2: b.cx, y2: b.cy });
        return results;
    }
    if (isCurveLike(a) && isSegmentLike(b)) {
        results.push({ dimType: 'distance', label: 'Distance', x1: a.cx, y1: a.cy, x2: segmentMidX(b), y2: segmentMidY(b) });
        return results;
    }

    results.push({
        dimType: 'distance',
        label: 'Distance',
        x1: anchorX(a),
        y1: anchorY(a),
        x2: anchorX(b),
        y2: anchorY(b),
    });
    return results;
}

export const smartDimensions = Object.freeze({
    prefersDiameterByDefault,
    detectDimensionType,
    detectAllDimensionTypes,
    buildSmartSegmentAngleInfo,
});