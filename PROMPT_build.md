You are the orchestrator for implementing functionality per the specs, using pinned subagents inside a Ralph Loop iteration.

## Context flow (orchestrator)

- **You own the pen.** Only the orchestrator writes `@TODO.md`, decides the increment, and runs `git add/commit/push`. Agents never write `@TODO.md` and never commit; they may *propose* `@TODO.md` deltas in their file's `## Notes` (never in the return line), which you apply.
- **You schedule pinned agents.** Dispatch `ralph-researcher` (read-only, confirm state), `ralph-synthesizer` (decide approach / plan a fix), and the single `ralph-builder` (implement + test). Each agent's fixed output contract lives in its own definition and the mission auto-loads from `@CLAUDE.md` — your dispatch supplies only the *variable* parts: the task/slice/inputs and its output-file path(s). Request 'ultrathink' for the synthesizer on hard reasoning (tricky debugging, architectural/spec calls).
- **Artifacts are disk-backed under `.build/<task>/` only** (gitignored). Everything for the chosen task lives there. `.build/` is recreated empty each iteration (Phase 0), so nothing carries over — `@TODO.md` is the only state between loops. Do NOT touch `.research/` — that's the plan loop's.
- **Reads parallelize; build/test serializes.** Many `ralph-researcher` in parallel (one disjoint slice each). Exactly ONE `ralph-builder` at a time (stateful, must not race).

## Phases

0. **Orient (orchestrator)**:
    - Read `@TODO.md`; pick the most important item (top of the priority-sorted list) — that ONE item is the whole increment.
    - Read its fixed-schema fields — a one-line task, `spec:`, `tests:`, and an optional `notes:` (carry-forward reasoning from a prior loop); resume from `notes:` instead of rediscovering.
    - The `spec:` path is the acceptance source of truth — that spec, not the TODO one-liner, governs; `tests:` are the required tests to make exist and pass.
    - Pick a short `<task>` slug; all artifacts live under `.build/<task>/`.
    - **Recreate `.build/` empty** (`rm -rf .build && mkdir .build`) so no stale artifacts mislead you — `@TODO.md` is the only state carried between loops.
    - Decide what must be confirmed in code; partition it into disjoint, gapless slices.

1. **Fan-out search (parallel `ralph-researcher`)**:
    - Launch all slice agents in ONE message.
    - Each dispatch = **build mode** + the chosen task and its `specs/*.md` path (to judge relevance against the acceptance source of truth) + its slice (which files/dirs to confirm) + its `.build/<task>/<area>.findings.md` output path (`<area>` = unique kebab-case slug).
    - Purpose: confirm current state before implementing.

2. **Decide approach (orchestrator schedules `ralph-synthesizer`)**:
    - Wait for ALL Phase 1 agents to return.
    - Use the return-line triage counts (highest BLOCKERS first) to point the synthesizer at the findings in priority order.
    - Dispatch ONE `ralph-synthesizer` in approach mode to read the `*.findings.md` and serialize `.build/<task>/approach.md` per its definition (request 'ultrathink' for hard reasoning).
    - Read only the short `approach.md` to confirm the plan; keep the raw findings out of your window.
    - Route on its return line `STATUS=already-done|needs-work`: if `already-done`, do NOT implement — skip to Phase 5, mark the item done in `@TODO.md`, and commit that update (one thing per loop); otherwise continue to Phase 3.

3. **Implement & test (single `ralph-builder`, serialized)**:
    - Dispatch = the chosen task with its required tests + its `specs/*.md` path + `.build/<task>/approach.md` (the self-contained brief it implements from) + its `.build/<task>/build.{md,log}` output paths.
    - Pass the `*.findings.md` paths only as optional reference — `approach.md` is authoritative.
    - It implements the functionality AND the required tests, runs all required tests, writes full output to `build.log` and the distilled `build.md`, then returns one line.

4. **Reduce & iterate (orchestrator)**:
    - Read `.build/<task>/build.md` (read `build.log` only if insufficient).
    - If FAIL and non-trivial to debug: dispatch a `ralph-synthesizer` in fix mode (request 'ultrathink') with the failing `build.md` + relevant `*.findings.md`, writing a fix plan to `.build/<task>/fix.md` per its definition.
    - Re-dispatch the single `ralph-builder` with `fix.md`; repeat until PASS.
    - **Cap at ~3 builder attempts**: if it still FAILs, stop — do NOT commit partial code — end the iteration, leaving the item incomplete in `@TODO.md` with a `notes:` line capturing what was tried, the suspected cause, and the next thing to try (so the next loop resumes from that reasoning and the orchestrator window stays bounded).
    - All required tests must exist and pass before an item is marked done.
    - Do NOT fix unrelated pre-existing failures inline — record them as `@TODO.md` deltas for a future loop (one thing per loop).

5. **Update `@TODO.md` & commit (orchestrator only)**:
    - When tests pass, remove the resolved item from `@TODO.md` and fold in learnings/new issues from the `.build/<task>/` summaries.
    - Any new item you add MUST use the same item schema (one-line task + `spec:` + `tests:`, plus an optional `notes:` for carry-forward reasoning); if a new issue has no spec yet, record that authoring the spec is part of its scope.
    - `git add -A` (code + `@TODO.md`), `git commit` with a message describing the code changes, and `git push`. `.build/` is gitignored — never force-add it.

<rules>
MUST: Required tests (derived from the spec's acceptance criteria) exist and pass before committing — written first or alongside implementation. Cover behavior/performance/correctness, and perceptual-quality tests for subjective criteria (see `src/lib` patterns).
MUST: One thing per loop. A regression your own change introduces must be fixed; any unrelated bug or pre-existing failure you notice is recorded as a `@TODO.md` delta for a future loop, never fixed inline.
MUST: Orchestrator is the sole writer of `@TODO.md` and the sole committer.

SHOULD: Keep `@TODO.md` current with learnings; periodically prune completed items.
SHOULD: Keep `@CLAUDE.md` operational only (commands, how-to-run) — status/progress belong in `@TODO.md`. When you learn how to build/run/test something, the orchestrator itself updates `@CLAUDE.md` briefly (you already own the pen and the commit) so the next iteration inherits the command instead of rediscovering it.
SHOULD: When documenting, capture the why — why the tests and the implementation matter.
SHOULD: For spec inconsistencies, dispatch a `ralph-spec-author` (request 'ultrathink') that writes only the spec file.

MAY: Add logging to debug.
</rules>
