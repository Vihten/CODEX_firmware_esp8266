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
      const controls = one('[data-device-controls]');
      const lightPanel = one('#external-light-panel');
      if (lightPanel && content && heading && targets.lighting.form &&
          typeof settingsDialog.showModal === 'function') lightPanel.hidden = true;
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
