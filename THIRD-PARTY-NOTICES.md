# Third-party notices

## Part of the released program
- **XboxRecomp** (https://github.com/sp00nznet/xboxrecomp, v0.14.0, commit eb0aa1d): the static
  recompiler and runtime the port is built with. MIT License, Copyright (c) 2026 sp00nz.
- **D3DCompiler_47** (`d3dcompiler_47.dll`, the HLSL shader compiler): Microsoft, redistributed
  under the Windows SDK license (Redist\D3D). Shipped so that Proton and Wine use it instead of
  their own compiler.
- **SDL2** 2.30.9 (`SDL2.dll`, PlayStation, Switch and other controllers): zlib License,
  Copyright (C) 1997-2024 Sam Lantinga.
- **Mountains of Christmas** font (`assets/fonts`, the settings menu): Apache License 2.0,
  Font Diner; the license is in `assets/fonts/LICENSE.txt`.

## The game itself
The Nightmare Before Christmas: Oogie's Revenge is (c) 2005 Touchstone Pictures / Disney, developed by
Capcom and TOSE. The program contains the game's Xbox code, translated to native code by the recompiler.
The game's art, sound, films, levels and text are not included: they are copied from the player's own
disc image on the first start, and the settings menu's frames and buttons are read from those files at
run time. The screenshots in `docs/images` and `docs/*.png` show the game running in this port.

## Behaviour references
xemu and Cxbx-Reloaded (GPL/LGPL) were used to understand how the console behaves; none of their
code is included. Ideas from another Oogie's Revenge port (OogiesRevengeRecompiled, GPL-3.0) were
taken as ideas only; none of its code is included.
