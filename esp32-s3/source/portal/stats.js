    const updateRanking = (view, ranking, agents = []) => {
      const {root, rows: rankingRows, empty, records, kind} = view;
      if (!root || !rankingRows) return;
      if (!ranking || !Array.isArray(ranking.entries) || (kind === 'weekly' && ranking.source === 'daily' && ranking.available !== true)) {
        text(`[data-${kind}-state]`, kind === 'weekly' && ranking?.source === 'daily' ? 'Денні записи за цей тиждень недоступні · історія може бути неповною' : 'Рейтинг недоступний у цій версії контролера', root);
        text(`[data-${kind}-pending]`, '', root);
        if (empty) { empty.hidden = false; empty.textContent = 'Немає доступних даних рейтингу'; }
        if (rankingRows.children.length) rankingRows.replaceChildren();
        records.clear();
        return;
      }
      const seen = new Set();
      const entries = ranking.entries.filter(row => row && /^[0-9a-f]{8}$/i.test(row.id) &&
        Number.isSafeInteger(row.tokens) && row.tokens >= 0 && /^#[0-9a-f]{6}$/i.test(row.color))
        .filter(row => {
          const id = row.id.toUpperCase();
          if (seen.has(id) || id === '00000000') return false;
          seen.add(id); return true;
        }).slice(0, 12).sort((a, b) => b.tokens - a.tokens || a.id.localeCompare(b.id));
      const total = entries.reduce((sum, row) => sum + BigInt(row.tokens), 0n);
      const retained = new Set();
      const rows = entries.map((row, index) => {
        const id = row.id.toUpperCase();
        retained.add(id);
        let record = records.get(id);
        if (!record) {
          const tr = document.createElement('tr');
          const cells = Array.from({length: 4}, () => tr.appendChild(document.createElement('td')));
          const color = document.createElement('span');
          color.className = 'weekly-agent-color'; color.setAttribute('aria-hidden', 'true');
          record = {tr, cells, color, values: []};
          records.set(id, record);
        }
        const values = [String(index + 1), typeof row.name === 'string' ? row.name : row.id,
          number(row.tokens), row.tokens > 0 && BigInt(row.tokens) * 1000n < total ? '<0,1%' :
          `${decimal.format(total ? Number((BigInt(row.tokens) * 1000n + total / 2n) / total) / 10 : 0)}%`];
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
      for (const id of records.keys()) if (!retained.has(id)) records.delete(id);
      if (rankingRows.children.length !== rows.length || rows.some((row, index) => rankingRows.children[index] !== row))
        rankingRows.replaceChildren(...rows);
      if (empty) {
        empty.hidden = entries.length > 0;
        const label = kind === 'lifetime' ? 'Немає доступної статистики за журналами агентів' : 'За цей тиждень ще немає статистики токенів';
        if (empty.textContent !== label) empty.textContent = label;
      }
      const known = new Set(entries.map(row => row.id.toUpperCase()));
      const pending = Array.isArray(agents) ? agents.filter(agent => agent && agent.online &&
        typeof agent.id === 'string' && !known.has(agent.id.toUpperCase())).slice(0, 12) : [];
      text(`[data-${kind}-pending]`, pending.length ? `Очікується статистика токенів: ${pending.map(agent =>
        typeof agent.name === 'string' ? agent.name : agent.id).join(', ')}` : '', root);
      const week = String(ranking.week);
      if (kind === 'weekly') text('[data-weekly-period]', /^\d{8}$/.test(week) ?
        `${week.slice(6, 8)}.${week.slice(4, 6)}.${week.slice(0, 4)} · понеділок–неділя, Київ` : 'Очікування синхронізації часу', root);
      const daily = kind === 'weekly' && ranking.source === 'daily';
      const coverage = daily && Number.isInteger(ranking.observed_days) && Number.isInteger(ranking.elapsed_days) &&
        ranking.elapsed_days >= 1 && ranking.elapsed_days <= 7 && ranking.observed_days >= 0 && ranking.observed_days <= ranking.elapsed_days
        ? `${ranking.observed_days} із ${ranking.elapsed_days} днів` : 'кількість днів невідома';
      const dailyState = daily ? `Сума денних записів · ${coverage} · історія може бути неповною` : '';
      text(`[data-${kind}-state]`, daily ? `${dailyState}${ranking.save_failed ? ' · Не вдалося зберегти статистику. Повторна спроба автоматично.' :
        ranking.dirty ? ' · Є нові дані · збереження кожні 5 хвилин' : ''}` : ranking.save_failed ? 'Не вдалося зберегти статистику. Повторна спроба автоматично.' :
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
    const quotaClock = new Intl.DateTimeFormat('uk-UA', {hour: '2-digit', minute: '2-digit'});
    const firmwareDate = new Intl.DateTimeFormat('uk-UA', {dateStyle: 'medium', timeStyle: 'short'});
    const firmwareDateNodes = all('[data-firmware-date]');
    const rotationStatusNodes = all('[data-quota-rotation-status]');
    const otaProgress = one('#ota-progress');
    const appearanceForms = all('form[action="/sky-colors"]');
    const quotaGuidance = all('[data-quota-guidance]');
    const quotaCards = new Map(all('[data-quota-card]').map(card => [card.dataset.quotaCard, {
      card, meter: one('[data-quota-meter]', card), current: one('[data-quota-current]', card),
      meta: one(`[data-live="${card.dataset.quotaCard}-quota-meta"]`, card), empty: one('[data-quota-empty]', card),
    }]));
    const compactSeconds = seconds => seconds < 60 ? `${Math.floor(seconds)} ${portalStrings.seconds}` :
      seconds < 3600 ? `${Math.floor(seconds / 60)} ${portalStrings.minutes}` : `${Math.floor(seconds / 3600)} ${portalStrings.hours}`;
    const update = (status, sequence) => {
      reconcileSubmissions(status, sequence);
      const stamp = {uptime: status.uptime_ms, frames: status.frames, device: status.device_id, version: status.version};
      let fps = missing;
      if (baseline && stamp.device === baseline.device && stamp.version === baseline.version && stamp.uptime > baseline.uptime && stamp.frames >= baseline.frames) {
        fps = decimal.format((stamp.frames - baseline.frames) * 1000 / (stamp.uptime - baseline.uptime));
      }
      baseline = stamp;
      // New firmware supplies actual build time in UTC seconds. Version IDs are
      // manually advanced; interpret them as milliseconds only for legacy status.
      const legacyVersion = typeof status.version === 'string' && /^\d{13}$/.test(status.version) ? Number(status.version) : NaN;
      const buildMilliseconds = status.build_epoch === undefined ? legacyVersion :
        Number.isSafeInteger(status.build_epoch) && status.build_epoch >= 1700000000 && status.build_epoch <= 8640000000000
          ? status.build_epoch * 1000 : NaN;
      setText(firmwareDateNodes, Number.isSafeInteger(buildMilliseconds) && buildMilliseconds >= 1700000000000 && buildMilliseconds <= 8640000000000000
        ? firmwareDate.format(new Date(buildMilliseconds)) : missing);
      live('remaining', status.quota_known === true ?
        `${percent(status.remaining)}${status.quota_fresh === true ? '' : ` · ${portalStrings.remainingLastData}`}` : missing);
      live('quota-provider', status.quota_provider === 'claude' ? 'Claude Code · 5 год' : 'Codex');
      if (Number.isInteger(status.quota_rotation_minutes) && status.quota_rotation_minutes >= 1 && status.quota_rotation_minutes <= 60)
        rotationInputs.forEach(input => {
          if (!dirtyForms.has(input.closest('form'))) input.value = String(status.quota_rotation_minutes);
        });
      const rotationMinutes = Number.isInteger(status.quota_rotation_minutes) && status.quota_rotation_minutes >= 1 && status.quota_rotation_minutes <= 60
        ? `${status.quota_rotation_minutes} хв` : 'заданий інтервал';
      const providerName = status.quota_provider === 'claude' ? 'Claude Code' : status.quota_provider === 'codex' ? 'Codex' : null;
      const rotationRule = `Codex і Claude Code перемикаються кожні ${rotationMinutes}, коли обидва мають доступний залишок. Якщо доступний лише один сервіс, використовується він.`;
      setText(rotationStatusNodes, `${rotationRule} ${status.on === false ? 'Матрицю вимкнено.' :
        status.quota_available !== true || !providerName ? 'Зараз немає доступних даних для неба.' : `Зараз небо показує ${providerName}.`}`);
      appearanceForms.forEach(form => {
        if (dirtyForms.has(form)) return;
        for (const provider of ['codex', 'claude']) {
          const color = status.sky_colors?.[provider];
          const input = one(`input[name=${provider}]`, form);
          if (input && typeof color === 'string' && /^#[0-9a-f]{6}$/i.test(color)) input.value = color.toLowerCase();
        }
      });
      for (const provider of ['codex', 'claude']) {
        const quota = status.quotas?.[provider];
        const known = quota?.known === true && finite(quota.remaining) && quota.remaining >= 0 && quota.remaining <= 100;
        const view = quotaCards.get(provider);
        live(`${provider}-remaining`, known ? `${percent(quota.remaining)}${!view && quota.fresh !== true ? ` · ${portalStrings.remainingLastData}` : ''}` : view ? '—' : missing);
        if (view) {
          const current = known && status.on === true && status.quota_available === true && status.quota_provider === provider;
          view.card.dataset.known = String(known);
          view.card.dataset.active = String(current);
          view.card.dataset.stale = String(known && (quota.stale === true || quota.fresh === false));
          if (view.current) view.current.hidden = !current;
          if (view.meter) { view.meter.hidden = !known; view.meter.max = 100; view.meter.value = known ? quota.remaining : 0; }
          if (view.meta) view.meta.hidden = !known;
          if (view.empty) view.empty.hidden = known;
          const color = status.sky_colors?.[provider];
          if (typeof color === 'string' && /^#[0-9a-f]{6}$/i.test(color)) view.card.style.setProperty('--quota-color', color);
        }
      }
      const bothQuotasKnown = ['codex', 'claude'].every(provider => {
        const quota = status.quotas?.[provider];
        return quota?.known === true && finite(quota.remaining) && quota.remaining >= 0 && quota.remaining <= 100;
      });
      quotaGuidance.forEach(node => { node.hidden = bothQuotasKnown; });
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
      for (const provider of ['codex', 'claude']) {
        const quota = status.quotas?.[provider];
        if (quota?.known !== true || !finite(quota.remaining) || quota.remaining < 0 || quota.remaining > 100) {
          live(`${provider}-quota-meta`, '');
          continue;
        }
        const validReset = finite(quota?.reset_at) && quota.reset_at > 0 && quota.reset_at <= 8640000000000;
        let reset = validReset ? `${portalStrings.resetPrefix}${quotaClock.format(new Date(quota.reset_at * 1000))}` : portalStrings.resetUnknown;
        if (validReset && quotaCards.has(provider) && finite(status.time_epoch) && status.time_epoch >= 1700000000 && status.time_epoch <= 8640000000000) {
          const remaining = Math.ceil(quota.reset_at - status.time_epoch);
          reset += remaining > 0 ? ` · через ${compactSeconds(remaining)}` : ' · очікуємо скидання';
        }
        const age = finite(quota?.age_seconds) && quota.age_seconds >= 0
          ? `${portalStrings.agePrefix}${compactSeconds(quota.age_seconds)}${portalStrings.ageSuffix}` : portalStrings.ageUnknown;
        live(`${provider}-quota-meta`, `${reset} · ${age}${quota?.stale === true ? ` · ${portalStrings.stale}` : ''}`);
      }
      const progress = otaProgress;
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
      const blackHole = status.black_hole;
      if (blackHole && typeof blackHole.manual === 'boolean' &&
          Number.isInteger(blackHole.remaining_seconds) && blackHole.remaining_seconds >= 0 && blackHole.remaining_seconds <= 60) {
        text('[data-black-hole-state]', blackHole.manual ? `Відтворення · ${blackHole.remaining_seconds} с` : '');
        all('[data-black-hole-play]').forEach(button => {
          button.disabled = blackHole.can_play !== true || status.on !== true || status.maintenance === true || memorial?.active === true;
        });
        all('[data-black-hole-stop]').forEach(button => {
          button.disabled = !blackHole.manual || status.maintenance === true;
          button.hidden = !blackHole.manual;
        });
      }
      all('[data-buzzer-test]').forEach(button => { button.disabled = status.buzzer_ready !== true || status.maintenance === true; });
      const volumeForm = one('#buzzer-volume-form');
      if (volumeForm && Number.isInteger(status.buzzer_volume) && status.buzzer_volume >= 0 && status.buzzer_volume <= 100) {
        const volume = one('#buzzer-volume', volumeForm);
        if (volume && !dirtyForms.has(volumeForm)) {
          volume.value = String(status.buzzer_volume);
          text('#buzzer-volume-value', `${status.buzzer_volume}%`);
        }
        const controls = one('fieldset', volumeForm);
        if (controls) controls.disabled = status.buzzer_ready !== true || status.maintenance === true;
        all('button[type=submit],button:not([type])', volumeForm).forEach(button => {
          button.disabled = status.buzzer_ready !== true || status.maintenance === true;
        });
        text('[data-buzzer-save]', status.buzzer_save_failed === true ? 'Не вдалося зберегти гучність. Повторюємо спробу.' :
          status.buzzer_saving === true ? 'Збереження…' : '');
      }
      const light = status.external_light;
      setLightOutputs(light);
      const lightForm = one('#external-light-form');
      if (lightForm && light && ['first', 'second'].every(key => Number.isInteger(light[key]) && light[key] >= 0 && light[key] <= 100) &&
          ['ready', 'allowed', 'follow_matrix', 'saving', 'save_failed'].every(key => typeof light[key] === 'boolean')) {
        if (!dirtyForms.has(lightForm)) {
          for (const key of ['first', 'second']) {
            const input = one(`#light-${key}`, lightForm);
            if (input) input.value = String(light[key]);
            text(`#light-${key}-value`, `${light[key]}%`);
          }
          const follow = one('input[name=follow_matrix]', lightForm);
          if (follow) follow.checked = light.follow_matrix;
        }
        const controls = one('fieldset', lightForm);
        if (controls) controls.disabled = !light.ready || status.maintenance === true;
        text('[data-light-state]', !light.ready ? 'Помилка виходів. Підсвітку вимкнено.' :
          !light.allowed && (light.first || light.second) ? 'Вимкнено разом із матрицею або на час обслуговування.' :
          light.first || light.second ? portalStrings.on : portalStrings.off);
        text('[data-light-save]', light.save_failed ? 'Не вдалося зберегти. Повторюємо спробу.' : light.saving ? 'Збереження…' : '');
      }
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
      const airAlert = status.air_alert;
      if (airAlertForm && airAlert && typeof airAlert === 'object' && !Array.isArray(airAlert) &&
          ['enabled', 'manual', 'fresh', 'save_failed'].every(key => typeof airAlert[key] === 'boolean') &&
          ['clear', 'yellow', 'red', 'untyped', 'unknown'].includes(airAlert.level) &&
          Number.isInteger(airAlert.remaining_seconds) && airAlert.remaining_seconds >= 0 && airAlert.remaining_seconds <= 60 &&
          (airAlert.updated_age_seconds === null || (finite(airAlert.updated_age_seconds) && airAlert.updated_age_seconds >= 0)) &&
          ['source_status', 'status', 'details'].every(key => typeof airAlert[key] === 'string')) {
        const checkbox = airAlertForm && one('input[name=enabled]', airAlertForm);
        if (checkbox && !dirtyForms.has(airAlertForm)) checkbox.checked = airAlert.enabled;
        // A stale clear response must never read as an all-clear.
        const labels = {clear: 'Тривоги не зафіксовано', yellow: 'Жовтий рівень', red: 'Червоний рівень',
          untyped: 'Тривога активна · тип загрози не уточнено', unknown: missing};
        const unknown = airAlert.level === 'unknown' || (airAlert.enabled === true && !airAlert.fresh && airAlert.level === 'clear');
        text('[data-air-alert-state]', unknown ? 'Немає актуальних даних' :
          airAlert.level === 'untyped' ? labels.untyped : airAlert.status || labels[airAlert.level]);
        text('[data-air-alert-note]', [airAlert.save_failed ? 'Не вдалося зберегти. Спробуйте ще раз.' : '',
          airAlert.source_status, airAlert.details,
          status.maintenance === true ? 'Контролер зайнятий. Тест недоступний.' :
          status.on !== true ? 'Матриця вимкнена. Для тесту ввімкніть її.' : ''].filter(Boolean).join(' · '));
        text('[data-air-alert-updated]', airAlert.updated_age_seconds === null ? 'Оновлення: ще немає даних' :
          `Оновлено ${integer.format(Math.floor(airAlert.updated_age_seconds))} с тому${airAlert.fresh ? '' : ' · дані застаріли'}`);
        text('[data-city-threat-state]', airAlert.city_demo === true ? 'Ручний тест блимання та звуку · 60 с' : airAlert.city_reported === true ? 'Є свіже повідомлення про об’єкт над містом' :
          airAlert.city_failed === true ? 'Джерело Повітряних сил тимчасово недоступне' :
          airAlert.city_fresh === true ? 'Свіжих повідомлень про об’єкти над містом не знайдено' : 'Очікування даних Повітряних сил');
        text('[data-city-threat-source]', airAlert.city_failed === true ? 'Зв’язок із каналом Повітряних сил втрачено. Підтверджений допис діє до завершення 5 хв від публікації.' : '');
        text('[data-air-alert-manual]', airAlert.manual ? `Ручний тест · ${airAlert.remaining_seconds} с` : 'Ручний тест не активний');
        all('[data-air-alert-play]').forEach(button => { button.disabled = status.on !== true || status.maintenance === true; });
        all('[data-air-alert-stop]').forEach(button => { button.disabled = !airAlert.manual || status.maintenance === true; });
      } else if (airAlertForm) {
        text('[data-air-alert-state]', 'Немає актуальних даних');
        text('[data-air-alert-updated]', 'Очікування коректних даних контролера');
        all('[data-air-alert-play],[data-air-alert-stop]').forEach(button => { button.disabled = true; });
      }
      updateAgents(status.agents);
      rankings.forEach(view => updateRanking(view, status[view.kind], status.agents));
    };
