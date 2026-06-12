import { existsSync } from 'node:fs';
import { join } from 'node:path';

jest.setTimeout(120000);

function pointLineDistance(point: { x: number; y: number }, lineA: { x: number; y: number }, lineB: { x: number; y: number }): number {
    const dx = lineB.x - lineA.x;
    const dy = lineB.y - lineA.y;
    const length = Math.hypot(dx, dy);
    if (length <= 1.0e-12) {
        throw new Error('line length must be non-zero');
    }
    return Math.abs(dx * (lineA.y - point.y) - dy * (lineA.x - point.x)) / length;
}

const distDir = join(__dirname, '..', '..', 'dist');
const hasBuiltSketchToolkit = ['sketch-toolkit.js', 'sketch-toolkit.wasm.js', 'sketch-toolkit.wasm.wasm'].every((fileName) =>
    existsSync(join(distDir, fileName)));
const describeIfBuilt = hasBuiltSketchToolkit ? describe : describe.skip;

describeIfBuilt('Real WASM sketch-toolkit integration', () => {
    let createSketchToolkit: () => Promise<any>;

    beforeAll(() => {
        const distSketchToolkit = require(join(distDir, 'sketch-toolkit.js')) as {
            createSketchToolkit: () => Promise<any>;
        };
        createSketchToolkit = distSketchToolkit.createSketchToolkit;
    });

    it('solves a fixed horizontal distance sketch through the compiled sketch-toolkit target', async () => {
        const toolkit = await createSketchToolkit();
        const sketch = toolkit.createSketch({
            name: 'Point solve',
            plane: {
                origin: [0, 0, 0],
                normal: [0, 0, 1],
                xAxis: [1, 0, 0],
                yAxis: [0, 1, 0],
            },
        });

        try {
            const pointA = toolkit.addPoint(sketch, { x: 0, y: 0, fixed: true });
            const pointB = toolkit.addPoint(sketch, { x: 3, y: 4 });
            const line = toolkit.addLineSegment(sketch, { start: pointA, end: pointB });

            toolkit.addConstraint(sketch, { kind: 'horizontal', entity: line });
            toolkit.addConstraint(sketch, {
                kind: 'distance-point-point',
                pointA,
                pointB,
                value: 10,
            });

            const result = toolkit.solveSketch(sketch, {
                algorithm: 'lm',
                maxIterations: 64,
                residualTolerance: 1e-8,
                stepTolerance: 1e-10,
            });
            const snapshot = toolkit.getSketchSnapshot(sketch);
            const solvedPointB = snapshot.entities.find((entity: any) => entity.id === pointB && entity.kind === 'point');

            expect(result.converged).toBe(true);
            expect(result.algorithm).toBe('lm');
            expect(result.status).toBe('fully-defined');
            expect(result.maxScaledResidual).toBeLessThanOrEqual(1e-6);
            expect(result.diagnostics.items.some((item: any) => item.code === 'NATIVE_POINT_SOLVER')).toBe(true);

            expect(solvedPointB).toBeDefined();
            expect(solvedPointB.x).toBeCloseTo(10, 6);
            expect(solvedPointB.y).toBeCloseTo(0, 6);
        } finally {
            toolkit.disposeSketch(sketch);
        }
    });

    it('treats explicit fix constraints as structural anchors in the compiled sketch-toolkit target', async () => {
        const toolkit = await createSketchToolkit();
        const sketch = toolkit.createSketch({
            name: 'Fix solve',
            plane: {
                origin: [0, 0, 0],
                normal: [0, 0, 1],
                xAxis: [1, 0, 0],
                yAxis: [0, 1, 0],
            },
        });

        try {
            const pointA = toolkit.addPoint(sketch, { x: 2, y: 3 });
            const pointB = toolkit.addPoint(sketch, { x: 6, y: 7 });
            const line = toolkit.addLineSegment(sketch, { start: pointA, end: pointB });

            toolkit.addConstraint(sketch, { kind: 'fix', entity: pointA });
            toolkit.addConstraint(sketch, { kind: 'horizontal', entity: line });
            toolkit.addConstraint(sketch, {
                kind: 'distance-point-point',
                pointA,
                pointB,
                value: 10,
            });

            const result = toolkit.solveSketch(sketch, {
                algorithm: 'lm',
                maxIterations: 64,
                residualTolerance: 1e-8,
                stepTolerance: 1e-10,
            });
            const snapshot = toolkit.getSketchSnapshot(sketch);
            const solvedPointA = snapshot.entities.find((entity: any) => entity.id === pointA && entity.kind === 'point');
            const solvedPointB = snapshot.entities.find((entity: any) => entity.id === pointB && entity.kind === 'point');

            expect(result.converged).toBe(true);
            expect(result.status).toBe('fully-defined');
            expect(result.maxScaledResidual).toBeLessThanOrEqual(1e-6);

            expect(solvedPointA).toBeDefined();
            expect(solvedPointA.x).toBeCloseTo(2, 6);
            expect(solvedPointA.y).toBeCloseTo(3, 6);
            expect(solvedPointB).toBeDefined();
            expect(solvedPointB.x).toBeCloseTo(12, 6);
            expect(solvedPointB.y).toBeCloseTo(3, 6);
        } finally {
            toolkit.disposeSketch(sketch);
        }
    });

    it('solves a parallel equal-length line relation through the compiled sketch-toolkit target', async () => {
        const toolkit = await createSketchToolkit();
        const sketch = toolkit.createSketch({
            name: 'Parallel solve',
            plane: {
                origin: [0, 0, 0],
                normal: [0, 0, 1],
                xAxis: [1, 0, 0],
                yAxis: [0, 1, 0],
            },
        });

        try {
            const pointA0 = toolkit.addPoint(sketch, { x: 0, y: 0, fixed: true });
            const pointA1 = toolkit.addPoint(sketch, { x: 10, y: 0, fixed: true });
            const pointB0 = toolkit.addPoint(sketch, { x: 1, y: 3, fixed: true });
            const pointB1 = toolkit.addPoint(sketch, { x: 4, y: 5 });
            const lineA = toolkit.addLineSegment(sketch, { start: pointA0, end: pointA1 });
            const lineB = toolkit.addLineSegment(sketch, { start: pointB0, end: pointB1 });

            toolkit.addConstraint(sketch, { kind: 'parallel', entityA: lineA, entityB: lineB });
            toolkit.addConstraint(sketch, { kind: 'equal-length', entityA: lineA, entityB: lineB });

            const result = toolkit.solveSketch(sketch, {
                algorithm: 'lm',
                maxIterations: 64,
                residualTolerance: 1e-8,
                stepTolerance: 1e-10,
            });
            const snapshot = toolkit.getSketchSnapshot(sketch);
            const solvedPointB1 = snapshot.entities.find((entity: any) => entity.id === pointB1 && entity.kind === 'point');

            expect(result.converged).toBe(true);
            expect(result.status).toBe('fully-defined');
            expect(result.maxScaledResidual).toBeLessThanOrEqual(1e-6);

            expect(solvedPointB1).toBeDefined();
            expect(solvedPointB1.x).toBeCloseTo(11, 6);
            expect(solvedPointB1.y).toBeCloseTo(3, 6);
        } finally {
            toolkit.disposeSketch(sketch);
        }
    });

    it('solves a perpendicular equal-length line relation through the compiled sketch-toolkit target', async () => {
        const toolkit = await createSketchToolkit();
        const sketch = toolkit.createSketch({
            name: 'Perpendicular solve',
            plane: {
                origin: [0, 0, 0],
                normal: [0, 0, 1],
                xAxis: [1, 0, 0],
                yAxis: [0, 1, 0],
            },
        });

        try {
            const pointA0 = toolkit.addPoint(sketch, { x: 0, y: 0, fixed: true });
            const pointA1 = toolkit.addPoint(sketch, { x: 10, y: 0, fixed: true });
            const pointB0 = toolkit.addPoint(sketch, { x: 2, y: 1, fixed: true });
            const pointB1 = toolkit.addPoint(sketch, { x: 5, y: 4 });
            const lineA = toolkit.addLineSegment(sketch, { start: pointA0, end: pointA1 });
            const lineB = toolkit.addLineSegment(sketch, { start: pointB0, end: pointB1 });

            toolkit.addConstraint(sketch, { kind: 'perpendicular', entityA: lineA, entityB: lineB });
            toolkit.addConstraint(sketch, { kind: 'equal-length', entityA: lineA, entityB: lineB });

            const result = toolkit.solveSketch(sketch, {
                algorithm: 'lm',
                maxIterations: 64,
                residualTolerance: 1e-8,
                stepTolerance: 1e-10,
            });
            const snapshot = toolkit.getSketchSnapshot(sketch);
            const solvedPointB1 = snapshot.entities.find((entity: any) => entity.id === pointB1 && entity.kind === 'point');

            expect(result.converged).toBe(true);
            expect(result.status).toBe('fully-defined');
            expect(result.maxScaledResidual).toBeLessThanOrEqual(1e-6);

            expect(solvedPointB1).toBeDefined();
            expect(solvedPointB1.x).toBeCloseTo(2, 6);
            expect(solvedPointB1.y).toBeCloseTo(11, 6);
        } finally {
            toolkit.disposeSketch(sketch);
        }
    });

    it('solves a point-line distance relation through the compiled sketch-toolkit target', async () => {
        const toolkit = await createSketchToolkit();
        const sketch = toolkit.createSketch({
            name: 'Point-line distance solve',
            plane: {
                origin: [0, 0, 0],
                normal: [0, 0, 1],
                xAxis: [1, 0, 0],
                yAxis: [0, 1, 0],
            },
        });

        try {
            const lineA0 = toolkit.addPoint(sketch, { x: 0, y: 0, fixed: true });
            const lineA1 = toolkit.addPoint(sketch, { x: 10, y: 0, fixed: true });
            const anchor = toolkit.addPoint(sketch, { x: 3, y: 0, fixed: true });
            const point = toolkit.addPoint(sketch, { x: 3, y: 1 });
            const baseLine = toolkit.addLineSegment(sketch, { start: lineA0, end: lineA1 });
            const guide = toolkit.addLineSegment(sketch, { start: anchor, end: point });

            toolkit.addConstraint(sketch, { kind: 'vertical', entity: guide });
            toolkit.addConstraint(sketch, { kind: 'distance-point-line', point, line: baseLine, value: 5 });

            const result = toolkit.solveSketch(sketch, {
                algorithm: 'lm',
                maxIterations: 64,
                residualTolerance: 1e-8,
                stepTolerance: 1e-10,
            });
            const snapshot = toolkit.getSketchSnapshot(sketch);
            const solvedPoint = snapshot.entities.find((entity: any) => entity.id === point && entity.kind === 'point');

            expect(result.converged).toBe(true);
            expect(result.status).toBe('fully-defined');
            expect(result.maxScaledResidual).toBeLessThanOrEqual(1e-6);

            expect(solvedPoint).toBeDefined();
            expect(solvedPoint.x).toBeCloseTo(3, 6);
            expect(solvedPoint.y).toBeCloseTo(5, 6);
        } finally {
            toolkit.disposeSketch(sketch);
        }
    });

    it('solves a radius-backed point-on-circle relation through the compiled sketch-toolkit target', async () => {
        const toolkit = await createSketchToolkit();
        const sketch = toolkit.createSketch({
            name: 'Point-on-circle solve',
            plane: {
                origin: [0, 0, 0],
                normal: [0, 0, 1],
                xAxis: [1, 0, 0],
                yAxis: [0, 1, 0],
            },
        });

        try {
            const center = toolkit.addPoint(sketch, { x: 0, y: 0, fixed: true });
            const point = toolkit.addPoint(sketch, { x: 6, y: 0 });
            const guideA = toolkit.addPoint(sketch, { x: 0, y: 0, fixed: true });
            const guideB = toolkit.addPoint(sketch, { x: 10, y: 0, fixed: true });
            const circle = toolkit.addCircle(sketch, { center, radius: 2 });
            const guide = toolkit.addLineSegment(sketch, { start: guideA, end: guideB });

            toolkit.addConstraint(sketch, { kind: 'radius', entity: circle, value: 5 });
            toolkit.addConstraint(sketch, { kind: 'point-on-circle', point, entity: circle });
            toolkit.addConstraint(sketch, { kind: 'distance-point-line', point, line: guide, value: 0 });

            const result = toolkit.solveSketch(sketch, {
                algorithm: 'lm',
                maxIterations: 64,
                residualTolerance: 1e-8,
                stepTolerance: 1e-10,
            });
            const snapshot = toolkit.getSketchSnapshot(sketch);
            const solvedPoint = snapshot.entities.find((entity: any) => entity.id === point && entity.kind === 'point');
            const solvedCircle = snapshot.entities.find((entity: any) => entity.id === circle && entity.kind === 'circle');

            expect(result.converged).toBe(true);
            expect(result.status).toBe('fully-defined');
            expect(result.maxScaledResidual).toBeLessThanOrEqual(1e-6);

            expect(solvedCircle).toBeDefined();
            expect(solvedCircle.radius).toBeCloseTo(5, 6);
            expect(solvedPoint).toBeDefined();
            expect(solvedPoint.x).toBeCloseTo(5, 6);
            expect(solvedPoint.y).toBeCloseTo(0, 6);
        } finally {
            toolkit.disposeSketch(sketch);
        }
    });

    it('solves a line-circle tangent relation through the compiled sketch-toolkit target', async () => {
        const toolkit = await createSketchToolkit();
        const sketch = toolkit.createSketch({
            name: 'Line-circle tangent solve',
            plane: {
                origin: [0, 0, 0],
                normal: [0, 0, 1],
                xAxis: [1, 0, 0],
                yAxis: [0, 1, 0],
            },
        });

        try {
            const center = toolkit.addPoint(sketch, { x: 0, y: 0, fixed: true });
            const circle = toolkit.addCircle(sketch, { center, radius: 3 });
            const pointA = toolkit.addPoint(sketch, { x: 5, y: 0, fixed: true });
            const pointB = toolkit.addPoint(sketch, { x: 4, y: 4 });
            const line = toolkit.addLineSegment(sketch, { start: pointA, end: pointB });

            toolkit.addConstraint(sketch, { kind: 'radius', entity: circle, value: 5 });
            toolkit.addConstraint(sketch, { kind: 'tangent', entityA: line, entityB: circle });
            toolkit.addConstraint(sketch, {
                kind: 'distance-point-point',
                pointA,
                pointB,
                value: 4,
            });

            const result = toolkit.solveSketch(sketch, {
                algorithm: 'lm',
                maxIterations: 64,
                residualTolerance: 1e-8,
                stepTolerance: 1e-10,
            });
            const snapshot = toolkit.getSketchSnapshot(sketch);
            const solvedPoint = snapshot.entities.find((entity: any) => entity.id === pointB && entity.kind === 'point');
            const solvedCircle = snapshot.entities.find((entity: any) => entity.id === circle && entity.kind === 'circle');

            expect(result.converged).toBe(true);
            expect(result.status).toBe('fully-defined');
            expect(result.maxScaledResidual).toBeLessThanOrEqual(1e-6);

            expect(solvedPoint).toBeDefined();
            expect(solvedCircle).toBeDefined();
            expect(solvedCircle.radius).toBeCloseTo(5, 6);
            expect(Math.hypot(solvedPoint.x - 5, solvedPoint.y)).toBeCloseTo(4, 6);
            expect(pointLineDistance({ x: 0, y: 0 }, { x: 5, y: 0 }, { x: solvedPoint.x, y: solvedPoint.y })).toBeCloseTo(5, 3);
            expect(solvedPoint.y).toBeGreaterThan(0);
        } finally {
            toolkit.disposeSketch(sketch);
        }
    });

    it('solves an endpoint-aware arc radius change through the compiled sketch-toolkit target', async () => {
        const toolkit = await createSketchToolkit();
        const sketch = toolkit.createSketch({
            name: 'Arc radius solve',
            plane: {
                origin: [0, 0, 0],
                normal: [0, 0, 1],
                xAxis: [1, 0, 0],
                yAxis: [0, 1, 0],
            },
        });

        try {
            const center = toolkit.addPoint(sketch, { x: 0, y: 0, fixed: true });
            const start = toolkit.addPoint(sketch, { x: 3, y: 0 });
            const end = toolkit.addPoint(sketch, { x: 0, y: 3 });
            const xGuideA = toolkit.addPoint(sketch, { x: 0, y: 0, fixed: true });
            const xGuideB = toolkit.addPoint(sketch, { x: 10, y: 0, fixed: true });
            const yGuideA = toolkit.addPoint(sketch, { x: 0, y: 0, fixed: true });
            const yGuideB = toolkit.addPoint(sketch, { x: 0, y: 10, fixed: true });
            const xGuide = toolkit.addLineSegment(sketch, { start: xGuideA, end: xGuideB });
            const yGuide = toolkit.addLineSegment(sketch, { start: yGuideA, end: yGuideB });
            const arc = toolkit.addArc(sketch, {
                center,
                radius: 3,
                startPoint: start,
                endPoint: end,
                startRadians: 0,
                sweepRadians: Math.PI / 2,
            });

            toolkit.addConstraint(sketch, { kind: 'radius', entity: arc, value: 5 });
            toolkit.addConstraint(sketch, { kind: 'distance-point-line', point: start, line: xGuide, value: 0 });
            toolkit.addConstraint(sketch, { kind: 'distance-point-line', point: end, line: yGuide, value: 0 });

            const result = toolkit.solveSketch(sketch, {
                algorithm: 'lm',
                maxIterations: 64,
                residualTolerance: 1e-8,
                stepTolerance: 1e-10,
            });
            const snapshot = toolkit.getSketchSnapshot(sketch);
            const solvedStart = snapshot.entities.find((entity: any) => entity.id === start && entity.kind === 'point');
            const solvedEnd = snapshot.entities.find((entity: any) => entity.id === end && entity.kind === 'point');
            const solvedArc = snapshot.entities.find((entity: any) => entity.id === arc && entity.kind === 'arc');

            expect(result.converged).toBe(true);
            expect(result.status).toBe('fully-defined');
            expect(result.maxScaledResidual).toBeLessThanOrEqual(1e-6);

            expect(solvedStart).toBeDefined();
            expect(solvedStart.x).toBeCloseTo(5, 6);
            expect(solvedStart.y).toBeCloseTo(0, 6);
            expect(solvedEnd).toBeDefined();
            expect(solvedEnd.x).toBeCloseTo(0, 6);
            expect(solvedEnd.y).toBeCloseTo(5, 6);
            expect(solvedArc).toBeDefined();
            expect(solvedArc.radius).toBeCloseTo(5, 6);
        } finally {
            toolkit.disposeSketch(sketch);
        }
    });

    it('solves an explicit angle relation through the compiled sketch-toolkit target', async () => {
        const toolkit = await createSketchToolkit();
        const sketch = toolkit.createSketch({
            name: 'Angle solve',
            plane: {
                origin: [0, 0, 0],
                normal: [0, 0, 1],
                xAxis: [1, 0, 0],
                yAxis: [0, 1, 0],
            },
        });

        try {
            const pointA0 = toolkit.addPoint(sketch, { x: 0, y: 0, fixed: true });
            const pointA1 = toolkit.addPoint(sketch, { x: 10, y: 0, fixed: true });
            const pointB0 = toolkit.addPoint(sketch, { x: 2, y: 1, fixed: true });
            const pointB1 = toolkit.addPoint(sketch, { x: 5, y: 4 });
            const lineA = toolkit.addLineSegment(sketch, { start: pointA0, end: pointA1 });
            const lineB = toolkit.addLineSegment(sketch, { start: pointB0, end: pointB1 });

            toolkit.addConstraint(sketch, { kind: 'angle', lineA, lineB, value: Math.PI / 4 });
            toolkit.addConstraint(sketch, {
                kind: 'distance-point-point',
                pointA: pointB0,
                pointB: pointB1,
                value: 10,
            });

            const result = toolkit.solveSketch(sketch, {
                algorithm: 'lm',
                maxIterations: 64,
                residualTolerance: 1e-8,
                stepTolerance: 1e-10,
            });
            const snapshot = toolkit.getSketchSnapshot(sketch);
            const solvedPoint = snapshot.entities.find((entity: any) => entity.id === pointB1 && entity.kind === 'point');
            const expectedOffset = 10 / Math.sqrt(2);

            expect(result.converged).toBe(true);
            expect(result.status).toBe('fully-defined');
            expect(result.maxScaledResidual).toBeLessThanOrEqual(1e-6);

            expect(solvedPoint).toBeDefined();
            expect(solvedPoint.x).toBeCloseTo(2 + expectedOffset, 6);
            expect(solvedPoint.y).toBeCloseTo(1 + expectedOffset, 6);
        } finally {
            toolkit.disposeSketch(sketch);
        }
    });

    it('reports measured driven dimensions after solve without driving geometry', async () => {
        const toolkit = await createSketchToolkit();
        const sketch = toolkit.createSketch({
            name: 'Driven dimension reporting',
            plane: {
                origin: [0, 0, 0],
                normal: [0, 0, 1],
                xAxis: [1, 0, 0],
                yAxis: [0, 1, 0],
            },
        });

        try {
            const pointA = toolkit.addPoint(sketch, { x: 0, y: 0, fixed: true });
            const pointB = toolkit.addPoint(sketch, { x: 3, y: 4 });
            const center = toolkit.addPoint(sketch, { x: 20, y: 0, fixed: true });
            const circle = toolkit.addCircle(sketch, { center, radius: 2 });

            toolkit.addConstraint(sketch, {
                kind: 'distance-point-point',
                pointA,
                pointB,
                value: 10,
            });
            toolkit.addConstraint(sketch, {
                kind: 'radius',
                entity: circle,
                value: 5,
            });

            const drivenDistanceId = toolkit.addConstraint(sketch, {
                kind: 'distance-point-point',
                pointA,
                pointB,
                drivingState: 'driven',
                name: 'dimension:distance',
            });
            const drivenDiameterId = toolkit.addConstraint(sketch, {
                kind: 'diameter',
                entity: circle,
                drivingState: 'driven',
                name: 'dimension:diameter',
            });

            const result = toolkit.solveSketch(sketch, {
                algorithm: 'lm',
                maxIterations: 64,
                residualTolerance: 1e-8,
                stepTolerance: 1e-10,
            });

            const drivenDistance = result.drivenDimensions.find((entry: any) => entry.constraintId === drivenDistanceId);
            const drivenDiameter = result.drivenDimensions.find((entry: any) => entry.constraintId === drivenDiameterId);

            expect(drivenDistance).toBeDefined();
            expect(drivenDistance?.name).toBe('dimension:distance');
            expect(Number(drivenDistance?.value)).toBeCloseTo(10, 6);
            expect(drivenDistance?.kind).toBe('distance-point-point');
            expect(result.diagnostics.items.some((item: any) => item.code === 'DRIVEN_CONSTRAINTS_SKIPPED')).toBe(true);

            expect(drivenDiameter).toBeDefined();
            expect(drivenDiameter?.name).toBe('dimension:diameter');
            expect(Number(drivenDiameter?.value)).toBeCloseTo(10, 6);
            expect(drivenDiameter?.kind).toBe('diameter');
        } finally {
            toolkit.disposeSketch(sketch);
        }
    });

    it('reports named driving dimensions as driven-conversion candidates when structurally overdefined', async () => {
        const toolkit = await createSketchToolkit();
        const sketch = toolkit.createSketch({
            name: 'Overdefined dimensions',
            plane: {
                origin: [0, 0, 0],
                normal: [0, 0, 1],
                xAxis: [1, 0, 0],
                yAxis: [0, 1, 0],
            },
        });

        try {
            const pointA = toolkit.addPoint(sketch, { x: 0, y: 0, fixed: true });
            const pointB = toolkit.addPoint(sketch, { x: 5, y: 0, fixed: true });

            const constraintId = toolkit.addConstraint(sketch, {
                kind: 'distance-point-point',
                pointA,
                pointB,
                value: 5,
                name: 'dimension:redundant-distance',
            });

            const result = toolkit.solveSketch(sketch, {
                algorithm: 'lm',
                maxIterations: 32,
                residualTolerance: 1e-8,
                stepTolerance: 1e-10,
            });

            const diagnostic = result.diagnostics.items.find((item: any) => item.code === 'DRIVING_DIMENSION_CONVERSION_CANDIDATES');

            expect(result.status).toBe('overdefined');
            expect(diagnostic).toBeDefined();
            expect(diagnostic?.message).toContain('dimension:redundant-distance');
            expect(diagnostic?.constraintIds).toContain(constraintId);
        } finally {
            toolkit.disposeSketch(sketch);
        }
    });
});