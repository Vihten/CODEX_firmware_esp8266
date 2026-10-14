
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
          all('[data-agent-color] rect', row).forEach(swatch => { swatch.setAttribute('fill', agent.color); });
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
      if (input.matches('.agent-color input[type=color]')) {
        const output = one('output', input.closest('form'));
        if (output) output.textContent = input.value.toUpperCase();
      }
    });
    document.addEventListener('submit', event => {
      const form = event.target;
      if (form instanceof Element && form.matches('form[data-confirm]') && !window.confirm(form.dataset.confirm)) event.preventDefault();
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
