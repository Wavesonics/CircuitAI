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

    def point(self, args, what="a position"):
        """[x, z] from either x and z or a sector name in args."""
        if args.get("sector"):
            return self.sector_center(args["sector"])
        if isinstance(args.get("x"), (int, float)) and isinstance(args.get("z"), (int, float)):
            return float(args["x"]), float(args["z"])
        raise ToolError(f"give {what} as x and z, or as a sector like C7")


class Server:
    def __init__(self, ports):
        self.links = {p: AILink(p) for p in ports}
        self.games = {p: Game(link) for p, link in self.links.items()}
        self.out_lock = threading.Lock()
        self.cmd_seq = 0

    # -- AI selection

    def connected(self, wait=0.0):
        """Live AIs; waits up to `wait` seconds for one when none is live yet."""
        deadline = time.time() + wait
        while True:
            live = [(p, l) for p, l in sorted(self.links.items()) if l.connected and l.hello and l.meta]
            if live or time.time() >= deadline:
                return live
            time.sleep(0.2)

    def game(self, args):
        port = args.get("port")
        live = self.connected(wait=3.0)
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
        for port, link in self.connected(wait=3.0):
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
        members = collections.defaultdict(list)
        for u in st["units"]:
            members[u[5]].append(u)
        squads = []
        for t in tasks:
            if t.get("fight") is None or t["n"] == 0:
                continue
            ms = members.get(t["id"], [])
            # A task's pos is where it is heading; the squad is where its units are
            at = [sum(u[2] for u in ms) / len(ms), sum(u[3] for u in ms) / len(ms)] if ms else t.get("pos")
            sq = {"squadId": t["id"], "type": meta["fightTypes"][t["fight"]], "units": t["n"],
                  "composition": dict(collections.Counter(g.def_name(u[1]) for u in ms)),
                  "power": round(t.get("power", 0), 1), "at": g.pos(at),
                  "heading": g.pos(t.get("targetPos") or t.get("pos"))}
            if t.get("promoteAt") is not None:
                sq["type"] = f"gathering (becomes {meta['fightTypes'][t['promote']]} at power {round(t['promoteAt'], 1)})"
            squads.append(sq)
        squads.sort(key=lambda s: -s["power"])
        jobs = collections.Counter()
        idle_jobs = collections.Counter()
        for t in tasks:
            if t.get("build") is None or t.get("recruit") is not None:
                continue
            name = meta["buildTypes"][t["build"]] if t["build"] < len(meta["buildTypes"]) else "?"
            (jobs if t["n"] else idle_jobs)[name] += 1
        queue = [g.def_name(t["def"]) for t in tasks if t.get("recruit") is not None and "def" in t]

        # Fakes (id -1) are guesses from unseen damage, kept apart from real sightings
        enemies = [e for e in st["enemies"] if e[0] >= 0]
        inferred = len(st["enemies"]) - len(enemies)
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
            # Before any sighting enemyPos is only the AI's starting guess
            "enemyCentre": g.pos(st["enemyPos"]) if any(e[0] >= 0 for e in st["enemies"]) else None,
            "economy": {
                "metal": {"income": round(eco["mInc"], 1), "pull": round(eco["mPull"], 1),
                          "stored": round(eco["mCur"]), "storage": round(eco["mStore"])},
                "energy": {"income": round(eco["eInc"], 1), "stored": round(eco["eCur"]), "storage": round(eco["eStore"])},
                "flags": [k for k in ("metalEmpty", "metalFull", "energyStalling", "energyEmpty", "energyFull") if eco.get(k)],
            },
            "ourUnits": {"total": len(st["units"]), "byType": dict(units.most_common(20)),
                         "armyMetalCost": round(mil["armyCost"])},
            # A unit can have several roles, so these overlap and do not sum to the army
            "armyByRole": roles[:12],
            "squads": squads[:10],
            "buildJobs": {"active": dict(jobs), "waitingForBuilder": dict(idle_jobs)},
            "factoryQueue": queue,
            "enemies": {
                "known": len(enemies), "inSight": seen, "radarOnly": radar,
                "unseenAttackers": inferred,
                "mobileThreat": round(mil["enemyMobileThreat"]), "staticThreat": round(mil["enemyStaticThreat"]),
                "byType": dict(enemy_types.most_common(15)),
                "strongestGroups": [self._group(g, gr) for gr in groups if gr[5]],
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

        # One character per sector, from the strongest positive and negative cells in it
        size = g.sector_size()
        rows = math.ceil(meta["map"]["height"] / size)
        hi = [[0.0] * SECTOR_COLS for _ in range(rows)]
        lo = [[0.0] * SECTOR_COLS for _ in range(rows)]
        for gz in range(grid["h"]):
            for gx in range(grid["w"]):
                v = data[gz * grid["w"] + gx]
                c = min(int(gx * grid["cell"] // size), SECTOR_COLS - 1)
                r = min(int(gz * grid["cell"] // size), rows - 1)
                hi[r][c] = max(hi[r][c], v)
                lo[r][c] = min(lo[r][c], v)
        scale = max([max(hi[r][c], -lo[r][c]) for r in range(rows) for c in range(SECTOR_COLS)] + [1e-6])
        diverging = name == "influence"

        def symbol(r, c):
            pos, neg = hi[r][c] / scale, -lo[r][c] / scale
            if diverging:
                if pos >= 0.25 and neg >= 0.25:
                    return "#"  # both sides strong: a front line
                t, v = (pos, 1) if pos >= neg else (neg, -1)
                if t < 0.05:
                    return "."
                return ("o" if t < 0.5 else "O") if v > 0 else ("x" if t < 0.5 else "X")
            t = max(pos, neg)
            return "." if t < 0.05 else "1234"[min(3, int(t * 4))]

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
                row.append(marks.get(sec, symbol(r, col)))
            lines.append(f"{r + 1:>3} " + " ".join(row))
        legend = ("O/o strong/weak ours, X/x strong/weak enemy, # both sides strong (front line), . empty"
                  if diverging else "1-4 low to high, . none")
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
            squad = tid if (t is not None and t.get("fight") is not None) else None
            out.append({"id": uid, "squadId": squad, "unit": d.get("name", "?"), "role": d.get("role", "?"),
                        "structure": not d.get("mobile", True), "at": g.pos([x, z]), "health": round(hp, 2) if isinstance(hp, (int, float)) else None,
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

    @staticmethod
    def _group(g, gr):
        # Radar-only units have no known type, so a group of them has no known cost
        return {"at": g.pos(gr[:2]), "units": gr[5], "metalCost": round(gr[2]) if gr[2] > 0 else "unknown",
                "strength": round(gr[3], 1)}

    def t_get_enemies(self, args):
        g = self.game(args)
        st = g.state()
        limit = int(args.get("limit", 40))
        base = st["basePos"]
        out = []
        unseen = []
        frame = st["frame"]
        for e in st["enemies"]:
            eid, did, x, z, los, cost = e[:6]
            last_seen = e[6] if len(e) > 6 else -1
            if eid < 0:
                # Placed where damage came from; the type is the AI's guess
                unseen.append({"at": g.pos([x, z]), "guessedUnit": g.def_name(did)})
                continue
            if los & 1:
                seen = "in sight"
            elif los & 2:
                seen = "radar blip"
            elif last_seen >= 0:
                seen = f"last seen {round((frame - last_seen) / FPS)} s ago"
            else:
                seen = "last known position"
            out.append({"unit": g.def_name(did), "at": g.pos([x, z]), "seen": seen, "metalCost": round(cost),
                        "_d": (x - base[0]) ** 2 + (z - base[1]) ** 2})
        out.sort(key=lambda e: e["_d"])
        for e in out:
            del e["_d"]
        groups = [self._group(g, gr) for gr in sorted(st["groups"], key=lambda gr: -gr[3]) if gr[5]]
        return {"known": len(out), "closestToOurBaseFirst": out[:limit], "truncated": len(out) > limit,
                "unseenAttackers": unseen, "groups": groups}

    def t_get_decisions(self, args):
        g = self.game(args)
        kind = args.get("kind")
        limit = max(1, min(int(args.get("limit", 5)), 20))
        with_waits = bool(args.get("include_waits", False))
        out = []
        last_key = None
        with g.link.lock:  # the reader thread appends concurrently
            recent = list(g.link.decisions)
        for raw in reversed(recent):
            d = json.loads(raw)
            if kind and d["kind"] != kind:
                continue
            # Nano turrets re-decide to assist every second; that is noise for strategy
            if not with_waits and d["kind"] == "factory" and d["path"][:1] == [["assist unit", "yes"]]:
                continue
            key = (d["kind"], d.get("unitDef"), json.dumps(d["path"]), d.get("result"))
            if key == last_key and out:
                out[-1]["repeats"] = out[-1].get("repeats", 1) + 1
                out[-1]["earliest"] = clock(d["frame"])
                continue
            last_key = key
            opts = []
            for i, o in enumerate(d["options"]):
                if o["label"].endswith("(0 units)"):
                    continue  # an empty k-means cluster, never a real candidate
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
        return {"decisions": out, "note": "most recent first; identical back-to-back decisions are merged "
                                          "(repeats, earliest)"}

    def t_get_tuning(self, args):
        g = self.game(args)
        # Asked fresh rather than read from the last state, which can be a second old
        c = self.command(g, "control").get("control")
        if not c:
            raise ToolError("this AI build does not report tuning")
        return {"values": {k: round(v, 4) for k, v in sorted(c["tuning"].items())},
                "changed": c.get("changed", {}),
                "ranges": "factory.* 0-10; quota.scouts 0-50; quota.attack/raidMin/raidAvg 0-10000; "
                          "role.<role>.maxPercent 0-1; role.<role>.factor 0-100"}

    # -- control tools

    MAX_ORDER_UNITS = 200

    def command(self, g, op, **fields):
        self.cmd_seq += 1
        obj = {"t": "cmd", "op": op, "id": f"mcp-{self.cmd_seq}", **fields}
        ack = g.link.request(obj, timeout=5.0)
        if ack is None:
            # The AI reads commands each sim frame, so a paused or lagging game
            # answers late but still applies the command
            raise ToolError("no answer from the AI within 5 s. If the game is paused or lagging the command "
                            "may still apply when it resumes: check get_overview before retrying")
        if not ack.get("ok"):
            raise ToolError(f"the AI refused: {ack.get('error', 'no reason given')}")
        return ack

    def control_summary(self, g, ack):
        c = ack.get("control") or {}
        out = {"ok": True}
        if ack.get("result"):
            res = dict(ack["result"])
            if "pos" in res:
                res["pos"] = g.pos(res["pos"])
            if "returnsToAI" in res:
                res["returnsToAI"] = clock(res["returnsToAI"])
            if res.get("skipped"):
                res["skipped"] = [{"id": i, "why": why} for i, why in res["skipped"]]
            out["result"] = res
        f = c.get("focus")
        out["focus"] = ({"at": g.pos([f["x"], f["z"]]), "radius": round(f["radius"]), "force": f["force"],
                         "lapsesAt": clock(f["until"])} if f else None)
        out["changedTuning"] = {k: {"now": round(c["tuning"][k], 3), "was": round(v, 3)}
                                for k, v in (c.get("changed") or {}).items()}
        out["unitsUnderYourControl"] = len(c.get("controlled", []))
        return out

    def t_set_tuning(self, args):
        g = self.game(args)
        return self.control_summary(g, self.command(g, "set", key=args["key"], value=args["value"]))

    def t_reset(self, args):
        g = self.game(args)
        fields = {"key": args["key"]} if args.get("key") is not None else {}
        return self.control_summary(g, self.command(g, "reset", **fields))

    def t_limit_unit(self, args):
        g = self.game(args)
        return self.control_summary(g, self.command(g, "limit", **{"def": args["unit"], "limit": args.get("limit")}))

    def t_build(self, args):
        g = self.game(args)
        x, z = g.point(args, "where to build")
        fields = {"def": args["unit"], "x": round(x), "z": round(z)}
        if args.get("type"):
            fields["type"] = args["type"]
        return self.control_summary(g, self.command(g, "build", **fields))

    def t_recruit(self, args):
        g = self.game(args)
        fields = {"def": args["unit"], "count": int(args.get("count", 1))}
        if args.get("factory") is not None:
            fields["factory"] = int(args["factory"])
        return self.control_summary(g, self.command(g, "recruit", **fields))

    def select_units(self, g, args):
        """(ids, matched) from explicit ids, or from role/type/area filters over the latest state.
        Area-only selections leave out the commander and constructors, which a stray fight order
        would throw away; name their role or type to order them."""
        if args.get("units"):
            ids = [int(u) for u in args["units"]]
            return ids[:self.MAX_ORDER_UNITS], len(ids)
        if args.get("squad") is not None:
            squad = int(args["squad"])
            ids = [u[0] for u in g.state()["units"] if u[5] == squad]
            if not ids:
                raise ToolError(f"squad {squad} has no units now (see squadId in get_overview)")
            return ids[:self.MAX_ORDER_UNITS], len(ids)
        role, name = args.get("role"), args.get("unit")
        area = args.get("from_sector")
        if not (role or name or area):
            raise ToolError("say which units: units (ids), squad, role, unit type and/or from_sector")
        defs = g.defs()
        st = g.state()
        if area:
            cx, cz = g.sector_center(area)
            half = g.sector_size() / 2
        ids = []
        for uid, did, x, z, _hp, _tid in st["units"]:
            d = defs.get(did, {})
            if not d.get("mobile", True):
                continue
            if role and d.get("role") != role:
                continue
            if name and d.get("name") != name:
                continue
            if not (role or name) and d.get("role") in ("commander", "builder", "builderT2"):
                continue
            if area and (abs(x - cx) > half or abs(z - cz) > half):
                continue
            ids.append(uid)
        if not ids:
            raise ToolError("no mobile units match that selection")
        return ids[:self.MAX_ORDER_UNITS], len(ids)

    def t_order_units(self, args):
        g = self.game(args)
        action = args["action"]
        ids, matched = self.select_units(g, args)
        fields = {"action": action, "units": ids}
        if action in ("move", "fight"):
            tx, tz = g.point({"x": args.get("x"), "z": args.get("z"), "sector": args.get("to_sector")}, "the destination")
            fields.update(x=round(tx), z=round(tz))
        if args.get("seconds") is not None:
            fields["seconds"] = int(args["seconds"])
        out = self.control_summary(g, self.command(g, "units", **fields))
        out["selected"] = len(ids)
        if matched > len(ids):
            out["leftOut"] = f"{matched - len(ids)} more units matched; at most {self.MAX_ORDER_UNITS} per order"
        return out

    def t_set_focus(self, args):
        g = self.game(args)
        x, z = g.point(args, "the focus")
        fields = {"x": round(x), "z": round(z), "force": bool(args.get("force", False))}
        for k in ("radius", "seconds"):
            if args.get(k) is not None:
                fields[k] = args[k]
        return self.control_summary(g, self.command(g, "focus", **fields))

    def t_clear_focus(self, args):
        g = self.game(args)
        return self.control_summary(g, self.command(g, "focus", clear=True))

    # -- MCP plumbing

    def dispatch(self, msg):
        method = msg.get("method")
        if "id" not in msg:
            return None  # notification
        mid = msg["id"]
        if mid is None:
            return self.error(None, -32600, "request id must not be null")
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
     "Kinds: factory (what to build), builder (which job), attackTarget (which enemy group), retreat. "
     "Identical back-to-back decisions are merged, and nano turrets' assist waits are left out.",
     {"kind": {"type": "string", "enum": ["factory", "builder", "attackTarget", "retreat"]},
      "limit": {"type": "integer", "description": "1-20, default 5"},
      "include_waits": {"type": "boolean", "description": "also show nano turrets' assist waits"}})(
    Server.t_get_decisions)
tool("get_tuning", "Current values of every tunable the AI exposes, and which ones you changed.")(
    Server.t_get_tuning)

WHERE = {"x": {"type": "number"}, "z": {"type": "number"},
         "sector": {"type": "string", "description": "sector name like C7, instead of x and z"}}

tool("set_tuning",
     "Change one of the AI's tuning values (see get_tuning). It applies from the AI's next decision and lasts "
     "until reset. Useful keys: factory.buildPowerRatio (0-10, builders per metal income: raise to expand "
     "faster), factory.responseWeight (0-10, how hard production counters the enemy's army), quota.attack "
     "(the floor on a gathering squad's power before it attacks; it also waits until it out-powers the "
     "enemy's strongest group, or the strongest inside your focus, so lowering this alone often changes "
     "little: see promoteAt in get_overview squads), quota.raidMin/raidAvg, "
     "quota.scouts, role.<role>.maxPercent (0-1, cap on that role's share of the army; 0 stops building it), "
     "role.<role>.factor (0-100, how readily that role is built to counter the enemy).",
     {"key": {"type": "string"}, "value": {"type": "number"}}, ("key", "value"))(Server.t_set_tuning)
tool("reset", "Undo your changes: one tuning key, or with no key everything (tuning, unit limits, the attack "
     "focus, and units under your control).", {"key": {"type": "string"}})(Server.t_reset)
tool("limit_unit", "Cap how many of a unit type the AI may have; 0 bans it, null restores the original limit.",
     {"unit": {"type": "string", "description": "unit type name, e.g. armflash"},
      "limit": {"type": ["integer", "null"]}}, ("unit",))(Server.t_limit_unit)
tool("build", "Order a structure at top priority; the nearest builder that can make it goes there. Metal "
     "extractors snap to the nearest free metal spot. The kind (factory, energy, defence, mex, radar...) is "
     "inferred unless given as type.",
     {"unit": {"type": "string", "description": "structure name, e.g. armsolar, armllt, armmex"},
      "type": {"type": "string"}, **WHERE}, ("unit",))(Server.t_build)
tool("recruit", "Queue units at a factory at high priority (built before the AI's own picks; 20 queued at "
     "most). Without factory, the first factory that can build them is used.",
     {"unit": {"type": "string", "description": "unit name, e.g. armflash"},
      "count": {"type": "integer", "description": "1-20, default 1"},
      "factory": {"type": "integer", "description": "factory unit id"}}, ("unit",))(Server.t_recruit)
tool("order_units",
     "Give units a direct order. move/fight (attack-move)/stop take them out of the AI's hands for "
     "`seconds` (default 120, max 1800; long maps need more), after which the AI uses them again. While "
     "under your control they do NOT retreat when damaged, so watch them or keep orders short. retreat "
     "sends them to the AI's retreat behaviour; release hands them back now. Pick units by ids (squadId "
     "via get_units or squad), or by role, unit type and/or the sector they are in (structures are never "
     "selected). The result lists units skipped and why, and when control lapses. Prefer set_focus for "
     "steering the whole army; use this for specific strikes or rescues.",
     {"action": {"type": "string", "enum": ["fight", "move", "stop", "retreat", "release"]},
      "units": {"type": "array", "items": {"type": "integer"}},
      "squad": {"type": "integer", "description": "all units of one squad (squadId from get_overview)"},
      "role": {"type": "string"}, "unit": {"type": "string"},
      "from_sector": {"type": "string", "description": "only units inside this sector; without role or unit "
                                                       "this skips the commander and constructors"},
      "x": {"type": "number"}, "z": {"type": "number"},
      "to_sector": {"type": "string", "description": "destination sector, instead of x and z"},
      "seconds": {"type": "integer"}}, ("action",))(Server.t_order_units)
tool("set_focus",
     "Point the AI's army at an area. While set, gathering squads go on the attack as soon as they "
     "out-power the strongest enemy group inside the area (quota.attack is the floor), attack squads only "
     "take targets within radius of it (nearest to it first), and with nothing to shoot they travel there "
     "if they out-power what is in it. Without force the AI still refuses fights it would lose, so a focus "
     "on a strong position may move nothing until the army is big enough (check promoteAt in get_overview "
     "squads); force skips the strength checks. Lapses after seconds (default 300, max 3600). Raids and "
     "scouts are unaffected.",
     {**WHERE, "radius": {"type": "number", "description": "default 1000 elmos"},
      "force": {"type": "boolean"}, "seconds": {"type": "integer"}})(Server.t_set_focus)
tool("clear_focus", "Let attack squads choose their own targets again.")(Server.t_clear_focus)


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--ports", default="7710-7717", help="AI brain ports, e.g. 7710-7713")
    args = ap.parse_args()
    server = Server(parse_ports(args.ports))
    server.serve()


if __name__ == "__main__":
    main()
