# The Dark Keep map format (.dkmap)

A `.dkmap` file is one keep level. The editor saves to a `maps` folder under the save directory;
campaign maps live in `assets/keeper/campaign/level_NN.dkmap`, and reach the game as loose files or
inside `devilutionx.mpq`, whichever the build makes, so the game finds them by asset name. See
docs/CAMPAIGN-MAPS.md. `Source/keeper/keeper_map.cpp` is the reference implementation.

All numbers are little-endian. There is no compression: a file is always about 9.7 KB.

## Layout

### Header (16 bytes)

| Offset | Type | Field | Notes |
|---|---|---|---|
| 0 | char[4] | magic | `DKMP` |
| 4 | u16 | version | 1 or 2: see Versions |
| 6 | u16 | flags | bit 0: playable, it passed validation when saved |
| 8 | u32 | payload size | must equal file size minus 16 |
| 12 | u32 | CRC-32 | of the payload (the zlib polynomial) |

### Payload

| Type | Field | Notes |
|---|---|---|
| u8 | tileset | 0 Cathedral, 1 Catacombs, 2 Caves, 3 Hell, 4 Nest, 5 Crypt |
| u8 | width | 40 |
| u8 | height | 40 |
| u8 | reserved | 0 |
| u8, u8 | entrance x, y | top-left megatile of the stairs' footprint; 255, 255 if not placed |
| u8, u8 | vault x, y | same |
| u32 | decoration seed | for the random wall and floor variants |
| u8, bytes | name | up to 40 printable ASCII characters |
| u8, bytes | author | up to 24 printable ASCII characters |
| u16 | object count | at most 64 in version 1, 80 in version 2 |
| 3 bytes each (v1), 4 (v2) | objects | u8 kind (see Objects and props), u8 x, u8 y as world tiles, and in version 2 u8 variant |
| 1600 bytes | layout | one per megatile, row by row: 0 rock, 1 floor, 2 door, 3 arch |
| 1600 bytes | tiles | one per megatile, row by row: the finished `dungeon` |
| 6400 bytes | transparency | one per world tile of the dungeon area, row by row: `dTransVal`, 0 to 127 |

World tile (x, y) of a dungeon is at megatile ((x - 16) / 2, (y - 16) / 2). The transparency grid
starts at world tile (16, 16).

## Which part does what

- **The layout is what the author made.** The editor loads it and edits it. Doors are layout cells:
  a door needs floor on two opposite sides and wall on the other two. In the Caves a door cell is instead on
  open floor in a straight stretch two or more cells wide, and a wooden fence crosses the cave there with the
  gate in it. Hell and the Nest have no doors.
- **The tiles and transparency are baked from the layout when the map is saved.** They are what the
  game loads, so a level plays exactly as it was saved and every co-op client builds the same one.
  Loading a map uses no random numbers.
- Door objects are not stored separately: as for generated levels, the game creates them from the
  door tiles. Barrels, chests, braziers and props are stored, and a map gets nothing else: no random
  objects. A barrel, chest or brazier on solid ground is skipped; a prop is judged by the rules below.
- The entrance and vault are the tileset's normal up and down stairs, stamped into the tiles. The
  game finds the vault through its usual stairs trigger.

## Objects and props

Kinds 0 to 2 are the version 1 objects and behave exactly as they always did: a barrel or chest can be
broken or opened, and they go anywhere on the floor. Kinds 3 and up are **props**: scenery that does
nothing. A prop can't be clicked, opened, broken or set off, and missiles fly through it, so it never
changes a fight. A solid prop is walked round, so the placement rules below keep it from changing where
heroes can go.

The variant byte holds the prop's look (its style, bits 0 to 6) and, for a wall prop, bit 7: clear if the
floor in front is at +x, set if at +y. Those are the two walls the camera sees. Anything else is refused.

