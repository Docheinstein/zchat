# The hub's models and layout, made in Blender from nothing: run it with
#
#     blender -b --factory-startup -P hub/assets/make_assets.py
#
# It writes, next to itself, a .mesh file per model (see "The mesh files" below) and layout.h (where the colliders,
# the lanes and the spawn point are), which the hub builds into itself. Both are checked in: building zchat never
# needs Blender, only changing the models does.
#
# Units are meters. Blender is Z up; the hub is Y up with -Z forward, so a Blender point (x, y, z) is the hub's
# (x, z, -y): north in Blender (+Y) is forward in the hub (-Z).
#
# The mesh files: little-endian, "HUBM", a u32 vertex count, a u32 index count, then the vertices (position and
# normal as 3 f32 each, then the color as 4 u8) and the u32 indices. Faces are flat: each triangle has its own
# vertices, with the face's normal. A color's alpha says whether the player's color tints it (255) or not (0).

import bmesh
import bpy
import math
import os
import random
import struct
from mathutils import Matrix, Vector

HERE = os.path.dirname(os.path.abspath(__file__))

# --- The layout, in the hub's coordinates (x, z on the ground, -z forward): shared with the code by layout.h.

SPAWN = (0.0, 7.0)
FOUNTAIN_RADIUS = 2.3
LANE_X = (-1.2, 1.2)            # the centers of the two lanes
LANE_HALF = 0.525               # half the width of a lane (41.5 inches)
GUTTER = 0.24
FOUL_Z = -18.0                  # the foul line: the pins are this far along the lane, and on
HEADPIN = 18.29                 # 60 feet from it to the head pin
PIN_ROW = 0.2639                # between rows of pins
PIN_SPACING = 0.3048            # between pins in a row
LANE_END = HEADPIN + 0.9        # the pin deck ends here, along the lane
PAVILION_X = 3.2
PAVILION_FRONT = -14.0
PAVILION_BACK = FOUL_Z - LANE_END - 0.6
WORLD_RADIUS = 38.0

circles = []                    # (x, z, r): what cannot be walked through
boxes = []                      # (x0, z0, x1, z1)


def hub_to_blender(x, y, z):
    return (x, -z, y)


# --- Making things.

materials = {}


def material(name, rgb, tint=False):
    key = (name, tint)
    if key in materials:
        return materials[key]
    m = bpy.data.materials.new(("TINT_" if tint else "") + name)
    m.diffuse_color = (rgb[0], rgb[1], rgb[2], 1.0)
    materials[key] = m
    return m


def color(hexa):
    return tuple(int(hexa[i:i + 2], 16) / 255.0 for i in (0, 2, 4))


def finish(obj, mat, group):
    obj.data.materials.clear()
    obj.data.materials.append(mat)
    obj["group"] = group
    return obj


def box(group, mat, x0, y0, z0, x1, y1, z1):
    """A box between two corners, in the hub's coordinates."""
    bpy.ops.mesh.primitive_cube_add(size=1.0)
    o = bpy.context.active_object
    cx, cy, cz = (x0 + x1) / 2, (y0 + y1) / 2, (z0 + z1) / 2
    o.location = hub_to_blender(cx, cy, cz)
    o.scale = (abs(x1 - x0), abs(z1 - z0), abs(y1 - y0))
    return finish(o, mat, group)


def cylinder(group, mat, x, z, radius, y0, y1, sides=16):
    bpy.ops.mesh.primitive_cylinder_add(vertices=sides, radius=radius, depth=y1 - y0)
    o = bpy.context.active_object
    o.location = hub_to_blender(x, (y0 + y1) / 2, z)
    return finish(o, mat, group)


def cone(group, mat, x, z, r0, r1, y0, y1, sides=12):
    bpy.ops.mesh.primitive_cone_add(vertices=sides, radius1=r0, radius2=r1, depth=y1 - y0)
    o = bpy.context.active_object
    o.location = hub_to_blender(x, (y0 + y1) / 2, z)
    return finish(o, mat, group)


