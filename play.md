# How to play GB-Slingshot

A slingshot puzzle game for the original Game Boy. Every level gives you a
fixed number of shots. Destroy every target before the shots run out.

There are 50 levels. You can start at any level from the level select; your best
star rating for each one is saved.

## Getting started

1. Build the ROM (see the [README](README.md#building)) or load
   `build/slingshot.gb` in an emulator such as mGBA.
2. Press **A** or **START** on the title screen to open the level select.
3. Use **left/right** to choose a level and press **A** or **START** to start it.

The level select opens on the first level you have not beaten. A level that
asks for a particular projectile shows `NEEDS B`, `NEEDS C` or `NEEDS D`.

## Controls

### Aiming and firing

| Input | Action |
|---|---|
| **Up / Down** | Raise or lower the launch angle, from 5° to 80° in 5° steps |
| **Left / Right** | Increase or decrease power, in 8 steps |
| **B** | Cycle the projectile type |
| **A** | Fire |

The dotted arc shows where the shot would fly if the level were empty. It
ignores structures on purpose: it is the clean trajectory, not a prediction of
what the ball will hit or how it will bounce through a wall.

### During a shot

| Input | Action |
|---|---|
| **A** | Spend the armed projectile's ability, once per shot |

There is no undo. If you have already spent the ability, pressing **A** again
does nothing.

### Menus

| Input | Action |
|---|---|
| **START** | Restart the level; after a win, bank the stars and load the next level |
| **SELECT** | Return to the level select |

## Reading the HUD

The bar along the bottom of the screen shows:

- **shots remaining**;
- **targets left** — how many targets must still be destroyed;
- the **projectile letter** currently armed (`A`, `B`, `C` or `D`);
- **SCORE**.

When the level ends, a splash screen replaces the play field. Press **START**
to continue.

## Stars and score

Stars are awarded on a win according to how many shots you have left:

| Shots left at the win | Stars |
|---|---|
| 2 or more | ★★★ |
| 1 | ★★ |
| 0 | ★ |

Only your best result for a level is kept. Most levels have a budget of two or
three shots, so finishing on the first shot is usually what earns the top
rating — on a two-shot level, the maximum is two stars. Progress is stored in
the cartridge's battery-backed RAM and survives a reset.

Score is separate from stars and does not affect progression. Destroyed cells
are worth:

| Cell | Points |
|---|---|
| Wood | 10 |
| Stone | 20 |
| Ice | 5 |
| Target | 100 |

A cell destroyed by a **collapse** — because something fell on it, or because it
fell far enough to break — scores **double**. When that happens, the HUD slides
up for a moment and shows `CHAIN +nn`.

## Materials

| Material | Hit points | Damage taken |
|---|---|---|
| **Wood** (brown) | 4 | Normal |
| **Stone** (grey) | 4 | Half |
| **Ice** (blue) | 2 | Double |
| **Target** (bullseye) | 2 | Normal |
| **Ground** | — | Indestructible |

A hit that destroys a cell lets the shot carry on, a quarter slower. A hit that
only damages a cell stops the shot there. Damaged cells stay cracked, so a cell
that took a hit is visibly weaker and one more solid impact may finish it.

Because speed decides damage, a shot that has already punched through a wall
arrives weaker than one that took a clean line. Stone shrugs off half of what
hits it; ice shatters easily.

## Collapse

The structures are not static. After every shot the game checks the board from
the bottom up:

- A cell with nothing underneath falls one row.
- A cell with a solid horizontal neighbour on **both** sides can bridge the gap
  and stay put until one of those supports goes.
- A cell that falls **two or more rows** damages whatever it lands on and takes
  damage itself.
- A stack that falls together does not damage itself.
- A fall of three or more rows destroys a target.

This is often the real way to win a level: the target may be too tough or too
well covered to hit directly, but the tower above it is not. Break the keystone
and let gravity do the work.

## Projectiles

Press **B** before firing to choose a type. Every type flies like the standard
shot; the difference is the one ability you can trigger with a second **A**
press while the shot is in the air.

| | Type | Ability |
|---|---|---|
| **A** | Standard | None. |
| **B** | Boost | The shot instantly speeds up by 50% in the same direction. Faster means one damage bucket more and more range. |
| **C** | Split | The shot fans into three, the two new shards angled slightly off the parent's heading. Each shard collides independently, so one shot can cover several targets or hit the same structure from three lines. |
| **D** | Pierce | The ball becomes a drill. For the rest of the flight it grinds through the cells it dents instead of stopping on the first one. It still cannot pass through the ground. |

The ability can be spent only once per shot. A split shot's shards do **not**
each get their own ability press.

## Strategy

- **The preview ignores the structure.** Use it to judge the clean arc, then
  expect the ball to stop or slow when it hits something.
- **Aim low to undermine.** Breaking a support is usually cheaper than breaking
  the target's cover. One collapsed tower can take out a target you could never
  reach directly.
- **Chain bonuses are free points.** A collapse that destroys cells scores them
  twice, and it does not cost an extra shot.
- **Watch the cracks.** A cracked cell is damaged. If one shot only dented it,
  the next solid hit may break through.
- **Match the projectile to the problem.** Boost is for distance and punching
  through; split is for several targets that one line cannot cover; pierce is
  for a column of cells you need to grind through rather than stop against.
- **Budget for the star.** If a level gives you three shots and you have used
  one, a two-shot finish still earns three stars; a three-shot finish does not.
  Restarting is free.

## Super Game Boy

On a Super Game Boy the cartridge installs a border around the game: a launch
rig at dusk, framing the play field in wood. It appears automatically on boot.
On a regular Game Boy or Game Boy Pocket the game fills the screen as usual.

## Troubleshooting

- **The game will not start in an emulator.** Make sure you built the ROM with
  `make` and are loading `build/slingshot.gb`, not a source file.
- **My stars disappeared.** Stars live in battery-backed cartridge RAM and are
  written when you press **START** on the win splash. Some emulators need
  battery saves enabled.
- **The aim arc does not match the shot.** It should, exactly, until the shot
  hits something. The arc shows the path through empty space, not the result of
  an impact.
