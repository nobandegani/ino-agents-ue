#!/usr/bin/env python3
# Copyright 2026 Inoland. Licensed under the Apache License, Version 2.0.
"""
Minimal Python ORT-DirectML repro for the speech_encoder_q4f16.onnx failure
seen in the UE plugin (InoAgents).

Background
----------
In the UE plugin we run Chatterbox Turbo's speech_encoder ONNX graph via
onnxruntime-directml 1.24.3. With fp16/q4f16 weights on an Intel Arc Xe-LPG
iGPU (Core Ultra 9 285K), Run() aborts with:

  Non-zero status code returned while running MultiHeadAttention node.
  Name:'/s3/encoder/blocks.0/attn/MultiHeadAttention'
  Status Message: <ORT>/core/providers/dml/DmlExecutionProvider/src/
                  MLOperatorAuthorImpl.cpp(2508):
      Exception(3) 80070057 The parameter is incorrect.

MLOperatorAuthorImpl.cpp:2508 is the line that calls
`m_operatorFactory->CreateKernel(...)` — so the DirectML kernel factory
itself is rejecting the MHA op configuration. That is an EP-level failure,
not a session/option misconfiguration on our side.

Before we declare this an upstream bug we want a **minimal Python repro**
that reproduces the failure with stock onnxruntime-directml (no UE, no
renamed DLLs, no PE import-table patching). If the stock Python wheel
fails identically, our C++ wrapper is out of the picture.

Usage
-----
  pip install onnxruntime-directml==1.24.3 numpy
  python repro-dml-encoder.py <abs-path-to-speech_encoder_q4f16.onnx>

If the path argument is omitted the script falls back to the default
project staging location:
  <PROJECT_ROOT>/Saved/PersistentDownloadDir/InoAgents/Models/Chatterbox/q4f16/
    speech_encoder_q4f16.onnx

Experiments run (in order):
  (1) CPU EP baseline — prove the graph itself is valid.
  (2) DML EP default  — match our plugin's setup exactly.
  (3) DML EP + disable_metacommands=True — the one provider-option
      workaround Microsoft documents for MHA-on-DML quantized failures.

Each experiment prints:
  - provider list requested
  - actual providers the session bound to
  - Run() latency or the exact error message

Exit status is 0 if CPU succeeded (so you at least know Python works);
non-zero otherwise.
"""

from __future__ import annotations

import os
import sys
import time
import traceback
from typing import Any

import numpy as np
import onnxruntime as ort


# ---------------------------------------------------------------------------

SCRIPT_DIR   = os.path.dirname(os.path.abspath(__file__))
PROJECT_ROOT = os.path.abspath(os.path.join(SCRIPT_DIR, "..", "..", "..", ".."))
DEFAULT_MODEL = os.path.join(
    PROJECT_ROOT, "Saved", "PersistentDownloadDir", "InoAgents",
    "Models", "Chatterbox", "q4f16", "speech_encoder_q4f16.onnx",
)

# Reference audio length for the probe. The encoder accepts variable-length
# fp32 audio at 24 kHz; 1 s is enough to exercise every attention block.
SAMPLE_RATE = 24000
N_SAMPLES   = 24000


def hr(title: str) -> None:
    print()
    print("=" * 72)
    print(title)
    print("=" * 72)


def print_env() -> None:
    hr("Environment")
    print(f"Python              : {sys.version.split()[0]}")
    print(f"onnxruntime version : {ort.__version__}")
    print(f"available providers : {ort.get_available_providers()}")
    try:
        build_info = ort.get_build_info()
        # Short-circuit print because build_info is a long string.
        for line in build_info.splitlines():
            line = line.strip()
            if any(k in line.lower() for k in ("dml", "directml", "cuda", "version", "build")):
                print(f"  build: {line}")
    except Exception:
        pass


def build_session_options() -> ort.SessionOptions:
    """Match the UE plugin's FInoOnnxSession setup exactly."""
    opts = ort.SessionOptions()
    # Per Microsoft's DML docs these two are mandatory for DML.
    # Our C++ code sets both unconditionally when DML is requested.
    opts.enable_mem_pattern = False
    opts.execution_mode     = ort.ExecutionMode.ORT_SEQUENTIAL
    # Matches our ORT_ENABLE_ALL setting.
    opts.graph_optimization_level = ort.GraphOptimizationLevel.ORT_ENABLE_ALL
    # No custom SessionConfig entries, no profiling, default thread counts.
    return opts


