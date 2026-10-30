---
id: c-utils-north-star
tags: [c-utils, direction, reliability, verification]
type: project
status: active
created: '2026-10-26'
---
# c-utils north star

## One-sentence promise

Provide fast, dependable C89 building blocks with explicit ownership, checked bounds, and evidence that their behavior matches the contracts consumers rely on.

This document ranks choices. The [public header](include/cu.h), existing checks, approved work, and [roadmap](ROADMAP.md) retain their authority. It does not reorder the roadmap or approve implementation.

## Users and jobs

C applications need portable strings, containers, allocation, I/O, and platform mechanisms without acquiring an application framework. Human and agent maintainers need small interfaces, visible lifetime and failure rules, and useful diagnostics. Consumers such as cgrep and Glyph provide concrete workloads and expose integration costs.

## Success signals

- Ownership, borrowing, invalidation, bounds, and failure effects are discoverable in the public API.
- Runtime or static checks enforce useful contracts where the C89 representation permits it.
- Focused tests reject representative contract violations, and deliberate compile-valid faults demonstrate that the tests can detect the broken rule.
- Deterministic models and fault injection expose state transitions without replacing real platform integration tests.
- A bounded proof states its property, model, target assumptions, and limits. A green result is not described as whole-library verification.
- Performance comparisons name the consumer, workload, exact revisions, budgets, and failure cases. An isolated speedup is not a universal claim.

These criteria guide future work; they do not assert complete coverage.

## Strategic bets

1. **Keep lifetime rules enforceable.** Build on arena scope identities, explicit lengths, generation-checked handles, and the existing regions checker. Distinguish enforced properties from caller obligations.
2. **Reuse the verification already present.** Scope misuse tests, the pool reference model, process-I/O platform models, fuzzers, and bounded proofs answer different questions. Strengthen a demonstrated gap rather than introducing a duplicate model or snapshot system.
3. **Make failures inspectable.** Prefer small deterministic scenarios with independently derived expectations. Stable logical identities and state transitions are more useful than addresses or large output dumps.
4. **Use proofs for specific blind spots.** The modeled realloc-failure branch in [the arena-growth proof](proofs/proof_arena_grow.c) complements ordinary allocation injection. Keep the distinction between symbolic evidence and real-allocator observations explicit.
5. **Qualify real consumers.** Glyph's compiler uses c-utils, so library lifetime or failure defects can undermine its verifier. Record the source revision and checks used by the consumer without silently changing its dependency mechanism.

## Constraints and non-goals

Preserve the Dependable C89 subset, portability boundaries, explicit allocation, and existing verification settings. OS interactions belong in platform implementations; scheduling, traversal, retry, and deployment policy stay with consumers unless a reusable need establishes otherwise.

c-utils is not generally memory-safe. Raw pointers retain lifetime obligations. Arena reinitialization with live users is unsupported, and typed container macros distinguish sizes rather than every C type. Handles do not enforce authorization or make arbitrary pointers safe.

Do not add a runtime, global identity service, compiler, or solver stack merely to remove a documented limitation. Do not weaken checks, change expectations to bless current output, or call unsupported checks successful.

## Off-track signals

- A public promise has no corresponding behavioral evidence.
- A test passes only because it repeats the implementation's assumptions.
- Runtime or API complexity grows without a demonstrated consumer need.
- A proof claim exceeds the modeled inputs, byte positions, target, or execution bounds.
- A benchmark improves while failure handling, ownership, fairness, or portability regresses.

Investigate or shrink the change before expanding scope. No new performance threshold or numeric kill rule is introduced here.

## Sprint decision filter

Prefer a consumer-backed contract improvement with a small interface and a test that fails when the promise is false. Reuse existing models and checks. Keep human review focused on contract accuracy, evidence independence, and maintainability.

The first two follow-ups, showing that the scope and stale-handle tests catch
the faults they target and qualifying the arena-grow failure evidence, are done:
`make controls` seeds each fault into a scratch copy and requires the focused
tests, fuzz input and proof to catch it ([fault controls](docs/FAULT-CONTROLS.md)).
They apply [Ron Minsky's reliability talk](https://www.youtube.com/watch?v=rUYP4C29yCw) to existing mechanisms. The [README](README.md) and [roadmap](ROADMAP.md) remain the entry points for current APIs and work order.
