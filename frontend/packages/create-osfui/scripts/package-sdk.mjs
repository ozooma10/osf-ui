import { access, cp, mkdir, rm } from 'node:fs/promises';
import { dirname, resolve } from 'node:path';
import { fileURLToPath } from 'node:url';

const packageRoot = resolve(dirname(fileURLToPath(import.meta.url)), '..');
const repositoryRoot = resolve(packageRoot, '..', '..', '..');
const outputRoot = resolve(packageRoot, 'package-sdk');
const files = [
  ['sdk', 'native', 'OSFUI_Views.h'],
  ['data/Scripts/Source', 'papyrus', 'OSFUI.psc'],
  ['data/Scripts/Source', 'papyrus', 'OSFUI_View.psc'],
];

const repositorySourcesExist = (await Promise.all(files.map(async ([sourceDirectory, , name]) => {
  try {
    await access(resolve(repositoryRoot, sourceDirectory, name));
    return true;
  } catch (error) {
    if (error.code === 'ENOENT') return false;
    throw error;
  }
}))).every(Boolean);

if (process.argv.slice(2).includes('--clean')) {
  // Only a repository pack creates this directory temporarily. A repack of an
  // installed package must retain its sole SDK payload.
  if (repositorySourcesExist) await rm(outputRoot, { recursive: true, force: true });
} else if (repositorySourcesExist) {
  await rm(outputRoot, { recursive: true, force: true });
  await Promise.all(files.map(async ([sourceDirectory, outputDirectory, name]) => {
    const destination = resolve(outputRoot, outputDirectory, name);
    await mkdir(dirname(destination), { recursive: true });
    await cp(resolve(repositoryRoot, sourceDirectory, name), destination);
  }));
} else {
  // npm may repack an installed copy. In that case package-sdk is already the
  // canonical payload embedded by the original repository pack.
  await Promise.all(files.map(([, outputDirectory, name]) =>
    access(resolve(outputRoot, outputDirectory, name))));
}