| Kind | Name | Engine object | Styles | Light | Solid | Stands | Tilesets |
|---|---|---|---|---|---|---|---|
| 0 | Barrel | OBJ_BARREL | 1 | | yes | floor | all (breakable) |
| 1 | Chest | OBJ_CHEST1 | 1 | | yes | floor | all (opens) |
| 2 | Brazier | OBJ_L1LIGHT | 1 | radius 5 | yes | floor | Cathedral |
| 3 | Candles | OBJ_CANDLE2 | 1 | radius 5 | yes | floor | all |
| 4 | Skull fire | OBJ_SKFIRE | 1 | radius 5 | yes | floor | all |
| 5 | Wall torch | OBJ_TORCHL (+x), OBJ_TORCHR (+y) | 1 | radius 8 | no | on a wall | all but the Nest |
| 6 | Banner | OBJ_BANNERL, M, R | 3 | | yes | floor | Cathedral, Catacombs, Hell, Crypt |
| 7 | Skull pile | OBJ_SKPILE | 1 | | yes | floor | all |
| 8 | Crucified skeleton | OBJ_CRUX1 to 3 | 3 | | yes | floor | all |
| 9 | Book stand | OBJ_BOOKSTAND, frames 1 and 3 | 2 | | yes | floor | Cathedral, Catacombs, Crypt |
| 10 | Bookcase | OBJ_BOOKCASEL (+x), OBJ_BOOKCASER (+y) | 1 | | yes | against a wall | Cathedral, Catacombs, Crypt |
| 11 | Weapon rack | OBJ_WEAPONRACKN, frames 1 (full) and 2 | 2 | | yes | floor | Catacombs, Caves, Hell, Crypt |
| 12 | Armour stand | OBJ_ARMORSTANDN, frames 1 (full) and 2 | 2 | | yes | floor | Catacombs, Caves, Hell, Crypt |
| 13 | Headless body | OBJ_DECAP, frames 1 to 8 | 8 | | yes | floor | all |
| 14 | Tortured body | OBJ_TNUDEM1 to 4, OBJ_TNUDEW1 to 3 | 7 | | yes | floor | all |
| 15 | Impaled body | OBJ_TORTURE1, 2 (+x); OBJ_TORTURE3, 4 (+y) | 2 | | no | on a wall | all but the Nest |
| 16 | Blood fountain | OBJ_BLOODFTN | 1 | | yes | floor | all |
| 17 | Crypt candles | OBJ_L5CANDLE | 1 | radius 3 | yes | floor | Crypt |
| 18 | Angel statue | OBJ_ANGEL | 1 | | yes | floor | Cathedral, Catacombs, Crypt |

`x, y` is the tile the prop is stored on: for a wall prop, the wall tile. An impaled body facing +x is drawn
from the floor tile in front (its art is made for that), so it stands there in the level, without blocking it.
Every tileset has a light: braziers, candles, skull fires and torches, and the Crypt's own candles.

### Where a prop may stand

The game judges each prop in file order when it builds the level, with the doors, the version 1 objects and
the props before it already standing, and leaves out any that fails; the editor says why (the same check),
and a left-out prop is a warning. The rules depend only on the tiles and the objects, so every co-op client
leaves out the same ones.

- Not within a tile of either staircase's footprint, nor where something already stands.
- **On the floor:** on a walkable tile. A solid prop must also keep the heroes' ways as they were: every
  walkable tile round it must still reach every other one without it, by the steps heroes take (BuildField's,
  diagonals round corners included), so it can't cut anything off or fill a doorway; and it must leave at least
  two tiles of open floor beside it along both axes, so it never narrows a way through to one tile or puts a
  pillar in the middle of a three-wide hall. In a room it only adds a step or two to a walk round it.
- **Against a wall** (bookcase): all of that, and a straight wall right behind it.
- **On a wall:** the tile and its two neighbours along the wall are solid, none is a door, and the tile in front
  is bare floor. The editor snaps a click on the floor within two tiles of such a wall onto it.
