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

// Every pre-rendered page. `dir` is the URL path segment under the site root
// ('' for the landing page, 'catalog/' for the catalog); metaTitleKey/
// metaDescKey are the I18N keys carrying this page's <title>/<meta
// description> translation, kept distinct per page so they do not collide.
const PAGES = [
  { file: 'index.html', dir: '', metaTitleKey: 'meta.title', metaDescKey: 'meta.desc' },
  { file: 'catalog/index.html', dir: 'catalog/', metaTitleKey: 'catalog.meta.title', metaDescKey: 'catalog.meta.desc' },
];

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
function alternates(page, current) {
  const links = LANGS.map(l => {
    const href = l === 'en' ? `${SITE}/${page.dir}` : `${SITE}/${l}/${page.dir}`;
    return `<link rel="alternate" hreflang="${l}" href="${href}">`;
  });
  // x-default points at the English page for languages we do not cover.
  links.push(`<link rel="alternate" hreflang="x-default" href="${SITE}/${page.dir}">`);
  return links.join('\n');
}

function render(page, source, pageSlots, lang) {
  const cat = lang === 'en' ? {} : (I18N[lang] || {});
  let html = source;

  // Replace from the end so earlier offsets stay valid.
  for (let i = pageSlots.length - 1; i >= 0; i--) {
    const s = pageSlots[i];
    const val = cat[s.key];
    if (val === undefined) continue;
    html = html.slice(0, s.start) + val + html.slice(s.end);
  }

  // English falls back to whatever the source file already says, rather than a
  // copy hard-coded here: duplicating it meant editing the page's own <title>
  // had no effect, because every build silently put the stale copy back.
  const esc = t => t.replace(/&/g, '&amp;').replace(/</g, '&lt;').replace(/"/g, '&quot;');
  const srcTitle = (source.match(/<title>([\s\S]*?)<\/title>/) || [, ''])[1];
  const srcDesc  = (source.match(/<meta name="description" content="([^"]*)">/) || [, ''])[1];
  // Catalogue values are plain text and need escaping; the source is already HTML.
  const title = cat[page.metaTitleKey] ? esc(cat[page.metaTitleKey]) : srcTitle;
  const desc  = cat[page.metaDescKey]  ? esc(cat[page.metaDescKey])  : srcDesc;
  const canonical = lang === 'en' ? `${SITE}/${page.dir}` : `${SITE}/${lang}/${page.dir}`;
  // Cross-page links: elements carry a stable `data-href="home|catalog"`
  // marker right before their href, e.g. `data-href="catalog" href="/catalog/">`.
  // The regex below recomputes href from the marker every run, regardless of
  // what is currently there — unlike a one-shot placeholder, this survives
  // index.html being both the source AND (for English) the written output:
  // a placeholder would get consumed on the very first build and silently
  // freeze at the English path on every build after that.
  const home = lang === 'en' ? '/' : `/${lang}/`;
  const catalogHref = lang === 'en' ? '/catalog/' : `/${lang}/catalog/`;

  html = html
    // Strip anything a previous run injected, so building twice is a no-op rather
    // than stacking duplicate hreflang and og:locale tags.
    .replace(/^[ \t]*<link rel="alternate" hreflang="[^"]*" href="[^"]*">\r?\n/gm, '')
    .replace(/^[ \t]*<meta property="og:locale" content="[^"]*">\r?\n/gm, '')
    .replace(/<html lang="[^"]*">/, `<html lang="${lang}">`)
    .replace(/<title>[\s\S]*?<\/title>/, `<title>${title}</title>`)
    .replace(/<meta name="description" content="[^"]*">/, `<meta name="description" content="${desc}">`)
    .replace(/<meta property="og:title" content="[^"]*">/, `<meta property="og:title" content="${title}">`)
    .replace(/<meta property="og:description" content="[^"]*">/, `<meta property="og:description" content="${desc}">`)
    .replace(/<link rel="canonical" href="[^"]*">/, `<link rel="canonical" href="${canonical}">\n${alternates(page, lang)}`)
    .replace(/<meta property="og:type"/, `<meta property="og:locale" content="${lang}">\n<meta property="og:type"`)
    .replace(/data-href="home" href="[^"]*"/g, `data-href="home" href="${home}"`)
    .replace(/data-href="catalog" href="[^"]*"/g, `data-href="catalog" href="${catalogHref}"`);

  return html;
}

// ── Write ────────────────────────────────────────────────────────────────────
let written = [];

for (const page of PAGES) {
  const source = fs.readFileSync(path.join(DIR, page.file), 'utf8');
  const pageSlots = slots(source);

  // English keeps the root URL for each page; it only gains the hreflang block.
  const enOut = path.join(DIR, page.dir);
  fs.mkdirSync(enOut, { recursive: true });
  fs.writeFileSync(path.join(enOut, 'index.html'), render(page, source, pageSlots, 'en'), 'utf8');
  written.push(`${page.dir}index.html`);

  for (const lang of LANGS.filter(l => l !== 'en')) {
    const out = path.join(DIR, lang, page.dir);
    fs.mkdirSync(out, { recursive: true });
    fs.writeFileSync(path.join(out, 'index.html'), render(page, source, pageSlots, lang), 'utf8');
    written.push(`${lang}/${page.dir}index.html`);
  }
}

const today = new Date().toISOString().slice(0, 10);
const urls = [];
for (const page of PAGES) {
  for (const l of LANGS) {
    const loc = l === 'en' ? `${SITE}/${page.dir}` : `${SITE}/${l}/${page.dir}`;
    const alts = LANGS.map(a => {
      const href = a === 'en' ? `${SITE}/${page.dir}` : `${SITE}/${a}/${page.dir}`;
      return `    <xhtml:link rel="alternate" hreflang="${a}" href="${href}"/>`;
    }).join('\n');
    const priority = (page.dir === '' && l === 'en') ? '1.0' : '0.8';
    urls.push(
      `  <url>\n    <loc>${loc}</loc>\n    <lastmod>${today}</lastmod>\n` +
      `    <changefreq>monthly</changefreq>\n    <priority>${priority}</priority>\n` +
      `${alts}\n  </url>`
    );
  }
}
const sitemap =
  '<?xml version="1.0" encoding="UTF-8"?>\n' +
  '<urlset xmlns="http://www.sitemaps.org/schemas/sitemap/0.9"\n' +
  '        xmlns:xhtml="http://www.w3.org/1999/xhtml">\n' +
  urls.join('\n') +
  '\n</urlset>\n';
fs.writeFileSync(path.join(DIR, 'sitemap.xml'), sitemap, 'utf8');
written.push('sitemap.xml');

console.log(`Built ${written.length} files:`);
written.forEach(f => console.log('  ' + f));
