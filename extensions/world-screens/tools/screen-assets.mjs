import { createHash } from 'node:crypto';
import { worldTexturePath, worldPlaceholder } from './world-texture.mjs';

// DisplayScreen5 / screens025:0, vanilla 1.16.244. The bezel and left
// controls overlap the textured quad: fit the page inside the visible face.
// Mesh UV bounds alone clip content behind those separate geometries.
const bounds = [0.075, 0.025, 0.985, 0.49];
export function screenMaterialPath(id, profile = 'display-screen5') {
  if (!['display-screen5', 'full-uv'].includes(profile)) throw new Error(`Unsupported screen profile: ${profile}`);
  return worldTexturePath(id).replace('textures/osfui/feeds/', `materials/osfui/screens/${profile}/v1/`).replace(/\.dds$/, '.mat');
}

export function screenAssets(id, profile = 'display-screen5') {
  const face = profile === 'full-uv' ? [0, 0, 1, 1] : bounds;
  const texture = worldTexturePath(id);
  const folder = texture.slice(0, texture.lastIndexOf('/'));
  const resource = (kind) => {
    const h = createHash('sha256').update(`osfui/screen/v1/${profile}/${id}/${kind}`).digest();
    const hex = (n) => (n >>> 0).toString(16).padStart(8, '0').toUpperCase();
    return `res:${hex(h.readUInt32LE(0))}:${hex((h.readUInt32LE(4) & 0x3ffff) | 0x40000)}:${hex((h.readUInt32LE(8) & 0x7ffffff) | 0xa0000000)}`;
  };
  const component = (kind, Data, Index = 0) => ({ Type: `BSMaterial::${kind}`, Index, Data });
  const object = (kind, Components) => ({ ID: resource(kind), Parent: `materials\\layered\\root\\${kind}.mat`, Components });
  const value2 = (x,y) => ({Value:{Type:'XMFLOAT2',Data:{x:String(x), y:String(y)}}});
  const sx = 1/(face[2]-face[0]), sy = 1/(face[3]-face[1]);
  const slots = {0:texture,1:`${folder}/normal.dds`,3:`${folder}/rough.dds`,4:`${folder}/black.dds`,5:`${folder}/white.dds`,7:texture};
  const material = {Version:1,Objects:[
    {Parent:'materials\\layered\\root\\layeredmaterials.mat',Components:[
      component('LayerID',{ID:resource('layers')}),
      component('ShaderModelComponent',{FileName:'ColorEmissive'}),
      component('EmissiveSettingsComponent',{Enabled:'true',Settings:{Type:'BSMaterial::EmittanceSettings',Data:{
        LuminousEmittance:'300.0',AdaptiveEmittance:'true',ExposureOffset:'5.5',EmissiveClipThreshold:'0.0',
        EmissiveTint:{Type:'BSMaterial::Color',Data:{Value:{Type:'XMFLOAT4',Data:{x:'1.0',y:'1.0',z:'1.0',w:'1.0'}}}}
      }}})]},
    object('layers',[component('MaterialID',{ID:resource('materials')}),component('UVStreamID',{ID:resource('uvstreams')})]),
    object('uvstreams',[component('Scale',value2(sx,sy)),component('Offset',value2(-face[0]*sx,-face[1]*sy))]),
    object('materials',[component('TextureSetID',{ID:resource('texturesets')})]),
    object('texturesets',Object.entries(slots).map(([slot,path])=>component('MRTextureFile',{FileName:path.replaceAll('/','\\')},Number(slot))))
  ]};
  const files = new Map([[screenMaterialPath(id, profile), Buffer.from(JSON.stringify(material,null,2)+'\n')]]);
  for (const [name,color] of Object.entries({normal:0xff8080ff,rough:0xffbebebe,black:0xff000000,white:0xffffffff})) {
    const dds=worldPlaceholder();
    for(let offset=128;offset<dds.length;offset+=4)dds.writeUInt32LE(color,offset);
    files.set(`${folder}/${name}.dds`,dds);
  }
  return files;
}
