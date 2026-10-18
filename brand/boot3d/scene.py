"""afterglow boot animation: a sun setting into a calm sea, rings of light rising around it.

Builds the whole scene in code and renders it, mono for previews or stereo (one camera per eye,
63 mm apart, parallel) for the headset. Timeline: a 3 s intro, then a 2.5 s loop the headset repeats
until boot finishes; every looping property has a 2.5 s period so frame LOOP_END wraps to LOOP_START.

Usage:
  blender -b --factory-startup -P brand/boot3d/scene.py -- --out out/boot3d/preview [--res 600]
          [--stereo] [--samples 32] [--frames 1-198] [--still 120]
"""
import argparse
import math
import os
import sys

import bpy

FPS = 36                 # shown twice per refresh on the 72 Hz panels
INTRO = 3.0              # seconds
PERIOD = 2.5             # seconds per loop
LOOP_START = round(INTRO * FPS) + 1
LOOP_END = round((INTRO + PERIOD) * FPS)
HERE = os.path.dirname(os.path.abspath(__file__))
FONT = os.path.join(HERE, "..", "fonts", "Montserrat-Light-merged.ttf")

AMBER, CORAL, VIOLET = (1.0, 0.60, 0.35), (1.0, 0.36, 0.54), (0.72, 0.36, 1.0)
CREAM, TEXT = (1.0, 0.90, 0.80), (0.96, 0.90, 0.85)


def ease(x):
    """Smoothstep-style ease in/out, clamped to 0..1."""
    x = min(max(x, 0.0), 1.0)
    return x * x * (3 - 2 * x)


def ease_out(x):
    x = min(max(x, 0.0), 1.0)
    return 1 - (1 - x) ** 3


def phase(t, delay=0.0):
    """0..1..0 wave with the loop period, delayed per ring so the glow travels outward."""
    return 0.5 - 0.5 * math.cos(2 * math.pi * (t - delay) / PERIOD)


def emission_material(name, color, strength):
    mat = bpy.data.materials.new(name)
    mat.use_nodes = True
    nodes = mat.node_tree.nodes
    nodes.clear()
    emit = nodes.new("ShaderNodeEmission")
    emit.inputs["Color"].default_value = (*color, 1)
    emit.inputs["Strength"].default_value = strength
    out = nodes.new("ShaderNodeOutputMaterial")
    mat.node_tree.links.new(emit.outputs[0], out.inputs["Surface"])
    return mat, emit


def sun_material():
    """Emission graded from gold at the top to deep orange at the waterline."""
    mat = bpy.data.materials.new("Sun")
    mat.use_nodes = True
    nt = mat.node_tree
    nt.nodes.clear()
    coord = nt.nodes.new("ShaderNodeTexCoord")
    sep = nt.nodes.new("ShaderNodeSeparateXYZ")
    ramp = nt.nodes.new("ShaderNodeValToRGB")
    ramp.color_ramp.elements[0].position, ramp.color_ramp.elements[0].color = 0.45, (1.0, 0.42, 0.25, 1)
    ramp.color_ramp.elements[1].position, ramp.color_ramp.elements[1].color = 1.0, (1.0, 0.86, 0.48, 1)
    emit = nt.nodes.new("ShaderNodeEmission")
    out = nt.nodes.new("ShaderNodeOutputMaterial")
    nt.links.new(coord.outputs["Generated"], sep.inputs[0])
    nt.links.new(sep.outputs["Z"], ramp.inputs["Fac"])
    nt.links.new(ramp.outputs["Color"], emit.inputs["Color"])
    nt.links.new(emit.outputs[0], out.inputs["Surface"])
    return mat, emit


def water_material():
    """Dark, weakly reflective water with a slow swell that breaks reflections into streaks."""
    mat = bpy.data.materials.new("Water")
    mat.use_nodes = True
    nt = mat.node_tree
    bsdf = nt.nodes["Principled BSDF"]
    bsdf.inputs["Base Color"].default_value = (0.0, 0.0, 0.0, 1)
    bsdf.inputs["Roughness"].default_value = 0.1
    bsdf.inputs["IOR"].default_value = 1.33
    bsdf.inputs["Specular IOR Level"].default_value = 0.5
    # World-space position, so the swell has a fixed real size (~15 cm) however big the plane is
    geometry = nt.nodes.new("ShaderNodeNewGeometry")
    wave = nt.nodes.new("ShaderNodeTexWave")
    wave.wave_type, wave.bands_direction = "BANDS", "Y"
    wave.inputs["Scale"].default_value = 7.0
    wave.inputs["Distortion"].default_value = 3.0
    wave.inputs["Detail"].default_value = 2.0
    bump = nt.nodes.new("ShaderNodeBump")
    bump.inputs["Strength"].default_value = 0.35
    nt.links.new(geometry.outputs["Position"], wave.inputs["Vector"])
    nt.links.new(wave.outputs["Fac"], bump.inputs["Height"])
    nt.links.new(bump.outputs["Normal"], bsdf.inputs["Normal"])
    # Reflective toward the horizon (y > 0), fading to none near the viewer (y < -2.5): keeps
    # the glitter path under the sun and drops stray streaks at the bottom of the frame
    sep = nt.nodes.new("ShaderNodeSeparateXYZ")
    fade = nt.nodes.new("ShaderNodeMapRange")
    fade.inputs["From Min"].default_value, fade.inputs["From Max"].default_value = -2.5, 0.0
    fade.inputs["To Min"].default_value, fade.inputs["To Max"].default_value = 0.0, 0.5
    nt.links.new(geometry.outputs["Position"], sep.inputs[0])
    nt.links.new(sep.outputs["Y"], fade.inputs["Value"])
    nt.links.new(fade.outputs["Result"], bsdf.inputs["Specular IOR Level"])
    return mat, wave


