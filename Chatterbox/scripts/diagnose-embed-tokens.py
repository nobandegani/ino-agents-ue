#!/usr/bin/env python3
"""
Diagnostic for Phase B3 chunk-1 failure:
  `Gather '/speech_emb/Gather': idx=15496 must be in [-6563, 6562]`

Ino.Chatterbox.EmbedTest fails on text IDs under our ORT 1.24.3.
Before we accept the working hypothesis (ResembleAI's embed_tokens.onnx
graph has an unmasked dual-Gather that modern ORT correctly rejects),
this script rules out every simpler explanation end-to-end:

  A. Our C++ tokenizer might be producing the wrong IDs
  B. Our staged .onnx file might be corrupted
  C. Our FInoOnnxSession might be passing inputs wrong (name/shape/dtype)
  D. Our FInoOnnxTensor might be constructing the int64 buffer wrong
  E. The graph might be working correctly against latest ORT in Python
     (which would prove the bug is on the C++ side, not the graph)

If Python against latest ORT fails with the identical bounds error on
the exact same .onnx file, the graph is the problem — full stop.

Usage:
    # 1) Make sure you have latest ORT + transformers installed:
    python -m pip install --upgrade onnxruntime onnx transformers numpy

    # 2) Run:
    python Plugins/InoAgents/Chatterbox/scripts/diagnose-embed-tokens.py

Output is plain text; paste the whole thing back and we decide next step.
"""

import hashlib
import os
import sys
import traceback

# Resolve model directory relative to this script — works from any CWD.
# Script path: <PROJECT_ROOT>/Plugins/InoAgents/Chatterbox/scripts/diagnose-embed-tokens.py
# so PROJECT_ROOT is exactly 4 levels up from SCRIPT_DIR.
SCRIPT_DIR   = os.path.dirname(os.path.abspath(__file__))
PROJECT_ROOT = os.path.abspath(os.path.join(SCRIPT_DIR, "..", "..", "..", ".."))
MODEL_DIR    = os.path.join(
    PROJECT_ROOT, "Saved", "PersistentDownloadDir", "InoAgents",
    "Models", "Chatterbox", "fp16"
)

SEPARATOR = "=" * 72


def hr(title):
    print()
    print(SEPARATOR)
    print(f"  {title}")
    print(SEPARATOR)


