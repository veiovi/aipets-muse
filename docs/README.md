# Integration

`sprite-aipets → .aipetframes → canonical frame_player → Muse avatar → LVGL`

Muse pin: `693cde9a884ad1edc87251b9f8944815f8de4809`.
Compiler: `0.3.0-sprite.3`, source commit
`fb571d522d5421a0000b28689c2b8b628cd653f2`, archive SHA-256
`51b2b42b16b929c8ee60b355774751b6a47e77152f5591f1bf2bce8bbe4c07e7`.
C and WASM come from that distribution. No build reads a sibling checkout or
requires the original private device repository.

`vendor/muse-dependencies.lock` fixes the resolved ESP-IDF component versions
and hashes for this board. Preparation copies it into the generated SDK.

## Runtime boundary

`integration/muse_pixel.c` implements Muse's renderer/strip contract. It owns
one player arena (ESP32 PSRAM) and passes state/audio/touch. Muse retains its
LCD/audio/radio/UI drivers and mode-dependent accent colours.

| Muse input | Player input |
|---|---|
| Boot/listening/thinking/speaking/error | Corresponding state |
| Idle + Link online | Natural idle |
| Idle + Link unpaired/pairing/confirm | Provisioning |
| Idle + Link connecting/offline/error | Corresponding state |
| Idle + Link boot | Booting |
| Powering off | Offline; Muse owns shutdown |
| Audio level | Clamped/rounded 0–100 mouth amplitude |
| Rising happiness | One touch event; decay does not retrigger |

Voice modes override Link status. Speech poses, blinks, effects and idle remain
inside the canonical director. Reply-driven emotion selection is not implemented.

Each refresh accumulates 33 ms ticks; the initial frame advances once. A gap
longer than 132 ms discards backlog and resumes with one tick, bounding work
after sleep or hidden UI. Muse's 64-cell dirty sampling would miss some native
pixel changes; we invalidate the whole avatar only on visual revision changes.
This trades display bandwidth for simplicity and covers all intermediate ticks.
Native RGB565 uses existing parallel strip decoding; pack binding happens on the
UI thread before strip readers run.

## Preparation and storage

`tools/prepare.mjs` validates a core pack with C/WASM, verifies dependency hashes,
exports pinned SDK sources, copies the unchanged C component and applies the
UI/CMake integration. Repeated preparation preserves build settings and replaces
integration-owned generated sources. Unrecognized output directories are rejected.

Original 4 MiB app slots/data partitions stay intact. Added `aipet` data subtype
is `0x42`, offset `0x830000`, size `0x300000`; the import cap is 3,000,000 bytes.
Firmware carries exact expected pack length/hash, so a different pack requires
preparation and rebuild. No filesystem, downloader or switching UI is added.
Pack failures display an error. Upstream OTA is off because it would remove
the adapter.

`configure:firmware` selects the same build directory/configuration as
`build:firmware`. A CMake guard rejects an existing configuration with Muse OTA
enabled, because saved settings otherwise override defaults.
If that guard fires before the configuration menu opens, set only
`CONFIG_HOMEHUB_OTA_ENABLED=n` in the ignored `build/firmware/sdkconfig` and
rebuild; preserve the remaining settings and credentials.

`build/sdk/receipt.json` records pins, pack identity/dimensions/hash/bytes, target
and partition, with `hardwareVerified: false`. Keep it with generated firmware
and review before any authorized write. Existing AI Pets firmware has a different
layout; unchanged Muse offsets do not imply safe migration.

## Verification

Host ASan/UBSan tests use real C and 120px raw / 240px zlib diagnostic packs:
modes/precedence, audio levels, touch edges, 33 ms timing under 40 ms refresh,
bounded resume, scaling/stride boundaries, malformed inputs and startup failures.
Preparation tests check actual artifacts and rejection behavior.

The six curated packs in `characters/catalog.json` are hash-checked and exercised
through the real C adapter under ASan/UBSan. Each authored speaking pose must have
distinct mouth drawings. For every level from 0 through 100, Muse strip pixels
must equal the canonical player, every declared mouth stage must be reached,
and the actual mouth-region pixels must differ between stages. Silence while
still in SPEAKING must return to the exact closed mouth. The test saves PPM
frames in `build/mouth-checks/`; `characters/mouth-checks.png` shows those pixels.
This proves the imported packages and adapter, not a live TTS service.

The full simulator compiles production Muse UI with the adapter; upstream
headless tests check deterministic scenes. Firmware must compile on ESP-IDF6.0.1
with asset offset and disabled OTA verified. These checks do not prove device
timing, pairing, audio or memory headroom.
