# HD Asset Enhancements (DEFERRED)

Not used during recompilation or bring-up. Original Xbox assets stay authoritative.

## Folders (in the owner's project-materials folder, kept separate from `ProjectOogie/game/`)
| Folder | Contents | Purpose |
|---|---|---|
| `The Nightmare Before Christmas - Oogie's Revenge (Modern)/SLUS-20860/` | 21 PNG files | PS2 HD replacement textures for PCSX2 |
| `Tim Burtons TNBC - Oogies Revenge (Modern Xbox Buttons)/SLUS-20860/` | 21 DDS files | Modern Xbox button-prompt textures for PCSX2 |

File names are PCSX2 texture-replacement hashes (`<hash>-<hash>-<fmt>`), keyed to the PS2 build SLUS-20860. Both folders have the same 21 hash names.

## Credits and source
Created by RetroGenerica, with help from Solid Snack. Original release: https://gbatemp.net/threads/the-nightmare-before-christmas-oogies-revenge-ps2-hd-texture-pack-xbox-button-texture-pack-100-complete-updated-2024.623200/post-10024146

**Permission (2026-11-04):** the owner reports having RetroGenerica's permission to use the packs with this port, with credit. Decision: the pack files are still not committed or shipped, because they are derived from the game's own textures (Disney/Capcom art), which the project never redistributes; the port ships only the match tables (`assets/texpacks/*.map`, keys and file names) and installs a pack from the zip the player downloads from the author. Credit goes in the README, the release notes and the Mods page.
Redistribution permission: **unknown, must be asked before any public release.**

## Compatibility notes
- PCSX2 hashes are computed from PS2 GS texture data and will not match Xbox textures. Mapping must be built by matching images (e.g. perceptual hash of the decoded Xbox texture vs the PNG/DDS).
- Xbox textures are swizzled/DXT; dimensions and alpha may differ from PS2.
- **Hypothesis**: hook point is the recompiled D3D texture upload (or the NV2A texture path in XboxRecomp), keyed by a hash of the Xbox texture bytes, with fallback to the original.

## Future tasks (after baseline gameplay)
1. Dump Xbox textures at runtime with content hashes.
2. Match against the 21 replacements; build a mapping table.
3. Add a config toggle with fallback to originals.
