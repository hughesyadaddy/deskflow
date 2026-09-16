export const meta = {
  name: 'fleet-memory-swarm',
  description: 'Per-hotspot auditor -> adversary -> draft PR swarm for the fleet memory program',
  whenToUse: 'Phase 6/7 of the fleet memory program: one hotspot/<id> branch and PR per registry entry',
  phases: [
    { title: 'Audit', detail: 'one auditor per hotspot: reproduce on hackintosh, fix on hotspot/<id>, add tests' },
    { title: 'Adversary', detail: 'independent rerun + compile matrix + behavior bench; approve|veto' },
    { title: 'PR', detail: 'gh pr create --draft --label needs-harness on approve; --post-merge rebases hotspot/*' },
  ],
}

// ============================================================================
// Pure helpers. Everything above the RUNTIME marker is side-effect free and
// uses no Workflow globals, so tools/tests/test_swarm_workflow.js can extract
// and import it under plain node. Keep it that way.
// ============================================================================

export const DEFAULTS = Object.freeze({
  baseBranch: 'fleet/memory-program',
  hackintoshSsh: 'hackintosh',
  dryRun: false,
  postMerge: false,
})

export const FINDING_SCHEMA = {
  type: 'object',
  properties: {
    id: { type: 'string', description: 'finding id, e.g. <hotspot>-f1' },
    hotspot: { type: 'string', description: 'registry hotspot id' },
    seat: { type: 'string', description: 'seat the scenario ran on (hackintosh)' },
    scenarioRow: { type: 'string', description: 'harness/scenarios row name' },
    fileAtCommitLine: {
      type: 'string',
      description: 'file@commit:line citation of the root cause, e.g. src/lib/x.cpp@abc1234:263',
    },
    evidence: {
      type: 'object',
      properties: {
        runId: {
          type: 'string',
          description: 'run id from harness/run-scenario.sh (harness/runs/<row>.jsonl header). Empty string if no run happened.',
        },
        deltaMB: { type: 'number', description: 'MB recovered: before-slope minus after-slope over 1000 iters' },
        beforeRunId: { type: 'string' },
        afterRunId: { type: 'string' },
      },
      required: ['runId', 'deltaMB'],
    },
    confidence: { type: 'number', description: '0..1' },
    fixClass: { type: 'string', enum: ['root-cause', 'mitigation'] },
    branch: { type: 'string', description: 'hotspot/<id>' },
    commit: { type: 'string', description: 'HEAD sha of the pushed hotspot branch' },
    testsAdded: { type: 'array', items: { type: 'string' } },
    notes: { type: 'string' },
  },
  required: ['id', 'hotspot', 'seat', 'scenarioRow', 'fileAtCommitLine', 'evidence', 'confidence', 'fixClass'],
}

export const VERDICT_SCHEMA = {
  type: 'object',
  properties: {
    verdict: { type: 'string', enum: ['approve', 'veto'] },
    deltaMB: { type: 'number', description: 'adversary-measured MB recovered on hackintosh' },
    runId: { type: 'string', description: 'adversary rerun id (after)' },
    beforeRunId: { type: 'string' },
    compileMatrix: {
      type: 'object',
      properties: {
        macosArm64: { type: 'boolean' },
        macosX86_64: { type: 'boolean' },
        windowsMsvc: { type: 'boolean' },
      },
    },
    behaviorBenchWithin10pct: { type: 'boolean' },
    notes: { type: 'string' },
  },
  required: ['verdict', 'deltaMB', 'notes'],
}

export const PR_SCHEMA = {
  type: 'object',
  properties: {
    prUrl: { type: 'string' },
    prNumber: { type: 'number' },
    checkPrExit: { type: 'number', description: 'exit code of harness/check-pr.sh <pr-number>' },
    notes: { type: 'string' },
  },
  required: ['prUrl'],
}

export const BRANCHES_SCHEMA = {
  type: 'object',
  properties: { branches: { type: 'array', items: { type: 'string' } } },
  required: ['branches'],
}

export const REBASE_SCHEMA = {
  type: 'object',
  properties: {
    branch: { type: 'string' },
    rebased: { type: 'boolean' },
    conflicts: { type: 'boolean' },
    compileMatrixGreen: { type: 'boolean' },
    notes: { type: 'string' },
  },
  required: ['branch', 'rebased', 'compileMatrixGreen'],
}

export const REGISTRY_SCHEMA = {
  type: 'object',
  properties: {
    registry: {
      type: 'object',
      properties: { hotspots: { type: 'array', items: { type: 'object' } } },
      required: ['hotspots'],
    },
  },
  required: ['registry'],
}

