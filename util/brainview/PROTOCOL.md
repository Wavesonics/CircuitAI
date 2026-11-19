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
- `defs`: `[id, name, mainRole, metalCost]`
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
  - fighter: `fight, pos, power, target?, targetPos?, leader?`
- `enemies`: `[id, defId, x, z, losBits, cost]`. `id` is -1 for fake enemies
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

### `error`
`{t, error}` in reply to a malformed or unknown client message.

## Client to AI

- `{"t":"cfg","stateEvery":N,"decisions":bool}`: set the state interval in
  frames (1..1800) and turn decision tracing on or off. Either field may be
  omitted. Applies to all clients of that AI.
- `{"t":"meta"}`: resend `meta`.
