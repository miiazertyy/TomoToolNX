"""
preprocess_u2net.py
Run once on PC to convert u2netp.onnx → u2netp.bin
Place u2netp.bin at /switch/TomoToolNX/u2netp.bin on your Switch SD card.

Usage:
  pip install onnx
  python tools/preprocess_u2net.py [u2netp.onnx] [u2netp.bin]
"""

import struct
import sys
import os

try:
    import onnx
    from onnx import shape_inference
    import numpy as np
except ImportError:
    print("ERROR: pip install onnx")
    sys.exit(1)

onnx_path = sys.argv[1] if len(sys.argv) > 1 else "u2netp.onnx"
out_path  = sys.argv[2] if len(sys.argv) > 2 else "u2netp.bin"

print(f"Loading {onnx_path} ...")
m = onnx.load(onnx_path)
m = shape_inference.infer_shapes(m)
g = m.graph

# ─── dtype map (name → elem_type: 1=float32, 7=int64) ───────────────────────

dtype_map = {}
for vi in list(g.value_info) + list(g.input) + list(g.output):
    dtype_map[vi.name] = vi.type.tensor_type.elem_type
for init in g.initializer:
    dtype_map[init.name] = init.data_type

# ─── Shape map for Resize target computation ──────────────────────────────────

shape_map = {}
for vi in list(g.value_info) + list(g.input) + list(g.output):
    tt = vi.type.tensor_type
    if tt.HasField("shape"):
        shape_map[vi.name] = [d.dim_value for d in tt.shape.dim]
for init in g.initializer:
    shape_map[init.name] = list(init.dims)

# Trace shape-computation nodes to find Resize target sizes
sim_vals = {}
for n in g.node:
    if n.op_type == "Constant":
        arr = onnx.numpy_helper.to_array(n.attribute[0].t)
        sim_vals[n.output[0]] = arr
    elif n.op_type == "Shape":
        inp = n.input[0]
        if inp in shape_map and shape_map[inp]:
            sim_vals[n.output[0]] = np.array(shape_map[inp], dtype=np.int64)
    elif n.op_type == "Gather":
        dn, idxn = n.input[0], n.input[1]
        if dn in sim_vals and idxn in sim_vals:
            d, i = sim_vals[dn], sim_vals[idxn]
            sim_vals[n.output[0]] = d[int(i.flat[0])]
    elif n.op_type == "Unsqueeze":
        inp_n = n.input[0]
        if inp_n in sim_vals:
            sim_vals[n.output[0]] = np.atleast_1d(sim_vals[inp_n])
    elif n.op_type == "Concat":
        if all(nm in sim_vals for nm in n.input if nm):
            parts = [np.atleast_1d(sim_vals[nm]) for nm in n.input if nm and nm in sim_vals]
            sim_vals[n.output[0]] = np.concatenate(parts)
    elif n.op_type == "Cast":
        inp_n = n.input[0]
        if inp_n in sim_vals:
            sim_vals[n.output[0]] = sim_vals[inp_n].astype(np.int64)
    elif n.op_type == "Slice":
        dn, sn, en = n.input[0], n.input[1], n.input[2]
        if all(nm in sim_vals for nm in [dn, sn, en]):
            d = sim_vals[dn]
            s, e = int(sim_vals[sn].flat[0]), int(sim_vals[en].flat[0])
            sim_vals[n.output[0]] = d[s:e]

# Resize name → (target_h, target_w)
resize_targets = {}
for n in g.node:
    if n.op_type == "Resize" and len(n.input) > 3:
        sizes_name = n.input[3]
        if sizes_name and sizes_name in sim_vals:
            sz = sim_vals[sizes_name]
            resize_targets[n.output[0]] = (int(sz[-2]), int(sz[-1]))

# ─── Collect weights (float initializers) ────────────────────────────────────

weights = {}  # name → numpy float32 array
for init in g.initializer:
    if init.data_type == 1:  # float32
        weights[init.name] = onnx.numpy_helper.to_array(init).astype(np.float32)

