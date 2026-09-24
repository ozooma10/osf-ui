import { createHash } from 'node:crypto';

export function worldTexturePath(qualifiedId) {
  const digest = createHash('sha256').update(qualifiedId, 'utf8').digest('hex');
  return `textures/osfui/feeds/${digest.slice(0, 32)}/${digest.slice(32)}.dds`;
}

// All feeds intentionally have identical dimensions and pixels. Only the asset
// path identifies them. The engine owns these normal one-mip BGRA8 resources.
export function worldPlaceholder() {
  const size = 64;
  const dds = Buffer.alloc(128 + size * size * 4);
  dds.write('DDS ', 0, 'ascii');
  const fields = {4:124,8:0x100f,12:size,16:size,20:size*4,76:32,80:0x41,
    88:32,92:0x00ff0000,96:0x0000ff00,100:0x000000ff,104:0xff000000,108:0x1000};
  for (const [offset, value] of Object.entries(fields)) dds.writeUInt32LE(value, Number(offset));
  for (let offset=128; offset<dds.length; offset+=4) dds.writeUInt32LE(0xff000000, offset);
  return dds;
}