def sphere(group, mat, x, y, z, radius, scale=(1, 1, 1), subdivisions=2):
    bpy.ops.mesh.primitive_ico_sphere_add(subdivisions=subdivisions, radius=radius)
    o = bpy.context.active_object
    o.location = hub_to_blender(x, y, z)
    o.scale = (scale[0], scale[2], scale[1])
    return finish(o, mat, group)


def uv_sphere(group, mat, x, y, z, radius, scale=(1, 1, 1), segments=16, rings=10):
    bpy.ops.mesh.primitive_uv_sphere_add(segments=segments, ring_count=rings, radius=radius)
    o = bpy.context.active_object
    o.location = hub_to_blender(x, y, z)
    o.scale = (scale[0], scale[2], scale[1])
    return finish(o, mat, group)


def lathe(group, profile, mats_by_height, sides=16):
    """A shape turned around the vertical axis from a profile [(radius, height), ...], colored by height bands
    [(height_from, height_to, material), ...] (the first band that holds a face's middle)."""
    mesh = bpy.data.meshes.new(group)
    bm = bmesh.new()
    rings = []
    for (r, h) in profile:
        ring = []
        for i in range(sides):
            a = 2 * math.pi * i / sides
            ring.append(bm.verts.new((r * math.cos(a), r * math.sin(a), h)))
        rings.append(ring)
    for k in range(len(rings) - 1):
        for i in range(sides):
            j = (i + 1) % sides
            bm.faces.new((rings[k][i], rings[k][j], rings[k + 1][j], rings[k + 1][i]))
    bmesh.ops.remove_doubles(bm, verts=bm.verts, dist=1e-5)
    bm.to_mesh(mesh)
    bm.free()
    o = bpy.data.objects.new(group, mesh)
    bpy.context.collection.objects.link(o)
    o["group"] = group
    for _, _, m in mats_by_height:
        if m.name not in [x.name for x in mesh.materials]:
            mesh.materials.append(m)
    names = [x.name for x in mesh.materials]
    for poly in mesh.polygons:
        z = poly.center.z
        for lo, hi, m in mats_by_height:
            if lo <= z < hi:
                poly.material_index = names.index(m.name)
                break
    return o


# --- The models.

