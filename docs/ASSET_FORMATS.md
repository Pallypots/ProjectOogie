# Asset Formats (original Xbox disc)

Inventory: `docs/game_inventory.tsv` (723 entries). Headers read from the XISO on 2026-10-16. **Verified** = seen in file bytes.

| Ext | Count | Total | Magic | Notes |
|---|---|---|---|---|
| .xwb | 113 | 1514.8 MB | `WBND` v3 | XACT wave banks (Verified magic). Played by the game's recompiled XACT engine. |
| .xsb | 105 | 0.2 MB | `SDBK` v0x0B | XACT sound banks (cue tables). |
| .bik | 24 | 1363.8 MB | `BIKi` | Bink video, revision i. Decoded by the recompiled BINK library sections. Files: 01Sboogie..06lastboogie, mv_00..mv_0f, mv_roll, mv_title. |
| .lz | 298 | 279.5 MB | starts `02 ..` | `stm_data/NNNN.lz`. Compressed. **Hypothesis**: hold the models (.bmd) and textures (.pig) the ResHax thread describes. Compression scheme not yet identified. |
| .stm | 67 | 173.3 MB | `@STM` | Stream/level data containers in `stm_data/`. |
| .inf | 1 | 6 KB | `@INF` | `data.inf`, a table of 4-hex-digit IDs (0000, 0001, ... 0400 ...). **Hypothesis**: index of stage/area IDs. |
| .xbx | 2 | 14 KB | `XPR0` | TitleImage / SaveImage (Xbox resource, dashboard icons). |
| .bin | 2 | 20 KB | zeros at start | `XACT_data/effect/*.bin`, DSP effect images. |
| .txt | 1 | 22 B | `_CAPCOM_CAPCOM_C...` | SERIAL.TXT |

## Implication for the port
Because the XBE contains its own XACT, Bink and D3D code, the port does not need host decoders for .xwb/.bik. The original code reads these files through the kernel file API, which XboxRecomp maps to `game/`. Format work is only needed for debugging and for the deferred HD texture feature.

## To do
- Identify the .lz compression (look at the loader function that reads `stm_data/*.lz`).
- Check Durik256 `fmt_bmd.py` against data unpacked from .lz.
