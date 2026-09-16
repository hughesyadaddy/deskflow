#!/usr/bin/env node
// Plain-node test for harness/swarm.workflow.js. No deps.
//
// The workflow script needs the Workflow runtime globals (args, agent,
// pipeline, ...) for everything below its RUNTIME marker, so this test
// extracts the pure prefix, writes it to a temp .mjs and imports it.
import assert from 'node:assert/strict'
import fs from 'node:fs'
import os from 'node:os'
import path from 'node:path'
import { fileURLToPath, pathToFileURL } from 'node:url'

const here = path.dirname(fileURLToPath(import.meta.url))
const root = path.resolve(here, '..', '..')
const scriptPath = path.join(root, 'harness', 'swarm.workflow.js')
const registryPath = path.join(root, 'harness', 'registry.json')

const source = fs.readFileSync(scriptPath, 'utf8')
assert.ok(source.startsWith('export const meta = {'), 'export const meta must be the first statement')

const marker = source.indexOf('// RUNTIME')
assert.ok(marker > 0, 'RUNTIME marker missing')
// Back up to the start of the comment banner line preceding the marker.
const cut = source.lastIndexOf('// ====', marker)
const pure = source.slice(0, cut)
assert.ok(!/\bawait agent\(|\bpipeline\(|\bphase\(/.test(pure), 'pure prefix must not use Workflow runtime hooks')

const tmp = fs.mkdtempSync(path.join(os.tmpdir(), 'swarm-test-'))
const modPath = path.join(tmp, 'swarm.pure.mjs')
fs.writeFileSync(modPath, pure)
const mod = await import(pathToFileURL(modPath).href)

const tests = []
const test = (name, fn) => tests.push({ name, fn })

test('meta is a pure literal with the three phases', () => {
  assert.equal(mod.meta.name, 'fleet-memory-swarm')
  assert.deepEqual(
    mod.meta.phases.map((p) => p.title),
    ['Audit', 'Adversary', 'PR'],
  )
  // meta must be a literal: no identifiers or calls inside its source text.
  const metaSrc = source.slice(0, source.indexOf('\n}\n') + 3)
  assert.ok(!/\$\{|\.\.\.|\w+\(/.test(metaSrc.replace(/'[^']*'/g, "''")), 'meta must be a pure literal')
})

test('assertDisjoint passes on the real registry', () => {
  const registry = JSON.parse(fs.readFileSync(registryPath, 'utf8'))
  assert.equal(registry.hotspots.length, 12)
  assert.equal(mod.assertDisjoint(registry.hotspots), true)
})

// Registry rows that are known to disagree with their driver header. Each
// entry is an expected failure with the JSON edit that would fix it; the
// assertion below still checks the row and reports when it starts passing so
// the entry can be deleted. Empty today: D6 -> epoch-flip, M6 -> window-toggle
// were repointed in the registry and mouse-absent-reconnect now declares
// `# proc: mouser` (M2).
const EXPECTED_REGISTRY_DRIVER_MISMATCHES = {
  // 'D6-osxscreen-polling': 'TODO(registry): set "scenario" to an automated deskflow-core row',
}

test('every registry hotspot points at a driver whose # proc: matches and is not manual', () => {
  const registry = JSON.parse(fs.readFileSync(registryPath, 'utf8'))
  const header = (driver, key) => {
    const m = driver.match(new RegExp(`^# ${key}:[ \\t]*(.*)$`, 'm'))
    return m ? m[1].trim() : ''
  }
  const problems = []
  const fixed = []
  for (const h of registry.hotspots) {
    const driverPath = path.join(root, 'harness', 'scenarios', `${h.scenario}.sh`)
    const errs = []
    if (!fs.existsSync(driverPath)) {
      errs.push(`scenario ${h.scenario} has no driver at harness/scenarios/${h.scenario}.sh`)
    } else {
      const driver = fs.readFileSync(driverPath, 'utf8')
      const proc = header(driver, 'proc')
      const automation = header(driver, 'automation')
      if (proc !== h.proc) errs.push(`registry proc ${h.proc} != driver '# proc: ${proc}' (${h.scenario}.sh)`)
      if (automation === 'manual') errs.push(`driver ${h.scenario}.sh is '# automation: manual'; the swarm cannot run it (exit 4)`)
      if (!['full', 'partial'].includes(automation) && automation !== 'manual') errs.push(`driver ${h.scenario}.sh has no valid '# automation:' header`)
    }
    const expected = EXPECTED_REGISTRY_DRIVER_MISMATCHES[h.id]
    if (errs.length && expected) {
      console.log(`  # expected failure ${h.id}: ${errs.join('; ')} -- ${expected}`)
    } else if (errs.length) {
      problems.push(`${h.id}: ${errs.join('; ')}`)
    } else if (expected) {
      fixed.push(h.id)
    }
  }
  assert.deepEqual(problems, [], `registry/driver mismatches:\n  ${problems.join('\n  ')}`)
  assert.deepEqual(fixed, [], `these rows now pass; remove them from EXPECTED_REGISTRY_DRIVER_MISMATCHES: ${fixed.join(', ')}`)
})

test('assertDisjoint throws on an overlapping fixture', () => {
  const fixture = [
    { id: 'A', repo: 'deskflow', files: ['src/a.cpp', 'src/shared.h'] },
    { id: 'B', repo: 'deskflow', files: ['src/b.cpp', 'src/shared.h'] },
  ]
  assert.throws(() => mod.assertDisjoint(fixture), /overlap.*src\/shared\.h.*A.*B/)
})

test('assertDisjoint allows the same path in different repos', () => {
  const fixture = [
    { id: 'A', repo: 'deskflow', files: ['README.md'] },
    { id: 'B', repo: 'Mouser', files: ['README.md'] },
  ]
  assert.equal(mod.assertDisjoint(fixture), true)
})

test('planFromRegistry filters by basename(repoPath) and has the dryRun shape', () => {
  const registry = JSON.parse(fs.readFileSync(registryPath, 'utf8'))
  const plan = mod.planFromRegistry(registry, '/Users/alexhughes/Desktop/deskflow')
  assert.equal(plan.disjoint, true)
  assert.equal(plan.repo, 'deskflow')
  assert.equal(plan.hotspots.length, 6)
  for (const h of plan.hotspots) {
    assert.deepEqual(Object.keys(h).slice(0, 5), ['id', 'branch', 'scenario', 'proc', 'files'])
    assert.equal(h.branch, `hotspot/${h.id}`)
    assert.ok(Array.isArray(h.files) && h.files.length > 0)
    assert.ok(typeof h.scenario === 'string' && typeof h.proc === 'string')
  }
  const mouser = mod.planFromRegistry(registry, '/Users/alexhughes/Desktop/Mouser/')
  assert.equal(mouser.hotspots.length, 6)
  assert.ok(mouser.hotspots.every((h) => h.id.startsWith('M')))
})

test('planFromRegistry on a two-entry fixture (Phase 5 acceptance)', () => {
  const fixture = {
    hotspots: [
      { id: 'X1', repo: 'deskflow', files: ['a.cpp'], scenario: 'epoch-flip', proc: 'deskflow-core', title: 't' },
      { id: 'X2', repo: 'deskflow', files: ['b.cpp'], scenario: 'screen-switch', proc: 'deskflow-core', title: 't' },
    ],
  }
  const plan = mod.planFromRegistry(fixture, '/x/deskflow')
  assert.deepEqual(plan.hotspots.map((h) => h.branch), ['hotspot/X1', 'hotspot/X2'])
  fixture.hotspots[1].files.push('a.cpp')
  assert.throws(() => mod.planFromRegistry(fixture, '/x/deskflow'), /overlap/)
})

test('resolveArgs applies the contract defaults', () => {
  const a = mod.resolveArgs({ repoPath: '/x/deskflow', registry: { hotspots: [] } })
  assert.equal(a.baseBranch, 'fleet/memory-program')
  assert.equal(a.hackintoshSsh, 'hackintosh')
  assert.equal(a.dryRun, false)
  assert.equal(a.postMerge, false)
  assert.throws(() => mod.resolveArgs({}), /repoPath/)
})

test('prompts carry the hackintosh ssh reproduction command and the template path', () => {
  const registry = JSON.parse(fs.readFileSync(registryPath, 'utf8'))
  const h = registry.hotspots[0]
  const a = mod.resolveArgs({ repoPath: '/x/deskflow', ghRepo: 'hughesyadaddy/deskflow', registry })
  const p = mod.auditorPrompt(h, a)
  assert.match(p, /harness\/prompts\/auditor\.md/)
  assert.match(p, new RegExp(`ssh hackintosh 'git -C <repo> fetch && git checkout hotspot/${h.id} && harness/run-scenario\\.sh ${h.scenario} --iters 1000'`))
  const finding = { id: 'f', evidence: { runId: 'r1', deltaMB: 3 }, fixClass: 'root-cause' }
  const q = mod.adversaryPrompt(h, a, finding)
  assert.match(q, /harness\/prompts\/adversary\.md/)
  assert.match(q, /findingRunId}} = r1/)
})

test('PR body carries run-id: and deltaMB: lines and the gh pr create flags', () => {
  const registry = JSON.parse(fs.readFileSync(registryPath, 'utf8'))
  const h = registry.hotspots[0]
  const a = mod.resolveArgs({ repoPath: '/x/deskflow', ghRepo: 'hughesyadaddy/deskflow', registry })
  const finding = { id: 'f', evidence: { runId: 'run-abc', deltaMB: 12.5 }, fixClass: 'mitigation', fileAtCommitLine: 'x@1:2', confidence: 0.9 }
  const verdict = { verdict: 'approve', deltaMB: 11, notes: 'ok' }
  const body = mod.prBody(h, finding, verdict, a)
  assert.match(body, /^run-id: run-abc$/m)
  assert.match(body, /^deltaMB: 11$/m)
  assert.match(body, /^mitigation: /m)
  const cmd = mod.prCreatePrompt(h, finding, verdict, a)
  assert.match(cmd, /gh pr create --repo hughesyadaddy\/deskflow --draft --base fleet\/memory-program --head hotspot\/D1-coordinator-blocking-connects --label needs-harness --title/)
  assert.match(cmd, /harness\/check-pr\.sh/)
})

test('whole script parses as an async Workflow body (top-level await/return)', () => {
  const body = source.replace(/^export\s+/gm, '')
  // AsyncFunction constructor: parses without executing.
  const AsyncFunction = Object.getPrototypeOf(async function () {}).constructor
  assert.doesNotThrow(() => new AsyncFunction('args', 'agent', 'pipeline', 'parallel', 'phase', 'log', body))
  assert.ok(!/Date\.now\(|Math\.random\(|new Date\(\)/.test(source), 'no Date.now/Math.random/new Date() in a workflow script')
})

test('isFinding: no run id => hypothesis', () => {
  assert.equal(mod.isFinding({ evidence: { runId: '', deltaMB: 1 } }), false)
  assert.equal(mod.isFinding({ evidence: { runId: 'r', deltaMB: 1 } }), true)
  assert.equal(mod.isFinding(null), false)
})

test('template placeholders in prompts/*.md are all bound', () => {
  const vars = new Set(
    Object.keys(
      mod.bindings({ id: 'i', files: [], scenario: 's', proc: 'p', evidence: { file: 'f', line: 1, token: 't' } }, mod.resolveArgs({ repoPath: '/x/deskflow', registry: {} }), {
        findingJson: '',
        findingRunId: '',
        findingDeltaMB: '',
        fixClass: '',
      }),
    ),
  )
  for (const f of ['auditor.md', 'adversary.md']) {
    const md = fs.readFileSync(path.join(root, 'harness', 'prompts', f), 'utf8')
    const used = new Set([...md.matchAll(/\{\{(\w+)\}\}/g)].map((m) => m[1]))
    for (const u of used) assert.ok(vars.has(u), `${f} uses unbound placeholder {{${u}}}`)
    assert.ok(used.has('id') && used.has('scenario') && used.has('hackintoshSsh'), `${f} must reference id/scenario/hackintoshSsh`)
  }
})

let failed = 0
for (const t of tests) {
  try {
    await t.fn()
    console.log(`ok - ${t.name}`)
  } catch (err) {
    failed++
    console.log(`not ok - ${t.name}\n  ${String(err && err.stack ? err.stack : err).split('\n').join('\n  ')}`)
  }
}
fs.rmSync(tmp, { recursive: true, force: true })
console.log(`${tests.length - failed}/${tests.length} passed`)
process.exit(failed ? 1 : 0)
