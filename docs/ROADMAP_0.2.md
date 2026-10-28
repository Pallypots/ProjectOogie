# Project Oogie 0.2: plan from the 0.1 feedback

Sources: the Reddit announcement of 0.1 (143 upvotes, 23K views, comments read
2026-10-27) and GitHub issue #1. Each item: who asked, what they reported, what
we know, what to do. Claims are Verified (we saw it) or Hypothesis.

## Promises already made in public (owner's replies)
- "Version 0.2 will have support for Steam Deck, Steam Machine etc." (to Clarityman)
- Look into the "Xbox XBE Recomp" approach as a next step (to MDM-808)
- An Android port after Windows (to Savannah-Nanas)

## Priority 1: Linux / Steam Deck / SteamOS (Proton)
| Report | Details |
|---|---|
| GitHub #1 (Bzshizzle) | Steam Machine (SteamOS, Proton): "doesn't even open" |
| Clarityman (Reddit) | Steam Deck: crashes on boot with every Proton version. Notes the other port crashes constantly too |
| blondejefazo (Reddit) | **Works** on CachyOS through Proton: played for an hour |

So Proton can run it (Verified by a user report), but it fails to start on SteamOS / the Deck. Hypotheses, to check in this order:
1. The first-start setup: a Windows file-open dialog and copying 3.5 GB from the disc image. Under Proton the dialog or the path handling may fail before the game starts.
2. The fixed low-memory reservation: the game's 64 MB memory is mapped at address 0x10000. Wine may already use low addresses on some systems, so the reservation fails. (Session 1 ran under Wine, but with a different setup.)
3. Direct3D 11 through DXVK on the Deck's GPU: feature level or a format the back end requires.
4. The access-violation trap the sound and USB emulation relies on (vectored exception handler) behaving differently under Wine.
5. SDL2.dll or XInput loading.

What to do:
- Ask the reporters for a log: `logs\play.log` next to the program and a Proton log (launch option `PROTON_LOG=1 %command%`, file `steam-<appid>.log` in the home folder). Without one we are guessing.
- Make the program write its log before anything else can fail, and catch startup failures with a clear message instead of closing silently.
- Make the memory reservation fall back to another address when the fixed one is taken (Hypothesis 2), and allow setup from an already-extracted `game` folder without the dialog.
- Test under Wine/Proton ourselves (a Linux machine or VM with Proton), then on a Steam Deck if one is available.
- Add a Steam Deck section to the README: how to add it as a non-Steam game, which Proton version, controller layout.

**Progress 2026-10-27:** four Proton startup causes found and fixed (KNOWN_ISSUES #81): backslash paths in the 0.1 zip, Wine's shader compiler, no log under Steam, a stack-walk crash in the freeze detector. Tested under Wine 10 in WSL2 (boot x3 and first-start setup, no crashes). Also tested through DXVK 3.1.1 (Proton's D3D11 path): boots to the title screen, picture correct, no crashes. Still to do: a real Steam Deck, Game Mode.

## Priority 2: weak PCs and performance
| Report | Details |
|---|---|
| MDM-808 | Uses a Celeron N3060 and an i3 3220. Ports made with "Xbox XBE Recomp" (Dead or Alive 3, DOA Xtreme, Black) run better on weak hardware than XboxRecomp ones. Will try ours on the i3 |
| blondejefazo | Runs very well on a modest PC; suggests XBE Recomp plus a Direct3D 12 back end as a next step |

What to do:
- Profile a weak-hardware case ourselves: force Low preset, limit the program to 2 cores, and measure (the casino and the spider boss are the heaviest).
- Research "Xbox XBE Recomp": what it does differently (code generation, runtime, GPU path), whether its approach or parts of it could speed up ours, and its licence. Research first, no switch of toolchain without measurements.
- Cheaper Low preset: also lower anisotropic filtering, skip the post-processing passes entirely, and look at the GPU command translation cost (the drawing thread was the busiest in the casino profile).
- A Direct3D 12 or Vulkan back end is a larger project; a Vulkan back end would also serve Linux and Android (see below).

**Progress 2026-10-27:** research in docs/XBE_RECOMP_RESEARCH.md (XboxXBERecomp is MIT; it replaces D3D8 and DirectSound at the API level, with a D3D12 renderer). Weak-PC test (two low-power cores): all cores 60 fps; two cores 9-29 fps, too noisy on this laptop to rank small changes. Fixed: D3D's vblank handler spun on the interrupt acknowledge (79% of a thread). The largest remaining cost is translating the GPU's command stream (6-6.6 s per 10 s of a weak core): the real fix is the HLE renderer idea, a large project, best tested on a real 2-core PC (MDM-808 offered an i3 3220). Update 22:xx: on a quiet system two low-power cores give about 49 fps mean in Casino Clash on Low (kk runs); the D3D KickOff flush spin is also removed (sub_001D76F0 override, no fps change, less CPU). Decision: no HLE renderer before real weak-PC reports on 0.2.

## Priority 3: bugs and known issues
- Owner checks still pending from 0.1 builds: spider boss (#77, #78), chapter card picture (#80), pad menu and stick (#79).
- #60 widescreen door transition missing its side pieces; #47 results screen skips announcer lines.
- Ask Reddit/GitHub players for their logs when anything goes wrong.

## Priority 4: languages and localisation
| Report | Details |
|---|---|
| iAnn57 (French) | Asks for French, from the European version |
| Vovker78 | Asks for tools to work with in-game text, fonts and audio, for fan translations |

What to do:
- Find out what the European (PAL) Xbox release contains: which languages, and whether its program (default.xbe) differs from the USA one. If only the data differs, the port could load another language's files; if the program differs, it needs its own recompilation and testing.
- Document the text, font and audio formats (docs/ASSET_FORMATS.md, docs/REVERSE_ENGINEERING.md) and add extract / rebuild tools, so translators can work through the existing mods folder.

## Later
- **Android** (promised): an ARM64 build of the recompiled code plus a Vulkan or OpenGL ES graphics back end, touch and controller input. A Vulkan back end shared with Linux makes this cheaper.
- **Frame rates above 60**: the game's logic runs per frame, so this needs the game's timing found and decoupled.
- Larger subtitles and more accessibility options.

## Community notes
- Mid-weekJarl (the other Oogie's Revenge recomp): glad to see this port out, and happy for people to use it so he can focus on his other projects. Friendly; worth staying in touch.
- Other comments: general excitement, "Medievil vibes", Halloween timing. Not development items: "Nightmare Kart" (unrelated game) and "DMC 2 on PC".

## Suggested order for 0.2
1. Startup robustness and logs (helps every other report), then the Proton fixes, tested under Proton.
2. Owner's pending checks and the quick bug fixes.
3. Weak-PC profiling and a cheaper Low preset; XBE Recomp research write-up.
4. PAL / language feasibility study.
Then release 0.2 with notes answering each public promise.
