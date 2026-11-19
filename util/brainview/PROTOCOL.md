# BARb brain protocol

BARb streams its world model to local clients when the `BARB_BRAIN` environment
variable is `1`, `true`, `yes` or `on`. Each AI instance listens on
`127.0.0.1:(BARB_BRAIN_PORT or 7710) + skirmishAIId`.

Transport is TCP with newline-delimited JSON: one object per line, each with a
`t` field naming the message type. The protocol version is in `hello.proto`.

Conventions:
- Positions are `[x, z]` in elmos (world units).
- Float grids are base64-encoded little-endian float32 arrays, row-major
  (`index = z * w + x`), each cell covering `cell` elmos.
- Tuples are used for bulk lists; their fields are listed below.

## AI to client

### `hello`
Sent once on connect, followed by `meta` and then recent `event` messages
(see `event`).
`{t, proto, ai, team, allyTeam, side, frame}`. `proto` is 2; version 1 had
no `event` messages, `state.silos` or def flags and categories.

### `meta`
Sent once on connect and on request. Static for the whole game.
- `map`: `{name, width, height}`
- `heights`: `{w, h, cell, min, max, data}` (float grid, downsampled heightmap)
- `threatGrid`, `inflGrid`: `{w, h, cell}` dimensions of the layers in `state`
- `roles`, `buildTypes`, `fightTypes`, `taskTypes`: name tables indexed by the
  numeric ids used in `state`
- `defs`: `[id, name, mainRole, metalCost, isMobile, flags, category]`.
  `flags`: `1` launches nukes, `2` intercepts nukes. A def is flagged by its
  mounted weapons: nuke weapons are the interceptable (`targetable`) ones,
  antinukes the `interceptor` ones. `category`: key unit category, `0` none,
  then `1` commander, `2` experimental, `3` factory, `4` superweapon, `5`
  defence, `6` economy, `7` structure (see `keyDeath`).
- `metalSpots`: `[x, z, income]`
- `clusters`: `[x, z, radius, income, [spotIndex...]]`
- `clusterEdges`: `[clusterA, clusterB]`

### `state`
Sent every `stateEvery` frames (default 30, one per second at normal speed).
- `frame`
- `droppedDecisions`: decisions not traced since the last state, because of
  the rate limit or because they happened inside another traced decision
- `basePos`, `startPos`, `enemyPos` (mean known enemy position)
- `eco`: `{mInc, eInc, mCur, mStore, mPull, eCur, eStore, ecoFactor, excessed,
  metalEmpty, metalFull, energyStalling, energyEmpty, energyFull}`
- `units`: `[id, defId, x, z, health, taskId]`, health is 0..1
- `silos`: `[id, defId, x, z, flags, stockpile, queued, finished]` for our
  units with a flagged def (see `meta.defs`). `stockpile` is missiles ready,
  `queued` missiles still to build; both are 0 while `finished` is false.
- `tasks`: objects with
  - always: `id, type, prio, state, n` (assignee count)
  - builder: `build, pos, def?, costM, progressM, recruit?`
  - fighter: `fight, pos, power, target?, targetPos?, leader?`; defend squads
    also `promote` (fight type they turn into) and `promoteAt` (power at which
    they do, lowered or raised by an attack focus)
- `enemies`: `[id, defId, x, z, losBits, cost, lastSeen]`. `lastSeen` is the
  frame the enemy left radar and sight, or -1 while it is still on radar or in
  sight (and for fakes). `id` is -1 for fake enemies
  (inferred from damage), `defId` is -1 when only seen on radar. `losBits`:
  `1` LOS, `2` radar, `4` hidden, `8` neutral/ignored, `32` dying, `64` dead.
- `groups`: `[x, z, cost, influence, vagueMetric, unitCount]` (enemy k-means
  clusters used for attack targeting)
- `military`: `{armyCost, enemyMobileThreat, enemyStaticThreat,
  enemyMobileCost, airValid, roleCost[], enemyRoleCost[]}`, role arrays indexed
  by role id
- `layers`: float grids. Threat layers use `threatGrid`, influence layers use
  `inflGrid`. Threat layers are empty strings for the first few frames, until
  the threat map initialises:
  - `threatAir`, `threatSurf`, `threatAmph`, `threatCloak`
  - `inflEnemy`, `inflAlly`, `inflAllyDefend`, `influence` (ally minus enemy)
- `openSpots`: string of `0`/`1` per metal spot, `1` meaning free to claim
- `control`: the current control state, see `ack` below

### `decision`
Sent as the AI makes a traced decision, at most 40 per kind per second of
game time (excess is counted in `state.droppedDecisions`).
- `kind`: `factory`, `builder`, `attackTarget` or `retreat`
- `frame`; `unit`, `unitDef`, `pos` when a unit is deciding
- `path`: `[[node, outcome]...]`, the branches taken in order
- `inputs`: object of named values the decision used
- `options`: up to 64 of `{label, def?, score?, pos?, task?, reject?,
  parts?}`. `parts` holds the score's components. Options with `reject` were
  ruled out; scored options without it competed. `omitted` counts options past
  the cap.
