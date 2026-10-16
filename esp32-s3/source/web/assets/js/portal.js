
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
    const settingsDialog = one('#device-settings-dialog');
    if (settingsDialog) {
      const content = one('[data-device-settings-body]', settingsDialog);
      const heading = one('#device-settings-title', settingsDialog);
      const targets = {
        brightness: {title: 'Яскравість матриці', form: one('form[action="/brightness"]')},
        power: {title: 'Живлення матриці', form: one('form[action="/power"]')},
        schedule: {title: 'Розклад матриці', form: one('form[action="/schedule"]')},
      };
      let moved = null, placeholder = null, trigger = null;
      settingsDialog.addEventListener('close', () => {
        if (moved && placeholder) placeholder.replaceWith(moved);
        moved = placeholder = null;
        if (trigger && trigger.isConnected) trigger.focus({preventScroll: true});
        trigger = null;
      });
      all('[data-device-settings]').forEach(button => button.addEventListener('click', () => {
        const target = targets[button.dataset.deviceSettings];
        if (!target || !target.form || !content || !heading || settingsDialog.open) return;
        if (typeof settingsDialog.showModal !== 'function') {
          target.form.scrollIntoView({block: 'center'});
          const field = one('input:not([type=hidden]),button', target.form);
          if (field) field.focus();
          return;
        }
        // Move the existing form, preserving native submission, field IDs and dirty state.
        placeholder = document.createComment('device settings form');
        target.form.before(placeholder);
        moved = target.form;
        trigger = button;
        heading.textContent = target.title;
        content.append(moved);
        settingsDialog.showModal();
        const field = one('input:not([type=hidden]),button', moved);
        if (field) field.focus({preventScroll: true});
      }));
      all('[data-device-settings-close]', settingsDialog).forEach(button =>
        button.addEventListener('click', () => settingsDialog.close()));
      settingsDialog.addEventListener('click', event => {
        if (event.target !== settingsDialog) return;
        const bounds = settingsDialog.getBoundingClientRect();
        if (event.clientX < bounds.left || event.clientX > bounds.right ||
            event.clientY < bounds.top || event.clientY > bounds.bottom) settingsDialog.close();
      });
    }
    const anyDirty = () => Array.from(document.forms).some(form => dirtyForms.has(form));
    const updateWeekly = (ranking, agents = []) => {
      const root = one('#weekly-ranking');
      if (!root || !ranking || !Array.isArray(ranking.entries)) return;
      const entries = ranking.entries.filter(row => row && /^[0-9a-f]{8}$/i.test(row.id) &&
        Number.isSafeInteger(row.tokens) && row.tokens >= 0 && /^#[0-9a-f]{6}$/i.test(row.color))
        .slice(0, 12).sort((a, b) => b.tokens - a.tokens || a.id.localeCompare(b.id));
      const total = entries.reduce((sum, row) => sum + row.tokens, 0);
      const rows = entries.map((row, index) => {
        const tr = document.createElement('tr');
        const values = [String(index + 1), typeof row.name === 'string' ? row.name : row.id,
          number(row.tokens), `${(total ? 100 * row.tokens / total : 0).toLocaleString('uk-UA', {maximumFractionDigits: 1})}%`];
        values.forEach((value, column) => {
          const td = document.createElement('td');
          td.textContent = value;
          if (column === 1) {
            const color = document.createElement('span');
            color.className = 'weekly-agent-color'; color.style.backgroundColor = row.color;
            color.setAttribute('aria-hidden', 'true'); td.prepend(color);
          }
          tr.append(td);
        });
        return tr;
      });
      one('[data-weekly-rows]', root).replaceChildren(...rows);
      one('[data-weekly-empty]', root).hidden = entries.length > 0;
      const known = new Set(entries.map(row => row.id.toUpperCase()));
      const pending = Array.isArray(agents) ? agents.filter(agent => agent && agent.online &&
        typeof agent.id === 'string' && !known.has(agent.id.toUpperCase())).slice(0, 12) : [];
      text('[data-weekly-pending]', pending.length ? `Очікується статистика токенів: ${pending.map(agent =>
        typeof agent.name === 'string' ? agent.name : agent.id).join(', ')}` : '', root);
      const week = String(ranking.week);
      text('[data-weekly-period]', /^\d{8}$/.test(week) ?
        `${week.slice(6, 8)}.${week.slice(4, 6)}.${week.slice(0, 4)} · понеділок–неділя, Київ` : 'Очікування синхронізації часу', root);
      text('[data-weekly-state]', ranking.save_failed ? 'Не вдалося зберегти статистику. Повторна спроба автоматично.' :
        ranking.dirty ? 'Є нові дані · збереження кожні 5 хвилин' : 'Збережено на контролері', root);
    };
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
        const updates = agent && agent.update_capable === true;
        const pending = agent && agent.update_pending === true;
        const available = agent && agent.update_available === true;
        text('[data-agent-update-state]', !agent || !agent.agent_version ? 'Очікування агента з підтримкою оновлень' :
          agent.update_failed === true ? 'Оновлення не вдалося; збережено попередню версію' :
          pending ? 'Запит у черзі; очікування підтвердження нової версії' :
          !online ? 'Агент не на зв’язку' : !updates ? 'Канал оновлень недоступний' :
          available ? `Установлена версія ${number(agent.agent_version)}` : 'Установлена актуальна версія', row);
        all('.agent-upgrade button[type=submit]', row).forEach(control => { control.disabled = !online || !updates || !available || pending; });
        const managementState = agent && agent.management_state;
        const managing = managementState === 1;
        const canManage = online && agent && agent.management_capable === true && !managing && !pending;
        text('[data-agent-management-state]', managing ? 'Команда в черзі; очікування відповіді агента' :
          managementState === 2 ? 'Агент підтвердив виконання' : managementState === 3 ? 'Не вдалося виконати команду на ПК' :
          managementState === 4 ? 'Час очікування минув; команду скасовано' :
          !agent || agent.management_capable !== true ? 'Для керування потрібна нова версія агента' : '', row);
        all('.agent-rename button,.agent-rename input[name=name],.agent-uninstall button', row).forEach(control => { control.disabled = !canManage; });
        all('form.agent-rename', row).forEach(form => {
          const input = one('[data-agent-name-input]', form);
          if (input && !dirtyForms.has(form) && agent && typeof agent.name === 'string') input.value = agent.name;
        });
        if (managing) all('.agent-upgrade button[type=submit]', row).forEach(control => { control.disabled = true; });
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
      updateWeekly(status.weekly, status.agents);
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
      if (manual) text('[data-weekly-state]', 'Оновлення рейтингу…');
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
          text('[data-weekly-state]', 'Не вдалося оновити рейтинг. Спробуйте ще раз.');
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
      if (input.matches('[data-agent-name-input]')) input.setCustomValidity?.('');
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
      if (form instanceof Element && form.matches('form.agent-rename')) {
        const input = one('[data-agent-name-input]', form);
        if (input && typeof TextEncoder === 'function' && typeof input.setCustomValidity === 'function') {
          input.setCustomValidity(new TextEncoder().encode(input.value.trim()).length > 48 ? 'Скоротіть ім’я: до 24 українських або 48 латинських літер.' : '');
          if (!input.checkValidity()) { event.preventDefault(); input.reportValidity(); return; }
        }
      }
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
