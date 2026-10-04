import {createHash} from 'node:crypto';
import {cpSync, existsSync, mkdirSync, readFileSync, writeFileSync} from 'node:fs';
import {dirname, join, resolve} from 'node:path';
import {fileURLToPath} from 'node:url';
import {execFileSync} from 'node:child_process';
import {parseFramePack, WasmFramePlayer} from '@aipet/frame-pack';

export const root = resolve(dirname(fileURLToPath(import.meta.url)), '..');
const upstream = join(root, 'upstream/muse');
const compiler = join(root, 'node_modules/@aipet/frame-pack');
export const sdkCommit = '693cde9a884ad1edc87251b9f8944815f8de4809';
const compilerHash = '51b2b42b16b929c8ee60b355774751b6a47e77152f5591f1bf2bce8bbe4c07e7';
const sha = bytes => createHash('sha256').update(bytes).digest('hex');
const read = path => readFileSync(path, 'utf8');
const git = (...args) => execFileSync('git', ['-C', upstream, ...args], {maxBuffer: 64 * 1024 * 1024});

function replace(source, before, after) {
  if (!source.includes(before) || source.indexOf(before) !== source.lastIndexOf(before)) {
    throw Error(`Pinned SDK integration anchor changed: ${before.slice(0, 70)}`);
  }
  return source.replace(before, after);
}