- At most 95 objects in the level with its doors: the rest of the engine's 127 are for what the game adds while
  it plays, which is mostly Barricades (a line of up to 12, one per lock). Props past that are left out.

A barrel or chest also blocks heroes but isn't held to these rules, so version 1 maps play as before.

### Where props come from

Vanilla Diablo and Hellfire place their decorations in theme rooms, quest rooms and per-level passes,
none of which the keep runs. What each one is and why it is or isn't a prop:

| Object | Vanilla use (tileset) | Light | Solid | Wall | Interactive | As a prop |
|---|---|---|---|---|---|---|
| OBJ_L1LIGHT brazier | Cathedral tiles (piece 269) | 5 | yes | | no | kind 2 |
| OBJ_CANDLE2, OBJ_BOOKCANDLE, OBJ_STORYCANDLE | shrine and library themes (Cathedral, Catacombs), Lazarus (Hell) | 5 (3) | yes | | no | Candles |
| OBJ_SKFIRE | skeleton room (Cathedral, Catacombs) | 5 | yes | | no | Skull fire |
| OBJ_TORCHL/R, L2/R2 | AddL2Torches (Catacombs), on wall tiles | 8 | no | yes | no | Wall torch (L and R; L2 and R2 hang on walls the camera can't see) |
| OBJ_BANNERL/M/R | skeleton room | | yes | | no | Banner |
| OBJ_SKPILE | set levels | | yes | | no | Skull pile |
| OBJ_CRUX1 to 3 | Leoric's tomb | | yes | | breaks, opens a door | Crucified skeleton, unbreakable |
| OBJ_BOOKSTAND, OBJ_SKELBOOK | library and skeleton rooms | | yes | | gives a book | Book stand, unreadable |
| OBJ_BOOKCASEL/R | library (Cathedral, Catacombs), against a straight wall | | yes, blocks missiles | | gives a book | Bookcase, inert |
| OBJ_WEAPONRACK(N), OBJ_ARMORSTAND(N) | weapon and armour themes (not the Cathedral) | | yes, block missiles | | the first gives an item | the N types, which never do |
| OBJ_DECAP | decapitated theme, Hell | | yes | | gives an item | Headless body, inert |
| OBJ_TNUDEM1 to 4, OBJ_TNUDEW1 to 3 | Butcher's room (Cathedral), Hell | | yes | | no | Tortured body |
| OBJ_TORTURE1 to 5 | Butcher's room, Hell walls (AddHookedBodies) | | no | yes | no | Impaled body (5 is a copy of 1) |
| OBJ_BLOODFTN | blood fountain theme (any) | | yes | | heals | Blood fountain, inert |
| OBJ_L5CANDLE | Crypt story books | 3 | yes | | no | Crypt candles |
| OBJ_ANGEL | set levels | | yes, blocks missiles | | no | Angel statue |
| OBJ_CAULDRON | Hell (cauldron theme) | | yes | | random shrine | left out: its art draws as a black blob in the keep |
| OBJ_SARC, OBJ_L5SARC | Cathedral, Crypt | | yes, two tiles | | opens, can hold a skeleton | left out: spawns monsters |
| OBJ_BCROSS, OBJ_TBCROSS burning cross | burning cross theme | 5 | yes | | burns whoever stands by | left out: does damage |
| Purifying, murky and tear fountains, goat shrine, shrines | themes | | yes, some four tiles | | buffs, curses | left out: interactive or too big |
| OBJ_LEVER, OBJ_BOOK2L/R, story books, pedestal, magic circles, mushroom patch | quests | | | | quest triggers | left out |
| OBJ_POD, OBJ_URN | Nest, Crypt | | yes | | break like barrels | left out: the barrel is the keep's breakable |
| OBJ_TRAPL/R, OBJ_FLAMEHOLE | traps | | | | fire | left out: the keep has its own traps |

Every prop is solid except the two wall kinds, and every one lets missiles through (the bookcase, the racks and
stands, the statue and the crucifixes would block them in vanilla). Object lights are baked into the level's
light when it's built, and also come and go as a moving light while a player is within the radius plus 10 tiles;
the engine has 32 moving lights in all.

**Art.** InitObjectGFX loads an object's sprite file only if a level-range, level-type, theme or quest says so,
and the keep (level 1, no themes, no quests) would get none of these. `AddMapObjectGraphics` adds the files of
every prop the tileset can have on the keep level. The engine holds 40 object sprite files a level (pObjCels):
the keep's own use about 9 (barrels, chests, traps, sarcophagi, its tileset's doors and braziers) and all props
add up to 21 more, so they fit; a kind whose files wouldn't fit is left out with a log line. A prop whose art the
game data lacks (Hellfire's, for the Crypt candles) isn't offered and is left out.

### Dressing

The editor's **Dress** button lights and furnishes a map in one go, with the same pass dkmap_tool runs on the
campaign's levels (`Source/keeper/keeper_dress.cpp`; docs/CAMPAIGN-MAPS.md says what it does). It only adds
ordinary props, each judged by the rules above, so the file format is unchanged and a dressed map is version 2.
It is seeded from the map's decoration seed and keeps off the heroes' way: solid props stay 2 tiles from the
route and the doors, and any prop that changes the route, a lock's detour, a neck's way round or where a
barricade goes is taken back out. Pressing it again adds only what's missing; Undo takes the lot out.

In a level source, `dress: <theme> [<theme>] [lights=N] [minlights=N] [spacing=N] [scenery=N] [seed=N] [centre=0]`
runs it after the `prop:` lines. The themes are bones, library, chapel, armoury, torture, prison, tomb and hell;
without one, the tileset's own (chapel for the Cathedral, tomb for the Catacombs and the Crypt, prison for the
Caves, hell for Hell, bones for the Nest).