def key(socket_or_obj, path, frame, value, index=-1):
    if index >= 0:
        getattr(socket_or_obj, path)[index] = value
    else:
        setattr(socket_or_obj, path, value)
    socket_or_obj.keyframe_insert(path, index=index, frame=frame)


def build(args):
    scene = bpy.context.scene
    for obj in list(bpy.data.objects):
        bpy.data.objects.remove(obj)

    scene.render.fps = FPS
    scene.frame_start, scene.frame_end = 1, LOOP_END
    scene.render.engine = "BLENDER_EEVEE_NEXT"
    scene.eevee.taa_render_samples = args.samples
    scene.eevee.use_raytracing = True
    scene.render.resolution_x = scene.render.resolution_y = args.res
    scene.render.film_transparent = False
    scene.view_settings.view_transform = "Standard"
    scene.world = bpy.data.worlds.new("Night")
    scene.world.use_nodes = True
    scene.world.node_tree.nodes["Background"].inputs["Strength"].default_value = 0.0

    # Water: a plane at z = 0 running from behind the camera to far beyond the sun, so everything
    # below the waterline is hidden (the sun and rings show only their upper halves and reflections)
    bpy.ops.mesh.primitive_plane_add(size=1, location=(0, 15.0, 0))
    water = bpy.context.object
    water.scale = (60, 50, 1)
    water_mat, wave = water_material()
    water.data.materials.append(water_mat)   # ray-traced reflections follow the swell; a planar probe wouldn't

    sun_mat, sun_emit = sun_material()
    bpy.ops.mesh.primitive_uv_sphere_add(radius=0.55, segments=96, ring_count=48)
    sun = bpy.context.object
    sun.data.materials.append(sun_mat)
    bpy.ops.object.shade_smooth()

    rings = []
    for i, (radius, color) in enumerate(((0.85, AMBER), (1.15, CORAL), (1.45, VIOLET))):
        bpy.ops.mesh.primitive_torus_add(major_radius=radius, minor_radius=0.035,
                                         major_segments=192, minor_segments=24, location=(0, 0.25 * i, 0))
        ring = bpy.context.object
        mat, emit = emission_material(f"Ring{i}", color, 1.0)
        ring.data.materials.append(mat)
        bpy.ops.object.shade_smooth()
        rings.append((ring, emit))

    bpy.ops.mesh.primitive_cylinder_add(radius=0.012, depth=4.2, location=(0, 0, 0.012),
                                        rotation=(0, math.pi / 2, 0))
    horizon = bpy.context.object
    hmat, h_emit = emission_material("Horizon", CREAM, 1.0)
    horizon.data.materials.append(hmat)

    # The caption stands on the water, nearer the camera so it sits below the sun in the picture,
    # leaning back to face the camera so the tops of its extruded edges don't show
    bpy.ops.object.text_add(location=(0, -3.0, 0.0), rotation=(math.radians(68), 0, 0))
    text = bpy.context.object
    text.data.body = "afterglow"
    text.data.font = bpy.data.fonts.load(FONT)
    text.data.size = 0.25
    text.data.extrude = 0.006   # no bevel: it self-intersects in Montserrat's sharp joins (the "w")
    text.data.align_x, text.data.align_y = "CENTER", "BOTTOM"
    text.data.space_character = 1.12
    tmat, t_emit = emission_material("Caption", TEXT, 0.0)
    text.data.materials.append(tmat)

    bpy.ops.object.camera_add()
    cam = bpy.context.object
    cam.data.lens_unit = "FOV"
    cam.data.angle = math.radians(args.fov)
    scene.camera = cam
    target = bpy.data.objects.new("Target", None)
    scene.collection.objects.link(target)
    track = cam.constraints.new("TRACK_TO")
    track.target, track.track_axis, track.up_axis = target, "TRACK_NEGATIVE_Z", "UP_Y"

    if args.stereo:
        scene.render.use_multiview = True
        scene.render.views_format = "STEREO_3D"
        cam.data.stereo.convergence_mode = "PARALLEL"
        cam.data.stereo.interocular_distance = 0.063
        scene.render.image_settings.views_format = "INDIVIDUAL"

    # One keyframe per frame from the closed-form timeline below
    for frame in range(1, LOOP_END + 2):
        t = (frame - 1) / FPS
        k = ease_out(t / 2.6)
        key(cam, "location", frame, -7.4 + 1.4 * k, index=1)
        key(cam, "location", frame, 1.9 - 0.45 * k, index=2)
        key(target, "location", frame, 0.35, index=2)

        key(horizon, "scale", frame, max(ease_out(t / 0.9), 0.001), index=2)
        rise = ease_out((t - 0.4) / 1.2)
        key(sun, "location", frame, -0.6 + 0.6 * rise, index=2)
        key(sun_emit.inputs["Strength"], "default_value", frame, 1.15 * rise + 0.1 * phase(t, 0.0) * (t >= INTRO))

        for i, (ring, emit) in enumerate(rings):
            grow = ease((t - 1.0 - 0.22 * i) / 1.0)
            key(ring, "rotation_euler", frame, (math.pi / 2) * grow, index=0)
            key(ring, "scale", frame, 0.55 + 0.45 * grow, index=0)
            key(ring, "scale", frame, 0.55 + 0.45 * grow, index=1)
            key(ring, "scale", frame, 0.55 + 0.45 * grow, index=2)
            pulse = 1.0 - 0.55 * phase(t, 0.3 * i) if t >= INTRO else 1.0
            key(emit.inputs["Strength"], "default_value", frame, 1.0 * grow * pulse)

        cap = ease((t - 2.0) / 1.0)
        key(text, "location", frame, -0.3 + 0.32 * cap, index=2)   # rises out of the water
        key(t_emit.inputs["Strength"], "default_value", frame, 0.9 * cap)
        key(wave.inputs["Phase Offset"], "default_value", frame, 2 * math.pi * t / PERIOD)

    # Bloom
    scene.use_nodes = True
    tree = scene.node_tree
    tree.nodes.clear()
    layers = tree.nodes.new("CompositorNodeRLayers")
    glare = tree.nodes.new("CompositorNodeGlare")
    try:
        glare.glare_type = "BLOOM"
    except TypeError:
        glare.glare_type = "FOG_GLOW"
    glare.quality = "HIGH"
    glare.threshold = 0.85
    glare.size = 6
    glare.mix = -0.35   # keep most of the crisp image; add a soft glow
    composite = tree.nodes.new("CompositorNodeComposite")
    tree.links.new(layers.outputs["Image"], glare.inputs["Image"])
    tree.links.new(glare.outputs["Image"], composite.inputs["Image"])


