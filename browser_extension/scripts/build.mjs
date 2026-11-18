import {copyFileSync, mkdirSync, readdirSync} from 'node:fs';
import {fileURLToPath} from 'node:url';
import path from 'node:path';

const root = path.resolve(path.dirname(fileURLToPath(import.meta.url)), '..');
const scripts = readdirSync(path.join(root, 'src')).filter(name => name.endsWith('.ts'))
  .map(name => name.slice(0, -3) + '.js');
for (const [directory, manifest] of [['dist', 'manifest.json'], ['dist-firefox', 'firefox.json']]) {
  const destination = path.join(root, directory);
  mkdirSync(destination, {recursive: true});
  for (const name of ['popup.html', 'popup.css'])
    copyFileSync(path.join(root, 'src', name), path.join(destination, name));
  if (directory !== 'dist') for (const name of scripts)
    copyFileSync(path.join(root, 'dist', name), path.join(destination, name));
  copyFileSync(path.join(root, 'manifests', manifest), path.join(destination, 'manifest.json'));
}
