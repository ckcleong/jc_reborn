# Story progression

The story is driven by a small amount of persistent state and the local wall
clock. This document describes the story-day cycle, the scenes attached to each
day, and how the clock and `currentDay`/`date` fields are determined and saved.

## Story days

There are **11 story days**, represented by `currentDay = 1..11`. After day 11,
the next date change wraps back to day 1.

The story table contains 63 scenes:

- **52 day-agnostic scenes** with `dayNo = 0`. These are eligible on every day.
- **11 day-specific scenes**, one for each story day.

`dayNo = 0` is a sentinel meaning "any day"; it is not a playable story day.
Every day's pool is the 52 generic scenes plus that day's one unique scene.
These are eligibility pools: `storyPickScene()` chooses randomly from the
scenes whose flags and `dayNo` match the current slot.

## Day-specific scenes

| Day | Scene | Flags |
|---:|---|---|
| 1 | `MARY.ADS` tag 2 | `ISLAND \| FINAL \| VARPOS_OK` |
| 2 | `JOHNNY.ADS` tag 2 | `ISLAND \| FINAL \| VARPOS_OK` |
| 3 | `SUZY.ADS` tag 1 | `FINAL \| FIRST` |
| 4 | `MARY.ADS` tag 3 | `ISLAND \| FINAL \| FIRST \| VARPOS_OK` |
| 5 | `MARY.ADS` tag 1 | `ISLAND \| FINAL \| VARPOS_OK \| LOWTIDE_OK` |
| 6 | `JOHNNY.ADS` tag 3 | `ISLAND \| VARPOS_OK \| LOWTIDE_OK` |
| 7 | `MARY.ADS` tag 4 | `ISLAND \| FINAL \| VARPOS_OK` |
| 8 | `MARY.ADS` tag 5 | `ISLAND \| LEFT_ISLAND \| FINAL \| FIRST \| NORAFT \| VARPOS_OK` |
| 9 | `SUZY.ADS` tag 2 | `FINAL \| FIRST` |
| 10 | `JOHNNY.ADS` tag 6 | `FINAL \| FIRST` |
| 11 | `JOHNNY.ADS` tag 1 | `FINAL \| FIRST` |

Day 6 is the only day-specific scene that is not marked `FINAL`; it enters the
normal scene pool for that day.

## Generic scenes

These 52 scenes have `dayNo = 0` and are available on every story day:

- `ACTIVITY.ADS`: tags 1, 4, 5, 6, 7, 8, 9, 10, 11, 12
- `BUILDING.ADS`: tags 1, 2, 3, 4, 5, 6, 7
- `FISHING.ADS`: tags 1–8
- `JOHNNY.ADS`: tags 4, 5
- `MISCGAG.ADS`: tags 1, 2
- `STAND.ADS`: tags 1–12, 15, 16
- `VISITOR.ADS`: tags 1, 3, 4, 5, 6, 7
- `WALKSTUF.ADS`: tags 1, 2, 3

For example, day 1's pool is the 52 generic scenes plus `MARY.ADS` tag 2.
Day 8's pool is the same 52 generic scenes plus `MARY.ADS` tag 5.

## Scene flags

| Flag | Meaning |
|---|---|
| `FINAL` | Can finish a story sequence |
| `FIRST` | Can start a sequence without a walk-in |
| `ISLAND` | Island is visible |
| `LEFT_ISLAND` | Island is placed on the left |
| `VARPOS_OK` | Random island position is allowed |
| `LOWTIDE_OK` | Low tide is allowed |
| `NORAFT` | Raft is omitted |
| `HOLIDAY_NOK` | Holiday props are suppressed |

`storyPickScene()` filters on these flags and on `dayNo`. In each story
sequence `storyPlay()` selects a final scene; unless that scene is `FIRST`, it
plays 6–19 intermediate scenes, then plays the final scene.

## `currentDay` and `date`

`currentDay` is the active story day. `date` is the local day-of-year at which
`currentDay` was last advanced; it is 0-based, matching `tm_yday`. Both are
stored as decimal integers in `$HOME/.jc_reborn` on PC, and in NVS on ESP32.

`storyUpdateCurrentDay()` runs at the start of every story sequence:

```c
cfgFileRead(&config);
today = getDayOfYear();

if (today != config.date) {
    config.date = today;
    config.currentDay += 1;
    hasChanged = 1;
}

if (config.currentDay < 1 || config.currentDay > 11) {
    config.currentDay = 1;
    hasChanged = 1;
}

if (hasChanged)
    cfgFileWrite(&config);

storyCurrentDay = config.currentDay;
```

Important consequences:

- The story day advances by exactly one when the local day-of-year changes.
- Any change in `date`, forwards or backwards, counts as a new day.
- A missing or empty config starts at `currentDay = 0`/`date = 0`, so the first
  sequence advances to day 1 and writes the new state.
