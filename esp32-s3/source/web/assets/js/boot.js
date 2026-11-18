(() => {
  'use strict';
  if (document.readyState !== 'loading') return;
  const root = document.documentElement;
  root.dataset.pageLoading = 'true';
  const finish = () => {
    clearTimeout(deadline);
    delete root.dataset.pageLoading;
  };
  const deadline = setTimeout(finish, 8000);
  document.addEventListener('DOMContentLoaded', finish, {once: true});
  window.addEventListener('pageshow', event => { if (event.persisted) finish(); });
})();
