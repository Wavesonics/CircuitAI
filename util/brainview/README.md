# Brain view

A live view into BARb's world model and decisions while it plays.

- The AI streams its state and decision traces when `BARB_BRAIN=1` is set in the
  engine's environment (see [PROTOCOL.md](PROTOCOL.md)).
- `bridge.py` connects to every AI and serves the viewer. Python 3 standard
  library only.
- `web/` is the viewer: a map with the AI's influence and threat layers, its
  units, enemies, enemy groups, squad targets and build jobs, plus economy,
  army-by-role and a feed of decisions. Selecting a decision shows the branch
  path it took, its inputs, and every option with its score and score parts.
  The Control tab changes what the AI does while it plays.

## Run

1. Start a match with the brain enabled, for example:
   ```bash
   BARB_BRAIN=1 spring --isolation --write-dir <data dir> <script.txt>
   ```
   Each AI logs `BrainServer: listening on 127.0.0.1:<port>` (7710 + AI id by
   default, or `BARB_BRAIN_PORT` + AI id).
2. Start the bridge:
   ```bash
   python3 util/brainview/bridge.py
   ```
   Use `--ports` if the AIs use other ports, e.g. `--ports 7720-7721`.
3. Open http://127.0.0.1:8770/ and pick an AI tab.

The bridge reconnects on its own, so it can be left running across matches.

## Reading the map

- Influence: blue is where the AI's side is stronger, red where the enemy is.
  The AI treats cells below its safety threshold as hostile ground.
- Threat layers: how much damage each cell would take, by movement type.
- Enemies: filled dots are in sight, hollow dots are radar blips, faint dots are
  remembered positions, crosses are attackers inferred from damage.
- Squares are build jobs (dashed when nobody is assigned), arrows are squad
  targets, red circles are the enemy groups the AI's attack logic scores.

## Control

The Control tab sends commands to the AI (see `cmd` in PROTOCOL.md):

- Select units with a click, or shift-drag a box; right-click the map to
  fight-move or move them there. Ordered units leave the AI's control for the
  "hold for" time (amber rings), then return to it. Stop, Retreat and
  "Give back to AI" act on the selection.
- Attack focus: attack squads only go after enemies inside the circle and
  travel there when it is empty. It lapses after the set time.
- Production: queue units at a factory, place a structure, or cap or ban a
  unit type.
- Tuning: production and army quotas, and per-role army composition. Changed
  values turn amber with a restore button; "Reset everything" undoes all
  changes, the focus and unit control.
- The command log shows each command and whether the AI accepted it.
