import assert from 'node:assert/strict';
import {execFileSync, spawnSync} from 'node:child_process';
import {mkdtempSync, readFileSync, writeFileSync} from 'node:fs';
import {tmpdir} from 'node:os';
import {join} from 'node:path';
import test from 'node:test';
import {prepare, root} from '../tools/prepare.mjs';
import {writeFixtures} from './fixtures.mjs';

test('Muse adapter preserves canonical pixels, input semantics and timing', () => {
  const packs = writeFixtures(join(root, 'build/test-packs'));
  const runtime = join(root, 'node_modules/@aipet/frame-pack/source/firmware/components/frame_player');
  const executable = join(root, 'build/avatar_test');
  execFileSync(process.env.CC || 'cc', ['-std=c11', '-O1', '-g', '-Wall', '-Wextra',
    '-fsanitize=address,undefined', '-fno-omit-frame-pointer',
    '-I', join(root, 'integration'), '-I', join(root, 'upstream/muse/esp32/components/muse'),
    '-I', join(runtime, 'include'), join(root, 'tests/avatar_test.c'),
    join(root, 'integration/muse_pixel.c'),
    ...['frame_player.c', 'frame_director.c', 'frame_actions.c', 'frame_display.c', 'frame_codec.c']
      .map(file => join(runtime, 'src', file)),
    join(runtime, 'vendor/miniz/miniz_tinfl.c'), '-lm', '-o', executable], {stdio: 'pipe'});
  const result = spawnSync(executable, packs, {encoding: 'utf8',
    env: {...process.env, ASAN_OPTIONS: `detect_leaks=${process.platform === 'darwin' ? 0 : 1}:halt_on_error=1`}});
  assert.equal(result.status, 0, result.stdout + result.stderr);
  console.log(result.stdout.trim());
});

test('prepare validates packs and stages the pinned SDK without changing source', () => {
  const parent = mkdtempSync(join(tmpdir(), 'aipets-muse-'));
  const packs = writeFixtures(parent);
  const output = join(parent, 'sdk');
  for (const path of packs) {
    const receipt = prepare(path, output);
    assert.equal(receipt.pack.bytes, readFileSync(path).length);
    assert.deepEqual(readFileSync(join(output, 'pet.aipetframes')), readFileSync(path));
    assert.equal(receipt.pack.width, Number(receipt.pack.id.split('-')[1]));
    assert.deepEqual(readFileSync(join(output, 'esp32/dependencies.lock')),
      readFileSync(join(root, 'vendor/muse-dependencies.lock')));
  }
  assert.doesNotThrow(() => prepare(join(output, 'pet.aipetframes'), output));
  const corrupt = join(parent, 'corrupt.aipetframes');
  const bad = Buffer.from(readFileSync(packs[1]));
  bad[bad.length - 1] ^= 0xff;
  writeFileSync(corrupt, bad);
  assert.throws(() => prepare(corrupt, output), /CHECKSUM|checksum/);
  const large = join(parent, 'large.aipetframes');
  writeFileSync(large, Buffer.alloc(3_000_001));
  assert.throws(() => prepare(large, output), /exceeds/);
  assert.throws(() => prepare(packs[0], parent), /not this pinned generated SDK/);
  assert.equal(execFileSync('git', ['-C', join(root, 'upstream/muse'), 'status', '--porcelain']).length, 0);
});
