# Career, rating and the win/lose screens (what the original does)

Reverse-engineering notes, 2026-10-16. All addresses are VAs in the shipped
`original/king.exe` (ImageBase 0x400000); the diagnostic names below are the
strings the binary passes to its own trace helpers, so they name the functions
the developers wrote.

## 1. The career world has no hard "you win"

* `ENV_GAME.CNF` (the career map) carries `playertypes`, `startposition` and the
  entity counters (`forsale 25`, `forhire 20`, `missions 10`, `fuel 10`,
  `semitrailers 28`, `containers 10`, `parkings 40`) - **no** scenario and no goal.
* `(scenario races)` exists only in **`MENV_GAME.CNF`** - the menu world that
  drives the attract/demo sequence.
* `ENV/dr.way` does contain 18 `start1_*` / `finish1_*` / `check_point*` nodes,
  but **no shipped script references them** (`grep -rn start1_ decrypted/` is
  empty): they are leftovers of the shared tile, not the career's goal.

So the career is **open-ended**: money, cargo, trucks and the *competition place*
below.  The win/lose art the port draws belongs to the race result (2).

## 2. The win/lose screens are the race result

Function entering at `0x51f600` (called on finishing a race):

```
edx = [0x6cec6c]; if (edx[0x524] == 0) goto tail     ; 0x51f623
eax = race[+0x194]                                   ; race state
if (eax == 0) goto tail                              ; 0x51f635
if (eax == 5) goto tail                              ; 0x51f63d
call 0x574530
eax = [0x67b75c]                                     ; the place / outcome global
if (eax == ebp) play "youwinSound"                   ; 0x51f65f
else if (eax == 0) play "youlostSound"               ; 0x51f6ae
tail: if (eax == 2) play "youwinSound"               ; 0x51f700
      else play "youlostSound"                       ; 0x51f708
```

* the prize is picked from `race+0x17c/0x180/0x184/0x188` (per-place money) into
  `race+0x190` depending on `[0x6cec98][0x180]` (the driver count >= 2), and the
  finish time is stored as a double at `race+0x198`;
* the screens themselves are ordinary ESC-family objects: the factory
  `0x10c9d0` allocates the whole screen set (`0x11eea0` = the 1600x1200 win/lose
  page, globals `0x6ceca8`) and `0x11fd70` is its button handler.

## 3. The competition place (the career's real progression)

Diagnostic names and their call sites give the whole chain:

| name | where it lives | what it is |
|---|---|---|
| `updateRating` | string `0x66e330`, referenced at `0x434d44` | writes each company's rating: raw value into `payload+0x300`, its share into `payload+0x2f8` (`0x41fed3`), aggregating `0x41fca0` (which uses `0x41b910` for classes 1/2 and returns 0 otherwise) |
| `calculateNewPlace(int& newPlace, double& mindelta)` | `0x427f40` | walks the player list (`0x68b990`), reads each one's payload (`0x40a9f0`), and keeps the best delta - i.e. decides the player's new *place* |
| `recalculateCompetitionPlace` | string `0x66daec`, referenced `0x428265`/`0x428277`/`0x4284ad`/`0x4284b9`/`0x42850f` | the wrapper that recomputes the place and applies it |
| `RACES_TO_TAKE_PLACE_IN` | string `0x66da90`, referenced `0x427e60` | the counter of races still to take part in |

The rating share (`payload+0x2f8`) is what `mafiaOnHim` reads through
`node+0x300` (see [original_ambush_load.md](original_ambush_load.md)): the mafia
turns hostile when the player's *share of the company rating* exceeds 0.2 and the
cargo is tagged.  So the same number that ranks the player drives the risk.

## 4. What this means for the port

* a "playthrough to the end" needs the competition place (`calculateNewPlace` /
  `recalculateCompetitionPlace`) and the race result path - not a single money
  threshold; the shipped career data defines no threshold at all;
* the port's win/lose screens (`src/ui/EscMenu.cpp`, screens `win`, `win1`,
  `win2`, `lose1..5`) currently have **no trigger**; wiring them means porting the
  race result (`[0x67b75c]`, `race+0x194`) or the final place;
* the port's own ambush path already consumes a boolean `formafia` proxy instead
  of the real rating share - this document is the missing link between the two.
