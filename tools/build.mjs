import {spawnSync} from 'node:child_process';
import {existsSync} from 'node:fs';
import {join} from 'node:path';
import {root} from './prepare.mjs';

const esp = join(root, 'build/sdk/esp32');
if (!existsSync(join(root, 'build/sdk/receipt.json'))) throw Error('Run prepare:muse with a pack first');
function run(program, args) {
  const result = spawnSync(program, args, {stdio: 'inherit'});
  if (result.error) throw result.error;
  if (result.status !== 0) process.exit(result.status || 1);
}
if (process.argv[2] === 'simulator') {
  const output = join(root, 'build/simulator');
  run('cmake', ['-S', join(esp, 'simulator'), '-B', output, '-DMUSE_SIM_WARNINGS_AS_ERRORS=ON']);
  run('cmake', ['--build', output, '--parallel', '4']);
} else if (process.argv[2] === 'firmware' || process.argv[2] === 'configure') {
  const output = join(root, 'build/firmware');
  const defaults = ['sdkconfig.defaults', 'devices/sdkconfig.muse',
    'devices/sdkconfig.muse-waveshare-s3-175c', 'sdkconfig.aipets'].map(p => join(esp, p)).join(';');
  run('idf.py', ['-C', esp, '-B', output, '-DIDF_TARGET=esp32s3',
    `-DSDKCONFIG=${join(output, 'sdkconfig')}`, `-DSDKCONFIG_DEFAULTS=${defaults}`,
    process.argv[2] === 'configure' ? 'menuconfig' : 'build']);
} else {
  throw Error('Usage: node tools/build.mjs simulator|firmware|configure');
}
