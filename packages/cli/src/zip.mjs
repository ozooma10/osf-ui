import { mkdir, open, readdir, readFile } from 'node:fs/promises';
import { dirname, isAbsolute, relative, resolve, sep } from 'node:path';

import { BUILD_MARKER } from './constants.mjs';

let table;
function crc32(buffer) {
  table ||= Array.from({ length: 256 }, (_, value) => {
    let crc = value;
    for (let bit = 0; bit < 8; bit++) crc = (crc >>> 1) ^ (crc & 1 ? 0xedb88320 : 0);
    return crc >>> 0;
  });
  let crc = 0xffffffff;
  for (const byte of buffer) crc = (crc >>> 8) ^ table[(crc ^ byte) & 0xff];
  return (crc ^ 0xffffffff) >>> 0;
}

async function walk(root) {
  const found = [];
  for (const entry of await readdir(root, { withFileTypes: true })) {
    const path = resolve(root, entry.name);
    if (entry.isDirectory()) found.push(...await walk(path));
    else found.push(path);
  }
  return found;
}

function header(signature, size) {
  const value = Buffer.alloc(size);
  value.writeUInt32LE(signature, 0);
  return value;
}

export async function writeZip(sourceRoot, destination) {
  sourceRoot = resolve(sourceRoot);
  destination = resolve(destination);
  const destinationRelative = relative(sourceRoot, destination);
  if (destinationRelative === '' ||
      (!isAbsolute(destinationRelative) &&
       destinationRelative !== '..' &&
       !destinationRelative.startsWith(`..${sep}`))) {
    throw new Error('Package output must be outside the directory being archived.');
  }
  await mkdir(dirname(destination), { recursive: true });
  const output = await open(destination, 'w');
  const central = [];
  let offset = 0;
  try {
    for (const path of await walk(sourceRoot)) {
      // Build bookkeeping, not mod content.
      if (relative(sourceRoot, path) === BUILD_MARKER) continue;
      const data = await readFile(path);
      const name = Buffer.from(relative(sourceRoot, path).replaceAll('\\', '/'));
      const crc = crc32(data);
      const local = header(0x04034b50, 30);
      local.writeUInt16LE(20, 4);
      local.writeUInt32LE(crc, 14);
      local.writeUInt32LE(data.length, 18);
      local.writeUInt32LE(data.length, 22);
      local.writeUInt16LE(name.length, 26);
      await output.write(Buffer.concat([local, name, data]));
      const directory = header(0x02014b50, 46);
      directory.writeUInt16LE(20, 4);
      directory.writeUInt16LE(20, 6);
      directory.writeUInt32LE(crc, 16);
      directory.writeUInt32LE(data.length, 20);
      directory.writeUInt32LE(data.length, 24);
      directory.writeUInt16LE(name.length, 28);
      directory.writeUInt32LE(offset, 42);
      central.push(Buffer.concat([directory, name]));
      offset += local.length + name.length + data.length;
    }
    const body = Buffer.concat(central);
    await output.write(body);
    const end = header(0x06054b50, 22);
    end.writeUInt16LE(central.length, 8);
    end.writeUInt16LE(central.length, 10);
    end.writeUInt32LE(body.length, 12);
    end.writeUInt32LE(offset, 16);
    await output.write(end);
  } finally {
    await output.close();
  }
  return destination;
}