export function basename(p) {
  const s = String(p).replace(/[\\/]+$/, '')
  const i = Math.max(s.lastIndexOf('/'), s.lastIndexOf('\\'))
  return i < 0 ? s : s.slice(i + 1)
}

export function branchFor(id) {
  return `hotspot/${id}`
}

/**
 * Throws unless every hotspot's files[] is pairwise disjoint with every other
 * hotspot's files[] *within the same repo*. Returns true on success.
 */
export function assertDisjoint(hotspots) {
  const byRepo = new Map()
  for (const h of hotspots) {
    if (!h || typeof h.id !== 'string') throw new Error('registry hotspot missing id')
    if (!Array.isArray(h.files)) throw new Error(`hotspot ${h.id}: files[] missing`)
    const repo = h.repo || ''
    if (!byRepo.has(repo)) byRepo.set(repo, new Map())
    const owners = byRepo.get(repo)
    for (const f of h.files) {
      const norm = String(f).replace(/^\.\//, '')
      const prev = owners.get(norm)
      if (prev && prev !== h.id) {
        throw new Error(`registry files[] overlap in repo "${repo}": ${norm} owned by ${prev} and ${h.id}`)
      }
      owners.set(norm, h.id)
    }
  }
  return true
}

export function hotspotsForRepo(registry, repoPath) {
  if (!registry || !Array.isArray(registry.hotspots)) throw new Error('registry has no hotspots[]')
  const repo = basename(repoPath)
  return registry.hotspots.filter((h) => h.repo === repo)
}

export function resolveArgs(raw) {
  const a = Object.assign({}, DEFAULTS, raw || {})
  if (!a.repoPath) throw new Error('args.repoPath is required')
  if (!a.registry) throw new Error('args.registry is required (parsed registry object, or an absolute path)')
  return a
}

/**
 * Pure plan: filters the registry to the repo, asserts disjointness across the
 * whole registry, returns {hotspots:[{id, branch, scenario, proc, files}], disjoint:true}.
 */
export function planFromRegistry(registry, repoPath) {
  assertDisjoint(registry.hotspots)
  const hotspots = hotspotsForRepo(registry, repoPath).map((h) => ({
    id: h.id,
    branch: branchFor(h.id),
    scenario: h.scenario,
    proc: h.proc,
    files: [...h.files],
    seat: h.seat,
    budgetMB: h.budgetMB,
    evidence: h.evidence,
    title: h.title,
  }))
  return { repo: basename(repoPath), hotspots, disjoint: true }
}

export function fill(template, vars) {
  return String(template).replace(/\{\{(\w+)\}\}/g, (m, k) => (k in vars ? String(vars[k]) : m))
}

export function bindings(h, a, extra) {
  return Object.assign(
    {
      id: h.id,
      title: h.title || '',
      files: (h.files || []).join(', '),
      scenario: h.scenario,
      proc: h.proc,
      seat: h.seat || 'hackintosh',
      budgetMB: h.budgetMB == null ? '' : String(h.budgetMB),
      evidence: h.evidence ? `${h.evidence.file}:${h.evidence.line} (${h.evidence.token})` : '',
      repoPath: a.repoPath,
      ghRepo: a.ghRepo || '',
      baseBranch: a.baseBranch,
      branch: branchFor(h.id),
      hackintoshSsh: a.hackintoshSsh,
    },
    extra || {},
  )
}

function bindingsBlock(vars) {
  return Object.entries(vars)
    .map(([k, v]) => `- {{${k}}} = ${v}`)
    .join('\n')
}

/**
 * The Workflow runtime has no filesystem, so the prompt templates are read by
 * the agent itself: the stub names the template file and the bindings.
 */
export function auditorPrompt(h, a) {
  const vars = bindings(h, a)
  return [
    `You are the AUDITOR for hotspot ${h.id} in ${a.repoPath}.`,
    `First Read ${a.repoPath}/harness/prompts/auditor.md and follow it exactly, substituting these placeholders:`,
    bindingsBlock(vars),
    '',
    'Hard rules from the swarm protocol:',
    `- Work only on branch ${branchFor(h.id)} created from ${a.baseBranch}; touch only the files listed above.`,
    `- Reproduce BEFORE and AFTER on hackintosh via: ssh ${a.hackintoshSsh} 'git -C <repo> fetch && git checkout ${branchFor(h.id)} && harness/run-scenario.sh ${h.scenario} --iters 1000'`,
    '- No run id => hypothesis, not a finding. Set evidence.runId to "" in that case.',
    '- fixClass "mitigation" cannot close the hotspot; say so in notes.',
    'Return the finding via StructuredOutput.',
  ].join('\n')
}

export function adversaryPrompt(h, a, finding) {
  const vars = bindings(h, a, {
    findingJson: JSON.stringify(finding),
    findingRunId: (finding && finding.evidence && finding.evidence.runId) || '',
    findingDeltaMB: finding && finding.evidence ? String(finding.evidence.deltaMB) : '',
    fixClass: (finding && finding.fixClass) || '',
  })
  return [
    `You are the ADVERSARY for hotspot ${h.id} in ${a.repoPath}.`,
    `First Read ${a.repoPath}/harness/prompts/adversary.md and follow it exactly, substituting these placeholders:`,
    bindingsBlock(vars),
    '',
    'Hard rules from the swarm protocol:',
    '- Rerun the scenario yourself on hackintosh (before on the base branch, after on the hotspot branch). Do not trust the auditor run.',
    '- Veto unless: reproduced before, absent after, on hackintosh; compile matrix green (macOS arm64, macOS x86_64, Windows MSVC); behavior bench within +/-10% of baseline.',
    '- Default to veto when uncertain.',
    'Return {verdict, deltaMB, notes} via StructuredOutput.',
  ].join('\n')
}

export function prBody(h, finding, verdict, a) {
  const lines = [
    `## ${h.id}: ${h.title || ''}`,
    '',
    `hotspot: ${h.id}`,
    `scenario: ${h.scenario}`,
    `proc: ${h.proc}`,
    `seat: ${h.seat || 'hackintosh'}`,
    `run-id: ${(finding.evidence && finding.evidence.runId) || ''}`,
    `adversary-run-id: ${verdict.runId || ''}`,
    `deltaMB: ${verdict.deltaMB}`,
    `auditor-deltaMB: ${finding.evidence ? finding.evidence.deltaMB : ''}`,
    `citation: ${finding.fileAtCommitLine}`,
    `confidence: ${finding.confidence}`,
    `fix-class: ${finding.fixClass}`,
  ]
  if (finding.fixClass === 'mitigation') {
    lines.push(`mitigation: ${h.id} is NOT closed by this PR (mitigation-class fix)`)
  }
  lines.push('', '### Adversary notes', verdict.notes || '', '', `base: ${a.baseBranch}`, `files: ${(h.files || []).join(', ')}`)
  return lines.join('\n')
}

export function prTitle(h, verdict) {
  return `${h.id}: ${h.title || ''} (-${verdict.deltaMB} MB)`.slice(0, 200)
}

export function prCreatePrompt(h, finding, verdict, a) {
  const body = prBody(h, finding, verdict, a)
  return [
    `Open a DRAFT pull request for hotspot ${h.id} in ${a.repoPath} (GitHub repo ${a.ghRepo}). Use Bash.`,
    `1. Write the body below verbatim to a temp file BODY (in your scratchpad).`,
    `2. Run exactly: gh pr create --repo ${a.ghRepo} --draft --base ${a.baseBranch} --head ${branchFor(h.id)} --label needs-harness --title ${JSON.stringify(prTitle(h, verdict))} --body-file BODY`,
    `   If the label does not exist, run: gh label create needs-harness --repo ${a.ghRepo} --color D93F0B --description "harness check pending" and retry once.`,
    `3. Run harness/check-pr.sh <pr-number> from ${a.repoPath} and report its exit code as checkPrExit. Do NOT mark the PR ready and do NOT merge.`,
    'Return {prUrl, prNumber, checkPrExit, notes} via StructuredOutput.',
    '',
    '----- BODY -----',
    body,
    '----- END BODY -----',
  ].join('\n')
}

export function loadRegistryPrompt(path) {
  return `Read the JSON file at ${path} and return it verbatim as {registry: <parsed JSON>} via StructuredOutput. Do not modify it.`
}

export function listBranchesPrompt(a) {
  return [
    `In ${a.repoPath}, run: git fetch --prune origin && git branch -r --list 'origin/hotspot/*' --format='%(refname:short)'`,
    `Also list which of them have an OPEN PR against ${a.baseBranch}: gh pr list --repo ${a.ghRepo} --base ${a.baseBranch} --state open --json headRefName --jq '.[].headRefName'`,
    'Return {branches: [...]} containing only branch names (without the origin/ prefix) that are both present on origin and have an open PR.',
  ].join('\n')
}

export function rebasePrompt(branch, a) {
  return [
    `In ${a.repoPath}: git fetch origin && git checkout ${branch} && git rebase origin/${a.baseBranch}.`,
    'If the rebase conflicts, run git rebase --abort and return rebased=false, conflicts=true with the conflicting files in notes.',
    `On success: git push --force-with-lease origin ${branch}.`,
    'Then rerun the compile matrix as described in harness/prompts/adversary.md (macOS arm64 here, macOS x86_64 on hackintosh via ssh, Windows MSVC via ssh tiny11 for Deskflow; per-seat .venv pytest for Mouser).',
    `Return {branch: "${branch}", rebased, conflicts, compileMatrixGreen, notes} via StructuredOutput.`,
  ].join('\n')
}

export function isFinding(f) {
  return !!(f && f.evidence && typeof f.evidence.runId === 'string' && f.evidence.runId.length > 0)
}

// ============================================================================
// RUNTIME — requires the Workflow globals (args, agent, pipeline, phase, log).
// tools/tests/test_swarm_workflow.js strips everything from this marker on.
// ============================================================================

const a = resolveArgs(args)

let registry = a.registry
if (typeof registry === 'string') {
  if (a.dryRun) throw new Error('dryRun requires args.registry to be the parsed registry object (the runtime has no filesystem)')
  const loaded = await agent(loadRegistryPrompt(registry), { label: 'load-registry', phase: 'Audit', schema: REGISTRY_SCHEMA, effort: 'low' })
  if (!loaded) throw new Error(`could not load registry ${registry}`)
  registry = loaded.registry
}

const plan = planFromRegistry(registry, a.repoPath)
log(`${plan.hotspots.length} hotspot(s) for repo ${plan.repo}; files[] pairwise disjoint`)

if (a.dryRun) {
  return plan
}

if (a.postMerge) {
  phase('PR')
  const found = await agent(listBranchesPrompt(a), { label: 'list-hotspot-branches', phase: 'PR', schema: BRANCHES_SCHEMA, effort: 'low' })
  const branches = (found && found.branches) || []
  log(`post-merge: rebasing ${branches.length} open hotspot/* branch(es) onto ${a.baseBranch}`)
  const results = await pipeline(branches, (b) =>
    agent(rebasePrompt(b, a), { label: `rebase:${b}`, phase: 'PR', schema: REBASE_SCHEMA }),
  )
  return results.filter(Boolean).map((r) => ({
    id: r.branch.replace(/^hotspot\//, ''),
    prUrl: '',
    deltaMB: null,
    verdict: r.rebased ? (r.compileMatrixGreen ? 'rebased' : 'rebased-matrix-red') : 'rebase-conflict',
    notes: r.notes || '',
  }))
}

const full = plan.hotspots.map((p) => registry.hotspots.find((h) => h.id === p.id))

const results = await pipeline(
  full,
  (h) => agent(auditorPrompt(h, a), { label: `audit:${h.id}`, phase: 'Audit', schema: FINDING_SCHEMA }),
  async (finding, h) => {
    if (!finding) return { finding: null, verdict: null, status: 'audit-failed' }
    if (!isFinding(finding)) {
      log(`hypothesis: ${h.id} has no run id; not a finding, skipping adversary`)
      return { finding, verdict: null, status: 'hypothesis' }
    }
    if (finding.fixClass === 'mitigation') log(`mitigation: ${h.id} cannot be closed by this fix`)
    const verdict = await agent(adversaryPrompt(h, a, finding), { label: `adversary:${h.id}`, phase: 'Adversary', schema: VERDICT_SCHEMA, effort: 'high' })
    return { finding, verdict, status: verdict ? verdict.verdict : 'adversary-failed' }
  },
  async (r, h) => {
    if (!r || r.status !== 'approve') return Object.assign({ prUrl: '' }, r)
    const pr = await agent(prCreatePrompt(h, r.finding, r.verdict, a), { label: `pr:${h.id}`, phase: 'PR', schema: PR_SCHEMA, effort: 'low' })
    return Object.assign({}, r, { prUrl: (pr && pr.prUrl) || '', checkPrExit: pr ? pr.checkPrExit : null })
  },
)

return full.map((h, i) => {
  const r = results[i] || { status: 'failed', prUrl: '' }
  return {
    id: h.id,
    prUrl: r.prUrl || '',
    deltaMB: r.verdict ? r.verdict.deltaMB : r.finding && r.finding.evidence ? r.finding.evidence.deltaMB : null,
    verdict: r.status,
    fixClass: r.finding ? r.finding.fixClass : null,
    checkPrExit: r.checkPrExit == null ? null : r.checkPrExit,
  }
})
