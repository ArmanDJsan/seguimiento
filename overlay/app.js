/*
 * MetraBola ranking overlay.
 *
 * Renders the ten numbered spheres (the same images the TouchDesigner project
 * used) ordered by "puesto", animating the swaps between messages.
 *
 * Data format is identical to what the TouchDesigner UDP receiver read: one
 * complete JSON message per datagram:
 *
 *   {"type":"ranking","seq":123,"canicas":[{"id":1,"puesto":2}, ...]}
 *
 *   id      1..10 known sphere; 0 / "u" / "?" / "unknown" / null = unknown
 *   puesto  1..10, repeated puestos and repeated ids are ignored
 *
 * URL parameters (all optional):
 *   demo=1            generate random rankings in the browser (no backend)
 *   debug=1           connection/status HUD
 *   size=0.085        sphere diameter as a fraction of viewport height
 *   gap=1.35          spacing between slot centers, in sphere diameters
 *   y=0.80            vertical center of the row, as a fraction of height
 *   dir=rtl           draw puesto 1 on the right instead of the left
 *   zero=ten          interpret id 0 as the sphere labelled 10 (protocol v1)
 *   unknown-opacity=0.55
 *   initial=empty     do not pre-populate the ten unknown placeholders
 */
(() => {
    'use strict';

    const TOTAL = 10;

    const query = new URLSearchParams(window.location.search);
    const numberParam = (name, fallback) => {
        const raw = query.get(name);
        if (raw === null || raw === '') return fallback;
        const parsed = Number.parseFloat(raw);
        return Number.isFinite(parsed) ? parsed : fallback;
    };
    const flagParam = (name, fallback = false) => {
        const raw = query.get(name);
        if (raw === null) return fallback;
        return raw !== '0' && raw !== 'false';
    };
    const clamp = (value, min, max) => Math.min(max, Math.max(min, value));

    const CONFIG = {
        size: clamp(numberParam('size', 0.085), 0.01, 0.6),
        gap: clamp(numberParam('gap', 1.35), 1.0, 4.0),
        y: clamp(numberParam('y', 0.8), 0, 1),
        direction: query.get('dir') === 'rtl' ? -1 : 1,
        unknownOpacity: clamp(numberParam('unknown-opacity', 0.55), 0, 1),
        zeroAsTen: query.get('zero') === 'ten',
        debug: flagParam('debug'),
        demo: flagParam('demo'),
        initial: query.get('initial') === 'empty' ? 'empty' : 'unknown',
        vertical: query.get('layout') === 'vertical',
        x: clamp(numberParam('x', 0.5), 0, 1),
        bar: query.get('bar') !== '0',
    };

    const canvas = document.getElementById('stage');
    const context = canvas.getContext('2d', { alpha: true });

    let width = 0;
    let height = 0;
    let diameter = 0;
    /** @type {Record<number, {x:number, y:number}>} */
    const slots = {};

    /** @type {Map<string, Sphere>} */
    const entities = new Map();
    /** @type {Map<string, HTMLImageElement>} */
    const images = new Map();

    let lastMessage = null;
    let lastSeq = null;
    let lastMessageAt = 0;
    let connectionState = 'starting';

    class Sphere {
        constructor(key, id, image) {
            this.key = key;
            this.id = id;
            this.image = image;
            this.puesto = 0;
            this.x = 0;
            this.y = 0;
            this.vx = 0;
            this.vy = 0;
            this.tx = 0;
            this.ty = 0;
            this.alpha = 0;
            this.targetAlpha = 1;
            this.scale = 1;
            this.targetScale = 1;
        }
    }

    function parsePuesto(raw) {
        if (raw === undefined || raw === null) return null;
        const value = Number.parseInt(String(raw).trim(), 10);
        if (!Number.isFinite(value)) return null;
        if (value < 1 || value > TOTAL) return null;
        return value;
    }

    function parseId(raw) {
        if (raw === undefined || raw === null) return null;

        if (typeof raw === 'number') {
            if (!Number.isInteger(raw)) return null;
            if (raw === 0 && CONFIG.zeroAsTen) return TOTAL;
            return raw >= 1 && raw <= TOTAL ? raw : null;
        }

        const text = String(raw).trim().toLowerCase();
        if (text === '' || text === '0') {
            return CONFIG.zeroAsTen ? TOTAL : null;
        }
        if (text === 'u' || text === '?' || text === 'unknown' || text === 'none' || text === 'null') {
            return null;
        }

        const value = Number.parseInt(text, 10);
        if (!Number.isFinite(value) || value < 1 || value > TOTAL) return null;
        return value;
    }

    function imageFor(id) {
        if (id === null) return images.get('unknown') ?? null;
        return images.get(String(id)) ?? images.get('unknown') ?? null;
    }

    function createEntity(key, id) {
        const entity = new Sphere(key, id, imageFor(id));
        entities.set(key, entity);
        return entity;
    }

    function assignTarget(entity, puesto, id) {
        const slot = slots[puesto];
        if (!slot) return;

        entity.id = id;
        entity.image = imageFor(id);
        entity.puesto = puesto;
        entity.tx = slot.x;
        entity.ty = slot.y;
        entity.targetAlpha = id === null ? CONFIG.unknownOpacity : 1;
        entity.targetScale = 1;
    }

    function applyRanking(message) {
        if (!message || typeof message !== 'object') return;
        const canicas = Array.isArray(message.canicas) ? message.canicas : [];
        if (canicas.length === 0) return;

        const usedPuestos = new Set();
        const usedIds = new Set();
        const targets = new Map();

        for (const entry of canicas) {
            if (!entry || typeof entry !== 'object') continue;

            const puesto = parsePuesto(entry.puesto !== undefined ? entry.puesto : entry.posicion);
            if (puesto === null || usedPuestos.has(puesto)) continue;

            const id = parseId(entry.id);
            let key;
            if (id === null) {
                key = `unknown:${puesto}`;
            } else {
                if (usedIds.has(id)) continue;
                usedIds.add(id);
                key = `sphere:${id}`;
            }

            usedPuestos.add(puesto);
            targets.set(key, { puesto, id });
        }

        if (targets.size === 0) return;

        for (const [key, target] of targets) {
            let entity = entities.get(key);
            const isNew = !entity;
            if (!entity) entity = createEntity(key, target.id);

            assignTarget(entity, target.puesto, target.id);

            if (isNew) {
                const slot = slots[target.puesto];
                entity.x = slot.x;
                entity.y = slot.y + 48;
                entity.alpha = 0;
                entity.scale = 0.55;
            }
        }

        for (const [key, entity] of entities) {
            if (!targets.has(key)) entity.targetAlpha = 0;
        }

        lastMessage = message;
        if (message.seq !== undefined && message.seq !== null) lastSeq = message.seq;
        lastMessageAt = Date.now();
    }

    function layout() {
        width = Math.max(1, window.innerWidth);
        height = Math.max(1, window.innerHeight);

        const ratio = Math.min(window.devicePixelRatio || 1, 2);
        canvas.width = Math.round(width * ratio);
        canvas.height = Math.round(height * ratio);
        canvas.style.width = `${width}px`;
        canvas.style.height = `${height}px`;
        context.setTransform(ratio, 0, 0, ratio, 0, 0);

        diameter = Math.max(24, height * CONFIG.size);

        if (CONFIG.vertical) {
            // Columna: 1o arriba. El diametro se achica para que entren las diez
            // aunque la ventana sea baja.
            const maxStep = (height * 0.9) / (TOTAL - 1);
            const step = Math.min(diameter * CONFIG.gap, maxStep);
            if (step < diameter * CONFIG.gap) {
                diameter = Math.max(18, step / CONFIG.gap);
            }
            const top = height / 2 - (step * (TOTAL - 1)) / 2;
            const centerX = width * CONFIG.x;
            for (let puesto = 1; puesto <= TOTAL; puesto += 1) {
                const visibleIndex = CONFIG.direction === 1 ? puesto - 1 : TOTAL - puesto;
                slots[puesto] = {
                    x: centerX,
                    y: top + visibleIndex * step,
                };
            }
        } else {
            const step = diameter * CONFIG.gap;
            const rowWidth = step * (TOTAL - 1);
            const centerY = height * CONFIG.y;
            for (let puesto = 1; puesto <= TOTAL; puesto += 1) {
                const visibleIndex = CONFIG.direction === 1 ? puesto - 1 : TOTAL - puesto;
                slots[puesto] = {
                    x: width / 2 - rowWidth / 2 + visibleIndex * step,
                    y: centerY,
                };
            }
        }

        // Re-apply the last ranking so entities follow the new geometry.
        if (lastMessage) {
            applyRanking(lastMessage);
        } else {
            for (const entity of entities.values()) {
                const slot = slots[entity.puesto];
                if (!slot) continue;
                entity.tx = slot.x;
                entity.ty = slot.y;
            }
        }
    }

    function seedInitialState() {
        if (CONFIG.initial === 'empty') return;
        for (let puesto = 1; puesto <= TOTAL; puesto += 1) {
            const key = `unknown:${puesto}`;
            if (entities.has(key)) continue;
            const entity = createEntity(key, null);
            entity.puesto = puesto;
            entity.x = slots[puesto].x;
            entity.y = slots[puesto].y;
            entity.tx = entity.x;
            entity.ty = entity.y;
            entity.alpha = CONFIG.unknownOpacity;
            entity.targetAlpha = CONFIG.unknownOpacity;
        }
    }

    function physics(deltaSeconds) {
        const steps = Math.max(1, Math.ceil(deltaSeconds / (1 / 60)));
        const step = deltaSeconds / steps;

        const omega = 22;
        const zeta = 0.85;
        const stiffness = omega * omega;
        const damping = 2 * zeta * omega;

        for (let index = 0; index < steps; index += 1) {
            for (const entity of entities.values()) {
                const ax = (entity.tx - entity.x) * stiffness - entity.vx * damping;
                const ay = (entity.ty - entity.y) * stiffness - entity.vy * damping;

                entity.vx += ax * step;
                entity.vy += ay * step;
                entity.x += entity.vx * step;
                entity.y += entity.vy * step;

                const alphaStep = Math.min(1, step * 7);
                const scaleStep = Math.min(1, step * 10);
                entity.alpha += (entity.targetAlpha - entity.alpha) * alphaStep;
                entity.scale += (entity.targetScale - entity.scale) * scaleStep;
            }
        }

        for (const [key, entity] of entities) {
            if (entity.targetAlpha === 0 && entity.alpha < 0.01) {
                entities.delete(key);
            }
        }
    }

    function render() {
        context.clearRect(0, 0, width, height);

        const ordered = [...entities.values()].sort((a, b) => {
            if (a.puesto !== b.puesto) return a.puesto - b.puesto;
            return String(a.key).localeCompare(String(b.key));
        });

        for (const entity of ordered) {
            if (entity.alpha <= 0.01) continue;
            if (!entity.image || !entity.image.complete || entity.image.naturalWidth === 0) continue;

            const size = diameter * entity.scale;
            const x = entity.x - size / 2;
            const y = entity.y - size / 2;

            context.save();
            context.globalAlpha = clamp(entity.alpha, 0, 1);
            context.shadowColor = 'rgba(0, 0, 0, 0.35)';
            context.shadowBlur = Math.max(6, diameter * 0.12);
            context.shadowOffsetY = Math.max(2, diameter * 0.035);
            context.drawImage(entity.image, x, y, size, size);
            context.restore();

            // Barra de posiciones: el puesto al lado de cada esfera.
            if (CONFIG.vertical && CONFIG.bar) {
                const chip = Math.max(22, diameter * 0.62);
                const cx = entity.x - diameter * 0.95 - chip / 2;
                const cy = entity.y;
                context.save();
                context.globalAlpha = clamp(entity.alpha, 0, 1);
                context.fillStyle = 'rgba(12, 16, 24, 0.72)';
                context.strokeStyle = 'rgba(255, 255, 255, 0.55)';
                context.lineWidth = Math.max(1.5, chip * 0.06);
                const r = chip * 0.22;
                context.beginPath();
                context.moveTo(cx - chip / 2 + r, cy - chip / 2);
                context.arcTo(cx + chip / 2, cy - chip / 2, cx + chip / 2, cy + chip / 2, r);
                context.arcTo(cx + chip / 2, cy + chip / 2, cx - chip / 2, cy + chip / 2, r);
                context.arcTo(cx - chip / 2, cy + chip / 2, cx - chip / 2, cy - chip / 2, r);
                context.arcTo(cx - chip / 2, cy - chip / 2, cx + chip / 2, cy - chip / 2, r);
                context.closePath();
                context.fill();
                context.stroke();
                context.fillStyle = '#ffffff';
                context.font = '700 ' + Math.round(chip * 0.62) + 'px "Segoe UI", Arial, sans-serif';
                context.textAlign = 'center';
                context.textBaseline = 'middle';
                context.fillText(String(entity.puesto), cx, cy + chip * 0.02);
                context.restore();
            }
        }

        if (CONFIG.debug) drawDebug();
    }

    function drawDebug() {
        const lines = [
            `MetraBola overlay`,
            `conexion: ${connectionState}`,
            `seq: ${lastSeq === null ? '-' : lastSeq}`,
            `esferas: ${entities.size}`,
            `ultimo mensaje: ${lastMessageAt ? `${Math.round((Date.now() - lastMessageAt) / 1000)} s` : 'nunca'}`,
            `viewport: ${width}x${height}`,
        ];

        context.save();
        context.font = '600 16px ui-monospace, SFMono-Regular, Menlo, Consolas, monospace';
        context.textBaseline = 'top';

        const padding = 12;
        const lineHeight = 22;
        const boxWidth = 300;
        const boxHeight = lines.length * lineHeight + padding * 2 - 4;

        context.fillStyle = 'rgba(8, 12, 24, 0.72)';
        context.fillRect(16, 16, boxWidth, boxHeight);

        context.fillStyle = '#7CFFB2';
        lines.forEach((line, index) => {
            context.fillText(line, 16 + padding, 16 + padding + index * lineHeight);
        });

        context.restore();
    }

    let previousFrame = performance.now();
    function frame(now) {
        const deltaSeconds = Math.min(0.05, (now - previousFrame) / 1000);
        previousFrame = now;

        physics(deltaSeconds);
        render();
        window.requestAnimationFrame(frame);
    }

    function randomRanking() {
        const knownCount = Math.floor(Math.random() * (TOTAL + 1));
        const labels = [];
        while (labels.length < knownCount) {
            const candidate = 1 + Math.floor(Math.random() * TOTAL);
            if (!labels.includes(candidate)) labels.push(candidate);
        }

        const positions = [];
        for (let puesto = 1; puesto <= knownCount; puesto += 1) positions.push(puesto);
        for (let index = positions.length - 1; index > 0; index -= 1) {
            const swap = Math.floor(Math.random() * (index + 1));
            [positions[index], positions[swap]] = [positions[swap], positions[index]];
        }

        const placement = new Map();
        for (let puesto = 1; puesto <= TOTAL; puesto += 1) placement.set(puesto, 0);
        labels.forEach((id, index) => placement.set(positions[index], id));

        return {
            type: 'ranking',
            seq: Math.floor(Date.now() / 1000),
            canicas: Array.from(placement, ([puesto, id]) => ({ id, puesto })),
        };
    }

    function connect() {
        if (CONFIG.demo) {
            connectionState = 'demo';
            const tick = () => applyRanking(randomRanking());
            tick();
            window.setInterval(tick, 1500);
            return;
        }

        const source = new EventSource('events');

        source.addEventListener('open', () => {
            connectionState = 'conectado';
        });

        source.addEventListener('ranking', (event) => {
            connectionState = 'conectado';
            try {
                applyRanking(JSON.parse(event.data));
            } catch (error) {
                connectionState = 'mensaje invalido';
            }
        });

        source.addEventListener('error', () => {
            connectionState = 'reconectando';
        });
    }

    function loadImages() {
        const ids = [...Array(TOTAL).keys()].map((index) => String(index + 1));
        ids.push('unknown');

        return Promise.all(ids.map((id) => new Promise((resolve) => {
            const image = new Image();
            image.decoding = 'async';
            image.onload = () => {
                images.set(id, image);
                resolve();
            };
            image.onerror = () => {
                resolve();
            };
            image.src = `assets/${id}.png`;
        })));
    }

    async function boot() {
        await loadImages();
        layout();
        seedInitialState();
        connect();
        window.requestAnimationFrame(frame);
    }

    window.addEventListener('resize', layout);
    boot();
})();
