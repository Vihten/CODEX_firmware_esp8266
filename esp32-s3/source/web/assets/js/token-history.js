/* Loaded with defer after portal.js; protected same-origin aggregate endpoint. */
(() => {
  'use strict';
  const DAY = 86400000;
  const number = new Intl.NumberFormat('uk-UA');
  const percent = new Intl.NumberFormat('uk-UA', {maximumFractionDigits: 1});
  const dateLabel = new Intl.DateTimeFormat('uk-UA', {day: 'numeric', month: 'long', year: 'numeric', timeZone: 'UTC'});
  const compactDate = new Intl.DateTimeFormat('uk-UA', {weekday: 'short', day: 'numeric', month: 'short', timeZone: 'UTC'});
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
  function cells(model) {
    return week(model, model.today).days.map(day => ({...day, value: day.tokens}));
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
    const status = find('status'), grid = find('grid'), details = find('details');
    const buttons = new Map(), refreshButton = find('refresh');
    let model = null, selected = null, hovered = null, pinned = false;
    let active = null, lastAttempt = -Infinity, denied = false, signature = '', disposed = false, pollTimer = null;
    const stopPolling = () => { if (pollTimer !== null) env.clearTimeout(pollTimer); pollTimer = null; };
    const schedule = () => {
      stopPolling();
      if (!disposed && !denied && !doc.hidden) pollTimer = env.setTimeout(() => { pollTimer = null; void refresh(); }, Math.max(1, 30000 - (env.now() - lastAttempt)));
    };
    function hide() {
      if (selected && buttons.has(selected)) {
        buttons.get(selected).setAttribute('aria-pressed', 'false');
        buttons.get(selected).removeAttribute('aria-describedby');
      }
      selected = null; pinned = false; details.hidden = true;
    }
    const clear = () => {
      hide(); model = null; signature = ''; hovered = null;
      buttons.clear(); grid.replaceChildren(); find('week-total').textContent = '';
    };
    function inspect(id) {
      if (!model?.available || !buttons.has(id)) return;
      if (selected !== id) hide();
      selected = id;
      const button = buttons.get(id), date = parseDate(id);
      const tokens = model.days.has(id) ? model.days.get(id) : null;
      button.setAttribute('aria-pressed', 'true');
      button.setAttribute('aria-describedby', 'token-history-tooltip');
      find('day').textContent = dateLabel.format(date);
      find('day-value').textContent = date > model.today ? 'Цей день ще не настав.' :
        tokens === null ? 'Токени: немає даних' : `Токени: ${number.format(tokens)}`;
      const quota = model.quotas.get(id);
      find('day-quota').textContent = quota === undefined ? 'Використання ліміту: немає денних даних' : `Використання ліміту: ${percent.format(quota)} %`;
      details.hidden = false;
    }
    function dismissIfIdle() {
      if (!pinned && !hovered && ![...buttons.values()].includes(doc.activeElement)) hide();
    }
    function render() {
      if (!model?.available) return;
      const entries = cells(model), summary = week(model, model.today);
      const max = Math.max(0, ...entries.map(day => day.tokens || 0));
      const nextSignature = key(summary.start);
      if (signature !== nextSignature) {
        const focusedId = [...buttons].find(([, button]) => button === doc.activeElement)?.[0];
        hide(); hovered = null; grid.replaceChildren(); buttons.clear();
        for (const day of entries) {
          const wrap = el('div', undefined, 'token-history-day');
          const button = el('button', undefined, 'token-history-cell');
          button.type = 'button'; button.dataset.date = day.id;
          button.addEventListener('pointerenter', () => { hovered = day.id; inspect(day.id); });
          // Keep the tooltip reachable when the pointer moves onto its content.
          button.addEventListener('pointerleave', () => { hovered = null; });
          button.addEventListener('focus', () => inspect(day.id));
          button.addEventListener('blur', dismissIfIdle);
          button.addEventListener('click', () => {
            if (selected === day.id && pinned) { hide(); return; }
            inspect(day.id); pinned = true;
          });
          button.addEventListener('keydown', event => {
            if (event.key === 'Escape') { event.preventDefault(); hovered = null; hide(); return; }
            const steps = {ArrowUp: -1, ArrowDown: 1, ArrowLeft: -1, ArrowRight: 1};
            let target = null;
            if (Object.hasOwn(steps, event.key)) target = key(add(day.date, steps[event.key]));
            else if (event.key === 'Home') target = entries[0].id;
            else if (event.key === 'End') target = entries[6].id;
            if (target && buttons.has(target)) { event.preventDefault(); buttons.get(target).focus(); }
          });
          const label = el('span', compactDate.format(day.date), 'token-history-date');
          label.setAttribute('aria-hidden', 'true');
          wrap.append(button, label); grid.append(wrap); buttons.set(day.id, button);
        }
        signature = nextSignature;
        if (focusedId) buttons.get(buttons.has(focusedId) ? focusedId : key(model.today)).focus();
      }
      for (const day of entries) {
        const button = buttons.get(day.id);
        button.dataset.level = String(level(day.value, max)); button.dataset.unknown = String(day.tokens === null);
        button.dataset.future = String(day.future); button.dataset.today = String(day.id === key(model.today));
        const amount = day.future ? 'Ще не настав' : day.tokens === null ? 'Немає даних' : `${number.format(day.tokens)} токенів`;
        button.setAttribute('aria-label', `${dateLabel.format(day.date)}: ${amount}; використання ліміту: немає денних даних`);
        button.setAttribute('aria-pressed', String(day.id === selected));
      }
      find('range').textContent = `${dateLabel.format(summary.start)} — ${dateLabel.format(add(summary.start, 6))} · усі агенти`;
      find('week-total').textContent = entries.every(day => day.tokens === null) ? 'Токени: немає даних' :
        `${number.format(summary.total)} токенів${summary.complete ? '' : ' за відомі дні'}`;
      if (selected && buttons.has(selected)) inspect(selected);
    }
    doc.addEventListener('keydown', event => { if (event.key === 'Escape') { hovered = null; hide(); } });
    doc.addEventListener('pointerdown', event => { if (!root.contains(event.target)) { hovered = null; hide(); } });
    root.addEventListener('focusout', event => { if (!root.contains(event.relatedTarget)) { hovered = null; hide(); } });
    root.addEventListener('pointerleave', () => { hovered = null; if (!pinned) dismissIfIdle(); });
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
        status.textContent = next.save_failed ? 'Не вдалося зберегти історію на контролері. Показано поточні дані; після перезапуску вони можуть бути втрачені.' : next.saving ? 'Історія зберігається на контролері…' : model.days.size ? 'Поточний тиждень · усі агенти. Пунктиром позначено дні без даних.' : 'Ще немає записаних днів. Невідомі значення не означають нуль токенів.';
      } catch (error) {
        if (!disposed && !doc.hidden) status.textContent = model ? 'Не вдалося оновити історію. Показано останні отримані дані; повторна спроба — через 30 секунд.' : 'Не вдалося завантажити історію. Повторна спроба — через 30 секунд.';
      } finally {
        env.clearTimeout(timeout);
        if (active === request) { active = null; root.setAttribute('aria-busy', 'false'); }
        if (refreshButton) refreshButton.disabled = denied;
        schedule();
      }
    }
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
