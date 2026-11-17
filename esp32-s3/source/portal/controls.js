    document.addEventListener('input', event => {
      const input = event.target;
      if (!(input instanceof Element)) return;
      if (!input.matches('[data-poll-toggle],#show-password')) markDirty(input.closest('form'));
      if (input.matches('[data-agent-name-input]')) input.setCustomValidity?.('');
      if (input.id === 'brightness') {
        const output = one('#brightnessValue');
        if (output) output.textContent = `${Math.round(Number(input.value) * 100 / 255)}%`;
      }
      if (input.matches('[data-light-range]')) text(`#${input.id}-value`, `${input.value}%`);
      if (input.id === 'buzzer-volume') text('#buzzer-volume-value', `${input.value}%`);
      if (input.matches('.agent-color input[type=color]')) {
        const output = one('output', input.closest('form'));
        if (output) output.textContent = input.value.toUpperCase();
      }
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
        markDirty(form);
        awaitingState.delete(form);
        const row = form instanceof Element && form.closest('[data-agent]');
        if (row) agentStates.delete(row);
        const range = one('#brightness', form);
        const output = one('#brightnessValue');
        if (range && output) output.textContent = `${Math.round(Number(range.value) * 100 / 255)}%`;
        const volume = one('#buzzer-volume', form);
        if (volume) text('#buzzer-volume-value', `${volume.value}%`);
      }, 0);
    });