- `rejected`: `{reason: count}` for candidates ruled out without an option
- `chosen`: index into `options`, or -1
- `result`: what the decision produced, e.g. `recruit armpw`, `wait`,
  `attack enemy 123`; `resultTask` is that task's id when there is one

Score direction depends on the kind: `factory` options are weights for a
random roll (higher is likelier); `builder` and `attackTarget` pick the lowest.

### `event`
Sent as combat events happen, whether or not a client is connected (new
clients get the last 200 on connect, oldest first). Raw engine events are gathered for a
moment first, so a nuke that hits forty units is one message.
Always `{t, seq, kind, frame}`: `seq` counts up from 1 per game, `frame` is
when the event began. A skirmish AI cannot see projectiles or enemy weapon
fire, so enemy nukes show up only as a `blast` where they hit our units, or as
an `intercept` when our antinuke fires.
- `blast`: one explosion that hit our units, or our nuke hitting enemies.
  Hits by the same weapon within 2 s and twice its damage radius are one
  blast; it is sent 1 s after the last hit.
  - `class`: `nuke`, or `large` for any other weapon with a damage radius of
    150 elmos or more (tactical missiles, EMP, big death explosions). `large`
    blasts are sent only when they hit 3 or more of our units or kill 2.
  - `weapon` (weapon def name), `sourceDef` (a unit def that mounts it, -1
    for death explosions), `aoe` (damage radius), `paralyzer`
  - `side`: whose weapon: `ours`, `ally`, `enemy` or `unknown`.
    `attackerSeen` is false when the engine hid the attacker; such hits are
    counted as `enemy`, except that a nuke landing on a target of one of our
    launches from the last 3 minutes counts as `ours`. `attackerDef` is the
    attacker's def, or -1 when not seen.
  - `pos`: mean position of the units hit
  - `oursHit`, `oursKilled`, `oursKilledCost` (metal), `enemyHit` (enemies
    in radar or sight that our nukes hit)
- `losses`: our units destroyed close together: each loss joins a burst
  within 1200 elmos of its centre, which ends after 5 s without losses or 30 s
  after it began. Sent when it ends, if it lost 4 or more units or 1000 or
  more metal. Nanoframes, reclaims and self-destruct timeouts are not losses.
  - `endFrame`, `pos` (mean position), `count`, `cost` (metal)
  - `peak5s`, `peak5sCost`: most units and metal lost within any 5 s
  - `massCasualty`: `peak5s` is 6 or more, or `peak5sCost` 3000 or more
  - `units`: `[defId, count]` of what we lost; `attackers`: `[defId,
    count]`, defId -1 for unseen attackers; `weapons`: `[name, count,
    class]`, `class` being `nuke`, `large` or empty. Engine damage without a
    weapon has names like `collision` or `unknown`.
- `launch`: one of our nuke silos fired: `unit`, `def`, `shots`, `stockpile`
  (left), `pos` (the silo), `target` (`[x, z]` the AI aimed at, or null).
- `intercept`: one of our antinukes fired, so an enemy nuke was falling
  within its cover: `unit`, `def`, `shots`, `stockpile`, `pos`.

