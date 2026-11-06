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
| 4 | u16 | version | 1, 2 or 3: see Versions |
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
| u16 | object count | at most 64 in version 1, 80 in versions 2 and 3 |
| 3 bytes each (v1), 4 (v2, v3) | objects | u8 kind (see Objects and props), u8 x, u8 y as world tiles, and from version 2 u8 variant |
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
- Some stairs are built into a wall, as the generator only ever places them: the Cathedral's up stairs (Diablo's
  own Cathedral set piece, `L5STAIRSUP`, which backs onto solid rock), the Crypt's up stairs, and both of the
  Caves' and the Nest's. The Cathedral's, Crypt's and Caves' up stairs go in a room's top-right wall (the stairs'
  top rows are rock: 2 deep, 3 for the Crypt) and the Caves' and Nest's down stairs in a top-left wall (the left
  2 columns). Their rock must already be painted rock, the wall must run on one cell past both sides of them,
  and their floor must be painted floor (`MapStairsWallProblem`), so the stamp never stands a wall in open floor.
  The editor snaps a click to the nearest such spot within 5 cells, and says where they go if there is none;
  dkmap_tool refuses a level that breaks it. A map saved before this rule still loads and plays as it was baked:
  the editor says so when it opens it, and `ValidateMap` lists it as a warning. The Cathedral's and Crypt's down
  stairs, and the Catacombs' and Hell's, stand free in a room, as the generator's do.

## Objects and props

Kinds 0 to 2 are the version 1 objects and behave exactly as they always did: a barrel or chest can be
broken or opened, and they go anywhere on the floor. Kinds 3 and up are **props**: scenery that does
nothing. A prop can't be clicked, opened, broken or set off, and missiles fly through it, so it never
changes a fight. A solid prop is walked round, so the placement rules below keep it from changing where
heroes can go.

The variant byte holds the prop's look (its style, bits 0 to 6) and, for a wall prop, bit 7: clear if the
floor in front is at +x, set if at +y. Those are the two walls the camera sees. Anything else is refused.

Each kind also has a stable id, used by level sources and the editor's theme stamps (`MapObjectId`,
`MapObjectKindNamed` in keeper_map.hpp, which takes the id or the name shown, any case). Ids never change once
released; the names shown may be reworded. The editor's prop list is grouped: Light, Wall, Furniture, Bodies,
Clutter and Set pieces.

| Kind | Name | Engine object | Styles | Light | Solid | Stands | Tilesets |
|---|---|---|---|---|---|---|---|
| 0 | Barrel | OBJ_BARREL | 1 | | yes | floor | all (breakable) |
| 1 | Chest | OBJ_CHEST1 | 1 | | yes | floor | all (opens) |
| 2 | Brazier | OBJ_L1LIGHT | 1 | radius 5 | yes | floor | Cathedral |
| 3 | Candles | OBJ_CANDLE2 | 1 | radius 5 | yes | floor | all |
| 4 | Skull fire | OBJ_SKFIRE | 1 | radius 5 | yes | floor | all |
| 5 | Wall torch | OBJ_TORCHL2 (+x), OBJ_TORCHR2 (+y) | 1 | radius 8 | no | on a wall | all but the Nest |
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

Version 3 adds these, all inert scenery however their vanilla object behaves. They are placed without their
type's own setup (`AddSceneryObject`): no shrine is picked, no skeleton hides in a sarcophagus, no chest gets
items, and frames are fixed by the style rather than rolled. A multi-tile kind takes the tiles the engine's own
setup marks (AddSarcophagus, AddLargeFountain, AddMushPatch): `x, y` is the tile it's drawn from, and the others
are at -y (two tiles) or -x, -y and both (four).

