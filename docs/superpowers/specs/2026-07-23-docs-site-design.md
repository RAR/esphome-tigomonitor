# Documentation + Config Builder Site — Design

**Date:** 2026-07-23
**Status:** Approved (brainstorming)
**Branch:** `feat/docs-site`

## Goal

Turn the standalone config-wizard Pages site into a full **documentation home** built
with Astro + Starlight, with the existing config builder folded in as one page. The
site becomes the project's front door: rendered docs (nav, search, dark mode) plus the
interactive YAML builder.

## Decisions (locked during brainstorming)

1. **Site scope:** Docs home + wizard. A proper docs site (landing page, sidebar nav,
   all `docs/` markdown rendered) with the config builder as one section.
2. **Build approach / SSG:** Astro + **Starlight** (real SSG, Node ecosystem, one
   toolchain).
3. **Docs source of truth:** Move `docs/*.md` **into** the Starlight content collection.
   Single canonical copy. Root `README.md` links get repointed to the hosted site. The
   old `docs/*.md` user docs are deleted.
4. **Project layout:** The Astro project **replaces `site/`**.
5. **Landing page:** Starlight **splash** hero.
6. **Old wizard `index.html`:** **fully deleted** once the Astro page reaches parity.

## Architecture

An Astro + Starlight site lives in `site/`, deployed to GitHub Pages at
`https://rar.github.io/esphome-tigomonitor/` (base path `/esphome-tigomonitor`, no
custom domain). Starlight owns docs navigation, full-text search, dark mode, and the
landing page. The config wizard is a single Starlight page.

**Preserve what works.** The wizard's tested *core logic* stays byte-for-byte where it
is at `site/` — `boards.js`, `rules.js`, `yaml.js`, and `lib/apikey.mjs`,
`lib/yaml-extract.mjs` — along with all 6 `test/*.test.mjs` files. Astro layers *around*
these files rather than moving them, so `node --test` keeps passing unchanged (crucially
`drift.test.mjs`, which reaches `../../boards/*.yaml`, keeps its relative paths valid).
Only the DOM/UI layer is rebuilt: `index.html`, `style.css`, and `wizard.js`'s glue are
absorbed into an Astro component.

## Repository layout (after)

```
site/
  package.json              # NEW: astro + @astrojs/starlight
  astro.config.mjs          # NEW: site/base + starlight() + sidebar
  tsconfig.json             # NEW
  boards.js rules.js yaml.js wizard.js   # UNCHANGED wizard core
  lib/  test/                            # UNCHANGED (node --test stays green)
  src/
    content/
      config.ts             # Starlight content collection schema
      docs/
        index.mdx           # NEW splash landing page
        guides/             # the 7 docs moved in (lowercased + title frontmatter)
          configuration.md
          wiring.md
          uart-optimization.md
          home-assistant.md
          web-server.md
          tsdb-integration.md
          troubleshooting.md
        config-builder.mdx  # imports and renders <ConfigWizard/>
    components/
      ConfigWizard.astro    # form markup + client <script> importing ../../boards.js …
    assets/                 # docs/images moved here
```

`docs/superpowers/` (specs + plans) is internal tooling and stays put — it is **not**
part of the public docs site and is not moved into the content collection.

The following files are **deleted** after parity: `site/index.html`, `site/style.css`,
and the 7 `docs/*.md` user guides (their content now lives under
`site/src/content/docs/guides/`).

## Docs migration

- The 7 user docs (`CONFIGURATION.md`, `WIRING.md`, `UART_OPTIMIZATION.md`,
  `HOME_ASSISTANT.md`, `WEB_SERVER_README.md`, `tsdb-integration.md`,
  `TROUBLESHOOTING.md`) move to `site/src/content/docs/guides/` with lowercased kebab
  filenames and a `--- title: … ---` frontmatter block each.
- Internal cross-links between docs are rewritten to Starlight routes (e.g.
  `./TROUBLESHOOTING.md` → `/guides/troubleshooting/`).
- Image references repoint to `site/src/assets/` (images moved from `docs/images/`).
- Root `README.md` stays for the GitHub repo page; its links into `docs/*.md` are
  repointed to hosted-site URLs.

## Wizard page

`ConfigWizard.astro` reproduces the form fields and preview `<pre>` from today's
`index.html`, styled to sit inside the Starlight layout. Its client `<script>` reuses
the existing `wizard.js` logic against the same element IDs, preserving every behavior:
board selection with constraint sync, live YAML preview, copy-to-clipboard, download
config, download `secrets.yaml`, and in-browser API-key generation. No JS framework
(no React/Preact) — a plain Astro client island. **Behavior parity with the current
wizard is the acceptance bar.**

## Landing page

`index.mdx` uses Starlight's `splash` template: project title, tagline, and hero CTAs
to *Get Started* (wiring/configuration), *Build a Config* (the wizard page), and the
GitHub repo, plus a short feature overview. Default Starlight theme with an accent
color; no custom theme work.

## CI / deployment

Rewrite `.github/workflows/pages.yml`:

```
build:
  - checkout
  - setup-node 22
  - working-directory site: npm ci
  - working-directory site: node --test test/*.test.mjs   # wizard logic gate (unchanged)
  - working-directory site: npm run build                 # astro build → site/dist
  - upload-pages-artifact  path: site/dist
deploy:
  - deploy-pages
```

Astro config: `site: 'https://rar.github.io'`, `base: '/esphome-tigomonitor'`. Starlight
fails the build on broken internal doc links (free link-checking). Workflow trigger
paths stay `site/**` (docs now live under `site/`).

## Testing

- **Wizard logic:** existing `node --test` suite in `site/test/` runs in CI before the
  Astro build and must stay green — this is the parity guarantee for the ported wizard.
- **Docs integrity:** `astro build` (Starlight) fails on broken internal links; a
  successful build gates deploy.
- **Manual:** spot-check the built `dist/` served under the `/esphome-tigomonitor/` base
  — sidebar, search, dark mode, and full wizard round-trip (preview + downloads).

## Success criteria

1. `cd site && npm run build` produces a `dist/` that works under the
   `/esphome-tigomonitor/` base path.
2. All 7 docs render with a working sidebar and full-text search; the splash landing
   page presents the project with CTAs to *Get Started*, *Build a Config*, and GitHub.
3. The wizard page behaves identically to today, proven by the untouched `node --test`
   suite plus a manual round-trip.
4. GitHub Pages deploys via the updated workflow on push to `main`.
5. Root `README.md` links resolve to the live site.

## Out of scope (YAGNI)

Versioned docs, i18n, a blog, custom theming beyond Starlight defaults + an accent
color, and any rewrite of the wizard core into a JS framework.
