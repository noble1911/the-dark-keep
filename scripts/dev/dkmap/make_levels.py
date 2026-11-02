#!/usr/bin/env python3
"""Writes the campaign's level sources (levels/level_NN.txt), which dkmap_tool turns into .dkmap files.

The layouts are designed here rather than typed row by row, so a room can be moved without redrawing
a level. x runs right and y runs down, in megatiles, 1 to 38. The text files are the source of truth
for the tool and can also be edited by hand; rerunning this overwrites them.

What the rules below come from (see docs/CAMPAIGN-MAPS.md):
- Rooms sharing a wall need exactly one rock cell between them, so that a door can sit in it. A door
  needs floor on two opposite sides and non-floor on the other two.
- A lock costs a hero's route 40 tiles, so a way round that is shorter than that makes them turn away,
  and a longer one makes them stop and break the door. Both are useful and the levels use both.
- In the Caves a door is a fence right across the cave with a gate in it: mark the cell in a straight stretch
  two or more cells wide. Hell has no doors at all: the Lock tool makes a barricade across any neck instead.
"""
import os


class Level:
    def __init__(self, number, name, tileset, seed, note):
        self.number = number
        self.name = name
        self.tileset = tileset
        self.seed = seed
        self.note = note
        self.grid = [['#'] * 40 for _ in range(40)]

    def room(self, x, y, w, h):
        for j in range(y, y + h):
            for i in range(x, x + w):
                self.grid[j][i] = '.'
        return self

    def hall_h(self, y, x1, x2, width=2):
        """Floor from x1 to x2 inclusive, on rows y to y + width - 1."""
        return self.room(min(x1, x2), y, abs(x2 - x1) + 1, width)

    def hall_v(self, x, y1, y2, width=2):
        """Floor from y1 to y2 inclusive, on columns x to x + width - 1."""
        return self.room(x, min(y1, y2), width, abs(y2 - y1) + 1)

    def rock(self, x, y, w=1, h=1):
        for j in range(y, y + h):
            for i in range(x, x + w):
                self.grid[j][i] = '#'
        return self

    def door(self, x, y):
        self.grid[y][x] = 'D'
        return self

    def entrance(self, x, y):
        self.grid[y][x] = 'E'
        return self

    def vault(self, x, y):
        self.grid[y][x] = 'V'
        return self

    def put(self, kind, *cells):
        for x, y in cells:
            self.grid[y][x] = kind
        return self

    def write(self, folder):
        with open(os.path.join(folder, f'level_{self.number:02}.txt'), 'w', newline='\n') as out:
            out.write(f'name: {self.name}\ntileset: {self.tileset}\nseed: {self.seed}\n')
            out.write(f'note: {self.note}\n')
            for row in self.grid:
                out.write(''.join(row) + '\n')


LEVELS = []


def level(number, name, tileset, seed, note):
    made = Level(number, name, tileset, seed, note)
    LEVELS.append(made)
    return made


# ---- Act one: the Cathedral. Doors everywhere; one idea per level. ------------------------------

l = level(1, 'Old Cellars', 'Cathedral', 0x1001,
          'One way in, two doors on it. Nothing to work out but where to stand: the hall is the place '
          'for traps, the door the place for a first lock, and the two closets take lairs out of the way.')
l.room(14, 3, 12, 8).entrance(18, 3)
l.hall_v(19, 11, 16)
l.door(19, 17)
l.room(10, 18, 20, 8)
l.room(3, 19, 6, 6).door(9, 21)
l.room(31, 19, 6, 6).door(30, 22)
l.door(20, 26)
l.room(13, 27, 14, 9).vault(18, 30)
l.put('b', (4, 20), (5, 23))
l.put('c', (35, 20))

l = level(2, 'Rat Warrens', 'Cathedral', 0x1002,
          'A long S with doors in a row and blind burrows off it. Still only one route, but now there '
          'is distance to spend: layers of defence, and burrows that hide lairs so that they strike the '
          'party from the side.')