| Kind | Id | Name | Engine object, frames | Styles | Light | Solid | Stands | Tiles | Tilesets | Vanilla anchor |
|---|---|---|---|---|---|---|---|---|---|---|
| 19 | closed_chest | Closed chest | OBJ_CHEST1, 2, 3, frame 1 (shut) | 3: small, medium, large | | yes | floor | 1 | all | random floor tile (AddChest) |
| 20 | sarcophagus | Sarcophagus | OBJ_SARC, frame 1 (shut) | 1 | | yes | floor | 2 | Cathedral, Catacombs, Hell, Crypt | floor; AddSarcophagus marks -y |
| 21 | crypt_sarcophagus | Crypt sarcophagus | OBJ_L5SARC, frame 1 | 1 | | yes | floor | 2 | Crypt, Hell | as the sarcophagus |
| 22 | shrine | Shrine | OBJ_SHRINEL (+x), OBJ_SHRINER (+y), frame 1 (unused) | 1 | | yes | against a wall | 1 | all but the Nest | TFit_Shrine: the floor tile with the wall at -x (L) or -y (R), as the bookcase |
| 23 | goat_shrine | Goat shrine | OBJ_GOATSHRINE, animated | 1 | | yes | floor | 1 | all | middle of 3 by 3 floor (TFit_GoatShrine) |
| 24 | large_fountain | Large fountain | OBJ_PURIFYINGFTN, OBJ_MURKYFTN, animated | 2: clear, murky | | yes | floor | 4 | all | 2 by 2 floor (TFit_Obj5); AddLargeFountain marks -x, -y |
| 25 | tear_fountain | Tear fountain | OBJ_TEARFTN, animated | 1 | | yes | floor | 1 | all | as the large fountain, one tile |
| 26 | pedestal | Pedestal | OBJ_PEDESTAL, frames 1 to 4 (no stones to three) | 4 | | yes | floor | 1 | all but the Nest | set level floor (Blood quest) |
| 27 | lectern | Lectern | OBJ_BOOKLVR (book1), frames 1 to 3 | 3: shut, open, bare | | yes | floor | 1 | all but the Nest | set level floor (book levers, quest tomes) |
| 28 | story_book | Story book | OBJ_STORYBOOK, frames 1 to 6 | 6: three books, shut and open | | yes | floor | 1 | all but the Nest | AddStoryBooks: open floor, candles round it |
| 29 | lever | Lever | OBJ_LEVER, frames 1, 2 | 2: up, pulled | | yes | floor | 1 | all but the Nest | set level floor |
| 30 | skull_switch | Skull switch | OBJ_SWITCHSKL, frames 1, 2 | 2 | | yes | floor | 1 | Hell | Diablo's level, on the floor |
| 31 | crypt_lever | Crypt lever | OBJ_L5LEVER, frames 1, 2 | 2 | | yes | floor | 1 | Crypt | Na-Krul's level, on the floor |
| 32 | crypt_book_stand | Crypt book stand | OBJ_L5BOOKS, frames 1 to 6 | 6 | | yes | floor | 1 | Crypt | AddCryptStoryBook: open floor, candles round it |
| 33 | altar | Altar | OBJ_ALTBOY, frame 1 | 1 | | yes | floor | 1 | all | Lazarus's set level, floor |
| 34 | staff_stand | Staff stand | OBJ_LAZSTAND, frames 1 (staff), 2 (bare) | 2 | | yes | floor | 1 | all | AddLazStand: open floor |
| 35 | rock_stand | Rock stand | OBJ_STAND, frame 1 | 1 | | yes | floor | 1 | all | Magic Rock quest, floor |
| 36 | magic_circle | Magic circle | OBJ_MCIRCLE1, frames 1 to 4, drawn under units | 4: pentagram and rings, grey and red | | no | floor | 1 | all | Lazarus's set level, painted on the floor |
| 37 | burning_cross | Burning cross | OBJ_TBCROSS, animated | 1 | radius 5 | yes | floor | 1 | all | middle of 3 by 3 floor (Theme_BrnCross) |
| 38 | hanging_body | Hanging body | OBJ_NUDEW2R, animated | 1 | | yes | floor | 1 | all | set levels, floor |
| 39 | bookshelf | Bookshelf | OBJ_BOOKSHELF (+x), OBJ_BOOKSHELFR (+y), the bare shelves | 1 | | yes | against a wall | 1 | Cathedral, Catacombs, Crypt | set levels, as the bookcase |
| 40 | mushroom_patch | Mushroom patch | OBJ_MUSHPATCH, frame 1 | 1 | | yes | floor | 4 | Caves, Nest | AddMushPatch marks -x, -y |
| 41 | urn | Urn | OBJ_URN, frame 1 (whole) | 1 | | yes | floor | 1 | Crypt | random floor tile (as barrels) |
| 42 | pod | Pod | OBJ_POD, frame 1 (whole) | 1 | | yes | floor | 1 | Nest | random floor tile (as barrels) |
| 43 | cauldron | Cauldron | OBJ_CAULDRON, frame 1 (full) | 1 | | yes | floor | 1 | all | 2 by 2 floor (TFit_Obj5) |

Every one was checked by eye in every tileset it's offered in, each style and facing (dkmap_tool builds survey maps,
the game screenshots them): floor kinds stand on their tile, the shrine and the bookshelf have their backs on the
wall, and no frame is blank or broken. The Hellfire kinds (the Crypt's and the Nest's) need Hellfire's data.

