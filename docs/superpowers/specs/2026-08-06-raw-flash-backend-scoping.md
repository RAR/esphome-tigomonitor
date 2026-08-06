# Raw-flash backend for esp_tsdb — scoping study

**Status:** scoping only. Not a proposal to build. No decision implied.
**Date:** 2026-08-06
**Question:** do we need littlefs, or can esp_tsdb write directly to a flash partition?

## Summary

It can, and the fit is better than expected — a ring buffer over NOR sectors
makes eviction and wear levelling both free. But it requires an **on-disk
format break**, and the performance case is already largely won by the sidecar
header (21.4 s → 196 ms + 757 ms per commit). Estimated cost is **2.5–3 weeks
of engineering plus a week of soak**.

The recommendation is to hold until the open crash has a diagnosed cause. If
the crash is flash- or cache-related, this becomes the fix and jumps the queue.
If not, it is a large investment in a number that has stopped hurting.

## Why littlefs is the source of the pain

Every storage problem this week traces to one littlefs property, not to flash:

- littlefs stores files as a CTZ skip-list of block *addresses*. Modifying byte
  N forces every block after N to be rewritten.
- Measured consequence: overwrite cost is **linear at ~20.4 ms/KB**; appends
  are flat at ~170 ms regardless of size.
- That is why a 512-byte header at offset 0 cost 15.2 s, why eviction falls off
  a cliff at ring wrap, and why the rolling-files design exists at all.

Raw flash has none of this shape. Its constraint is different: erase before
write, at 4 KB sector granularity, with bits only programmable 1→0.

## The fit is unusually good

esp_tsdb already writes in **1024-byte blocks** (`TSDB_BLOCK_SIZE`), so four
blocks land exactly in one 4 KB sector. Three properties follow:

**Eviction becomes free.** When the ring wraps into sector K you erase sector K,
which discards precisely the oldest records — which *is* eviction. The cliff
stops existing rather than getting cheaper.

**Wear levelling becomes free.** A rotating ring erases every sector equally.
That is the ideal NOR access pattern, and it is most of why we wanted littlefs.

**Commits become nearly free.** Records program into pre-erased slots with no
erase. Sector erases amortise to roughly one per `4 × records_per_block`
records — at 30-minute cadence, order once per week.

| | today (littlefs + sidecar) | raw flash |
|---|---|---|
| commit | 196 ms sys / 757 ms panels | ~1–5 ms |
| header write | 46 ms (sidecar) | ~0.1 ms (journal append) |
| worst case (offset 0) | 15,000 ms | ~30–50 ms (one sector erase) |
| eviction at wrap | cliff | free |

## The blocker: an on-disk format break

The write path is read-modify-write of a whole block:

```
tsdb_read_block() → set slot in RAM → tsdb_write_block()   # full 1024 B
```

Records written into *pre-erased* slots are pure 1→0 programs, so this is cheap
— except for `TSDB_BLOCK_COUNT`, a mutable counter in the block header.
Incrementing 1→2 requires setting a bit back to 1, which needs an erase. Left
as-is, every record write would force a 4 KB read-modify-erase-write (~30–50 ms),
which is *worse* than what we have now.

Three format changes resolve it, all standard NOR journal patterns:

1. **Block count** — drop the stored counter; derive it by scanning for the
   first slot with `timestamp == 0xFFFFFFFF`.
2. **Header** — generalise the sidecar into a journal: append 32-byte header
   snapshots sequentially into a 2-sector region (~128 per sector), erase and
   restart when full. Pure programs, no erase per commit.
3. **Index** — entries are write-once per stride today, so they program fine;
   but wrap overwrites them, so the index region needs its own ring or
   ping-pong.

All three are sound. All three break the format, which means migration is
mandatory rather than optional — the rig holds 8,000+ system records we care
about, with an `oldest_timestamp` going back to 2026-05-27.

## Coupling assessment

The seam is narrow. **142 stdio call sites**, and every one is
`fseek(file, offset, SEEK_SET)` + read/write — no directories, no filesystem
features. A four-call block interface covers the whole surface:

```c
read_at(off, buf, len) / write_at(off, buf, len) / erase(sector) / sync()
```

| file | sites | notes |
|---|---|---|
| `tsdb_core.c` | 87 | mechanical |
| `tsdb_migrate.c` | 34 | **needs redesign** — see below |
| `tsdb_write.c` | 10 | mechanical |
| `tsdb_query.c` | 4 | mechanical |
| `tsdb_index.c` | 2 | mechanical |

Keeping a **stdio backend alongside the flash one** means `host_test` keeps
working, so crash-consistency stays testable on the desktop instead of only on
the rig. Given what the sidecar taught us about power-fail edge cases, this is
the single most valuable property of the design.

`tsdb_migrate.c` is the exception. It does a streaming rewrite into a sibling
`.mig` file then an atomic `rename` — raw flash has neither. It needs a
different mechanism: write into a spare region, then flip a pointer in the
header journal.

## Effort

| Piece | Estimate | Risk |
|---|---|---|
| Block-device seam + two backends | 2 days | Low |
| Format changes (count, header journal, index ring) | 3–4 days | **High — design work** |
| Swap 142 call sites | 2 days | Low, mechanical |
| Host harness w/ erase semantics + power-cut injection | 2–3 days | **High — where surprises live** |
| Migration off littlefs (preserve existing data) | 2 days | Medium |
| Partition layout, multi-DB allocation | 1 day | Low (needs repartition) |
| Rig validation + soak | ~1 week wall-clock | — |

**≈ 2.5–3 weeks engineering, plus soak.**

## Recommendation

**Hold.** Two reasons.

The performance case is mostly spent. The sidecar already took commits from
21.4 s to under a second. Going to ~5 ms is a 200× improvement on a number that
is no longer a problem, and it costs a format break plus re-earning the
crash-safety properties littlefs gives us for free.

The case that *would* justify it is the unexplained reset at 23.5 h uptime
(2026-08-05 16:32:34). Raw flash cuts total flash-busy time by orders of
magnitude, so if the crash is flash- or cache-related this becomes the actual
fix rather than an optimisation. But that cause is currently **undiagnosed** —
there is no coredump partition and the boot banner was missed. A watcher is now
running to capture the reset reason on the next event.

Committing 3 weeks of storage-layer rewrite to maybe fix an undiagnosed crash
is backwards. Diagnose first.

## Knock-on

The rolling-files plan
(`docs/superpowers/plans/2026-08-04-panel-rolling-files.md`, 9 tasks) is a
littlefs workaround. If raw flash goes ahead, that plan is **deleted rather
than implemented**. Both should be held on the same signal.

## Open questions

- Does the reset reason implicate flash/cache? Decides everything above.
- Migration: in-firmware converter, or export/reflash/import? Affects the
  2-day estimate and whether a serial reflash is needed.
- One partition per database, or sub-allocation within the existing 3 MB
  `tsdb` partition? Separate partitions are cleaner but need a repartition,
  which is a two-step OTA.
