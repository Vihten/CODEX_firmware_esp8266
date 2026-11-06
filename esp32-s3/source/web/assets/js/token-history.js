/* Loaded with defer after portal.js; protected same-origin aggregate endpoint. */
(() => {
  'use strict';
  const DAY = 86400000;
  const number = new Intl.NumberFormat('uk-UA');
  const percent = new Intl.NumberFormat('uk-UA', {maximumFractionDigits: 1});
  const dateLabel = new Intl.DateTimeFormat('uk-UA', {day: 'numeric', month: 'long', year: 'numeric', timeZone: 'UTC'});
  const compactDate = new Intl.DateTimeFormat('uk-UA', {weekday: 'short', day: 'numeric', month: 'short', timeZone: 'UTC'});
  const monthLabel = new Intl.DateTimeFormat('uk-UA', {month: 'short', timeZone: 'UTC'});
  function parseDate(value) {
    const text = String(value);
    if (!/^\d{8}$/.test(text)) return null;
    const date = new Date(Date.UTC(+text.slice(0, 4), +text.slice(4, 6) - 1, +text.slice(6)));
    return key(date) === text ? date : null;
  }
  function key(date) { return date.toISOString().slice(0, 10).replace(/-/g, ''); }
  function add(date, days) { return new Date(date.getTime() + days * DAY); }
  function monday(date) { return add(date, -((date.getUTCDay() + 6) % 7)); }
  function normalize(payload) {
    if (!payload || typeof payload.available !== 'boolean') throw new Error('Invalid history');
    if (!payload.available) return {available: false, saving: payload.saving === true, save_failed: payload.save_failed === true};
    const today = parseDate(payload.today);
    if (!today || !Array.isArray(payload.days)) throw new Error('Invalid history dates');
    const start = add(today, -365), days = new Map(), quotas = new Map();
    let retainedTotal = 0;
    for (const record of payload.days) {
      const date = record && parseDate(record.date);
      if (!date || !Number.isSafeInteger(record.tokens) || record.tokens < 0) throw new Error('Invalid daily total');
      if (date < start || date > today) continue;
      const id = key(date);
      if (days.has(id)) throw new Error('Duplicate daily total');
      // Nonnegative daily counts make this an upper bound for every weekly
      // and cumulative sum. Reject before an imprecise total reaches the UI.
      const next = retainedTotal + record.tokens;
      if (!Number.isSafeInteger(next)) throw new Error('Unsafe retained token total');
      retainedTotal = next;
      days.set(id, record.tokens);
      // No official account daily-quota source exists. Never trust an
      // incidental percentage field or derive quota from token totals.
    }
    return {available: true, today, start, days, quotas, saving: payload.saving === true, save_failed: payload.save_failed === true};
  }
  function week(model, date) {
    const start = monday(date);
    const days = Array.from({length: 7}, (_, i) => {
      const day = add(start, i), id = key(day);
      return {date: day, id, tokens: model.days.has(id) ? model.days.get(id) : null, quota_used_percent: model.quotas.has(id) ? model.quotas.get(id) : null, future: day > model.today};
    });
    const total = days.reduce((sum, day) => sum + (day.tokens === null ? 0 : day.tokens), 0);
    return {start, days, total, complete: days.every(day => day.tokens !== null)};
  }
  function cells(model, mode) {
    let running = 0;
    const weeks = new Map();
    return Array.from({length: 366}, (_, i) => {
      const date = add(model.start, i), id = key(date), tokens = model.days.has(id) ? model.days.get(id) : null;
      let value = tokens;
      if (mode === 'weekly') {
        const wk = key(monday(date));
        if (!weeks.has(wk)) weeks.set(wk, week(model, date));
        value = tokens === null ? null : weeks.get(wk).total;
      } else if (mode === 'cumulative') {
        if (tokens !== null) running += tokens;
        value = tokens === null ? null : running;
      }
      return {date, id, tokens, value};
    });
  }
  function level(value, max) { return value === null || value === 0 || max === 0 ? 0 : Math.min(4, Math.max(1, Math.ceil(value / max * 4))); }
  function mount(root, env) {
    const doc = root.ownerDocument;
    const find = name => root.querySelector(`[data-history-${name}]`);
    const el = (tag, text, className) => {
      const node = doc.createElement(tag);
      if (text !== undefined) node.textContent = text;
      if (className) node.className = className;
      return node;
    };
    const status = find('status'), grid = find('grid'), months = find('months'), legend = find('legend');
    const details = find('details'), modes = Array.from(root.querySelectorAll('[data-history-mode]'));
    let model = null, selected = null, mode = 'daily', active = null, lastAttempt = -Infinity, denied = false;
    let signature = '', disposed = false, pollTimer = null;
    const refreshButton = find('refresh');
    const stopPolling = () => { if (pollTimer !== null) env.clearTimeout(pollTimer); pollTimer = null; };
    const schedule = () => {
      stopPolling();
      if (!disposed && !denied && !doc.hidden) pollTimer = env.setTimeout(() => { pollTimer = null; void refresh(); }, Math.max(1, 30000 - (env.now() - lastAttempt)));
    };
    const buttons = new Map();
    const clear = () => {
      model = null; signature = ''; selected = null; buttons.clear();
      grid.replaceChildren(); months.replaceChildren(); legend.replaceChildren(); details.hidden = true;
    };
    function inspect(id) {
      if (!model || !model.days || !parseDate(id)) return;
      if (selected && buttons.has(selected)) buttons.get(selected).setAttribute('aria-pressed', 'false');
      selected = id;
      if (buttons.has(id)) buttons.get(id).setAttribute('aria-pressed', 'true');
      const date = parseDate(id), tokens = model.days.has(id) ? model.days.get(id) : null;
      find('day').textContent = dateLabel.format(date);
      find('day-value').textContent = tokens === null ? 'Немає даних за цей день.' : `${number.format(tokens)} токенів · усі агенти`;
      const quota = model.quotas.get(id);
      find('day-quota').textContent = quota === undefined ? 'Ліміт акаунта: немає денних даних' : `Ліміт акаунта: ${percent.format(quota)} %`;
      const summary = week(model, date);
      find('week').textContent = `Тиждень ${dateLabel.format(summary.start)} — ${dateLabel.format(add(summary.start, 6))}`;
      find('week-total').textContent = `${number.format(summary.total)} токенів${summary.complete ? '' : ' за відомі дні'}`;
      find('week-note').textContent = `Токени не визначають відсоток ліміту акаунта.${summary.complete ? '' : ' Сума тижня охоплює лише відомі дні.'}`;
      const rows = summary.days.map(day => {
        const row = el('tr'); row.dataset.selected = String(day.id === selected);
        const heading = el('th', compactDate.format(day.date)); heading.scope = 'row';
        const amount = el('td', day.tokens === null ? (day.future ? 'Ще не настав' : 'Немає даних') : number.format(day.tokens));
        const quotaCell = el('td', day.quota_used_percent === null ? '—' : `${percent.format(day.quota_used_percent)} %`);
        row.append(heading, amount, quotaCell); return row;
      });
      find('week-rows').replaceChildren(...rows); details.hidden = false;
    }
    function render() {
      if (!model?.available) return;
      const entries = cells(model, mode), max = Math.max(0, ...entries.map(day => day.value || 0));
      const first = monday(model.start);
      const columns = Math.floor((monday(model.today) - first) / (7 * DAY)) + 1;
      // Keep native day buttons in date order; refreshes preserve keyboard focus.
      if (signature !== key(model.today)) {
        const focusedId = [...buttons].find(([, button]) => button === doc.activeElement)?.[0];
        grid.replaceChildren(); months.replaceChildren(); buttons.clear();
        const offset = (model.start.getUTCDay() + 6) % 7;
        for (let i = 0; i < offset; ++i) { const space = el('span'); space.setAttribute('aria-hidden', 'true'); grid.append(space); }
        let previousMonth = -1;
        for (const day of entries) {
          if (day.date.getUTCMonth() !== previousMonth) {
            const label = el('span', monthLabel.format(day.date));
            const column = Math.floor((day.date - first) / (7 * DAY)) + 1;
            // A tiny partial month at the left edge must not collide with next label.
            if (previousMonth !== -1 || day.date.getUTCDate() < 24) {
              label.style.gridColumn = `${column} / span ${Math.min(3, columns - column + 1)}`;
              months.append(label);
            }
            previousMonth = day.date.getUTCMonth();
          }
          const button = el('button', undefined, 'token-history-cell'); button.type = 'button'; button.dataset.date = day.id;
          button.addEventListener('click', () => inspect(day.id));
          button.addEventListener('focus', () => inspect(day.id));
          button.addEventListener('keydown', event => {
            const steps = {ArrowUp: -1, ArrowDown: 1, ArrowLeft: -7, ArrowRight: 7};
            let target = null;
            if (Object.hasOwn(steps, event.key)) target = key(add(day.date, steps[event.key]));
            else if (event.key === 'Home') target = entries[0].id;
            else if (event.key === 'End') target = entries[365].id;
            if (target && buttons.has(target)) { event.preventDefault(); buttons.get(target).focus(); }
          });
          buttons.set(day.id, button); grid.append(button);
        }
        signature = key(model.today);
        if (focusedId) buttons.get(buttons.has(focusedId) ? focusedId : key(model.start)).focus();
      }
      for (const day of entries) {
        const button = buttons.get(day.id);
        button.dataset.level = String(level(day.value, max)); button.dataset.unknown = String(day.tokens === null);
        let text = `${dateLabel.format(day.date)}: ${day.tokens === null ? 'Немає даних' : `${number.format(day.tokens)} токенів`}`;
        if (day.tokens !== null && mode !== 'daily') text += mode === 'weekly' ? `; сума відомих днів тижня ${number.format(day.value)}` : `; накопичено за відомі дні ${number.format(day.value)}`;
        button.setAttribute('aria-label', text); button.title = text;
        button.setAttribute('aria-pressed', String(day.id === selected));
      }
      const items = [];
      function swatch(text, value, unknown = false) {
        const item = el('span', undefined, 'token-history-legend-item');
        const sample = el('span', undefined, 'token-history-swatch'); sample.dataset.level = String(value); sample.dataset.unknown = String(unknown); sample.setAttribute('aria-hidden', 'true');
        item.append(sample, el('span', text)); items.push(item);
      }
      swatch('Немає даних', 0, true); swatch('0', 0);
      if (max > 0) {
        for (let i = 1; i <= 4; ++i) {
          const lower = Math.floor((i - 1) * max / 4) + 1, upper = Math.floor(i * max / 4);
          if (lower <= upper) swatch(lower === upper ? number.format(upper) : `${number.format(lower)}–${number.format(upper)}`, i);
        }
      }
      legend.replaceChildren(...items);
      find('mode-note').textContent = mode === 'daily' ? 'Колір: токени за день; лінійна шкала до максимуму видимих 366 днів. Оберіть день для деталей.' : mode === 'weekly' ? 'Колір: сума відомих днів календарного тижня. Кожна кнопка відкриває окремий день; невідомі дні залишаються порожніми.' : 'Колір: накопичена сума лише відомих днів у видимому періоді, не за весь час. Пропуски не вважаються нулями.';
      find('range').textContent = `${dateLabel.format(model.start)} — ${dateLabel.format(model.today)} · усі агенти`;
      inspect(selected && buttons.has(selected) ? selected : key(model.today));
    }
    async function refresh() {
      if (disposed || denied || doc.hidden || active) return;
      if (env.now() - lastAttempt < 30000) { schedule(); return; }
      stopPolling();
      lastAttempt = env.now();
      const request = new env.AbortController(); active = request;
      const timeout = env.setTimeout(() => request.abort(), 6000);
      root.setAttribute('aria-busy', 'true');
      if (refreshButton) refreshButton.disabled = true;
      try {
        const response = await env.fetch('/token-history', {credentials: 'same-origin', cache: 'no-store', signal: request.signal, headers: {Accept: 'application/json'}});
        if (request.signal.aborted || disposed) return;
        if (response.status === 401) { denied = true; stopPolling(); clear(); status.textContent = 'Сесію завершено. Увійдіть знову, щоб переглянути історію.'; return; }
        if (!response.ok) throw new Error('History request failed');
        const next = normalize(await response.json());
        if (request.signal.aborted || disposed) return;
        if (!next.available) { clear(); status.textContent = 'Історія поки недоступна. Дані не підмінюються нулями.'; return; }
        model = next; render();
        status.textContent = next.save_failed ? 'Не вдалося зберегти історію на контролері. Показано поточні дані; після перезапуску вони можуть бути втрачені.' : next.saving ? 'Історія зберігається на контролері…' : model.days.size ? 'Історія всіх агентів. Пунктиром позначено дні без даних.' : 'Ще немає записаних днів. Невідомі значення не означають нуль токенів.';
      } catch (error) {
        if (!disposed && !doc.hidden) status.textContent = model ? 'Не вдалося оновити історію. Показано останні отримані дані; повторна спроба — через 30 секунд.' : 'Не вдалося завантажити історію. Повторна спроба — через 30 секунд.';
      } finally {
        env.clearTimeout(timeout);
        if (active === request) { active = null; root.setAttribute('aria-busy', 'false'); }
        if (refreshButton) refreshButton.disabled = denied;
        schedule();
      }
    }
    modes.forEach(button => button.addEventListener('click', () => {
      mode = button.dataset.historyMode;
      modes.forEach(item => item.setAttribute('aria-pressed', String(item === button)));
      render();
    }));
    if (refreshButton) refreshButton.addEventListener('click', () => {
      if (!active && !denied && env.now() - lastAttempt < 30000) status.textContent = 'Історія оновлюється раз на 30 секунд. Наступне оновлення заплановано.';
      void refresh();
    });
    const onVisibility = () => {
      if (doc.hidden) { stopPolling(); active?.abort(); }
      else void refresh();
    };
    const onPageHide = () => { disposed = true; stopPolling(); active?.abort(); };
    const onPageShow = () => { disposed = false; void refresh(); };
    doc.addEventListener('visibilitychange', onVisibility);
    env.addEventListener('focus', () => { void refresh(); });
    env.addEventListener('pagehide', onPageHide); env.addEventListener('pageshow', onPageShow);
    void refresh();
    return {refresh, inspect};
  }
  if (typeof module !== 'undefined' && module.exports) module.exports = {parseDate, key, add, monday, normalize, week, cells, level, mount};
  if (typeof document !== 'undefined') {
    const root = document.querySelector('#token-history');
    if (root) mount(root, {fetch: window.fetch.bind(window), AbortController: window.AbortController, now: () => performance.now(), setTimeout: window.setTimeout.bind(window), clearTimeout: window.clearTimeout.bind(window), addEventListener: window.addEventListener.bind(window)});
  }
})();