`x, y` is the tile the prop is stored on: for a wall prop, the wall tile. An impaled body facing +x is drawn
from the floor tile in front (its art is made for that), so it stands there in the level, without blocking it.
Every tileset has a light: braziers, candles, skull fires and torches, and the Crypt's own candles.

### Where a prop may stand

The game judges each prop in file order when it builds the level, with the doors, the version 1 objects and
the props before it already standing, and leaves out any that fails; the editor says why (the same check),
and a left-out prop is a warning. The rules depend only on the tiles and the objects, so every co-op client
leaves out the same ones.

- Not within a tile of either staircase's footprint, nor where something already stands.
- **On the floor:** on a walkable tile, every tile of a multi-tile kind's footprint, which is judged as one
  block by the rules below. A solid prop must also keep the heroes' ways as they were: every
  walkable tile round it must still reach every other one without it, by the steps heroes take (BuildField's,
  diagonals round corners included), so it can't cut anything off or fill a doorway; and it must leave at least
  two tiles of open floor beside it along both axes, so it never narrows a way through to one tile or puts a
  pillar in the middle of a three-wide hall. In a room it only adds a step or two to a walk round it.
- **Against a wall** (bookcase): all of that, and a straight wall right behind it.
- **On a wall:** the tile and its two neighbours along the wall are solid, none is a door, the tile in front
  is bare floor, and the wall is painted as rock (the Caves' wooden fence, on floor either side of its gate, isn't a wall). The editor snaps a click on the floor within two tiles of such a wall onto it.
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
| OBJ_TORCHL/R, L2/R2 | AddL2Torches (Catacombs): L2 and R2 on the wall tile of a wall with floor at +x or +y, L and R on the floor in front of a wall with floor at -x or -y | 8 | no | yes | no | Wall torch (L2 and R2, on the walls the camera sees; L and R drawn on those walls float clear of them or stand on top) |
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
| OBJ_CAULDRON | Hell (cauldron theme) | | yes | | random shrine | Cauldron, inert, frame 1: frame 3 is the drained pot, the black blob seen before |
| OBJ_CHEST1 to 3, OBJ_TCHEST1 to 3 | any level | | yes | | items, traps | Closed chest, shut and inert (the v1 Chest still opens) |
| OBJ_SARC, OBJ_L5SARC | Cathedral, Crypt | | yes, two tiles | | opens, can hold a skeleton | Sarcophagus, Crypt sarcophagus: shut, no skeleton, never opens |
| OBJ_BCROSS, OBJ_TBCROSS burning cross | burning cross theme | 5 | yes | | burns the player on the tile at -y | Burning cross: lights, never burns |
| OBJ_SHRINEL/R | shrine theme, against a wall | | no (yes as a prop) | | buffs, curses | Shrine, unusable |
| OBJ_GOATSHRINE, purifying, murky and tear fountains | themes | | yes, two by two for the big fountains | | buffs, heals, curses | Goat shrine, Large fountain, Tear fountain: inert |
| OBJ_LEVER, OBJ_SWITCHSKL, OBJ_L5LEVER, OBJ_FLAMELVR | quests, Diablo's and Na-Krul's levels | | yes | | open doors, start traps | Lever, Skull switch, Crypt lever: never pulled (the flame lever is the same art) |
| OBJ_BOOKLVR, OBJ_BLINDBOOK, OBJ_BLOODBOOK, OBJ_STEELTOME (book1) | quests | | yes | | quest text, open doors | Lectern, unreadable |
| OBJ_STORYBOOK, OBJ_L5BOOKS | levels 4, 8, 12, the Crypt | | yes | | story text | Story book, Crypt book stand: unreadable |
| OBJ_PEDESTAL, OBJ_STAND, OBJ_LAZSTAND, OBJ_ALTBOY | quests | | yes | | quest steps | Pedestal, Rock stand, Staff stand, Altar: inert |
| OBJ_MCIRCLE1/2 | Lazarus's level | | no | | teleport | Magic circle: never teleports (not updated) |
| OBJ_NUDEW2R | set levels | | yes | | no | Hanging body |
| OBJ_BOOKSHELF, OBJ_BOOKSHELFR | set levels | | yes | | no | Bookshelf |
| OBJ_MUSHPATCH | Mushroom quest | | yes, two by two | | gives the mushroom | Mushroom patch, inert |
| OBJ_POD, OBJ_URN | Nest, Crypt | | yes | | break like barrels, can hold a skeleton | Pod, Urn: whole and unbreakable. The exploding OBJ_PODEX, OBJ_URNEX and OBJ_BARRELEX stay out |
| OBJ_BOOK2L/R, OBJ_SKELBOOK | quests, skeleton room | | yes | | books | left out: the same art as the Book stand |
| OBJ_SLAINHERO | level 9 | | yes | | gives an item | left out: it is OBJ_DECAP's frame 2, the Headless body's second style |
| OBJ_SIGNCHEST, OBJ_WEAPRACK, OBJ_WARARMOR, OBJ_WARWEAP | quests | | | | | left out: the same art as the closed chest, racks and stands |
| OBJ_WATER (miniwatr) | unused | | | | | left out: no vanilla anchor to copy |
| OBJ_TORCHL/R | Catacombs, on the floor in front of the near walls | 8 | | yes | | left out: the format's facings are the walls the camera sees, where they float |
| OBJ_TRAPL/R, OBJ_FLAMEHOLE | traps | | | | fire | left out: the keep has its own traps |

