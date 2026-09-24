import assert from 'node:assert/strict';
import { mkdir, mkdtemp, rm, writeFile } from 'node:fs/promises';
import { tmpdir } from 'node:os';
import { resolve } from 'node:path';
import test from 'node:test';

import { loadProject, manifestFor } from '../src/config.mjs';

async function fixture(t, view) {
  const root = await mkdtemp(resolve(tmpdir(), 'osfui-world-manifest-'));
  t.after(() => rm(root, { recursive: true, force: true }));
  const source = resolve(root, 'src/views/example.mod/screen');
  await mkdir(source, { recursive: true });
  await writeFile(resolve(source, 'index.html'), '<p>World display</p>');
  await writeFile(resolve(root, 'osfui.config.mjs'),
    `export default ${JSON.stringify({ modId: 'example.mod', view: { id: 'screen', ...view } })};`);
  return root;
}


import { readFile } from 'node:fs/promises';
import { buildProject } from '../src/build.mjs';
import { worldTexturePath, worldPlaceholder } from '../src/world-texture.mjs';

test('64 feeds get stable unique asset paths at identical dimensions', async (t) => {
  const root = await fixture(t, { kind: 'world' });
  const views = [];
  for (let i=0; i<64; ++i) {
    const id = `board-${i}`;
    const source = resolve(root, `src/views/example.mod/${id}`);
    await mkdir(source, { recursive:true });
    await writeFile(resolve(source, 'index.html'), '<p>Independent feed</p>');
    views.push({id, kind:'world', width:1000, height:1000});
  }
  const save = () => writeFile(resolve(root, 'osfui.config.mjs'), `export default ${JSON.stringify({modId:'example.mod',views})};`);
  await save();
  const first = await loadProject(root, 'build');
  assert.equal(new Set(first.views.map(v=>v.texture)).size,64);
  views.reverse(); await save();
  const second = await loadProject(root, 'build');
  for (const view of second.views) assert.equal(view.texture, first.views.find(v=>v.id===view.id).texture);
  assert.notEqual(worldTexturePath('market/prices'),worldTexturePath('another-mod/prices'));
  assert.notEqual(worldTexturePath('Market/prices'),worldTexturePath('market/prices'));
});

test('build packages the declared DDS asset and passive opaque world manifest', async(t)=>{
  const root = await fixture(t, {kind:'world',width:1000,height:1000,transparent:true,capturesInput:true,pausesGame:true,openOnStart:true});
  const project = await loadProject(root,'build');
  await buildProject(project,{quiet:true});
  const view=project.views[0];
  const manifest=JSON.parse(await readFile(resolve(project.outputViewsRoot,'example.mod/screen/manifest.json'),'utf8'));
  assert.equal(manifest.texture, worldTexturePath('example.mod/screen'));
  assert.equal(manifest.transparent,false); assert.equal(manifest.capturesInput,false);
  assert.equal(manifest.pausesGame,false); assert.equal(manifest.openOnStart,false);
  assert.equal('placeholderSize' in manifest,false);
  const dds=await readFile(resolve(project.outDir,'Data',manifest.texture));
  assert.deepEqual(dds,worldPlaceholder());
  assert.equal(dds.readUInt32LE(12),64); assert.equal(dds.readUInt32LE(16),64);
  assert.equal(dds.readUInt32LE(128),0xff000000);
});

test('rejects legacy dimension binding and attempts to claim another asset',async(t)=>{
  for(const authored of [{placeholderSize:1000},{texture:worldTexturePath('other/feed')}]) {
    const root=await fixture(t,{kind:'world',...authored});
    await assert.rejects(loadProject(root,'build'),/generated texture binding/);
  }
  for(const key of ['width','height']) for(const size of [null,true,'900',0,-1,1.5,4097,4294968196]) {
    const root=await fixture(t,{kind:'world',[key]:size});
    await assert.rejects(loadProject(root,'build'),new RegExp(key));
  }
});

test('defaults and boundary dimensions do not change texture identity',async(t)=>{
  const root=await fixture(t,{kind:'world'});
  const project=await loadProject(root,'build');
  assert.equal(project.views[0].width,1600); assert.equal(project.views[0].height,900);
  const edge=await fixture(t,{kind:'world',width:1,height:4096});
  assert.equal((await loadProject(edge,'build')).views[0].texture,project.views[0].texture);
  assert.equal(manifestFor(project.views[0]).texture,worldTexturePath('example.mod/screen'));
});
