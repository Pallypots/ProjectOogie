# Roadmap

Follows the project instruction sheet, sections 9 and 13. A milestone is ticked only with evidence in `TEST_RESULTS.md`.

| # | Milestone | Status |
|---|---|---|
| 1 | Executable parsed | Done 2026-10-16 |
| 2 | Functions recovered | Done (10,322 + runtime seeds) |
| 3 | Machine code translated | Done (0 failed) |
| 4 | Generated code compiled | Done (MinGW cross-build) |
| 5 | Native executable linked | Done (`oogie_recomp.exe`) |
| 6 | Runtime initialized | Done: entry point runs, thread/heap/GPU init |
| 7 | First valid rendered frame | **Done** run4: game's own disc-error + Now Loading screens |
| 8 | Title screen operational | Not yet |
| 9 | Gameplay operational | Not yet |
| 10 | Campaign completable | Not yet |
| 11 | PC enhancements (widescreen, resolution, HD textures) | Deferred |

Phase mapping: Phases 1 to 4 complete; Phase 5 (game initialization) in progress: audio, data loading and first level-data reads work; blocked on wild function pointer after `door.stm`.