l.room(3, 3, 9, 8).entrance(5, 3)
l.hall_h(6, 12, 24)
l.door(25, 6)
l.room(26, 3, 11, 8)
l.hall_v(30, 11, 16)
l.room(24, 17, 13, 8)
l.door(23, 20)
l.hall_h(20, 13, 22)
l.door(12, 20)
l.room(3, 17, 9, 8)
l.hall_v(6, 25, 29)
l.room(3, 30, 13, 7).vault(7, 32)
# burrows: three blind rooms off the route
l.hall_v(16, 8, 11)
l.room(13, 12, 9, 6)
l.hall_v(17, 22, 24, 1)
l.door(17, 25)
l.room(14, 26, 8, 3)
l.door(16, 33)
l.room(17, 30, 20, 7)
l.put('b', (14, 13), (20, 16), (15, 27), (35, 35))
l.put('c', (20, 13), (35, 31))

l = level(3, 'Sunken Chapel', 'Cathedral', 0x1003,
          'The first loop. Two aisles reach the chancel, one shorter, and a side chapel links them. A '
          'lock on the short aisle turns the party down the long one, so a single lock chooses which '
          'hall the traps go in. Both aisles meet at the one last door.')
l.room(11, 2, 10, 7).entrance(13, 2)
l.door(10, 5)
l.hall_v(7, 4, 26, 3)
l.door(21, 5)
l.hall_h(5, 22, 27)
l.hall_v(28, 4, 26, 3)
l.room(11, 12, 16, 12)
l.door(10, 17)
l.door(27, 20)
l.room(7, 27, 24, 4)
l.door(13, 31)
l.room(7, 32, 16, 6).vault(13, 33)
l.put('b', (12, 13), (25, 13), (12, 22), (25, 22))
l.put('i', (18, 17))

l = level(4, 'Bone Pits', 'Cathedral', 0x1004,
          'A hub with pits around it. The party crosses the central pit whatever it does, so whatever '
          'stands there sees it. Two ways lead on from the hub, by the east pit and by the south pit, '
          'and the north and west pits are dead ends: places for lairs, not places anyone need go.')
l.room(2, 2, 10, 7).entrance(4, 2)
l.hall_h(5, 12, 21)
l.door(22, 5)
l.room(23, 2, 12, 8)
l.hall_v(19, 7, 12)
l.door(19, 13)
l.room(14, 14, 12, 12)
l.door(13, 19)
l.room(2, 14, 11, 12)
l.door(26, 17)
l.room(27, 13, 11, 12)
l.door(33, 25)
l.door(15, 26)
l.room(12, 27, 14, 8)
l.door(26, 33)
l.room(27, 26, 11, 12).vault(30, 29)
l.put('b', (24, 3), (33, 8), (3, 15), (11, 24))
l.put('c', (33, 3), (3, 24))

l = level(5, 'Black Vaults', 'Cathedral', 0x1005,
          'Nine strongrooms joined by doors in a lattice, the doors set far apart so each room has to be '
          'crossed. The door count is the point: gold for locks runs out long before doors do, and every '
          'lock moves the route somewhere else, so the player is choosing a path through the keep.')
for (cx, cy) in [(2, 2), (14, 2), (26, 2), (2, 14), (14, 14), (26, 14), (2, 26), (14, 26), (26, 26)]:
    l.room(cx, cy, 11, 11)
l.entrance(3, 2).vault(32, 32)
# across
l.door(13, 11).door(25, 3).door(13, 15).door(25, 23).door(25, 27)
# down
l.door(3, 13).door(35, 13).door(23, 13).door(3, 25).door(15, 25).door(35, 25)
l.put('c', (35, 3), (3, 35))
l.put('b', (15, 15), (23, 23), (15, 3), (23, 35))

l = level(6, 'Weeping Halls', 'Cathedral', 0x1006,
          'Six long galleries in a switchback, with doors through the walls as shortcuts. The party '
          'takes every shortcut unless locked out, so an open keep is a straight dash. A lock near a turn '
          'sends them round; on a door far from the turn the way round costs more than the lock, so they '
          'stop and break it, and stand where the player wants them while they do.')
