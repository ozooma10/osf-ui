import { spawn } from 'node:child_process';
import { mkdir, readFile, rm } from 'node:fs/promises';
import { delimiter, dirname, relative, resolve, sep } from 'node:path';

import { LOCAL_FILE } from './constants.mjs';
import { exists, latestMtime, pexFor } from './fsutil.mjs';
import { pscFiles } from './papyrus.mjs';

async function localSettings(project) {
  try {
    return JSON.parse(await readFile(resolve(project.root, LOCAL_FILE), 'utf8'));
  } catch {
    return {};
  }
}

function configuredPath(project, value) {
  if (typeof value !== 'string' || !value.trim()) return null;
  return resolve(project.root, value);
}

async function commandOnPath(names) {
  const extensions = process.platform === 'win32'
    ? (process.env.PATHEXT || '.EXE;.CMD;.BAT').split(';')
    : [''];
  for (const directory of (process.env.PATH || '').split(delimiter).filter(Boolean)) {
    for (const name of names) {
      const hasExtension = /\.[a-z0-9]+$/i.test(name);
      for (const extension of hasExtension ? [''] : extensions) {
        const candidate = resolve(directory, name + extension.toLowerCase());
        if (await exists(candidate)) return candidate;
        if (extension) {
          const upper = resolve(directory, name + extension.toUpperCase());
          if (await exists(upper)) return upper;
        }
      }
    }
  }
  return null;
}

async function firstExisting(candidates) {
  for (const candidate of candidates.filter(Boolean)) {
    if (await exists(candidate)) return candidate;
  }
  return null;
}

async function firstPapyrusSource(candidates) {
  for (const candidate of candidates.filter(Boolean)) {
    if (await exists(resolve(candidate, 'Quest.psc')) &&
        await exists(resolve(candidate, 'Starfield_Papyrus_Flags.flg'))) {
      return candidate;
    }
  }
  return null;
}

function steamStarfieldRoots() {
  if (process.platform !== 'win32') return [];
  const programFilesX86 = process.env['ProgramFiles(x86)'] || 'C:\\Program Files (x86)';
  return [
    resolve(programFilesX86, 'Steam/steamapps/common/Starfield'),
    'C:\\XboxGames\\Starfield\\Content',
  ];
}

export async function papyrusToolchain(project) {
  const local = await localSettings(project);
  const starfieldRoots = [
    configuredPath(project, local.starfieldRoot),
    configuredPath(project, process.env.STARFIELD_ROOT),
    configuredPath(project, process.env.STARFIELD_PATH),
    ...steamStarfieldRoots(),
  ].filter(Boolean);

  const spriggitCli = await firstExisting([
    configuredPath(project, local.spriggitCli),
    configuredPath(project, process.env.SPRIGGIT_CLI),
    resolve(project.root, 'tools/Spriggit.CLI.exe'),
  ]) || await commandOnPath(['Spriggit.CLI.exe', 'Spriggit.CLI', 'spriggit']);

  const papyrusCompiler = await firstExisting([
    configuredPath(project, local.papyrusCompiler),
    configuredPath(project, process.env.PAPYRUS_COMPILER),
    ...starfieldRoots.map((root) => resolve(root, 'Tools/Papyrus Compiler/PapyrusCompiler.exe')),
  ]) || await commandOnPath(['PapyrusCompiler.exe', 'PapyrusCompiler']);

  const explicitImports = [
    configuredPath(project, local.papyrusImports),
    configuredPath(project, process.env.PAPYRUS_IMPORTS),
  ].filter(Boolean);
  let papyrusImports = await firstPapyrusSource(explicitImports);
  if (!papyrusImports) {
    papyrusImports = await firstPapyrusSource([
      ...starfieldRoots.map((root) => resolve(root, 'Data/Scripts/Source')),
      resolve(project.root, '.osfui/papyrus-ck/Scripts/Source'),
    ]);
  }

  const contentResources = await firstExisting(
    starfieldRoots.map((root) => resolve(root, 'Tools/ContentResources.zip')),
  );
  const archiveTool = papyrusImports
    ? null
    : await commandOnPath(process.platform === 'win32' ? ['tar.exe', 'tar'] : ['tar']);
  const papyrusApi = resolve(project.root, 'tools/papyrus/OSFUI.psc');
  return {
    spriggitCli,
    papyrusCompiler,
    papyrusImports,
    contentResources,
    archiveTool,
    papyrusApi: await exists(papyrusApi) ? papyrusApi : null,
  };
}

function run(executable, args, options = {}) {
  return new Promise((resolvePromise, reject) => {
    const child = spawn(executable, args, {
      cwd: options.cwd,
      stdio: options.quiet ? 'pipe' : 'inherit',
      windowsHide: true,
    });
    let stderr = '';
    if (options.quiet) child.stdout?.on('data', () => {});
    if (options.quiet) child.stderr?.on('data', (chunk) => { stderr += chunk; });
    child.once('error', reject);
    child.once('exit', (code) => {
      if (code === 0) resolvePromise();
      else reject(new Error(
        `${options.label || executable} exited with ${code ?? 'no status'}${stderr ? `: ${stderr.trim()}` : ''}`,
      ));
    });
  });
}