## What makes a file bad

These are all refused with a message for the player, and the keep falls back to a random layout:

- Too short, too long (over 60 KB), wrong magic, a newer version, a size that doesn't match, or a
  bad CRC.
- An unknown tileset or object kind (version 1 knows kinds 0 to 2), a style the kind hasn't got, a facing on
  a kind that doesn't stand by a wall, more objects than the version holds, or a size other than 40 by 40.
- A layout value above 3, a transparency value above 127, or anything past the end of the payload.
- An object off the map, or stairs whose footprint leaves the paintable area (megatiles 1 to 38).
- Tile 0, or a tile beyond the end of that tileset's `.til` file (checked at load, against the file).
- More stairs tiles than the engine's trigger list holds.
- Flag bit 0 clear, or no entrance or vault: the map isn't finished, so it can only be opened in the
  editor.

Names are printable ASCII only; anything else reads back as `?`. File names are made from the map's
name and keep only letters, digits, spaces, `-` and `_`.

## Versions

Readers refuse a higher version than they know. Add a new version rather than reusing the reserved
byte or flag bits for anything older readers must understand.

- **1:** objects are 3 bytes, kinds 0 to 2, at most 64.
- **2:** objects are 4 bytes (the variant), kinds 0 to 18, at most 80. Nothing else changed.

A map is written as version 1 whenever that can hold it (64 objects or fewer, all kinds 0 to 2): a version 1
file reads back and writes back byte for byte, so the campaign's maps and players' older maps stay as they are
and still open in older builds. A map with props needs version 2. `dkmap_tool --selftest` checks both ways
round, and `--check-campaign` that every campaign map writes back unchanged and builds every object it holds.

## Co-op

The host sends the map's bytes in the join data, with the rest of the setup (a full version 2 map is about
10 KB). The joiner checks them (size up to
60000 bytes, everything above, flagged playable, the tileset in its game data, the tiles in range), then saves
them as `maps/received/from_host.dkmap` before the level loads, so everyone builds the same level from the start.
A joiner who gets a map that fails is told why and the game exits, since playing a different keep from everyone
else's isn't possible. A host refuses to send a map that would fail, and the Custom Map screen says what's wrong.
