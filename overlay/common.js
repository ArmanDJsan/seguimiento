/*
 * Shared helpers for the MetraBola overlay pages.
 *
 * All pages consume the same ranking messages (one JSON object per datagram,
 * see README) and share the parsing rules, the sphere images and the
 * connection handling. Each page is an independent vMix Web Browser Input.
 */
window.OVL = (function () {
    'use strict';

    const TOTAL = 10;
    const params = new URLSearchParams(window.location.search);

    function param(name, fallback) {
        const raw = params.get(name);
        if (raw === null || raw === '') return fallback;
        const parsed = Number.parseFloat(raw);
        return Number.isFinite(parsed) ? parsed : fallback;
    }

    function flag(name, fallback) {
        const raw = params.get(name);
        if (raw === null) return fallback;
        return raw !== '0' && raw !== 'false';
    }

    function clamp(value, min, max) {
        return Math.min(max, Math.max(min, value));
    }

    function parsePuesto(raw) {
        if (raw === undefined || raw === null) return null;
        const value = Number.parseInt(String(raw).trim(), 10);
        if (!Number.isFinite(value) || value < 1 || value > TOTAL) return null;
        return value;
    }

    function parseId(raw, zeroAsTen) {
        if (raw === undefined || raw === null) return null;

        if (typeof raw === 'number') {
            if (!Number.isInteger(raw)) return null;
            if (raw === 0) return zeroAsTen ? TOTAL : null;
            return raw >= 1 && raw <= TOTAL ? raw : null;
        }

        const text = String(raw).trim().toLowerCase();
        if (text === '' || text === '0') return zeroAsTen ? TOTAL : null;
        if (text === 'u' || text === '?' || text === 'unknown' || text === 'none' || text === 'null') return null;

        const value = Number.parseInt(text, 10);
        if (!Number.isFinite(value) || value < 1 || value > TOTAL) return null;
        return value;
    }

    /**
     * Applies the same rules as the TouchDesigner receiver: first valid entry
     * per puesto wins, repeated ids are ignored, missing puestos are not
     * filled. Returns entries sorted by puesto.
     */
    function normalizarRanking(message, zeroAsTen) {
        if (!message || typeof message !== 'object') return [];
        const canicas = Array.isArray(message.canicas) ? message.canicas : [];

        const usedPuestos = new Set();
        const usedIds = new Set();
        const entries = [];

        for (const entry of canicas) {
            if (!entry || typeof entry !== 'object') continue;

            const puesto = parsePuesto(entry.puesto !== undefined ? entry.puesto : entry.posicion);
            if (puesto === null || usedPuestos.has(puesto)) continue;

            const id = parseId(entry.id, zeroAsTen);
            if (id === null) {
                usedPuestos.add(puesto);
                entries.push({ key: `unknown:${puesto}`, id: null, puesto });
                continue;
            }

            if (usedIds.has(id)) continue;
            usedIds.add(id);
            usedPuestos.add(puesto);
            entries.push({ key: `sphere:${id}`, id, puesto });
        }

        entries.sort((a, b) => a.puesto - b.puesto);
        return entries;
    }

    function loadImages() {
        const ids = [];
        for (let id = 1; id <= TOTAL; id += 1) ids.push(String(id));
        ids.push('unknown');

        return Promise.all(ids.map((id) => new Promise((resolve) => {
            const image = new Image();
            image.decoding = 'async';
            image.onload = () => resolve([id, image]);
            image.onerror = () => resolve([id, null]);
            image.src = `assets/${id}.png`;
        }))).then((pairs) => {
            const map = new Map();
            for (const [id, image] of pairs) {
                if (image) map.set(id, image);
            }
            return map;
        });
    }

    function imageFor(images, id) {
        if (id === null) return images.get('unknown') || null;
        return images.get(String(id)) || images.get('unknown') || null;
    }

    /**
     * Subscribes to the ranking stream. `onMessage` receives the parsed JSON,
     * `onStatus` receives 'connected' | 'reconnecting' | 'demo'.
     */
    function connect(onMessage, onStatus, demo) {
        if (demo) {
            onStatus('demo');
            const tick = () => onMessage(randomRanking());
            tick();
            window.setInterval(tick, 1500);
            return null;
        }

        const source = new EventSource('events');

        source.addEventListener('open', () => onStatus('connected'));

        // Visual: los puntos solo avanzan; el historial del centro muestra TODO lo
        // que va llegando, marcando lo que llega tarde (pedido 13-nov-2026).
        // Historial: UNA linea por punto (no crece), ordenado por numero de punto.
        let ultimoPunto = 0;
        const historialPuntos = new Map();   // punto -> llegoTarde

        function pintarHistorial() {
            const etiqueta = document.getElementById('punto-test');
            if (!etiqueta) return;
            const filas = [...historialPuntos.entries()]
                .sort((a, b) => a[0] - b[0])
                .map(([p, d]) => '<div class="' + (d.tarde ? 'tarde' : 'ok') + '">P' + p + ': ' +
                                 (d.seq ? d.seq + ' · ' : '') + d.lista +
                                 (d.tarde ? '  tarde' : '') + '</div>');
            etiqueta.innerHTML = filas.join('');
        }

        source.addEventListener('ranking', (event) => {
            onStatus('connected');
            try {
                const msg = JSON.parse(event.data);
                const punto = Number(msg.punto || 0);
                if (punto > 0) {
                    const tarde = punto < ultimoPunto;
                    // orden publicado: puesto 1..10 -> clase 0..9 (id 10 = bola 0)
                    const puesto = new Array(11).fill('?');
                    const canicas = Array.isArray(msg.canicas) ? msg.canicas : [];
                    for (const c of canicas) {
                        const p = Number(c.puesto || 0);
                        const v = Number(c.id);
                        if (p >= 1 && p <= 10) {
                            puesto[p] = (v === 10) ? '0' : (v > 0 ? String(v) : '?');
                        }
                    }
                    const lista = puesto.slice(1).join(' ');
                    if (!historialPuntos.has(punto) || tarde) {
                        historialPuntos.set(punto, { tarde: tarde, lista: lista, seq: String(msg.seq || '') });
                    }
                    pintarHistorial();
                    if (tarde) {
                        onStatus('ignorado P' + punto + ' (ya va P' + ultimoPunto + ')');
                        return;
                    }
                    ultimoPunto = Math.max(ultimoPunto, punto);
                }
                onMessage(msg);
            } catch (error) {
                onStatus('invalid message');
            }
        });

        source.addEventListener('error', () => onStatus('reconnecting'));
        return source;
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

    /** Keeps a canvas matched to the viewport with a capped device pixel ratio. */
    function fitCanvas(canvas, onChange) {
        const context = canvas.getContext('2d', { alpha: true });
        const state = { width: 1, height: 1, ratio: 1, context };

        function apply() {
            state.width = Math.max(1, window.innerWidth);
            state.height = Math.max(1, window.innerHeight);
            state.ratio = Math.min(window.devicePixelRatio || 1, 2);

            canvas.width = Math.round(state.width * state.ratio);
            canvas.height = Math.round(state.height * state.ratio);
            canvas.style.width = `${state.width}px`;
            canvas.style.height = `${state.height}px`;
            context.setTransform(state.ratio, 0, 0, state.ratio, 0, 0);

            if (onChange) onChange(state);
        }

        apply();
        window.addEventListener('resize', apply);
        return state;
    }

    /** Critically under-damped spring used for smooth positional changes. */
    class Spring {
        constructor(omega, zeta) {
            this.omega = omega;
            this.zeta = zeta;
            this.x = 0;
            this.y = 0;
            this.vx = 0;
            this.vy = 0;
            this.tx = 0;
            this.ty = 0;
        }

        update(dt) {
            const stiffness = this.omega * this.omega;
            const damping = 2 * this.zeta * this.omega;
            const ax = (this.tx - this.x) * stiffness - this.vx * damping;
            const ay = (this.ty - this.y) * stiffness - this.vy * damping;
            this.vx += ax * dt;
            this.vy += ay * dt;
            this.x += this.vx * dt;
            this.y += this.vy * dt;
        }

        snap(tx, ty) {
            this.tx = tx;
            this.ty = ty;
            this.x = tx;
            this.y = ty;
            this.vx = 0;
            this.vy = 0;
        }
    }

    return {
        TOTAL,
        params,
        param,
        flag,
        clamp,
        normalizarRanking,
        loadImages,
        imageFor,
        connect,
        randomRanking,
        fitCanvas,
        Spring,
    };
})();