def make_plaza():
    g = "plaza"
    grass = material("grass", color("7cb35a"))
    grass_dark = material("grass_dark", color("6aa04c"))
    stone = material("stone", color("d9cfbf"))
    stone_dark = material("stone_dark", color("b8ab97"))
    water = material("water", color("5fb4e0"))
    wood = material("wood", color("c99a5f"))
    lane = material("lane", color("e8c48a"))
    gutter = material("gutter", color("5b6270"))
    wall = material("wall", color("eae2d6"))
    wall_trim = material("wall_trim", color("e05a47"))
    dark = material("dark", color("39404d"))
    trunk = material("trunk", color("7a5236"))
    leaves = [material("leaves%d" % i, color(c)) for i, c in enumerate(("4f9a4a", "5aa857", "3f8c45", "6bb35e"))]
    metal = material("metal", color("3b3f47"))
    light = material("light", color("ffe29a"))
    red = material("red", color("e0413a"))
    white = material("white", color("f4f1ea"))
    bench = material("bench", color("a8744a"))

    # The ground: grass, darker at the edge, and the stone plaza in the middle with a ring.
    cylinder(g, grass_dark, 0, 0, WORLD_RADIUS + 6, -0.3, 0.0, sides=48)
    cylinder(g, grass, 0, 0, WORLD_RADIUS - 2, -0.25, 0.005, sides=48)
    cylinder(g, stone_dark, 0, 0, 12.6, -0.2, 0.02, sides=40)
    cylinder(g, stone, 0, 0, 12.0, -0.2, 0.03, sides=40)

    # The fountain: a low basin with a round rim, full of water.
    cylinder(g, stone_dark, 0, 0, FOUNTAIN_RADIUS - 0.05, 0.0, 0.25, sides=24)
    cylinder(g, water, 0, 0, FOUNTAIN_RADIUS - 0.15, 0.0, 0.4, sides=24)
    bpy.ops.mesh.primitive_torus_add(major_radius=FOUNTAIN_RADIUS - 0.13, minor_radius=0.16, major_segments=28,
                                     minor_segments=8)
    rim = bpy.context.active_object
    rim.location = hub_to_blender(0, 0.42, 0)
    finish(rim, stone, g)
    cylinder(g, stone, 0, 0, 0.35, 0.0, 1.5, sides=12)
    cone(g, stone_dark, 0, 0, 0.3, 0.95, 1.4, 1.75, sides=16)
    cylinder(g, water, 0, 0, 0.82, 1.6, 1.72, sides=16)
    sphere(g, water, 0, 1.95, 0, 0.22, subdivisions=1)
    circles.append((0.0, 0.0, FOUNTAIN_RADIUS + 0.05))

    # The path to the bowling, and the lamps along the plaza.
    box(g, stone, -1.6, 0.0, PAVILION_FRONT, 1.6, 0.035, -11.5)
    for i in range(8):
        a = 2 * math.pi * (i + 0.5) / 8
        x, z = 10.8 * math.cos(a), 10.8 * math.sin(a)
        cylinder(g, metal, x, z, 0.09, 0.0, 3.0, sides=8)
        cylinder(g, metal, x, z, 0.2, 0.0, 0.25, sides=8)
        sphere(g, light, x, 3.15, z, 0.22, subdivisions=1)
        circles.append((x, z, 0.25))

    # Benches around the fountain.
    for i in range(4):
        a = 2 * math.pi * i / 4 + math.pi / 4
        x, z = 5.2 * math.cos(a), 5.2 * math.sin(a)
        # Along the tangent: a box rotated, made by hand as a long thin box between two points.
        tx, tz = -math.sin(a), math.cos(a)
        # (Blender's y is the hub's -z: the angle of the tangent there.)
        turn = math.atan2(-tz, tx)
        seat = box(g, bench, -0.9, 0.42, -0.25, 0.9, 0.5, 0.25)
        seat.rotation_euler = (0, 0, turn)
        seat.location = hub_to_blender(x, 0.46, z)
        for s in (-0.7, 0.7):
            leg = box(g, metal, -0.06, 0.0, -0.22, 0.06, 0.42, 0.22)
            leg.rotation_euler = (0, 0, turn)
            leg.location = hub_to_blender(x + tx * s, 0.21, z + tz * s)
        circles.append((x, z, 0.75))

    # Trees, around, not on the path nor the pavilion.
    rng = random.Random(7)
    placed = 0
    while placed < 34:
        a = rng.uniform(0, 2 * math.pi)
        r = rng.uniform(15.0, WORLD_RADIUS - 3)
        x, z = r * math.cos(a), r * math.sin(a)
        if abs(x) < PAVILION_X + 2.5 and z < PAVILION_FRONT + 3:
            continue
        if any((x - cx) ** 2 + (z - cz) ** 2 < (cr + 2.2) ** 2 for cx, cz, cr in circles):
            continue
        height = rng.uniform(1.2, 2.0)
        cylinder(g, trunk, x, z, 0.18, 0.0, height, sides=7)
        size = rng.uniform(1.0, 1.6)
        sphere(g, rng.choice(leaves), x, height + size * 0.7, z, size, scale=(1, 1.15, 1), subdivisions=1)
        if rng.random() < 0.5:
            sphere(g, rng.choice(leaves), x + size * 0.5, height + size * 0.3, z, size * 0.7, subdivisions=1)
        circles.append((x, z, 0.4))
        placed += 1

    # The bowling pavilion: a wooden floor, two lanes with their gutters, the foul line, walls, the pin deck's hood.
    back, front = PAVILION_BACK, PAVILION_FRONT
    box(g, wood, -PAVILION_X, 0.0, back, PAVILION_X, 0.05, front)
    for lx in LANE_X:
        z0, z1 = FOUL_Z - LANE_END, FOUL_Z
        box(g, lane, lx - LANE_HALF, 0.0, z0, lx + LANE_HALF, 0.07, z1)
        for side in (-1, 1):
            gx = lx + side * (LANE_HALF + GUTTER / 2)
            box(g, gutter, gx - GUTTER / 2, 0.0, z0, gx + GUTTER / 2, 0.035, z1)
        box(g, red, lx - LANE_HALF - GUTTER, 0.0, FOUL_Z - 0.02, lx + LANE_HALF + GUTTER, 0.075, FOUL_Z + 0.02)
        # The arrows on the lane.
        for k in range(-3, 4):
            ax = lx + k * 0.13
            ad = 4.6 + abs(k) * 0.3
            box(g, dark, ax - 0.015, 0.0, FOUL_Z - ad - 0.12, ax + 0.015, 0.072, FOUL_Z - ad)
        # The approach dots.
        for k in (-2, 0, 2):
            box(g, dark, lx + k * 0.15 - 0.02, 0.0, FOUL_Z + 1.5 - 0.02, lx + k * 0.15 + 0.02, 0.052, FOUL_Z + 1.5 + 0.02)
    # Between the lanes, and on their sides.
    box(g, dark, LANE_X[0] + LANE_HALF + GUTTER, 0.0, FOUL_Z - LANE_END, LANE_X[1] - LANE_HALF - GUTTER, 0.12, FOUL_Z)
    for side in (-1, 1):
        x0 = side * PAVILION_X
        x1 = side * (abs(LANE_X[0]) + LANE_HALF + GUTTER)
        box(g, dark, min(x0, x1), 0.0, FOUL_Z - LANE_END, max(x0, x1), 0.12, FOUL_Z)
    # The ball return.
    box(g, dark, -0.35, 0.0, FOUL_Z + 1.2, 0.35, 0.65, FOUL_Z + 3.4)
    box(g, metal, -0.3, 0.65, FOUL_Z + 1.3, 0.3, 0.7, FOUL_Z + 3.3)
    boxes.append((-0.4, FOUL_Z + 1.15, 0.4, FOUL_Z + 3.45))
    # The walls, with a red trim, and the back wall behind the pins.
    for side in (-1, 1):
        x0, x1 = side * PAVILION_X, side * (PAVILION_X + 0.25)
        box(g, wall, min(x0, x1), 0.0, back, max(x0, x1), 1.6, FOUL_Z + 1.0)
        box(g, wall_trim, min(x0, x1), 1.6, back, max(x0, x1), 1.8, FOUL_Z + 1.0)
        boxes.append((min(x0, x1) - 0.05, back, max(x0, x1) + 0.05, FOUL_Z + 1.0))
    box(g, dark, -PAVILION_X - 0.25, 0.0, back - 0.3, PAVILION_X + 0.25, 3.2, back)
    box(g, wall_trim, -PAVILION_X - 0.25, 3.2, back - 0.3, PAVILION_X + 0.25, 3.5, back)
    boxes.append((-PAVILION_X - 0.3, back - 0.35, PAVILION_X + 0.3, back + 0.05))
    # The hood over the pins, hiding the machinery, and the masking board with a pin on it.
    hood_z = FOUL_Z - HEADPIN + 0.6
    box(g, dark, -PAVILION_X, 1.0, back, PAVILION_X, 1.9, hood_z)
    box(g, wall_trim, -PAVILION_X, 1.0, hood_z - 0.05, PAVILION_X, 1.15, hood_z)
    for lx in LANE_X:
        box(g, white, lx - 0.05, 1.3, hood_z - 0.02, lx + 0.05, 1.75, hood_z + 0.01)
        sphere(g, red, lx, 1.62, hood_z + 0.03, 0.06, subdivisions=1)
    # The lanes cannot be walked on, past the foul line.
    boxes.append((-PAVILION_X, back, PAVILION_X, FOUL_Z - 0.05))
    # Posts at the front corners, and a sign: a big pin and ball over the entrance.
    for side in (-1, 1):
        cylinder(g, wall_trim, side * (PAVILION_X + 0.12), FOUL_Z + 1.0, 0.18, 0.0, 2.6, sides=10)
        circles.append((side * (PAVILION_X + 0.12), FOUL_Z + 1.0, 0.25))
    sign = lathe(g, [(0.0, 0.0), (0.18, 0.05), (0.3, 0.45), (0.3, 0.6), (0.15, 0.95), (0.11, 1.1), (0.17, 1.35),
                     (0.15, 1.5), (0.0, 1.55)],
                 [(1.0, 1.15, red), (-1.0, 9.0, white)], sides=12)
    sign.location = hub_to_blender(-1.0, 2.6, PAVILION_FRONT)
    uv_sphere(g, material("sign_ball", color("3a5fd0")), 0.7, 2.95, PAVILION_FRONT, 0.35)
    box(g, wall_trim, -2.2, 2.45, PAVILION_FRONT - 0.15, 2.2, 2.6, PAVILION_FRONT + 0.15)
    for side in (-1, 1):
        cylinder(g, metal, side * 2.1, PAVILION_FRONT, 0.07, 0.0, 2.45, sides=8)
        circles.append((side * 2.1, PAVILION_FRONT, 0.15))


