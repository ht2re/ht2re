#!/usr/bin/env python3
"""traffic_sim.py -- offline mirror of src/game/Traffic.cpp.

Reads every original/ENV/*.way with its own tiny RSEG/RNOD reader (the format is
documented in src/world/Way.h), rebuilds the same road graph, runs the same AI
traffic simulation for a few hundred ticks and prints the numbers quoted in
docs/traffic_ai.md:

  * vertices / edges of the welded road graph,
  * named RNOD waypoints that resolve to a graph vertex,
  * mean speed,
  * cars sitting at their lane offset from the centreline,
  * U-turns (must be 0),
  * cars that got stuck.

The C++ module is the reference; this file exists so the behaviour can be
checked without building the game.  It follows Traffic.cpp line for line, down
to the tuning constants, the LCG and the LRU route-field cache, so the two
agree up to float-vs-double rounding.

Usage:
  python3 tools/traffic_sim.py                       # 200 cars, 600 ticks
  python3 tools/traffic_sim.py --cars 16 --ticks 3000
  python3 tools/traffic_sim.py --mode greedy         # straight-line rule only
  python3 tools/traffic_sim.py --ticks 100 --trace   # per-tick sanity trace
"""
from __future__ import annotations

import argparse
import glob
import heapq
import math
import os
import struct
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
ENV = os.path.join(ROOT, 'original', 'ENV')

# --- tuning: keep in sync with src/game/Traffic.cpp -------------------------
MIN_EDGE_LEN = 0.5
DEFAULT_WIDTH = 8.0
MIN_WIDTH = 2.5
MAX_WIDTH = 40.0
CAR_LENGTH = 4.5
ACCEL = 3.0
BRAKE = 7.0
HEADWAY_MIN = 3.0
HEADWAY_TIME = 0.7
STAND_GAP = 2.0
UTURN_DOT = -0.85
STUCK_SPEED = 0.4
STUCK_TIME = 3.0
GRIDLOCK_TIME = 10.0
DEST_TIMEOUT = 240.0
MAX_TRANSITIONS = 4
RELOCATE_TRIES = 64
SPAWN_GAP = 20.0
MIN_DEST_DIST = 300.0
MAX_DEST_DIST = 1800.0
ROUTE_CACHE = 24
ROUTE_BUDGET = 6
LANE_OFFSET = 2.0
DEST_REACH = 30.0
SPAWN = (-3020.0, -4625.0)          # ENV/GAME.CNF (startposition -3020 -4625 20)
RADIUS = 900.0
DT = 1.0 / 30.0


# ---------------------------------------------------------------------------
# .way reader (WTWR chunked container: <tag[4]><u32 size><data>, aligned to 4)
# ---------------------------------------------------------------------------
def _u32(d: bytes, off: int) -> int:
    return struct.unpack_from('<I', d, off)[0]


def _al(n: int) -> int:
    return (n + 3) & ~3


def _s(b: bytes) -> str:
    return b.split(b'\x00')[0].decode('latin1')


