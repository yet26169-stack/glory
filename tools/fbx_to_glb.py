"""
Blender headless script: FBX → GLB with full PBR material, weight, and texture cleanup.
Fixes:
  1. Vertex explosion (limit bone influences to 4, normalize weights)
  2. Full PBR texture linking (Base Color, Normal, Metallic, Roughness via filename keywords)
  3. Coordinate/scale (keep cm units via global_scale=100)
  4. Oversized textures (downscale preserving aspect ratio, max 2048px on longest edge)
  5. GLB transparency (blend_method=BLEND for 4-channel textures)
  6. Headless context safety (deselect-all before weight paint, try/except recovery)

Usage:
  blender --background --python fbx_to_glb.py -- input.fbx output.glb
"""

import bpy
import sys

argv = sys.argv
args = argv[argv.index("--") + 1:]
input_fbx = args[0]
output_glb = args[1]

# 1. Clear default scene
bpy.ops.wm.read_factory_settings(use_empty=True)

# 2. Import FBX — counteract cm→m conversion to keep original centimeter units
bpy.ops.import_scene.fbx(
    filepath=input_fbx,
    global_scale=100.0,
    automatic_bone_orientation=True,
    force_connect_children=False,
    primary_bone_axis='Y',
    secondary_bone_axis='X',
    use_anim=True,
    anim_offset=1.0,
    ignore_leaf_bones=False,
)

# 3. Pack all external images into the blend data (safely — missing files won't abort)
try:
    bpy.ops.file.pack_all()
except Exception as e:
    print(f"  Warning during pack_all: {e}")

# 4. Material cleanup — full PBR linking via filename keyword detection
for mat in bpy.data.materials:
    if not mat.use_nodes:
        mat.use_nodes = True

    tree = mat.node_tree
    nodes = tree.nodes
    links = tree.links

    # Find or create Principled BSDF
    bsdf = None
    for node in nodes:
        if node.type == 'BSDF_PRINCIPLED':
            bsdf = node
            break
    if not bsdf:
        bsdf = nodes.new('ShaderNodeBsdfPrincipled')
        bsdf.location = (0, 0)

    # Find or create Material Output
    output = None
    for node in nodes:
        if node.type == 'OUTPUT_MATERIAL':
            output = node
            break
    if not output:
        output = nodes.new('ShaderNodeOutputMaterial')
        output.location = (300, 0)

    # Ensure Principled BSDF → Material Output
    if not any(link.to_node == output for link in bsdf.outputs['BSDF'].links):
        links.new(bsdf.outputs['BSDF'], output.inputs['Surface'])

    # Collect all image texture nodes
    tex_nodes = [n for n in nodes if n.type == 'TEX_IMAGE' and n.image]
    if not tex_nodes:
        print(f"  Material '{mat.name}': no textures found, skipping")
        continue

    base_color_node = None

    for tex_node in tex_nodes:
        img_name = tex_node.image.name.lower()

        if any(kw in img_name for kw in ('norm', 'nrm', 'bump')):
            # Normal map: Non-Color space + NormalMap conversion node
            try:
                tex_node.image.colorspace_settings.name = 'Non-Color'
            except Exception:
                pass
            norm_map = nodes.new('ShaderNodeNormalMap')
            norm_map.location = (bsdf.location[0] - 200, bsdf.location[1] - 200)
            for link in list(bsdf.inputs['Normal'].links):
                links.remove(link)
            links.new(tex_node.outputs['Color'], norm_map.inputs['Color'])
            links.new(norm_map.outputs['Normal'], bsdf.inputs['Normal'])
            print(f"  Material '{mat.name}': linked Normal map '{img_name}'")

        elif any(kw in img_name for kw in ('metal', 'mtl')):
            # Metallic map: Non-Color space
            try:
                tex_node.image.colorspace_settings.name = 'Non-Color'
            except Exception:
                pass
            for link in list(bsdf.inputs['Metallic'].links):
                links.remove(link)
            links.new(tex_node.outputs['Color'], bsdf.inputs['Metallic'])
            print(f"  Material '{mat.name}': linked Metallic map '{img_name}'")

        elif any(kw in img_name for kw in ('rough', 'rgh')):
            # Roughness map: Non-Color space
            try:
                tex_node.image.colorspace_settings.name = 'Non-Color'
            except Exception:
                pass
            for link in list(bsdf.inputs['Roughness'].links):
                links.remove(link)
            links.new(tex_node.outputs['Color'], bsdf.inputs['Roughness'])
            print(f"  Material '{mat.name}': linked Roughness map '{img_name}'")

        else:
            # Fallback: treat as Base Color
            base_color_node = tex_node

    # Link Base Color (and Alpha if 4-channel)
    if base_color_node:
        for link in list(bsdf.inputs['Base Color'].links):
            links.remove(link)
        links.new(base_color_node.outputs['Color'], bsdf.inputs['Base Color'])
        print(f"  Material '{mat.name}': linked Base Color '{base_color_node.image.name}'")

        if base_color_node.image.channels == 4:
            for link in list(bsdf.inputs['Alpha'].links):
                links.remove(link)
            links.new(base_color_node.outputs['Alpha'], bsdf.inputs['Alpha'])
            mat.blend_method = 'BLEND'
            try:
                mat.shadow_method = 'HASHED'
            except AttributeError:
                pass  # Removed in Blender 5+

# 5. Downscale oversized textures — preserve aspect ratio (longest edge ≤ 2048)
MAX_TEX_SIZE = 2048
for img in bpy.data.images:
    width, height = img.size[0], img.size[1]
    if width > MAX_TEX_SIZE or height > MAX_TEX_SIZE:
        scale_factor = MAX_TEX_SIZE / max(width, height)
        new_width = max(1, int(width * scale_factor))
        new_height = max(1, int(height * scale_factor))
        print(f"  Resizing '{img.name}' from {width}x{height} to {new_width}x{new_height}")
        img.scale(new_width, new_height)

# 6. Limit bone influences to 4 and normalize weights (context-safe headless version)
for obj in bpy.data.objects:
    if obj.type == 'MESH' and obj.vertex_groups:
        bpy.ops.object.select_all(action='DESELECT')
        bpy.context.view_layer.objects.active = obj
        obj.select_set(True)
        try:
            bpy.ops.object.mode_set(mode='WEIGHT_PAINT')
            bpy.ops.object.vertex_group_limit_total(group_select_mode='ALL', limit=4)
            bpy.ops.object.vertex_group_normalize_all(group_select_mode='ALL', lock_active=False)
            bpy.ops.object.mode_set(mode='OBJECT')
        except Exception as e:
            print(f"  Failed to clean weights for '{obj.name}': {e}")
            bpy.ops.object.mode_set(mode='OBJECT')

# 7. Export as GLB — preserve node transforms (armature rotation etc.)
# The glTF exporter automatically combines Metallic + Roughness into the ORM channel.
bpy.ops.export_scene.gltf(
    filepath=output_glb,
    export_format='GLB',
    export_apply=False,
    export_animations=True,
    export_skins=True,
    export_materials='EXPORT',
    export_image_format='AUTO',
    export_texcoords=True,
    export_normals=True,
)

print(f"SUCCESS: Exported {output_glb}")
