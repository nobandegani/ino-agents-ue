# Third-Party Notices

InoAgents is distributed under the [Apache License 2.0](LICENSE), Copyright 2026 Inoland.

**That grant does not cover every path in this repository.** The table below is authoritative
where it disagrees with any other document here. Each component listed remains the property of
its respective owner and is governed by its own license.

---

## Summary

| Path | Component | License | Commercial use |
|---|---|---|---|
| `Source/`, `Resources/`, `*.md` | InoAgents | Apache-2.0 | Unrestricted |
| `Content/` *except* `Content/NeuTTS/` | InoAgents | Apache-2.0 | Unrestricted |
| `Content/NeuTTS/Voices/*.uasset` | Derived from NeuTTS reference voices | NeuTTS Open License v1.0 | **Capped — see below** |
| `DepricatedModules/NeuTTS/vendor/` | [neuphonic/neutts](https://github.com/neuphonic/neutts) v1.2.0 | NeuTTS Open License v1.0 | **Capped — see below** |
| `DepricatedModules/NeuTTS/voices/*.inv` | Derived from NeuTTS reference voices | NeuTTS Open License v1.0 | **Capped — see below** |
| *(not in repo)* LiteRT / LiteRT-LM | [google-ai-edge/LiteRT-LM](https://github.com/google-ai-edge/LiteRT-LM) | Apache-2.0 | Unrestricted |
| *(not in repo)* eSpeak NG | [espeak-ng](https://github.com/espeak-ng/espeak-ng) | GPL-3.0 | See §3 |
| *(not in repo)* RuntimeAudioImporter | [gtreshchev/RuntimeAudioImporter](https://github.com/gtreshchev/RuntimeAudioImporter) | MIT | Unrestricted |
| *(not in repo)* Gemma model weights | Google | [Gemma Terms of Use](https://ai.google.dev/gemma/terms) | See upstream |

"*(not in repo)*" means this repository contains no copy of that component — it is resolved at
build time from a sibling plugin, or downloaded at runtime from its upstream host.

---

## 1. NeuTTS / NeuCodec — Neuphonic Limited

**License:** NeuTTS Open License v1.0 (11 December 2025).
**Full text:** [`DepricatedModules/NeuTTS/vendor/LICENSE`](DepricatedModules/NeuTTS/vendor/LICENSE)
**Upstream:** https://github.com/neuphonic/neutts

> This is **not** an OSI-approved open-source license. Read it before shipping.

### What is covered

1. **Vendored source** — `DepricatedModules/NeuTTS/vendor/` is a copy of the upstream `neutts`
   Python package v1.2.0, including its `examples/`, `tests/`, `samples/` and build files. It
   is retained for reference only; it is not built, imported, or executed by this plugin.
2. **Voice assets** — `Content/NeuTTS/Voices/INV_IA_Dave.uasset` and `INV_IA_Jo.uasset`, plus
   `DepricatedModules/NeuTTS/voices/*.inv`, are **derivative works** of Neuphonic's reference
   voice samples (`vendor/samples/dave.*`, `jo.*`, `greta.*`, `juliette.*`, `mateo.*`). They
   were produced by `DepricatedModules/NeuTTS/scripts/build-voices.py`, which re-encodes the
   upstream `.pt` NeuCodec code tensors and `.txt` transcripts into an engine-ingestible form.
   These assets ship inside the plugin's `Content/` directory and therefore reach every
   consumer of the plugin.
3. **Model weights** — the NeuTTS Nano backbone and NeuCodec decoder are **not** in this
   repository. They download at runtime from their upstream hosts and carry their own terms.

### The commercial-use cap (§5)

The copyright and patent grants are conditioned on your legal entity **not reaching
$5,000,000 USD in annual revenue**. Specifically:

- §5(a) — commercial rights are conditioned on staying under the Threshold.
- §5(b) — commercial use by an entity over the Threshold **is not licensed** by this agreement;
  you must obtain a paid license from Neuphonic.
- §5(c) — the Threshold does not apply to a qualified non-profit's non-commercial or research
  use.
- §9 — the license **terminates automatically and immediately** on any breach, and you must
  then cease all use and delete all copies.

"Commercial Use" is defined broadly as *any* use for direct or indirect commercial advantage or
monetary compensation — which includes shipping a commercial game. The cap applies to the
**aggregated revenue of your legal entity**, including parents and subsidiaries under common
control, not to revenue attributable to the product.

### Redistribution conditions (§4)

If you redistribute this plugin or a derivative, §4 requires that you:

- **(a)** give recipients a copy of the NeuTTS license — satisfied by retaining
  `DepricatedModules/NeuTTS/vendor/LICENSE`;
- **(b)** mark any modified files with prominent notices stating that you changed them;
- **(c)** retain all copyright, patent, trademark and attribution notices from the source form;
- **(d)** include the attribution notices from any upstream `NOTICE` file. Upstream ships no
  `NOTICE` file as of v1.2.0, so this file serves that role.

**Modifications made by Inoland.** The vendored tree under
`DepricatedModules/NeuTTS/vendor/` is an unmodified copy of upstream v1.2.0. The `.inv` and
`.uasset` voice files are Inoland-authored derivative works as described above;
`DepricatedModules/NeuTTS/scripts/build-voices.py` is Inoland-authored and is not part of the
upstream package.

### Removing the restriction entirely

NeuTTS is the only component in this repository with a commercial-use cap, and nothing else
depends on it. To make the whole plugin Apache-2.0-clean, delete:

```
Source/InoNeuTTS/
Source/InoNeuTTSEditor/
Content/NeuTTS/
DepricatedModules/NeuTTS/
DepricatedModules/InoNeuTtsNative/
DepricatedModules/InoNeuTtsNativeEditor/
```

and remove the `InoNeuTTS` and `InoNeuTTSEditor` entries from `InoAgents.uplugin`'s `Modules`
array. LiteRT-LM chat, ElevenLabs cloud TTS, and all animation / audio / camera helpers are
unaffected — the backends are independent deletion units by design.

---

## 2. LiteRT / LiteRT-LM — Google LLC

**License:** Apache-2.0. **Upstream:** https://github.com/google-ai-edge/LiteRT-LM

No copy of LiteRT or LiteRT-LM exists in this repository. The C APIs, runtime binaries, and
per-platform packaging are supplied by the sibling **InoLiteRT** plugin. Gemma model weights
download at runtime and are subject to Google's
[Gemma Terms of Use](https://ai.google.dev/gemma/terms).

---

## 3. eSpeak NG

**License:** GPL-3.0. **Upstream:** https://github.com/espeak-ng/espeak-ng

No copy exists in this repository. It is consumed through the sibling **InoSpeakNG** plugin
(itself GPL-3.0), and **the linkage is not dynamic on every platform**:

| Platform | Linkage | Artifact |
|---|---|---|
| Win64 | Dynamic | `espeak-ng.dll` |
| Android (arm64-v8a, x86_64) | Dynamic | `libespeak-ng.so` |
| **iOS** (arm64) | **Static** | `libespeak-ng.a` linked into the executable |
| **macOS** (universal) | **Static** | `libespeak-ng.a` linked into the executable |

> **⚠️ On iOS and macOS, eSpeak NG's object code is compiled into your shipped binary.** The
> "it's only dynamically linked, so the GPL doesn't reach my code" argument is not available to
> you there — and it is contested even on Win64 and Android. If you ship a closed-source
> commercial build, or target a console or locked-down store where GPL-3.0's anti-tivoization
> terms (§6) apply, get the obligations reviewed before shipping.

eSpeak NG is required **only** by the NeuTTS backend. LiteRT-LM chat, ElevenLabs cloud TTS, and
every animation / audio / camera helper work without it — and deleting the NeuTTS backend (see
§1, "Removing the restriction entirely") removes the eSpeak NG dependency at the same time,
which is what keeps InoAgents itself Apache-2.0.

Per-platform detail and the GPL-3.0 §6 corresponding-source pointer live in
[ino-espeak-ng-ue](https://github.com/nobandegani/ino-espeak-ng-ue#licensing).

---

## 4. RuntimeAudioImporter

**License:** MIT. **Upstream:** https://github.com/gtreshchev/RuntimeAudioImporter

No copy exists in this repository. It provides `UStreamingSoundWave`, the audio sink for TTS
output.

---

## Reporting a problem with these notices

If you believe a component is misattributed or a notice is missing, please open an issue at
https://github.com/nobandegani/ino-agents-ue/issues and it will be corrected.