def read_way(path: str):
    """-> (segs, nodes); seg = dict(name, attr, width, pts=[(x,h,z)])"""
    d = open(path, 'rb').read()
    if d[:4] != b'WTWR':
        return [], []
    segs, nodes = [], []

    def walk(off: int, end: int):
        i = off
        while i + 8 <= end:
            tag, size = d[i:i + 4], _u32(d, i + 4)
            de = i + 8 + size
            if de > end:
                break
            if tag == b'RSEG':
                sg = {'name': '', 'attr': 0, 'width': 8.0, 'pts': []}
                j = i + 8
                while j + 8 <= de:
                    t2, s2 = d[j:j + 4], _u32(d, j + 4)
                    pay = d[j + 8:j + 8 + s2]
                    if j + 8 + s2 > de:
                        break
                    if t2 == b'ATTR' and s2 >= 4:
                        sg['attr'] = _u32(pay, 0)
                    elif t2 == b'RTEN':
                        sg['name'] = _s(pay)
                    elif t2 == b'WDTH' and s2 >= 8:
                        sg['width'] = struct.unpack_from('<d', pay, 0)[0]
                    elif t2 == b'VDAT' and s2 >= 4:
                        cnt = _u32(pay, 0)
                        if cnt and s2 >= 4 + cnt * 24:
                            a = struct.unpack_from('<%dd' % (cnt * 3), pay, 4)
                            # POSN/VDAT store x, z, height
                            sg['pts'] = [(a[k * 3], a[k * 3 + 2], a[k * 3 + 1])
                                         for k in range(cnt)]
                    j += 8 + _al(s2)
                if sg['pts']:
                    segs.append(sg)
            elif tag == b'RNOD':
                nd = {'name': '', 'pos': None, 'flag': 0}
                j = i + 8
                while j + 8 <= de:
                    t2, s2 = d[j:j + 4], _u32(d, j + 4)
                    pay = d[j + 8:j + 8 + s2]
                    if j + 8 + s2 > de:
                        break
                    if t2 == b'NNAM':
                        nd['name'] = _s(pay)
                    elif t2 == b'POSN' and s2 >= 24:
                        x, z, h = struct.unpack_from('<3d', pay, 0)
                        nd['pos'] = (x, h, z)
                    elif t2 == b'FLAG' and s2 >= 4:
                        nd['flag'] = _u32(pay, 0)
                    j += 8 + _al(s2)
                if nd['pos']:
                    nodes.append(nd)
            elif tag in (b'GDAT', b'GROM'):
                walk(i + 8, de)
            i += 8 + _al(size)

    walk(8, len(d))
    return segs, nodes


def load_world(env: str = ENV):
    segs, nodes = [], {}
    for path in sorted(glob.glob(os.path.join(env, '*.way'))):
        s, n = read_way(path)
        segs += s
        for nd in n:
            if nd['name']:
                nodes[nd['name']] = nd          # last tile wins, as in App.cpp
    return segs, nodes


def _llround(v: float) -> int:
    """C llround: half away from zero (Python's round() is banker's)."""
    return int(math.floor(v + 0.5)) if v >= 0 else int(math.ceil(v - 0.5))


# ---------------------------------------------------------------------------
# traffic module mirror
# ---------------------------------------------------------------------------
class Car:
    __slots__ = ('x', 'z', 'h', 'yaw', 'speed', 'model', 'dest', 'edge', 'dir', 's',
                 'dest_vertex', 'cruise', 'lane', 'stuck', 'dest_timer', 'recoveries',
                 'yielding')

    def __init__(self):
        self.x = self.z = self.h = 0.0
        self.yaw = 0.0
        self.speed = 14.0
        self.model = 0
        self.dest = ''
        self.edge = -1
        self.dir = 1
        self.s = 0.0
        self.dest_vertex = -1
        self.cruise = 14.0
        self.lane = 0.0
        self.stuck = 0.0
        self.dest_timer = 0.0
        self.recoveries = 0
        self.yielding = False


