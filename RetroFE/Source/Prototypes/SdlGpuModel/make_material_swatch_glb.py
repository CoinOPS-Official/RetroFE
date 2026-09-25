"""Generate a small metal/roughness sweep for visual PBR regression checks.

Columns: metallic 0, 1/3, 2/3, 1. Rows, top down: roughness .09,
.25, .45, .70, 1.00. All spheres share the same glTF base color.
"""

import json
import math
import struct
import sys
import zlib
from pathlib import Path


def pack_accessor(payload, values, fmt, type_name, target):
    while len(payload) % 4:
        payload.append(0)
    offset = len(payload)
    flattened = [component for value in values for component in
                 (value if isinstance(value, tuple) else (value,))]
    data = struct.pack("<" + fmt * len(flattened), *flattened)
    payload.extend(data)
    view = {"buffer": 0, "byteOffset": offset, "byteLength": len(data),
            "target": target}
    accessor = {"bufferView": None, "componentType": 5125 if fmt == "I" else 5126,
                "count": len(values), "type": type_name}
    return view, accessor


longitude, latitude = 32, 20
positions, normals, uvs, indices = [], [], [], []
for row in range(latitude + 1):
    theta = math.pi * row / latitude
    for column in range(longitude + 1):
        phi = 2 * math.pi * column / longitude
        normal = (math.sin(theta) * math.cos(phi),
                  math.cos(theta),
                  math.sin(theta) * math.sin(phi))
        normals.append(normal)
        positions.append(tuple(0.17 * component for component in normal))
        uvs.append((column / longitude, row / latitude))
for row in range(latitude):
    for column in range(longitude):
        a = row * (longitude + 1) + column
        b = a + longitude + 1
        indices.extend((a, b, a + 1, a + 1, b, b + 1))

binary = bytearray()
views, accessors = [], []
for values, fmt, shape, target in (
        (positions, "f", "VEC3", 34962),
        (normals, "f", "VEC3", 34962),
        (uvs, "f", "VEC2", 34962),
        (indices, "I", "SCALAR", 34963)):
    view, accessor = pack_accessor(binary, values, fmt, shape, target)
    accessor["bufferView"] = len(views)
    if values is positions:
        accessor["min"] = [min(p[i] for p in positions) for i in range(3)]
        accessor["max"] = [max(p[i] for p in positions) for i in range(3)]
    views.append(view)
    accessors.append(accessor)

metallic_values = (0, 1 / 3, 2 / 3, 1)
roughness_values = (0.09, 0.25, 0.45, 0.70, 1)
materials, meshes, nodes = [], [], []
for row, roughness in enumerate(roughness_values):
    for column, metallic in enumerate(metallic_values):
        material_index = len(materials)
        materials.append({
            "name": f"M{metallic:.2f}_R{roughness:.2f}",
            "pbrMetallicRoughness": {
                "baseColorFactor": [0.78, 0.58, 0.30, 1],
                "metallicFactor": metallic,
                "roughnessFactor": roughness,
            },
        })
        meshes.append({"primitives": [{
            "attributes": {"POSITION": 0, "NORMAL": 1, "TEXCOORD_0": 2},
            "indices": 3,
            "material": material_index,
        }]})
        nodes.append({"mesh": material_index,
                      "translation": [(column - 1.5) * 0.44, 1.94 - row * 0.44, 0]})

occlusion_test = "--occlusion-test" in sys.argv
if occlusion_test:
    def png_chunk(kind, data):
        return (struct.pack(">I", len(data)) + kind + data +
                struct.pack(">I", zlib.crc32(kind + data)))

    # Red is glTF occlusion; green and blue deliberately differ to expose
    # accidental channel selection or color-space decoding.
    png = (b"\x89PNG\r\n\x1a\n" +
           png_chunk(b"IHDR", struct.pack(">IIBBBBB", 1, 1, 8, 6, 0, 0, 0)) +
           png_chunk(b"IDAT", zlib.compress(bytes((0, 64, 255, 255, 255)))) +
           png_chunk(b"IEND", b""))
    while len(binary) % 4:
        binary.append(0)
    image_view = len(views)
    views.append({"buffer": 0, "byteOffset": len(binary), "byteLength": len(png)})
    binary.extend(png)
    for material in materials:
        material["occlusionTexture"] = {"index": 0, "strength": 0.8}

document = {
    "asset": {"version": "2.0", "generator": "RetroFE metal roughness swatch"},
    "scene": 0,
    "scenes": [{"nodes": list(range(len(nodes)))}],
    "nodes": nodes,
    "meshes": meshes,
    "materials": materials,
    "buffers": [{"byteLength": len(binary)}],
    "bufferViews": views,
    "accessors": accessors,
}
if occlusion_test:
    document["images"] = [{"bufferView": image_view, "mimeType": "image/png"}]
    document["textures"] = [{"source": 0}]
json_chunk = json.dumps(document, separators=(",", ":")).encode("utf-8")
json_chunk += b" " * (-len(json_chunk) % 4)
binary += b"\0" * (-len(binary) % 4)
payload = (struct.pack("<III", 0x46546C67, 2,
                       12 + 8 + len(json_chunk) + 8 + len(binary))
           + struct.pack("<II", len(json_chunk), 0x4E4F534A) + json_chunk
           + struct.pack("<II", len(binary), 0x004E4942) + binary)
destination = Path(__file__).resolve().parent / "Assets" / (
    "occlusion-swatch.glb" if occlusion_test else "metal-roughness-swatch.glb")
destination.write_bytes(payload)
print(f"Wrote {destination} ({len(payload)} bytes)")
