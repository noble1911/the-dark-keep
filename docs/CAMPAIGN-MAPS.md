# The Dark Keep: campaign maps

Campaign level N plays `assets/keeper/campaign/level_NN.dkmap`. A missing file means a random layout
for that level, and `CMake/Assets.cmake` lists the files, so a removed level has to come out of there too.
So does game data without the map's tileset (the shareware data, say): that level stays random too.

## How they're made

The sources are `scripts/dev/dkmap/levels/level_NN.txt`, which `scripts/dev/dkmap/make_levels.py` writes.
`dkmap_tool` (`scripts/dev/dkmap/dkmap_tool.cpp`) links the game's map code and opens no window. For each
level it joins the walls and bakes with the same code as the editor, rerolls the decoration if that broke
the map, writes the file, then reads it back and loads it by the game's own road, and checks:

- `ParseMap` accepts it, it is flagged playable, and it is under the 60000 byte co-op limit;
- `ValidateMap` finds nothing blocking on the loaded level;
- the game's first down-stairs trigger is in the vault's footprint, and every tile a party can spawn on
  (free floor up to 6 tiles from the entrance) reaches it by `BuildField`'s rules (solids, diagonals; doors
  count as open);
- every painted door formed a door object, and every arch can be walked through;
- the name matches `CampaignLevels` in `keeper_campaign.cpp`.

To rebuild, after configuring with `-DDARK_KEEP_MAP_TOOL=ON` and building the `dkmap_tool` target:

```
python scripts\dev\dkmap\make_levels.py
build\Release\dkmap_tool.exe --data E:\Games\Diablo --assets build\assets --levels scripts\dev\dkmap\levels --out assets\keeper\campaign --doc docs\CAMPAIGN-MAPS.md
```

`--check-campaign` instead finds all twenty the way a shipped game does, by asset name.

## Dressing

