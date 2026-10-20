import assert from 'node:assert/strict';
import test from 'node:test';
import { readFile } from 'node:fs/promises';
import { screenAssets, screenMaterialPath } from '../tools/screen-assets.mjs';
import { worldTexturePath } from '../tools/world-texture.mjs';

test('bundled browser has both screen materials and every referenced texture', async()=>{
  const id='osfui.browser/tv';
  const root=new URL('../data/',import.meta.url);
  const manifest=JSON.parse(await readFile(new URL('SFSE/Plugins/OSF/UI/views/osfui.browser/tv/manifest.json',root)));
  assert.equal(manifest.texture,worldTexturePath(id));
  for(const profile of ['display-screen5','full-uv']) {
    for(const [path,expected] of screenAssets(id,profile)) {
      const actual=await readFile(new URL(path,root));
      if(path.endsWith('.mat')) assert.deepEqual(JSON.parse(actual),JSON.parse(expected));
      else assert.deepEqual(actual,expected);
    }
    const material=JSON.parse(await readFile(new URL(screenMaterialPath(id,profile),root)));
    for(const component of material.Objects.flatMap(o=>o.Components).filter(c=>c.Type==='BSMaterial::MRTextureFile')) {
      const bytes=await readFile(new URL(component.Data.FileName.replaceAll('\\','/'),root));
      assert.equal(bytes.toString('ascii',0,4),'DDS ');
    }
  }
});

test('screen materials have independent graphs and use their named texture',()=>{
  const ids=new Set();
  for (const view of ['alpha/screen','beta/screen','alpha/other']) {
    const assets=screenAssets(view);
    assert.deepEqual(assets,screenAssets(view));
    const material=JSON.parse(assets.get(screenMaterialPath(view)));
    const files=material.Objects.flatMap(o=>o.Components).filter(c=>c.Type==='BSMaterial::MRTextureFile');
    for(const slot of [0,7]) assert.equal(files.find(c=>c.Index===slot).Data.FileName.replaceAll('\\','/'),worldTexturePath(view));
    for(const object of material.Objects.filter(o=>o.ID)) { assert(!ids.has(object.ID)); ids.add(object.ID); }
    for(const file of files.filter(c=>![0,7].includes(c.Index))) {
      const bytes=assets.get(file.Data.FileName.replaceAll('\\','/'));
      assert.equal(bytes.toString('ascii',0,4),'DDS ');
      assert.equal(bytes.length,128+64*64*4);
    }
  }
});

test('DisplayScreen5 visible face maps to the full logical viewport',()=>{
  const material=JSON.parse(screenAssets('alpha/screen').get(screenMaterialPath('alpha/screen')));
  const uv=material.Objects.find(o=>o.Parent.endsWith('uvstreams.mat')).Components;
  const scale=uv.find(c=>c.Type==='BSMaterial::Scale').Data.Value.Data;
  const offset=uv.find(c=>c.Type==='BSMaterial::Offset').Data.Value.Data;
  // The inner face excludes the overlapping vanilla bezel and controls.
  const transform=(x,y)=>[x*Number(scale.x)+Number(offset.x),y*Number(scale.y)+Number(offset.y)];
  const topLeft=transform(0.075,0.025);
  const bottomRight=transform(0.985,0.49);
  assert(topLeft.every(n=>Math.abs(n)<1e-12));
  assert(bottomRight.every(n=>Math.abs(n-1)<1e-12));
  // This verifies asset math, not the game's shader evaluation or appearance.
});

test('authored full-UV screens share a texture without sharing the vanilla UV graph',()=>{
  const id='alpha/screen';
  const original=JSON.parse(screenAssets(id).get(screenMaterialPath(id)));
  const authored=JSON.parse(screenAssets(id,'full-uv').get(screenMaterialPath(id,'full-uv')));
  const originalIds=new Set(original.Objects.map(o=>o.ID).filter(Boolean));
  for(const object of authored.Objects.filter(o=>o.ID)) assert(!originalIds.has(object.ID));
  const uv=authored.Objects.find(o=>o.Parent.endsWith('uvstreams.mat')).Components;
  assert.deepEqual(uv.find(c=>c.Type==='BSMaterial::Scale').Data.Value.Data,{x:'1',y:'1'});
  assert.deepEqual(uv.find(c=>c.Type==='BSMaterial::Offset').Data.Value.Data,{x:'0',y:'0'});
  const texture=authored.Objects.flatMap(o=>o.Components).find(c=>c.Type==='BSMaterial::MRTextureFile' && c.Index===0);
  assert.equal(texture.Data.FileName.replaceAll('\\','/'),worldTexturePath(id));
});
