"""Generate a tiny, original glTF 2.0 cabinet fixture for the SDL_GPU test."""

import json
import math
import struct
import zlib
from pathlib import Path


def png(width, height, pixel):
    def chunk(kind, data):
        body = kind + data
        return struct.pack(">I", len(data)) + body + struct.pack(">I", zlib.crc32(body))

    rows = bytearray()
    for y in range(height):
        rows.append(0)
        for x in range(width):
            rows.extend(pixel(x, y))
    return (b"\x89PNG\r\n\x1a\n"
            + chunk(b"IHDR", struct.pack(">IIBBBBB", width, height, 8, 6, 0, 0, 0))
            + chunk(b"IDAT", zlib.compress(bytes(rows), 9))
            + chunk(b"IEND", b""))


binary = bytearray()
views = []
accessors = []


def append(data, target=None):
    while len(binary) % 4:
        binary.append(0)
    offset = len(binary)
    binary.extend(data)
    view = {"buffer": 0, "byteOffset": offset, "byteLength": len(data)}
    if target is not None:
        view["target"] = target
    views.append(view)
    return len(views) - 1


def accessor(values, shape, components, component_type=5126, target=34962):
    formats = {5126: "f", 5125: "I"}
    flat = [item for value in values for item in (value if isinstance(value, tuple) else (value,))]
    data = struct.pack("<" + formats[component_type] * len(flat), *flat)
    view = append(data, target)
    entry = {"bufferView": view, "componentType": component_type,
             "count": len(values), "type": shape}
    if shape == "VEC3" and component_type == 5126:
        entry["min"] = [min(v[i] for v in values) for i in range(3)]
        entry["max"] = [max(v[i] for v in values) for i in range(3)]
    accessors.append(entry)
    return len(accessors) - 1


faces = [
    ((0, 0, -1), ((0, 0, 0), (0, 1, 0), (1, 1, 0), (1, 0, 0))),
    ((0, 0, 1), ((0, 0, 1), (1, 0, 1), (1, 1, 1), (0, 1, 1))),
    ((-1, 0, 0), ((0, 0, 1), (0, 1, 1), (0, 1, 0), (0, 0, 0))),
    ((1, 0, 0), ((1, 0, 0), (1, 1, 0), (1, 1, 1), (1, 0, 1))),
    ((0, 1, 0), ((0, 1, 0), (0, 1, 1), (1, 1, 1), (1, 1, 0))),
    ((0, -1, 0), ((0, 0, 1), (0, 0, 0), (1, 0, 0), (1, 0, 1))),
]


def box(low, high, material):
    # Author the fixture in glTF's right handed coordinates, front at +Z.
    low, high = (low[0], low[1], -high[2]), (high[0], high[1], -low[2])
    positions, normals, uvs, indices = [], [], [], []
    for normal, corners in faces:
        base = len(positions)
        for i, corner in enumerate(corners):
            positions.append(tuple(low[j] if corner[j] == 0 else high[j] for j in range(3)))
            normals.append(normal)
            front_uvs = ((0, 1), (1, 1), (1, 0), (0, 0))
            other_uvs = ((0, 0), (0, 1), (1, 1), (1, 0))
            uvs.append((front_uvs if normal == (0, 0, 1) else other_uvs)[i])
        indices.extend(base + i for i in (0, 1, 2, 0, 2, 3))
    return {"attributes": {
        "POSITION": accessor(positions, "VEC3", 3),
        "NORMAL": accessor(normals, "VEC3", 3),
        "TEXCOORD_0": accessor(uvs, "VEC2", 2)},
        "indices": accessor(indices, "SCALAR", 1, 5125, 34963),
        "material": material}


images = [
    png(64, 64, lambda x, y: (18 + (x // 8 % 2) * 10, 31 + (y // 8 % 2) * 12, 62, 255)),
    png(1, 1, lambda x, y: (255, 105, 80, 255)),
    png(1, 1, lambda x, y: (128, 128, 255, 255)),
    png(128, 32, lambda x, y: (225 if (x // 16 + y // 8) % 2 else 70,
                               25 + int(40 * math.sin(x / 12) ** 2), 18, 255)),
]
image_entries = []
for image in images:
    image_entries.append({"bufferView": append(image), "mimeType": "image/png"})

primitives = [
    box((-0.50, 0, -0.37), (0.50, 2.12, 0.37), 0),
    box((-0.46, 0.98, -0.395), (0.46, 1.73, -0.37), 1),
    box((-0.385, 1.08, -0.423), (0.385, 1.62, -0.395), 2),
    box((-0.39, 1.075, -0.429), (0.39, 1.625, -0.426), 4),
    box((-0.45, 1.82, -0.422), (0.45, 2.05, -0.37), 3),
    box((-0.48, 0.73, -0.52), (0.48, 0.91, -0.37), 1),
    box((-0.17, 0.26, -0.385), (0.17, 0.45, -0.37), 1),
]

document = {
    "asset": {"version": "2.0", "generator": "RetroFE SDL_GPU fixture generator"},
    "scene": 0, "scenes": [{"nodes": [0]}],
    "nodes": [{"mesh": 0}], "meshes": [{"primitives": primitives}],
    "buffers": [{"byteLength": len(binary)}],
    "bufferViews": views, "accessors": accessors,
    "images": image_entries, "textures": [{"source": i} for i in range(len(images))],
    "materials": [
        {"name": "body", "pbrMetallicRoughness": {
            "baseColorTexture": {"index": 0}, "metallicRoughnessTexture": {"index": 1},
            "metallicFactor": 0.25, "roughnessFactor": 0.6},
         "normalTexture": {"index": 2}},
        {"name": "trim", "pbrMetallicRoughness": {
            "baseColorFactor": [0.6, 0.63, 0.68, 1], "metallicFactor": 0.85,
            "roughnessFactor": 0.22}},
        {"name": "retrofe.screen", "pbrMetallicRoughness": {
            "metallicFactor": 0, "roughnessFactor": 0.6}},
        {"name": "retrofe.marquee", "pbrMetallicRoughness": {
            "baseColorTexture": {"index": 3}, "metallicFactor": 0,
            "roughnessFactor": 0.6},
         "emissiveTexture": {"index": 3}, "emissiveFactor": [1, 1, 1],
         "extensions": {"KHR_materials_emissive_strength": {"emissiveStrength": 2.5}}},
        {"name": "ScreenGlass", "pbrMetallicRoughness": {
            "baseColorFactor": [0.3, 0.48, 0.65, 0.18], "metallicFactor": 0,
            "roughnessFactor": 0.08}, "alphaMode": "BLEND"},
    ],
    "extensionsUsed": ["KHR_materials_emissive_strength"],
}

json_chunk = json.dumps(document, separators=(",", ":")).encode("utf-8")
json_chunk += b" " * (-len(json_chunk) % 4)
binary += b"\x00" * (-len(binary) % 4)
payload = (struct.pack("<III", 0x46546C67, 2, 12 + 8 + len(json_chunk) + 8 + len(binary))
           + struct.pack("<II", len(json_chunk), 0x4E4F534A) + json_chunk
           + struct.pack("<II", len(binary), 0x004E4942) + binary)
destination = Path(__file__).resolve().parent / "Assets" / "demo-cabinet.glb"
destination.parent.mkdir(exist_ok=True)
destination.write_bytes(payload)
print(f"Wrote {destination} ({len(payload)} bytes)")
