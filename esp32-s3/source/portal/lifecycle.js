    const tokenHistory = one('#token-history');
    const updateTokenHistory = detail => tokenHistory?.dispatchEvent(new CustomEvent('portal-token-history', {detail}));
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
      setLightOutputs(null);
      clearCityThreat();
      live('output-limit', '');
      if (!authenticated()) connection('Сеанс завершено. Увійдіть знову.', 'error');
      else connection(document.hidden || !pageActive ? 'Оновлення призупинено' : 'Автооновлення вимкнено');
    };
    const poll = async (manual = false, feedback = true) => {
      if (!authenticated() || !pageActive || document.hidden || (!manual && !automatic())) return;
      const manualFeedback = request => {
        request.manual = true;
        all('[data-refresh]').forEach(button => { button.disabled = true; });
        connection('Оновлення…');
        text('[data-weekly-state],[data-lifetime-state]', 'Оновлення рейтингу…');
      };
      if (active) {
        if (manual && feedback && !active.manual && !active.reason) manualFeedback(active);
        return;
      }
      if (timer !== null) clearTimeout(timer);
      timer = null;
      const request = {controller: new AbortController(), reason: '', manual: false, sequence: ++pollSequence};
      active = request;
      tokenHistory?.setAttribute('aria-busy', 'true');
      if (manual && feedback) manualFeedback(request);
      else if (!baseline) connection('Оновлення…');
      const timeout = setTimeout(() => { request.reason = 'timeout'; request.controller.abort(); }, 6000);
      try {
        const response = await fetch('/status', {signal: request.controller.signal, cache: 'no-store', credentials: 'same-origin'});
        if (response.status === 401 || response.redirected || (response.headers.get('content-type') || '').includes('text/html')) {
          expireSession();
          return;
        }
        if (!response.ok) throw new Error('HTTP');
        const status = await response.json();
        if (!status || !finite(status.uptime_ms) || !finite(status.frames) || !Array.isArray(status.agents)) throw new Error('JSON');
        if (request.reason || !authenticated() || document.hidden) return;
        update(status, request.sequence);
        const week = status.weekly;
        updateTokenHistory(week?.source === 'daily' && Array.isArray(week.days) ? {
          ...week, available: week.codex_available ?? week.available,
          saving: week.codex_dirty ?? week.dirty, save_failed: week.codex_save_failed ?? week.save_failed,
        } : {available: false});
        failures = 0;
        connection(status.maintenance === true ? 'Контролер зайнятий' : 'Дані оновлено', 'ok');
      } catch (_) {
        if (authenticated() && request.reason !== 'pause' && !document.hidden) {
          ++failures;
          baseline = null;
          setLightOutputs(null);
          live('fps', missing);
          live('output-limit', '');
          text('[data-weekly-state],[data-lifetime-state]', 'Не вдалося оновити рейтинг. Спробуйте ще раз.');
          updateTokenHistory(null);
          text('[data-air-alert-state]', 'Немає актуальних даних');
          text('[data-air-alert-updated]', 'Зв’язок із контролером втрачено');
          clearCityThreat();
          connection(request.reason === 'timeout' ? 'Контролер не відповів вчасно' : 'Не вдалося оновити дані', 'error');
        }
      } finally {
        clearTimeout(timeout);
        active = null;
        tokenHistory?.setAttribute('aria-busy', 'false');
        if (request.manual) all('[data-refresh]').forEach(button => { button.disabled = !authenticated(); });
        schedule(Math.min(30000, 5000 * Math.pow(2, Math.min(failures, 3))));
        if (refreshAfterActive && authenticated()) { refreshAfterActive = false; setTimeout(() => poll(true, false), 0); }
      }
    };
    const refreshCommandStatus = () => {
      if (!authenticated()) return;
      if (active) refreshAfterActive = true;
      else void poll(true, false);
    };
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
    if (!authenticated()) stopForAuth();
    else if (tokenHistory && !document.hidden) void poll(true, false);
    else if (automatic()) { connection('Очікування оновлення'); schedule(5000); }
    else pause();
