#!/usr/bin/env python3
"""MCP server that lets an LLM read and steer BARb while it plays.

Speaks MCP (JSON-RPC 2.0, one message per line) on stdin/stdout and connects
straight to each AI's brain port, so the bridge and viewer are optional.
Standard library only.

  python3 brain_mcp.py [--ports 7710-7717]
"""

import argparse
import base64
import collections
import json
import math
import os
import struct
import sys
import threading
import time

# bridge.py sits next to this file; MCP clients may start us from anywhere
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from bridge import AILink, parse_ports  # noqa: E402

SERVER_NAME = "barb-brain"
SERVER_VERSION = "0.1.0"
PROTOCOLS = ("2025-06-18", "2025-03-26", "2024-11-05")
FPS = 30
SECTOR_COLS = 8  # map is cut into SECTOR_COLS columns of square sectors

INSTRUCTIONS = """\
Tools for watching and steering BARb, the Beyond All Reason AI, in a live game.
BARb plays on its own; you act as its strategist. Start with list_ais, then
get_overview. Positions are world coordinates [x, z] in elmos, with x growing
east and z growing south, plus a sector name like "C7" (letter = column from
the west, number = row from the north). Game time advances while you think, so
prefer a few decisive changes over many small ones."""


def clock(frame):
    s = max(0, int(frame // FPS))
    return f"{s // 60}:{s % 60:02d}"


def floats(b64):
    if not b64:
        return []
    raw = base64.b64decode(b64)
    return struct.unpack(f"<{len(raw) // 4}f", raw)


class ToolError(Exception):
    """A tool failure the model should see and can fix (bad arguments, no AI)."""


class Game:
    """Parsed view of one AI's brain stream, cached per message."""

    def __init__(self, link):
        self.link = link
        self._meta = (None, None)
        self._state = (None, None)

    def meta(self):
        raw = self.link.meta
        if raw is None:
            raise ToolError(f"AI on port {self.link.port} has not sent its map data yet")
        if self._meta[0] is not raw:
            self._meta = (raw, json.loads(raw))
        return self._meta[1]

    def state(self):
        raw = self.link.state
        if raw is None:
            raise ToolError(f"AI on port {self.link.port} has not sent a state yet")
        if self._state[0] is not raw:
            self._state = (raw, json.loads(raw))
        return self._state[1]

    def defs(self):
        meta = self.meta()
        if "_defs" not in meta:
            meta["_defs"] = {d[0]: {"name": d[1], "role": meta["roles"][d[2]] if d[2] < len(meta["roles"]) else "?",
                                    "cost": d[3], "mobile": d[4] if len(d) > 4 else True} for d in meta["defs"]}
        return meta["_defs"]

    def def_name(self, def_id):
        d = self.defs().get(def_id)
        return d["name"] if d else ("unknown" if def_id < 0 else f"def{def_id}")

    # -- geometry

    def sector_size(self):
        return self.meta()["map"]["width"] / SECTOR_COLS

    def sector(self, x, z):
        size = self.sector_size()
        col = min(int(x // size), SECTOR_COLS - 1)
        row = int(z // size) + 1
        return f"{chr(ord('A') + max(col, 0))}{max(row, 1)}"

    def pos(self, p):
        if p is None or p[0] < 0:
            return None
        return {"x": round(p[0]), "z": round(p[1]), "sector": self.sector(p[0], p[1])}

    def sector_center(self, name):
        name = name.strip().upper()
        if len(name) < 2 or not name[0].isalpha() or not name[1:].isdigit():
            raise ToolError(f"'{name}' is not a sector name like C7")
        size = self.sector_size()
        col = ord(name[0]) - ord("A")
        row = int(name[1:]) - 1
        m = self.meta()["map"]
        if not (0 <= col < SECTOR_COLS) or not (0 <= row * size < m["height"]):
            raise ToolError(f"sector {name} is off the map")
        return (col + 0.5) * size, min((row + 0.5) * size, m["height"] - 1)


class Server:
    def __init__(self, ports):
        self.links = {p: AILink(p) for p in ports}
        self.games = {p: Game(link) for p, link in self.links.items()}
        self.out_lock = threading.Lock()

    # -- AI selection

    def connected(self):
        return [(p, l) for p, l in sorted(self.links.items()) if l.connected and l.hello]

    def game(self, args):
        port = args.get("port")
        live = self.connected()
        if port is None:
            if len(live) == 1:
                return self.games[live[0][0]]
            if not live:
                raise ToolError("no AI is connected; start a match with BARB_BRAIN=1")
            raise ToolError("several AIs are connected; pass port (see list_ais)")
        if port not in self.links:
            raise ToolError(f"port {port} is not watched; watched ports: {sorted(self.links)}")
        if not self.links[port].connected:
            raise ToolError(f"no AI on port {port} right now")
        return self.games[port]

    # -- read tools

    def t_list_ais(self, args):
        out = []
        for port, link in self.connected():
            h = link.hello
            g = self.games[port]
            frame = g.state()["frame"] if link.state else h.get("frame", 0)
            out.append({"port": port, "ai": h["ai"], "team": h["team"], "allyTeam": h["allyTeam"],
                        "side": h["side"], "gameTime": clock(frame)})
        return {"ais": out} if out else {"ais": [], "note": "no AI connected; start a match with BARB_BRAIN=1"}

    def t_get_overview(self, args):
        g = self.game(args)
        st, meta = g.state(), g.meta()
        eco = st["eco"]
        m = meta["map"]

        units = collections.Counter(g.def_name(u[1]) for u in st["units"])
        tasks = st["tasks"]
        squads = []
        for t in tasks:
            if t.get("fight") is None or t["n"] == 0:
                continue
            squads.append({"type": meta["fightTypes"][t["fight"]], "units": t["n"], "power": round(t.get("power", 0)),
                           "at": g.pos(t.get("pos")), "target": g.pos(t.get("targetPos"))})
        squads.sort(key=lambda s: -s["power"])
        jobs = collections.Counter()
        idle_jobs = collections.Counter()
        for t in tasks:
            if t.get("build") is None or t.get("recruit") is not None:
                continue
            name = meta["buildTypes"][t["build"]] if t["build"] < len(meta["buildTypes"]) else "?"
            (jobs if t["n"] else idle_jobs)[name] += 1
        queue = [g.def_name(t["def"]) for t in tasks if t.get("recruit") is not None and "def" in t]

        enemies = st["enemies"]
        seen = sum(1 for e in enemies if e[4] & 1)
        radar = sum(1 for e in enemies if not e[4] & 1 and e[4] & 2)
        enemy_types = collections.Counter(g.def_name(e[1]) for e in enemies if e[1] >= 0)
        groups = sorted(st["groups"], key=lambda gr: -gr[3])[:8]

        mil = st["military"]
        roles = []
        for i, name in enumerate(meta["roles"]):
            ours, theirs = mil["roleCost"][i], mil["enemyRoleCost"][i]
            if ours > 0 or theirs > 0:
                roles.append({"role": name, "ours": round(ours), "enemy": round(theirs)})
        roles.sort(key=lambda r: -max(r["ours"], r["enemy"]))

        c = st.get("control") or {}
        return {
            "gameTime": clock(st["frame"]),
            "map": {"name": m["name"], "width": m["width"], "height": m["height"],
                    "sectors": f"{SECTOR_COLS} columns A-{chr(ord('A') + SECTOR_COLS - 1)} of "
                               f"{round(g.sector_size())} elmos; rows numbered from the north"},
            "base": g.pos(st["basePos"]),
            "enemyCentre": g.pos(st["enemyPos"]),
            "economy": {
                "metal": {"income": round(eco["mInc"], 1), "pull": round(eco["mPull"], 1),
                          "stored": round(eco["mCur"]), "storage": round(eco["mStore"])},
                "energy": {"income": round(eco["eInc"], 1), "stored": round(eco["eCur"]), "storage": round(eco["eStore"])},
                "flags": [k for k in ("metalEmpty", "metalFull", "energyStalling", "energyEmpty", "energyFull") if eco.get(k)],
            },
            "ourUnits": {"total": len(st["units"]), "byType": dict(units.most_common(20)),
                         "armyMetalCost": round(mil["armyCost"])},
            "armyByRole": roles[:12],
            "squads": squads[:10],
            "buildJobs": {"active": dict(jobs), "waitingForBuilder": dict(idle_jobs)},
            "factoryQueue": queue,
            "enemies": {
                "known": len(enemies), "inSight": seen, "radarOnly": radar,
                "mobileThreat": round(mil["enemyMobileThreat"]), "staticThreat": round(mil["enemyStaticThreat"]),
                "byType": dict(enemy_types.most_common(15)),
                "strongestGroups": [{"at": g.pos(gr[:2]), "units": gr[5], "metalCost": round(gr[2]),
                                     "strength": round(gr[3], 1)} for gr in groups if gr[5]],
            },
            "control": {
                "focus": ({"at": g.pos([c["focus"]["x"], c["focus"]["z"]]), "radius": round(c["focus"]["radius"]),
                           "force": c["focus"]["force"], "lapsesAt": clock(c["focus"]["until"])}
                          if c.get("focus") else None),
                "changedTuning": {k: {"now": round(c["tuning"][k], 3), "was": round(v, 3)}
                                  for k, v in (c.get("changed") or {}).items()},
                "unitLimits": [{"unit": g.def_name(d), "limit": lim, "was": was} for d, lim, was in c.get("limits", [])],
                "unitsUnderYourControl": len(c.get("controlled", [])),
            },
        }

    LAYERS = {
        "influence": ("influence", "inflGrid", "who controls the ground: + ours, - enemy"),
        "enemy": ("inflEnemy", "inflGrid", "enemy military influence"),
        "ally": ("inflAlly", "inflGrid", "our military influence"),
        "threat": ("threatSurf", "threatGrid", "damage a ground unit would take"),
        "air_threat": ("threatAir", "threatGrid", "damage an aircraft would take"),
    }

    def t_get_map(self, args):
        g = self.game(args)
        name = args.get("layer", "influence")
        if name not in self.LAYERS:
            raise ToolError(f"layer must be one of {sorted(self.LAYERS)}")
        key, grid_key, desc = self.LAYERS[name]
        st, meta = g.state(), g.meta()
        grid = meta[grid_key]
        data = floats(st["layers"].get(key))
        if len(data) != grid["w"] * grid["h"]:
            raise ToolError(f"layer {name} is not ready yet")

        # One character per sector, using the strongest cell in it
        size = g.sector_size()
        rows = math.ceil(meta["map"]["height"] / size)
        cells = [[None] * SECTOR_COLS for _ in range(rows)]
        for gz in range(grid["h"]):
            for gx in range(grid["w"]):
                v = data[gz * grid["w"] + gx]
                c = min(int(gx * grid["cell"] // size), SECTOR_COLS - 1)
                r = min(int(gz * grid["cell"] // size), rows - 1)
                cur = cells[r][c]
                if cur is None or abs(v) > abs(cur):
                    cells[r][c] = v
        flat = [abs(v) for row in cells for v in row if v]
        scale = max(flat) if flat else 1.0
        diverging = name == "influence"

        def symbol(v):
            if v is None or abs(v) < scale * 0.05:
                return "."
            t = abs(v) / scale
            if diverging:
                return ("o" if t < 0.5 else "O") if v > 0 else ("x" if t < 0.5 else "X")
            return "1234"[min(3, int(t * 4))]

        marks = {}
        for label, p in (("B", st["basePos"]), ("E", st["enemyPos"])):
            if p and p[0] >= 0:
                marks[g.sector(*p)] = label
        c = st.get("control") or {}
        if c.get("focus"):
            marks[g.sector(c["focus"]["x"], c["focus"]["z"])] = "F"

        lines = ["    " + " ".join(chr(ord("A") + i) for i in range(SECTOR_COLS))]
        for r in range(rows):
            row = []
            for col in range(SECTOR_COLS):
                sec = f"{chr(ord('A') + col)}{r + 1}"
                row.append(marks.get(sec, symbol(cells[r][col])))
            lines.append(f"{r + 1:>3} " + " ".join(row))
        legend = ("O/o strong/weak ours, X/x strong/weak enemy, . contested or empty" if diverging
                  else "1-4 low to high, . none")
        return {"layer": name, "meaning": desc, "peak": round(scale, 2),
                "legend": legend + "; B our base, E enemy centre, F attack focus",
                "grid": "\n".join(lines)}

    def t_get_units(self, args):
        g = self.game(args)
        st, meta = g.state(), g.meta()
        defs = g.defs()
        role = args.get("role")
        name = args.get("unit")
        near = args.get("near")
        limit = int(args.get("limit", 60))
        held = {u[0] for u in (st.get("control") or {}).get("controlled", [])}
        tasks = {t["id"]: t for t in st["tasks"]}
        out = []
        for uid, did, x, z, hp, tid in st["units"]:
            d = defs.get(did, {})
            if role and d.get("role") != role:
                continue
            if name and d.get("name") != name:
                continue
            if near and (x - near["x"]) ** 2 + (z - near["z"]) ** 2 > near.get("radius", 1000) ** 2:
                continue
            t = tasks.get(tid)
            out.append({"id": uid, "unit": d.get("name", "?"), "role": d.get("role", "?"),
                        "at": g.pos([x, z]), "health": round(hp, 2) if isinstance(hp, (int, float)) else None,
                        "doing": self._task_label(g, t), "yourControl": uid in held})
        out.sort(key=lambda u: (u["role"], u["unit"]))
        return {"count": len(out), "units": out[:limit], "truncated": len(out) > limit}

    @staticmethod
    def _task_label(g, t):
        if t is None:
            return "nothing"
        meta = g.meta()
        if t.get("recruit") is not None:
            return f"recruit {g.def_name(t['def'])}" if "def" in t else "recruit"
        if t.get("build") is not None:
            kind = meta["buildTypes"][t["build"]] if t["build"] < len(meta["buildTypes"]) else "build"
            return f"{kind} {g.def_name(t['def'])}" if "def" in t else kind
        if t.get("fight") is not None:
            return f"fight {meta['fightTypes'][t['fight']]}"
        return meta["taskTypes"][t["type"]] if t["type"] < len(meta["taskTypes"]) else "task"

    def t_get_enemies(self, args):
        g = self.game(args)
        st = g.state()
        limit = int(args.get("limit", 40))
        base = st["basePos"]
        out = []
        for eid, did, x, z, los, cost in st["enemies"]:
            seen = "fake (inferred from damage)" if eid < 0 else "in sight" if los & 1 else \
                "radar blip" if los & 2 else "last known position"
            out.append({"unit": g.def_name(did), "at": g.pos([x, z]), "seen": seen, "metalCost": round(cost),
                        "_d": (x - base[0]) ** 2 + (z - base[1]) ** 2})
        out.sort(key=lambda e: e["_d"])
        for e in out:
            del e["_d"]
        groups = [{"at": g.pos(gr[:2]), "units": gr[5], "metalCost": round(gr[2]), "strength": round(gr[3], 1)}
                  for gr in sorted(st["groups"], key=lambda gr: -gr[3]) if gr[5]]
        return {"known": len(out), "closestToOurBaseFirst": out[:limit], "truncated": len(out) > limit,
                "groups": groups}

    def t_get_decisions(self, args):
        g = self.game(args)
        kind = args.get("kind")
        limit = max(1, min(int(args.get("limit", 5)), 20))
        out = []
        for raw in reversed(list(g.link.decisions)):
            d = json.loads(raw)
            if kind and d["kind"] != kind:
                continue
            opts = []
            for i, o in enumerate(d["options"]):
                # Builder labels name the job kind; the def says what it builds
                label = f"{o['label']} {g.def_name(o['def'])}" if d["kind"] == "builder" and "def" in o else o["label"]
                entry = {"option": label, "score": round(o["score"], 3) if "score" in o else None}
                if o.get("pos"):
                    entry["at"] = g.pos(o["pos"])
                if o.get("reject"):
                    entry["ruledOut"] = o["reject"]
                if i == d["chosen"]:
                    entry["chosen"] = True
                opts.append(entry)
            out.append({"kind": d["kind"], "gameTime": clock(d["frame"]),
                        "unit": g.def_name(d["unitDef"]) if "unitDef" in d else None,
                        "path": [f"{node}: {outcome}" for node, outcome in d["path"]],
                        "inputs": d.get("inputs", {}), "options": opts[:12],
                        "ruledOutCounts": d.get("rejected", {}), "result": d.get("result")})
            if len(out) >= limit:
                break
        return {"decisions": out, "note": "most recent first"}

    def t_get_tuning(self, args):
        g = self.game(args)
        c = g.state().get("control")
        if not c:
            raise ToolError("this AI build does not report tuning")
        return {"values": {k: round(v, 4) for k, v in sorted(c["tuning"].items())},
                "changed": c.get("changed", {}),
                "ranges": "factory.* 0-10; quota.scouts 0-50; quota.attack/raidMin/raidAvg 0-10000; "
                          "role.<role>.maxPercent 0-1; role.<role>.factor 0-100"}

    # -- MCP plumbing

    def dispatch(self, msg):
        method = msg.get("method")
        mid = msg.get("id")
        if mid is None:
            return None  # notification
        try:
            if method == "initialize":
                asked = (msg.get("params") or {}).get("protocolVersion")
                return self.reply(mid, {
                    "protocolVersion": asked if asked in PROTOCOLS else PROTOCOLS[0],
                    "capabilities": {"tools": {"listChanged": False}},
                    "serverInfo": {"name": SERVER_NAME, "version": SERVER_VERSION},
                    "instructions": INSTRUCTIONS,
                })
            if method == "ping":
                return self.reply(mid, {})
            if method == "tools/list":
                return self.reply(mid, {"tools": [t["spec"] for t in TOOLS]})
            if method == "tools/call":
                params = msg.get("params") or {}
                return self.reply(mid, self.call(params.get("name"), params.get("arguments") or {}))
            return self.error(mid, -32601, f"method not found: {method}")
        except Exception as exc:  # never let one request kill the server
            return self.error(mid, -32603, f"internal error: {exc}")

    def call(self, name, args):
        tool = next((t for t in TOOLS if t["spec"]["name"] == name), None)
        if tool is None:
            return {"content": [{"type": "text", "text": f"unknown tool {name}"}], "isError": True}
        try:
            result = tool["fn"](self, args)
        except ToolError as exc:
            return {"content": [{"type": "text", "text": str(exc)}], "isError": True}
        except (KeyError, ValueError, TypeError) as exc:
            return {"content": [{"type": "text", "text": f"bad arguments or data: {exc!r}"}], "isError": True}
        return {"content": [{"type": "text", "text": json.dumps(result, ensure_ascii=False)}], "isError": False}

    @staticmethod
    def reply(mid, result):
        return {"jsonrpc": "2.0", "id": mid, "result": result}

    @staticmethod
    def error(mid, code, message):
        return {"jsonrpc": "2.0", "id": mid, "error": {"code": code, "message": message}}

    def write(self, obj):
        with self.out_lock:
            sys.stdout.write(json.dumps(obj, ensure_ascii=False) + "\n")
            sys.stdout.flush()

    def serve(self):
        for line in sys.stdin:
            line = line.strip()
            if not line:
                continue
            try:
                msg = json.loads(line)
            except ValueError:
                self.write(self.error(None, -32700, "parse error"))
                continue
            reply = self.dispatch(msg) if isinstance(msg, dict) else self.error(None, -32600, "invalid request")
            if reply is not None:
                self.write(reply)


TOOLS = []  # filled by tool() below
PORT_ARG = {"port": {"type": "integer", "description": "AI brain port from list_ais; optional when one AI is connected"}}


def tool(name, description, properties=None, required=()):
    def wrap(fn):
        TOOLS.append({"fn": fn, "spec": {
            "name": name, "description": description,
            "inputSchema": {"type": "object", "properties": {**PORT_ARG, **(properties or {})},
                            "required": list(required), "additionalProperties": False}}})
        return fn
    return wrap


tool("list_ais", "List the BARb AIs currently streaming, with their port, team and game time.")(Server.t_list_ais)
tool("get_overview",
     "One-call summary of the AI's situation: economy, our units and army by role, active squads and their "
     "targets, build jobs, factory queue, what is known about the enemy, and any changes you have made.")(
    Server.t_get_overview)
tool("get_map",
     "The map as a small sector grid (one character per sector) for one layer of the AI's world model. "
     "Use it to see the front line (influence) or where it is dangerous (threat).",
     {"layer": {"type": "string", "enum": ["influence", "enemy", "ally", "threat", "air_threat"],
                "description": "default influence"}})(Server.t_get_map)
tool("get_units", "List our units with id, type, role, position, health and what each is doing. "
     "Unit ids are what order_units takes.",
     {"role": {"type": "string", "description": "only this role, e.g. raider, assault, builder, aa"},
      "unit": {"type": "string", "description": "only this unit type, e.g. armflash"},
      "near": {"type": "object", "properties": {"x": {"type": "number"}, "z": {"type": "number"},
                                                "radius": {"type": "number"}},
               "required": ["x", "z"], "description": "only units within radius (default 1000) of a point"},
      "limit": {"type": "integer", "description": "default 60"}})(Server.t_get_units)
tool("get_enemies", "Enemy units the AI knows about (closest to our base first) and the enemy groups its "
     "attack logic scores.", {"limit": {"type": "integer", "description": "default 40"}})(Server.t_get_enemies)
tool("get_decisions",
     "The AI's most recent decisions with the branch it took, its inputs and the options it weighed. "
     "Kinds: factory (what to build), builder (which job), attackTarget (which enemy group), retreat.",
     {"kind": {"type": "string", "enum": ["factory", "builder", "attackTarget", "retreat"]},
      "limit": {"type": "integer", "description": "1-20, default 5"}})(Server.t_get_decisions)
tool("get_tuning", "Current values of every tunable the AI exposes, and which ones you changed.")(
    Server.t_get_tuning)


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--ports", default="7710-7717", help="AI brain ports, e.g. 7710-7713")
    args = ap.parse_args()
    server = Server(parse_ports(args.ports))
    time.sleep(0.3)  # let the AI links connect before the first request
    server.serve()


if __name__ == "__main__":
    main()
