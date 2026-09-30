# TODO

Open work is tracked as GitHub issues on `sajibsrs/ace`, not in this file.
Issues are non-normative: no issue is a decision. Anything promoted to work is
carved through the Decision Queue in `spec/`, then implemented and logged in
`changelog/`.

```sh
gh issue list -m "2. Compiler and backend completion"   # one priority band
gh issue list -l linux-x64                              # one target
gh issue list -l bug                                    # defects
gh issue view 72                                        # largest measured gaps
```

Milestones are the priority bands, recommendations the owner resolves under
the priority stack (`PHILOSOPHY.md`): performance > simplicity > modularity >
compile time > safety. The active Decision Queue always wins.

Labels:

- Kind: `bug` (implemented behavior is incorrect), `feature` (new
  capability), `task` (performance, measurement, cleanup, tooling).
- Status: `queued` (listed for later work in `spec/`), `open-decision` (open
  in the Decision Queue or normative spec), `additive` (deferred as an additive
  extension by a carved decision), `unspecified` (neither decided nor rejected
  in `spec/`). An issue without one was observed first there.
- Area: `lang`, `compiler`, `std`, `pkg`, `tooling`, `docs`.
- Target: `linux-x64`, `darwin-arm64`, only when platform-specific.

Territory rejected by design gets no issue: manual memory through one fixed
strategy (no GC, ownership, or allocator values), no exceptions or propagation,
no inference, no optional type operator, no COW, no iterator contracts, no
comptime or reflection, no block-scoped defer, no `++`/`--`, no hidden-capture
closures.