def build_zero_inputs(sess: ort.InferenceSession) -> dict[str, np.ndarray]:
    """Allocate zero-filled inputs matching the session's declared shapes.

    The speech_encoder takes a single fp32 tensor called `audio_values` of
    shape [1, N]. `N` is symbolic (dynamic) in the ONNX graph, so we pick
    N_SAMPLES at call time.
    """
    feed: dict[str, np.ndarray] = {}
    for meta in sess.get_inputs():
        name = meta.name
        # Resolve symbolic dims to concrete values. First symbolic dim is
        # batch (1), last symbolic dim is sample/time axis (N_SAMPLES).
        shape = []
        dims = list(meta.shape)
        for i, d in enumerate(dims):
            if isinstance(d, int) and d > 0:
                shape.append(d)
            else:
                # symbolic: batch dim → 1, time/feature dim → N_SAMPLES
                if i == len(dims) - 1:
                    shape.append(N_SAMPLES)
                else:
                    shape.append(1)

        # Map ONNX dtype string to numpy dtype.
        t = meta.type  # e.g. "tensor(float)"
        if "float16" in t:
            dtype = np.float16
        elif "float" in t:
            dtype = np.float32
        elif "int64" in t:
            dtype = np.int64
        elif "int32" in t:
            dtype = np.int32
        elif "bool" in t:
            dtype = np.bool_
        else:
            raise RuntimeError(f"Unhandled input dtype: {t}")

        arr = np.zeros(shape, dtype=dtype)
        print(f"    input '{name}': shape={list(arr.shape)} dtype={arr.dtype}")
        feed[name] = arr
    return feed


def run_experiment(
    label: str,
    model_path: str,
    providers: list[Any],
) -> bool:
    """Load the session, allocate inputs, run one inference.

    Returns True on success, False on any failure (printing full trace).
    """
    hr(f"Experiment: {label}")
    print(f"requested providers: {providers}")
    opts = build_session_options()

    t_load0 = time.perf_counter()
    try:
        sess = ort.InferenceSession(
            model_path,
            sess_options=opts,
            providers=providers,
        )
    except Exception as e:
        print("!! InferenceSession() threw:")
        traceback.print_exc()
        return False
    t_load = time.perf_counter() - t_load0

    print(f"session loaded in {t_load*1000:.1f} ms")
    print(f"actual providers    : {sess.get_providers()}")
    print(f"provider options    : {sess.get_provider_options()}")

    print("  building inputs:")
    try:
        feed = build_zero_inputs(sess)
    except Exception:
        print("!! build_zero_inputs() threw:")
        traceback.print_exc()
        return False

    t_run0 = time.perf_counter()
    try:
        outputs = sess.run(None, feed)
    except Exception as e:
        t_run = time.perf_counter() - t_run0
        print(f"!! Run() failed after {t_run*1000:.1f} ms:")
        # The message from ORT's status is on the exception text itself.
        # Print it in full — this is the signature we want to compare.
        print(repr(e))
        return False
    t_run = time.perf_counter() - t_run0

    print(f"Run() OK in {t_run*1000:.1f} ms")
    for i, (meta, arr) in enumerate(zip(sess.get_outputs(), outputs)):
        print(f"  output[{i}] '{meta.name}': shape={list(arr.shape)} dtype={arr.dtype}")
    return True


def main() -> int:
    model_path = sys.argv[1] if len(sys.argv) > 1 else DEFAULT_MODEL
    if not os.path.isfile(model_path):
        print(f"model not found: {model_path}")
        return 2
    print_env()
    print()
    print(f"model  : {model_path}")
    print(f"size   : {os.path.getsize(model_path)/1e6:.1f} MB")
    data_path = model_path + "_data"
    if os.path.isfile(data_path):
        print(f"weights: {data_path} ({os.path.getsize(data_path)/1e9:.2f} GB)")

    # Sanity: make sure DML is actually available.
    avail = set(ort.get_available_providers())
    if "DmlExecutionProvider" not in avail:
        print("!! DmlExecutionProvider not available in this Python install.")
        print("   `pip install onnxruntime-directml==1.24.3` to fix.")
        return 3

    results: dict[str, bool] = {}

    # (1) CPU baseline.
    results["cpu"] = run_experiment(
        "CPU baseline (sanity check)",
        model_path,
        providers=["CPUExecutionProvider"],
    )

    # (2) DML default — matches our plugin exactly.
    results["dml-default"] = run_experiment(
        "DML EP (device_id=0, default options) — matches UE plugin",
        model_path,
        providers=[
            ("DmlExecutionProvider", {"device_id": 0}),
            "CPUExecutionProvider",
        ],
    )

    # (3) DML + disable_metacommands (the one untried workaround).
    results["dml-nometacmd"] = run_experiment(
        "DML EP + disable_metacommands=True",
        model_path,
        providers=[
            ("DmlExecutionProvider", {
                "device_id": 0,
                "disable_metacommands": True,
            }),
            "CPUExecutionProvider",
        ],
    )

    hr("Summary")
    for k, v in results.items():
        print(f"  {k:<16} : {'PASS' if v else 'FAIL'}")
    # We consider the script "successful" if CPU worked — that proves our
    # harness is sane. The DML lines may FAIL by design (that's the bug
    # we're probing).
    return 0 if results.get("cpu") else 1


if __name__ == "__main__":
    sys.exit(main())
