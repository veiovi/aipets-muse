# AI Pets for Muse

Private integration of our `.aipetframes` packages into the
[Muse ESP32 SDK](https://github.com/facebookincubator/muse-gadget-sdk/tree/main/esp32).
Codex's **sprite-aipets** skill remains the authoring workflow. Muse supplies
its device UI and connection; our canonical C player supplies the animated pet.

## Use a pack

Requires Node.js 22+, npm, Python 3, Git, CMake 3.24+, and a C/C++ compiler.
The simulator supports macOS and Linux and fetches pinned LVGL/SDL.
Create and visually review a pet with the installed `sprite-aipets` skill,
then import its delivered pack:

```sh
git clone --recurse-submodules https://github.com/veiovi/aipets-muse.git
cd aipets-muse
npm ci --ignore-scripts
npm run prepare:muse -- /absolute/path/to/pet.aipetframes
npm run build:simulator
AIPET_PACK="$PWD/build/sdk/pet.aipetframes" ./build/simulator/muse_simulator
```

The simulator runs Muse's production UI. Use its
[keyboard controls](upstream/muse/esp32/simulator/README.md) to change states,
audio level and touch/happiness. It is a native desktop preview, not a website.

Preparation validates with the canonical C/WASM player, verifies pinned runtime
sources, and produces an isolated SDK under `build/sdk/` with a SHA-256 receipt.
The upstream submodule stays unchanged. Packs, firmware and credentials stay ignored.

## Compatibility

- Complete **120×120 v1 and 240×240 v2** core packs, up to **3,000,000 bytes**
  (v1 retains its smaller canonical format limit). Zlib resources, speech poses,
  layered effects, idle motion and touch use the existing player unchanged.
- Compatible core packs from `fal-h3-aipet` use the same file contract; its
  authoring tools are not bundled. Standalone move packs cannot replace a core pet.
- Firmware target: **Waveshare ESP32-S3-Touch-AMOLED-1.75C**, 32 MB flash and
  8 MB PSRAM. Other boards need separate memory/layout/build verification.
- Muse's 40 ms refresh feeds fixed 33 ms player ticks. Native pixels are scaled
  into RGB565 strips without a 64×64 intermediate.

## Firmware

With **ESP-IDF 6.0.1** installed and exported, `npm run build:firmware` builds
without opening a device. The pack occupies a separate 3 MiB data partition at
`0x830000`; Muse's original application/settings offsets stay intact. Firmware
checks exact length, SHA-256 and canonical validation. The generated flash target
includes both firmware and matching pack. Upstream OTA is disabled to prevent
it replacing the custom renderer.

Building is not permission to flash. Migrating an existing AI Pets device to
Muse changes its firmware and partition layout: follow the release workflow,
identify hardware, hold its lease, preserve private settings, and verify an
interaction. Never flash a `.aipetframes` file at address zero.

Pairing needs the user's Muse SDK token and app. Set credentials only in ignored
build configuration through `npm run configure:firmware`; simulator and compile-only
checks need none. Muse currently returns **text replies**; spoken replies need
TTS integration. Actual playback audio levels already drive our mouth stages.

## Verify

```sh
npm test
# Tests create diagnostic packs, not character artwork.
npm run prepare:muse -- build/test-packs/240.aipetframes
npm run build:simulator
AIPET_PACK="$PWD/build/sdk/pet.aipetframes" ctest --test-dir build/simulator --output-on-failure
```

Host tests compare with the canonical C player using ASan/UBSan. CI also builds
and runs Muse's headless simulator checks. Hardware timing, PSRAM headroom,
pairing and live speech require physical acceptance.
See [integration details](docs/README.md).

## Source and service terms

Muse source is Apache-2.0; its default Jollybot artwork is excluded from that
license and is not our avatar. The compiler/runtime archive is the MIT-licensed
`sprite-aipets` distribution; miniz notices remain inside it. See
[compiler license](vendor/LICENSE.frame-pack) and [Muse license](upstream/muse/LICENSE).

Muse service access separately follows its
[SDK token terms](https://gadgets.muse.ai/sdk-terms), including personal,
non-commercial use and restricted device distribution. A private repository
does not change those service terms.
