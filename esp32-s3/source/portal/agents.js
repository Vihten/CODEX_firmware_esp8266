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
        if (root.closest('.agent-summary-compact')) ordered.forEach((row, index) => { row.hidden = index >= 3; });
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
        text('[data-agent-state]', online ? portalStrings.online : portalStrings.offline, row);
        text('[data-agent-activity]', percent(activity), row);
        text('[data-agent-remaining]', online && agent.quota_source === true ? percent(agent.remaining) : missing, row);
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
          if (!node.hasAttribute('data-agent-state') && !one('[data-agent-state]', node)) node.textContent = online ? portalStrings.online : portalStrings.offline;
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