- `keyDeath`: a key unit died. `category` comes from unit def data, first
  match wins: `commander` (BARb's commander role, decoys included), `factory`
  (a structure with build options, gantries included), `experimental`
  (custom param `techlevel` 3), `superweapon` (nuke and antinuke launchers,
  BARb's `super` role such as LRPCs), `defence` (armed structures from 1000
  metal, e.g. armanni), `economy` (unarmed structures from 300 metal whose
  `unitgroup` is `energy` or `metal`: T2 mexes, fusions, metal makers),
  `structure` (other unarmed structures from 300 metal: advanced radar,
  shields, targeting facilities). Fields: `side` (`ours`, `ally` or `enemy`), `unit`, `def`, `team`,
  `allyTeam`, `pos`, `killerSide`, `killerDef` and `killerTeam` (-1 when the
  killer was not seen), `weapon` (name, or empty when unknown),
  `commandersLeft` (commanders left on that team; -1 when unknown or not a
  commander). What the AI can see:
  - ours: every death but nanoframes, reclaims and self-destruct timeouts,
    with the killer when it was in our radar or sight;
  - enemy: only finished units that die in our radar or sight and whose type
    we know, with the killer only when it is ours or an ally's; `team` and
    `allyTeam` are -1 if the engine hides them;
  - ally: the engine reports nothing, so a key unit that leaves the allied
    unit list (checked every second) is reported from its last position,
    without a killer.
- `teamEliminated`: `{side, team, allyTeam, reason}`, sent once when our team
  or an allied team loses its last commander (`reason` `last commander
  died`). Under deathmode `com` that ends the team. Enemy eliminations are not
  detectable: the AI never sees all of an enemy team's commanders.
- `underAttack`: our units damaged by enemies in one sector (the map cut into
  8 columns of square sectors, as `brain_mcp.py` names them). The first hit
  opens a report, sent 3 s later; after that a sector is reported at most
  every 25 s, each report covering the hits since the last. Friendly fire and
  damage without a weapon (collisions, water) are left out. Fields:
  `endFrame`, `pos` (mean position of the damaged units), `damaged`,
  `structures`, `offensive` (damaged units in our attack, raid, bomb or scout
  squads: `offensive == damaged` means our own attack is trading, not that we
  are being raided), `units` (`[defId, count]` damaged), `attackers`
  (`[defId, count]` of distinct attackers seen, -1 for unidentified radar
  blips), `unseenHits` (hits by attackers outside our radar and sight:
  artillery, bombers, nukes).
- `keyUnderAttack`: a key unit of ours (see `keyDeath`) was hit by an enemy,
  sent at once and then at most every 10 s per unit: `category`, `unit`,
  `def`, `pos`, `health` (0..1), `attackerSeen`, `attackerDef`, `weapon`.

New clients also get every commander `keyDeath` and every `teamEliminated` of
the game, even those older than the last 200 events.

`launch` and `intercept` are read off stockpile drops, polled every 15
frames: the engine sends `WeaponFired` only for shots made under an attack
command, which antinukes never have.

### `ack`
Reply to every `cmd`: `{t, id?, op, ok, error?, result, control}`.
- `id` echoes the command's `id` (integer or string) when one was sent.
- `result` holds op-specific details, listed with each op.
- `control` is the control state after the command:
  - `tuning`: `{key: value}` for every tunable
  - `changed`: `{key: originalValue}` for tunables changed since the last reset
  - `limits`: `[defId, limit, originalLimit]` for unit limits changed
  - `focus`: `{x, z, radius, force, until}` or null; `until` is the frame it
    lapses
  - `controlled`: `[unitId, until]` for units taken out of the AI's hands by
    `units` orders; `until` is the frame they return to the AI

### `error`
`{t, error}` in reply to a malformed or unknown client message.

## Client to AI

- `{"t":"cfg","stateEvery":N,"decisions":bool}`: set the state interval in
  frames (1..1800) and turn decision tracing on or off. Either field may be
  omitted. Applies to all clients of that AI.
- `{"t":"meta"}`: resend `meta`.
- `{"t":"cmd","op":...,"id"?:...}`: change what the AI does; answered by an
  `ack`. Ops:
  - `control`: no change; returns the control state.
  - `set` `{key, value}`: set a tunable. Keys:
    - `factory.buildPowerRatio` (0..10): builders wanted per metal income
    - `factory.responseWeight` (0..10): how strongly production counters the
      enemy's army
    - `quota.scouts` (0..50), `quota.attack` (0..10000, power a squad needs
      before it attacks), `quota.raidMin`, `quota.raidAvg` (0..10000)
    - `role.<role>.maxPercent` (0..1): cap on that role's share of the army
    - `role.<role>.factor` (0..100): how readily that role is built in response
  - `reset` `{key?}`: restore one tunable, or with no key undo everything:
    tunables, unit limits, the attack focus and units under control.
  - `limit` `{def, limit}`: cap how many of a unit the AI may have; 0 bans it,
    `null` restores the original.
  - `build` `{def, x, z, type?}`: queue a structure at top priority. `type` is
    inferred when omitted (factory, nano, store, energy, defence, bunker,
    big_gun, radar, sonar, convert, mex). Mex orders snap to the nearest free
    metal spot. Result: `{type, pos}`.
  - `recruit` `{def, count?, factory?}`: queue 1-20 units at high priority, at
    the given factory unit or the first one that can build them. Queued units
    are built before the AI's own picks, so the queue is capped at 20 in
    total. Result: `{factory}`.
  - `units` `{action, units, x?, z?, seconds?}`: `move`, `fight`
    (attack-move) and `stop` take the units out of the AI's control for
    `seconds` (default 120, at most 1800) or until `release`; `retreat` hands
    them back to the AI and sends them to retreat. Structures are skipped for
    move, fight and retreat. Units under control do not retreat on their own
    when damaged. Result: `{applied: [ids], skipped: [[id, reason]],
    returnsToAI?: frame}`.
  - `focus` `{x, z, radius?, force?, seconds?}` or `{clear: true}`: for
    `seconds` (default 300, at most 3600):
    - gathering squads turn into attack squads once they out-power the
      strongest enemy group inside the focus, rather than the strongest one
      anywhere (`quota.attack` is still the floor);
    - attack squads only pick enemies within `radius` (default 1000) of the
      point, nearest to it first, judging each group at full strength;
    - with no target there, they travel to the point if they out-power what is
      in it.
    `force` skips the strength checks, so `quota.attack` alone decides when
    squads attack. Raids, scouts and squads that stay defending are not
    affected.

Tuning changes apply from the AI's next decision. Nothing persists between
games.
