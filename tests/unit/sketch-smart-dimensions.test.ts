import {
    buildSmartSegmentAngleInfo,
    detectAllDimensionTypes,
    detectDimensionType,
    smartDimensions,
} from '../../src/sketch-api';

describe('smart sketch dimensions', () => {
    it('defaults circles to diameter and arcs to radius while keeping both options', () => {
        const circle = { type: 'circle', cx: 5, cy: 7, radius: 3 } as const;
        const arc = { type: 'arc', cx: 2, cy: 4, radius: 6, sweepAngle: Math.PI / 2 } as const;

        expect(detectDimensionType(circle)?.dimType).toBe('diameter');
        expect(detectDimensionType(arc)?.dimType).toBe('radius');

        expect(detectAllDimensionTypes(circle).map((entry) => entry.dimType)).toEqual(['diameter', 'radius']);
        expect(detectAllDimensionTypes(arc).map((entry) => entry.dimType)).toEqual(['radius', 'diameter']);
    });

    it('prefers angle with stable endpoint rays for non-parallel segments', () => {
        const base = {
            type: 'segment',
            x1: -2,
            y1: 0,
            x2: 8,
            y2: 0,
            p1: { x: -2, y: 0 },
            p2: { x: 8, y: 0 },
        } as const;
        const branch = {
            type: 'segment',
            x1: 0,
            y1: 0,
            x2: 3,
            y2: 4,
            p1: { x: 0, y: 0 },
            p2: { x: 3, y: 4 },
        } as const;

        const options = detectAllDimensionTypes(base, branch);
        expect(options.map((entry) => entry.label)).toEqual(['Angle', 'Angle (Inverted)', 'Distance']);
        expect(options[0]?.angleEndpointAKey).toBe('p2');
        expect(options[0]?.angleEndpointBKey).toBe('p2');
        expect(options[1]?.angleEndpointAKey).toBe('p2');
        expect(options[1]?.angleEndpointBKey).toBe('p2');
        expect(options[0]?.angleSweep).toBeCloseTo(Math.atan2(4, 3), 6);
        expect(options[1]?.angleSweep).toBeCloseTo(-Math.atan2(4, 3), 6);
    });

    it('falls back to distance for parallel segments', () => {
        const segmentA = {
            type: 'segment',
            x1: 0,
            y1: 0,
            x2: 10,
            y2: 0,
            p1: { x: 0, y: 0 },
            p2: { x: 10, y: 0 },
        } as const;
        const segmentB = {
            type: 'segment',
            x1: 0,
            y1: 5,
            x2: 10,
            y2: 5,
            p1: { x: 0, y: 5 },
            p2: { x: 10, y: 5 },
        } as const;

        const options = detectAllDimensionTypes(segmentA, segmentB);
        expect(options).toHaveLength(1);
        expect(options[0]?.dimType).toBe('distance');
        expect(options[0]?.x1).toBeCloseTo(5, 6);
        expect(options[0]?.y2).toBeCloseTo(5, 6);
    });

    it('exports the smart dimension utility bundle', () => {
        expect(smartDimensions.detectAllDimensionTypes).toBe(detectAllDimensionTypes);
        expect(typeof smartDimensions.buildSmartSegmentAngleInfo).toBe('function');
    });

    it('preserves explicit endpoint keys when rebuilding smart segment angles', () => {
        const segA = {
            type: 'segment',
            x1: -5,
            y1: 0,
            x2: 5,
            y2: 0,
            p1: { x: -5, y: 0 },
            p2: { x: 5, y: 0 },
        } as const;
        const segB = {
            type: 'segment',
            x1: 0,
            y1: -5,
            x2: 0,
            y2: 5,
            p1: { x: 0, y: -5 },
            p2: { x: 0, y: 5 },
        } as const;

        const info = buildSmartSegmentAngleInfo(segA, segB, { endpointAKey: 'p2', endpointBKey: 'p2' });
        expect(info.angleEndpointAKey).toBe('p2');
        expect(info.angleEndpointBKey).toBe('p2');
        expect(info.sweep).toBeCloseTo(Math.PI / 2, 6);
    });
});