for y in (2, 8, 14, 20, 26, 32):
    l.room(3, y, 34, 5)
l.entrance(5, 2).vault(30, 33)
# the turns
l.room(34, 7, 3, 1).room(3, 13, 3, 1).room(34, 19, 3, 1).room(3, 25, 3, 1).room(34, 31, 3, 1)
# the shortcuts
l.door(28, 7).door(16, 13).door(26, 19).door(14, 25).door(24, 31)
l.put('b', (4, 10), (20, 16), (6, 28), (35, 22))
l.put('i', (19, 10), (19, 34))

l = level(7, 'Ashen Crypts', 'Cathedral', 0x1007,
          'The vault sits at the centre of two rings, each with doors on opposite sides of it, so the '
          'party walks half of one ring and half of the other. The near door of each pair is the one to '
          'lock: that sends them the far way round, past everything built on it. Three tombs off the outer '
          'ring are for lairs that hit from behind.')
l.room(15, 1, 10, 6).entrance(18, 1)
l.door(19, 7)
# outer ring
l.room(6, 8, 28, 3).room(6, 31, 28, 3).room(6, 8, 3, 26).room(31, 8, 3, 26)
l.door(9, 15).door(30, 28)
# inner ring
l.room(10, 12, 20, 3).room(10, 27, 20, 3).room(10, 12, 3, 18).room(27, 12, 3, 18)
l.door(26, 18).door(19, 26)
l.room(14, 16, 12, 10).vault(18, 19)
# tombs
l.room(1, 18, 4, 8).door(5, 22)
l.room(35, 18, 3, 8).door(34, 22)
l.room(14, 35, 12, 3).door(19, 34)
l.put('c', (2, 19), (24, 36))
l.put('b', (3, 24), (36, 19), (15, 36))

l = level(8, 'Iron Gaol', 'Cathedral', 0x1008,
          'A U of corridors with gates across them and cell blocks between. One cell column has doors all '
          'the way through, so the short route takes three doors, and one lock on it is cheaper to break '
          'than the walk round. Two locks together send the party the long way, through every gate.')
# north corridor, gates at x 12 and 24
l.room(2, 9, 30, 4).entrance(3, 9)
l.rock(12, 9, 1, 4).door(12, 11)
l.rock(24, 9, 1, 4).door(24, 10)
# the turn, with a gate across it
l.room(32, 9, 5, 21)
l.rock(32, 19, 5, 1).door(34, 19)
# south corridor, gates at x 17 and 10
l.room(2, 26, 30, 4)
l.rock(17, 26, 1, 4).door(17, 28)
l.rock(10, 26, 1, 4).door(10, 27)
l.room(2, 30, 8, 7).vault(4, 31)
# the cell block
for x in (4, 11, 18, 25):
    l.room(x, 14, 6, 5).door(x + 2, 13)
    l.room(x, 20, 6, 5).door(x + 2, 25)
l.door(20, 19)
l.put('b', (5, 15), (30, 15), (5, 23), (30, 23))
l.put('c', (12, 15), (26, 23))
l.put('i', (34, 12), (34, 22))

# ---- Act two: the Catacombs. Doors still, and the loops get bigger. -----------------------------

l = level(9, 'Maggot Deeps', 'Catacombs', 0x2009,
          'Tunnels off a central chamber. Two tunnels reach the deep room, one by each flank, and the longer '
          'is near enough that one lock on the short one turns the party away. The deep room is wide, so '
          'the vault is still a walk. '
          'First level in the Catacombs: the same rules in a new stone.')
