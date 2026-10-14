#pragma once

inline constexpr char kPreviewCss[] = R"PREVIEWCSS(
.preview-section>details{border-top:0;margin-top:0;padding-top:0}
.preview-toolbar{display:flex;align-items:center;justify-content:space-between;gap:.5rem 1.25rem;flex-wrap:wrap;margin-bottom:.75rem;font-size:.875rem}
#preview-state{color:var(--muted);overflow-wrap:anywhere}
#preview-state[data-state=live]{color:var(--accent)}
#preview-state[data-state=error]{color:var(--warning)}
#matrix-canvas{display:block;width:100%;height:auto;aspect-ratio:348/233;background:#080a09;image-rendering:pixelated}
.device-visual{position:relative;width:100%;max-width:956px;margin-inline:auto;aspect-ratio:956/570}
.device-visual>img{display:block;width:100%;height:auto;aspect-ratio:956/570;pointer-events:none}
.device-display{position:absolute;left:17.468619%;top:16.140351%;width:64.853556%;height:77.54386%;display:flex;align-items:center;justify-content:center;background:#080a09;overflow:hidden}
)PREVIEWCSS";

inline constexpr char kPreviewScript[] = R"PREVIEWJS(
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
    let intersecting = !('IntersectionObserver' in window);
    let pageActive = true;
    const enabled = () => details.open && !document.hidden && intersecting && pageActive;
    const draw = () => {
      context.fillStyle = '#080a09';
      context.fillRect(0, 0, canvas.width, canvas.height);
      if (!frame) return;
      const widthMm = frame.width * 7 - 2;
      const heightMm = frame.height * 12 - 7;
      const scale = Math.min(canvas.width / widthMm, canvas.height / heightMm);
      const left = (canvas.width - widthMm * scale) / 2;
      const top = (canvas.height - heightMm * scale) / 2;
      const gain = normalize.checked && frame.brightness < 255 ? 256 / (frame.brightness + 1) : 1;
      const channel = value => {
        const linear = Math.max(0, Math.min(1, value * gain / 255));
        const srgb = linear <= 0.0031308
          ? 12.92 * linear
          : 1.055 * Math.pow(linear, 1 / 2.4) - 0.055;
        return Math.max(0, Math.min(255, Math.round(srgb * 255)));
      };
      for (let y = 0; y < frame.height; ++y) {
        for (let x = 0; x < frame.width; ++x) {
          const offset = (y * frame.width + x) * 3;
          context.fillStyle = `rgb(${channel(frame.rgb[offset])},${channel(frame.rgb[offset + 1])},${channel(frame.rgb[offset + 2])})`;
          context.fillRect(left + x * 7 * scale, top + y * 12 * scale, 5 * scale, 5 * scale);
        }
      }
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
      const request = {controller: new AbortController(), generation, timeout: null, started: Date.now()};
      active = request;
      request.timeout = setTimeout(() => request.controller.abort(), 2000);
      let delay = null;
      try {
        const response = await fetch('/frame', {cache: 'no-store', signal: request.controller.signal});
        if (!response.ok) throw new Error(response.status === 503 ? 'maintenance' : 'http');
        const next = decode(await response.arrayBuffer());
        if (request.generation !== generation || request.controller.signal.aborted || !enabled()) return;
        frame = next;
        failures = 0;
        draw();
        canvas.dataset.paused = 'false';
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
        schedule(delay === null ? Math.max(0, 100 - (Date.now() - request.started)) : delay);
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
)PREVIEWJS";
