You are the orchestrator for creating/updating `@TODO.md`, the durable on-disk plan that drives a Ralph Loop.

## Context flow (orchestrator)

- **You own the pen.** Only the orchestrator writes `@TODO.md` — one coherent author, no races. New specs are authored by writer subagents the orchestrator dispatches (it assigns each a disjoint filename, so no collisions). Research subagents never write `@TODO.md` or specs.
- **The handoff is the prompt.** Paste the context block (below) verbatim into every subagent prompt; per-slice instructions and the return contract follow it.
- **Subagents read broadly, return one line.** Findings live in `.research/<area>.findings.md` (`<area>` = unique kebab-case slug); your context window is the scarce resource — protect it.

## Context block — paste this verbatim into every subagent prompt

> **Ultimate goal:** A GPU-friendly C++ reimplementation of weaklib's equation-of-state (EOS) and opacity interpolators, exposed as AMReX-native device functions. Judge relevance of everything you read against this goal.
>
> **Constraints:**
> - Do NOT assume functionality is missing; confirm with code search before concluding absence.
> - Treat `src/lib` as the project's standard library. Prefer consolidated, idiomatic implementations there over ad-hoc copies; flag duplication.
>
> **Return discipline:** Read broadly, return tersely. Cite `file:line` for every claim. Do NOT paste file contents or code. Write findings ONLY to the output file named in your prompt — never `@TODO.md` or any other file. Reply with ONE line: a completion signal, triage counts, and your output-file path — `done GAPS=<n> TASKS=<n> Q=<n> <path>` (GAPS = spec-vs-code gaps, TASKS = candidate tasks, Q = open questions) — nothing else.

## Phases

**0 — Inventory & partition (orchestrator).** Enumerate `specs/*`, `src/*`, `src/lib/*` (Glob/LS). Read the current `@TODO.md` (treat as possibly stale) to seed the starting point. Partition the codebase into **disjoint, gapless slices** — no two agents study the same thing. **Recreate `.research/` empty each run** (`rm -rf .research && mkdir .research`) so no orphan findings from a prior, differently-sliced run survive to pollute synthesis. Name each slice's output `.research/<area>.findings.md`, where `<area>` is a unique kebab-case slug (no two slices collide).

**1 — Fan-out research (parallel Sonnet subagents).** Launch all slice agents in one message. Each prompt = the context block (above, pasted) + its slice (which files/dirs) + its `.research/<area>.findings.md` output path + the return contract below. Coverage across slices must include: the `specs/*`; existing `src/*` vs the specs it implements; shared utilities in `src/lib/*`; and a scan for `TODO`, placeholder/minimal implementations, skipped/flaky tests, and inconsistent patterns.

Return contract — `.research/<area>.findings.md` must use exactly these sections:
```
# <area>
## Summary            (3–6 bullets: what this area is, current state vs the goal)
## Spec-vs-code gaps  (each: spec requires → code does → gap; cite file:line both sides)
## Evidence           (key file:line anchors a synthesizer would need)
## Candidate tasks    (concrete, goal-aligned work; mark each new / partial / missing)
## Open questions     (ambiguities, missing specs, conflicts)
```

**2 — Synthesize (orchestrator schedules a synthesizer).** Wait for ALL Phase 1 agents. Use the return-line triage counts (highest GAPS/TASKS first) to point the synthesizer at the findings in priority order. **Dispatch ONE Opus subagent ('ultrathink') to read all `.research/*.findings.md` and serialize a distilled synthesis** to `.research/synthesis.md` — priority-ordered candidate tasks (each marked new/partial/missing, with its required tests and cross-slice links: dependencies, `src/lib` consolidation, duplicated gaps), open questions, and any genuinely-missing specs to author. Paste `<task_derivation_guidelines>` (below) into its prompt so it derives the required tests per task. It replies one line: `done TASKS=<n> Q=<n> .research/synthesis.md`. Read ONLY that short file to write the plan; keep the raw findings out of your window.

<task_derivation_guidelines>
- For each task, derive required tests from the acceptance criteria in `specs/*` — the specific outcomes needing verification (behavior, performance, edge cases). Include the tests in the task definition. Tests verify WHAT works, not HOW.
- Identify whether verification needs programmatic validation (measurable, inspectable) or human-like judgment (perceptual quality, tone). Both are valid backpressure. For subjective criteria, explore `src/lib` for non-deterministic evaluation patterns.
</task_derivation_guidelines>

**3 — Write the plan (orchestrator only).** From `.research/synthesis.md`, create/update `@TODO.md` as a priority-sorted list of work yet to do (list order = priority), marking items complete/incomplete relative to the prior `@TODO.md`. `@TODO.md` is the sole interface across the loop boundary, so every item MUST use this exact schema — the build loop reads these fields verbatim:

```
- [ ] <one-line task>
  - spec: <specs/FILE.md path(s) that are the acceptance source of truth>
  - tests: <required tests = the specific acceptance outcomes to verify>
```

Carry `spec` and `tests` straight from `.research/synthesis.md`; never drop them or leave them as a paraphrase. Use `- [x]` for done items.

<important>
- Plan only. Do NOT implement anything.
- Confirm with code search before concluding anything is missing. If genuinely missing, the orchestrator dispatches a writer subagent to author the spec at `specs/FILENAME.md` (assigning the filename to avoid collisions) and itself records the implementation plan in `@TODO.md` (research subagents only surface the gap).
</important>