Every level is dressed: lit and furnished, so it looks like a lived-in dungeon in the keep's dark. Each source
ends with a `dress:` line (from `DRESSING` in make_levels.py) naming the level's themes, and dkmap_tool runs the
dressing pass (`Source/keeper/keeper_dress.cpp`, the same code as the editor's Dress button) on the decorated
level, after any `prop:` lines. It is seeded from the map's decoration seed, uses no game random numbers, and
what it places is stored in the file as ordinary props, so every co-op client builds the same level.

- **Lights first.** Wall torches (radius 8) on the walls the camera sees, one wherever the floor is furthest
  from any light, so each room and junction gets one and a corridor one every 9 steps or so, with dark left
  between the pools. Spots near the heroes' route and near doors come first, so the master sees the fights. A
  candle or skull fire (radius 5) goes where no torch reaches: in the keep's dark radius 5 lights only a tile or
  two, so these count as half a torch. That is also why the Cathedral levels that had braziers (3, 6 and 8) now
  have torches instead. The Cathedral's own candlestick tiles still bring braziers of their own, counted in. The
  Catacombs' vanilla wall torches (AddL2Torches) never run on a map, so a Catacombs map has only what it's given.
- **Then scenery** from the themes: bones, library, chapel, armoury, torture, prison, tomb, hell. Up to 18 props
  (24 in Hell), spread over the rooms by size, against the walls and at least 4 tiles apart, with one
  centrepiece (an angel statue, a blood fountain, a crucifix) in the middle of the biggest room. Kinds the
  tileset hasn't got are skipped. Corridors get lights only.
- **Balance can't change.** Solid props keep 2 tiles off the heroes' route and off doors, so the lanes stay
  clear. Each prop is taken back out if it changes where heroes appear, the route, the route with any door
  locked, the way round a neck shut at any route point, or where a barricade could go along it. Then
  dkmap_tool builds every level bare as well, and fails it if the door or neck summary differs. Props do take
  a little floor: "tiles to build on" is 11 to 18 lower than bare, under 1%.

Hell is the flattest art: its floor is bright and even, and the red tops of its walls glow whatever the light.
Its ambient is darker than the other tilesets' (7 preparing, 10 in a raid, in `keeper_lighting.cpp`), and its
levels get 18 lights and up to 24 props. The Catacombs' floor is the darkest art, so its levels get 18 lights too.

## Tilesets

| Levels | Tileset | Why |
|---|---|---|
| 1 to 8 | Cathedral | The fullest tileset (docs/TODO.md): doors, arches, braziers. Where each mechanic is taught. |
| 9 to 14 | Catacombs | Doors, so locks still work. Bigger loops. |
| 15 to 17 | Caves | A door is a wooden fence across the cave with a gate in it (the same objects the base game uses). Locks work on the gates. |
| 18 to 20 | Hell | The art has no doors at all, so the Lock tool makes a **Barricade** across any neck up to 12 tiles wide. The longest routes. |

Crypt and Nest are not used. All four tilesets have been looked at in test runs, without a window: walls, stairs,
gates and barricades draw as they should. No level has been played through by a person, so how they feel is
still a judgement from the numbers. Catacombs and Caves get brighter ambient light than the other two, because
their art is darker.

A Barricade only goes where it really shuts the way: `keeper_barricade.cpp` goes on past thin pillars and checks
there's no short walk round. In a hall too wide to bar, look for a neck, or the shoulders on level 18.

## Reading the numbers

- A lock costs a hero's route 40 tiles. A way round shorter than that turns the party away; a longer one
  makes it stop and break the door. "A lock turns the party away from N" counts the doors of the first
  kind on the short route.
- "Shut at any of N points along the route": a 9 by 9 tile square is blocked at points along the shortest
  route, and that says how often there's no other way on (a neck), and how often the way round is a real
  one. Halls four cells wide or more can often be passed along a wall. It measures the map, not a game.
- "Route" is the shortest walk in tiles from where heroes appear to the vault's stairs.

## Reading the previews

One character is one megatile (two world tiles square), x to the right and y down; in game this is turned
45 degrees. `#` wall, `.` floor, `+` floor on the shortest route, `D` door, `A` arch, `E` entrance stairs,
`V` vault stairs, `b` barrel, `c` chest, `i` brazier, `o` a prop (docs/MAP-FORMAT.md). The stairs' footprints
cover cells that really are partly walkable. A level source places props with `prop: <x> <y> <kind>[/<style>]`
lines, by world tile, as a click in the editor would: see the top of scripts/dev/dkmap/dkmap_tool.cpp.

<!-- generated by dkmap_tool from here on -->

## Level 1: Old Cellars

**Cathedral.** Route 56 tiles, 466 floor cells, 1968 tiles to build on. 4 doors: 2 can't be bypassed, 0 on the short route with a way round, 2 off the short route. Shut at any of 8 points along the route, 4 leave no way on. 19 light sources, 26 props.

One way in, two doors on it. Nothing to work out but where to stand: the hall is the place for traps, the door the place for a first lock, and the two closets take lairs out of the way.

```


             ############o#
             o....EEEE...o#
             #....EEEE....#
             #....EEEE....#
             #....EEEE....#
             #.......++...#
             #........+...#
             #.......++..o#
             o..o...++....#
             #####o.+######
                  #.+#
                  #.+#
                  #.+#
                  #.+#
                  #++#
         ##########D#########o#
  ########..........+.........######o#
  #......o.......o...+........#.....o#
  o.b....#............+.......#....c.#
  #......D.....o.......+......o......#
  #......#..........o..++.....D......#
  #..b...#............++......#......#
  #...o..#...........++.......#......#
  #######oo.........++.o...o..########
         ###########D#####o####
            #...o....+.....#
            #.........+....#
            #.........++..o#
            #.....VVVV+....#
            #.....VVVV....o#
            #.....VVVV.....#
            #.o............#
            #..............#
            #..............#
            ################



```

## Level 2: Rat Warrens

**Cathedral.** Route 123 tiles, 724 floor cells, 3161 tiles to build on. 5 doors: 3 can't be bypassed, 0 on the short route with a way round, 2 off the short route. Shut at any of 19 points along the route, 16 leave no way on. 19 light sources, 29 props.

A long S with doors in a row and blind burrows off it. Still only one route, but now there is distance to spend: layers of defence, and burrows that hide lairs so that they strike the party from the side.

```


  ###########            ##########o##
  #..EEEE...#            #...........#
  #..EEEE...#            #..........o#
  #..EEEE...##o#######o###...........#
  #..EEEE...............+D...........#
  #.....++.+++++++++++++.#+..........#
  #......++.####+.########.+.........#
  #.........#  #..#      #..+........#
  #.....o.o.#  #..#      #...+...o...#
  ##############..#####  #####+.######
            #.........#      #.+#
            #.b.....c.#      #.+#
            #.........#      #.+#
            #.........#      #++#
  o##########....o..b.#o######+.####o#
  #.........#.........##....++.......#
  #.........############...++........#
  #.........#o######o###..++........o#
  #........+D.........+D.++..........#
  #.......++#++++++++++#++...........#
  #......++.#####+######............o#
  #.....++..#   #.#    #..o..........#
  #.o..++.o.#   #.#    #.............#
  ####.+#########D####################
     #.+#    o........#
     o.+#    #.b......#
     #.+#    #.....o..#
  ####.+#############o################
  #.....+.......#....................#
  #......+......#..................c.#
  #....VVVV.....#....................#
  #....VVVV.....D..........o.........#
  #....VVVV.....#...................o#
  #.............#..................b.#
  #.....o.o....o#.............o.o....#
  ####################################


```

## Level 3: Sunken Chapel

**Cathedral.** Route 71 tiles, 604 floor cells, 2610 tiles to build on. 5 doors: 1 can't be bypassed, 1 on the short route with a way round (detour +34 tiles), of which a lock turns the party away from 1, 3 off the short route. Shut at any of 10 points along the route, 1 leave no way on; 2 have a real way round (+10 tiles or more, longest +34). 18 light sources, 31 props.

The first loop. Two aisles reach the chancel, one shorter, and a side chapel links them. A lock on the short aisle turns the party down the long one, so a single lock chooses which hall the traps go in. Both aisles meet at the one last door.

```

          ############
          #..EEEE....#
      o####..EEEE...o#     ###o#
      #...#..EEEE....##o####..o#
      #..+D..EEEE....D.........#
      #..+#++........#.........#
      #..+#..........#######...#
      #..+#o.........#     #..o#
      #..+############     #...#
      #..+#                #...#
      #..+#################o..o#
      o..+#................#...#
      #..+#.b............b.#...#
      #..+#.......o.o......#...#
      #..+o................#...#
      #..+#................#...#
      #..+D................#...#
      o..+#........o.......#...#
      #..+#...o............o..o#
      #..+#................D...#
      #..+#................#..o#
      #..+#.b............b.#...#
      #..+#..o...o........o#...#
      #..+##################...#
      o..+#                #...#
      #..+#####o########o###...#
      #...+....................#
      #....+...................#
      #.....+..................#
      #o.....+.................#
      o######D########o#########
      #......++........#
      #......VVVV......#
      #......VVVV......#
      #......VVVV......#
      #................#
      #....o....o.....o#
      ##################

```

## Level 4: Bone Pits

**Cathedral.** Route 83 tiles, 850 floor cells, 3634 tiles to build on. 7 doors: 1 can't be bypassed, 2 on the short route with a way round (detour +16, +16 tiles), of which a lock turns the party away from 2, 4 off the short route. Shut at any of 12 points along the route, 6 leave no way on; 4 have a real way round (+10 tiles or more, longest +16). 19 light sources, 29 props.

A hub with pits around it. The party crosses the central pit whatever it does, so whatever stands there sees it. Two ways lead on from the hub, by the east pit and by the south pit, and the north and west pits are dead ends: places for lairs, not places anyone need go.

```

 ############         ############o#
 #..EEEE....#         #............#
 #..EEEE....#         #.b........c.#
 #..EEEE....##o########............#
 #..EEEE..............D...........o#
 #.....++..++++++++...#............#
 #......+++.#######+.##............#
 #..o....+.o#     #.+##..........b.#
 ############     #.+#o.....o......#
                  #.+###############
                  o.+#
                  #++#    #############
 o##########o######D#####o#......o..o.#
 #...........#......+.....#...........#
 #.b.........#.......+...o#...........#
 #...........#........+...#..........o#
 #..........o#.........+.+D...........#
 #...........#..........++#+..........#
 #...........D............#.+.........#
 #...........#......o.....#..+........#
 #...........#............#...+.......#
 #...........#............#....+......#
 #...........#...........oo.....+.....#
 #.c.......b.#............#......+....#
 o...o..o....#..........o.#####o#D#####
 ##############D#########o#.......+...#
           #..............#........+..#
           #..............#.......++..#
           #...o..........#...VVVV+...#
           #..............#...VVVV....#
           #..............#...VVVV....#
           #..............#...........#
           #..............D...........#
           #.......o......#...........#
           ################...........#
                          #...........#
                          #.....o..o..#
                          #############

```

## Level 5: Black Vaults

**Cathedral.** Route 96 tiles, 1089 floor cells, 4653 tiles to build on. 11 doors: 0 can't be bypassed, 4 on the short route with a way round (detour +5, +5, +7, +7 tiles), of which a lock turns the party away from 4, 7 off the short route. Shut at any of 14 points along the route, 0 leave no way on. 19 light sources, 29 props.

Nine strongrooms joined by doors in a lattice, the doors set far apart so each room has to be crossed. The door count is the point: gold for locks runs out long before doors do, and every lock moves the route somewhere else, so the player is choosing a path through the keep.

```

 ##############o#########o##########o#
 #.EEEE......#...........#...........#
 #.EEEE.....o#.b.........D.........c.#
 #.EEEE......#...........#...........#
 #.EEEE......#...........#...........#
 #....++.o...#...........#...........#
 #.....++....#..........o#...........#
 #.....++...o#...........#...........#
 #....++.....#..........o#..........o#
 #...++......#...........#...........#
 #..++.......D...........#...........#
 #.++........o...........o......o....#
 ##D###################D#o#########Do#
 #..+........#...........#...........#
 #...+......+D.b.........#...........#
 #....+....++#+..........#...........#
 #.....+..++.#.+.........#..........o#
 #......+++..#..+........#...........#
 o.......+...#...+.......#...........#
 #...........#....+......#...........#
 #...........#....++.....#...........#
 #...........#...++.....o#...........#
 #...........#..++.....b.D...........#
 #.......o.o.#.++......o.#..o........#
 ##D########o##D#########o#########D##
 #..........o#..+........#...........#
 #...........#...+......+D...........#
 #...........#....+....++#+..........#
 #...........#.....+..++.#.+.........#
 #...........#......+++..#..+........#
 o...........#.......+...#...+.......#
 #...........#...........#....+.VVVV.#
 #...........#...........#.....+VVVV.#
 #...........#...........#......VVVV.#
 #.c.........#.........b.#...........#
 #......o....#.o.....o...#.........o.#
 #####################################


```

## Level 6: Weeping Halls

**Cathedral.** Route 157 tiles, 1035 floor cells, 4486 tiles to build on. 5 doors: 0 can't be bypassed, 5 on the short route with a way round (detour +12, +20, +28, +36, +44 tiles), of which a lock turns the party away from 4, 0 off the short route. Shut at any of 25 points along the route, 0 leave no way on; 10 have a real way round (+10 tiles or more, longest +44). 20 light sources, 24 props.

Six long galleries in a switchback, with doors through the walls as shortcuts. The party takes every shortcut unless locked out, so an open keep is a straight dash. A lock near a turn sends them round; on a door far from the turn the way round costs more than the lock, so they stop and break it, and stand where the player wants them while they do.

```

  ###############o#########o##########
  #..EEEE............................#
  #..EEEE............................#
  #..EEEE............................#
  #..EEEE............................#
  #.....+++++++++++++++++++++.......o#
  ##o#########o########o####D#####...#
  #........................++........#
  #.......................++.........#
  #.b....................++..........#
  #.....................++...........#
  #............o+++++++++............#
  #...##########D###################o#
  #..............+...................#
  #...............+..o..o............#
  #................+b................#
  #.................+................#
  o..................++++++..........#
  ########################D#######...#
  #................o.....++..........#
  #.....................++...........#
  o...................+++..........b.#
  #..................++.............o#
  #...........++++++++...............#
  #...########D#########o###########o#
  #............+.....................#
  #.............+....................#
  #...b..........+...................#
  #...............+..................#
  #.o.........o.o..++++++............#
  o#####################D#########..o#
  #...........o..........+...........#
  #.......................+...VVVV...#
  #........................+..VVVV...#
  #............o............+.VVVV...#
  #..........................++...o..#
  ####################################


```

## Level 7: Ashen Crypts

**Cathedral.** Route 92 tiles, 752 floor cells, 3376 tiles to build on. 8 doors: 1 can't be bypassed, 2 on the short route with a way round (detour +8, +10 tiles), of which a lock turns the party away from 2, 5 off the short route. Shut at any of 14 points along the route, 2 leave no way on; 1 have a real way round (+10 tiles or more, longest +10). 20 light sources, 29 props.

The vault sits at the centre of two rings, each with doors on opposite sides of it, so the party walks half of one ring and half of the other. The near door of each pair is the one to lock: that sends them the far way round, past everything built on it. Three tombs off the outer ring are for lairs that hit from behind.

```
              ############
              #...EEEE...#
              #...EEEE...#
              #...EEEE...#
              #...EEEE...#
              #.....++...#
              #o...++.o..#
     ########o#####D####o##########
     #............++..............#
     #...........++...............#
     #..++++++++++..o.............#
     #..+######o###############...#
     #..+#....................#...#
     #..+#....................#...#
     #..+#......o.......o.....#...#
     #..+D...##############...#...#
     o...#+..#............#...#...#
o#####...#.+.#............#...#...#o###
#....#...#..+#............D...#..o#...#
#.c..#...#..+#....VVVV....#..o#...#.b.#
#...o#...#..+#....VVVV....#...#...#...#
#....#...o..+#....VVVV...o#...#...#...#
#....D...#..+#...++.o.....#o..#...D...#
#....#...#..+#...+........#...#...#...#
#..b.#...#..+#....+......o#...#..o#..o#
#....#...#..+#o....+..o...#...#...#...#
######...#..+######D#######...#...#####
     #...#...+....++..........#...#
     o...#....+..++...........D...#
     #...#.....+++............#...#
     #...############o#########...#
     #............................#
     #............................#
     #................o...........#
     ##############D#####o#########
             #............#
             #.b........c.#
             o.o..........#
             ##############

```

## Level 8: Iron Gaol

**Cathedral.** Route 93 tiles, 620 floor cells, 2778 tiles to build on. 14 doors: 3 can't be bypassed, 3 on the short route with a way round (detour +44, +44, +44 tiles), of which a lock turns the party away from 0, 8 off the short route. Shut at any of 14 points along the route, 6 leave no way on; 6 have a real way round (+10 tiles or more, longest +44). 20 light sources, 29 props.

A U of corridors with gates across them and cell blocks between. One cell column has doors all the way through, so the short route takes three doors, and one lock on it is cheaper to break than the walk round. Two locks together send the party the long way, through every gate.

```








 #############o#######o###############
 #.EEEE.....#...........#............#
 #.EEEE.....#...........D............#
 #.EEEE....+D...........#............#
 #.EEEE++++.#++++++++..o#.o...o......#
 #####D###o##D######D######D####.....#
   #......#......#...+..#....o.#.....#
   #.b....#.c.o..#....+.#.....b#.....#
   #......#......#....++#......#.....#
   #......#......#...++.#......#.....#
   #..o...#......#..++.o#......#.....#
   #o#############o#D####o########D###
   #.....o#.....o#...+..#......#.....#
   #......#......#....+.#......#.....#
   #......#......#....++#......#.....#
   #.b....#......#...++.#.c...b#.....#
   #......#......#.o++..#.....oo.....#
 o####D##o###D######D####o#D####.....#
 #........#.....o#.++........o.......#
 #.......+D......#++.................#
 #.......+#+....+D+..................#
 #.......+#.+++++o......o..o........o#
 #......++############################
 #..VVVV+.#
 #..VVVV..#
 #..VVVV..#
 #........#
 #........#
 #.o.o....#
 ##########


```

## Level 9: Maggot Deeps

**Catacombs.** Route 83 tiles, 680 floor cells, 2926 tiles to build on. 3 doors: 0 can't be bypassed, 2 on the short route with a way round (detour +18, +18 tiles), of which a lock turns the party away from 2, 1 off the short route. Shut at any of 12 points along the route, 2 leave no way on; 5 have a real way round (+10 tiles or more, longest +18). 18 light sources, 36 props.

Tunnels off a central chamber. Two tunnels reach the deep room, one by each flank, and the longer is near enough that one lock on the short one turns the party away. The deep room is wide, so the vault is still a walk. First level in the Catacombs: the same rules in a new stone.

```


  #############   ####o######o#
  #..EEEE.....#   #...........#
  #..EEEE.....#   #.b........o#
  #..EEEE....oo####...........#
  #..EEEE+....................#
  #......++..++++++...........####o#
  #.......+++.#####+...............#
  #........+..#   #.+..............##
  #.....o.....#   o..+........####..#
  #############   #...+.....b.#  #..#
                  #..+....o.o.#  #..#
                  ###D#########  o..#
                    #++#         #..#
                    #.+#         #..#
                    #.+#         #..#
               ####o#.+#####o##  #..#
               #.......+.....o#  #..#
               #........+.....#  #..#
               #.........+....#  #..#
     #o#####o###.........+....#  #..#
     #.........D........+.....#  #..#
     #.........#.......+......#  #..#
     #..########.b....+......o#  o..#
     #..#      o.....+.....o..#  #..#
     #..#      ######D#########  #..#
     #..#           #++#         #..#
  #o##..##o#######o##.+##o#####o##..##
  #....................+.............#
  #.....................+............#
  #......................+....VVVV..o#
  #.......................+...VVVV...#
  #.................o......+..VVVV..o#
  #.........................+.VVVV...#
  o.c........................++......#
  #o...o..o...o..o....o..o...........#
  ####################################


```

## Level 10: Hollow Court

**Catacombs.** Route 96 tiles, 756 floor cells, 3256 tiles to build on. 4 doors: 2 can't be bypassed, 1 on the short route with a way round (detour +19 tiles), of which a lock turns the party away from 1, 1 off the short route. Shut at any of 14 points along the route, 4 leave no way on; 6 have a real way round (+10 tiles or more, longest +19). 18 light sources, 36 props.

A great court with a gatehouse on each flank and no door south, though the vault lies south. The court is the killing ground, overlooked from everywhere, and a lock on the near gate sends the party all the way across it to the far one. Lairs in both gatehouses, or none of it holds.

```
             o#############
             #...........o#
             #....EEEE....#
             #....EEEE....#
             #....EEEE....#
             #....EEEE+..o#
             #.......+....#
             o......+.....#
             #.o...+......#
 #o######o###o#####D#o######o#
 #........#.......+..........#
 #.c......#.b....+.........b.#
 #.......o#.....+...........o#
 #........#....+.............########o#
 #.......+D+..+..............#........#
 #.......+#+++...............#......c.#
 #......++#.+................#........#
 #.....++.o..................#........#
 #....++.o#.........o........#........#
 #...++...#.................oo........#
 ###.+#####..................#........#
   o.+#   #..................#........#
   #.+#   #..................D.......o#
   #.+#   #..................#.....o..#
   #.+#   #.b..............b.#####..###
   #.+#   oo...o.......o.....#   #..#
   #.+#   ####################   #..#
   #.+#                          #..#
   #.+#                          #..#
 #o#.+##o###############o#########..#o#
 #....+...............................#
 #.....+..............................#
 #.o....+++++++.....o........o........#
 #######o#####Do#######################
        #.....++....VVVV...#
        #......++...VVVV...#
        #.......++..VVVV...#
        #.....o..+++VVVV..o#
        ####################

```

## Level 11: Blood Galleries

**Catacombs.** Route 89 tiles, 930 floor cells, 3997 tiles to build on. 7 doors: 1 can't be bypassed, 3 on the short route with a way round (detour +21, +29, +29 tiles), of which a lock turns the party away from 3, 3 off the short route. Shut at any of 13 points along the route, 2 leave no way on; 8 have a real way round (+10 tiles or more, longest +29). 18 light sources, 36 props.

Three long galleries with cross-doors. Every route is about as long as every other, so the party moves easily: lock the door it would have taken and it switches gallery without a fight. So traps do little alone. They go where the routes meet, and the locks herd the party there.

```

 ############
 #..........o######o############o######
 #..EEEE....#.........................#
 #..EEEE....#..b......................#
 #..EEEE...+D+.......................o#
 #..EEEE+.+.#++.......................#
 #......++..#.++....................bo#
 #..........o..++++++.....o...o.......#
 #...o......#######oD#############D####
 ############      #+#           #.#
 o###########o######+#####o#######.##o#
 #...................+...............o#
 #.b..................+...............#
 #.....................+..............#
 #......................+.............#
 #.......................+............#
 #..o.....o...............+++++....o..#
 #####D#######################D########
     o.#                     o+#
 #####.#####o##########o######+#####o##
 #.............................+......#
 #..............................+.....#
 #...............................+....#
 #................................+...#
 #.................................+..#
 oobo............o...o............+.b.#
 #############D###################D####
             #.#                 o+#
   ###########.#o#################+##
   #.............................++.#
   #............................++..#
   o...........................++...#
   #........................VVVV....#
   #................o.......VVVV....#
   #........................VVVV....#
   #.c......................VVVV....#
   #o....o.....o.o..................#
   ##################################

```

## Level 12: Shrouded Nave

**Catacombs.** Route 111 tiles, 604 floor cells, 2664 tiles to build on. 7 doors: 2 can't be bypassed, 2 on the short route with a way round (detour +13, +13 tiles), of which a lock turns the party away from 2, 3 off the short route. Shut at any of 17 points along the route, 4 leave no way on; 6 have a real way round (+10 tiles or more, longest +13). 18 light sources, 32 props.

A cross-shaped church. The nave runs straight to the altar door, but both transepts have back doors into aisles that come round to the same place, and the last door is well off to one side of them all. Three routes of different lengths: two locks to control it, and not many to spare.

```
               ##########
               #........#
               #..EEEE..#
               #..EEEE.o#
               #..EEEE..#
               #..EEEE..#
               #....+...#
               #...+....#
               ####D###o#
               #...++..o#
               o.b..++..#
               #....+...#
               #...+....#
   o#######o####..+.....o########o###
   #...........#.+......#...........#
   #.b.........#+.......#.........b.#
   #..........+D........#...........#
   #.........+.#........#...........#
   #........+..o........D...........#
   #.c.....+...#....o...#.........c.#
   o.o...++..o.#........#.o.......o.#
   ######D######........#####oD######
        #++#   #........#    #..#
        o.+#   #.......o#    #..#
        #.+#   #........#    #..#
        #.+#   #.......o#    #..#
        #.+#   #........#    #..#
        #.+#   #......b.#    #..#
        #.+#   #.o....o.#    #..#
        #.+#   ####Do####    #..#
    #o###.+#o######..######o##..##o#
    #....+.........................#
    #...+..........................#
   o####D######o########o##############
   #....++....................VVVV....#
   #.....++...................VVVV....#
   #......++..................VVVV....#
   #.o...o.+++++++++++++++++++VVVV...o#
   ####################################

```

## Level 13: Gallows Reach

**Catacombs.** Route 80 tiles, 663 floor cells, 2922 tiles to build on. 6 doors: 0 can't be bypassed, 3 on the short route with a way round (detour +34, +34, +34 tiles), of which a lock turns the party away from 3, 3 off the short route. Shut at any of 12 points along the route, 0 leave no way on; 9 have a real way round (+10 tiles or more, longest +34). 18 light sources, 34 props.

A single long bridge over rock, a door at each end, runs straight at the vault. Around it go two long ways, past the gallows rooms, only a little shorter than a lock costs: one lock on the bridge sends the party the long way round, where the traps are. Lock the outer doors too, and it has to stop and break something, and the player has chosen what.

```

 o###########o#        o############o#
 #............#        #.............#
 #.b..........##########.c...........#
 #..................................o#
 #...................................#
 #..........b.##########.............#
 #o.....o.....#        #..o........b.#
 #####..#######        ############..#
     o..#                       o##..##
     #..#                       #.....#
     #..#                       #.....#
     #..#                       #....o#
     #..#                 #o#####.....#
 #####D#####              #.....#.....#
 #.EEEE....#              #....+D+....#
 #.EEEE....o#####o#####o###...+.#++..o#
 #.EEEE...+D+............+D+.+..#.++..#
 #.EEEE+.+.#+++++++++++++.#++...#..++.#
 #.....++..################.....o...++#
 #.........#              #.....#..o.+#
 #.........#              o.....#....+#
 #.....o..o#              #.....#....+#
 #####D#####              #.....#....+#
     #..#                 #.....#...++#
     #..#                 #....o#VVVV.#
     o..#                 #.....#VVVV.#
     #..#                 #.....#VVVV.#
     #..#                 #....o#VVVV.#
 #####..#######        o#####D#o#######
 #............#        #.............#
 #............#        #.............#
 #............##o#######.............#
 #...................................#
 #...................................#
 #.b..........##########.c.........b.#
 o..o.o.......#        o..o.o..o.....#
 ##############        ###############


```

## Level 14: Cinder Keep

**Catacombs.** Route 122 tiles, 1105 floor cells, 4802 tiles to build on. 11 doors: 1 can't be bypassed, 4 on the short route with a way round (detour +8, +8, +8, +22 tiles), of which a lock turns the party away from 4, 6 off the short route. Shut at any of 19 points along the route, 3 leave no way on; 1 have a real way round (+10 tiles or more, longest +22). 18 light sources, 36 props.

Three wards and a lane under them, then a keep of two halls, then a last lane that runs the whole width to the vault door. Gates and posterns give every stage a choice, and any two of them decide the whole route. The hardest of the door levels, and big enough to need a real garrison.

```

 ##############o#####################o#
 #...........o#...........#...........#
 #...EEEE.....#.b.........#...........#
 #...EEEE.....#..........o#...........#
 #...EEEE.....#...........#...........#
 #...EEEE+....D...........#...........#
 #.......++...#...........D...........#
 #........+...#...........o...........#
 #.......+...o#...........#...........#
 #......+.....#...........#.........b.#
 #..o..+......o...o......o#.o.....o...#
 o#####D####o#######D####o######D####o#
 #.....++.............................#
 #......+++...........................#
 #########D###################D######o#
 o........++......#...................#
 #.........++.....#.................b.#
 #..........++....#..................o#
 #...........++...#...................#
 #............++.+D+..................#
 #.............++.#++................o#
 #................o.++................#
 #.b.............o#..++...............#
 #..o....o........#...+++++++...o.....#
 #######D#o#######o#########Do######o##
 o..........................++........#
 #...........................++++++...#
 ##################o#########o####D####
   #.............................+....#
   #............................+.....#
   #...........................+.....o#
   #.....VVVV.................+.......#
   #.....VVVV........o.......+........#
   #.....VVVV...............+.........#
   #.....VVVV..............+..........#
   #........+.......c.....+.........c.#
   #.....o...+++++++++++++..o.........#
   ####################################

```

## Level 15: Howling Stair

**Caves.** Route 123 tiles, 776 floor cells, 2982 tiles to build on. 5 doors: 2 can't be bypassed, 1 on the short route with a way round (detour +46 tiles), of which a lock turns the party away from 0, 2 off the short route. Shut at any of 19 points along the route, 5 leave no way on; 2 have a real way round (+10 tiles or more, longest +46). 15 light sources, 33 props.

A long winding gully with one short cut through a tight neck halfway, and in the Caves a door is a fence with a gate in it. Gates on the gully and the short cut decide which way the party goes, and the last two gates are forced, so the gully is worth garrisoning.

```


  ###########         o########o######
  #..EEE....#         #.............o#
  #.oEEE....#         #.b............#
  #..EEE....######o####..............#
  #..EEE............o................#
  #.....+..+++......D................#
  #......+++++++.....................#
  #.......+.###++.#####..............#
  #.........# #.++#   #..............#
  #.o....o..# #..+#   #............b.#
  ########### #..+#   o..o...........#
              o..+#   ########...#####
              #.++#          o...#
              #.D.#          #..o#
              #.+.#          #.D.#
              #..+#          #...#
  ########o#  #..+#          #...#
  o.......o#  #..+######o#####...####o#
  #........#  #.++##..................#
  #........#  o++.##..................#
  #........####+..##..................#
  #..........++......................o#
  #........+D+........................#
  #......+++.....o....................#
  #.....++.#########.................o#
  #.b..++..#       #.......o..........#
  #.o..+...#       ####################
  ###.++####
    #oDo#
  #o#.+.o######o########o#############
  #....+....................+++++....#
  #.....+..................++.VVVV...#
  #......+.........o......++..VVVV...#
  #.......+..............++...VVVV...#
  #........+++++++++++++++...........#
  #.o.......+++++++++++++.....o......#
  ####################################

```

## Level 16: Tomb of Kings

**Caves.** Route 144 tiles, 774 floor cells, 2978 tiles to build on. 7 doors: 1 can't be bypassed, 3 on the short route with a way round (detour +0, +0, +10 tiles), of which a lock turns the party away from 3, 3 off the short route. Shut at any of 22 points along the route, 4 leave no way on; 2 have a real way round (+10 tiles or more, longest +10). 15 light sources, 33 props.

A chain of burial chambers, each with two gated necks to the next. A locked gate or a trap seen once pushes the party through the other neck, so they work in pairs, and the chambers between are where the party is caught either way.

```

 ##########o#  ###########o#
 #..........#  #...........#
 #..EEE....o####.b........o#
 #..EEE......D.............#
 #..EEE....................######
 #..EEE.....####..........+++...#
 #.....+....#  o.........++++++.#
 #......+..o####........++.###D.#
 #.......+.++D++.......++..# #+.#
 #........+++..+++++++++..o# o.+#
 #..o.......####.++++++....###.+#####o#
 ############  ##############..++.....#
        o######o####        #...++....#
        #.........o#        #....++..o#
        #.b........#        #....++...#
        #..........###o######...++....#
        #..............D.......++.....#
        #.........o...........++......#
        #..........##########++.......#
        #..........##########+........#
        o.............+D+..++.......b.#
        #.........+++++o.+++..o...o...#
        #........++####################
        #....o...+.#
        ###..##o++##
          #D.# #D.#
          #..# #+.#
   o#######..o##.+#####o###############
   #.............++.........+++++.....#
   #..............++.......+..VVVV....#
   #...............++.....+...VVVV....#
   #................++...+...oVVVV....#
   #.................++.+.............#
   #................o.++..............#
   #.b...............................o#
   o..............o....o.....o.....o..#
   ####################################


```

## Level 17: Silent Abbey

**Caves.** Route 108 tiles, 756 floor cells, 2896 tiles to build on. 4 doors: 2 can't be bypassed, 1 on the short route with a way round (detour +56 tiles), of which a lock turns the party away from 0, 1 off the short route. Shut at any of 16 points along the route, 5 leave no way on; 3 have a real way round (+10 tiles or more, longest +56). 15 light sources, 33 props.

A wide cloister with a rock island in the middle, and the vault below it on the side the party starts from, with no way down there. It has to go round, by the gated middle walk or the far side, and a lock on the middle sends it the whole way round, so the army can stand in one place.

```

 ##############    #o#######o########o#
 #...........o#    #.................o#
 #...EEE......#    #.b................#
 #...EEE......#    #..................#
 #...EEE......#    #..................#
 #...EEE.....o#    #..................#
 #......+.....#    #................b.#
 #.......+....#    #..................#
 #........+...#    #############...####
 #.........+..o######o##########...#o##
 #..........+.........................#
 #...........+........................#
 #............+++.....o...............#
 o.o..o....o...++++...........o.......#
 #################++.###########......#
                 #.++#         #.....o#
                 #..+#         #......#
                 #..+#         #......#
                 #.++#         o......#
                 #oD.#         #..D...#
                 #.+.#         #o.....#
                 #..+#         #......#
                 #..+#         #......#
                 #.++#         #......#
                 #++.#         #......#
               ###+..#####o#####......#
       ######o##++....................#
       #.......++.....................#
       #..+D+.++.....................b#
       #.++..++.o..............o...o..#
       #.D.############################
   o####.+.o#####o######o##########
   #......+..............VVVV....o#
   #.......+.............VVVV.....#
   #........+............VVVV....o#
   #.........++++++++++++++.......#
   #...o..o...++++++++++++.o......#
   ################################

```

## Level 18: Chains of Night

**Hell.** Route 184 tiles, 1012 floor cells, 4191 tiles to build on. 0 doors: 0 can't be bypassed, 0 on the short route with a way round, 0 off the short route. Shut at any of 29 points along the route, 0 leave no way on; 3 have a real way round (+10 tiles or more, longest +16); a Barricade fits at 8 of them. 18 light sources, 29 props.

Long chained halls in a switchback, with a broad neck at each turn and one bypass through the middle wall. No doors in Hell: the Barricade bars a neck instead, wall to wall, for what a lock costs. Each turn is a place to bar, and shoulders of rock narrow every hall to one more.

```

  #############   o############o######
  #.EEEE......#   #..................#
  #.EEEE......#####...............b..#
  #.EEEE.............................#
  #.EEEE.........o................o..#
  #.EEEE+............................#
  #......+...........................#
  #.......+++++++++++++++.......o....#
  #######################+...####....#
  ######o##   ######o####+...#o##....#
  #.......#   o........++............#
  #.......#####.......++...o.........#
  #..................++..............#
  #.................++...............#
  #................++...............o#
  #..b...........+++.................#
  #...++++++++++++...................#
  o...+###############################
  #...+###o######o#   o######o######o#
  #....+..........#   #....o.........#
  #.....+.........#####..............#
  #......+........o..o...............#
  #.......+..........................#
  #........+.........................#
  #.........+..........++..........b.#
  o..........++++++++++++++++++++....#
  ###############################+...#
  ##o#######o######o#   ####o####+...#
  #.................#   #......++....#
  #.................#####.....++.....#
  #..VVVVV...................++......#
  #..VVVVV..................++.......#
  #..VVVVV.................++........#
  #..VVVVV................++.........#
  #..VVVVV+.........o....++..........#
  o.......++..++........++...........#
  #........++++.+++++++++........o.o.#
  ####################################

```

## Level 19: Burning Sepulcre

**Hell.** Route 154 tiles, 825 floor cells, 3459 tiles to build on. 0 doors: 0 can't be bypassed, 0 on the short route with a way round, 0 off the short route. Shut at any of 24 points along the route, 0 leave no way on; 1 have a real way round (+10 tiles or more, longest +11); a Barricade fits at 21 of them. 18 light sources, 31 props.

A burial hall ringed by a broad ambulatory, with the vault below and behind it. The hall has two necks in, and the ring is cut on the east, so the party crosses the hall or walks the whole ring, and the only way down is at the far corner, with the vault back at the other end. Traps herd rather than block: there is always another way on, and lairs hold the ring.

```
             ##############
             #....EEEE....#
             o....EEEE....#
             #....EEEE....#
             #.o..EEEE....#
             #....EEEE....#
             #......++....#
             #.....++.....#
             #####++..#####
                 #+...#
  o##########o####+...###o########o#
  #.............++................o#
  #.b..........++................b.#
  #...........++...................#
  #..........++....................#
  #....+++++++.....................#
  #....+############################
  #....+#  o########o########
  #o...+#  #................#
  #....+####................#o########
  #.....+............................#
  o.....++..........................o#
  #....++.o.................o........#
  #....+####.......o........####o....#
  #....+#  o................#  #.....#
  #....+#  ##################  #.....#
  #....+##o#######o#########o###.....#
  #.....+............................#
  #......+...........................#
  #.......++........o................#
  #.o......++......................b.#
  o....o.....++++++++++++++++++++....#
  o###############o#########o####+..o#
  #...VVVVV....................++....#
  #...VVVVV...................++.....#
  #...VVVVV......o.o........+++......#
  #...VVVVV................++........#
  #...VVVVV+++++++++++++++++.........#
  ####################################

```

## Level 20: Throne of Ash

**Hell.** Route 204 tiles, 1082 floor cells, 4541 tiles to build on. 0 doors: 0 can't be bypassed, 0 on the short route with a way round, 0 off the short route. Shut at any of 32 points along the route, 0 leave no way on; 6 have a real way round (+10 tiles or more, longest +16); a Barricade fits at 22 of them. 18 light sources, 34 props.

The last keep: the longest route in the campaign, five halls in a switchback with a broad neck at each turn and bypasses through the walls. Nothing can be sealed and only the last neck has no way round, so it asks for everything learned so far, with barricades for locks, and ends in a throne room big enough for a last stand.

```
 ################o#########o#########o#
 #.EEEE.........o................o....#
 #.EEEE.....o...............o.......b.#
 #.EEEE+..............................#
 #.EEEE++...................o.........#
 #.EEEE.++............................#
 #....o..+++++++++++++++++............#
 #########################+..####.....#
 ######o##########o#######+..o###.....#
 #......................++............#
 #.....................++.............#
 #....................++..............#
 #.b.................++...............#
 #.........+++++++++++..o.............#
 #.....##..+###########################
 #.....##..+#######o##############o####
 #..........+.........................#
 #...........++...o...................#
 #............++....................b.#
 #..............++....................#
 o...............+++++++++...o........#
 #########################+..####.....#
 o############o########o##+..####....o#
 #.....................++.............#
 #....................++..............#
 #.b........o......o.++...............#
 #..................+.................#
 #....++++++++++++++..................#
 #....+################################
 #....+#########o########o#############
 #.....+..............................#
 #......+..........##.....##.........o#
 #.......+.........##....o##..VVVVV...#
 #........+...++..............VVVVV...#
 #.........++++++.............VVVVV...#
 o..........##...+.##.....##..VVVVV...#
 #.b........o#....+o#.....o#..VVVVV.b.#
 #..............o..+++++++++++++...o..#
 ######################################

```

