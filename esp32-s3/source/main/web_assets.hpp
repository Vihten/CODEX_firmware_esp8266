#pragma once

inline constexpr char kPortalCss[] = R"PORTALCSS(
:root{color-scheme:dark;--bg:#111315;--surface:#1a1e20;--text:#edf2ee;--muted:#a6b0aa;--line:#353d38;--accent:#56d39b;--warning:#e8bc72;--danger:#f2a09a;font-family:system-ui,-apple-system,"Segoe UI",sans-serif;font-size:16px;line-height:1.5}
*{box-sizing:border-box;letter-spacing:0}
body{margin:0;background:var(--bg);color:var(--text)}
[hidden]{display:none!important}
::selection{background:var(--accent);color:#102219}
a{color:var(--accent);text-underline-offset:4px;overflow-wrap:anywhere}
button,input,select,textarea,summary,a{ -webkit-tap-highlight-color:transparent}
:focus-visible{outline:2px solid var(--accent);outline-offset:4px}
button,input,select,textarea{font:inherit;accent-color:var(--accent);caret-color:var(--accent)}
button,input:not([type=hidden]),select,textarea{min-height:44px}
button{border:1px solid transparent;border-radius:4px;background:var(--accent);color:#102219;padding:.65rem 1rem;font-weight:650;cursor:pointer;line-height:1.35;white-space:normal;overflow-wrap:anywhere}
.button{display:inline-flex;align-items:center;justify-content:center;min-height:44px;padding:.65rem 1rem;border:1px solid #58645c;border-radius:4px;font:inherit;font-weight:650;line-height:1.35;text-decoration:none;color:var(--text);background:transparent;overflow-wrap:anywhere}
button:disabled{cursor:not-allowed;background:#303833;color:#b2bcb5;border-color:var(--line)}
input:not([type=checkbox]):not([type=radio]):not([type=range]):not([type=color]):not([type=hidden]),select,textarea{display:block;width:100%;min-width:0;padding:.65rem .75rem;border:1px solid #58645c;border-radius:4px;background:var(--surface);color:var(--text)}
input::placeholder,textarea::placeholder{color:var(--muted);opacity:1}
input[type=time]{min-width:0;max-width:100%}
input[type=range]{width:100%;margin:0;cursor:pointer}
input[type=checkbox],input[type=radio]{width:20px;height:20px;min-height:20px;margin:0;flex:none}
input:disabled,select:disabled,textarea:disabled{color:var(--muted);cursor:not-allowed}
input[aria-invalid=true]{border-color:var(--danger)}
label{display:block;color:var(--text);overflow-wrap:anywhere}
label>input:not([type=checkbox]):not([type=radio]),label>select{margin-top:.4rem}
form{margin:0;min-width:0}
form>label,form>.field,form>.form-grid,form>fieldset{margin-bottom:1rem}
form>button{margin-top:.25rem}
h1,h2,h3,p,dl{margin:0}
h1{font-size:1.5rem;line-height:1.25;font-weight:700}
h2{font-size:1.125rem;line-height:1.4;font-weight:650}
h3,.section h3{font-size:1rem;line-height:1.4;font-weight:650}
p+p{margin-top:.75rem}
small,.muted{font-size:.875rem;color:var(--muted)}
code{font-family:ui-monospace,Consolas,monospace;font-size:.875rem;overflow-wrap:anywhere}
.topbar,.tabs,.shell{width:100%;max-width:1104px;margin-inline:auto}
.shell{padding-inline:2rem}
.topbar{display:flex;align-items:center;justify-content:space-between;gap:1.5rem;padding-block:1.75rem 1rem;flex-wrap:wrap}
.brand{min-width:0;display:flex;flex-direction:row;align-items:baseline;gap:.5rem;font-size:1.25rem;font-weight:700}
.brand a{color:var(--text);text-decoration:none}
.device-meta{display:flex;gap:.35rem 1rem;flex-wrap:wrap;color:var(--muted);font-size:.875rem;overflow-wrap:anywhere}
.header-status{display:flex;align-items:center;gap:.5rem 1rem;flex-wrap:wrap;font-size:.875rem;color:var(--muted)}
.header-status [data-live=connection]{max-width:28ch;overflow-wrap:anywhere}
[data-live=connection][data-state=ok]{color:var(--accent)}
[data-live=connection][data-state=error]{color:var(--warning)}
.tabs{display:flex;gap:1.5rem;border-bottom:1px solid var(--line);overflow-x:auto}
.tabs a{display:flex;align-items:center;min-height:48px;padding:.75rem 0;border-bottom:2px solid transparent;text-decoration:none;color:var(--muted);font-weight:600;flex:none}
.tabs a[aria-current=page]{border-bottom-color:var(--accent);color:var(--text)}
.shell{padding-block:0 3rem}
.footer{display:flex;align-items:center;justify-content:space-between;gap:.75rem 1.5rem;flex-wrap:wrap;margin-top:1.75rem;padding-block:1rem;border-top:1px solid var(--line);font-size:.875rem;color:var(--muted)}
.footer a{display:inline-flex;align-items:center;min-height:44px}
.statusbar{padding:1.25rem 0;border-bottom:1px solid var(--line)}
.stats{display:grid;grid-template-columns:repeat(auto-fit,minmax(130px,1fr));gap:1rem 1.5rem}
.stats>div{min-width:0}
.stats dt{color:var(--muted);font-size:.8125rem;margin-bottom:.25rem}
.stats dd{margin:0;font-size:1rem;font-weight:600;font-variant-numeric:tabular-nums;overflow-wrap:anywhere}
.workspace{display:grid;grid-template-columns:minmax(0,2fr) minmax(0,1fr);column-gap:3rem}
.column{min-width:0}
.section{min-width:0;padding:1.75rem 0;border-bottom:1px solid var(--line)}
.section-head{display:flex;align-items:baseline;justify-content:space-between;gap:.75rem 1rem;flex-wrap:wrap;margin-bottom:1.25rem}
.section-head h2{margin:0}
.section>h2,.section>h3{margin-bottom:1rem}
.section>p{margin-block:.75rem}
.section form+p{margin-top:1rem}
.toolbar,.actions,.quickbuttons{display:flex;gap:.5rem;align-items:center;flex-wrap:wrap}
.toolbar{font-size:.875rem}
.actions{margin-top:1rem}
.actions>form{min-width:0}
.quickbuttons{margin-top:.5rem}
.secondary,.quickbuttons button,.filter button{background:transparent;color:var(--text);border:1px solid #58645c}
.quickbuttons button{padding:.5rem .8rem;font-size:.875rem}
.danger{background:transparent;border-color:#925954;color:var(--danger)}
.power{display:grid;grid-template-columns:repeat(3,minmax(0,1fr));gap:.5rem}
.power button{margin:0;min-width:0;padding:.7rem .4rem;background:var(--surface);color:var(--text);border:1px solid #58645c}
.power button[aria-pressed=true],.filter button[aria-pressed=true]{background:var(--accent);border-color:var(--accent);color:#102219}
.form-grid{display:grid;grid-template-columns:repeat(2,minmax(0,1fr));gap:1rem}
.field{min-width:0}
.label-row{display:flex;align-items:baseline;justify-content:space-between;gap:.5rem;margin-bottom:.4rem;flex-wrap:wrap}
.label-row label{margin:0}
output{font-variant-numeric:tabular-nums}
.checkbox-label{display:flex;align-items:center;gap:.65rem;min-height:44px;cursor:pointer;width:fit-content;max-width:100%}
fieldset{padding:0;border:0;min-width:0;margin:0}
legend{padding:0;margin-bottom:.5rem}
.days{display:flex;flex-wrap:wrap;gap:.4rem}
.days label{display:flex;align-items:center;justify-content:center;gap:.4rem;min-height:44px;padding:.4rem .6rem;border:1px solid #58645c;border-radius:4px;cursor:pointer}
.days label:has(input:checked){border-color:var(--accent);color:var(--accent)}
.notice{color:var(--text);background:#1c2520;border:1px solid var(--line);border-radius:4px;padding:.85rem 1rem;margin-block:1rem;overflow-wrap:anywhere}
.notice.warning,.notice[role=alert]{background:#282219;color:#f0cd94;border-color:#75623e}
.notice.danger{background:#2a1e1d;color:var(--danger);border-color:#925954}
details{border-top:1px solid var(--line);margin-top:1rem;padding-top:.5rem}
summary{cursor:pointer;min-height:44px;padding:.6rem 0;color:var(--text);font-weight:600}
details[open]>summary{margin-bottom:.75rem}
.system-list{display:grid;gap:.75rem}
.system-list>div{display:grid;grid-template-columns:minmax(0,1fr) minmax(0,1.4fr);gap:1rem}
.system-list dt{color:var(--muted);font-size:.875rem}
.system-list dd{margin:0;text-align:right;overflow-wrap:anywhere;font-variant-numeric:tabular-nums}
.filter{display:flex;gap:.5rem;flex-wrap:wrap}
.filter button{font-size:.875rem;padding:.5rem .8rem}
.filter-empty{padding:1.5rem 0;color:var(--muted)}
.agent{padding:1.5rem 0;border-bottom:1px solid var(--line);min-width:0}
.agent:first-of-type{padding-top:.5rem}
.agent-heading{display:flex;align-items:center;justify-content:space-between;gap:.75rem;flex-wrap:wrap}
.agent-heading h3{display:flex;align-items:center;gap:.65rem;min-width:0;margin:0;font-size:1.125rem;overflow-wrap:anywhere}
.swatch{display:inline-block;width:18px;height:18px;flex:none;border:1px solid #a6b0aa;border-radius:3px;vertical-align:middle}
.agent-status{display:inline-flex;align-items:center;gap:.4rem;font-size:.875rem;color:var(--muted)}
.agent-status>span:empty{width:6px;height:6px;border-radius:50%;background:var(--muted)}
.agent-status.online{color:var(--accent)}
.agent-status.online>span:empty{background:var(--accent)}
.agent-meta{display:flex;gap:.5rem 1rem;flex-wrap:wrap;margin:.65rem 0 1.25rem;color:var(--muted);font-size:.8125rem}
.agent-metrics{display:grid;grid-template-columns:repeat(2,minmax(0,1fr));gap:1.5rem;margin-bottom:1.25rem}
.agent-metrics dt{color:var(--muted);font-size:.875rem}
.agent-metrics dd{margin:.3rem 0 0;font-size:1.25rem;font-variant-numeric:tabular-nums;overflow-wrap:anywhere}
.agent-metrics dd span{font-size:.875rem;color:var(--muted)}
.agent-metrics [data-agent-activity]{font-size:inherit;color:var(--text)}
meter,progress{display:block;width:100%;max-width:100%;height:6px;border:0;border-radius:2px;accent-color:var(--accent);background:#343d37}
meter{appearance:none}
meter::-webkit-meter-bar{height:6px;background:#343d37;border:0;border-radius:2px}
meter::-webkit-meter-optimum-value{background:var(--accent)}
meter::-moz-meter-bar{background:var(--accent)}
progress::-webkit-progress-bar{background:#343d37}
progress::-webkit-progress-value{background:var(--accent)}
progress::-moz-progress-bar{background:var(--accent)}
.agent-metrics meter{margin-top:.6rem}
.agent-color{display:grid;grid-template-columns:minmax(0,1fr) auto auto;gap:.65rem;align-items:end}
.color-field label{font-size:.875rem;margin-bottom:.5rem}
.color-value{display:flex;align-items:center;gap:.65rem;min-width:0}
.color-value output{font-family:ui-monospace,Consolas,monospace;font-size:.875rem;color:var(--text);overflow-wrap:anywhere}
.agent-color input[type=color]{appearance:none;width:44px;height:44px;flex:none;margin:0;padding:4px;background:var(--surface);border:1px solid #78877c;border-radius:4px;cursor:pointer}
input[type=color]::-webkit-color-swatch-wrapper{padding:0}
input[type=color]::-webkit-color-swatch{border:0;border-radius:2px}
input[type=color]::-moz-color-swatch{border:0;border-radius:2px}
.agent-color button{margin:0;font-size:.875rem;padding:.65rem .85rem}
.agent-summary{display:grid;grid-template-columns:minmax(0,1fr) auto auto;gap:.5rem .65rem;align-items:center;padding:.9rem 0;border-bottom:1px solid var(--line);min-width:0}
.agent-name{display:flex;align-items:center;gap:.65rem;min-width:0}
.agent-name strong{min-width:0;overflow-wrap:anywhere}
.agent-summary [data-agent-name]{overflow-wrap:anywhere;min-width:0}
.agent-summary [data-agent-state]{font-size:.8125rem;color:var(--muted)}
.agent-summary [data-agent-activity]{font-size:.875rem;text-align:right;font-variant-numeric:tabular-nums}
.agent-summary meter{grid-column:1/-1}
table.agent-summary{display:table;width:100%;border-collapse:collapse}
table.agent-summary th,table.agent-summary td{padding:.75rem .4rem;text-align:left;border-bottom:1px solid var(--line);overflow-wrap:anywhere}
table.agent-summary th{color:var(--muted);font-size:.8125rem;font-weight:500}
.pill{display:inline-block;color:var(--muted);font-size:.8125rem}
@media(hover:hover) and (pointer:fine){button:not(:disabled):hover{filter:brightness(1.08)}.secondary:not(:disabled):hover,.quickbuttons button:hover,.filter button:hover,.button:hover{border-color:var(--accent)}a:hover{color:var(--text)}}
@media(max-width:760px){.shell{padding-inline:1.25rem}.workspace{grid-template-columns:minmax(0,1fr);gap:0}.stats{grid-template-columns:repeat(3,minmax(0,1fr))}.section{padding-block:1.5rem}}
@media(max-width:440px){.shell{padding-inline:1rem}.topbar{padding-top:1.25rem;gap:1rem}.header-status{width:100%;justify-content:space-between}.tabs{gap:1.25rem}.stats{grid-template-columns:repeat(2,minmax(0,1fr));gap:1rem}.form-grid{grid-template-columns:minmax(0,1fr)}.agent-color{grid-template-columns:repeat(2,minmax(0,1fr))}.color-field{grid-column:1/-1;margin-bottom:.3rem}.agent-metrics{gap:1rem}.agent-summary{grid-template-columns:minmax(0,1fr) auto}.agent-summary [data-agent-state]{grid-column:1;grid-row:2}.agent-summary [data-agent-activity]{grid-column:2;grid-row:1/3}.actions>button,.actions>form{flex:1 1 auto}.actions>form>button{width:100%}.system-list>div{gap:.5rem}.days{gap:.35rem}.days label{padding:.4rem .5rem}}
)PORTALCSS";

inline constexpr char kPortalScript[] = R"PORTALJS(
(() => {
  'use strict';
  const start = () => {
    const all = (selector, root = document) => Array.from(root.querySelectorAll(selector));
    const one = (selector, root = document) => root.querySelector(selector);
    const dirtyForms = new WeakSet();
    const toggle = one('[data-poll-toggle]');
    const agentSelector = '.agent[data-agent],.agent-summary[data-agent],.agent-summary [data-agent]';
    let timer = null;
    let active = null;
    let failures = 0;
    let baseline = null;
    let selectedFilter = 'all';
    const missing = 'Немає даних';
    const finite = value => typeof value === 'number' && Number.isFinite(value);
    const number = value => finite(value) ? value.toLocaleString('uk-UA') : missing;
    const percent = value => finite(value) && value >= 0 && value <= 100 ? `${Math.round(value)}%` : missing;
    let clock;
    try { clock = new Intl.DateTimeFormat('uk-UA', {timeZone: 'Europe/Kyiv', hour: '2-digit', minute: '2-digit', second: '2-digit'}); }
    catch (_) {
      try { clock = new Intl.DateTimeFormat('uk-UA', {timeZone: 'Europe/Kiev', hour: '2-digit', minute: '2-digit', second: '2-digit'}); }
      catch (_) { clock = {format: date => `${date.toISOString().slice(11, 19)} UTC`}; }
    }
    const controllerClock = epoch => finite(epoch) && epoch > 0 && epoch <= 8640000000000
      ? clock.format(new Date(epoch * 1000)) : 'Очікування NTP';
    const text = (selector, value, root = document) => all(selector, root).forEach(node => {
      const next = String(value);
      if (node.textContent !== next) node.textContent = next;
    });
    const live = (key, value) => text(`[data-live='${key}']`, value);
    const connection = (label, state = '') => all('[data-live=connection]').forEach(node => {
      node.setAttribute('role', 'status');
      node.setAttribute('aria-live', 'polite');
      if (node.textContent !== label) node.textContent = label;
      node.dataset.state = state;
    });
    const automatic = () => !document.hidden && (!toggle || toggle.checked);
    const markDirty = form => { if (form) dirtyForms.add(form); };
    const anyDirty = () => Array.from(document.forms).some(form => dirtyForms.has(form));
    const duration = milliseconds => {
      if (!finite(milliseconds) || milliseconds < 0) return missing;
      const seconds = Math.floor(milliseconds / 1000);
      const days = Math.floor(seconds / 86400);
      const hours = Math.floor(seconds / 3600) % 24;
      const minutes = Math.floor(seconds / 60) % 60;
      return `${days ? `${days} д ` : ''}${hours} год ${minutes} хв`;
    };
    const filterAgents = () => {
      const rows = all('.agent[data-agent]');
      let visible = 0;
      rows.forEach(row => {
        const online = row.dataset.online === undefined ? !!one('.agent-status.online', row) : row.dataset.online === 'true';
        const meter = one('[data-agent-bar]', row);
        const activity = row.dataset.activity === undefined ? (meter ? Number(meter.value) : 0) : Number(row.dataset.activity);
        const activeAgent = online && activity > 0;
        row.hidden = selectedFilter === 'online' ? !online : selectedFilter === 'active' ? !activeAgent : false;
        if (!row.hidden) ++visible;
      });
      all('.filter-empty').forEach(node => { node.hidden = rows.length === 0 || visible !== 0; });
      all('[data-filter]').forEach(button => button.setAttribute('aria-pressed', String(button.dataset.filter === selectedFilter)));
    };
    const updateAgents = incoming => {
      const agents = new Map();
      incoming.forEach(agent => {
        if (agent && typeof agent.id === 'string' && /^[0-9a-f]{8}$/i.test(agent.id)) agents.set(agent.id.toUpperCase(), agent);
      });
      const known = new Set();
      all(agentSelector).forEach(row => {
        const id = (row.dataset.agent || '').toUpperCase();
        if (!/^[0-9A-F]{8}$/.test(id)) return;
        known.add(id);
        const agent = agents.get(id);
        const online = !!agent && agent.online === true;
        const activity = online && finite(agent.activity) ? Math.max(0, Math.min(100, agent.activity)) : 0;
        row.dataset.online = String(online);
        row.dataset.activity = String(activity);
        text('[data-agent-state]', online ? 'На зв’язку' : 'Не на зв’язку', row);
        text('[data-agent-activity]', percent(activity), row);
        text('[data-agent-remaining]', online ? percent(agent.remaining) : missing, row);
        if (agent && typeof agent.name === 'string') text('[data-agent-name]', agent.name, row);
        all('.agent-status', row).forEach(node => {
          node.classList.toggle('online', online);
          if (!node.hasAttribute('data-agent-state') && !one('[data-agent-state]', node)) node.textContent = online ? 'На зв’язку' : 'Не на зв’язку';
        });
        all('[data-agent-bar]', row).forEach(meter => { meter.value = activity; meter.textContent = percent(activity); });
        if (agent && typeof agent.color === 'string' && /^#[0-9a-f]{6}$/i.test(agent.color)) {
          all('[data-agent-color]', row).forEach(swatch => { swatch.style.backgroundColor = agent.color; });
        }
      });
      const unknown = Array.from(agents.keys()).some(id => !known.has(id));
      all('[data-new-agents]').forEach(node => { node.hidden = !unknown; });
      filterAgents();
    };
    const update = status => {
      const stamp = {uptime: status.uptime_ms, frames: status.frames, device: status.device_id, version: status.version};
      let fps = missing;
      if (baseline && stamp.device === baseline.device && stamp.version === baseline.version && stamp.uptime > baseline.uptime && stamp.frames >= baseline.frames) {
        fps = ((stamp.frames - baseline.frames) * 1000 / (stamp.uptime - baseline.uptime)).toLocaleString('uk-UA', {maximumFractionDigits: 1});
      }
      baseline = stamp;
      live('remaining', percent(status.remaining));
      live('activity', percent(status.activity));
      live('agents', number(status.agents_count));
      live('wifi', status.wifi === true ? 'Підключено' : status.wifi === false ? 'Не підключено' : missing);
      live('fps', fps);
      live('uptime', duration(status.uptime_ms));
      live('heap', finite(status.heap) ? `${(status.heap / 1024).toLocaleString('uk-UA', {maximumFractionDigits: 0})} КіБ` : missing);
      live('rssi', finite(status.rssi) ? `${status.rssi} дБм` : missing);
      live('ota', typeof status.ota_label === 'string' ? status.ota_label : typeof status.ota === 'string' ? status.ota : missing);
      live('matrix', typeof status.matrix === 'string' ? status.matrix : missing);
      live('clock', controllerClock(status.time_epoch));
      live('updated', clock.format(new Date()));
      const progress = one('#ota-progress');
      if (progress) {
        const valid = finite(status.ota_total) && status.ota_total > 0 && finite(status.ota_done);
        progress.hidden = !valid;
        if (valid) { progress.max = status.ota_total; progress.value = Math.max(0, Math.min(status.ota_done, status.ota_total)); }
      }
      if (!anyDirty() && ['auto', 'on', 'off'].includes(status.power)) {
        all('.power button[name=mode]').forEach(button => button.setAttribute('aria-pressed', String(button.value === status.power)));
      }
      updateAgents(status.agents);
    };
    const schedule = delay => {
      if (timer !== null) clearTimeout(timer);
      timer = null;
      if (automatic() && !active) timer = setTimeout(() => { timer = null; poll(); }, delay);
    };
    const pause = () => {
      if (timer !== null) clearTimeout(timer);
      timer = null;
      if (active) { active.reason = 'pause'; active.controller.abort(); }
      baseline = null;
      connection(document.hidden ? 'Оновлення призупинено' : 'Автооновлення вимкнено');
    };
    const poll = async (manual = false) => {
      if (document.hidden || (!manual && !automatic()) || active) return;
      if (timer !== null) clearTimeout(timer);
      timer = null;
      const request = {controller: new AbortController(), reason: ''};
      active = request;
      all('[data-refresh]').forEach(button => { button.disabled = true; });
      if (manual || !baseline) connection('Оновлення…');
      const timeout = setTimeout(() => { request.reason = 'timeout'; request.controller.abort(); }, 6000);
      try {
        const response = await fetch('/status', {signal: request.controller.signal, cache: 'no-store', credentials: 'same-origin', redirect: 'error'});
        if (!response.ok) throw new Error('HTTP');
        const status = await response.json();
        if (!status || !finite(status.uptime_ms) || !finite(status.frames) || !Array.isArray(status.agents)) throw new Error('JSON');
        if (request.reason === 'pause' || document.hidden) return;
        update(status);
        failures = 0;
        connection(status.maintenance === true ? 'Контролер зайнятий' : 'Дані оновлено', 'ok');
      } catch (_) {
        if (request.reason !== 'pause' && !document.hidden) {
          ++failures;
          baseline = null;
          live('fps', missing);
          connection(request.reason === 'timeout' ? 'Контролер не відповів вчасно' : 'Не вдалося оновити дані', 'error');
        }
      } finally {
        clearTimeout(timeout);
        active = null;
        all('[data-refresh]').forEach(button => { button.disabled = false; });
        schedule(Math.min(30000, 5000 * Math.pow(2, Math.min(failures, 3))));
      }
    };
    document.addEventListener('input', event => {
      const input = event.target;
      if (!(input instanceof Element)) return;
      if (!input.matches('[data-poll-toggle],#show-password')) markDirty(input.closest('form'));
      if (input.id === 'brightness') {
        const output = one('#brightnessValue');
        if (output) output.textContent = `${Math.round(Number(input.value) * 100 / 255)}%`;
      }
    });
    document.addEventListener('change', event => {
      const input = event.target;
      if (!(input instanceof Element)) return;
      if (input.matches('[data-poll-toggle]')) {
        if (automatic()) { connection('Автооновлення увімкнено'); schedule(5000); }
        else pause();
      } else if (input.id === 'show-password') {
        const password = one('#wifi-password');
        if (password) password.type = input.checked ? 'text' : 'password';
      } else markDirty(input.closest('form'));
    });
    document.addEventListener('click', event => {
      const target = event.target instanceof Element ? event.target.closest('[data-brightness],[data-days],[data-filter],[data-refresh]') : null;
      if (!target || target.disabled) return;
      event.preventDefault();
      if (target.hasAttribute('data-refresh')) { poll(true); return; }
      if (target.hasAttribute('data-filter')) {
        if (['all', 'online', 'active'].includes(target.dataset.filter)) { selectedFilter = target.dataset.filter; filterAgents(); }
        return;
      }
      if (target.hasAttribute('data-brightness')) {
        const value = target.dataset.brightness;
        const range = one('#brightness');
        if (range && ['26', '64', '128', '255'].includes(value)) {
          range.value = value;
          markDirty(range.closest('form'));
          range.dispatchEvent(new Event('input', {bubbles: true}));
        }
      } else if (['62', '65', '127'].includes(target.dataset.days)) {
        const form = target.closest('form') || one('form[action="/schedule"]');
        if (!form) return;
        const mask = Number(target.dataset.days);
        for (let day = 0; day < 7; ++day) {
          const checkbox = one(`input[name='d${day}']`, form);
          if (checkbox) { checkbox.checked = !!(mask & (1 << day)); checkbox.dispatchEvent(new Event('change', {bubbles: true})); }
        }
        markDirty(form);
      }
    });
    document.addEventListener('reset', event => {
      const form = event.target;
      setTimeout(() => {
        dirtyForms.delete(form);
        const range = one('#brightness', form);
        const output = one('#brightnessValue');
        if (range && output) output.textContent = `${Math.round(Number(range.value) * 100 / 255)}%`;
      }, 0);
    });
    document.addEventListener('visibilitychange', () => {
      if (automatic()) { connection('Очікування оновлення'); schedule(5000); }
      else pause();
    });
    all('[data-js][hidden]').forEach(node => { node.hidden = false; });
    all('[data-live=connection]').forEach(node => { node.setAttribute('role', 'status'); node.setAttribute('aria-live', 'polite'); });
    if (!('fetch' in window) || !('AbortController' in window)) {
      connection('Доступний статичний перегляд');
      all('[data-refresh],[data-poll-toggle]').forEach(control => { control.disabled = true; });
      return;
    }
    if (automatic()) { connection('Очікування оновлення'); schedule(5000); }
    else pause();
  };
  if (document.readyState === 'loading') document.addEventListener('DOMContentLoaded', start, {once: true});
  else start();
})();
)PORTALJS";