class Traffic:
    def __init__(self, seed=0x1234567):
        self.rng = seed
        self.verts = []        # [x, h, z]
        self.vedges = []       # vertex -> [edge ids]
        self.edges = []        # dict(a,b,len,width,attr,name,poly=[(x,h,z,d)])
        self.poly = []
        self.names = []
        self.node_vertex = {}
        self.resolved = []     # [(vertex, name)]
        self.names_offered = 0
        self.cars = []
        self.models = []
        self.leaders = []
        self.lane_offset = LANE_OFFSET
        self.dest_reach = DEST_REACH
        self.max_waypoint_dist = 100.0
        self.routes = []       # dict(dest, stamp, dist, next)
        self.route_stamp = 0
        self.route_budget = 0
        self.active_radius = 0.0
        self.pathfinding = True
        self.stats = dict(cars=0, uturns=0, recycles=0, respawns=0, reroutes=0,
                          stalls=0, arrivals=0, distance=0.0, time=0.0)

    # --- rng ---------------------------------------------------------------
    def next_u32(self):
        self.rng = (self.rng * 1664525 + 1013904223) & 0xFFFFFFFF
        return self.rng

    def next_float(self):
        return ((self.next_u32() >> 8) & 0xFFFFFF) / 16777216.0

    def next_int(self, n):
        return self.next_u32() % n if n > 0 else 0

    # --- build -------------------------------------------------------------
    def build(self, segs, nodes):
        weld = {}
        verts, vedges, edges, poly, names = [], [], [], [], []

        def vertex_of(px, py, pz):
            k = (_llround(px * 100), _llround(py * 100), _llround(pz * 100))
            v = weld.get(k)
            if v is None:
                v = len(verts)
                weld[k] = v
                verts.append((px, py, pz))
                vedges.append([])
            return v

        for sg in segs:
            if len(sg['pts']) < 2:
                continue
            width = sg['width']
            if not (width >= MIN_WIDTH):
                width = DEFAULT_WIDTH
            width = min(width, MAX_WIDTH)
            name_id = -1
            if sg['name']:
                if sg['name'] not in names:
                    names.append(sg['name'])
                name_id = names.index(sg['name'])
            for k in range(len(sg['pts']) - 1):
                p0, p1 = sg['pts'][k], sg['pts'][k + 1]
                a = vertex_of(*p0)
                b = vertex_of(*p1)
                if a == b:
                    continue
                dx = verts[b][0] - verts[a][0]
                dz = verts[b][2] - verts[a][2]
                ln = math.hypot(dx, dz)
                if ln < MIN_EDGE_LEN:
                    continue
                first = len(poly)
                poly.append((verts[a][0], verts[a][1], verts[a][2], 0.0))
                poly.append((verts[b][0], verts[b][1], verts[b][2], ln))
                eid = len(edges)
                edges.append(dict(a=a, b=b, len=ln, width=width, attr=sg['attr'],
                                  name=name_id, first=first, count=2))
                vedges[a].append(eid)
                vedges[b].append(eid)

        self.verts, self.vedges, self.edges, self.poly, self.names = \
            verts, vedges, edges, poly, names

        best_cache = {}
        names_offered = 0
        resolved = []
        for name in sorted(nodes):
            nd = nodes[name]
            x, _, z = nd['pos']
            names_offered += 1
            best, bd = -1, float('inf')
            for i, v in enumerate(verts):
                d = (v[0] - x) ** 2 + (v[2] - z) ** 2
                if d < bd:
                    bd, best = d, i
            if best < 0:
                continue
            best_cache[name] = best
            if math.sqrt(bd) <= self.max_waypoint_dist:
                resolved.append((best, name))
        self.node_vertex = best_cache
        self.resolved = resolved
        self.names_offered = names_offered
        return self

    # --- geometry ----------------------------------------------------------
    def distance_to_edge(self, e, x, z):
        ed = self.edges[e]
        best = float('inf')
        for k in range(ed['first'], ed['first'] + ed['count'] - 1):
            ax, az = self.poly[k][0], self.poly[k][2]
            bx, bz = self.poly[k + 1][0], self.poly[k + 1][2]
            dx, dz = bx - ax, bz - az
            l2 = dx * dx + dz * dz
            t = ((x - ax) * dx + (z - az) * dz) / l2 if l2 > 1e-9 else 0.0
            t = 0.0 if t < 0 else (1.0 if t > 1 else t)
            best = min(best, math.hypot(ax + dx * t - x, az + dz * t - z))
        return best

    def sample(self, e, direction, s):
        """-> (x, h, z, fx, fz); s is metres from the entry vertex."""
        ed = self.edges[e]
        p = self.poly
        i0 = ed['first']
        n = ed['count']
        if s < 0:
            s = 0.0
        if s > ed['len']:
            s = ed['len']
        d = s if direction > 0 else ed['len'] - s
        k = i0
        while k < i0 + n - 2 and p[k + 1][3] < d:
            k += 1
        seg = p[k + 1][3] - p[k][3]
        t = (d - p[k][3]) / seg if seg > 1e-6 else 0.0
        t = 0.0 if t < 0 else (1.0 if t > 1 else t)
        x = p[k][0] + (p[k + 1][0] - p[k][0]) * t
        h = p[k][1] + (p[k + 1][1] - p[k][1]) * t
        z = p[k][2] + (p[k + 1][2] - p[k][2]) * t
        dx, dz = p[k + 1][0] - p[k][0], p[k + 1][2] - p[k][2]
        ln = math.hypot(dx, dz)
        if ln < 1e-6:
            dx, dz, ln = 0.0, 1.0, 1.0
        return x, h, z, dx / ln * direction, dz / ln * direction

    def refresh_pose(self, c):
        ed = self.edges[c.edge]
        c.s = max(0.0, min(c.s, ed['len']))
        x, h, z, fx, fz = self.sample(c.edge, c.dir, c.s)
        c.x, c.h, c.z = x, h, z
        c.yaw = math.degrees(math.atan2(fx, fz))
        c.lane = min(self.lane_offset, ed['width'] / 3.0)
        c.x += -fz * c.lane          # right = forward x up
        c.z += fx * c.lane

    def lane_for(self, e):
        return min(self.lane_offset, self.edges[e]['width'] / 3.0)

    # --- route fields ------------------------------------------------------
    def route_field(self, dest):
        if dest < 0 or dest >= len(self.verts):
            return None
        for f in self.routes:
            if f['dest'] == dest:
                self.route_stamp += 1
                f['stamp'] = self.route_stamp
                return f
        if self.route_budget <= 0:
            return None
        self.route_budget -= 1
        if len(self.routes) < ROUTE_CACHE:
            f = {'dest': dest, 'stamp': 0, 'dist': [], 'next': []}
            self.routes.append(f)
        else:
            f = min(self.routes, key=lambda r: r['stamp'])
        self.compute_route(dest, f)
        self.route_stamp += 1
        f['stamp'] = self.route_stamp
        return f

    def compute_route(self, dest, f):
        n = len(self.verts)
        inf = float('inf')
        dist = [inf] * n
        nxt = [-1] * n
        dist[dest] = 0.0
        pq = [(0.0, dest)]
        edges, vedges, verts = self.edges, self.vedges, self.verts
        while pq:
            d, v = heapq.heappop(pq)
            if d > dist[v] + 1e-4:
                continue
            for ei in vedges[v]:
                e = edges[ei]
                o = e['b'] if e['a'] == v else e['a']
                nd = d + e['len']
                if nd < dist[o] - 1e-4:
                    dist[o] = nd
                    nxt[o] = ei
                    heapq.heappush(pq, (nd, o))
        f['dest'] = dest
        f['dist'] = dist
        f['next'] = nxt

    # --- destinations ------------------------------------------------------
    def pick_resolved_vertex(self, fx, fz, min_dist):
        if not self.resolved:
            return self.next_int(len(self.verts)) if self.verts else -1
        n = len(self.resolved)
        max_d = MAX_DEST_DIST
        if self.active_radius > 0:
            max_d = min(max_d, max(min_dist + 100.0, 0.8 * self.active_radius))
        lo, hi = min_dist * min_dist, max_d * max_d
        fallback, fbd = -1, float('inf')
        for _ in range(32):
            v, _name = self.resolved[self.next_int(n)]
            if len(self.vedges[v]) < 2:
                continue
            x, _, z = self.verts[v]
            d = (x - fx) ** 2 + (z - fz) ** 2
            if lo <= d <= hi:
                return v
            if d >= lo and d < fbd:
                fbd, fallback = d, v
        return fallback if fallback >= 0 else self.resolved[self.next_int(n)][0]

    def new_destination(self, c, x, z):
        c.dest_vertex = self.pick_resolved_vertex(x, z, MIN_DEST_DIST)
        c.dest = ''
        if c.dest_vertex >= 0:
            for v, nm in self.resolved:
                if v == c.dest_vertex:
                    c.dest = nm
                    break
        c.dest_timer = 0.0

    # --- placement ---------------------------------------------------------
    def occupied(self, edge, direction, s, min_gap):
        for o in self.cars:
            if o.edge != edge or o.dir != direction:
                continue
            if abs(o.s - s) < min_gap:
                return True
        return False

    def entry_blocked(self, edge, direction, s, self_car):
        for o in self.cars:
            if o is self_car or o.edge != edge or o.dir != direction:
                continue
            d = o.s - s
            if -CAR_LENGTH < d < CAR_LENGTH + STAND_GAP:
                return True
        return False

    def place_on(self, c, edge, direction, s):
        c.edge, c.dir, c.s = edge, (1 if direction >= 0 else -1), s
        c.stuck, c.recoveries = 0.0, 0
        self.refresh_pose(c)

    def relocate(self, c, px, pz, radius):
        if not self.edges:
            return False
        r2 = radius * radius if radius > 0 else float('inf')
        for _ in range(RELOCATE_TRIES):
            ei = self.next_int(len(self.edges))
            if self.distance_to_edge(ei, px, pz) ** 2 > r2:
                continue
            direction = 1 if (self.next_u32() & 1) else -1
            s = (0.15 + 0.7 * self.next_float()) * self.edges[ei]['len']
            if self.occupied(ei, direction, s, SPAWN_GAP):
                continue
            self.place_on(c, ei, direction, s)
            self.new_destination(c, c.x, c.z)
            return True
        best, bd = -1, float('inf')
        for i in range(len(self.edges)):
            d = self.distance_to_edge(i, px, pz)
            if d < bd:
                bd, best = d, i
        if best < 0:
            return False
        self.place_on(c, best, 1, 0.5 * self.edges[best]['len'])
        self.new_destination(c, c.x, c.z)
        return True

    def spawn(self, px, pz, radius, count):
        self.cars = []
        if radius > 0:
            self.active_radius = radius
        if not self.edges or count <= 0:
            self.stats['cars'] = 0
            return
        r2 = radius * radius if radius > 0 else float('inf')
        near = [i for i in range(len(self.edges))
                if self.distance_to_edge(i, px, pz) ** 2 <= r2]
        if not near:
            near = list(range(len(self.edges)))
        for k in range(count):
            c = Car()
            c.model = self.models[k % len(self.models)] if self.models else 0
            c.cruise = 11.0 + 6.0 * self.next_float()
            c.speed = c.cruise * (0.6 + 0.4 * self.next_float())
            placed = -1
            for attempt in range(8):
                ei = near[self.next_int(len(near))]
                direction = 1 if ((k + attempt) & 1) else -1
                s = (0.1 + 0.8 * self.next_float()) * self.edges[ei]['len']
                if self.occupied(ei, direction, s, SPAWN_GAP):
                    continue
                self.place_on(c, ei, direction, s)
                placed = ei
                break
            if placed < 0:
                continue
            self.new_destination(c, c.x, c.z)
            self.cars.append(c)
        self.stats['cars'] = len(self.cars)

    # --- routing -----------------------------------------------------------
    def build_order(self):
        order = sorted(range(len(self.cars)),
                       key=lambda i: (self.cars[i].edge, self.cars[i].dir,
                                      self.cars[i].s, i))
        leaders = [-1] * len(self.cars)
        for k in range(len(order) - 1):
            a, b = self.cars[order[k]], self.cars[order[k + 1]]
            if a.edge == b.edge and a.dir == b.dir:
                leaders[order[k]] = order[k + 1]
        self.order, self.leaders = order, leaders
        return order

    def advance_junction(self, c, px, pz, radius):
        e = self.edges[c.edge]
        over = c.s - e['len']
        v = e['b'] if c.dir > 0 else e['a']
        prev = e['a'] if c.dir > 0 else e['b']

        if c.dest_vertex == v:
            self.stats['arrivals'] += 1
            self.new_destination(c, self.verts[v][0], self.verts[v][2])

        _, _, _, ifx, ifz = self.sample(c.edge, c.dir, e['len'])
        cur_dist = float('inf')
        if c.dest_vertex >= 0:
            dx = self.verts[v][0] - self.verts[c.dest_vertex][0]
            dz = self.verts[v][2] - self.verts[c.dest_vertex][2]
            cur_dist = math.hypot(dx, dz)
        routed = -1
        if self.pathfinding and c.dest_vertex >= 0:
            rf = self.route_field(c.dest_vertex)
            if rf and v < len(rf['next']):
                routed = rf['next'][v]

        best = best_dir = fwd = fwd_dir = on_route = -1
        route_dir = 1
        best_d, fwd_dot = float('inf'), -2.0
        for j in self.vedges[v]:
            if j == c.edge:
                continue
            ne = self.edges[j]
            other = ne['b'] if ne['a'] == v else ne['a']
            if other == prev:
                continue
            ndir = 1 if ne['a'] == v else -1
            _, _, _, ofx, ofz = self.sample(j, ndir, 0.0)
            dot = ifx * ofx + ifz * ofz
            if dot < UTURN_DOT:
                continue
            if j == routed:
                on_route, route_dir = j, ndir
            if dot > fwd_dot:
                fwd_dot, fwd, fwd_dir = dot, j, ndir
            if c.dest_vertex < 0:
                continue
            d = math.hypot(self.verts[other][0] - self.verts[c.dest_vertex][0],
                           self.verts[other][2] - self.verts[c.dest_vertex][2])
            if d < cur_dist - 1.0 and d < best_d:
                best_d, best, best_dir = d, j, ndir

        if on_route >= 0:
            pick, pick_dir = on_route, route_dir
        elif best >= 0:
            pick, pick_dir = best, best_dir
        else:
            pick, pick_dir = fwd, fwd_dir

        if pick == c.edge:
            self.stats['uturns'] += 1
        elif pick >= 0:
            _, _, _, ofx, ofz = self.sample(pick, pick_dir, 0.0)
            if ifx * ofx + ifz * ofz < UTURN_DOT:
                self.stats['uturns'] += 1

        if pick < 0:                                  # dead end: recycle, no U-turn
            self.stats['recycles'] += 1
            self.relocate(c, px, pz, radius)
            return False
        entry = max(0.0, over)
        if self.entry_blocked(pick, pick_dir, entry, c):
            c.s = e['len']
            c.speed = 0.0
            c.yielding = True
            return False
        c.edge, c.dir, c.s = pick, pick_dir, entry
        return True

    # --- update ------------------------------------------------------------
    def update(self, dt, px, pz, radius):
        if not self.edges or not self.cars or dt <= 0:
            return
        if radius > 0:
            self.active_radius = radius
        dt = min(dt, 0.25)
        st = self.stats
        st['cars'] = len(self.cars)
        st['time'] += dt * len(self.cars)
        self.route_budget = ROUTE_BUDGET
        self.build_order()
        leaders = self.leaders

        # acceleration + integration
        for i, c in enumerate(self.cars):
            if c.edge < 0 or c.edge >= len(self.edges):
                self.relocate(c, px, pz, radius)
                continue
            c.yielding = False
            target = c.cruise
            li = leaders[i]
            if 0 <= li < len(self.cars):
                l = self.cars[li]
                gap = l.s - c.s - CAR_LENGTH
                want = HEADWAY_MIN + HEADWAY_TIME * c.speed
                if gap <= 0:
                    target = 0.0
                else:
                    if gap < want:
                        target = min(target, l.speed * (gap / want))
                    target = min(target, l.speed + gap)
            dv = target - c.speed
            lim = (ACCEL if dv > 0 else BRAKE) * dt
            c.speed += max(-lim, min(lim, dv))
            if c.speed < 0:
                c.speed = 0.0
            travel = c.speed * dt
            st['distance'] += travel
            c.s += travel
            guard = 0
            while c.s > self.edges[c.edge]['len'] and guard < MAX_TRANSITIONS:
                guard += 1
                if not self.advance_junction(c, px, pz, radius):
                    break
            if 0 <= c.edge < len(self.edges):
                ln = self.edges[c.edge]['len']
                c.s = max(0.0, min(c.s, ln))
            c.dest_timer += dt

        # hard no-overlap pass, front to back
        order = self.build_order()
        for k in range(len(order) - 1, 0, -1):
            back, front = self.cars[order[k - 1]], self.cars[order[k]]
            if back.edge != front.edge or back.dir != front.dir:
                continue
            limit = front.s - CAR_LENGTH - STAND_GAP
            if back.s > limit:
                back.s = max(0.0, limit)
                if back.speed > front.speed:
                    back.speed = front.speed

        # poses, destinations, streaming, stall recovery
        for i, c in enumerate(self.cars):
            if c.edge < 0:
                continue
            self.refresh_pose(c)
            if c.dest_vertex >= 0:
                dx = c.x - self.verts[c.dest_vertex][0]
                dz = c.z - self.verts[c.dest_vertex][2]
                if dx * dx + dz * dz <= self.dest_reach ** 2:
                    st['arrivals'] += 1
                    self.new_destination(c, c.x, c.z)
            if c.dest_timer > DEST_TIMEOUT:
                st['reroutes'] += 1
                self.new_destination(c, c.x, c.z)
            if radius > 0 and (c.x - px) ** 2 + (c.z - pz) ** 2 > radius * radius:
                st['respawns'] += 1
                self.relocate(c, px, pz, radius)
                continue
            queued = False
            li = leaders[i]
            if 0 <= li < len(self.cars):
                l = self.cars[li]
                if l.edge == c.edge and l.dir == c.dir:
                    gap = l.s - c.s - CAR_LENGTH
                    if gap < HEADWAY_MIN + HEADWAY_TIME * c.speed:
                        queued = True
            if c.speed < STUCK_SPEED:
                c.stuck += dt
            else:
                c.stuck = 0.0
            if c.stuck > (GRIDLOCK_TIME if (c.yielding or queued) else STUCK_TIME):
                c.stuck = 0.0
                st['stalls'] += 1
                if c.recoveries < 2:
                    c.recoveries += 1
                    self.new_destination(c, c.x, c.z)
                else:
                    c.recoveries = 0
                    st['respawns'] += 1
                    self.relocate(c, px, pz, radius)
        st['cars'] = len(self.cars)

    def mean_speed(self):
        t = self.stats['time']
        return self.stats['distance'] / t if t > 0 else 0.0


