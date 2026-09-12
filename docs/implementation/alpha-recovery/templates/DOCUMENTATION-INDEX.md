# Current documentation inventory and phase review

Create/reconcile this inventory at docs/DOCUMENTATION-INDEX.md using actual tracked files. This template lists expected categories, not a complete repository inventory. Enumerate every active first-party documentation file, including root guides and adopted instructions, and explicitly classify historical/generated/vendor materials. Do not assume that only docs/*.md are documentation.

Current plan: windows-alpha-recovery-2026-09-12  
Latest phase review: [R#; UTC date; phase receipt link]

| Actual path | Class | Canonical subject / supersedes | Reviewed phase | Content update or reviewed-unchanged evidence |
|---|---|---|---|---|
| README.md | active first-party | Product entry point/status | [actual] | [actual] |
| CHANGELOG.md | active first-party | Implemented user-facing changes | [actual] | [actual] |
| ROADMAP.md | active first-party | Phase/milestone delivery | [actual] | [actual] |
| docs/STATUS.md | active first-party | Current source and execution evidence | [actual] | [actual] |
| [every remaining real active doc] | active first-party | [actual] | [actual] | [actual] |
| [real historical receipt or retired plan] | historical immutable | [original evidence; current replacement link] | [classification] | Preserved, not retested/relabelled |
| [real third-party/license file] | vendor/legal | [actual] | [classification] | Preserve notices |
| [actual generated doc/config] | generated | [generator/source of truth] | [actual] | Update generator/source when applicable |

A practical unchanged-current-document stamp is:
`Phase review: R# — YYYY-MM-DD — applicable content checked; no behavior changes in this document.`

Replace the existing single stamp rather than appending one per phase. For changed documents, update substance and review marker. Machine-readable files use their existing metadata/index rather than invalid Markdown comments. Historical logs/receipts and legal notices are not edited merely to manufacture a current timestamp.

At each phase, refresh every active first-party file's applicable content or review marker and update this index. Reconcile new/deleted/renamed docs; no silent omission. Root README/CHANGELOG/ROADMAP require actual phase status/change updates even when other documents are substantively unchanged.
