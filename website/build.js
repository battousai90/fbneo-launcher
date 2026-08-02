#!/usr/bin/env node
/* Pre-renders one static page per language, plus the sitemap.
 *
 * Why this exists: the runtime language switcher is fine for visitors, but search
 * engines index a single URL and see only the English text. Giving each language a
 * real URL (/fr/, /ja/, …) with its own <title>, description and hreflang links is
 * what actually gets the translated pages ranked.
 *
 * index.html stays the English source and remains fully usable on its own; this
 * script only derives the others from it, so there is a single place to edit.
 *
 *   node website/build.js
 */
'use strict';

const fs = require('fs');
const path = require('path');

const DIR = __dirname;
const SITE = 'https://fbneo-launcher.netlify.app';
const LANGS = ['en', 'fr', 'es', 'de', 'pt', 'ja', 'zh', 'th'];

// Load the catalogues without a DOM: i18n.js assigns window.I18N, then touches the
// document. Stub just enough for the assignment to happen.
const win = {};
global.window = win;
global.document = {
  getElementById: () => null,
  querySelectorAll: () => [],
  documentElement: { dataset: {} },
  addEventListener: () => {},
};
// Node 21+ exposes a read-only `navigator`, so only define one if it is absent.
if (typeof globalThis.navigator === 'undefined') {
  Object.defineProperty(globalThis, 'navigator', { value: { language: 'en' }, configurable: true });
}
global.localStorage = { getItem: () => null, setItem: () => {} };
require(path.join(DIR, 'i18n.js'));
const I18N = win.I18N;

const source = fs.readFileSync(path.join(DIR, 'index.html'), 'utf8');

// Locate every data-i18n element and capture its inner HTML, honouring nesting.
function slots(html) {
  const out = [];
  const re = /<(\w+)([^>]*?)data-i18n="([^"]+)"([^>]*)>/g;
  let m;
  while ((m = re.exec(html))) {
    const tag = m[1], key = m[3], start = m.index + m[0].length;
    let depth = 1, i = start;
    while (depth > 0) {
      const next = new RegExp(`</?${tag}\\b`, 'g');
      next.lastIndex = i;
      const hit = next.exec(html);
      if (!hit) break;
      depth += html.startsWith(`</${tag}`, hit.index) ? -1 : 1;
      i = hit.index + tag.length + 2;
      if (depth === 0) out.push({ key, start, end: hit.index });
    }
  }
  return out;
}
const SLOTS = slots(source);

function alternates(current) {
  const links = LANGS.map(l => {
    const href = l === 'en' ? `${SITE}/` : `${SITE}/${l}/`;
    return `<link rel="alternate" hreflang="${l}" href="${href}">`;
  });
  // x-default points at the English page for languages we do not cover.
  links.push(`<link rel="alternate" hreflang="x-default" href="${SITE}/">`);
  return links.join('\n');
}

function render(lang) {
  const cat = lang === 'en' ? {} : (I18N[lang] || {});
  let html = source;

  // Replace from the end so earlier offsets stay valid.
  for (let i = SLOTS.length - 1; i >= 0; i--) {
    const s = SLOTS[i];
    const val = cat[s.key];
    if (val === undefined) continue;
    html = html.slice(0, s.start) + val + html.slice(s.end);
  }

  const title = cat['meta.title'] || 'FBNeo Launcher — Native arcade launcher & ROM manager for Linux';
  const desc  = cat['meta.desc']  ||
    'A fast, native GTK launcher and RomVault-style ROM manager for FinalBurn Neo on Linux. ' +
    'Browse 25,000 games instantly, verify every set against the DAT files, and rebuild broken sets automatically.';
  const esc = t => t.replace(/&/g, '&amp;').replace(/</g, '&lt;').replace(/"/g, '&quot;');
  const canonical = lang === 'en' ? `${SITE}/` : `${SITE}/${lang}/`;

  html = html
    // Strip anything a previous run injected, so building twice is a no-op rather
    // than stacking duplicate hreflang and og:locale tags.
    .replace(/^[ \t]*<link rel="alternate" hreflang="[^"]*" href="[^"]*">\r?\n/gm, '')
    .replace(/^[ \t]*<meta property="og:locale" content="[^"]*">\r?\n/gm, '')
    .replace(/<html lang="[^"]*">/, `<html lang="${lang}">`)
    .replace(/<title>[\s\S]*?<\/title>/, `<title>${esc(title)}</title>`)
    .replace(/<meta name="description" content="[^"]*">/, `<meta name="description" content="${esc(desc)}">`)
    .replace(/<meta property="og:title" content="[^"]*">/, `<meta property="og:title" content="${esc(title)}">`)
    .replace(/<meta property="og:description" content="[^"]*">/, `<meta property="og:description" content="${esc(desc)}">`)
    .replace(/<link rel="canonical" href="[^"]*">/, `<link rel="canonical" href="${canonical}">\n${alternates(lang)}`)
    .replace(/<meta property="og:type"/, `<meta property="og:locale" content="${lang}">\n<meta property="og:type"`);

  return html;
}

// ── Write ────────────────────────────────────────────────────────────────────
let written = [];

// English keeps the root URL; it only gains the hreflang block.
fs.writeFileSync(path.join(DIR, 'index.html'), render('en'), 'utf8');
written.push('index.html');

for (const lang of LANGS.filter(l => l !== 'en')) {
  const out = path.join(DIR, lang);
  fs.mkdirSync(out, { recursive: true });
  fs.writeFileSync(path.join(out, 'index.html'), render(lang), 'utf8');
  written.push(`${lang}/index.html`);
}

const today = new Date().toISOString().slice(0, 10);
const sitemap =
  '<?xml version="1.0" encoding="UTF-8"?>\n' +
  '<urlset xmlns="http://www.sitemaps.org/schemas/sitemap/0.9"\n' +
  '        xmlns:xhtml="http://www.w3.org/1999/xhtml">\n' +
  LANGS.map(l => {
    const loc = l === 'en' ? `${SITE}/` : `${SITE}/${l}/`;
    const alts = LANGS.map(a =>
      `    <xhtml:link rel="alternate" hreflang="${a}" href="${a === 'en' ? SITE + '/' : SITE + '/' + a + '/'}"/>`
    ).join('\n');
    return `  <url>\n    <loc>${loc}</loc>\n    <lastmod>${today}</lastmod>\n` +
           `    <changefreq>monthly</changefreq>\n    <priority>${l === 'en' ? '1.0' : '0.8'}</priority>\n` +
           `${alts}\n  </url>`;
  }).join('\n') +
  '\n</urlset>\n';
fs.writeFileSync(path.join(DIR, 'sitemap.xml'), sitemap, 'utf8');
written.push('sitemap.xml');

console.log(`Built ${written.length} files:`);
written.forEach(f => console.log('  ' + f));
