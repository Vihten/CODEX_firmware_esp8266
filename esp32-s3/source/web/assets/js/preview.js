
(() => {
  'use strict';
  const start = () => {
    const details = document.querySelector('#matrix-preview');
    const canvas = document.querySelector('#matrix-canvas');
    const state = document.querySelector('#preview-state');
    const normalize = document.querySelector('#preview-normalize');
    if (!details || !canvas || !state || !normalize) return;
    const context = canvas.getContext('2d');
    const label = (text, kind = '') => {
      if (state.textContent !== text) state.textContent = text;
      state.dataset.state = kind;
    };
    if (!context || !('fetch' in window) || !('AbortController' in window)) {
      label('Живий перегляд недоступний у цьому браузері', 'error');
      return;
    }
    canvas.width = 696;
    canvas.height = 466;
    let timer = null;
    let active = null;
    let generation = 0;
    let failures = 0;
    let frame = null;
    let useStream = 'ReadableStream' in window;
    let streamRetryAt = 0;
    const channels = new Uint8Array(256);
    let cachedGain = null;
    let layout = null;
    let drawnRgb = null;
    let intersecting = !('IntersectionObserver' in window);
    let pageActive = true;
    const enabled = () => details.open && !document.hidden && intersecting && pageActive;
    const draw = () => {
      if (!frame) {
        context.fillStyle = '#080a09';
        context.fillRect(0, 0, canvas.width, canvas.height);
        return;
      }
      const resized = !layout || layout.width !== frame.width || layout.height !== frame.height ||
        layout.canvasWidth !== canvas.width || layout.canvasHeight !== canvas.height;
      if (resized) {
        const widthMm = frame.width * 7 - 2;
        const heightMm = frame.height * 12 - 7;
        const scale = Math.min(canvas.width / widthMm, canvas.height / heightMm);
        layout = {width: frame.width, height: frame.height, canvasWidth: canvas.width, canvasHeight: canvas.height,
          scale, left: (canvas.width - widthMm * scale) / 2, top: (canvas.height - heightMm * scale) / 2};
      }
      const {scale, left, top} = layout;
      const gain = normalize.checked && frame.brightness < 255 ? 256 / (frame.brightness + 1) : 1;
      const gainChanged = cachedGain !== gain;
      if (gainChanged) {
        for (let value = 0; value < 256; ++value) {
          const linear = Math.max(0, Math.min(1, value * gain / 255));
          const srgb = linear <= 0.0031308
            ? 12.92 * linear
            : 1.055 * Math.pow(linear, 1 / 2.4) - 0.055;
          channels[value] = Math.max(0, Math.min(255, Math.round(srgb * 255)));
        }
        cachedGain = gain;
      }
      let repaint = resized || gainChanged;
      if (!repaint && scale < 1) {
        for (let offset = 0; offset < frame.rgb.length; ++offset) {
          if (drawnRgb[offset] !== frame.rgb[offset]) { repaint = true; break; }
        }
        if (!repaint) { drawnRgb = frame.rgb; return; }
      }
      if (repaint) {
        context.fillStyle = '#080a09';
        context.fillRect(0, 0, canvas.width, canvas.height);
      }
      for (let y = 0; y < frame.height; ++y) {
        for (let x = 0; x < frame.width; ++x) {
          const offset = (y * frame.width + x) * 3;
          if (!repaint && drawnRgb[offset] === frame.rgb[offset] &&
              drawnRgb[offset + 1] === frame.rgb[offset + 1] && drawnRgb[offset + 2] === frame.rgb[offset + 2]) continue;
          const ledX = left + x * 7 * scale, ledY = top + y * 12 * scale;
          if (!repaint) {
            // Reset fractional edge coverage. At scale >= 1 these integer bounds cannot touch another LED.
            const resetX = Math.floor(ledX), resetY = Math.floor(ledY);
            context.fillStyle = '#080a09';
            context.fillRect(resetX, resetY, Math.ceil(ledX + 5 * scale) - resetX, Math.ceil(ledY + 5 * scale) - resetY);
          }
          context.fillStyle = `rgb(${channels[frame.rgb[offset]]},${channels[frame.rgb[offset + 1]]},${channels[frame.rgb[offset + 2]]})`;
          context.fillRect(ledX, ledY, 5 * scale, 5 * scale);
        }
      }
      // Retain only the displayed RGB data for comparison; stream playback has no queue.
      drawnRgb = frame.rgb;
    };
    const decode = buffer => {
      if (buffer.byteLength < 24 || buffer.byteLength > 49176) throw new Error('length');
      const view = new DataView(buffer);
      if (view.getUint32(0, true) !== 0x3150464c) throw new Error('magic');
      const width = view.getUint16(4, true);
      const height = view.getUint16(6, true);
      if (!width || width > 128 || !height || height > 128 || buffer.byteLength !== 24 + width * height * 3) throw new Error('dimensions');
      if (view.getUint8(17) !== 1) throw new Error('flags');
      for (let i = 18; i < 24; ++i) if (view.getUint8(i) !== 0) throw new Error('reserved');
      return {width, height, sequence: view.getUint32(8, true), capturedMs: view.getUint32(12, true), brightness: view.getUint8(16), rgb: new Uint8Array(buffer, 24)};
    };
    const schedule = delay => {
      if (timer !== null || active || !enabled()) return;
      timer = setTimeout(() => { timer = null; poll(); }, delay);
    };
    const pause = () => {
      ++generation;
      if (timer !== null) { clearTimeout(timer); timer = null; }
      if (active) {
        clearTimeout(active.timeout);
        active.controller.abort();
      }
      canvas.dataset.paused = 'true';
      label(frame ? 'Перегляд призупинено · останній кадр' : 'Перегляд вимкнено');
    };
    const poll = async () => {
      if (active || !enabled()) return;
      if (!useStream && 'ReadableStream' in window && Date.now() >= streamRetryAt) useStream = true;
      const request = {controller: new AbortController(), generation, timeout: null, started: Date.now()};
      active = request;
      request.timeout = setTimeout(() => request.controller.abort(), 2000);
      let delay = null;
      try {
        const response = await fetch(useStream ? '/frames' : '/frame', {cache: 'no-store', signal: request.controller.signal});
        if (response.status === 401) {
          pageActive = false;
          window.location.replace('/login');
          return;
        }
        if (useStream && (!response.ok || !response.body || !response.body.getReader)) {
          if (response.body && response.body.cancel) await response.body.cancel();
          useStream = false;
          streamRetryAt = Date.now() + 30000;
          delay = 0;
          return;
        }
        if (!response.ok) throw new Error(response.status === 503 ? 'maintenance' : 'http');
        if (useStream) {
          const reader = response.body.getReader();
          const pending = new Uint8Array(49176);
          let filled = 0, needed = 24, received = false;
          const armStreamTimeout = () => {
            clearTimeout(request.timeout);
            request.timeout = setTimeout(() => request.controller.abort(), 2000);
          };
          try {
            armStreamTimeout();
            for (;;) {
              const chunk = await reader.read();
              if (request.generation !== generation || request.controller.signal.aborted || !enabled()) return;
              if (chunk.done) {
                if (filled || !received) throw new Error('truncated');
                delay = 100;
                return;
              }
              let cursor = 0, latest = null;
              while (cursor < chunk.value.length) {
                const count = Math.min(needed - filled, chunk.value.length - cursor);
                pending.set(chunk.value.subarray(cursor, cursor + count), filled);
                filled += count;
                cursor += count;
                if (filled === 24 && needed === 24) {
                  const header = new DataView(pending.buffer);
                  const width = header.getUint16(4, true), height = header.getUint16(6, true);
                  if (header.getUint32(0, true) !== 0x3150464c || !width || width > 128 || !height || height > 128)
                    throw new Error('header');
                  needed = 24 + width * height * 3;
                }
                if (filled === needed) {
                  latest = decode(pending.slice(0, needed).buffer);
                  filled = 0;
                  needed = 24;
                }
              }
              if (latest) {
                // Coalesced network reads render only the newest complete frame, never a catch-up queue.
                frame = latest;
                failures = 0;
                received = true;
                armStreamTimeout();
                draw();
                canvas.dataset.paused = 'false';
                canvas.dataset.transport = 'stream';
                canvas.dataset.sequence = String(frame.sequence);
                label('Живий перегляд', 'live');
              }
            }
          } finally {
            request.controller.abort();
            reader.releaseLock();
          }
        }
        const next = decode(await response.arrayBuffer());
        if (request.generation !== generation || request.controller.signal.aborted || !enabled()) return;
        frame = next;
        failures = 0;
        draw();
        canvas.dataset.paused = 'false';
        canvas.dataset.transport = 'poll';
        canvas.dataset.sequence = String(frame.sequence);
        label('Живий перегляд', 'live');
      } catch (error) {
        if (request.generation !== generation || !enabled()) return;
        failures = Math.min(5, failures + 1);
        delay = failures * 1000;
        canvas.dataset.paused = 'true';
        label(error.message === 'maintenance'
          ? 'Контролер зайнятий · повторне підключення'
          : 'Кадр недоступний · повторне підключення', 'error');
      } finally {
        clearTimeout(request.timeout);
        active = null;
        schedule(delay === null ? Math.max(0, 66 - (Date.now() - request.started)) : delay);
      }
    };
    const resume = () => {
      if (!enabled()) { pause(); return; }
      if (!active && timer === null) {
        label(frame ? 'Відновлення перегляду · останній кадр' : 'Очікування кадру');
        schedule(0);
      }
    };
    details.addEventListener('toggle', resume);
    document.addEventListener('visibilitychange', resume);
    normalize.addEventListener('change', draw);
    window.addEventListener('pagehide', () => { pageActive = false; pause(); });
    window.addEventListener('pageshow', () => { pageActive = true; resume(); });
    if ('IntersectionObserver' in window) {
      const observer = new IntersectionObserver(entries => {
        entries.forEach(entry => {
          if (entry.target === canvas) intersecting = entry.isIntersecting;
        });
        resume();
      });
      observer.observe(canvas);
    }
    draw();
    resume();
  };
  if (document.readyState === 'loading') document.addEventListener('DOMContentLoaded', start, {once: true});
  else start();
})();
