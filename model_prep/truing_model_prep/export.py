"""Compact binary artifact for the firmware (SPEC 8.7, 11.4 "expansion happens at
initialization"), plus C headers embedding it and the parity cases for tests.

Binary layout (all little-endian; matrices float32 row-major):

  header (120 bytes)
    u32 magic 'TRIA'            u16 schema_version=1        u16 reserved
    u32 artifact_id             char[32] artifact_name (NUL padded)
    u8[32] generating_fingerprint (raw SHA-256)   u8[32] content_hash (SHA-256 over the payload)
    u8 n_spokes  u8 n_rim_angles  u8 N_lat  u8 N_rad
    u8 asymmetric  u8 n_mt_identified  u8 n_layouts  u8 T_target_present
    u32 payload_len
  payload
    f32 tol_lateral_mm, tol_radial_mm, tol_tension_n, trust_radial, trust_tension, c_side_a, c_side_b
    f32 cond_i_residual, cond_i_tolerance, cond_ii_residual, cond_ii_tolerance,
        uniqueness_margin, uniqueness_threshold        (NaN when not evaluated)
    u8  displacement_null_dim
    f32 T_target_assumed[n_spokes]     (present only when T_target_present)
    f32 n_mt[n_spokes]                 (present only when n_mt_identified)
    f32 coeffs_lat[n_spokes][2*N_lat+1]
    f32 coeffs_rad[n_spokes][2*N_rad+1]
    f32 phi_t[n_spokes][n_spokes]
    u8  class_map[n_spokes]            (side<<4 | lead_trail; side A=1 B=2; leading=1 trailing=2)
    per layout:
      u8 layout_id (1 FULL, 2 TENSION_ABSENT)  u8 effective_rank  u8 expected_null_dim  u8 reserved
      u16 n_rows  u16 reserved  f32 effective_condition_number
      u32 row_mask[4]
      f32 phi_pseudoinverse[n_spokes][n_rows]
"""
from __future__ import annotations

import hashlib
import json
import struct

import numpy as np

from .artifact import LAYOUT_FULL, LAYOUT_TENSION_ABSENT, Artifact

MAGIC = 0x41495254  # 'TRIA' little-endian
SCHEMA_VERSION = 1
HEADER_BYTES = 120  # 12 + 32 name + 32 fingerprint + 32 content hash + 8 dims + 4 payload_len
LAYOUT_CODES = {LAYOUT_FULL: 1, LAYOUT_TENSION_ABSENT: 2}
SIDE_CODES = {"A": 1, "B": 2}
LT_CODES = {"leading": 1, "trailing": 2}


def _f32s(a) -> bytes:
    return np.ascontiguousarray(np.asarray(a, dtype="<f4")).tobytes()


