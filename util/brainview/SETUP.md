# Brain view: setup from zero

Step-by-step instructions for a new contributor (or their coding agent) with no
Beyond All Reason (BAR) setup at all. Follow them in order. Everything here was
verified on Linux (x86_64); the engine build runs in a container, so the host
needs very little.

## What this project is

BAR is an RTS running on the Recoil engine (a Spring fork). It ships a skirmish
AI called **BARb** (a fork of CircuitAI, C++ plus AngelScript). This project
adds a "brain view" to BARb:

1. **Brain server** (C++, inside BARb): streams the AI's world model and its
   decision traces as JSON over a local TCP socket, and accepts commands that
   steer it during a live game.
2. **Bridge + web viewer** (`bridge.py`, `web/`): a browser view of the AI's
   map layers, units, enemies, squads, economy and decisions, plus a Control
   tab.
3. **MCP server** (`brain_mcp.py`): lets an LLM (Claude) read the AI's state
   and steer it with tools, so it can act as the strategist while BARb does the
   micro.

Detailed references live next to this file:
[README.md](README.md) (viewer and tools), [PROTOCOL.md](PROTOCOL.md) (wire
protocol).

## Repositories and branches

| Repo | Clone from | Branch | Why |
|---|---|---|---|
| RecoilEngine | https://github.com/Wavesonics/RecoilEngine | `brain-view` | Engine. Its `AI/Skirmish/BARb` submodule points at the BARb fork below |
| BARb | https://github.com/Wavesonics/CircuitAI | `brain-view` | All of this project's code. Checked out automatically as a submodule of RecoilEngine |
| Beyond-All-Reason | https://github.com/beyond-all-reason/Beyond-All-Reason | `master` | The game itself (Lua, unit defs, BARb's config and scripts). Unmodified |

Upstreams: BARb's upstream is `rlcevg/CircuitAI` (branch `barbarian`), the
engine's is `beyond-all-reason/RecoilEngine` (`master`). `brain-view` in BARb
branched from upstream commit `f4a6ca3`, which is the BARb that ships with
engine `recoil_2026.09.03`.

BARb reads its config and AngelScript from the game repo, in
`Beyond-All-Reason/luarules/configs/BARb/stable/{config,script}/`. Behaviour
tweaks that do not need C++ go there.

## 1. Prerequisites

- `git`, `python3` (3.8+, standard library only, nothing to pip install).
- A container runtime for the engine build: Docker Engine (with the
  post-install steps so your user can run `docker`) or Podman. The build
  script uses Docker if both are installed; set `CONTAINER_RUNTIME=podman` to
  override.
- Disk: roughly 20 GB. RecoilEngine with build output and cache is about
  15 GB, the build image about 2.3 GB, and Beyond-All-Reason about 3 GB with a
  shallow clone (its full history adds 4.5 GB).