async function ensureImports(project, tools) {
  if (tools.papyrusImports) return tools.papyrusImports;
  if (!tools.contentResources) {
    throw new Error(
      'Creation Kit script sources are missing. Install the Starfield Creation Kit, or set papyrusImports in .osfui/local.json.',
    );
  }
  const cache = resolve(project.root, '.osfui/papyrus-ck');
  const source = resolve(cache, 'Scripts/Source');
  await mkdir(cache, { recursive: true });
  console.log('[osfui] Extracting Creation Kit Papyrus sources (one time)...');
  if (!tools.archiveTool) {
    throw new Error('Could not find tar, which is required to unpack Creation Kit ContentResources.zip.');
  }
  await run(tools.archiveTool, ['-xf', tools.contentResources, '-C', cache, 'Scripts/Source'], {
    label: 'Creation Kit source extraction',
  });
  if (!await exists(resolve(source, 'Quest.psc'))) {
    throw new Error('Creation Kit ContentResources.zip did not contain Scripts/Source/Quest.psc.');
  }
  return source;
}

function requiredTools(tools) {
  const missing = [];
  if (!tools.spriggitCli) missing.push('Spriggit CLI');
  if (!tools.papyrusCompiler) missing.push('Creation Kit Papyrus compiler');
  if (!tools.papyrusImports && !tools.contentResources) missing.push('Creation Kit Papyrus script sources');
  if (!tools.papyrusImports && tools.contentResources && !tools.archiveTool) missing.push('tar archive extractor');
  if (!tools.papyrusApi) missing.push('the generated OSFUI.psc compiler API');
  return missing;
}

export async function doctorPapyrus(project) {
  if (!project.papyrus) return [];
  const tools = await papyrusToolchain(project);
  const rows = [
    ['Spriggit CLI', tools.spriggitCli],
    ['Papyrus compiler', tools.papyrusCompiler],
    ['Creation Kit sources', tools.papyrusImports || tools.contentResources],
    ...(!tools.papyrusImports ? [['Source archive extractor', tools.archiveTool]] : []),
    ['OSF UI Papyrus API', tools.papyrusApi],
  ];
  for (const [label, path] of rows) {
    console.log(`[osfui] ${path ? 'OK' : 'MISSING'} ${label}${path ? `: ${path}` : ''}`);
  }
  const missing = requiredTools(tools);
  if (missing.includes('Spriggit CLI')) {
    console.log('[osfui] Install Spriggit CLI from https://github.com/Mutagen-Modding/Spriggit/releases');
  }
  if (missing.some((item) => item.startsWith('Creation Kit'))) {
    console.log('[osfui] Install Starfield Creation Kit in Steam (Library > Tools).');
  }
  if (missing.length) {
    console.log(`[osfui] Nonstandard paths can be set in ${LOCAL_FILE}; see this project's README.`);
  }
  return missing;
}

export async function buildPapyrus(project, options = {}) {
  if (!project.papyrus) return { pluginBuilt: false, scriptsBuilt: 0 };
  const tools = await papyrusToolchain(project);
  const missing = requiredTools(tools);
  if (missing.length) {
    throw new Error(`Papyrus toolchain incomplete (${missing.join(', ')}). Run "npm run doctor" for setup help.`);
  }

  let pluginBuilt = false;
  if (await latestMtime(project.papyrus.sourceDir) > await latestMtime(project.papyrus.outputPath)) {
    await mkdir(dirname(project.papyrus.outputPath), { recursive: true });
    console.log(`[osfui] Generating ${project.papyrus.plugin} with Spriggit...`);
    await run(tools.spriggitCli, [
      'deserialize',
      '--InputPath', project.papyrus.sourceDir,
      '--OutputPath', project.papyrus.outputPath,
    ], { cwd: project.root, quiet: options.quiet, label: 'Spriggit' });
    if (!await exists(project.papyrus.outputPath)) {
      throw new Error(`Spriggit reported success but did not create ${project.papyrus.outputPath}.`);
    }
    pluginBuilt = true;
  }

  const imports = await ensureImports(project, tools);
  const sourceRoot = resolve(project.modRoot, 'Scripts/Source');
  const sources = await pscFiles(sourceRoot);
  const apiMtime = await latestMtime(tools.papyrusApi);
  let scriptsBuilt = 0;
  for (const psc of sources) {
    const pex = pexFor(project.modRoot, psc);
    if (Math.max(await latestMtime(psc), apiMtime) <= await latestMtime(pex)) continue;
    const rel = relative(sourceRoot, psc);
    const compilerObject = rel.toLowerCase().startsWith(`user${sep}`)
      ? rel.slice(`user${sep}`.length)
      : rel;
    await mkdir(dirname(pex), { recursive: true });
    await rm(pex, { force: true });
    console.log(`[osfui] Compiling ${rel.replaceAll(sep, '/')}...`);
    const importPaths = [
      resolve(sourceRoot, 'User'),
      sourceRoot,
      dirname(tools.papyrusApi),
      imports,
    ];
    await run(tools.papyrusCompiler, [
      compilerObject,
      `-output=${resolve(project.modRoot, 'Scripts')}`,
      `-import=${importPaths.join(';')}`,
      `-flags=${resolve(imports, 'Starfield_Papyrus_Flags.flg')}`,
      '-release',
      '-ignorecwd',
    ], { cwd: sourceRoot, quiet: options.quiet, label: 'Papyrus compiler' });
    if (!await exists(pex)) {
      throw new Error(`Papyrus compilation failed to create ${pex}. Review the compiler errors above.`);
    }
    scriptsBuilt++;
  }
  return { pluginBuilt, scriptsBuilt };
}
