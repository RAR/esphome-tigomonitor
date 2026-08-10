---
name: writing-tigo-docs
description: Use when adding or editing any page under site/src/content/docs/, the README, CHANGELOG, or an engineering note in docs/ — covers which audience each destination serves, the frontmatter and sidebar wiring a new page needs, and the build steps that silently publish stale or fabricated content.
---

# Writing Tigo Monitor Docs

Three destinations, three audiences. Most doc mistakes here are a page written
for the wrong one.

| Destination | Reader | Voice |
|---|---|---|
| `site/src/content/docs/` | Someone installing this on their roof | Task-first, no ESPHome internals unless they must type them |
| `docs/*.md` (root) | Us, six months from now | Engineering notes — mechanism, measurements, dead theories |
| `CLAUDE.md` | An agent starting cold | Rules and invariants only, no tutorials |
| `CHANGELOG.md` | Someone deciding whether to upgrade | What changed for *them*, grouped Added/Fixed/Changed/Removed |

A finding worth keeping usually belongs in two of these in different words:
the mechanism in `docs/`, the consequence for the user in a guide.

## Adding a page to the docs site

A new `.md`/`.mdx` under `site/src/content/docs/guides/` is unreachable until
you wire it up. Both steps, same commit:

1. **Frontmatter** — `title` is required by Starlight. `description` is not,
   but write one anyway: it is the meta description and the social-card text.
   Five of the ten existing pages are missing it; don't add a sixth.
2. **Sidebar** — add an entry in `site/astro.config.mjs`. The three groups
   ("Setting it up" / "Once it's running" / "Reference") are ordered by when a
   first-time installer needs the page, not alphabetically. Put it where its
   reader is, not at the end.

Internal links are validated at build time by `starlight-links-validator` —
a broken link or anchor fails the build rather than shipping. Site-absolute
links need the base path (`/esphome-tigomonitor/guides/...`); relative links
don't.

## Two build steps that publish things you didn't look at

**`npm run build` regenerates screenshots.** `screenshots/capture.mjs` runs
first and rewrites `docs/images/*.png` — the committed README images. After any
build, `git status` will show PNG churn. Commit it or discard it, but decide;
don't leave it for the next person to guess at.

**Screenshots render `app.html` against `screenshots/fixtures.mjs`, not against
a device.** So:

- Change any `/api/*` JSON shape → update `fixtures.mjs` in the same commit.
  A stale fixture does not fail the build. It publishes a screenshot of the UI
  rendering `undefined`.
- Fixtures are published assets. No real serials, site IDs, MACs, SSIDs, or
  IP addresses — invent them.

Verify with `cd site && npm run screenshots` and actually open the PNGs.

## House voice

The guides explain the mechanism, then the number, then what to do. The
`tsdb-integration.md` cost table is the model: it says *why* a LittleFS
overwrite is linear before it says what that costs per commit, so the
recommendation follows from something the reader now understands.

- **Every number is measured.** "~20.4 ms/KB across a 190× size range" is
  publishable; "roughly a second" is not. If it wasn't measured on the rig,
  don't print a figure.
- **Retire caveats when they stop being true.** A `:::caution` saying a cause
  "isn't yet understood" outlives the investigation by default. When something
  gets solved, grep the docs for the old framing — it will still be there.
- **Admonitions are rare on purpose** — four in ~15,000 words. One genuine
  warning reads as a warning; six read as decoration.
- Second person for the reader ("you"), first-person plural for project
  decisions ("we pin a fork because…").

For the prose itself, **use `elements-of-style:writing-clearly-and-concisely`.**

## Common mistakes

| Mistake | Consequence |
|---|---|
| New guide, no sidebar entry | Page builds and is unreachable |
| API field renamed, fixtures not touched | Screenshots publish `undefined` |
| Build run, PNGs left uncommitted | Next commit carries unrelated image churn |
| Real MAC/serial in a fixture | Published to GitHub Pages permanently |
| Estimated number stated as measured | Someone sizes their flash budget on it |
| Solved problem still documented as open | Reader distrusts the rest of the page |
