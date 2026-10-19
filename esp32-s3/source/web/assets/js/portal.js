
(() => {
  'use strict';
  const start = () => {
    const all = (selector, root = document) => Array.from(root.querySelectorAll(selector));
    const one = (selector, root = document) => root.querySelector(selector);
    const dirtyForms = new WeakSet();
    const memorialForm = one('#memorial-settings');
    const liveNodes = new Map();
    all('[data-live]').forEach(node => {
      const key = node.dataset.live;
      if (!liveNodes.has(key)) liveNodes.set(key, []);
      liveNodes.get(key).push(node);
    });
    const agentStates = new WeakMap();
    const weeklyRoot = one('#weekly-ranking');
    const weeklyRows = weeklyRoot && one('[data-weekly-rows]', weeklyRoot);
    const weeklyEmpty = weeklyRoot && one('[data-weekly-empty]', weeklyRoot);
    const weeklyRecords = new Map();
    const numeric = new Intl.NumberFormat('uk-UA');
    const decimal = new Intl.NumberFormat('uk-UA', {maximumFractionDigits: 1});
    const integer = new Intl.NumberFormat('uk-UA', {maximumFractionDigits: 0});
    const toggle = one('[data-poll-toggle]');
    const agentSelector = '.agent[data-agent],.agent-summary[data-agent],.agent-summary [data-agent]';
    const rosters = [
      {root: one('[data-agent-list]'), template: one('#agent-row-template')},
      {root: one('[data-agent-summary-list]'), template: one('#agent-summary-template')},
    ].filter(roster => roster.root && roster.template?.content?.firstElementChild);
    let timer = null;
    let active = null;
    let failures = 0;
    let baseline = null;
    let pageActive = true;
    let selectedFilter = 'all';
    const missing = 'Немає даних';
    const finite = value => typeof value === 'number' && Number.isFinite(value);
    const number = value => finite(value) ? numeric.format(value) : missing;
    const percent = value => finite(value) && value >= 0 && value <= 100 ? `${Math.round(value)}%` : missing;
    let clock;
    try { clock = new Intl.DateTimeFormat('uk-UA', {timeZone: 'Europe/Kyiv', hour: '2-digit', minute: '2-digit', second: '2-digit'}); }
    catch (_) {
      try { clock = new Intl.DateTimeFormat('uk-UA', {timeZone: 'Europe/Kiev', hour: '2-digit', minute: '2-digit', second: '2-digit'}); }
      catch (_) { clock = {format: date => `${date.toISOString().slice(11, 19)} UTC`}; }
    }
    const controllerClock = epoch => finite(epoch) && epoch > 0 && epoch <= 8640000000000
      ? clock.format(new Date(epoch * 1000)) : 'Очікування NTP';
    const setText = (nodes, value) => nodes.forEach(node => {
      const next = String(value);
      if (node.textContent !== next) node.textContent = next;
    });
    const text = (selector, value, root = document) => setText(all(selector, root), value);
    const live = (key, value) => setText(liveNodes.get(key) || [], value);
    const connection = (label, state = '') => (liveNodes.get('connection') || []).forEach(node => {
      if (node.textContent !== label) node.textContent = label;
      node.dataset.state = state;
    });
    const automatic = () => pageActive && !document.hidden && (!toggle || toggle.checked);
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
      const controls = one('[data-device-controls]');
      if (controls && content && heading && typeof settingsDialog.showModal === 'function' &&
          Object.values(targets).every(target => target.form)) controls.hidden = true;
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
    const updateWeekly = (ranking, agents = []) => {
      const root = weeklyRoot;
      if (!root || !ranking || !Array.isArray(ranking.entries)) return;
      const entries = ranking.entries.filter(row => row && /^[0-9a-f]{8}$/i.test(row.id) &&
        Number.isSafeInteger(row.tokens) && row.tokens >= 0 && /^#[0-9a-f]{6}$/i.test(row.color))
        .slice(0, 12).sort((a, b) => b.tokens - a.tokens || a.id.localeCompare(b.id));
      const total = entries.reduce((sum, row) => sum + row.tokens, 0);
      const retained = new Set();
      const rows = entries.map((row, index) => {
        const id = row.id.toUpperCase();
        retained.add(id);
        let record = weeklyRecords.get(id);
        if (!record) {
          const tr = document.createElement('tr');
          const cells = Array.from({length: 4}, () => tr.appendChild(document.createElement('td')));
          const color = document.createElement('span');
          color.className = 'weekly-agent-color'; color.setAttribute('aria-hidden', 'true');
          record = {tr, cells, color, values: []};
          weeklyRecords.set(id, record);
        }
        const values = [String(index + 1), typeof row.name === 'string' ? row.name : row.id,
          number(row.tokens), `${decimal.format(total ? 100 * row.tokens / total : 0)}%`];
        values.forEach((value, column) => {
          if (record.values[column] === value) return;
          const td = record.cells[column];
          td.textContent = value;
          if (column === 1) td.prepend(record.color);
        });
        record.values = values;
        if (record.rgb !== row.color) { record.color.style.backgroundColor = row.color; record.rgb = row.color; }
        return record.tr;
      });
      for (const id of weeklyRecords.keys()) if (!retained.has(id)) weeklyRecords.delete(id);
      if (weeklyRows.children.length !== rows.length || rows.some((row, index) => weeklyRows.children[index] !== row))
        weeklyRows.replaceChildren(...rows);
      weeklyEmpty.hidden = entries.length > 0;
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
    const sortAgentRows = () => {
      const focused = document.activeElement;
      const selection = focused && typeof focused.selectionStart === 'number'
        ? [focused.selectionStart, focused.selectionEnd, focused.selectionDirection] : null;
      rosters.forEach(({root}) => {
        const rows = all(agentSelector, root);
        const online = row => row.dataset.online === undefined
          ? !!one('.agent-status.online', row) : row.dataset.online === 'true';
        const ordered = [...rows].sort((a, b) => Number(online(b)) - Number(online(a)));
        ordered.forEach((row, index) => {
          if (rows[index] === row) return;
          // moveBefore preserves focus and open controls where supported.
          if (typeof root.moveBefore === 'function') root.moveBefore(row, rows[index]);
          else rows[index].before(row);
          rows.splice(rows.indexOf(row), 1);
          rows.splice(index, 0, row);
        });
      });
      if (focused && focused.isConnected && !focused.disabled && document.activeElement !== focused) {
        focused.focus({preventScroll: true});
        if (selection && typeof focused.setSelectionRange === 'function') focused.setSelectionRange(...selection);
      }
    };
    const reconcileAgents = agents => {
      rosters.forEach(({root, template}) => {
        const rows = new Map(all(agentSelector, root).map(row => [(row.dataset.agent || '').toUpperCase(), row]));
        agents.forEach((agent, id) => {
          if (rows.has(id)) return;
          // Bound retained history; never evict a row containing edits or active controls.
          if (rows.size >= 24) {
            const stale = Array.from(rows.entries()).find(([key, row]) => !agents.has(key) &&
              !row.contains(document.activeElement) && !one('details[open]', row) &&
              !all('form', row).some(form => dirtyForms.has(form)));
            if (!stale) return;
            stale[1].remove(); rows.delete(stale[0]);
          }
          const row = template.content.firstElementChild.cloneNode(true);
          row.dataset.agent = id;
          all('[id],[for]', row).forEach(node => {
            for (const attribute of ['id', 'for']) {
              const value = node.getAttribute(attribute);
              if (value) node.setAttribute(attribute, value.replaceAll('00000000', id));
            }
          });
          all('input[name=client]', row).forEach(input => { input.value = id; input.setAttribute('value', id); });
          text('[data-agent-id]', id, row);
          root.append(row); rows.set(id, row);
        });
        all('[data-agents-empty]', root).forEach(node => { node.hidden = rows.size > 0; });
      });
    };
    const updateAgents = incoming => {
      const agents = new Map();
      incoming.slice(0, 12).forEach(agent => {
        if (agent && typeof agent.id === 'string' && /^[0-9a-f]{8}$/i.test(agent.id) && agent.id !== '00000000')
          agents.set(agent.id.toUpperCase(), agent);
      });
      reconcileAgents(agents);
      let changed = false;
      all(agentSelector).forEach(row => {
        const id = (row.dataset.agent || '').toUpperCase();
        if (!/^[0-9A-F]{8}$/.test(id)) return;
        const agent = agents.get(id);
        const renameForms = all('form.agent-rename', row);
        const signature = JSON.stringify([agent, renameForms.some(form => dirtyForms.has(form))]);
        if (agentStates.get(row) === signature) return;
        agentStates.set(row, signature);
        changed = true;
        const online = !!agent && agent.online === true;
        const activity = online && finite(agent.activity) ? Math.max(0, Math.min(100, agent.activity)) : 0;
        row.dataset.online = String(online);
        row.dataset.activity = String(activity);
        text('[data-agent-state]', online ? 'На зв’язку' : 'Не на зв’язку', row);
        text('[data-agent-activity]', percent(activity), row);
        text('[data-agent-remaining]', online ? percent(agent.remaining) : missing, row);
        if (agent && typeof agent.name === 'string') text('[data-agent-name]', agent.name, row);
        if (agent && typeof agent.custom_color === 'boolean')
          text('[data-agent-color-mode]', agent.custom_color ? 'Власний колір' : 'Автоматичний колір', row);
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
        all('.agent-rename button,.agent-rename input[name=name],.agent-uninstall button,.agent-uninstall input[type=checkbox]', row).forEach(control => { control.disabled = !canManage; });
        text('[data-agent-tools-label]', managing || pending ? 'Керування · команда в черзі' :
          agent && (agent.update_failed || managementState === 3 || managementState === 4) ? 'Керування · потрібна увага' :
          available ? 'Керування · доступне оновлення' : 'Керування агентом', row);
        if (agent && typeof agent.name === 'string') all('form.agent-uninstall', row).forEach(form => {
          form.dataset.confirm = `Видалити агента «${agent.name}» (${id}) з його ПК та вимкнути автозапуск? Ім’я, налаштування та статистика залишаться для повторного встановлення.`;
        });
        renameForms.forEach(form => {
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
          all('form.agent-color', row).forEach(form => {
            const input = one('input[type=color]', form);
            if (!dirtyForms.has(form)) {
              if (input) input.value = agent.color.toLowerCase();
              text('output', agent.color.toUpperCase(), form);
            }
            if (input && typeof agent.name === 'string') input.setAttribute('aria-label', `Колір метеорів агента ${agent.name}`);
          });
        }
      });
      sortAgentRows();
      if (changed) filterAgents();
    };
    const update = status => {
      const stamp = {uptime: status.uptime_ms, frames: status.frames, device: status.device_id, version: status.version};
      let fps = missing;
      if (baseline && stamp.device === baseline.device && stamp.version === baseline.version && stamp.uptime > baseline.uptime && stamp.frames >= baseline.frames) {
        fps = decimal.format((stamp.frames - baseline.frames) * 1000 / (stamp.uptime - baseline.uptime));
      }
      baseline = stamp;
      live('remaining', percent(status.remaining));
      live('activity', percent(status.activity));
      live('agents', number(status.agents_count));
      live('wifi', status.wifi === true ? 'Підключено' : status.wifi === false ? 'Не підключено' : missing);
      live('fps', fps);
      live('uptime', duration(status.uptime_ms));
      live('heap', finite(status.heap) ? `${integer.format(status.heap / 1024)} КіБ` : missing);
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
      if (['auto', 'on', 'off'].includes(status.power)) {
        all('.power button[name=mode]').forEach(button => button.setAttribute('aria-pressed', String(button.value === status.power)));
      }
      const range = one('#brightness');
      if (range && !dirtyForms.has(range.closest('form')) && Number.isInteger(status.brightness) && status.brightness >= 0 && status.brightness <= 255) {
        range.value = String(status.brightness);
        text('#brightnessValue', `${Math.round(status.brightness * 100 / 255)}%`);
      }
      const memorial = status.memorial;
      if (memorial && typeof memorial === 'object' && !Array.isArray(memorial) &&
          ['enabled', 'active', 'manual', 'time_ready', 'saving', 'save_failed'].every(key => typeof memorial[key] === 'boolean') &&
          Number.isInteger(memorial.remaining_seconds) && memorial.remaining_seconds >= 0 && memorial.remaining_seconds <= 60) {
        const checkbox = memorialForm && one('input[name=enabled]', memorialForm);
        if (checkbox && !dirtyForms.has(memorialForm)) checkbox.checked = memorial.enabled;
        text('[data-memorial-state]', memorial.active ?
          `${memorial.manual ? 'Перевірка' : 'Хвилина мовчання'} · ${memorial.remaining_seconds} с` : 'Не відтворюється');
        all('[data-memorial-play]').forEach(button => {
          button.disabled = status.on !== true || memorial.active || status.maintenance === true;
        });
        all('[data-memorial-stop]').forEach(button => {
          button.disabled = !memorial.active || status.maintenance === true;
        });
        text('[data-memorial-save]', memorial.save_failed ? 'Не вдалося зберегти. Спробуйте ще раз.' :
          memorial.saving ? 'Збереження…' : '');
        text('[data-memorial-note]', status.on !== true ? 'Матриця вимкнена. Для відтворення ввімкніть її.' :
          !memorial.time_ready ? 'Очікування часу NTP. Ручне відтворення доступне.' : '');
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
      connection(document.hidden || !pageActive ? 'Оновлення призупинено' : 'Автооновлення вимкнено');
    };
    const poll = async (manual = false) => {
      if (!pageActive || document.hidden || (!manual && !automatic())) return;
      const manualFeedback = request => {
        request.manual = true;
        all('[data-refresh]').forEach(button => { button.disabled = true; });
        connection('Оновлення…');
        text('[data-weekly-state]', 'Оновлення рейтингу…');
      };
      if (active) {
        if (manual && !active.manual && !active.reason) manualFeedback(active);
        return;
      }
      if (timer !== null) clearTimeout(timer);
      timer = null;
      const request = {controller: new AbortController(), reason: '', manual: false};
      active = request;
      if (manual) manualFeedback(request);
      else if (!baseline) connection('Оновлення…');
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
        if (request.manual) all('[data-refresh]').forEach(button => { button.disabled = false; });
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
        const row = form instanceof Element && form.closest('[data-agent]');
        if (row) agentStates.delete(row);
        const range = one('#brightness', form);
        const output = one('#brightnessValue');
        if (range && output) output.textContent = `${Math.round(Number(range.value) * 100 / 255)}%`;
      }, 0);
    });
    document.addEventListener('visibilitychange', () => {
      if (automatic()) { connection('Очікування оновлення'); schedule(5000); }
      else pause();
    });
    window.addEventListener('pagehide', () => { pageActive = false; pause(); });
    window.addEventListener('pageshow', () => {
      pageActive = true;
      if (automatic()) schedule(5000);
    });
    sortAgentRows();
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
