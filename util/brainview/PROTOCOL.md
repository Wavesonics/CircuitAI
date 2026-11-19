# BARb brain protocol

BARb streams its world model to local clients when the `BARB_BRAIN` environment
variable is set (any value except `0`). Each AI instance listens on
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
  `inflGrid`:
  - `threatAir`, `threatSurf`, `threatAmph`, `threatCloak`
  - `inflEnemy`, `inflAlly`, `inflAllyDefend`, `influence` (ally minus enemy)
- `openSpots`: string of `0`/`1` per metal spot, `1` meaning free to claim

### `error`
`{t, error}` in reply to a malformed or unknown client message.

## Client to AI

- `{"t":"cfg","stateEvery":N}`: set the state interval in frames (1..1800).
  Applies to all clients of that AI.
- `{"t":"meta"}`: resend `meta`.