def main():
    hr("Section 0 — Environment")
    print(f"Python        : {sys.version.split()[0]} ({sys.executable})")

    # Import guard so we can still report what's installed vs missing.
    missing = []
    try:
        import onnxruntime as ort
        print(f"onnxruntime   : {ort.__version__}")
    except ImportError:
        missing.append("onnxruntime")
    try:
        import onnx
        print(f"onnx          : {onnx.__version__}")
    except ImportError:
        missing.append("onnx")
    try:
        import numpy as np
        print(f"numpy         : {np.__version__}")
    except ImportError:
        missing.append("numpy")
    try:
        from transformers import AutoTokenizer
        import transformers
        print(f"transformers  : {transformers.__version__}")
    except ImportError:
        missing.append("transformers")

    if missing:
        print()
        print(f"MISSING: {', '.join(missing)}")
        print("Install with:  python -m pip install --upgrade "
              + " ".join(missing))
        sys.exit(1)

    hr("Section 1 — Staged model directory")
    print(f"Model dir: {MODEL_DIR}")
    if not os.path.isdir(MODEL_DIR):
        print("MODEL DIR DOES NOT EXIST — run setup-chatterbox.ps1 first.")
        sys.exit(2)

    files = sorted(os.listdir(MODEL_DIR))
    for f in files:
        p = os.path.join(MODEL_DIR, f)
        try:
            sz = os.path.getsize(p)
        except OSError:
            sz = -1
        print(f"  {f}  ({sz} bytes)")

    embed_path = os.path.join(MODEL_DIR, "embed_tokens_fp16.onnx")
    embed_data = os.path.join(MODEL_DIR, "embed_tokens_fp16.onnx_data")
    tok_path   = os.path.join(MODEL_DIR, "tokenizer.json")

    for p in (embed_path, embed_data, tok_path):
        if not os.path.isfile(p):
            print(f"MISSING FILE: {p}")
            sys.exit(3)

    hr("Section 2 — File hashes (rule out corrupt download)")
    for p in (embed_path, embed_data):
        with open(p, "rb") as f:
            h = hashlib.sha256(f.read()).hexdigest()
        print(f"  {os.path.basename(p)}")
        print(f"    size   = {os.path.getsize(p)}")
        print(f"    sha256 = {h}")
    print()
    print("(Cross-check sizes: upstream reports embed_tokens_fp16.onnx ≈ 1754")
    print(" bytes and embed_tokens_fp16.onnx_data ≈ 116 MB. If ours match,")
    print(" the download is not corrupted.)")

    hr("Section 3 — HF AutoTokenizer cross-check (rule out our C++ tokenizer)")
    tokenizer = AutoTokenizer.from_pretrained(MODEL_DIR)
    for text in ["Hello world", "Hello, world! I don't know if this is a [laugh] test?"]:
        ids = tokenizer(text, return_tensors="np")["input_ids"].astype(np.int64)
        print(f"  text  : {text!r}")
        print(f"  ids   : {ids.tolist()}")
    print()
    print("Compare with our C++ tokenizer output (from Ino.Chatterbox.EmbedTest):")
    print("   'Hello world'  ->  [15496, 995]")

    hr("Section 4 — Graph structure (which Gather nodes exist, how they're wired)")
    model = onnx.load(embed_path)
    nodes = model.graph.node
    print(f"Graph has {len(nodes)} nodes.")

    op_counts = {}
    for n in nodes:
        op_counts[n.op_type] = op_counts.get(n.op_type, 0) + 1
    print(f"Op types:")
    for op, count in sorted(op_counts.items()):
        print(f"  {op}: {count}")

    print()
    print("Gather nodes (the suspect op):")
    gather_nodes = [n for n in nodes if n.op_type == "Gather"]
    for g in gather_nodes:
        print(f"  name       = {g.name!r}")
        print(f"    inputs   = {list(g.input)}")
        print(f"    outputs  = {list(g.output)}")
    if not gather_nodes:
        print("  (none)")

    print()
    print("Where nodes (the expected routing mux if the graph IS dual-path):")
    where_nodes = [n for n in nodes if n.op_type == "Where"]
    for w in where_nodes:
        print(f"  name       = {w.name!r}")
        print(f"    inputs   = {list(w.input)}")
        print(f"    outputs  = {list(w.output)}")
    if not where_nodes:
        print("  (none)")

    print()
    print("Clip nodes (expected to clamp indices before each Gather, if safe):")
    clip_nodes = [n for n in nodes if n.op_type == "Clip"]
    for c in clip_nodes:
        print(f"  name       = {c.name!r}")
        print(f"    inputs   = {list(c.input)}")
        print(f"    outputs  = {list(c.output)}")
    if not clip_nodes:
        print("  (none — this is suspicious if Gather count is 2)")

    print()
    print("Session inputs/outputs (does the graph REALLY take only input_ids?):")
    sess = ort.InferenceSession(embed_path, providers=["CPUExecutionProvider"])
    print(f"  active providers = {sess.get_providers()}")
    for i in sess.get_inputs():
        print(f"  INPUT  : name={i.name!r} shape={i.shape} type={i.type}")
    for o in sess.get_outputs():
        print(f"  OUTPUT : name={o.name!r} shape={o.shape} type={o.type}")

    hr("Section 5 — Run EXACT Python reference against our EXACT .onnx file")
    print("This is the decisive test. The Python reference in the ResembleAI")
    print("README does `embed_tokens_session.run(None, {\"input_ids\": input_ids})`")
    print("with the text-tokenizer output (int64 [1, N]). We're going to run")
    print("that same call on the same file with the latest ORT.")
    print()

    # ---- 5a: feed a known-safe speech ID ----
    print("-- 5a: id=0 (safe for any table) --")
    ids0 = np.array([[0]], dtype=np.int64)
    try:
        out = sess.run(None, {"input_ids": ids0})[0]
        print(f"   SUCCESS: output shape {out.shape}, dtype {out.dtype}")
    except Exception as e:
        print(f"   FAILED: {type(e).__name__}: {e}")

    # ---- 5b: feed START_SPEECH_TOKEN (6561) ----
    print()
    print("-- 5b: id=6561 (START_SPEECH_TOKEN) --")
    ids_speech = np.array([[6561]], dtype=np.int64)
    try:
        out = sess.run(None, {"input_ids": ids_speech})[0]
        print(f"   SUCCESS: output shape {out.shape}, dtype {out.dtype}")
    except Exception as e:
        print(f"   FAILED: {type(e).__name__}: {e}")

    # ---- 5c: feed tokenized text "Hello world" ----
    print()
    print("-- 5c: tokenized 'Hello world' (expects [15496, 995] from HF tokenizer) --")
    ids_text = tokenizer("Hello world", return_tensors="np")["input_ids"].astype(np.int64)
    print(f"   feeding {ids_text.tolist()}")
    try:
        out = sess.run(None, {"input_ids": ids_text})[0]
        print(f"   SUCCESS: output shape {out.shape}, dtype {out.dtype}")
        print(f"   first 8 dims of token[0]: {out[0, 0, :8].tolist()}")
        print()
        print("   !!! Python+latest-ORT SUCCEEDS on text IDs, but our C++ fails.")
        print("   !!! That means the bug is in our C++, NOT the graph.")
    except Exception as e:
        print(f"   FAILED: {type(e).__name__}: {e}")
        print()
        print("   >>> Python+latest-ORT FAILS identically to our C++.")
        print("   >>> Confirms the graph is broken against modern ORT.")

    # ---- 5d: try the README's claimed signature — does the README REALLY work? ----
    print()
    print("-- 5d: sanity — same call pattern from ResembleAI's README --")
    print("       embed_tokens_session.run(None, {'input_ids': input_ids})")
    print("       where input_ids is the raw tokenizer output, no preprocessing.")
    try:
        out = sess.run(None, {"input_ids": ids_text})[0]
        print(f"   (same as 5c, SUCCESS — README is valid against current ORT)")
    except Exception as e:
        print(f"   (same as 5c, FAILS — README's reference is broken against current ORT)")
        print(f"   {type(e).__name__}: {e}")

    hr("Conclusion")
    print("Paste this entire output back to the agent. The key signals:")
    print("  - Section 3: do HF AutoTokenizer IDs match our C++ [15496, 995]?")
    print("  - Section 4: how many Gather nodes? Are there protective Clip/Where")
    print("               ops? What does the graph topology look like?")
    print("  - Section 5c: does Python with latest ORT reproduce our failure?")
    print()


if __name__ == "__main__":
    try:
        main()
    except SystemExit:
        raise
    except Exception:
        print()
        print("UNCAUGHT EXCEPTION:")
        traceback.print_exc()
        sys.exit(99)
