/* FBNeo Launcher — game catalog.
 *
 * Static page, dynamic data: catalog-data.json and manifest.json are
 * generated on the homelab from the published FBNeo DAT files (see
 * PLAN-fbneo-web.md) and fetched cross-origin from here. This file never
 * talks to any backend of its own — it only filters/renders data already
 * sitting in memory.
 *
 * Release-type classification mirrors src/Game.h exactly (is_hack/
 * is_homebrew/is_bootleg/is_prototype/is_original), so a game is tagged the
 * same way here as in the desktop app. Keep the two in sync if the rules
 * change.
 *
 * Layout mirrors the desktop app's three panes (filter tree | list |
 * detail), minus what only makes sense with a local, per-user library:
 * no "Available/Missing" ROM status (nobody visiting has scanned anything),
 * no Favorites (no accounts), no Sources filter yet (not extracted from the
 * DAT). The detail pane is the same DOM/JS whether it renders as a static
 * third column (wide screens) or a full-screen overlay (narrow screens,
 * see catalog.css) — only the CSS positioning differs.
 */
(function () {
  'use strict';

  var DATA_URL = 'https://files.gcourtot.duckdns.org/dat/catalog-data.json';
  var MANIFEST_URL = 'https://files.gcourtot.duckdns.org/dat/manifest.json';
  var ART_BASE = 'https://files.gcourtot.duckdns.org/artwork/';
  var DAT_BASE = 'https://files.gcourtot.duckdns.org/dat/';
  var ROMS_BASE = 'https://roms.gcourtot.duckdns.org/roms/';
  var PAGE_SIZE = 80;

  function previewUrl(g) { return ART_BASE + 'previews/' + encodeURIComponent(g.n) + '.png'; }
  function titleUrl(g)   { return ART_BASE + 'titles/'   + encodeURIComponent(g.n) + '.png'; }
  function datFileUrl(f) { return DAT_BASE + encodeURIComponent(f); }
  // Roms/<rf>/<n>.zip on the NAS, mirrored verbatim as the URL path — auth
  // (HTTP Basic, single shared account) is enforced server-side by nginx,
  // not here; this link is only reachable/openable by someone who already
  // has those credentials.
  function romUrl(g) { return ROMS_BASE + encodeURIComponent(g.rf) + '/' + encodeURIComponent(g.n) + '.zip'; }

  function humanSize(bytes) {
    if (!bytes) return '';
    if (bytes < 1024) return bytes + ' B';
    var kb = bytes / 1024;
    if (kb < 1024) return (kb >= 10 ? Math.round(kb) : kb.toFixed(1)) + ' KB';
    var mb = bytes / (1024 * 1024);
    return (mb >= 10 ? Math.round(mb) : mb.toFixed(1)) + ' MB';
  }

  var LANG = document.documentElement.lang || 'en';
  var CAT = (window.I18N && window.I18N[LANG]) || {};
  function t(key, fallback) { return CAT[key] !== undefined ? CAT[key] : fallback; }

  var TYPES = [
    { id: 'original',  fallback: 'Original' },
    { id: 'clone',     fallback: 'Clone' },
    { id: 'hack',      fallback: 'Hack' },
    { id: 'homebrew',  fallback: 'Homebrew' },
    { id: 'bootleg',   fallback: 'Bootleg' },
    { id: 'prototype', fallback: 'Prototype' },
  ];

  function classify(g) {
    var d = g.d || '';
    var isClone     = !!g.c;
    var isHack      = /\(hack/i.test(d);
    var isHomebrew  = /\(hb\)|\(hb,|\(hb /i.test(d);
    var isBootleg   = /bootleg/i.test(d);
    var isPrototype = /\(proto/i.test(d);
    var isOriginal  = !isClone && !isHack && !isBootleg && !isPrototype && !isHomebrew;
    var flags = [];
    if (isOriginal)  flags.push('original');
    if (isClone)     flags.push('clone');
    if (isHack)       flags.push('hack');
    if (isHomebrew)   flags.push('homebrew');
    if (isBootleg)    flags.push('bootleg');
    if (isPrototype)  flags.push('prototype');
    return flags;
  }

  var els = {
    count: document.getElementById('cat-count'),
    search: document.getElementById('cat-search'),
    systems: document.getElementById('cat-systems'),
    types: document.getElementById('cat-types'),
    manufacturers: document.getElementById('cat-manufacturers'),
    years: document.getElementById('cat-years'),
    aspects: document.getElementById('cat-aspects'),
    orientations: document.getElementById('cat-orientations'),
    reset: document.getElementById('cat-reset'),
    grid: document.getElementById('cat-grid'),
    empty: document.getElementById('cat-empty'),
    more: document.getElementById('cat-more'),
    modal: document.getElementById('cat-modal'),
    modalBackdrop: document.getElementById('cat-modal-backdrop'),
    modalClose: document.getElementById('cat-modal-close'),
    modalMedia: document.getElementById('cat-modal-media'),
    modalTitle: document.getElementById('cat-modal-title'),
    modalMeta: document.getElementById('cat-modal-meta'),
    modalBadges: document.getElementById('cat-modal-badges'),
    modalClone: document.getElementById('cat-modal-clone'),
    modalSpecs: document.getElementById('cat-modal-specs'),
    modalActions: document.getElementById('cat-modal-actions'),
    datList: document.getElementById('cat-dat-list'),
    lightbox: document.getElementById('cat-lightbox'),
    lightboxImg: document.getElementById('cat-lightbox-img'),
    lightboxClose: document.getElementById('cat-lightbox-close'),
  };

  var GAMES = [];
  var GAMES_BY_NAME = {};
  var activeSystems = new Set();
  var activeTypes = new Set();
  var activeManufacturers = new Set();
  var activeYears = new Set();
  var activeAspects = new Set();
  var activeOrientations = new Set();
  var filtered = [];
  var shown = 0;
  var selectedRow = null;

  function escapeHtml(s) {
    return String(s).replace(/[&<>"]/g, function (c) {
      return { '&': '&amp;', '<': '&lt;', '>': '&gt;', '"': '&quot;' }[c];
    });
  }

  function matches(g, query) {
    if (activeSystems.size && !activeSystems.has(g.s)) return false;
    if (activeManufacturers.size && !activeManufacturers.has(g.mf)) return false;
    if (activeYears.size && !activeYears.has(g.y)) return false;
    if (activeAspects.size && !activeAspects.has(g._aspect)) return false;
    if (activeOrientations.size && !activeOrientations.has(g.or)) return false;
    if (activeTypes.size) {
      var flags = g._flags;
      var hit = false;
      for (var i = 0; i < flags.length; i++) if (activeTypes.has(flags[i])) { hit = true; break; }
      if (!hit) return false;
    }
    if (query) {
      var hay = g._hay;
      for (var j = 0; j < query.length; j++) if (hay.indexOf(query[j]) === -1) return false;
    }
    return true;
  }

  function anyFilterActive() {
    return activeSystems.size || activeTypes.size || activeManufacturers.size ||
           activeYears.size || activeAspects.size || activeOrientations.size;
  }

  function applyFilters() {
    var raw = els.search.value.trim().toLowerCase();
    var query = raw ? raw.split(/\s+/) : [];
    filtered = GAMES.filter(function (g) { return matches(g, query); });
    shown = 0;
    selectedRow = null;
    els.grid.innerHTML = '';
    renderMore();
    els.empty.hidden = filtered.length !== 0;
    els.reset.hidden = !(anyFilterActive() || query.length);
    updateCount();
    if (filtered.length) openModal(filtered[0]); else els.modal.hidden = true;
  }

  function updateCount() {
    var tpl = t('catalog.count', '{n} games' + (filtered.length !== GAMES.length ? ' matching' : ' across {s} systems'));
    var sys = new Set(GAMES.map(function (g) { return g.s; }));
    els.count.textContent = tpl
      .replace('{n}', filtered.length.toLocaleString(LANG))
      .replace('{s}', sys.size);
  }

  function badgesHtml(g, limit) {
    var flags = limit ? g._flags.slice(0, limit) : g._flags;
    return flags.map(function (id) {
      var def = TYPES.filter(function (x) { return x.id === id; })[0];
      var cls = id === 'original' ? ' original' : '';
      return '<span class="cat-badge' + cls + '">' + escapeHtml(t('catalog.type.' + id, def.fallback)) + '</span>';
    }).join('');
  }

  function actionsHtml(g) {
    // Artwork already shows full-size above (click it for the lightbox) —
    // a redundant download link here just duplicated that.
    return (
      '<a href="' + romUrl(g) + '" rel="noopener" title="' + escapeHtml(t('catalog.rom.protected', 'Private — requires the access credentials')) + '">' +
        escapeHtml(t('catalog.dl.rom', 'ROM')) +
      '</a>'
    );
  }

  function row(g) {
    var el = document.createElement('div');
    el.className = 'cat-row';
    el.innerHTML =
      '<div class="cat-row-art"><img loading="lazy" alt="" src="' + previewUrl(g) + '" onerror="this.parentNode.textContent=\'🕹️\'"></div>' +
      '<div class="cat-row-title"><b>' + escapeHtml(g.d) + '</b><span>' + escapeHtml(g.n) + '</span></div>' +
      '<span class="cat-row-sys">' + escapeHtml(g.s) + '</span>' +
      '<span class="cat-row-year">' + escapeHtml(g.y) + '</span>';
    el.addEventListener('click', function () { openModal(g, el); });
    el._game = g;
    return el;
  }

  function renderMore() {
    var next = filtered.slice(shown, shown + PAGE_SIZE);
    var frag = document.createDocumentFragment();
    next.forEach(function (g) { frag.appendChild(row(g)); });
    els.grid.appendChild(frag);
    shown += next.length;
    els.more.hidden = shown >= filtered.length;
  }

  // ── Detail panel (static 3rd column on wide screens, overlay below the
  // breakpoint defined in catalog.css — same markup and JS either way) ──────
  function specRow(label, value) {
    if (!value) return '';
    return '<div class="cat-spec"><span>' + escapeHtml(label) + '</span><b>' + escapeHtml(value) + '</b></div>';
  }

  function romsHtml(g) {
    if (!g.r || !g.r.length) return '';
    var rows = g.r.map(function (r) {
      return '<tr><td>' + escapeHtml(r[0]) + '</td><td>' + humanSize(r[1]) + '</td><td>' + escapeHtml((r[2] || '').toUpperCase()) + '</td></tr>';
    }).join('');
    return (
      '<details class="cat-roms"><summary>' +
        escapeHtml(t('catalog.spec.roms', 'ROM files')) + ' (' + g.r.length + ')' +
      '</summary>' +
      '<div class="cat-roms-archive"><span>' + escapeHtml(t('catalog.spec.archive', 'Archive')) + '</span><b>' + escapeHtml(g.n) + '.zip</b></div>' +
      '<div class="cat-roms-scroll"><table>' + rows + '</table></div></details>'
    );
  }

  // Splits the translated "Clone of {n}" template around {n} so the parent
  // name can be a real clickable element while keeping each language's word
  // order intact (e.g. Japanese/Chinese put {n} before "clone").
  function renderClone(g) {
    els.modalClone.innerHTML = '';
    if (!g.c) return;
    var parts = t('catalog.cloneof', 'Clone of {n}').split('{n}');
    var parent = GAMES_BY_NAME[g.s + '|' + g.c];
    els.modalClone.appendChild(document.createTextNode(parts[0] || ''));
    if (parent) {
      var btn = document.createElement('button');
      btn.type = 'button';
      btn.className = 'cat-clone-link';
      btn.textContent = parent.d;
      btn.addEventListener('click', function () { openModal(parent); });
      els.modalClone.appendChild(btn);
    } else {
      els.modalClone.appendChild(document.createTextNode(g.c));
    }
    els.modalClone.appendChild(document.createTextNode(parts[1] || ''));
  }

  function openModal(g, rowEl) {
    if (selectedRow) selectedRow.classList.remove('active');
    selectedRow = rowEl || null;
    if (selectedRow) selectedRow.classList.add('active');

    var meta = [g.s, g.y, g.mf].filter(Boolean).join(' · ');
    els.modalMedia.innerHTML =
      '<div class="cat-modal-shot"><span>' + escapeHtml(t('catalog.dl.art', 'Artwork')) + '</span>' +
        '<img alt="" src="' + previewUrl(g) + '" onerror="this.parentNode.hidden=true"></div>' +
      '<div class="cat-modal-shot"><span>' + escapeHtml(t('catalog.title', 'Title screen')) + '</span>' +
        '<img alt="" src="' + titleUrl(g) + '" onerror="this.parentNode.hidden=true"></div>';
    els.modalTitle.textContent = g.d;
    els.modalMeta.textContent = meta;
    els.modalBadges.innerHTML = badgesHtml(g);
    renderClone(g);

    var resolution = (g.w && g.h) ? (g.w + ' × ' + g.h) : '';
    var driverLabel = g.ds ? t('catalog.driver.' + g.ds, g.ds) : '';
    els.modalSpecs.innerHTML =
      specRow(t('catalog.spec.system', 'System'), g.s) +
      specRow(t('catalog.spec.manufacturer', 'Manufacturer'), g.mf) +
      specRow(t('catalog.spec.rom', 'ROM name'), g.n) +
      specRow(t('catalog.spec.resolution', 'Resolution'), resolution) +
      specRow(t('catalog.spec.orientation', 'Orientation'), g.or ? t('catalog.orientation.' + g.or, g.or) : '') +
      specRow(t('catalog.spec.video', 'Video'), g.vt ? t('catalog.video.' + g.vt, g.vt) : '') +
      specRow(t('catalog.spec.aspect', 'Aspect ratio'), g._aspect || '') +
      specRow(t('catalog.spec.driver', 'Driver'), driverLabel) +
      romsHtml(g);

    els.modalActions.innerHTML = actionsHtml(g);

    els.modal.hidden = false;
  }

  function closeModal() {
    els.modal.hidden = true;
    if (selectedRow) selectedRow.classList.remove('active');
    selectedRow = null;
  }

  els.modalClose.addEventListener('click', closeModal);
  els.modalBackdrop.addEventListener('click', closeModal);
  document.addEventListener('keydown', function (e) {
    if (e.key !== 'Escape') return;
    if (!els.lightbox.hidden) { closeLightbox(); return; }
    if (!els.modal.hidden && window.matchMedia('(max-width: 980px)').matches) closeModal();
  });

  // ── Lightbox: click any artwork thumbnail to see it full scale ──────────────
  function openLightbox(src, alt) {
    els.lightboxImg.src = src;
    els.lightboxImg.alt = alt || '';
    els.lightbox.hidden = false;
  }
  function closeLightbox() { els.lightbox.hidden = true; els.lightboxImg.src = ''; }

  els.modalMedia.addEventListener('click', function (e) {
    var img = e.target.closest('img');
    if (!img) return;
    openLightbox(img.src, img.alt);
  });
  els.lightbox.addEventListener('click', closeLightbox);
  els.lightboxClose.addEventListener('click', closeLightbox);

  // ── Sidebar filters ────────────────────────────────────────────────────────
  function filterRow(container, key, label, count, activeSet) {
    var b = document.createElement('button');
    b.type = 'button';
    b.className = 'cat-filter-row';
    b.innerHTML = '<span>' + escapeHtml(label) + '</span><span class="n">' + count.toLocaleString(LANG) + '</span>';
    b.addEventListener('click', function () {
      b.classList.toggle('active');
      if (b.classList.contains('active')) activeSet.add(key); else activeSet.delete(key);
      applyFilters();
    });
    container.appendChild(b);
  }

  function buildFacet(container, keyFn, activeSet, sortByCount) {
    var counts = {};
    GAMES.forEach(function (g) {
      var k = keyFn(g);
      if (!k) return;
      counts[k] = (counts[k] || 0) + 1;
    });
    var keys = Object.keys(counts);
    keys.sort(sortByCount ? function (a, b) { return counts[b] - counts[a]; } : undefined);
    keys.forEach(function (k) { filterRow(container, k, k, counts[k], activeSet); });
  }

  function bindTypeChips() {
    [].slice.call(els.types.querySelectorAll('.cat-filter-row')).forEach(function (b) {
      b.addEventListener('click', function () {
        var key = b.dataset.type;
        b.classList.toggle('active');
        if (b.classList.contains('active')) activeTypes.add(key); else activeTypes.delete(key);
        applyFilters();
      });
    });
  }

  function bindCollapsibles() {
    [].slice.call(document.querySelectorAll('.cat-filter-head')).forEach(function (head) {
      head.addEventListener('click', function () {
        var open = head.getAttribute('aria-expanded') === 'true';
        head.setAttribute('aria-expanded', open ? 'false' : 'true');
        document.getElementById(head.dataset.target).classList.toggle('is-collapsed', open);
      });
    });
  }

  // Long facets (Manufacturers, Years, ...) get a search box instead of a
  // scroll-and-hunt list — filters the already-built rows in place, no rebuild.
  function bindFacetSearch() {
    [].slice.call(document.querySelectorAll('.cat-filter-search')).forEach(function (input) {
      var list = document.getElementById(input.dataset.filter);
      input.addEventListener('input', function () {
        var q = input.value.trim().toLowerCase();
        [].slice.call(list.children).forEach(function (row) {
          row.hidden = !!q && row.textContent.toLowerCase().indexOf(q) === -1;
        });
      });
    });
  }

  els.search.addEventListener('input', applyFilters);
  els.more.addEventListener('click', renderMore);
  els.reset.addEventListener('click', function () {
    activeSystems.clear();
    activeTypes.clear();
    activeManufacturers.clear();
    activeYears.clear();
    activeAspects.clear();
    activeOrientations.clear();
    els.search.value = '';
    [].slice.call(document.querySelectorAll('.cat-filter-row.active')).forEach(function (b) { b.classList.remove('active'); });
    applyFilters();
  });

  bindTypeChips();
  bindCollapsibles();
  bindFacetSearch();

  // ── DAT files panel ────────────────────────────────────────────────────────
  function buildDatList(manifestByFile) {
    var bySystem = {};
    GAMES.forEach(function (g) { if (!bySystem[g.s]) bySystem[g.s] = g.f; });
    var systems = Object.keys(bySystem).sort();
    els.datList.innerHTML = systems.map(function (sys) {
      var file = bySystem[sys];
      var info = manifestByFile[file];
      var size = info ? humanSize(info.size) : '';
      return (
        '<div class="cat-dat-row"><span><b>' + escapeHtml(sys) + '</b><span class="size">' + size + '</span></span>' +
        '<a href="' + datFileUrl(file) + '" rel="noopener">' + escapeHtml(t('catalog.dl.dat', 'DAT')) + '</a></div>'
      );
    }).join('');

    // Every DAT is regenerated by the same run, so one date badge next to
    // the section title is enough — no need to repeat it on every row.
    var dates = Object.keys(manifestByFile)
      .map(function (f) { return manifestByFile[f].date; })
      .filter(Boolean)
      .sort();
    var latest = dates[dates.length - 1];
    if (latest) {
      var d = new Date(latest);
      var formatted = d.toLocaleDateString(LANG, { year: 'numeric', month: 'short', day: 'numeric' });
      document.getElementById('cat-dat-updated').textContent =
        t('catalog.datUpdated', 'Updated {d}').replace('{d}', formatted);
    }
  }

  // ── Boot ─────────────────────────────────────────────────────────────────
  fetch(DATA_URL)
    .then(function (r) { if (!r.ok) throw new Error(r.status); return r.json(); })
    .then(function (d) {
      GAMES = d.games || [];
      GAMES.forEach(function (g) {
        g._flags = classify(g);
        g._hay = (g.d + ' ' + g.mf + ' ' + g.n).toLowerCase();
        g._aspect = (g.ax && g.ay) ? (g.ax + ':' + g.ay) : '';
        // Keyed by system too: short names aren't unique across DATs (e.g.
        // the same short name could exist on two different systems), and
        // cloneof always refers to a parent on the same system.
        GAMES_BY_NAME[g.s + '|' + g.n] = g;
      });
      buildFacet(els.systems, function (g) { return g.s; }, activeSystems, true);
      buildFacet(els.manufacturers, function (g) { return g.mf; }, activeManufacturers, true);
      buildFacet(els.years, function (g) { return g.y; }, activeYears, false);
      buildFacet(els.aspects, function (g) { return g._aspect; }, activeAspects, true);
      buildFacet(els.orientations, function (g) { return g.or; }, activeOrientations, true);
      applyFilters();

      // Separate failure domain on purpose: the DAT panel is a bonus, not
      // the catalog itself — losing it must not blank the "N games" count
      // and error out a page that otherwise loaded fine.
      fetch(MANIFEST_URL)
        .then(function (r) { return r.ok ? r.json() : []; })
        .then(function (manifest) {
          var byFile = {};
          (manifest || []).forEach(function (m) { byFile[m.name] = m; });
          buildDatList(byFile);
        })
        .catch(function () { /* DAT panel just stays empty */ });
    })
    .catch(function () {
      els.count.textContent = t('catalog.error', 'Could not load the catalog right now — please try again later.');
    });
})();
