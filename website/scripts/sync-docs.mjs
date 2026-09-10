// Generate website/docs/ from the READMEs that already exist in the repo.
//
// The READMEs are the single source of truth: they have to read well when
// browsing the repo on GitHub, and duplicating them into a docs/ folder would
// guarantee the two copies drift. So this runs before every build (and in CI)
// and rewrites what needs rewriting:
//
//   - links to other .md files  -> Docusaurus routes
//   - links to source files     -> GitHub blob URLs (more useful than a 404)
//   - <img src="preview.svg">   -> a copy under static/img, width preserved
//
// docs/ and static/img/ are generated and gitignored. Never edit them.
import { cpSync, mkdirSync, readFileSync, readdirSync, rmSync, statSync, writeFileSync } from 'node:fs'
import { dirname, join, relative, resolve } from 'node:path'

const REPO = resolve(import.meta.dirname, '../..')
const OUT = resolve(REPO, 'website/docs')
const IMG = resolve(REPO, 'website/static/img')
const GITHUB = 'https://github.com/rtorcato/ESP32-apps/blob/main'
const BASE_URL = '/ESP32-apps/'

// Devices in the order they should appear in the sidebar: owned first.
const DEVICES = [
  'esp32-c6-lcd-1.47',
  'esp32-2432s028r-cyd',
  'esp32-s3-touch-lcd-7',
  'elecrow-rotary-2.1',
  'elecrow-rotary-1.28',
]

// repo-relative markdown path -> { route, out, title, position }
const pages = new Map()

const titleOf = (md) => (md.match(/^#\s+(.+)$/m)?.[1] ?? 'Untitled').replace(/\*\*/g, '').trim()

function addPage(src, out, route, position) {
  pages.set(src, { out, route, position })
}

addPage('README.md', 'index.md', '/', 1)
addPage('DEVICES.md', 'devices.md', '/devices', 2)
addPage('SECURITY.md', 'security.md', '/security', 3)

DEVICES.forEach((device, i) => {
  addPage(`${device}/README.md`, `${device}/index.md`, `/${device}/`, 10 + i * 10)
  const appsDir = join(REPO, device, 'apps')
  let apps = []
  try {
    apps = readdirSync(appsDir).filter((a) => statSync(join(appsDir, a)).isDirectory())
  } catch {
    return
  }
  // Built apps first, then alphabetical -- the ones with code are the interesting ones.
  const isBuilt = (a) => {
    try {
      return readFileSync(join(appsDir, a, 'README.md'), 'utf8').includes('**built**')
    } catch {
      return false
    }
  }
  apps.sort((a, b) => isBuilt(b) - isBuilt(a) || a.localeCompare(b))
  apps.forEach((app, j) =>
    addPage(
      `${device}/apps/${app}/README.md`,
      `${device}/${app}.md`,
      `/${device}/${app}`,
      11 + i * 10 + j * 0.01,
    ),
  )
})

// ── link rewriting ───────────────────────────────────────────────────────
// Resolve a relative link against the file it appears in, then decide whether
// it lands on a generated page or on a source file in the repo.
function rewriteTarget(target, srcPath) {
  if (/^(https?:|mailto:|#)/.test(target)) return target

  const [pathPart, hash = ''] = target.split('#')
  const anchor = hash ? `#${hash}` : ''
  if (!pathPart) return target

  const abs = resolve(dirname(join(REPO, srcPath)), pathPart)
  let repoRel = relative(REPO, abs).split('\\').join('/')

  // A directory link means that directory's README.
  const asReadme = repoRel.replace(/\/$/, '') + '/README.md'
  if (pages.has(repoRel)) return pages.get(repoRel).route + anchor
  if (pages.has(asReadme)) return pages.get(asReadme).route + anchor

  // Anything else is a real file in the repo -- link to GitHub rather than
  // leaving a dead relative path.
  return `${GITHUB}/${repoRel}${anchor}`
}

function rewrite(md, srcPath, outPath) {
  // Markdown links, skipping images (handled below).
  md = md.replace(/(?<!!)\[([^\]]*)\]\(([^)\s]+)\)/g, (m, text, target) => {
    return `[${text}](${rewriteTarget(target, srcPath)})`
  })

  // <img src="preview.svg" ...> -- copy the asset and point at it, keeping the
  // width attribute, which matters because these SVGs are drawn at 2x.
  md = md.replace(/<img\s+src="([^"]+)"([^>]*)>/g, (m, src, rest) => {
    if (/^https?:/.test(src)) return m
    const abs = resolve(dirname(join(REPO, srcPath)), src)
    const repoRel = relative(REPO, abs).split('\\').join('/')
    const dest = join(IMG, repoRel)
    try {
      mkdirSync(dirname(dest), { recursive: true })
      cpSync(abs, dest)
    } catch {
      return m // asset missing; leave the tag alone rather than breaking the page
    }
    return `<img src="${BASE_URL}img/${repoRel}"${rest}>`
  })

  return md
}

// ── generate ─────────────────────────────────────────────────────────────
rmSync(OUT, { recursive: true, force: true })
rmSync(IMG, { recursive: true, force: true })

let n = 0
for (const [src, { out, position }] of pages) {
  let md
  try {
    md = readFileSync(join(REPO, src), 'utf8')
  } catch {
    console.warn(`skip (missing): ${src}`)
    continue
  }

  const title = titleOf(md)
  // Strip the H1: Docusaurus renders the frontmatter title as the page heading,
  // so leaving it would show the title twice.
  const body = rewrite(md.replace(/^#\s+.+\n/, ''), src, out)

  const fm = [
    '---',
    `title: ${JSON.stringify(title)}`,
    `sidebar_position: ${position}`,
    out === 'index.md' ? 'slug: /' : null,
    `description: ${JSON.stringify(`Generated from ${src}`)}`,
    '---',
    '',
    `{/* Generated from ${src} -- edit that file, not this one. */}`,
    '',
  ]
    .filter(Boolean)
    .join('\n')

  const dest = join(OUT, out)
  mkdirSync(dirname(dest), { recursive: true })
  writeFileSync(dest, fm + body)
  n++
}

// A category label for each device folder, so the sidebar reads properly.
DEVICES.forEach((device, i) => {
  const dir = join(OUT, device)
  try {
    statSync(dir)
  } catch {
    return
  }
  writeFileSync(
    join(dir, '_category_.json'),
    JSON.stringify(
      { label: device, position: 10 + i * 10, link: { type: 'doc', id: `${device}/index` } },
      null,
      2,
    ),
  )
})

console.log(`sync-docs: wrote ${n} pages from repo READMEs`)
