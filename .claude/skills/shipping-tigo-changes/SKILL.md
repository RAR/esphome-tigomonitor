---
name: shipping-tigo-changes
description: Use when writing a commit message, adding a CHANGELOG entry, annotating a version tag, or drafting GitHub release notes for esphome-tigomonitor — covers the commit type/scope vocabulary in use, the evidence a body must carry, and how the same change is described differently in each of the three places.
---

# Shipping Changes

One change gets described in up to three places, for three readers. Say it
differently each time; never paste one into another.

| Where | Reader | Answers |
|---|---|---|
| Commit message | Whoever runs `git log -S` in a year | Why the obvious fix didn't work, and how we know this one does |
| `CHANGELOG.md` | Someone deciding whether to upgrade | What's different for them, in one to three sentences |
| Release notes | Someone who just clicked the tag | Should I take this, and do I have to do anything |

## Commit messages

```
type(scope): subject, lowercase, no trailing period (#NN)

Body wrapped at 72 characters.
```

**Types** — the vocabulary already in use, in frequency order:
`fix`, `feat`, `docs`, `style`, `chore`, `perf`, `test`, `ci`, `build`,
`revert`, and `release` for version bumps. Don't invent more. `diag` is a
*scope* (`feat(diag):`), not a type.

**Scope** is the thing that changed as a reader would name it — a component
(`tigo_server`, `history`, `wizard`), a board (`p4`), or a subsystem
(`tsdb`, `ble`). Omit it rather than stretch for one.

**Subject** carries the consequence, not just the action. An em-dash clause
is the house way to add it:

> `fix(p4): put the P4 on the sidecar pin too — it was stuck at esp_tsdb 2.0.3 (#46)`

Append `(#NN)` when squash-merging a PR.

**Body** — 87% of non-merge commits have one; the good ones follow this arc:

1. What was actually wrong, in terms of the mechanism
2. Why the obvious fix doesn't work (skip if there wasn't one)
3. What was done instead
4. **Verification, with numbers.** This is the part that gets skipped and
   the part that matters most later.

Verification means an observation, not an intention. "Verified: P4 compiles
against the new ref and the fetched dependency is the fork — not a cached
resolve, flash moved 1,432,902 → 1,437,230 bytes" is evidence. "Should work
on P4" is not. If you didn't run it, say what you didn't run.

End with the session trailer (`Claude-Session: …`).

## CHANGELOG entries

Keep a Changelog + SemVer. `## [Unreleased]` stays at the top, empty between
releases. Group in Keep a Changelog order: **Added, Changed, Deprecated,
Removed, Fixed**.

An entry is a **bold lede sentence, then one to three sentences.** Mechanism
deep enough to be trusted, then a link out for the rest:

```markdown
- **Writing history got about 30× faster.** Snapshots locked the filesystem
  for ~21 s; they now take 0.63 s median, 1.02 s worst, measured over 132
  commits on the reference rig. The cause was a header rewrite at offset 0
  forcing LittleFS to rewrite every later block — see
  [Saving History to Flash](...) for the cost model. (#45)
```

- **The lede is the user's symptom**, not our diagnosis. "Opening the web UI
  could reboot the device" — not "added a LittleFS mutex."
- **Numbers are measured or absent.** Cite where they came from.
- **No internal symbol names in the lede.** They're fine in the body when
  the reader would type them (`history_interval`, `execute_from_psram`).
- **Say what the reader must do**, if anything: rebuild a config, re-pin a
  ref, or nothing at all.
- Add the `[X.Y.Z]:` link reference at the bottom. These currently stop at
  1.1.0 — everything since is missing.
- **Before tagging, read the section end to end for duplicates.** rc.1
  shipped the same crash-fix entry twice.

## Releases

1. Move `## [Unreleased]` entries under `## [X.Y.Z] - YYYY-MM-DD`, add the
   link reference, commit as `release: vX.Y.Z`.
2. Annotated tag `vX.Y.Z`. First line of the annotation is `vX.Y.Z` —
   with the `v`. The body is the maintainer's summary: what's in it, and
   what's still open.
3. GitHub release titled `vX.Y.Z — short descriptor`
   (`v1.4.5 — frame parsing, crash & memory fixes`). Bare tag names as
   titles tell a reader nothing on the releases page.
4. `--prerelease` for anything with `-rc`/`-beta`, and say in the first
   line what keeps it one.

Release notes are not the changelog. They open with who this is for and
what they must do, then the detail:

```markdown
Second release candidate for 2.0.0. Same as rc.1 except that the
**ESP32-P4 now gets the faster history writes too**.

## What changed
## If you run a P4          ← the action, with the exact YAML to paste
## Known dependency         ← what keeps this a candidate
```

Link back to the previous release rather than restating it.

## Common mistakes

| Mistake | Consequence |
|---|---|
| Commit body with no verification line | Nobody can tell later whether it was tested or hoped |
| CHANGELOG entry naming the fix, not the symptom | Reader can't tell if it affects them |
| Release notes pasted from the CHANGELOG | Buries the one action the reader must take |
| Bare `vX.Y.Z` release title | Releases page reads as a list of numbers |
| Missing `[X.Y.Z]:` link reference | Broken version link, unnoticed for nine releases |
| `chore(release):` and `release:` both used | `git log --grep '^release'` misses half the tags |