l.room(3, 3, 11, 8).entrance(5, 3)
l.hall_h(6, 14, 18)
l.room(19, 3, 11, 10)
l.door(21, 13)
l.hall_v(21, 14, 17)
l.room(16, 18, 14, 8)
# the east tunnel, long
l.hall_h(8, 30, 34)
l.hall_v(34, 10, 28)
# the west tunnel, by the near flank
l.door(15, 22)
l.hall_h(22, 7, 14)
l.hall_v(6, 22, 28)
l.room(3, 29, 34, 8).vault(30, 31)
l.door(21, 26)
l.hall_v(21, 27, 28)
l.put('b', (20, 4), (28, 11), (17, 24))
l.put('c', (4, 35))

l = level(10, 'Hollow Court', 'Catacombs', 0x200A,
          'A great court with a gatehouse on each flank and no door south, though the vault lies south. '
          'The court is the killing ground, overlooked from everywhere, and a lock on the near gate sends '
          'the party all the way across it to the far one. Lairs in both gatehouses, or none of it holds.')
l.room(14, 1, 12, 8).entrance(18, 2)
l.door(19, 9)
l.room(11, 10, 18, 16)
l.door(10, 14)
l.room(2, 10, 8, 10)
l.door(29, 22)
l.room(30, 14, 8, 10)
l.hall_v(4, 20, 29)
l.hall_v(34, 24, 29)
l.room(2, 30, 36, 3)
l.door(14, 33)
l.room(9, 34, 18, 4).vault(20, 34)
l.put('b', (12, 11), (27, 11), (12, 24), (27, 24))
l.put('c', (3, 11), (36, 15))

l = level(11, 'Blood Galleries', 'Catacombs', 0x200B,
          'Three long galleries with cross-doors. Every route is about as long as every other, so the '
          'party moves easily: lock the door it would have taken and it switches gallery without a fight. '
          'So traps do little alone. They go where the routes meet, and the locks herd the party there.')
l.room(2, 2, 10, 8).entrance(4, 3)
l.door(12, 5)
l.room(13, 3, 25, 6)
l.room(2, 12, 36, 6)
l.room(2, 21, 36, 6)
l.door(20, 9).hall_v(20, 10, 11, 1)
l.door(34, 9).hall_v(34, 10, 11, 1)
l.door(6, 18).hall_v(6, 19, 20, 1)
l.door(30, 18).hall_v(30, 19, 20, 1)
l.door(14, 27).hall_v(14, 28, 29, 1)
l.door(34, 27).hall_v(34, 28, 29, 1)
l.room(4, 30, 32, 8).vault(28, 33)
l.put('b', (15, 4), (36, 7), (3, 13), (36, 26), (3, 26))
l.put('c', (5, 36))

l = level(12, 'Shrouded Nave', 'Catacombs', 0x200C,
          'A cross-shaped church. The nave runs straight to the altar door, but both transepts have back '
          'doors into aisles that come round to the same place, and the last door is well off to one side '
          'of them all. Three routes of different lengths: two locks to control it, and not many to spare.')
l.room(16, 1, 8, 7).entrance(18, 2)
l.door(19, 8)
l.room(16, 9, 8, 20)
l.room(4, 14, 11, 7).door(15, 16)
l.room(25, 14, 11, 7).door(24, 18)
l.door(9, 21)
l.door(30, 21)
l.hall_v(9, 22, 30)
l.hall_v(30, 22, 30)
l.door(19, 29)
l.hall_v(19, 30, 30)
l.room(5, 31, 30, 2)
l.door(8, 33)
l.room(4, 34, 34, 4).vault(30, 34)
l.put('b', (5, 15), (34, 15), (17, 10), (22, 27))
l.put('c', (5, 19), (34, 19))

l = level(13, 'Gallows Reach', 'Catacombs', 0x200D,
          'A single long bridge over rock, a door at each end, runs straight at the vault. Around it go two '
          'long ways, past the gallows rooms, only a little shorter than a lock costs: one lock on the '
          'bridge sends the party the long way round, where the traps are. Lock the outer doors too, and '
          'it has to stop and break something, and the player has chosen what.')