**Wall dressing.** Vanilla's only wall-mounted objects are the torches (OBJ_TORCHL2/R2 on the wall tile), the
impaled bodies (OBJ_TORTURE1 to 5) and the stick torches of the near walls. The chains, skeletons and banners on
dungeon walls are wall tiles (the decoration variants a saved map bakes), not objects, so there are no more wall
kinds to add from vanilla's art; the shrine and the bookshelf stand against walls like the bookcase.

**Inert.** A prop's Objects[] slot is noted when InitMapObjects places it (and again from the map if the level
comes from its saved copy). `keeper::IsInertMapProp` is checked first thing by OperateObject, SyncOpObject and
DeltaSyncOpObject, BreakObject, BreakObjectMissile, SyncBreakObj and DeltaSyncBreakObj, so neither a click, a
telekinesis spell nor a message from another player can use one; ProcessObjects only lights and animates it (no
sarcophagus update, no burning cross damage, no magic circle). Its selection region is none, so the cursor and
the controller never pick it, its break state is 0, its trap flag is clear and missiles pass. Bots and hero
monsters never operate objects at all. dkmap_tool --selftest places every kind in every tileset, uses, breaks
and syncs each one and checks nothing changes, nothing drops, nothing spawns, and standing by a burning cross
does no harm; and that the version 1 chest still opens.

Every prop is solid except the two wall kinds and the magic circle, and every one lets missiles through (the bookcase, the racks and
stands, the statue and the crucifixes would block them in vanilla). Object lights are baked into the level's
light when it's built, and also come and go as a moving light while a player is within the radius plus 10 tiles;
the engine has 32 moving lights in all.

