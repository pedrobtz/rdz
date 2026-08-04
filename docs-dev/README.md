# Developer documentation

This directory contains internal planning and status documents. User-facing
behavior belongs in the repository [README](../README.md) and release changes
belong in [NEWS](../NEWS.md).

## Maintained documents

| Document | Purpose |
|---|---|
| [Development status](status.md) | Verified state, active work, and immediate TODOs. |
| [Roadmap](roadmap.md) | Accepted priorities, open product decisions, and candidate future work. |
| [Benchmark guide](../inst/benchmarks/README.md) | Reproducible benchmark commands, inputs, and result locations. |

## Historical reviews

Files under [archive](archive/) are point-in-time assessments. They preserve
useful reasoning but are not sources of current truth. Their line references,
benchmark claims, and recommendations may be obsolete.

## Maintenance rules

- Keep current facts in `status.md`; move completed user-visible work to
  `NEWS.md` rather than accumulating a project diary.
- Keep benchmark numbers in `inst/benchmarks/*-results.md` and link to them.
- Record only accepted work as roadmap checkboxes. Label unapproved design
  directions as open decisions or candidates.
- Prefer links to files and symbol names over source line numbers.
- Archive dated reviews instead of continually patching their historical text.
