/*
 * Scoreboard: translucent white vertical panel flush against the left edge of
 * the frame. Each row shows the rank number and the sphere currently in that
 * position, with orange separators.
 *
 * Independent render: vMix loads this page as its own Web Browser Input and
 * can hide or remove it without affecting the sphere row or the positions
 * indicator.
 *
 * URL parameters:
 *   demo=1            random rankings in the browser
 *   debug=1           status HUD
 *   x=0               panel left, fraction of viewport width
 *   y=0.0278          panel top, fraction of viewport height
 *   panel-width=0.091  panel width, fraction of viewport width
 *   panel-height=0.944  panel height, fraction of viewport height
 *   numbers-width=0.449  white band behind the ranks, fraction of panel width
 *   rows=10           number of rows to show (10 by default, as the reference)
 *   opacity=0.8       white band opacity
 *   min-interval=0    ignore updates faster than this (ms); 0 = every message
 *   zero=ten          protocol v1: id 0 means sphere 10
 */
(() => {
    'use strict';

    const OVL = window.OVL;
    const { TOTAL, param, flag, clamp, normalizarRanking, loadImages, imageFor, connect, fitCanvas, Spring } = OVL;

    const CONFIG = {
        x: clamp(param('x', 0), -0.5, 1),
        y: clamp(param('y', 0.0278), -0.5, 1),
        panelWidth: clamp(param('panel-width', 0.091), 0.05, 0.9),
        panelHeight: clamp(param('panel-height', 0.944), 0.1, 1.5),
        numbersWidth: clamp(param('numbers-width', 0.449), 0.1, 0.95),
        rows: Math.round(clamp(param('rows', TOTAL), 1, TOTAL)),
        opacity: clamp(param('opacity', 0.8), 0, 1),
        minInterval: Math.max(0, param('min-interval', 0)),
        zeroAsTen: OVL.params.get('zero') === 'ten',
        debug: flag('debug', false),
        demo: flag('demo', false),
        unknownOpacity: clamp(param('unknown-opacity', 0.35), 0, 1),
    };

    const NUMBER_COLOR = '#101010';
    const SEPARATOR_COLOR = 'rgba(242, 160, 61, 0.9)';

    const canvas = document.getElementById('stage');
    const view = fitCanvas(canvas);
    const context = view.context;

    let images = new Map();
    let width = 1;
    let height = 1;
    let panel = { x: 0, y: 0, width: 1, height: 1, rowHeight: 1 };

    let connectionState = 'starting';
    let lastSeq = null;
    let lastMessageAt = 0;

    /**
     * Animated spheres keyed by ranking entry. Row numbers stay fixed; the
     * spheres slide vertically when two puestos swap.
     * @type {Map<string, {key:string,id:number|null,puesto:number,image:HTMLImageElement|null,spring:Spring,alpha:number,targetAlpha:number,scale:number,targetScale:number}>}
     */
    const entities = new Map();

    function rowCenter(puesto) {
        return panel.y + (puesto - 0.5) * panel.rowHeight;
    }

    function sphereX() {
        return panel.x + panel.width * 0.739;
    }

    function createEntity(key, id, puesto) {
        const entity = {
            key,
            id,
            puesto,
            image: imageFor(images, id),
            spring: new Spring(12, 0.82),
            alpha: 0,
            targetAlpha: 1,
            scale: 0.7,
            targetScale: 1,
        };
        entity.spring.snap(sphereX(), rowCenter(puesto));
        entities.set(key, entity);
        return entity;
    }

    function setTarget(entity, puesto, id) {
        entity.id = id;
        entity.puesto = puesto;
        entity.image = imageFor(images, id);
        entity.spring.tx = sphereX();
        entity.spring.ty = rowCenter(puesto);
        entity.targetAlpha = id === null ? CONFIG.unknownOpacity : 1;
        entity.targetScale = 1;
    }

    function computeLayout() {
        width = view.width;
        height = view.height;

        const panelWidth = Math.min(width * CONFIG.panelWidth, width);
        const panelHeight = Math.min(height * CONFIG.panelHeight, height * 1.2);

        panel = {
            x: width * CONFIG.x,
            y: height * CONFIG.y,
            width: panelWidth,
            height: panelHeight,
            rowHeight: panelHeight / CONFIG.rows,
            bandWidth: panelWidth * CONFIG.numbersWidth,
        };

        for (const entity of entities.values()) {
            entity.spring.tx = sphereX();
            entity.spring.ty = rowCenter(entity.puesto);
        }
    }

    function applyRanking(message) {
        const entries = normalizarRanking(message, CONFIG.zeroAsTen)
            .filter((entry) => entry.puesto <= CONFIG.rows)
            .slice(0, CONFIG.rows);
        if (entries.length === 0) return;

        const targets = new Map(entries.map((entry) => [entry.key, entry]));
        const occupied = new Set(entries.map((entry) => entry.puesto));

        for (const entry of entries) {
            let entity = entities.get(entry.key);
            if (!entity) entity = createEntity(entry.key, entry.id, entry.puesto);
            setTarget(entity, entry.puesto, entry.id);
        }

        // A puesto that changed hands fades out. Puestos missing from the
        // message keep their last value, so the protocol gaps do not flicker.
        for (const entity of entities.values()) {
            if (targets.has(entity.key)) continue;
            if (occupied.has(entity.puesto)) entity.targetAlpha = 0;
        }

        if (message.seq !== undefined && message.seq !== null) lastSeq = message.seq;
        lastMessageAt = Date.now();
    }

    let pending = null;
    let scheduled = null;
    let lastApplied = 0;

    function queueRanking(message) {
        if (CONFIG.minInterval <= 0) {
            applyRanking(message);
            return;
        }

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

    function drawPanel() {
        context.save();
        // White band behind the rank numbers only; the sphere area stays
        // fully transparent so the video shows through.
        context.fillStyle = `rgba(255, 255, 255, ${CONFIG.opacity})`;
        context.fillRect(panel.x, panel.y, panel.bandWidth, panel.height);
        context.restore();
    }

    function drawSeparators() {
        context.save();
        context.strokeStyle = SEPARATOR_COLOR;
        context.lineWidth = Math.max(1, panel.rowHeight * 0.03);
        context.beginPath();

        const bandEdge = panel.x + panel.bandWidth;

        // Vertical divider between the white number band and the transparent
        // sphere area.
        context.moveTo(bandEdge, panel.y);
        context.lineTo(bandEdge, panel.y + panel.height);

        // Horizontal separators only over the sphere area; the white band
        // stays clean. No line above the first row or below the last one.
        for (let index = 1; index < CONFIG.rows; index += 1) {
            const y = panel.y + index * panel.rowHeight;
            context.moveTo(bandEdge, y);
            context.lineTo(panel.x + panel.width, y);
        }

        context.stroke();
        context.restore();
    }

    function drawNumbers() {
        const numberSize = Math.max(12, panel.rowHeight * 0.49);
        const numberX = panel.x + panel.bandWidth / 2;

        context.save();
        context.font = `italic 900 ${numberSize}px "Race Sport", "Arial Black", "Segoe UI", Arial, sans-serif`;
        context.textAlign = 'center';
        context.textBaseline = 'middle';
        context.lineJoin = 'round';
        context.lineWidth = Math.max(1, numberSize * 0.03);
        context.strokeStyle = 'rgba(255, 255, 255, 0.95)';
        context.fillStyle = NUMBER_COLOR;

        for (let index = 0; index < CONFIG.rows; index += 1) {
            const label = String(index + 1);
            const centerY = rowCenter(index + 1);
            context.strokeText(label, numberX, centerY);
            context.fillText(label, numberX, centerY);
        }

        context.restore();
    }

    function drawEntities() {
        const size = panel.rowHeight * 0.873;
        const ordered = [...entities.values()]
            .filter((entity) => entity.alpha > 0.01)
            .sort((a, b) => a.spring.y - b.spring.y);

        for (const entity of ordered) {
            if (!entity.image || !entity.image.complete || entity.image.naturalWidth === 0) continue;

            const drawSize = size * entity.scale;
            context.save();
            context.globalAlpha = clamp(entity.alpha, 0, 1);
            context.shadowColor = 'rgba(0, 0, 0, 0.25)';
            context.shadowBlur = Math.max(2, size * 0.06);
            context.shadowOffsetY = Math.max(1, size * 0.02);
            context.drawImage(
                entity.image,
                entity.spring.x - drawSize / 2,
                entity.spring.y - drawSize / 2,
                drawSize,
                drawSize,
            );
            context.restore();
        }
    }

    function drawDebug() {
        const lines = [
            'MetraBola score',
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
        context.fillRect(width - 336, 16, 320, lines.length * lineHeight + padding * 2 - 4);
        context.fillStyle = '#7CFFB2';
        lines.forEach((line, index) => {
            context.fillText(line, width - 336 + padding, 16 + padding + index * lineHeight);
        });
        context.restore();
    }

    function draw() {
        context.clearRect(0, 0, width, height);
        drawPanel();
        drawSeparators();
        drawNumbers();
        drawEntities();
        if (CONFIG.debug) drawDebug();
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

        draw();
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

        // Force layout once more after fonts load so text metrics are stable.
        if (document.fonts && document.fonts.ready) {
            document.fonts.ready.then(() => computeLayout());
        }

        // Draw the first frame with the real number face, not the fallback.
        if (document.fonts && document.fonts.load) {
            await document.fonts.load('italic 900 50px "Race Sport"');
        }

        window.requestAnimationFrame(frame);
    }

    boot();
})();
