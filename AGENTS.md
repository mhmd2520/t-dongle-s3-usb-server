# T-Dongle-S3 — Multi-Agent System

Skill files live in `.claude/skills/`. Invoke with `/skill-name [args]` in the Claude Code terminal.

---

## Agent Roster

| Skill | Command | Role |
|-------|---------|------|
| **Master** | `/master [request]` | Entry point for any request — classifies and routes to the right pipeline |
| **Orchestrate** | `/orchestrate [task]` | Runs a multi-step pipeline for complex requests |
| **Architect** | `/architect [feature]` | Designs implementation plan before any code is written |
| **Research** | `/research [topic]` | Searches web for solutions, libraries, best practices |
| **Compare** | `/compare [old vs new]` | Before/after analysis before replacing implementations |
| **Validate** | `/validate [change]` | Pre-implementation safety check (memory, conflicts, FreeRTOS) |
| **Implement** | `/implement [feature]` | Makes targeted code changes following project conventions |
| **DebugBuild** | `/debug-build` | Diagnoses and fixes build/upload failures |
| **Report** | `/report` | Updates CLAUDE.md change log and feature registry |
| **MemCheck** | `/mem-check` | Analyzes RAM/Flash usage from build output |

---

## Standard Pipelines

```
New Feature:    /architect → /validate → /implement → /debug-build → /report
Bug Fix:        /debug-build → /validate → /implement → /report
Performance:    /research → /compare → /architect → /validate → /implement → /report
Research only:  /research → /compare
Unsure:         /master [describe what you want]
```

---

## When to Use Each Agent

### `/master` — Always the safe default
Use when you're not sure which agent to invoke. The Master classifies your request and assembles the right pipeline automatically.

### `/architect` — Plan before you code
Use for: any change touching 3+ files, new modules, FreeRTOS task additions, library migrations.
Output: affected file list, step-by-step sequence, key decisions, risks.

### `/research` — Unknown territory
Use for: choosing between libraries, finding ESP-IDF 5.x compatible solutions, understanding a new protocol or technique.
Output: compatibility-filtered comparison table + recommendation.

### `/compare` — Before replacing anything
Use for: evaluating a proposed change vs the current implementation, migration planning.
Output: before/after table covering approach, RAM, Flash, complexity, risk.

### `/validate` — Safety gate
Use for: any change touching USB/WiFi coexistence, SD card access from tasks, shared volatile state, new FreeRTOS tasks.
Output: ✅ Safe / ⚠️ Concern / ❌ Blocker with explanation.

### `/implement` — Targeted coding
Use for: straightforward implementation once the approach is decided. Follows project conventions automatically.

### `/debug-build` — Build failures
Use for: any `*** Error` in PlatformIO output, cmake cache corruption, upload failures. Has a built-in table of known ESP32/pioarduino failure patterns.

### `/report` — After every successful implementation
Updates CLAUDE.md Feature Registry, Build Phases, and Change Log. Run after hardware verification too.

### `/mem-check` — After significant additions
Checks RAM/Flash budget against the build output. Warns when approaching limits (400 KB SRAM, 250 KB IRAM).

---

## Design Principles

1. **Master first, agents second** — `/master` selects the right specialists; use individual agents when you already know what you need.
2. **Never skip Validate** for changes touching: FreeRTOS tasks, SD card, WiFi/USB coexistence, NVS, shared state.
3. **Never skip Architect** for changes spanning 3+ files or introducing new modules.
4. **Always end with Report** after hardware-verified changes to keep CLAUDE.md accurate.
5. **Research is read-only, Implement changes code** — never mix roles in one agent.

---

## Adding New Agents

1. Create `.claude/skills/<name>.md` with a `description:` frontmatter line
2. Add it to the roster table above and to the `orchestrate.md` and `master.md` Agent Rosters
3. Define its output format clearly — agents chain outputs as inputs
