// Сборка: esbuild для main / preload / renderer + генерация реестра эффектов.
// Новый эффект = папка src/effects/<id>/ с manifest.json и index.ts — реестр
// собирается автоматически, правок в этом файле не требуется.
import * as esbuild from 'esbuild'
import * as fs from 'node:fs'
import * as path from 'node:path'
import { execFileSync } from 'node:child_process'
import { fileURLToPath } from 'node:url'

const root = path.dirname(fileURLToPath(import.meta.url))
const dist = path.join(root, 'dist')
const effectsDir = path.join(root, 'src/effects')

fs.rmSync(dist, { recursive: true, force: true })
fs.mkdirSync(path.join(dist, 'renderer'), { recursive: true })

// --- реестр эффектов -------------------------------------------------------
const effectIds = fs
  .readdirSync(effectsDir, { withFileTypes: true })
  .filter((d) => d.isDirectory())
  .map((d) => d.name)
  .filter(
    (id) =>
      fs.existsSync(path.join(effectsDir, id, 'index.ts')) &&
      fs.existsSync(path.join(effectsDir, id, 'manifest.json')),
  )

if (effectIds.length === 0) {
  console.error('нет ни одного эффекта в src/effects')
  process.exit(1)
}

const manifests = effectIds.map((id) =>
  JSON.parse(fs.readFileSync(path.join(effectsDir, id, 'manifest.json'), 'utf8')),
)

const generated = [
  '// файл сгенерирован build.mjs — не редактировать',
  ...effectIds.map(
    (id, i) => `import * as m${i} from ${JSON.stringify(path.join(effectsDir, id, 'index.ts'))}`,
  ),
  `export const EFFECTS: { manifest: any; module: any }[] = [`,
  ...effectIds.map(
    (id, i) => `  { manifest: ${JSON.stringify(manifests[i])}, module: m${i} },`,
  ),
  `]`,
  '',
].join('\n')
fs.writeFileSync(path.join(root, 'src/renderer/.effects.generated.ts'), generated)

// манифесты нужны и главному процессу (трей + автогенерация настроек)
for (const id of effectIds) {
  const dir = path.join(dist, 'effects', id)
  fs.mkdirSync(dir, { recursive: true })
  fs.copyFileSync(path.join(effectsDir, id, 'manifest.json'), path.join(dir, 'manifest.json'))
}

// --- нативный трекер окон (macOS) ------------------------------------------
// Компилируем здесь, а не в make-app.sh: dist полностью пересоздаётся, и без этого
// dev-запуск оставался без хелпера.
fs.mkdirSync(path.join(dist, 'bin'), { recursive: true })
try {
  execFileSync(
    'swiftc',
    ['-O', path.join(root, 'scripts/windowtracker.swift'), '-o', path.join(dist, 'bin/windowtracker')],
    { stdio: 'inherit' },
  )
  console.log('трекер окон: собран')
} catch (error) {
  console.warn('трекер окон не собран:', error.message)
}

// --- статика ---------------------------------------------------------------
fs.copyFileSync(path.join(root, 'src/renderer/index.html'), path.join(dist, 'renderer/index.html'))
fs.copyFileSync(path.join(root, 'src/settings/settings.html'), path.join(dist, 'settings.html'))

const common = { bundle: true, logLevel: 'info', sourcemap: false, target: 'es2022' }

await Promise.all([
  esbuild.build({
    ...common,
    entryPoints: [path.join(root, 'src/main/main.ts')],
    outfile: path.join(dist, 'main.js'),
    platform: 'node',
    format: 'cjs',
    external: ['electron'],
  }),
  esbuild.build({
    ...common,
    entryPoints: [path.join(root, 'src/preload/preload.ts')],
    outfile: path.join(dist, 'preload.js'),
    platform: 'node',
    format: 'cjs',
    external: ['electron'],
  }),
  esbuild.build({
    ...common,
    entryPoints: [path.join(root, 'src/renderer/renderer.ts')],
    outfile: path.join(dist, 'renderer/renderer.js'),
    platform: 'browser',
    format: 'iife',
  }),
  esbuild.build({
    ...common,
    entryPoints: [path.join(root, 'src/settings/settings.ts')],
    outfile: path.join(dist, 'settings.js'),
    platform: 'browser',
    format: 'iife',
  }),
])

console.log(`эффекты: ${effectIds.join(', ')}`)