l.room(2, 15, 9, 8).entrance(3, 15)
l.door(11, 17)
l.hall_h(17, 12, 25)
l.door(26, 17)
l.room(27, 14, 5, 15)
l.door(32, 15)
l.room(33, 10, 5, 19).vault(33, 25)
# north: long, and into the vault's room from behind
l.door(6, 14)
l.hall_v(6, 8, 13)
l.room(2, 2, 12, 6)
l.hall_h(4, 14, 23)
l.room(24, 2, 13, 6)
l.hall_v(35, 8, 9, 2)
# south: long, and it still has the last door to pass
l.door(6, 23)
l.hall_v(6, 24, 29)
l.room(2, 30, 12, 7)
l.hall_h(33, 14, 23)
l.room(24, 30, 13, 7)
l.door(29, 29)
l.put('b', (3, 3), (12, 6), (35, 7), (3, 35), (35, 35))
l.put('c', (25, 3), (25, 35))

l = level(14, 'Cinder Keep', 'Catacombs', 0x200E,
          'Three wards and a lane under them, then a keep of two halls, then a last lane that runs the whole '
          'width to the vault door. Gates and posterns give every stage a choice, and any two of them decide '
          'the whole route. The hardest of the door levels, and big enough to need a real garrison.')
l.room(2, 2, 12, 10).entrance(5, 3)
l.door(14, 6)
l.room(15, 2, 11, 10)
l.door(26, 7)
l.room(27, 2, 11, 10)
# the lane
l.hall_h(13, 2, 37)
l.door(7, 12).door(20, 12).door(32, 12)
# the keep
l.door(10, 15)
l.room(2, 16, 16, 9)
l.door(18, 20)
l.room(19, 16, 19, 9)
l.door(30, 15)
# the last lane
l.hall_h(26, 2, 37)
l.door(8, 25).door(28, 25)
l.door(34, 28)
l.room(4, 29, 34, 9).vault(9, 32)
l.put('b', (16, 3), (36, 10), (3, 23), (36, 17))
l.put('c', (36, 36), (20, 36))

# ---- Act three: the Caves. No doors: necks and chambers. ----------------------------------------

l = level(15, 'Howling Stair', 'Caves', 0x300F,
          'A long winding gully with one short cut through a tight neck halfway, and in the Caves a door is '
          'a fence with a gate in it. Gates on the gully and the short cut decide which way the party goes, '
          'and the last two gates are forced, so the gully is worth garrisoning.')
l.room(3, 3, 9, 9).entrance(5, 3)
l.hall_h(6, 12, 22, 3)
l.room(23, 3, 14, 10)
l.hall_v(30, 13, 19, 3)
l.room(20, 20, 18, 8)
l.hall_h(23, 8, 19, 3)
l.room(3, 19, 8, 10)
l.hall_v(5, 29, 31, 3)
l.room(3, 32, 34, 6).vault(30, 33)
l.hall_v(15, 9, 22, 3)
l.door(20, 7).door(16, 15).door(31, 16).door(12, 24).door(6, 30)
l.put('b', (24, 4), (35, 11), (4, 27))

l = level(16, 'Tomb of Kings', 'Caves', 0x3010,
          'A chain of burial chambers, each with two gated necks to the next. A locked gate or a trap seen '
          'once pushes the party through the other neck, so they work in pairs, and the chambers between '
          'are where the party is caught either way.')
l.room(2, 2, 10, 10).entrance(4, 3)
l.room(16, 2, 11, 10)
l.hall_h(4, 12, 15, 2)
l.hall_h(9, 12, 15, 2)
l.room(29, 12, 9, 11)
l.hall_v(30, 6, 11, 2)
l.hall_h(6, 27, 29, 2)
l.hall_h(17, 19, 28, 2)
l.hall_h(21, 19, 28, 2)
l.room(9, 14, 10, 11)
l.hall_v(11, 25, 28, 2)
l.hall_v(16, 25, 28, 2)
l.room(4, 29, 34, 8).vault(30, 30)
l.door(13, 4).door(13, 9).door(30, 8).door(23, 17).door(23, 21).door(11, 26).door(16, 26)
l.put('b', (17, 3), (36, 21), (10, 15), (5, 35))

