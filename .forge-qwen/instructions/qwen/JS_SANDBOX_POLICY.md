# JavaScript sandbox policy

Allowed external reasoning uses:

- parse/validate/normalize JSON;
- compare tool/schema snapshots;
- generate deterministic tables from existing data;
- check graph dependencies;
- calculate bounded statistics;
- create a proposed data-only patch for later review.

Forbidden:

- product runtime or installed dependency;
- source-of-truth writes to the repository;
- network/secrets;
- executing untrusted project scripts;
- replacing native tests/builds;
- adding Node/npm to the Windows product.

Qwen must inspect sandbox output and write approved data through the filesystem tool.