**Art.** InitObjectGFX loads an object's sprite file only if a level-range, level-type, theme or quest says so,
and the keep (level 1, no themes, no quests) would get none of these. `AddMapObjectGraphics` adds the files of
the props the map uses. The engine holds 40 object sprite files a level (pObjCels): the keep's own use 9 or 10
(barrels, chests, traps, the sarcophagus, its tileset's doors, the Cathedral's braziers), and every kind a
tileset offers would need 41 to 49 in all, so a level can't load them all. A map stays within 40 if it uses
fewer kinds: the editor refuses a kind that would go over, ValidateMap warns, dkmap_tool fails a level that
does, and the game leaves out a kind whose files don't fit, with a log line. The editor loads the map's own
kinds first, then the chosen kind and the rest of the tileset's while there's room; a kind it didn't load
reloads the level when placed (`MapNeedsArtReload`). A prop whose art the game data lacks (Hellfire's, for the
Crypt's and the Nest's kinds) isn't offered and is left out.

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
Caves, hell for Hell, bones for the Nest). `rooms=N` on that line also gives the N biggest rooms a theme-room
stamp each (below), for the themes in turn, before the lights and scenery; a stamped room gets no scattered
scenery or centrepiece, and its props don't count against `scenery=`. `stamps=crypt,treasure` names the stamps
instead, biggest room first (one the tileset can't have is passed over). A room much bigger than a stamp
(a hall, a cavern) can take a second or third in corners of it far apart, once every room has had one, and
keeps its scattered scenery; a room a stamp can't furnish doesn't use up the count. Without `rooms=` the pass is
unchanged.

### Theme-room stamps

The editor's **Stamp** tool (key T) puts a themed group of props in a room at once. Click inside a room for
that room (one wider than 10 megatiles gives a 10 by 10 corner of it round the click, against the walls the
camera sees if it can, so it has walls to stand by), or drag a rectangle for its floor. The stamp shows at
once, outlined, and isn't kept until you say: click it again (or Enter) to keep it, Shift-click it (or R) for
another layout, right-click (or Esc) to drop it. Clicking another room moves it there. Once kept, one Undo
takes the whole stamp out. Click the tool again for the list of stamps meant for the tileset; `[` and `]` step
through them, and a stamp on show turns into the new one where it stands. On a controller: the Confirm button
stamps the room under the cursor (hold it to drag) and keeps the stamp when pressed on it again, Shift (the
left trigger) with Confirm rerolls it, the Order button drops it, the shoulder buttons step through the stamps,
and the tool button opens the list as for props.

| Stamp | Tilesets | What it puts in |
|---|---|---|
| library | Cathedral, Catacombs, Crypt | bookcases and bare bookshelves round the walls, an open lectern in the middle, story books (the Crypt's own book stands there), candles, a torch, an urn in the Crypt |
| shrine | Cathedral, Catacombs, Caves, Crypt | a shrine against the wall, two candles round it, pedestals, a fountain or goat shrine, a banner, a torch |
| chapel | Cathedral, Catacombs, Crypt | an altar, candles round it, banners, a lectern, a pedestal, a grey magic circle, a tear fountain, a torch |
| crypt | Cathedral, Catacombs, Crypt | a sarcophagus in the middle (the Crypt's own there), more by the walls, urns or skull piles, a story book, a closed chest, crypt candles |
| torture | all but the Nest | a hanging body ringed with skull fires, impaled and more hanging bodies, tortured bodies, a pulled lever, headless bodies, a torch |
| prison | all but the Nest | impaled, hanging and tortured bodies round the walls, a lever, skull piles, a torch |
| armoury | Catacombs, Caves, Hell, Crypt | weapon racks and armour stands round the walls, a banner, a large closed chest, torches |
| treasure | all but the Nest | a large closed chest, smaller ones by the walls, a pedestal of stones, a staff stand, candles round it, a torch |
| altar | Hell, Nest | an altar ringed with skull fires and a red magic circle, a burning cross, a skull switch or goat shrine, crucifixes and bodies |
| bones | all | skull piles, crucifixes, headless bodies, a pod or mushroom patch in the Nest and Caves, a skull fire |

A stamp names its props by id (`MapObjectId`) or name, with choices: `crypt_sarcophagus|sarcophagus|crucified_skeleton`
takes the first this build has that the tileset allows and the level's art budget can load (a kind the map doesn't
use yet is passed over if it would take the level past 40 object sprite files), so a stamp uses fewer kinds rather
than fail. `closed_chest/2` asks for a style (the large chest, here); without one the seed picks. Names that aren't
kinds are skipped. The stamps leave out the cauldron and the bare pedestal, which read as black blobs in the
keep's dark; they are still in the editor's list. Each entry says where in the room its props go (round the edge, in the middle,
two or three tiles round the centrepiece, or anywhere), how many (a range the seed picks from, more in a big
room) and how far apart. Every prop is judged as the Dress pass judges its own: by the rules above, kept 2
tiles off the heroes' route and the doors if solid, and taken back out if it changes the route, a lock's
detour, a neck's way round or a barricade spot. The editor's stamps stop at the object caps (80 stored, 95 on
the level with its doors) and say so; the stamps' table is `MakeRoomStamps` in `Source/keeper/keeper_dress.cpp`.

In a level source, `stamp: <stamp> <x> <y> [seed=N]` stamps the room under world tile x, y as a click does, and
`stamp: <stamp> <x1> <y1> <x2> <y2> [seed=N]` the floor in that rectangle as a drag does. Stamps go in after the
`prop:` lines and before the `dress:` line, and a stamp that fits nothing fails the level.

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
- **3:** kinds 0 to 43. Nothing else changed.

A map is written as the oldest version that holds it: version 1 for 64 objects or fewer, all kinds 0 to 2;
version 2 if it has only kinds up to 18; version 3 only if it uses a newer kind. Each reads back and writes
back byte for byte, so the campaign's maps and players' older maps stay exactly as they are and still open in
older builds. The new kinds got a version rather than going into version 2 because a version 2 reader refuses
an unknown kind as damage ("it has an unknown object"); with version 3 it says the map is from a newer game,
which is the truth, and the rule above says a change older readers can't understand takes a new version. `dkmap_tool --selftest` checks both ways
round, and `--check-campaign` that every campaign map writes back unchanged and builds every object it holds.

## Co-op

The host sends the map's bytes in the join data, with the rest of the setup (a full version 2 map is about
10 KB). The joiner checks them (size up to
60000 bytes, everything above, flagged playable, the tileset in its game data, the tiles in range), then saves
them as `maps/received/from_host.dkmap` before the level loads, so everyone builds the same level from the start.
A joiner who gets a map that fails is told why and the game exits, since playing a different keep from everyone
else's isn't possible. A host refuses to send a map that would fail, and the Custom Map screen says what's wrong.
