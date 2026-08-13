# Why the board configs pin a fork of esp_tsdb, and how to move the pin

## One-line statement

The reference configs pin `RAR/esp_tsdb` at an immutable SHA that is upstream
`main` plus exactly three commits. This is a deliberate, maintained state, not
a stopgap waiting on a merge.

## What the fork actually contains

As of 2026-08-13 the pin is `ebfc360f00263ab90116ee3e556a9153ab4041a2`, which is
`zakery292/esp_tsdb` 2.3.0 (`209bcca`) with three commits on top and **nothing
behind** — it is a rebased topic stack, not a divergent history.

| Commit | What it does | Why we need it | Upstream |
|---|---|---|---|
| `dbf4ebf` | Writes the DB header to an alternating sidecar (`<db>.h0`/`.h1`) instead of in place at offset 0 | A snapshot took 21.4 s; 15.2 s of that was header rewrites. LittleFS overwrite cost is linear at ~20.4 ms/KB, so a byte-0 write rewrites the whole file. Now 633 ms median / 1,019 ms max over 132 commits on the rig, with no drift as the DBs fill. | [PR #6](https://github.com/zakery292/esp_tsdb/pull/6), open since 2026-08-09 |
| `3fb785f` | Adds `tsdb_peek_span` — read a database's time span without opening it | The Diagnostics page needs each DB's span; opening every DB to get it is the expensive path we just removed. | **Not submitted, by choice** (2026-08-13). Ours to carry indefinitely. |
| `ebfc360` | Adds `esp32p4` to the manifest's `targets` list | Manifest-only. Without it the component manager refuses to install on a P4; the code itself is target-agnostic. | [PR #4](https://github.com/zakery292/esp_tsdb/pull/4), open since 2026-07-05 |

Upstream's last commit was 2026-07-12, and `development` is identical to `main`.
PRs #1–#3 were merged in a single batch on 2026-07-05, so the maintainer works in
bursts. Treat a merge as welcome but unscheduled: **nothing in this project should
block on one.** In particular, the v2.0.0 release does not.

## Branch layout on the fork

| Branch | Role |
|---|---|
| `main` | Mirror of upstream `main`. Never commit here — it exists so that anyone landing on the fork sees the real upstream code. |
| `tigo/on-<upstream-version>` | The integration branch. Its name states the version it is rebased onto; its tip is the SHA the board configs pin. Currently `tigo/on-2.3.0`. |
| `feat/manifest-esp32p4`, `upstream/sidecar-header` | PR heads for #4 and #6. Do not delete these — deleting a PR's head branch closes the PR. |

One branch per upstream base, rather than one long-lived branch that gets
rebased in place, means a shipped pin's base is always readable from a branch
name and old pins never dangle.

## Moving the pin to a new upstream release

The stack is three commits and must stay that way — **rebase, never merge**. A
merge commit makes the stack unreadable and the next rebase painful.

```bash
git clone git@github.com:RAR/esp_tsdb.git && cd esp_tsdb
git remote add up https://github.com/zakery292/esp_tsdb.git && git fetch up

# 1. Mirror upstream, then branch the stack onto the new base.
git checkout main && git merge --ff-only up/main && git push origin main
git checkout -b tigo/on-<new-version> tigo/on-<old-version>
git rebase --onto up/main <old-upstream-sha>

# 2. Sanity-check the shape before trusting it: expect "0<TAB>3".
git rev-list --left-right --count up/main...HEAD

# 3. Verify on hardware, not by eye — see below.
git push origin tigo/on-<new-version>
```

Then update `ref:` in every board config that carries one (`esp32s3-atoms3r`,
`esp32p4-evboard`, `test-p4-tigomonitor`, `test-p4-ble-tigomonitor`) **and** in
the deployed rig config, which is a standalone file outside this repo.

**Verification is a rig run, not a compile.** A clean build proves the ref
resolved, nothing more. The sidecar change is in the flash hot path, so confirm
snapshot time is still sub-second in the History logs over several commits
before calling a new pin good.

## If upstream merges the PRs

Retiring the fork means dropping to a registry version — but only once *all
three* commits are upstream, and that is not currently on the table. Two of the
three are in open PRs; `tsdb_peek_span` is deliberately not submitted, so even
if both PRs merge tomorrow the stack shortens to one commit rather than
disappearing. Plan for the fork to stay.

## Open item

`idf_component.yml` still declares `version: "2.3.0"`, identical to upstream
despite the three extra commits, so a build's provenance is not self-describing.
`2.3.0+tigo.1` is valid semver build metadata and would fix that. Not done yet —
it touches the manifest that PR #4 also edits, so it is worth doing after that
PR resolves rather than creating a conflict now.

## See also

- [Saving History to Flash](https://rar.github.io/esphome-tigomonitor/guides/tsdb-integration/) — the user-facing cost model and the reason the sidecar change exists
- [`tsdb-flash-crash-issue.md`](tsdb-flash-crash-issue.md) — the separate flash-write crash investigation (cause was cabinet power, not the filesystem)
