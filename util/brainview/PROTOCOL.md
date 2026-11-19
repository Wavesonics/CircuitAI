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
Sent once on connect.
`{t, proto, ai, team, allyTeam, side, frame}`

### `meta`
Sent once on connect and on request. Static for the whole game.
- `map`: `{name, width, height}`
- `heights`: `{w, h, cell, min, max, data}` (float grid, downsampled heightmap)
- `threatGrid`, `inflGrid`: `{w, h, cell}` dimensions of the layers in `state`
- `roles`, `buildTypes`, `fightTypes`, `taskTypes`: name tables indexed by the
  numeric ids used in `state`
- `defs`: `[id, name, mainRole, metalCost, isMobile]`
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
