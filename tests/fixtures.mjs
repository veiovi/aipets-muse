import {mkdirSync, writeFileSync} from 'node:fs';
import {join, resolve} from 'node:path';
import {deflateSync} from 'node:zlib';
import {fileURLToPath} from 'node:url';
import {writeFramePack} from '../node_modules/@aipet/frame-pack/dist/binary.js';

// Diagnostic pixels, not character artwork or a visually approved pet.
export function fixture(canvas) {
  const palette = [[8, 16, 24], [248, 64, 64], [64, 248, 64], [64, 64, 248],
    [248, 248, 64], [248, 64, 248], [64, 248, 248], [248, 248, 248]];
  const frames = Array.from({length: 16}, (_, frame) => {
    const data = Uint8Array.from({length: canvas * canvas}, (_, pixel) =>
      ((pixel % canvas) + Math.floor(pixel / canvas) * 3 + frame) % palette.length);
    return canvas === 240 ? {codec: 5, data: deflateSync(data)} : data;
  });
  frames.push({codec: 1, data: new Uint8Array([23, 37, 2, 2, 15, 1, 3, 5, 7])});
  const clips = [
    {name: 'neutral', loopMode: 2, steps: [{frame: 0, durationTicks: 1}]},
    {name: 'idle', loopMode: 1, steps: [{frame: 1, durationTicks: 2}, {frame: 0, durationTicks: 3}]},
    ...[10, 11, 12, 13, 14].map((frame, i) =>
      ({name: `state-${i}`, loopMode: 1, steps: [{frame, durationTicks: 2}, {frame: 15, durationTicks: 2}]})),
    {name: 'touch', loopMode: 0, steps: [{frame: 16, durationTicks: 8}]}
  ];
  const action = {exitClip: 65535, expressionMask: 32767, profileMask: 14, priority: 100,
    essential: true, intervalMinTicks: 0, intervalMaxTicks: 0, cooldownTicks: 0};
  return writeFramePack({canvas, maxBytes: canvas === 120 ? 0x2a0000 : 3_000_000, id: `diagnostic-${canvas}`,
    name: 'Diagnostic fixture', version: '1.0.0', gender: 0, approved: false,
    palette, bgPaletteIndex: 0, frames, clips, roles: [0, 65535, 2, 3, 4, 5, 6, 65535], emotions: [],
    talk: {stageFrames: [0, 2, 3, 4, 5, 6, 7, 8],
      levelToStage: Uint8Array.from({length: 101}, (_, level) => Math.min(7, Math.floor(level * 8 / 101)))},
    idle: {bobAmplitudePx: 0, bobPeriodTicks: 0, microGapMinTicks: 200,
      microGapMaxTicks: 400, micro: [], fallbacksEnabled: false},
    actions: [
      {...action, id: 'idle', semantic: 'signature', plane: 'base', activationKind: 'ambient-loop',
        clip: 1, stateMask: 1 << 3},
      {...action, id: 'touch', semantic: 'touch', plane: 'touchFx', activationKind: 'semantic',
        clip: 7, stateMask: 0x1ff}
    ], meta: {diagnostic: true}});
}

export function writeFixtures(directory) {
  mkdirSync(directory, {recursive: true});
  return [120, 240].map(canvas => {
    const path = join(directory, `${canvas}.aipetframes`);
    writeFileSync(path, fixture(canvas));
    return path;
  });
}

if (process.argv[1] && resolve(process.argv[1]) === fileURLToPath(import.meta.url)) {
  console.log(writeFixtures(resolve('build/test-packs')).join('\n'));
}
