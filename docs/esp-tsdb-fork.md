# The esp_tsdb fork: what it was, and why it is gone

## One-line statement

**Retired on 2026-08-31.** The board configs pin `zakery292/esp_tsdb^2.4.1` from
the Espressif component registry — no `source:`, no `ref:`, no fork. Everything
below is the record of why a fork existed for four weeks and what replaced it.

## What the fork carried

The pin was `RAR/esp_tsdb` at `ebfc360f00263ab90116ee3e556a9153ab4041a2` —
upstream 2.3.0 plus three commits, rebased, nothing behind.

| Commit | What it does | Where it ended up |
|---|---|---|
| `dbf4ebf` | Writes the DB header to an alternating sidecar (`<db>.h0`/`.h1`) instead of in place at offset 0 | Upstream via [PR #6](https://github.com/zakery292/esp_tsdb/pull/6), merged 2026-08-31, released in **2.4.0** |
| `ebfc360` | Adds `esp32p4` to the manifest's `targets` list | Upstream via [PR #4](https://github.com/zakery292/esp_tsdb/pull/4), merged 2026-08-31, released in **2.4.0** |
| `3fb785f` | Adds `tsdb_peek_span` — read a database's time span without opening it | **Nowhere. Never used.** See below. |

2.4.1 followed minutes later and adds only a PlatformIO manifest
(`library.json`), which is inert for an ESP-IDF build. The floor is 2.4.1 rather
than 2.4.0 simply because it is the newest release with no reason to prefer the
older one.

## Why `tsdb_peek_span` did not block the retirement

It was written for a Diagnostics feature that reports each database's time span
without paying to open it. That feature was built a different way, and
`tsdb_peek_span` was never called: `git log -S peek_span -- components/` returns
nothing, and the string does not appear anywhere in the firmware. It sat in the
pin for four weeks as the sole remaining justification for a fork, protecting a
call site that did not exist.

The lesson worth keeping: a fork's cost is not the diff, it is that the diff has
to be re-justified every time you look at it, and dead entries in it survive
because nobody re-reads a pin they are not changing.

## Two things that were wrong in the fork's own documentation

**The P4 rationale looked stale and was not.** Upstream commit `914074ef`
(2026-07-05) added `esp32p4` to the manifest, which made it look as though the
fork's copy had been redundant since July. It had not: the 2.2.0 release commit
rewrote the manifest and dropped the target again, so `esp32p4` is absent from
the v2.2.0 and v2.3.0 manifests and present only from v2.4.0. Checking a claim
like this against `main` is worthless — `main` already contains whatever you just
merged. Check it against the **releases**, which are what a version pin resolves
to.

**"Upstream works in bursts; nothing should block on a merge."** That was true
and remains good practice, but the resolution came from neither waiting nor
forking indefinitely — it came from getting commit access. Worth remembering as
an option next time rather than treating fork-forever as the only alternative to
waiting.

## What replaced it

```yaml
esp32:
  framework:
    type: esp-idf
    components:
      - zakery292/esp_tsdb^2.4.1
      - joltwallet/littlefs^1.16
```

`^2.4.1` is a floor, not a preference. Below 2.4.0 the header is rewritten in
place, and that failure is silent — no build error, no log line, just a device
spending ~20 s of every snapshot interval inside a flash write, growing worse as
the databases fill. Do not relax it to `^2` or to a bare version.

Carried in six board configs (`esp32s3-atoms3r`, `esp32p4-evboard`,
`esp32s3-lilygo-t-connect-pro-lite`, `esp32s3-waveshare-rs485-can`,
`test-p4-tigomonitor`, `test-p4-ble-tigomonitor`), in the Config Builder
(`site/boards.js`), and in the deployed rig config, which is a standalone file
outside this repo and has to be updated by hand.

## The fork repository

`RAR/esp_tsdb` still exists and should be left alone. Its `tigo/on-2.3.0` branch
is the shipped pin's history, and `feat/manifest-esp32p4` /
`upstream/sidecar-header` are the merged PRs' head branches — deleting a head
branch on a merged PR degrades the upstream PR page.

## Verifying a version bump

A clean compile proves the version resolved, nothing more. The sidecar write is
in the flash hot path, so confirm snapshot time is still sub-second in the
History logs over several commits on the rig before calling a new floor good.

To confirm what a build actually resolved, read the lockfile rather than the
config:

```
grep -A20 'zakery292/esp_tsdb' boards/.esphome/build/<name>/dependencies.lock
```

`version:` is the resolved release and `source: type: service` means it came from
the registry rather than git.

## See also

- [Saving History to Flash](https://rar.github.io/esphome-tigomonitor/guides/tsdb-integration/) — the user-facing cost model and the measurement behind the 2.4.1 floor
- [`tsdb-flash-crash-issue.md`](tsdb-flash-crash-issue.md) — the separate flash-write crash investigation (cause was cabinet power, not the filesystem)
