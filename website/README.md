# Landing page

Static site for [fbneo-launcher](https://github.com/battousai90/fbneo-launcher),
deployed on Netlify. No build step, no dependencies — plain HTML and CSS.

| File | Role |
|---|---|
| `index.html` | The whole page |
| `style.css`  | Theme lifted from the app's GTK stylesheets (`assets/style-*.css`) |
| `logo.svg`   | Copy of `assets/logo.svg` |
| `og.png`     | Social preview card, 1200×630 |

## Deploying

Netlify is configured by `netlify.toml` at the repository root: publish directory
`website`, no build command. Connect the repo once and every push to the default
branch redeploys.

Two short links are set up as redirects: `/download` and `/github`.

## Keeping it in sync with the app

The colours are the app's own, so when `assets/style-common.css` or
`assets/style-dark.css` change, update the `:root` block in `style.css` to match.

Download buttons all point at `/releases/latest`, so a new version needs no change
here — tag it and the release workflow does the rest.

## Local preview

```bash
python3 -m http.server -d website 8000   # then open http://localhost:8000
```

To regenerate `og.png` after editing the card, see the snippet in the project
history, or simply screenshot a 1200×630 page with headless Chromium.
