    // Native anchors remain the no-JS fallback. With JS, show one agent section
    // at a time so managing a PC never requires scrolling past token history.
    const agentNavigation = one('.agent-page-nav');
    if (agentNavigation) {
      const entries = all('a[href^="#agent-"]', agentNavigation).map(link => ({
        link, panel: one(link.getAttribute('href')),
      })).filter(entry => entry.panel);
      const selectSection = hash => {
        const selected = entries.find(entry => entry.link.getAttribute('href') === hash) || entries[0];
        entries.forEach(entry => {
          entry.panel.hidden = entry !== selected;
          if (entry === selected) entry.link.setAttribute('aria-current', 'page');
          else entry.link.removeAttribute('aria-current');
        });
      };
      entries.forEach(({link}) => link.addEventListener('click', event => {
        if (event.ctrlKey || event.metaKey || event.shiftKey || event.altKey) return;
        event.preventDefault();
        const hash = link.getAttribute('href');
        window.history?.replaceState(null, '', hash);
        selectSection(hash);
      }));
      window.addEventListener('hashchange', () => selectSection(window.location.hash));
      selectSection(window.location.hash);
    }
    const settingsDialog = one('#device-settings-dialog');
    if (settingsDialog) {
      const content = one('[data-device-settings-body]', settingsDialog);
      const heading = one('#device-settings-title', settingsDialog);
      const targets = {
        brightness: {title: 'Яскравість матриці', form: one('form[action="/brightness"]')},
        power: {title: 'Живлення матриці', form: one('form[action="/power"]')},
        schedule: {title: 'Розклад матриці', form: one('form[action="/schedule"]')},
        lighting: {title: 'Зовнішня підсвітка', form: one('form[action="/external-light"]')},
      };
      if (content && heading && typeof settingsDialog.showModal === 'function') {
        for (const key of ['schedule', 'lighting']) {
          const wrapper = targets[key].form?.closest('[data-device-advanced]');
          if (wrapper) wrapper.hidden = true;
        }
      }
      let moved = null, placeholder = null, trigger = null;
      settingsDialog.addEventListener('close', () => {
        if (moved && placeholder) placeholder.replaceWith(moved);
        moved = placeholder = null;
        if (authenticated() && trigger && trigger.isConnected) trigger.focus({preventScroll: true});
        trigger = null;
      });
      all('[data-device-settings]').forEach(button => button.addEventListener('click', event => {
        if (!authenticated()) { event.preventDefault(); showSessionBanner(); return; }
        const target = targets[button.dataset.deviceSettings];
        if (!target || !target.form || !content || !heading || settingsDialog.open) return;
        if (typeof settingsDialog.showModal !== 'function') {
          if (button.tagName === 'A') return;
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
        try { settingsDialog.showModal(); }
        catch (_) {
          placeholder.replaceWith(moved);
          const wrapper = moved.closest('[data-device-advanced]');
          if (wrapper) wrapper.hidden = false;
          moved = placeholder = trigger = null;
          return;
        }
        event.preventDefault();
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