# ---------------------------------------------------------------------------
# run + report
# ---------------------------------------------------------------------------
def run(cars=200, ticks=600, radius=RADIUS, mode='shortest', seed=0x1234567,
        dt=DT, trace=False, quiet=False):
    segs, nodes = load_world()
    tr = Traffic(seed)
    tr.pathfinding = (mode == 'shortest')
    tr.build(segs, nodes)
    tr.models = [0, 1, 2, 3, 4]
    tr.spawn(SPAWN[0], SPAWN[1], radius, cars)

    n = len(tr.cars)
    prev = [None] * n
    halt = [0] * n
    max_halt = [0] * n
    uturn_flip = 0
    lane_ok = lane_n = 0
    oncoming_ok = oncoming = 0
    for i, c in enumerate(tr.cars):
        prev[i] = (c.edge, c.dir, c.s, c.x, c.z, c.yaw)

    for t in range(ticks):
        tr.update(dt, SPAWN[0], SPAWN[1], radius)
        for i, c in enumerate(tr.cars):
            pe, pd, ps, pxx, pzz, pyaw = prev[i]
            moved = math.hypot(c.x - pxx, c.z - pzz)
            teleport = moved > c.speed * dt + 2.0 * tr.lane_offset + 0.5
            if teleport:
                halt[i] = 0
            else:
                if c.edge == pe and c.dir != pd and abs(c.s - ps) <= c.speed * dt + 0.5:
                    uturn_flip += 1
                dy = c.yaw - pyaw
                while dy > 180:
                    dy -= 360
                while dy < -180:
                    dy += 360
                if c.edge != pe and abs(dy) > 150:
                    uturn_flip += 1
                if moved < 0.03:
                    halt[i] += 1
                    max_halt[i] = max(max_halt[i], halt[i])
                else:
                    halt[i] = 0
            prev[i] = (c.edge, c.dir, c.s, c.x, c.z, c.yaw)
        if trace and t % max(1, ticks // 5) == 0:
            sys.stderr.write('  t=%5.2fs cars=%d mean=%.2f m/s arrivals=%d\n' %
                             ((t + 1) * dt, len(tr.cars), tr.mean_speed(),
                              tr.stats['arrivals']))

    # final-state checks
    for i, c in enumerate(tr.cars):
        lane_n += 1
        if abs(tr.distance_to_edge(c.edge, c.x, c.z) - c.lane) < 0.25:
            lane_ok += 1
        for j in range(i + 1, len(tr.cars)):
            o = tr.cars[j]
            if o.edge != c.edge or o.dir == c.dir:
                continue
            sep = math.hypot(c.x - o.x, c.z - o.z)
            if sep < 0.5:
                continue
            oncoming += 1
            if sep >= 2.0 * min(c.lane, o.lane) - 0.25:
                oncoming_ok += 1

    # "Stuck" = halted for a while with nobody ahead on the same edge and not
    # simply queued at a junction line waiting for the next edge to free up.
    def really_stuck(i):
        if max_halt[i] < 3 / dt:
            return False
        c = tr.cars[i]
        if c.s > tr.edges[c.edge]['len'] - 1.5:
            return False
        for j, o in enumerate(tr.cars):
            if j == i or o.edge != c.edge or o.dir != c.dir:
                continue
            if 0 < o.s - c.s < 20.0:
                return False
        return True

    stuck_3s = sum(1 for i in range(len(tr.cars)) if really_stuck(i))
    stuck_10s = sum(1 for i in range(len(tr.cars))
                    if really_stuck(i) and max_halt[i] >= 10 / dt)
    st = tr.stats
    return dict(
        mode=mode, cars=len(tr.cars), want_cars=cars, ticks=ticks, dt=dt,
        vertices=len(tr.verts), edges=len(tr.edges), road_names=len(tr.names),
        waypoints=len(tr.resolved), waypoint_names=tr.names_offered,
        mean_speed=tr.mean_speed(), lane_ok=lane_ok, lane_n=lane_n,
        oncoming_ok=oncoming_ok, oncoming=oncoming,
        uturns_module=st['uturns'], uturns_observed=uturn_flip,
        stuck_3s=stuck_3s, stuck_10s=stuck_10s,
        recycles=st['recycles'], respawns=st['respawns'], reroutes=st['reroutes'],
        stalls=st['stalls'], arrivals=st['arrivals'],
        distance=st['distance'], car_time=st['time'],
    )


def report(r):
    print('traffic_sim: %s routing | %d ticks @ %.4f s (%.1f s of traffic)'
          % (r['mode'], r['ticks'], r['dt'], r['ticks'] * r['dt']))
    print('  graph      : %d vertices, %d edges, %d road names'
          % (r['vertices'], r['edges'], r['road_names']))
    print('  waypoints  : %d/%d named RNOD nodes resolved to a vertex'
          % (r['waypoints'], r['waypoint_names']))
    print('  cars       : %d spawned of %d requested' % (r['cars'], r['want_cars']))
    print('  mean speed : %.2f m/s (%.1f km/h)' % (r['mean_speed'], r['mean_speed'] * 3.6))
    print('  lane       : %d/%d cars at their lane offset from the centreline'
          % (r['lane_ok'], r['lane_n']))
    if r['oncoming']:
        print('  oncoming   : %d/%d opposing pairs on their own side of the road'
              % (r['oncoming_ok'], r['oncoming']))
    print('  uturns     : %d (module invariant) | %d (observed dir flips)'
          % (r['uturns_module'], r['uturns_observed']))
    print('  stuck      : %d cars stuck >= 3 s, %d of them >= 10 s (not queued)'
          % (r['stuck_3s'], r['stuck_10s']))
    print('  events     : arrivals %d, reroutes %d, stalls %d, respawns %d, recycles %d'
          % (r['arrivals'], r['reroutes'], r['stalls'], r['respawns'], r['recycles']))
    print('  distance   : %.0f m driven, %.0f car-seconds, %.0f m per arrival'
          % (r['distance'], r['car_time'],
             r['distance'] / r['arrivals'] if r['arrivals'] else 0.0))


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('--cars', type=int, default=60)
    ap.add_argument('--ticks', type=int, default=600)
    ap.add_argument('--radius', type=float, default=RADIUS)
    ap.add_argument('--mode', choices=('shortest', 'greedy'), default='shortest')
    ap.add_argument('--seed', type=lambda s: int(s, 0), default=0x1234567)
    ap.add_argument('--dt', type=float, default=DT)
    ap.add_argument('--trace', action='store_true')
    ap.add_argument('--both', action='store_true',
                    help='run the shortest-path and the straight-line-only mode')
    a = ap.parse_args(argv)
    modes = ('shortest', 'greedy') if a.both else (a.mode,)
    for m in modes:
        r = run(cars=a.cars, ticks=a.ticks, radius=a.radius, mode=m,
                seed=a.seed, dt=a.dt, trace=a.trace)
        report(r)
        if m != modes[-1]:
            print()
    return 0


if __name__ == '__main__':
    sys.exit(main())