export function prepare(packPath, output = join(root, 'build/sdk')) {
  const bytes = readFileSync(packPath);
  if (bytes.length > 3_000_000) throw Error('Pack exceeds the 3,000,000-byte skill limit');
  const pack = parseFramePack(bytes);
  const wasm = readFileSync(join(compiler, 'dist/frame_player.wasm'));
  // Binding also rejects move-only packs: this adapter requires a complete core pet.
  WasmFramePlayer.create(wasm, bytes, 1);
  if (sha(readFileSync(join(root, 'vendor/frame-pack-0.3.0-sprite.3.tgz'))) !== compilerHash) {
    throw Error('Compiler archive hash mismatch');
  }
  const provenance = JSON.parse(read(join(compiler, 'compiler-provenance.json')));
  if (sha(wasm) !== provenance.portableWasmSha256) throw Error('Installed WASM hash mismatch');
  const runtime = join(compiler, 'source/firmware/components/frame_player');
  for (const [path, expected] of Object.entries(provenance.sources)) {
    if (path.startsWith('firmware/components/frame_player/') && sha(readFileSync(join(compiler, 'source', path))) !== expected) {
      throw Error(`Canonical runtime hash mismatch: ${path}`);
    }
  }
  if (git('rev-parse', 'HEAD').toString().trim() !== sdkCommit) throw Error('Wrong Muse submodule commit');
  const marker = join(output, '.aipets-generated');
  if (existsSync(output) && (!existsSync(marker) || read(marker).trim() !== sdkCommit)) {
    throw Error('Output is not this pinned generated SDK; choose a new output directory');
  }
  mkdirSync(output, {recursive: true});
  if (!existsSync(marker)) {
    execFileSync('tar', ['-x', '-C', output], {input: git('archive', sdkCommit, 'esp32', 'LICENSE')});
    writeFileSync(marker, sdkCommit + '\n');
  }
  const esp = join(output, 'esp32');
  cpSync(join(root, 'vendor/muse-dependencies.lock'), join(esp, 'dependencies.lock'));
  function patch(path, transform) {
    writeFileSync(join(output, path), transform(git('show', `${sdkCommit}:${path}`).toString()));
  }
  cpSync(runtime, join(esp, 'components/frame_player'), {recursive: true});
  writeFileSync(join(esp, 'components/frame_player/CMakeLists.txt'), `idf_component_register(
    SRCS "src/frame_player.c" "src/frame_director.c" "src/frame_actions.c"
         "src/frame_display.c" "src/frame_codec.c" "vendor/miniz/miniz_tinfl.c"
    INCLUDE_DIRS "include")
`);
  const avatar = join(esp, 'components/muse/avatar');
  mkdirSync(avatar, {recursive: true});
  for (const name of ['muse_pixel.c', 'aipet_avatar.h']) cpSync(join(root, 'integration', name), join(avatar, name));
  writeFileSync(join(avatar, 'aipet_pack_config.h'), `#pragma once
#define AIPET_PACK_BYTES ${bytes.length}u
#define AIPET_PACK_SHA256 "${sha(bytes)}"
`);
  writeFileSync(join(output, 'pet.aipetframes'), bytes);

  patch('esp32/components/muse/CMakeLists.txt', s => replace(s, 'noise_core minimp3', 'noise_core minimp3 frame_player esp_partition'));
  patch('esp32/components/muse/muse_pixel.h', s => s + '\n/* Canonical player presentation revision, including all native pixels. */\nuint32_t muse_pixel_revision(void);\n');
  patch('esp32/components/muse/muse_ui.c', s => {
    s = replace(s, '#include "muse_pixel.h"', '#include "muse_pixel.h"\n#include "avatar/aipet_avatar.h"');
    s = replace(s, 'static void muse_image_init(void)\n{', `static void muse_image_init(void)
{
    /* Bind on the UI thread before parallel LVGL strip readers can run. */
    muse_pixel_render(&(muse_pose_t){.mode = MUSE_MODE_BOOT});`);
    s = replace(s, '    muse_pixel_render(&pose);', `    fp_sys_state_t idle_state = FP_SYS_IDLE;
    switch (muse_link_state()) {
    case MUSE_LINK_BOOT:
        idle_state = FP_SYS_BOOTING;
        break;
    case MUSE_LINK_UNPAIRED:
    case MUSE_LINK_PAIRING:
    case MUSE_LINK_CONFIRM:
        idle_state = FP_SYS_PROVISIONING;
        break;
    case MUSE_LINK_CONNECTING:
        idle_state = FP_SYS_CONNECTING;
        break;
    case MUSE_LINK_OFFLINE:
        idle_state = FP_SYS_OFFLINE;
        break;
    case MUSE_LINK_ERROR:
        idle_state = FP_SYS_ERROR;
        break;
    default:
        break;
    }
    aipet_avatar_set_idle_state(idle_state);
    muse_pixel_render(&pose);`);
    const start = s.indexOf('#define MUSE_PIXEL_MAX_PX');
    const end = s.indexOf('static lv_result_t muse_dec_info', start);
    if (start < 0 || end < 0) throw Error('Missing avatar decoder declarations');
    s = s.slice(0, start) + `static lv_image_dsc_t s_muse_src;
static lv_draw_buf_t *s_strips[STRIPS];
static bool s_strip_busy[STRIPS];

` + s.slice(end);
    const allocation = s.indexOf('    s_cells = heap_caps_malloc');
    const next = s.indexOf('static lv_obj_t *make_label', allocation);
    if (allocation < 0 || next < 0) throw Error('Missing avatar invalidation block');
    s = s.slice(0, allocation) + `}

/* A 64-cell sample misses native 120/240px changes; invalidate on player revision.
 * A full avatar redraw also covers all ticks accumulated between presentations. */
static void invalidate_muse(void)
{
    static uint32_t shown;
    static bool valid;
    uint32_t revision = muse_pixel_revision();
    if (!valid || revision != shown) {
        lv_obj_invalidate(s_canvas);
        shown = revision;
        valid = true;
    }
}

` + s.slice(next);
    return s;
  });
  patch('esp32/simulator/CMakeLists.txt', s => {
    s = replace(s, '"${MUSE_COMPONENT_DIR}/../../avatar/muse_pixel.c"', '"${MUSE_COMPONENT_DIR}/avatar/muse_pixel.c"');
    return s + `
set(FRAME_PLAYER "\${MUSE_COMPONENT_DIR}/../frame_player")
target_sources(muse_simulator PRIVATE
    "\${FRAME_PLAYER}/src/frame_player.c" "\${FRAME_PLAYER}/src/frame_director.c"
    "\${FRAME_PLAYER}/src/frame_actions.c" "\${FRAME_PLAYER}/src/frame_display.c"
    "\${FRAME_PLAYER}/src/frame_codec.c" "\${FRAME_PLAYER}/vendor/miniz/miniz_tinfl.c")
target_include_directories(muse_simulator PRIVATE "\${FRAME_PLAYER}/include")
`;
  });
  patch('esp32/partitions_muse.csv', s => s + '\n# Immutable core pack, separate from both application OTA slots.\naipet, data, 0x42, 0x830000, 0x300000,\n');
  patch('esp32/CMakeLists.txt', s => s + `
if(CONFIG_HOMEHUB_OTA_ENABLED)
    message(FATAL_ERROR "AI Pets requires Muse OTA disabled; use npm run configure:firmware to disable it.")
endif()
esptool_py_flash_to_partition(flash "aipet" "\${CMAKE_CURRENT_LIST_DIR}/../pet.aipetframes")
`);
  writeFileSync(join(esp, 'sdkconfig.aipets'), '# Keep the custom renderer: upstream OTA would replace it.\nCONFIG_HOMEHUB_OTA_ENABLED=n\n');
  const receipt = {museCommit: sdkCommit, compilerVersion: provenance.version, compilerCommit: provenance.commit,
    compilerSha256: compilerHash,
    firmwareDependenciesSha256: sha(readFileSync(join(root, 'vendor/muse-dependencies.lock'))),
    pack: {file: 'pet.aipetframes', sha256: sha(bytes), bytes: bytes.length,
      id: pack.info.id, width: pack.info.width, height: pack.info.height},
    board: 'waveshare-s3-175c', partition: {name: 'aipet', offset: '0x830000', bytes: 0x300000},
    hardwareVerified: false};
  writeFileSync(join(output, 'receipt.json'), JSON.stringify(receipt, null, 2) + '\n');
  return receipt;
}

if (process.argv[1] && resolve(process.argv[1]) === fileURLToPath(import.meta.url)) {
  if (process.argv.length !== 3) throw Error('Usage: npm run prepare:muse -- /path/to/pet.aipetframes');
  console.log(JSON.stringify(prepare(resolve(process.argv[2])), null, 2));
}