def make_crewmate():
    g = "crewmate"
    body = material("body", (1, 1, 1), tint=True)
    shade = material("body_shade", (0.78, 0.78, 0.78), tint=True)
    visor = material("visor", color("9fd6ef"))
    visor_shine = material("visor_shine", color("e8f7ff"))
    # Facing forward (-z in the hub): the visor in front, the backpack behind.
    uv_sphere(g, body, 0, 0.78, 0, 0.36, scale=(1.0, 1.32, 0.92), segments=18, rings=12)
    for side in (-1, 1):
        cylinder(g, shade, side * 0.16, 0.0, 0.13, 0.0, 0.42, sides=10)
    box(g, shade, -0.24, 0.45, 0.22, 0.24, 1.0, 0.45)
    uv_sphere(g, visor, 0, 0.95, -0.27, 0.2, scale=(1.25, 0.72, 0.6), segments=14, rings=8)
    uv_sphere(g, visor_shine, 0.1, 1.02, -0.37, 0.05, scale=(1.4, 0.6, 0.5), segments=8, rings=5)


def make_pin():
    g = "pin"
    white = material("pin_white", color("f6f3ec"))
    red = material("pin_red", color("d6322b"))
    # A real pin's shape: 38 cm tall, 12 cm at its widest.
    profile = [(0.0, 0.0), (0.026, 0.0), (0.04, 0.02), (0.056, 0.07), (0.0605, 0.11), (0.058, 0.14), (0.048, 0.18),
               (0.032, 0.23), (0.023, 0.26), (0.022, 0.28), (0.028, 0.31), (0.032, 0.335), (0.03, 0.355),
               (0.02, 0.372), (0.0, 0.381)]
    lathe(g, profile, [(0.245, 0.262, red), (0.272, 0.289, red), (-1.0, 9.0, white)], sides=14)


