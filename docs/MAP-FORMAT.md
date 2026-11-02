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
| 4 | u16 | version | 1 |
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
| u16 | object count | at most 64 |
| 3 bytes each | objects | u8 kind (0 barrel, 1 chest, 2 brazier), u8 x, u8 y, as world tiles |
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
  door tiles. Barrels, chests and braziers are stored, and a map gets nothing else: no random
  objects. Objects on solid ground are skipped.
- The entrance and vault are the tileset's normal up and down stairs, stamped into the tiles. The
  game finds the vault through its usual stairs trigger.

## What makes a file bad

These are all refused with a message for the player, and the keep falls back to a random layout:

- Too short, too long (over 60 KB), wrong magic, a newer version, a size that doesn't match, or a
  bad CRC.
- An unknown tileset or object kind, or a size other than 40 by 40.
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

## Co-op

The host sends the map's bytes in the join data, with the rest of the setup. The joiner checks them (size up to
60000 bytes, everything above, flagged playable, the tileset in its game data, the tiles in range), then saves
them as `maps/received/from_host.dkmap` before the level loads, so everyone builds the same level from the start.
A joiner who gets a map that fails is told why and the game exits, since playing a different keep from everyone
else's isn't possible. A host refuses to send a map that would fail, and the Custom Map screen says what's wrong.