l = level(17, 'Silent Abbey', 'Caves', 0x3011,
          'A wide cloister with a rock island in the middle, and the vault below it on the side the party '
          'starts from, with no way down there. It has to go round, by the gated middle walk or the far side, '
          'and a lock on the middle sends it the whole way round, so the army can stand in one place.')
l.room(2, 2, 12, 9).entrance(5, 3)
l.room(2, 11, 36, 4)
l.room(32, 11, 6, 20)
l.room(16, 27, 22, 4)
l.hall_v(18, 15, 26, 3)
l.room(20, 2, 18, 7)
l.hall_v(32, 9, 10, 3)
l.hall_h(28, 8, 15, 3)
l.hall_v(8, 31, 32, 3)
l.room(4, 33, 30, 5).vault(25, 33)
l.door(19, 20).door(34, 20).door(11, 29).door(9, 31)
l.put('b', (21, 3), (36, 7), (37, 29))

# ---- Act four: Hell. No doors, the longest routes, the most ways through. ------------------------

l = level(18, 'Chains of Night', 'Hell', 0x4012,
          'Long chained halls in a switchback, with a broad neck at each turn and one bypass through the '
          'middle wall. No doors in Hell: the Barricade bars a neck instead, wall to wall, for what a lock costs. '
          'A long, plain, brutal route, and each turn is a place to bar.')
l.room(3, 2, 34, 7).entrance(4, 2)
l.room(33, 9, 4, 2)
l.room(3, 11, 34, 7)
l.room(3, 18, 4, 2)
l.room(25, 9, 4, 2)
l.room(3, 20, 34, 7)
l.room(33, 27, 4, 2)
l.room(3, 29, 34, 9).vault(5, 31)
l.put('b', (34, 3), (5, 16), (35, 25))

l = level(19, 'Burning Sepulcre', 'Hell', 0x4013,
          'A burial hall ringed by a broad ambulatory, with the vault below and behind it. The hall has '
          'two necks in, and the ring is cut on the east, so the party crosses the hall or walks the whole ring, '
          'and the only way down is at the far corner, with the vault back at the other end. Traps herd '
          'rather than block: there is always another way on, and lairs hold the ring.')
l.room(14, 1, 12, 7).entrance(18, 1)
l.hall_v(18, 8, 10, 4)
l.room(3, 11, 32, 5)
l.room(3, 11, 5, 21)
l.room(32, 20, 5, 12)
l.room(3, 27, 34, 5)
l.room(12, 18, 16, 7)
l.hall_h(20, 8, 11, 3)
l.hall_h(20, 28, 31, 3)
l.hall_v(33, 32, 33, 3)
l.room(3, 33, 34, 5)
l.vault(6, 33)
l.put('b', (4, 12), (33, 12), (4, 30), (35, 30))

l = level(20, 'Throne of Ash', 'Hell', 0x4014,
          'The last keep: the longest route in the campaign, five halls in a switchback with a broad neck at '
          'each turn and bypasses through the walls. Nothing can be sealed and only the last neck has no way '
          'round, so it asks for everything learned so far, with barricades for locks, and ends in a throne room big '
          'enough for a last stand.')
l.room(2, 1, 36, 6).entrance(3, 1)
l.room(33, 7, 5, 2)
l.room(2, 9, 36, 5)
l.room(26, 7, 3, 2)
l.room(2, 14, 5, 2)
l.room(2, 16, 36, 5)
l.room(9, 14, 3, 2)
l.room(33, 21, 5, 2)
l.room(2, 23, 36, 5)
l.room(26, 21, 3, 2)
l.room(2, 28, 5, 2)
l.room(2, 30, 36, 8).vault(30, 32)
l.put('b', (36, 2), (3, 12), (36, 18), (3, 25), (36, 36), (3, 36))


if __name__ == '__main__':
    here = os.path.join(os.path.dirname(os.path.abspath(__file__)), 'levels')
    os.makedirs(here, exist_ok=True)
    for made in LEVELS:
        made.write(here)
