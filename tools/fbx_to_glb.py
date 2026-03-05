"""
Blender headless script: FBX → GLB with full material, weight, and texture cleanup.
Fixes:
  1. Vertex explosion (limit bone influences to 4, normalize weights)
  2. Missing textures (ensure Principled BSDF with Base Color texture link)
  3. Coordinate/scale (keep cm units via global_scale=100)
  4. Oversized textures (downscale to 2048×2048 max)

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

# 3. Pack all external images into the blend data
bpy.ops.file.pack_all()

# 4. Material cleanup — ensure Principled BSDF with texture linked to Base Color
for mat in bpy.data.materials:
    if not mat.use_nodes:
        mat.use_nodes = True

    tree = mat.node_tree
    nodes = tree.nodes
    links = tree.links

    # Find existing image texture node (if any)
    tex_node = None
    tex_image = None
    for node in nodes:
        if node.type == 'TEX_IMAGE' and node.image:
            tex_node = node
            tex_image = node.image
            break

    if not tex_image:
        # Also search for textures in Specular/Diffuse nodes
        for node in nodes:
            if hasattr(node, 'image') and node.image:
                tex_image = node.image
                break

    if not tex_image:
        print(f"  Material '{mat.name}': no texture found, skipping")
        continue

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
    if not bsdf.outputs['BSDF'].links:
        links.new(bsdf.outputs['BSDF'], output.inputs['Surface'])
    else:
        # Check if already connected to output
        connected_to_output = False
        for link in bsdf.outputs['BSDF'].links:
            if link.to_node == output:
                connected_to_output = True
        if not connected_to_output:
            links.new(bsdf.outputs['BSDF'], output.inputs['Surface'])

    # Ensure image texture → Base Color input of Principled BSDF
    if not tex_node:
        tex_node = nodes.new('ShaderNodeTexImage')
        tex_node.location = (-400, 0)
    tex_node.image = tex_image

    # Clear existing Base Color links
    for link in list(bsdf.inputs['Base Color'].links):
        links.remove(link)

    # Connect texture to Base Color
    links.new(tex_node.outputs['Color'], bsdf.inputs['Base Color'])

    # Also connect alpha if texture has alpha
    if tex_image.channels == 4:
        for link in list(bsdf.inputs['Alpha'].links):
            links.remove(link)
        links.new(tex_node.outputs['Alpha'], bsdf.inputs['Alpha'])

    print(f"  Material '{mat.name}': linked texture '{tex_image.name}' to Principled BSDF Base Color")

# 5. Downscale oversized textures to 2048×2048 max
MAX_TEX_SIZE = 2048
for img in bpy.data.images:
    if img.size[0] > MAX_TEX_SIZE or img.size[1] > MAX_TEX_SIZE:
        print(f"  Resizing texture '{img.name}' from {img.size[0]}x{img.size[1]} to {MAX_TEX_SIZE}x{MAX_TEX_SIZE}")
        img.scale(MAX_TEX_SIZE, MAX_TEX_SIZE)

# 6. Limit bone influences to 4 and normalize weights
for obj in bpy.data.objects:
    if obj.type == 'MESH' and obj.vertex_groups:
        bpy.context.view_layer.objects.active = obj
        obj.select_set(True)
        bpy.ops.object.mode_set(mode='WEIGHT_PAINT')
        bpy.ops.object.vertex_group_limit_total(group_select_mode='ALL', limit=4)
        bpy.ops.object.vertex_group_normalize_all(group_select_mode='ALL', lock_active=False)
        bpy.ops.object.mode_set(mode='OBJECT')
        obj.select_set(False)

# 7. Export as GLB — preserve node transforms (armature rotation etc.)
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