def make_ball():
    g = "ball"
    shell = material("ball", (1, 1, 1), tint=True)
    hole = material("ball_hole", color("1c1f26"))
    uv_sphere(g, shell, 0, 0, 0, 0.108, segments=16, rings=10)
    # Finger holes, as little dark bumps on top.
    for (x, z) in ((-0.025, -0.02), (0.025, -0.02), (0.0, 0.035)):
        sphere(g, hole, x, 0.098, z, 0.017, subdivisions=1)


def make_shadow():
    # A dark disc under each player: its alpha is the code's.
    g = "shadow"
    cylinder(g, material("shadow", (0, 0, 0)), 0, 0, 0.45, 0.0, 0.002, sides=20)


# --- Writing them.

def export(group, path):
    deps = bpy.context.evaluated_depsgraph_get()
    vertices = []
    for obj in [o for o in bpy.context.scene.objects if o.get("group") == group]:
        evaluated = obj.evaluated_get(deps)
        mesh = evaluated.to_mesh()
        mesh.calc_loop_triangles()
        world = obj.matrix_world
        normal_matrix = world.to_3x3().inverted().transposed()
        for tri in mesh.loop_triangles:
            mat = mesh.materials[tri.material_index] if mesh.materials else None
            rgb = mat.diffuse_color if mat else (1, 1, 1, 1)
            tint = 255 if mat and mat.name.startswith("TINT_") else 0
            n = (normal_matrix @ tri.normal).normalized()
            hub_n = (n.x, n.z, -n.y)
            col = tuple(max(0, min(255, round(c * 255))) for c in rgb[:3]) + (tint,)
            for vi in tri.vertices:
                p = world @ mesh.vertices[vi].co
                vertices.append(((p.x, p.z, -p.y), hub_n, col))
        evaluated.to_mesh_clear()
    with open(path, "wb") as f:
        f.write(b"HUBM")
        f.write(struct.pack("<II", len(vertices), len(vertices)))
        for (p, n, c) in vertices:
            f.write(struct.pack("<6f4B", *p, *n, *c))
        f.write(struct.pack("<%dI" % len(vertices), *range(len(vertices))))
    print("%s: %d triangles" % (os.path.basename(path), len(vertices) // 3))


def write_layout(path):
    def f(v):
        return "%.4ff" % v
    lines = [
        "// The hub's layout: made by hub/assets/make_assets.py with the models (do not change it by hand).",
        "#pragma once",
        "",
        "static const float layout_spawn[2] = {%s, %s};" % (f(SPAWN[0]), f(SPAWN[1])),
        "static const float layout_world_radius = %s;" % f(WORLD_RADIUS),
        "static const float layout_lane_x[2] = {%s, %s};" % (f(LANE_X[0]), f(LANE_X[1])),
        "static const float layout_lane_half = %s;" % f(LANE_HALF),
        "static const float layout_gutter = %s;" % f(GUTTER),
        "static const float layout_foul_z = %s;" % f(FOUL_Z),
        "static const float layout_headpin = %s;" % f(HEADPIN),
        "static const float layout_pin_row = %s;" % f(PIN_ROW),
        "static const float layout_pin_spacing = %s;" % f(PIN_SPACING),
        "static const float layout_lane_end = %s;" % f(LANE_END),
        "",
        "// What cannot be walked through: circles (x, z, radius) and boxes (x0, z0, x1, z1).",
        "static const float layout_circles[][3] = {",
    ]
    lines += ["    {%s, %s, %s}," % (f(x), f(z), f(r)) for x, z, r in circles]
    lines += ["};", "static const float layout_boxes[][4] = {"]
    lines += ["    {%s, %s, %s, %s}," % (f(x0), f(z0), f(x1), f(z1)) for x0, z0, x1, z1 in boxes]
    lines += ["};", ""]
    with open(path, "w", newline="\n") as out:
        out.write("\n".join(lines))


def main():
    bpy.ops.wm.read_factory_settings(use_empty=True)
    make_plaza()
    make_crewmate()
    make_pin()
    make_ball()
    make_shadow()
    for group in ("plaza", "crewmate", "pin", "ball", "shadow"):
        export(group, os.path.join(HERE, group + ".mesh"))
    write_layout(os.path.join(HERE, "layout.h"))


main()
