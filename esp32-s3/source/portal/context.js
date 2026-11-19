    // @portal-strings
    const all = (selector, root = document) => Array.from(root.querySelectorAll(selector));
    const one = (selector, root = document) => root.querySelector(selector);
    const dirtyForms = new WeakSet();
    const formRevisions = new WeakMap();
    const submitting = new WeakSet();
    const currentIntents = new WeakMap();
    const queuedIntents = new WeakMap();
    let authEpoch = 0;
    const commandTimers = new Set();
    const commandControllers = new Set();
    const awaitingState = new Map();
    const commandRoutes = new Set(['/save', '/power', '/brightness', '/external-light', '/schedule',
      '/sky-colors', '/quota-rotation', '/agent-color', '/restart', '/update', '/memorial', '/black-hole', '/air-alert', '/buzzer-test', '/buzzer-volume',
      '/agent-upgrade', '/agent-rename', '/agent-uninstall']);
    let pollSequence = 0, refreshAfterActive = false;
    const memorialForm = one('#memorial-settings');
    const airAlertForm = one('#air-alert-settings');
    const overviewAir = one('[data-overview-air-state]');
    const rotationInputs = all('#quota-rotation-minutes,form[action="/sky-colors"] input[name=minutes]');
    const liveNodes = new Map();
    all('[data-live]').forEach(node => {
      const key = node.dataset.live;
      if (!liveNodes.has(key)) liveNodes.set(key, []);
      liveNodes.get(key).push(node);
    });
    const agentStates = new WeakMap();
    const rankings = ['weekly', 'lifetime'].map(kind => {
      const root = one(`#${kind}-ranking`);
      return {kind, root, rows: root && one(`[data-${kind}-rows]`, root),
        empty: root && one(`[data-${kind}-empty]`, root),
        notes: root ? all(`[data-${kind}-state],[data-${kind}-pending]`, root) : [], records: new Map()};
    });
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
    const missing = portalStrings.missing;
    const finite = value => typeof value === 'number' && Number.isFinite(value);
    // Decorative housing paths mirror measured output, never the editable setpoints.
    const lightWashes = all('.device-light-wash');
    const setLightOutputs = light => {
      const enabled = light?.ready === true && light.allowed === true;
      lightWashes.forEach(node => {
        const channel = node.dataset.lightChannel;
        const output = enabled && (channel === 'first' || channel === 'second') ? light[`${channel}_output`] : null;
        const next = finite(output) && output >= 0 && output <= 100 ? String(output / 100) : '0';
        if (node.style.getPropertyValue('--light-output') !== next) node.style.setProperty('--light-output', next);
      });
    };
    setLightOutputs(null);
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
    const setOverviewAir = (level, label, note = '') => {
      if (!overviewAir) return;
      if (overviewAir.dataset.overviewAirState !== level) overviewAir.dataset.overviewAirState = level;
      text('[data-overview-air-label]', label, overviewAir);
      text('[data-overview-air-note]', note, overviewAir);
    };
    const clearCityThreat = () => {
      text('[data-city-threat-state]', 'Немає актуальних даних');
      text('[data-city-threat-source]', '');
    };
    const live = (key, value) => setText(liveNodes.get(key) || [], value);
    const connection = (label, state = '') => (liveNodes.get('connection') || []).forEach(node => {
      if (window.portalAuthExpired === true) label = '';
      if (node.textContent !== label) node.textContent = label;
      node.dataset.state = state;
    });
    const authenticated = () => window.portalAuthExpired !== true;
    const automatic = () => authenticated() && pageActive && !document.hidden &&
      (!toggle || !toggle.isConnected || toggle.closest('[hidden]') ||
        (typeof toggle.getClientRects === 'function' && toggle.getClientRects().length === 0) ||
        window.getComputedStyle?.(toggle).display === 'none' ||
        window.getComputedStyle?.(toggle).visibility === 'hidden' || toggle.checked);
    const showSessionBanner = () => {
      const banner = one('#session-banner');
      if (!banner) return;
      banner.hidden = false;
      banner.setAttribute('role', 'status');
      banner.setAttribute('aria-live', 'polite');
      let message = one('[data-session-message]', banner);
      if (!message) {
        message = document.createElement('span');
        message.dataset.sessionMessage = '';
        const link = one('a', banner) || document.createElement('a');
        link.href = '/login'; link.textContent = 'Увійти';
        banner.replaceChildren(message, ' ', link);
      }
      message.textContent = 'Сеанс завершено. Увійдіть знову.';
    };
    const stopForAuth = () => {
      window.portalAuthExpired = true;
      ++authEpoch;
      if (timer !== null) clearTimeout(timer);
      timer = null;
      refreshAfterActive = false;
      for (const id of commandTimers) clearTimeout(id);
      commandTimers.clear();
      for (const controller of commandControllers) controller.abort();
      if (active) { active.reason = 'auth'; active.controller.abort(); }
      awaitingState.clear();
      baseline = null;
      setLightOutputs(null);
      clearCityThreat();
      setOverviewAir('unknown', 'Немає актуальних даних', 'Сеанс завершено');
      live('output-limit', '');
      connection('Сеанс завершено. Увійдіть знову.', 'error');
      const dialog = one('#device-settings-dialog');
      const modalWasOpen = dialog?.open === true;
      if (modalWasOpen && typeof dialog.close === 'function') {
        try { dialog.close(); } catch (_) { /* The session banner remains available. */ }
      }
      showSessionBanner();
      if (modalWasOpen) one('#session-banner a')?.focus({preventScroll: true});
      all('[data-command-feedback]').forEach(node => { node.textContent = ''; node.hidden = true; });
    };
    const expireSession = () => {
      if (!authenticated()) return;
      window.portalAuthExpired = true;
      stopForAuth();
      window.dispatchEvent(new Event('portal-auth-expired'));
    };
    window.addEventListener('portal-auth-expired', stopForAuth);
