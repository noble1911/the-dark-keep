# The Dark Keep: next phase

Five workstreams run in parallel, each in its own git worktree, then get merged. Balance comes
last, after the merge. Read this whole file before starting.

## What the user asked for

- Smarter heroes, upgrades, and the smaller open items from docs/GAME-PLAN.md.
- A level editor instead of hand-written new keep layouts. It will make the campaign's dungeons,
  and players can make their own maps.
- A reworked menu: choosing a difficulty and a class makes no sense for a dungeon master.
- Monsters from the whole game, unlocked by levelling up through the campaign.
- 20 campaign levels. They use random layouts for now; editor maps replace them later.

## Workstreams and who owns what

| Stream | Owns | New files |
|---|---|---|
| hero-ai | Hero and bot reactions to traps, bait, locks | none expected |
| upgrades | Upgrades for lairs, traps, spells, Keep Lord, bought with vault gold | keeper_upgrades.* |
| qol | Rebindable keys, patrol routes, drag-select and group rally, mid-game joiner fix | keeper_input.* if useful |
| campaign | Menus, 20 levels, rank, save file, the random-layout seed | keeper_campaign.*, menu code |
| roster | Monsters from the whole game, unlock ranks, costs, loading only what fits | keeper_roster.* |
| editor | Map format, loading a map into the keep, the editor itself | keeper_map.*, keeper_editor.* |

The campaign and roster work may be one stream; the table still says who owns each part.

## Shared contracts

These exist so the streams can merge. Don't change their meaning; add to them if you must, and say
so in your report.

- `Source/keeper/keeper_campaign.hpp`: `CurrentSetup()`, `SetCurrentSetup()`, `CampaignRank()`.
  Campaign fills in the real behaviour. Everyone else only reads them.
- **Roster:** `BuildKinds`, `NumBuildKinds` and `BuildKind` will change. Nobody else may assume there
  are 6 kinds, or that a kind index is stable, or hard-code the Trap's index. Use `IsLairKind()`.
  A kind is available when `CampaignRank()` reaches its unlock rank.
- **Maps:** when `CurrentSetup().mapFile` is empty the keep is generated from `CurrentSetup().seed`.
  When it is set, the file is loaded. Campaign level N uses `assets/keeper/campaign/level_NN.dkmap`
  if that file exists, otherwise a random layout, so editor maps can replace levels one at a time.
  Editor owns the loader; campaign owns choosing the path and seed.
- **Upgrades** last for the current game only. Saving them between games is done at integration.
- **Co-op:** the host stays authoritative. Anything that changes the simulation goes through
  `Send` / `Action` in keeper_game.cpp. Hosts must tell joiners the setup, rank and map.

## Merge rules

- Put new code in new files. Keep edits to keeper.cpp, keeper_game.cpp and engine files small, and
  mark engine edits `// [DARK KEEP]`.
- Add to enums and lists at the end rather than reordering them.
- Don't reformat code you aren't changing.
- Line endings: keeper files, docs and README are LF, engine files are CRLF, `Source/player.cpp` is
  mixed. Check with `git ls-files --eol` and preserve them. `Source/CMakeLists.txt` is CRLF.
- Keep balance numbers as named constants in one place. Balance is tuned after the merge.
- Don't edit README.md or docs/GAME-PLAN.md. List doc changes in your report.

## Building in a worktree

Use the PowerShell tool; CMake fails from Bash. Configure once, sharing the installed vcpkg packages:

```
$env:PATH = "$env:USERPROFILE\scoop\apps\cmake\current\bin;$env:PATH"
cmake -S . -B build -G "Visual Studio 18 2026" -DCMAKE_BUILD_TYPE=Release -DBUILD_TESTING=OFF -DCMAKE_TOOLCHAIN_FILE=C:\Users\rong0\vcpkg\scripts\buildsystems\vcpkg.cmake -DVCPKG_MANIFEST_INSTALL=OFF -DVCPKG_INSTALLED_DIR=E:/local/IdeaProjects/diablo-rts/build/vcpkg_installed
cmake --build build --config Release --target devilutionx -- /m:4 /p:CL_MPCount=2
```

The machine has 20 threads and 32 GB shared between all streams, so keep those limits. Never touch
the main checkout's files or its `build` folder, other than reading the vcpkg packages above.

## Hard rules

- **Do not launch the game.** The user plays other games on this PC and test windows steal focus.
  Compile only. List exactly what needs playtesting in your report.
- Commit to your worktree's branch only. Never push and never touch master.
- Commit messages end with `Co-Authored-By: Claude Opus 5.5 <noreply@anthropic.com>`.
- Don't kill any process you didn't start.
- ../diablo-switch is reference only. Hellfire stays mandatory. CI stays Windows and Switch only.
- Stop and report rather than guess if a contract above turns out to be impossible.

## Report

Finish with: what works, what you could not verify without playing, every edit to a shared file,
contract additions, doc changes needed, and the branch and commit hash.
