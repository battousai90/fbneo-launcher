/* FBNeo Launcher — landing page runtime.
 *
 * Deliberately small. Translation happens at build time (build.js writes one static
 * page per language), so nothing here touches the copy: each URL is already in its
 * own language, works with JavaScript disabled, and is indexed on its own.
 *
 * What is left is the genuinely dynamic part: the theme toggle, the language button
 * label, and pulling the current release from GitHub.
 */
(function () {
  'use strict';

  // ── Theme: System / Light / Dark, matching the application ─────────────────
  var THEME_KEY = 'fbneo-theme';
  var btn = document.getElementById('theme-btn');
  if (btn) {
    var MODES = ['system', 'light', 'dark'];
    var LABEL = { system: 'Theme: system', light: 'Theme: light', dark: 'Theme: dark' };

    var set = function (mode) {
      if (mode === 'system') delete document.documentElement.dataset.theme;
      else document.documentElement.dataset.theme = mode;
      btn.dataset.mode = mode;
      btn.title = LABEL[mode];
      btn.setAttribute('aria-label', LABEL[mode]);
      try { localStorage.setItem(THEME_KEY, mode); } catch (e) { /* private mode */ }
    };

    var stored = 'system';
    try {
      var m = localStorage.getItem(THEME_KEY);
      if (MODES.indexOf(m) >= 0) stored = m;
    } catch (e) {}
    set(stored);

    btn.addEventListener('click', function () {
      set(MODES[(MODES.indexOf(btn.dataset.mode) + 1) % MODES.length]);
    });
  }

  // ── Language menu ─────────────────────────────────────────────────────────
  // The entries are ordinary links to the pre-rendered pages, so this only opens
  // and closes the menu and marks the page's own language as selected.
  var lbtn = document.getElementById('lang-btn');
  var menu = document.getElementById('lang-menu');
  if (lbtn && menu) {
    var items = [].slice.call(menu.querySelectorAll('[data-lang]'));
    var here = document.documentElement.lang || 'en';

    items.forEach(function (a) {
      var on = a.getAttribute('data-lang') === here;
      a.setAttribute('aria-current', on ? 'true' : 'false');
      if (on) {
        var flag = lbtn.querySelector('.flag');
        var name = document.getElementById('lang-name');
        if (flag) flag.innerHTML = a.querySelector('.flag').innerHTML;
        if (name) name.textContent = a.textContent.trim();
      }
    });

    var open = function (state) {
      menu.hidden = !state;
      lbtn.setAttribute('aria-expanded', state ? 'true' : 'false');
    };
    lbtn.addEventListener('click', function (e) { e.stopPropagation(); open(menu.hidden); });
    document.addEventListener('click', function (e) {
      if (!menu.hidden && !menu.contains(e.target) && e.target !== lbtn) open(false);
    });
    document.addEventListener('keydown', function (e) {
      if (e.key === 'Escape' && !menu.hidden) { open(false); lbtn.focus(); }
    });
    menu.addEventListener('keydown', function (e) {
      var i = items.indexOf(document.activeElement);
      if (e.key === 'ArrowDown') { e.preventDefault(); items[(i + 1) % items.length].focus(); }
      if (e.key === 'ArrowUp')   { e.preventDefault(); items[(i - 1 + items.length) % items.length].focus(); }
    });
  }

  // ── Current release ───────────────────────────────────────────────────────
  // Turns the download buttons into direct asset links and shows the version.
  // Purely additive: without it the buttons still lead to the releases page.
  var REPO = 'battousai90/fbneo-launcher';
  var CACHE_KEY = 'fbneo-release', CACHE_MS = 30 * 60 * 1000;

  function size(bytes) {
    var mb = bytes / 1048576;
    return (mb >= 10 ? Math.round(mb) : mb.toFixed(1)) + ' MB';
  }

  function apply(rel) {
    if (!rel || !rel.tag_name) return;

    var badge = document.getElementById('rel-badge');
    if (badge) {
      var when = rel.published_at
        ? new Date(rel.published_at).toLocaleDateString(document.documentElement.lang || undefined,
            { year: 'numeric', month: 'short', day: 'numeric' })
        : '';
      badge.textContent = '🏷️ ' + rel.tag_name + (when ? ' · ' + when : '');
      badge.hidden = false;
    }

    var assets = rel.assets || [];
    document.querySelectorAll('[data-asset]').forEach(function (b) {
      var suffix = b.getAttribute('data-asset');
      // endsWith keeps .tar.gz from colliding with .gz and skips SHA256SUMS.
      var hit = assets.filter(function (a) { return a.name.endsWith(suffix); })[0];
      if (!hit) return;
      b.href = hit.browser_download_url;
      var slot = document.querySelector('[data-size="' + suffix + '"]');
      if (slot) slot.textContent = size(hit.size);
    });
  }

  try {
    var cached = JSON.parse(sessionStorage.getItem(CACHE_KEY) || 'null');
    if (cached && Date.now() - cached.t < CACHE_MS) { apply(cached.d); return; }
  } catch (e) {}

  fetch('https://api.github.com/repos/' + REPO + '/releases/latest',
        { headers: { Accept: 'application/vnd.github+json' } })
    .then(function (r) { return r.ok ? r.json() : Promise.reject(r.status); })
    .then(function (d) {
      apply(d);
      try { sessionStorage.setItem(CACHE_KEY, JSON.stringify({ t: Date.now(), d: d })); } catch (e) {}
    })
    .catch(function () { /* no release yet, offline or rate-limited: keep defaults */ });
})();