- A GPU and display for windowed games. Headless games need neither.
- Optional: the official BAR client (Flatpak `info.beyondallreason.bar`) to
  play online with friends (see [Playing online with friends](#8-playing-online-with-friends)).
- Optional: Claude Code, for the MCP server.

## 2. Clone

Use one workspace directory; the rest of this guide calls it `$BAR` and
assumes this layout:

```
$BAR/
  RecoilEngine/              engine, with AI/Skirmish/BARb inside
  Beyond-All-Reason/         game
  devdata/                   engine data dir for local matches (created below)
  run-local.sh               match launcher (created below)
  .mcp.json                  MCP registration for Claude Code (created below)
```

```bash
export BAR=~/Code/BAR
mkdir -p "$BAR" && cd "$BAR"
git clone -b brain-view --recurse-submodules --jobs 8 https://github.com/Wavesonics/RecoilEngine.git
git clone --depth 1 --recurse-submodules https://github.com/beyond-all-reason/Beyond-All-Reason.git
```

The game is cloned shallow because we do not modify it; run
`git fetch --unshallow` there if you ever need its history.

The recursive clone of RecoilEngine pulls about twenty submodules, which takes
a while. Then put BARb on its branch (submodules check out a detached HEAD) and
add the upstream for reference:

```bash
cd "$BAR/RecoilEngine/AI/Skirmish/BARb"
git switch brain-view
git remote add upstream https://github.com/rlcevg/CircuitAI.git
```

Check: `git -C "$BAR/RecoilEngine" submodule status | grep BARb` shows no `+`
or `-` prefix, and `git -C "$BAR/RecoilEngine/AI/Skirmish/BARb" log -1` is the
tip of `brain-view`.

If the clone was copied from another machine instead (tarball, rsync), two
things commonly break: executable bits are lost (`git diff` shows only
`old mode 100755`), and empty submodule directories vanish. Fix with
`chmod +x` on the reported files, `mkdir -p` for missing submodule paths, then
`git submodule update --init --recursive`.

## 3. Build the engine and BARb

From `$BAR/RecoilEngine`:

```bash
cd "$BAR/RecoilEngine"
docker-build-v2/build.sh -j "$(nproc)" linux > ../RecoilEngine-build.log 2>&1; echo "exit $?"
```

- The first run pulls the build image from GitHub Packages, then configures,
  compiles and installs. Expect it to take a while; later builds are
  incremental (cache in `.cache/`).
- Output that matters: `build-amd64-linux/install/` holds `spring` (windowed),
  `spring-headless`, `pr-downloader`, and
  `AI/Skirmish/BARb/stable/libSkirmishAI.so`.
- Search the log for failures with `grep -nE "error:|FAILED" ../RecoilEngine-build.log`.

Rebuild rules learned the hard way:

- **Adding a new `.cpp` file needs the full command above.** Sources are
  globbed at configure time, so a compile-only build will not see new files.
- **Compile-only does not install.** For a quick compile check of BARb alone:
  `docker-build-v2/build.sh -j "$(nproc)" --compile linux -t BARb`. The
  game keeps using the previously installed library until you run the full
  command.
- **Submodule guard.** If BARb has commits that RecoilEngine's recorded
  submodule pointer does not match (normal while you work), the build script
  stops and asks you to confirm. Create the ignored marker file once to skip
  that check: `touch "$BAR/RecoilEngine/.i-understand-git-submodules.txt"`.
- **Do not rebuild while a match is running from `install/`**; stop the match
  first (see [Stopping matches](#stopping-matches)).

## 4. Game data directory

The engine runs with `--isolation --write-dir $BAR/devdata`, so nothing touches
a system BAR install. The data dir needs the game (as a symlink to the git
checkout) and at least one map.

```bash
cd "$BAR"
mkdir -p devdata/games devdata/maps
ln -sfn ../../Beyond-All-Reason devdata/games/BAR.sdd
PRD_HTTP_SEARCH_URL=https://files-cdn.beyondallreason.dev/find \
PRD_RAPID_USE_STREAMER=false \
PRD_RAPID_REPO_MASTER=https://repos-cdn.beyondallreason.dev/repos.gz \
RecoilEngine/build-amd64-linux/install/pr-downloader --filesystem-writepath "$BAR/devdata" --download-map "All That Glitters v2.2.3"
```

The `PRD_*` variables point `pr-downloader` at BAR's mirrors (the same ones the
official launcher uses). The map lands in `devdata/maps/` (about 76 MB). Use
the same command with another map name to add maps.

Optional: if you have the official BAR client, copying its
`springsettings.cfg` into `devdata/` gives local windowed games your graphics
and key settings. For the Flatpak that file is
`~/.var/app/info.beyondallreason.bar/data/springsettings.cfg`.

## 5. Launcher script

Save this as `$BAR/run-local.sh` and `chmod +x` it. It writes a start script
and launches a match on the local build.

```bash
#!/usr/bin/env bash
# Starts a match on the locally built engine and the local Beyond-All-Reason
# checkout. Everything lives in ./devdata, isolated from any other BAR install.
#
# Usage: ./run-local.sh [--headless] [--spectate] [map name]
#   default     you (Armada) vs BARb (Cortex); place your start and ready up
#   --spectate  BARb vs BARb, you watch
#   --headless  no window, implies --spectate (smoke tests, AI-only runs)
#
# Env: BAR_DATA (data dir, default ./devdata), HOST_PORT (default 8452),
#      BARB_BRAIN (default 1), BARB_BRAIN_PORT (default 7710).
# Set the first two to run a second match alongside one already running.
set -euo pipefail

ROOT="$(cd "$(dirname "$0")" && pwd)"
INSTALL="$ROOT/RecoilEngine/build-amd64-linux/install"
DATA="${BAR_DATA:-$ROOT/devdata}"

BIN=spring
SPECTATE=0
while [[ "${1:-}" == --* ]]; do
	case "$1" in
		--headless) BIN=spring-headless; SPECTATE=1 ;;
		--spectate) SPECTATE=1 ;;
		*) echo "unknown option $1" >&2; exit 1 ;;
	esac
	shift
done
MAP="${1:-All That Glitters v2.2.3}"

ai() { # index team side
	cat <<EOF
	[AI$1] { Name=BARb$2; ShortName=BARb; Version=stable; Team=$2; Host=0; }
	[TEAM$2] { TeamLeader=0; AllyTeam=$2; Side=$3; }
	[ALLYTEAM$2] { NumAllies=0; }
EOF
}

# draft_mode is disabled because draft turn order needs a lobby; without one
# start placement can stall in pregame
{
	cat <<EOF
[GAME]
{
	GameType=Beyond All Reason \$VERSION;
	MapName=$MAP;
	IsHost=1;
	HostPort=${HOST_PORT:-8452};
	MyPlayerName=Player;
	[MODOPTIONS]
	{
		draft_mode=disabled;
	}
EOF
	if ((SPECTATE)); then
		# random fixed starts: nobody has to place a start position
		echo "	StartPosType=1;"
		echo "	[PLAYER0] { Name=Player; Spectator=1; Team=0; }"
		ai 0 0 Armada
		ai 1 1 Cortex
	else
		echo "	StartPosType=2;"
		echo "	[PLAYER0] { Name=Player; Team=0; Spectator=0; }"
		echo "	[TEAM0] { TeamLeader=0; AllyTeam=0; Side=Armada; }"
		echo "	[ALLYTEAM0] { NumAllies=0; }"
		ai 0 1 Cortex
	fi
	echo "}"
} > "$DATA/script.txt"

# BARb streams its world model on 127.0.0.1:7710+ (see util/brainview in BARb)
export BARB_BRAIN="${BARB_BRAIN:-1}"

exec "$INSTALL/$BIN" --isolation --write-dir "$DATA" "$DATA/script.txt"
```

Why the script looks the way it does:

- `GameType=Beyond All Reason $VERSION` resolves to the `BAR.sdd` symlink, so
  game-side Lua edits apply on the next match with no packaging.
- `draft_mode=disabled`: BAR's draft start order expects a lobby; without one
  the match can hang in pregame.
- Spectate and headless use `StartPosType=1` (fixed random starts). With
  `StartPosType=2` (choose in game) and no human to place, the match never
  leaves frame -1.

## 6. Running matches

```bash
cd "$BAR"
./run-local.sh                 # you vs BARb, windowed
./run-local.sh --spectate      # BARb vs BARb, windowed, you watch
./run-local.sh --headless      # BARb vs BARb, no window
```

- Logs: the engine's `infolog.txt` is in the data dir (`devdata/infolog.txt`);
  stdout has the same lines. BARb's lines are prefixed
  `Skirmish AI <BARbarIAn-stable>`.
- In spectate and headless mode the AIs are AI ids 0 and 1 (Armada team 0,
  Cortex team 1), so their brain ports are 7710 and 7711.

### Headless test runs

Headless matches are how changes get tested. Wrap them in `timeout` so a stuck
match cannot run forever, and give each concurrent match its own data dir,
host port and brain port base so it cannot collide with another match (or with
a real game you are playing):

```bash
cd "$BAR"
mkdir -p devdata-test/games devdata-test/maps
ln -sfn ../../Beyond-All-Reason devdata-test/games/BAR.sdd
ln -sfn ../../devdata/maps/all_that_glitters_v2.2.3.sd7 devdata-test/maps/
BAR_DATA=$PWD/devdata-test HOST_PORT=8470 BARB_BRAIN_PORT=7720 \
  timeout 1500 ./run-local.sh --headless > devdata-test/brain-test.log 2>&1 &
```

That match's AIs listen on 7720 and 7721. Useful checks while it runs:

```bash
grep -E "listening on|Task guard|invalid team|Error" devdata-test/brain-test.log
ss -ltnp | grep -E ':77[0-9][0-9]'
```

For scripted checks, `bridge.py`'s `AILink` class is a ready client: put
`util/brainview` on `sys.path`, then `link = AILink(7720)`. It reconnects on
its own and keeps `link.hello`, `link.meta` and `link.state` (raw JSON lines)
and `link.decisions` up to date; `link.request({"t": "cmd", "op": ...})` sends
a command and waits for its `ack`. Run such scripts while the match runs, then
assert on what came back (for example that decisions show "attack focus set"
after a `focus` command).

### Stopping matches

`pkill -f install/spring` also matches the shell running the command and kills
it. Use a bracket pattern so the pattern does not match itself:

```bash
pgrep -af "[i]nstall/spring"           # list running local matches
pkill -f "[i]nstall/spring-headless"   # stop headless ones
```

## 7. Brain server, viewer, MCP

### Brain server

- Opt-in: only active when the engine's environment has `BARB_BRAIN=1`
  (`run-local.sh` sets it by default; without it BARb behaves exactly like
  stock).
- Each AI listens on `127.0.0.1:(BARB_BRAIN_PORT or 7710) + skirmishAIId` and
  logs `BrainServer: listening on 127.0.0.1:<port>`.
- It is polled on the AI's own update, which only starts once the game starts.
  During start placement a client can connect (the OS accepts it) but gets no
  `hello` yet. That is expected.
- Protocol: newline-delimited JSON, see [PROTOCOL.md](PROTOCOL.md). On connect
  the AI sends `hello` and `meta` (map, heightmap, unit defs, metal spots),
  then a `state` every second, plus `decision` traces and `event` messages. Clients send `cfg`,
  `meta` and `cmd` (steering), and each `cmd` is answered by an `ack`.
- Decision tracing is rate-limited per kind (factory, builder, attackTarget,
  retreat) at 40 per second; `state.droppedDecisions` counts what was skipped.

Code map (BARb, `src/circuit/`):

| Where | What |
|---|---|
| `brain/BrainServer.*` | sockets, clients, message dispatch, trace budget |
| `brain/BrainSnapshot.cpp` | `meta` and `state` serialization |
| `brain/BrainControl.cpp` | `cmd` ops: tunables, limits, build, recruit, unit orders, focus |
| `brain/BrainEvents.cpp`, `BrainAlerts.cpp`, `BrainEventUtil.h` | `event` messages: key unit deaths, attacks, nukes, blasts, loss bursts |
| `brain/DecisionTrace.*` | decision records and the `CTraceScope` used by hooks |
| `brain/JsonOut.h` | streaming JSON writer |
| `module/FactoryManager.cpp`, `BuilderManager.cpp`, `task/fighter/AttackTask.cpp`, `FighterTask.cpp` | decision trace hooks |
| `module/MilitaryManager.*`, `task/fighter/DefendTask.*` | attack focus |
| `task/fighter/SquadTask.*`, `module/TaskModule.cpp` | squad member guard (see Known issues) |
| `CircuitAI.cpp` | starts the server and polls it in `Update` |

### Bridge and web viewer

```bash
python3 "$BAR/RecoilEngine/AI/Skirmish/BARb/util/brainview/bridge.py" --ports 7710-7717,7720-7721 --listen 8770
```

Open http://127.0.0.1:8770/ and pick an AI tab. The bridge reconnects on its
own, so leave it running across matches. HTTP interface (127.0.0.1 only; it
rejects other `Host` headers):

| Endpoint | |
|---|---|
| `GET /` | the viewer (`web/`) |
| `GET /api/ais` | connected AIs and their `hello` |
| `GET /api/meta?port=N` | one AI's `meta` |
| `GET /events?port=N` | Server-Sent Events: `state`, `decision`, `ack`, `aierror` and event messages |
| `POST /api/send?port=N` | forward one JSON object (a `cmd`) to the AI; the reply arrives on `/events` |

The viewer: influence and threat layers, own units, enemies (in sight, radar,
remembered, inferred attackers), enemy groups, squad targets and build jobs,
economy, army by role, and a decision feed where each decision shows its
branch path, inputs and every scored option. The Control tab issues the same
commands as the MCP tools (select units, right-click to move or fight, attack
focus, production, tuning, reset). See [README.md](README.md) for how to read
the map.

### MCP server

`brain_mcp.py` speaks MCP over stdio (JSON-RPC 2.0, standard library only) and
connects straight to the AI ports, so the bridge does not need to be running.
Register it for Claude Code by saving this as `$BAR/.mcp.json` (adjust the
path), then start Claude Code in `$BAR` and approve the server:

```json
{
  "mcpServers": {
    "barb-brain": {
      "command": "python3",
      "args": ["<BAR>/RecoilEngine/AI/Skirmish/BARb/util/brainview/brain_mcp.py",
               "--ports", "7710-7717,7720-7721"]
    }
  }
}
```

Tools (every tool takes an optional `port`, needed only when several AIs are
connected):

| Tool | Does |
|---|---|
| `list_ais` | AIs streaming now, with port, team, game time |
| `get_overview` | one-call summary: economy, units and army by role, squads (position, size, composition, target, "gathering (becomes X at power Y)"), build jobs, factory queue, known enemy, your active changes; plus `commanderDeaths` (every commander death this game), `alerts` (key unit deaths and attacks on them, sectors under attack, nuke hits and launches, antinuke shots, mass casualties) and `nukes` (our silos and antinukes with stockpiles, enemy ones we have seen) |
| `get_events` | recent combat events, newest first, filterable by kind: `commander`, `key`, `attack`, `nuke`, `blast`, `losses`, `launch`, `intercept` |
| `get_map` | a layer (influence, threat, ...) as a sector grid; `#` marks the front |
| `get_units` | own units, filterable; shows squad ids and structures |
| `get_enemies` | known enemies with "last seen N s ago" and attackers inferred from damage |
| `get_decisions` | recent decision traces, repeats merged; nano assist waits hidden unless `include_waits` |
| `get_tuning` | current tunables, limits, focus and controlled units |
| `set_tuning` | change a tunable (production ratio, counter weight, attack and raid quotas, role caps) |
| `reset` | undo one tunable or everything |
| `limit_unit` | cap or ban a unit type |
| `build` | queue a structure at top priority (mex snaps to the nearest free spot) |
| `recruit` | queue 1-20 units at a factory, ahead of the AI's own picks |
| `order_units` | move, fight, stop, retreat or release units, selected by squad, role, unit ids or sector; area selections skip the commander and builders; ordered units return to the AI after a timeout |
| `set_focus` / `clear_focus` | attack focus: squads attack once they out-power the strongest enemy group inside the circle, only target enemies inside it, and travel there when they can win; `force` skips the strength checks |

Positions are world `x, z` (elmos) or sector names like `C7`: 8 columns `A`-`H`
west to east, each 768 elmos wide, rows numbered from the north.

The game keeps running while the model thinks, so the LLM works best as a
strategist that checks in every 30-60 seconds and leaves the fast decisions to
BARb.

Limits of the events: a skirmish AI cannot see projectiles or enemy weapon
fire, so an enemy nuke shows up only where it hits us or when our antinuke
fires at it. The engine does not report allied deaths (a key allied unit is
reported when it leaves the allied unit list, with no killer), enemy deaths
only count inside our radar or sight, and enemy team elimination is not
detectable.

## 8. Playing online with friends

- AIs run on the PC of the player who adds them in the lobby (the host of the
  bot). Other players need nothing special; only that one PC needs the brain
  build.
- The official client is a Flatpak. Its engine lives in
  `~/.var/app/info.beyondallreason.bar/data/engine/<version>/`. To use the
  brain build, replace that engine's BARb library with the one from your
  build, keeping a backup:

  ```bash
  E=~/.var/app/info.beyondallreason.bar/data/engine/recoil_2026.09.03/AI/Skirmish/BARb/stable
  cp -n "$E/libSkirmishAI.so" "$E/libSkirmishAI.so.stock"
  cp "$BAR/RecoilEngine/build-amd64-linux/install/AI/Skirmish/BARb/stable/libSkirmishAI.so" "$E/"
  ```

  This works because the official engine (`recoil_2026.09.03`) and our
  RecoilEngine build have the same skirmish AI interface, and `brain-view` is
  based on the BARb that ships with it. When BAR updates its engine, a new
  version directory appears with a stock library: check the AI interface still
  matches (`AI/Interfaces/C` in the engine), rebuild if needed, and swap again.
  Restore the backup to go back to stock.
- Launch the client with the brain enabled:
  `flatpak run --env=BARB_BRAIN=1 info.beyondallreason.bar`. Once the game
  starts, the infolog (`~/.var/app/info.beyondallreason.bar/data/infolog.txt`)
  shows `BrainServer: listening on 127.0.0.1:7710` and the AI taking over its
  team; the viewer and MCP connect to it as above.
- Lobby notes: repeated failed logins trip the server's flood protection, so
  wait it out rather than retrying, and never script or probe the lobby
  server. In SPADS-hosted rooms, "inconsistent team/ID configuration" is fixed
  with `!fixid` or `!balance`; `!set autoBalance off` stops auto-balance from
  reshuffling teams (may need a vote).

## 9. Known issues and gotchas

- **Squad crash guard.** One play-test crashed with a segfault in
  `ActivePath` (squad pathing on a broken member) around 10:45 game time. It
  never reproduced. BARb now checks squad members every update and repairs
  broken ones, logging `Task guard [...]` once per unit with the AI, squad and
  unit. If those lines show up in an infolog, keep the log: it is the lead on
  the root cause.
- **Threat layers are empty for the first few frames**, until the threat map
  initializes; clients must accept empty layer strings.
- **Port collisions.** A second match with the default `HOST_PORT` fails if
  another match (including the official client) is running; set `HOST_PORT`,
  `BAR_DATA` and `BARB_BRAIN_PORT`.
- **"invalid team ID ... for SkirmishAI" warnings** appeared once in a
  multiplayer log and did not recur; note them if you see them.

## 10. How we work

- All work happens on the `brain-view` branches, with small checkpoint commits.
  After each commit, run a code review of it and fix every finding before
  moving on.
- Every engine-side change is verified in a live headless match (build, run,
  connect, check the behaviour through the protocol), not just compiled.
- Commit messages and code comments: concise; comments describe the code as it
  is, never its history, and only explain the non-obvious.
- When BARb gets new commits, also commit the new submodule pointer on
  RecoilEngine's `brain-view` so a fresh recursive clone gets the same code.
