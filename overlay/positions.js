/*
 * Bottom indicator: the first four positions with progressive flames
 * (rank 1 the strongest, rank 4 the smallest) plus 1ST..4TH labels and the
 * red baseline.
 *
 * Independent render: vMix loads this page as its own Web Browser Input and
 * can hide or remove it without affecting the sphere row or the scoreboard.
 *
 * URL parameters:
 *   demo=1          random rankings in the browser
 *   debug=1         status HUD
 *   size=0.16       sphere diameter as a fraction of viewport height
 *   y=0.85          row center as a fraction of viewport height
 *   spread=0.36     horizontal space used by the four slots, fraction of width
 *   min-interval=1500  ignore updates faster than this (ms); 0 = every message
 *   flames=0        disable the flames
 *   labels=0        disable the 1ST..4TH labels
 *   line=1          draw the red baseline (off by default)
 *   zero=ten        protocol v1: id 0 means sphere 10
 */
(() => {
    'use strict';

    const OVL = window.OVL;
    const { param, flag, clamp, normalizarRanking, loadImages, imageFor, connect, fitCanvas, Spring } = OVL;

    const CONFIG = {
        size: clamp(param('size', 0.16), 0.02, 0.6),
        y: clamp(param('y', 0.85), 0, 1),
        spread: clamp(param('spread', 0.36), 0.2, 1),
        minInterval: Math.max(0, param('min-interval', 1500)),
        flames: flag('flames', false),
        labels: flag('labels', true),
        line: flag('line', false),
        zeroAsTen: OVL.params.get('zero') === 'ten',
        debug: flag('debug', false),
        demo: flag('demo', false),
        unknownOpacity: clamp(param('unknown-opacity', 0.55), 0, 1),
    };

    const VISIBLE = 4;
    const LABELS = ['1ST', '2ND', '3RD', '4TH'];
    const FLAME_INTENSITY = [1, 0.72, 0.48, 0];
    const LABEL_SCALE = [1, 1, 1, 1];
    const SPHERE_SCALE = [1, 1, 1, 1];
    const SUFFIX_SCALE = 50 / 120;
    const SUFFIX_COLOR = '#FF0000';
    const LABEL_STROKE_NUMERAL = 4 / 120;
    const LABEL_STROKE_SUFFIX = 2 / 50;

    const canvas = document.getElementById('stage');
    const view = fitCanvas(canvas);
    const context = view.context;

    let images = new Map();
    let width = 1;
    let height = 1;
    let diameter = 1;
    let slots = [];

    let connectionState = 'starting';
    let lastSeq = null;
    let lastMessageAt = 0;

    /** @type {Map<string, {key:string,id:number|null,puesto:number,image:HTMLImageElement|null,spring:Spring,alpha:number,targetAlpha:number,scale:number,targetScale:number}>} */
    const entities = new Map();

    function computeLayout() {
        width = view.width;
        height = view.height;
        diameter = Math.max(24, height * CONFIG.size);

        slots = [];
        for (let rank = 1; rank <= VISIBLE; rank += 1) {
            const t = (rank - 1) - (VISIBLE - 1) / 2;
            slots.push({
                x: width / 2 + t * (width * CONFIG.spread) / (VISIBLE - 1),
                y: height * CONFIG.y,
            });
        }

        for (const entity of entities.values()) {
            const slot = slots[entity.puesto - 1];
            if (slot) {
                entity.spring.tx = slot.x;
                entity.spring.ty = slot.y;
            }
        }
    }

    function createEntity(key, id, puesto) {
        const slot = slots[puesto - 1] || { x: width / 2, y: height * CONFIG.y };
        const entity = {
            key,
            id,
            puesto,
            image: imageFor(images, id),
            spring: new Spring(12, 0.82),
            alpha: 0,
            targetAlpha: 1,
            scale: 0.6,
            targetScale: 1,
        };
        entity.spring.snap(slot.x, slot.y);
        entities.set(key, entity);
        return entity;
    }

    function setTarget(entity, puesto, id) {
        const slot = slots[puesto - 1];
        if (!slot) return;

        entity.id = id;
        entity.puesto = puesto;
        entity.image = imageFor(images, id);
        entity.spring.tx = slot.x;
        entity.spring.ty = slot.y;
        entity.targetAlpha = id === null ? CONFIG.unknownOpacity : 1;
        entity.targetScale = SPHERE_SCALE[puesto - 1];
    }

    function applyRanking(message) {
        const entries = normalizarRanking(message, CONFIG.zeroAsTen)
            .filter((entry) => entry.puesto <= VISIBLE)
            .slice(0, VISIBLE);

        if (entries.length === 0) return;

        const targets = new Map(entries.map((entry) => [entry.key, entry]));

        for (const entry of entries) {
            let entity = entities.get(entry.key);
            const isNew = !entity;
            if (!entity) entity = createEntity(entry.key, entry.id, entry.puesto);
            setTarget(entity, entry.puesto, entry.id);
            if (isNew) {
                entity.alpha = 0;
                entity.scale = 0.6;
            }
        }

        for (const [key, entity] of entities) {
            if (!targets.has(key)) entity.targetAlpha = 0;
        }

        if (message.seq !== undefined && message.seq !== null) lastSeq = message.seq;
        lastMessageAt = Date.now();
    }

    // Throttling: keep only the newest message and apply it at most once per
    // minInterval, so a high-rate feed does not make the flames jitter.
    let pending = null;
    let scheduled = null;
    let lastApplied = 0;

    function queueRanking(message) {
        pending = message;
        if (scheduled !== null) return;

        const wait = Math.max(0, CONFIG.minInterval - (Date.now() - lastApplied));
        scheduled = window.setTimeout(() => {
            scheduled = null;
            if (pending === null) return;
            const next = pending;
            pending = null;
            lastApplied = Date.now();
            applyRanking(next);
        }, wait);
    }

    function drawFlames(centerX, centerY, radius, intensity, time, seed) {
        if (!CONFIG.flames || intensity <= 0.01) return;

        context.save();
        context.globalCompositeOperation = 'lighter';

        // Warm glow under the sphere, visible between the tongues.
        const glow = context.createRadialGradient(centerX, centerY, radius * 0.3, centerX, centerY, radius * (2.1 + intensity * 0.6));
        glow.addColorStop(0, `rgba(255, 168, 56, ${0.42 * intensity})`);
        glow.addColorStop(0.55, `rgba(255, 96, 20, ${0.18 * intensity})`);
        glow.addColorStop(1, 'rgba(255, 60, 0, 0)');
        context.fillStyle = glow;
        context.beginPath();
        context.arc(centerX, centerY, radius * (2.1 + intensity * 0.6), 0, Math.PI * 2);
        context.fill();

        // The fan covers the upper half of the sphere: tongues rise and only
        // the outermost ones lean sideways.
        const tongues = 4 + Math.round(intensity * 5);
        const fanStart = -Math.PI + 0.6;
        const fanWidth = Math.PI - 1.2;

        for (let index = 0; index < tongues; index += 1) {
            const phase = time * (1.7 + index * 0.29) + seed + index * 1.7;
            const across = (index + 0.5) / tongues;
            const angle = fanStart + across * fanWidth + Math.sin(phase) * 0.16;

            const length = radius * (0.95 + 1.05 * intensity) * (0.7 + 0.3 * Math.sin(phase * 1.6));
            const width = radius * (0.34 + 0.26 * intensity) * (0.75 + 0.25 * Math.sin(phase * 2.2));

            const baseX = centerX + Math.cos(angle) * radius * 0.7;
            const baseY = centerY + Math.sin(angle) * radius * 0.7;
            const tipAngle = angle * 0.55 - (Math.PI / 2) * 0.45;
            const tipX = centerX + Math.cos(tipAngle) * (radius * 0.7 + length);
            const tipY = centerY + Math.sin(tipAngle) * (radius * 0.7 + length) - length * 0.15;

            const perpX = Math.cos(angle + Math.PI / 2) * width;
            const perpY = Math.sin(angle + Math.PI / 2) * width;

            const gradient = context.createRadialGradient(baseX, baseY, radius * 0.06, tipX, tipY, length * 0.95);
            gradient.addColorStop(0, 'rgba(255, 244, 168, 0.98)');
            gradient.addColorStop(0.32, 'rgba(255, 164, 52, 0.88)');
            gradient.addColorStop(0.66, 'rgba(240, 82, 24, 0.5)');
            gradient.addColorStop(1, 'rgba(176, 26, 8, 0)');

            context.fillStyle = gradient;
            context.beginPath();
            context.moveTo(baseX - perpX, baseY - perpY);
            context.quadraticCurveTo(baseX + perpX * 0.8, baseY + perpY * 0.8, tipX, tipY);
            context.quadraticCurveTo(baseX - perpX * 1.6, baseY - perpY * 1.6, baseX - perpX, baseY - perpY);
            context.fill();
        }

        context.restore();
    }

    function drawLabel(text, x, y, size, factor) {
        if (!CONFIG.labels) return;
        const fontSize = size * factor;
        const suffixSize = fontSize * SUFFIX_SCALE;
        const family = '"Race Sport", "Arial Black", "Segoe UI", Arial, sans-serif';
        const numeral = text.slice(0, 1);
        const suffix = text.slice(1);

        context.save();
        context.textAlign = 'left';
        context.textBaseline = 'alphabetic';
        context.lineJoin = 'round';

        context.font = `italic 900 ${fontSize}px ${family}`;
        const numeralWidth = context.measureText(numeral).width;
        context.font = `italic 900 ${suffixSize}px ${family}`;
        const suffixWidth = context.measureText(suffix).width;
        let cursor = x - (numeralWidth + suffixWidth) / 2;

        context.font = `italic 900 ${fontSize}px ${family}`;
        context.lineWidth = Math.max(1, fontSize * LABEL_STROKE_NUMERAL);
        context.strokeStyle = 'rgba(255, 255, 255, 0.95)';
        context.strokeText(numeral, cursor, y);
        context.fillStyle = '#0d0d0d';
        context.fillText(numeral, cursor, y);

        cursor += numeralWidth;
        context.font = `italic 900 ${suffixSize}px ${family}`;
        context.lineWidth = Math.max(1, suffixSize * LABEL_STROKE_SUFFIX);
        context.strokeText(suffix, cursor, y);
        context.fillStyle = SUFFIX_COLOR;
        context.fillText(suffix, cursor, y);
        context.restore();
    }

    function draw(time) {
        context.clearRect(0, 0, width, height);

        const ordered = [...entities.values()].sort((a, b) => a.puesto - b.puesto);

        for (const entity of ordered) {
            if (entity.alpha <= 0.01) continue;
            const intensity = FLAME_INTENSITY[entity.puesto - 1] || 0;
            drawFlames(entity.spring.x, entity.spring.y, (diameter * entity.scale) / 2, intensity, time, (entity.id || 0) * 1.7);
        }

        for (const entity of ordered) {
            if (entity.alpha <= 0.01) continue;
            if (!entity.image || !entity.image.complete || entity.image.naturalWidth === 0) continue;

            const size = diameter * entity.scale;
            context.save();
            context.globalAlpha = clamp(entity.alpha, 0, 1);
            context.shadowColor = 'rgba(0, 0, 0, 0.4)';
            context.shadowBlur = Math.max(6, diameter * 0.1);
            context.shadowOffsetY = Math.max(2, diameter * 0.03);
            context.drawImage(entity.image, entity.spring.x - size / 2, entity.spring.y - size / 2, size, size);
            context.restore();
        }

        const labelSize = Math.max(18, height * 0.0556);
        const labelY = Math.min(height - labelSize * 0.15, height * CONFIG.y + diameter * 0.62);
        for (let rank = 1; rank <= VISIBLE; rank += 1) {
            const slot = slots[rank - 1];
            drawLabel(LABELS[rank - 1], slot.x, labelY, labelSize, LABEL_SCALE[rank - 1]);
        }

        // Red baseline: disabled by default, re-enable with ?line=1.
        if (CONFIG.line) {
            context.save();
            context.fillStyle = '#e01b2d';
            const lineHeight = Math.max(2, height * 0.004);
            context.fillRect(0, height - lineHeight - height * 0.012, width, lineHeight);
            context.restore();
        }

        if (CONFIG.debug) drawDebug();
    }

    function drawDebug() {
        const lines = [
            'MetraBola posiciones',
            `conexion: ${connectionState}`,
            `seq: ${lastSeq === null ? '-' : lastSeq}`,
            `ultimo mensaje: ${lastMessageAt ? `${Math.round((Date.now() - lastMessageAt) / 1000)} s` : 'nunca'}`,
            `viewport: ${width}x${height}`,
        ];

        context.save();
        context.font = '600 16px ui-monospace, SFMono-Regular, Menlo, Consolas, monospace';
        context.textBaseline = 'top';

        const padding = 12;
        const lineHeight = 22;
        context.fillStyle = 'rgba(8, 12, 24, 0.72)';
        context.fillRect(16, 16, 320, lines.length * lineHeight + padding * 2 - 4);

        context.fillStyle = '#7CFFB2';
        lines.forEach((line, index) => {
            context.fillText(line, 16 + padding, 16 + padding + index * lineHeight);
        });
        context.restore();
    }

    let previous = performance.now();
    function frame(now) {
        const dt = Math.min(0.05, (now - previous) / 1000);
        previous = now;

        const steps = Math.max(1, Math.ceil(dt / (1 / 60)));
        const step = dt / steps;
        for (let index = 0; index < steps; index += 1) {
            for (const entity of entities.values()) {
                entity.spring.update(step);
                entity.alpha += (entity.targetAlpha - entity.alpha) * Math.min(1, step * 7);
                entity.scale += (entity.targetScale - entity.scale) * Math.min(1, step * 9);
            }
        }

        for (const [key, entity] of entities) {
            if (entity.targetAlpha === 0 && entity.alpha < 0.01) entities.delete(key);
        }

        draw(now / 1000);
        window.requestAnimationFrame(frame);
    }

    async function boot() {
        images = await loadImages();
        computeLayout();
        window.addEventListener('resize', computeLayout);

        connect(
            (message) => queueRanking(message),
            (status) => { connectionState = status; },
            CONFIG.demo,
        );

        window.requestAnimationFrame(frame);
    }

    boot();
})();
