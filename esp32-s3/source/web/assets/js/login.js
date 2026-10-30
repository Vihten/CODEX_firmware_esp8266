(() => {
  'use strict';
  const start = () => {
    const form = document.querySelector('#login-form');
    if (!form || form.method !== 'post') return;
    const url = new URL(form.action, location.href);
    if (url.origin !== location.origin || url.pathname !== '/login') return;
    const password = form.querySelector('input[name=password]');
    const button = form.querySelector('button[type=submit]');
    const feedback = form.querySelector('#login-feedback');
    const show = form.querySelector('#login-show-password');
    if (!password || !button || !feedback) return;
    if (show) {
      show.closest('label').hidden = false;
      show.addEventListener('change', () => { password.type = show.checked ? 'text' : 'password'; });
    }
    if (!('fetch' in window) || !('AbortController' in window) || !('FormData' in window) ||
        !('URLSearchParams' in window)) return;
    let busy = false, cooldownUntil = 0, countdown = null, request = null, leaving = false;
    const label = button.textContent;
    let message = '';
    const render = () => {
      const seconds = Math.max(0, Math.ceil((cooldownUntil - Date.now()) / 1000));
      button.disabled = busy || seconds > 0;
      button.textContent = busy ? 'Перевірка…' : seconds ? `Повторити через ${seconds} с` : label;
      feedback.textContent = seconds && !busy ? `${message} Повторна спроба через ${seconds} с.` : message;
      if (!seconds && countdown !== null) { clearInterval(countdown); countdown = null; }
    };
    const secondsValue = value => {
      const number = typeof value === 'number' ? value : /^\d+$/.test(value || '') ? Number(value) : 0;
      return Number.isFinite(number) && number >= 0 && number <= 86400 ? Math.ceil(number) : 0;
    };
    password.addEventListener('input', () => password.removeAttribute('aria-invalid'));
    form.addEventListener('submit', async event => {
      if (event.defaultPrevented) return;
      event.preventDefault();
      if (busy || Date.now() < cooldownUntil) return;
      const symbols = Array.from(password.value).length;
      const bytes = typeof TextEncoder === 'function' ? new TextEncoder().encode(password.value).length : null;
      if (symbols < 10 || (bytes !== null && bytes > 128) || !form.reportValidity()) {
        message = 'Введіть щонайменше 10 символів, максимум 128 байт UTF-8.';
        password.setAttribute('aria-invalid', 'true');
        render(); password.focus(); return;
      }
      // Serialize before changing busy controls; never persist or log credentials.
      const body = new URLSearchParams();
      for (const [name, value] of new FormData(form)) body.append(name, String(value));
      const submitter = event.submitter;
      if (submitter?.name) body.append(submitter.name, submitter.value);
      body.set('__async', '1');
      busy = true; leaving = false;
      message = 'Перевіряємо пароль. Зачекайте.';
      form.setAttribute('aria-busy', 'true');
      render();
      const controller = new AbortController(); request = controller;
      const timeout = setTimeout(() => controller.abort(), 20000);
      try {
        const response = await fetch(url.href, {method: 'POST', body, credentials: 'same-origin',
          headers: {Accept: 'application/json', 'Content-Type': 'application/x-www-form-urlencoded'}, signal: controller.signal});
        const headerDelay = secondsValue(response.headers.get('Retry-After'));
        if (headerDelay) cooldownUntil = Date.now() + headerDelay * 1000;
        if (response.redirected || !(response.headers.get('content-type') || '').includes('application/json')) {
          message = 'Контролер надіслав неочікувану відповідь. Спробуйте ввійти ще раз.';
          return;
        }
        const result = await response.json();
        if (!result || typeof result !== 'object' || Array.isArray(result)) {
          message = 'Не вдалося прочитати відповідь контролера. Спробуйте ще раз.';
          return;
        }
        if (response.status === 200 && result.authenticated === true && result.redirect === '/') {
          message = 'Вхід підтверджено.';
          window.location.assign('/');
          return;
        }
        const delay = Math.max(headerDelay, secondsValue(result.retry_after));
        if (delay) cooldownUntil = Date.now() + delay * 1000;
        if (!response.ok && typeof result.error === 'string' && result.error.trim()) {
          message = result.error.slice(0, 400);
          if (response.status === 401) password.setAttribute('aria-invalid', 'true');
        } else message = 'Контролер не підтвердив вхід. Спробуйте ще раз.';
      } catch (_) {
        if (!leaving) message = controller.signal.aborted ?
          'Контролер не відповів за 20 секунд. Спробуйте ввійти ще раз.' :
          'Немає зв’язку з контролером або відповідь пошкоджена. Перевірте підключення та повторіть вхід.';
      } finally {
        clearTimeout(timeout);
        request = null; busy = false;
        form.removeAttribute('aria-busy');
        render();
        if (!leaving && Date.now() < cooldownUntil && countdown === null) countdown = setInterval(render, 250);
      }
    });
    window.addEventListener('pagehide', () => {
      leaving = true;
      if (request) request.abort();
      if (countdown !== null) { clearInterval(countdown); countdown = null; }
    });
    window.addEventListener('pageshow', () => {
      leaving = false; render();
      if (Date.now() < cooldownUntil && countdown === null) countdown = setInterval(render, 250);
    });
  };
  if (document.readyState === 'loading') document.addEventListener('DOMContentLoaded', start, {once: true});
  else start();
})();
