    const markDirty = form => {
      if (!form) return;
      dirtyForms.add(form);
      formRevisions.set(form, (formRevisions.get(form) || 0) + 1);
    };
    // Include disabled controls in the edit snapshot: status polling may disable them.
    const formSnapshot = form => JSON.stringify(Array.from(form.elements)
      .filter(input => input.matches('input,select,textarea'))
      .map(input => [input.name, input.value, input.checked,
        input.multiple ? Array.from(input.selectedOptions, option => option.value) : null]));
    const formFeedback = (form, message, login = false) => {
      let node = one('[data-command-feedback]', form);
      if (!node) {
        node = document.createElement('p');
        node.className = 'muted';
        node.dataset.commandFeedback = '';
        node.setAttribute('role', 'status');
        node.setAttribute('aria-live', 'polite');
        form.append(node);
      }
      node.textContent = message;
      if (login) {
        const link = document.createElement('a');
        link.href = '/login'; link.textContent = 'Увійти';
        node.append(' ', link);
      }
    };
    const reflected = (record, status) => {
      const data = record.data, value = name => data.get(name);
      const agent = Array.isArray(status.agents) && status.agents.find(item =>
        typeof item.id === 'string' && item.id.toUpperCase() === (value('client') || '').toUpperCase());
      switch (record.path) {
        case '/brightness': return status.brightness === Number(value('value'));
        case '/buzzer-volume': return status.buzzer_volume === Number(value('value'));
        case '/power': return status.power === value('mode');
        case '/external-light': {
          const light = status.external_light;
          return light && light.first === (value('off') === '1' ? 0 : Number(value('first'))) &&
            light.second === (value('off') === '1' ? 0 : Number(value('second'))) &&
            light.follow_matrix === (value('follow_matrix') === '1');
        }
        case '/memorial': return value('mode') === 'save' && status.memorial &&
          status.memorial.enabled === (value('enabled') === '1');
        case '/air-alert': return value('mode') === 'save' && status.air_alert &&
          status.air_alert.enabled === (value('enabled') === '1');
        case '/quota-rotation': return status.quota_rotation_minutes === Number(value('minutes'));
        case '/agent-rename': return agent && agent.name === value('name');
        case '/agent-color': return agent && (value('mode') === 'auto' ? agent.custom_color === false :
          agent.custom_color === true && agent.color?.toLowerCase() === value('color')?.toLowerCase());
        default: return false;
      }
    };
    const tracksState = record => ['/brightness', '/buzzer-volume', '/power', '/external-light',
      '/quota-rotation', '/agent-rename', '/agent-color'].includes(record.path) ||
      (['/memorial', '/air-alert'].includes(record.path) && record.data.get('mode') === 'save');
    const acceptedMessage = record => {
      switch (record.path) {
        case '/buzzer-test': return 'Запит перевірки надіслано.';
        case '/black-hole': return record.data.get('mode') === 'stop' ? 'Запит зупинки надіслано.' : 'Запит відтворення чорної діри на 60 секунд надіслано.';
        case '/restart': return 'Запит перезапуску прийнято. Стан підключення оновлюється окремо.';
        case '/update': return 'Запит перевірки оновлення прийнято. Перебіг відображається у стані оновлення.';
        case '/memorial':
        case '/air-alert': return 'Запит прийнято в чергу. Перебіг відображається у стані контролера.';
        default: return 'Команду прийнято в чергу.';
      }
    };
    const reconcileSubmissions = (status, sequence) => {
      for (const [form, record] of awaitingState) {
        if (!form.isConnected || record.revision !== (formRevisions.get(form) || 0) ||
            record.snapshot !== formSnapshot(form)) { awaitingState.delete(form); continue; }
        const failed = ({'/external-light': status.external_light?.save_failed,
          '/memorial': status.memorial?.save_failed, '/air-alert': status.air_alert?.save_failed,
          '/buzzer-volume': status.buzzer_save_failed})[record.path] === true;
        if (sequence < record.poll || (!failed && !reflected(record, status))) continue;
        dirtyForms.delete(form);
        awaitingState.delete(form);
        formFeedback(form, failed ? 'Не вдалося зберегти. Показано стан контролера.' : 'Стан контролера оновлено.');
        const row = form.closest('[data-agent]');
        if (row) agentStates.delete(row);
      }
    };
    const sendCommand = async (form, record) => {
      if (!authenticated()) { formFeedback(form, 'Сеанс завершено.', true); return; }
      const {data, submitter, url} = record;
      const epoch = authEpoch;
      awaitingState.delete(form);
      submitting.add(form);
      currentIntents.set(form, record);
      form.setAttribute('aria-busy', 'true');
      const previousBusy = submitter?.getAttribute('aria-busy');
      if (submitter) submitter.setAttribute('aria-busy', 'true');
      formFeedback(form, 'Надсилання…');
      const controller = new AbortController();
      commandControllers.add(controller);
      const timeout = setTimeout(() => controller.abort(), 8000);
      let accepted = false;
      try {
        const response = await fetch(url.href, {method: 'POST', body: data, credentials: 'same-origin',
          headers: {Accept: 'application/json'}, signal: controller.signal});
        const type = response.headers.get('content-type') || '';
        if (response.redirected || response.status === 401 || type.includes('text/html')) {
          expireSession();
          return;
        }
        if (!authenticated() || epoch !== authEpoch) return;
        if (!response.ok) {
          let detail = '';
          if (type.includes('application/json')) {
            const error = await response.json();
            detail = typeof error.message === 'string' ? error.message : typeof error.error === 'string' ? error.error : '';
          } else if (type.includes('text/plain')) detail = await response.text();
          if (!authenticated() || epoch !== authEpoch) return;
          formFeedback(form, `Команду не прийнято (HTTP ${response.status}). ${detail.slice(0, 300)}`);
          return;
        }
        const result = type.includes('application/json') && await response.json();
        if (!authenticated() || epoch !== authEpoch) return;
        if (response.status !== 202 || !result || result.accepted !== true) {
          formFeedback(form, 'Сервер не підтвердив прийняття команди. Перевірте стан контролера.');
          return;
        }
        accepted = true;
        if (tracksState(record)) {
          record.poll = pollSequence + 1;
          awaitingState.set(form, record);
          formFeedback(form, 'Команду прийнято в чергу. Очікуємо стан контролера.');
        } else formFeedback(form, acceptedMessage(record));
        refreshCommandStatus();
        // Bounded GET follow-ups observe queue processing without retrying the command.
        for (const delay of [500, 1500]) {
          const id = setTimeout(() => { commandTimers.delete(id); refreshCommandStatus(); }, delay);
          commandTimers.add(id);
        }
      } catch (_) {
        if (authenticated()) formFeedback(form, controller.signal.aborted ?
          'Час очікування минув. Команда могла надійти; перевірте стан перед повтором.' :
          'Немає підтвердження від контролера. Перевірте стан перед повтором.');
      } finally {
        clearTimeout(timeout);
        commandControllers.delete(controller);
        if (!accepted && record.revision === (formRevisions.get(form) || 0) && record.snapshot === formSnapshot(form)) {
          dirtyForms.delete(form);
          const row = form.closest('[data-agent]');
          if (row) agentStates.delete(row);
        }
        submitting.delete(form);
        currentIntents.delete(form);
        form.removeAttribute('aria-busy');
        if (submitter) {
          if (previousBusy === null) submitter.removeAttribute('aria-busy');
          else submitter.setAttribute('aria-busy', previousBusy);
        }
        const next = queuedIntents.get(form);
        queuedIntents.delete(form);
        if (next && epoch !== authEpoch) {
          formFeedback(form, 'Сеанс завершено. Останній запит не надіслано. Увійдіть і повторіть дію.', true);
        } else if (next && form.isConnected && pageActive) void sendCommand(form, next);
      }
    };
    document.addEventListener('submit', event => {
      const form = event.target;
      if (!(form instanceof HTMLFormElement) || event.defaultPrevented) return;
      const submitter = event.submitter;
      const method = (submitter?.getAttribute('formmethod') || form.getAttribute('method') || 'get').toLowerCase();
      if (method === 'dialog') return;
      if (form instanceof Element && form.matches('form.agent-rename')) {
        const input = one('[data-agent-name-input]', form);
        if (input && typeof TextEncoder === 'function' && typeof input.setCustomValidity === 'function') {
          input.setCustomValidity(new TextEncoder().encode(input.value.trim()).length > 48 ? 'Скоротіть ім’я: до 24 українських або 48 латинських літер.' : '');
          if (!input.checkValidity()) { event.preventDefault(); input.reportValidity(); return; }
        }
      }
      const url = new URL(submitter?.getAttribute('formaction') || form.action, location.href);
      if (method !== 'post' || url.origin !== location.origin || !commandRoutes.has(url.pathname) ||
          !('fetch' in window) || !('AbortController' in window)) {
        if (form.matches('form[data-confirm]') && !window.confirm(form.dataset.confirm)) event.preventDefault();
        return;
      }
      event.preventDefault();
      if (!form.noValidate && !submitter?.formNoValidate && !form.reportValidity()) return;
      // Capture the user's intent now, including the submitter and hidden fields.
      const data = new URLSearchParams();
      for (const [name, value] of new FormData(form)) data.append(name, String(value));
      if (submitter?.name) data.append(submitter.name, submitter.value);
      data.set('__async', '1');
      const signature = `${url.href}\n${data.toString()}`;
      const current = currentIntents.get(form);
      if (current?.signature === signature) {
        // Returning to the in-flight intent supersedes an intervening queued action.
        markDirty(form);
        current.revision = formRevisions.get(form);
        current.snapshot = formSnapshot(form);
        queuedIntents.delete(form);
        formFeedback(form, 'Надсилання…');
        return;
      }
      if (queuedIntents.get(form)?.signature === signature) return;
      if (form.matches('form[data-confirm]') && !window.confirm(form.dataset.confirm)) return;
      markDirty(form);
      awaitingState.delete(form);
      const record = {data, submitter, url, signature, path: url.pathname,
        revision: formRevisions.get(form), snapshot: formSnapshot(form)};
      if (submitting.has(form)) {
        queuedIntents.set(form, record);
        formFeedback(form, 'Останній запит буде надіслано після поточного.');
      } else void sendCommand(form, record);
    });