def _mask_words(mask: np.ndarray) -> list[int]:
    words = [0, 0, 0, 0]
    for r, on in enumerate(mask):
        if on:
            words[r // 32] |= 1 << (r % 32)
    return words


def export_binary(art: Artifact, artifact_id: int) -> bytes:
    cm = art.common_mode_direction
    n = art.n_spokes
    p = bytearray()
    p += struct.pack("<7f", art.weights_used["tol_lateral_mm"], art.weights_used["tol_radial_mm"],
                     art.weights_used["tol_tension_n"], art.weights_used["trust_radial"], art.weights_used["trust_tension"],
                     art.c_side_a, art.c_side_b)
    nan = float("nan")
    p += struct.pack("<6f", nan if cm.cond_i_residual is None else cm.cond_i_residual, cm.cond_i_tolerance,
                     nan if cm.cond_ii_residual is None else cm.cond_ii_residual, cm.cond_ii_tolerance,
                     nan if cm.uniqueness_margin is None else cm.uniqueness_margin, cm.uniqueness_threshold)
    p += struct.pack("<B", cm.displacement_null_dim)
    t_present = cm.T_target_assumed is not None
    if t_present:
        p += _f32s(cm.T_target_assumed)
    if cm.identified:
        p += _f32s(cm.vector)
    p += _f32s(art.coeffs_lat)
    p += _f32s(art.coeffs_rad)
    p += _f32s(art.phi_t)
    p += bytes((SIDE_CODES[e["side"]] << 4) | LT_CODES[e["lead_trail"]] for e in art.phi_t_class_map)
    layouts = [art.layout(LAYOUT_FULL), art.layout(LAYOUT_TENSION_ABSENT)]
    for L in layouts:
        n_rows = int(L.row_mask.sum())
        p += struct.pack("<BBBBHHf", LAYOUT_CODES[L.layout_id], L.effective_rank, L.expected_null_dim, 0, n_rows, 0,
                         L.effective_condition_number)
        p += struct.pack("<4I", *_mask_words(L.row_mask))
        assert L.phi_pseudoinverse.shape == (n, n_rows)
        p += _f32s(L.phi_pseudoinverse)
    payload = bytes(p)
    chash = hashlib.sha256(payload).digest()
    fp = bytes.fromhex(art.generating_fingerprint)
    name = art.artifact_id.encode("utf-8")[:31].ljust(32, b"\0")
    header = struct.pack("<IHHI", MAGIC, SCHEMA_VERSION, 0, artifact_id) + name + fp + chash + \
        struct.pack("<8B", n, art.n_rim_angles, art.N_lat, art.N_rad, int(art.generating_parameters["wheel"]["asymmetric"]),
                    int(cm.identified), len(layouts), int(t_present)) + struct.pack("<I", len(payload))
    assert len(header) == HEADER_BYTES
    return header + payload


def c_array(data: bytes, symbol: str, comment: str) -> str:
    lines = [f"/* {comment} */", "#include <stddef.h>", "#include <stdint.h>", "",
             f"const size_t {symbol}_len = {len(data)}u;", f"const uint8_t {symbol}[{len(data)}] = {{"]
    for i in range(0, len(data), 16):
        lines.append("    " + ", ".join(f"0x{b:02x}" for b in data[i:i + 16]) + ",")
    lines.append("};")
    return "\n".join(lines) + "\n"


def _flit(x) -> str:
    """C float literal: always carries a decimal point or exponent so the f suffix is legal."""
    t = f"{float(x):.9g}"
    if "." not in t and "e" not in t and "n" not in t:
        t += ".0"
    return t + "f"


def parity_c_header(parity_doc: dict, symbol_prefix: str) -> str:
    """Emit the parity cases as C initialisers for the native and on-device parity tests."""
    cases = parity_doc["cases"]
    n = len(cases[0]["d_applied_rev"])
    nra = len(cases[0]["u_mm"])
    out = ["/* GENERATED by truing_model_prep.export.parity_c_header from golden/parity_sym32.json. Do not edit. */",
           "#include <stddef.h>", "#include <stdint.h>", "",
           f"#define {symbol_prefix.upper()}_N_SPOKES {n}u", f"#define {symbol_prefix.upper()}_N_RIM {nra}u",
           f"#define {symbol_prefix.upper()}_N_CASES {len(cases)}u",
           f"static const char {symbol_prefix}_fingerprint_hex[] = \"{parity_doc['generating_fingerprint']}\";",
           "typedef struct {", "    int layout;                /* 1 FULL, 2 TENSION_ABSENT */",
           "    int refused;               /* 1 when the host reference refused this layout */",
           f"    float d_ls[{n}];", f"    float d_shape[{n}];", f"    float d_cm[{n}];", f"    float d_adj[{n}];",
           "    float cost_j;", "    int n_active_rows;", "    float s_scale;", "    int mean_tension_applied;",
           f"}} {symbol_prefix}_layout_case_t;", "typedef struct {", f"    float d_applied[{n}];", f"    float u_mm[{nra}];",
           f"    float v_mm[{nra}];", f"    float T_n[{n}];", f"    {symbol_prefix}_layout_case_t layouts[2];",
           f"}} {symbol_prefix}_case_t;", ""]

    def flist(vals):
        return "{" + ", ".join(_flit(x) for x in vals) + "}"

    out.append(f"static const {symbol_prefix}_case_t {symbol_prefix}_cases[{len(cases)}] = {{")
    for c in cases:
        out.append("  {")
        out.append(f"    {flist(c['d_applied_rev'])},")
        out.append(f"    {flist(c['u_mm'])},")
        out.append(f"    {flist(c['v_mm'])},")
        out.append(f"    {flist(c['T_n'])},")
        out.append("    {")
        for lid in (LAYOUT_FULL, LAYOUT_TENSION_ABSENT):
            e = c["layouts"][lid]
            refused = e.get("refused") is not None
            zeros = [0.0] * n
            out.append("      {" + f"{LAYOUT_CODES[lid]}, {int(refused)}, {flist(e['d_ls_rev'])}, "
                       f"{flist(e.get('d_shape_rev', zeros))}, {flist(e.get('d_cm_rev', zeros))}, "
                       f"{flist(e.get('d_adj_rev', zeros))}, {_flit(e['cost_j'])}, {int(e['n_active_rows'])}, "
                       f"{_flit(e['s_scale_n'] if e['s_scale_n'] is not None else 0.0)}, "
                       f"{int(bool(e.get('mean_tension_targeting_applied', False)))}" + "},")
        out.append("    },")
        out.append("  },")
    out.append("};")
    return "\n".join(out) + "\n"


def write_c_fixtures(art: Artifact, artifact_id: int, parity_json_path: str, artifact_c_path: str,
                     parity_h_path: str, name: str) -> tuple[str, str]:
    """Artifact blob as a C source (one definition, declared by the fixtures library) and the
    parity cases as a header of static initialisers (included by exactly one test unit)."""
    blob = export_binary(art, artifact_id)
    with open(artifact_c_path, "w", encoding="utf-8") as f:
        f.write(c_array(blob, f"{name}_artifact_blob",
                        f"GENERATED by truing_model_prep export-c-fixtures: compact influence artifact '{art.artifact_id}'. "
                        "SYNTHETIC fixture wheel (bike-wheel-calc), not a measured wheel. Do not edit."))
        f.write(f'const char {name}_artifact_fingerprint_hex[65] = "{art.generating_fingerprint}";\n')
        f.write(f'const char {name}_artifact_content_hash_hex[65] = "{blob[76:108].hex()}";\n')
    with open(parity_json_path, "r", encoding="utf-8") as f:
        doc = json.load(f)
    with open(parity_h_path, "w", encoding="utf-8") as f:
        f.write(parity_c_header(doc, f"{name}_parity"))
    return artifact_c_path, parity_h_path