# ─── Filter to real compute nodes ─────────────────────────────────────────────

SHAPE_OPS = {"Shape", "Gather", "Unsqueeze", "Slice", "Cast"}

OP_CODES = {
    "Conv": 0, "Relu": 1, "MaxPool": 2, "Concat": 3,
    "Resize": 4, "Sigmoid": 5, "Add": 6,
}

def is_shape_node(n):
    if n.op_type in SHAPE_OPS:
        return True
    if n.op_type == "Constant":
        return True  # all Constants (int64 shape vals + empty float tensors)
    if n.op_type == "Concat":
        return dtype_map.get(n.output[0], 1) == 7  # int64 concat = shape
    return False

real_nodes = [n for n in g.node if not is_shape_node(n) and n.op_type in OP_CODES]
print(f"Real compute nodes: {len(real_nodes)}")
from collections import Counter
c = Counter(n.op_type for n in real_nodes)
for k, v in sorted(c.items(), key=lambda x: -x[1]):
    print(f"  {k}: {v}")

# ─── Write binary ─────────────────────────────────────────────────────────────

def write_u32(f, v):  f.write(struct.pack("<I", int(v)))
def write_i32(f, v):  f.write(struct.pack("<i", int(v)))
def write_str(f, s):
    b = s.encode("utf-8")
    write_u32(f, len(b))
    f.write(b)

with open(out_path, "wb") as f:
    # Magic + version
    f.write(b"U2NP")
    write_u32(f, 1)

    # ── Weights ──
    write_u32(f, len(weights))
    for name, arr in weights.items():
        write_str(f, name)
        flat = arr.flatten().astype(np.float32)
        shape = list(arr.shape)
        write_u32(f, len(shape))
        for d in shape: write_i32(f, d)
        write_u32(f, len(flat))
        f.write(flat.tobytes())

    # ── Nodes ──
    write_u32(f, len(real_nodes))
    for n in real_nodes:
        op_code = OP_CODES[n.op_type]
        f.write(struct.pack("B", op_code))

        # For Resize, only emit the data input (index 0), skip roi/scales/sizes
        if n.op_type == "Resize":
            inputs = [n.input[0]]
        else:
            inputs = list(n.input)

        f.write(struct.pack("B", len(inputs)))
        for inp in inputs:
            write_str(f, inp)

        f.write(struct.pack("B", len(n.output)))
        for out in n.output:
            write_str(f, out)

        # attrs[6]
        attrs = [0] * 6
        attr_map = {a.name: a for a in n.attribute}
        if n.op_type == "Conv":
            pads  = list(attr_map["pads"].ints)      if "pads"      in attr_map else [0,0,0,0]
            strs  = list(attr_map["strides"].ints)   if "strides"   in attr_map else [1,1]
            dils  = list(attr_map["dilations"].ints) if "dilations" in attr_map else [1,1]
            attrs = [pads[0], pads[1], strs[0], strs[1], dils[0], dils[1]]
        elif n.op_type == "MaxPool":
            kern  = list(attr_map["kernel_shape"].ints)
            strs  = list(attr_map["strides"].ints) if "strides" in attr_map else [1,1]
            ceil_m = attr_map["ceil_mode"].i if "ceil_mode" in attr_map else 0
            attrs = [kern[0], kern[1], strs[0], strs[1], ceil_m, 0]
        elif n.op_type == "Concat":
            attrs[0] = attr_map["axis"].i if "axis" in attr_map else 1
        elif n.op_type == "Resize":
            th, tw = resize_targets.get(n.output[0], (0, 0))
            attrs = [th, tw, 0, 0, 0, 0]

        for a in attrs:
            write_i32(f, a)

sz = os.path.getsize(out_path)
print(f"\nWrote {out_path}  ({sz/1e6:.1f} MB)")
print("Copy this file to /switch/TomoToolNX/u2netp.bin on your Switch SD card.")