- Day 11 advances to 12, which the range check wraps to day 1.
- The state is written only when a value changed.

## How `currentDay` is used

`storyCurrentDay` has three visible effects:

1. **Scene selection**: `storyPickScene()` accepts scenes where
   `dayNo == 0 || dayNo == storyCurrentDay`.
2. **Raft state**: `storyCalculateIslandFromScene()` maps the day to the raft:
   - `NORAFT` scenes draw no raft.
   - Days 1–2 draw raft stage 1; day 0 is a defensive fallback in the same
     branch.
   - Days 3–5 draw raft stages 2–4.
   - Days 6–11 draw the completed raft (stage 5).
3. **Day-specific scenes**: the one scene attached to each day becomes
   eligible for that day's pool.

## Wall clock

The clock helpers live in `utils.c`:

- `getDayOfYear()` — local `tm_yday`, used for story progression.
- `getHour()` — local hour, used for day/night selection.
- `getMonthAndDay()` — local `MMDD`, used for holiday selection.

`storyCalculateIslandFromDateAndTime()` uses these as follows:

- Night when `getHour() % 8` is 0 or 7.
- Halloween from October 29–31.
- St Patrick's Day from March 15–17.
- Christmas from December 23–25.
- New Year from December 29 through January 1.
- `VISITOR.ADS` tag 3 also suppresses holiday props through `HOLIDAY_NOK`.

### `JC_TESTTIME`

`JC_TESTTIME=MMDDHH` pins all three clock helpers to a fixed moment. The pinned
calendar is a non-leap year. When set, it takes precedence over the real clock
for the engine's date and hour helpers. This makes baseline story runs
reproducible.

On PC, the baseline runner sets `JC_TESTTIME=102014` (October 20, 14:00) and
uses a pinned `$HOME/.jc_reborn` with `currentDay=5` and `date=292`. Since
day-of-year 292 is October 20 in the pinned non-leap calendar, the run records
no day bump and stays on day 5.

On ESP32, `main.c` sets `JC_TESTTIME=102014` only when no valid clock is stored
in NVS. If the setup clock is valid, the real clock is used instead.

## PC persistence: `$HOME/.jc_reborn`

On PC, `config.c` reads and writes `$HOME/.jc_reborn`:

```text
currentDay=5
date=292
```

- `cfgFileRead()` parses `currentDay=` and `date=` lines, defaulting to 0/0 when
  the file is missing.
- `cfgFileWrite()` rewrites both lines.
- The user's normal `$HOME` persists the state between runs.
- Test runners point `HOME` at a temporary directory so the story state is
  isolated and reproducible.

## ESP32 persistence: NVS

The ESP32 resource partition is mounted read-only, so story state is stored in
NVS instead:

| item | value |
|---|---|
| namespace | `jc` |
| story-day key | `story_day` |
| story-date key | `story_date` |

`cfgFileRead()` on ESP32:

1. Tries to read `story_day`/`story_date` from NVS.
2. If both keys exist, uses those values.
3. Otherwise reads the seed file `$HOME/.jc_reborn` from the resource image
   (`HOME=/resources`) and writes the values to NVS.
4. If the seed file is also missing, defaults to 0/0.

`cfgFileWrite()` writes the new values to NVS and logs:

```text
story state updated in NVS: currentDay=<n> date=<n>
```

The seed file is therefore used only on first boot, or after NVS has been
erased. Later changes to the seed file do not override existing NVS state.

## Setup clock persistence

The ESP32 has no battery-backed RTC. The setup portal stores the wall clock in
the same `jc` NVS namespace:

| item | key |
|---|---|
| POSIX epoch | `epoch` |
| timezone offset in minutes | `tzmin` |

- `POST /set` applies the submitted time with `settimeofday()`, writes
  `epoch`/`tzmin`, and restarts.
- On boot, `setup_restore_time()` restores the last saved clock and timezone.
- During playback, `setup_snapshot_time()` saves the current time every
  ~10 minutes.

Because there is no RTC, a reset restores the **last saved snapshot**, not the
exact time at reset. Time spent powered off is not recovered, so the clock can
lag by the time since the last snapshot plus the power-off gap. If no valid
clock is stored, or setup is requested, the device enters setup mode.

Story-day state is separate from the clock snapshot: the story state is
persisted in NVS immediately whenever it changes, so day progression survives
reboots even if the clock snapshot is a few minutes old.

## Debugging

- `jc_cfg` logs NVS story-state updates.
- `jc_reborn` logs the NVS `currentDay`/`date`, the effective clock and the
  sound preference at story start.
- `jc_story` logs each ADS scene with its tag and flags.
- `resmgrCacheReport()` reports LRU hits/evictions when retention is enabled.