def main():
    argv = sys.argv[sys.argv.index("--") + 1:] if "--" in sys.argv else []
    p = argparse.ArgumentParser()
    p.add_argument("--out", required=True)
    p.add_argument("--res", type=int, default=600)
    p.add_argument("--fov", type=float, default=59.0, help="camera field of view in degrees")
    p.add_argument("--samples", type=int, default=32)
    p.add_argument("--stereo", action="store_true")
    p.add_argument("--frames", default=None, help="e.g. 1-198")
    p.add_argument("--step", type=int, default=1, help="render every Nth frame (quick checks)")
    p.add_argument("--still", type=int, default=None, help="render a single frame")
    p.add_argument("--debug-water", action="store_true", help="paint the water dark red to see where it is")
    args = p.parse_args(argv)

    build(args)
    scene = bpy.context.scene
    if args.debug_water:
        bsdf = bpy.data.materials["Water"].node_tree.nodes["Principled BSDF"]
        bsdf.inputs["Base Color"].default_value = (0.25, 0.0, 0.0, 1)
        bsdf.inputs["Emission Color"].default_value = (0.25, 0.0, 0.0, 1)
        bsdf.inputs["Emission Strength"].default_value = 1.0
        water = next(o for o in scene.objects if o.data and "Water" in [m.name for m in getattr(o.data, "materials", [])])
        print("WATER", water.name, tuple(water.location), tuple(water.scale), tuple(water.dimensions))
    os.makedirs(args.out, exist_ok=True)
    scene.render.image_settings.file_format = "PNG"
    scene.render.image_settings.color_mode = "RGB"
    if args.still is not None:
        scene.frame_set(args.still)
        scene.render.filepath = os.path.join(args.out, f"still_{args.still:04d}")
        bpy.ops.render.render(write_still=True)
        return
    if args.frames:
        scene.frame_start, scene.frame_end = map(int, args.frames.split("-"))
    scene.frame_step = args.step
    scene.render.filepath = os.path.join(args.out, "frame_")
    bpy.ops.render.render(animation=True)
    print(f"rendered frames {scene.frame_start}-{scene.frame_end}; loop is {LOOP_START}-{LOOP_END}")


main()
