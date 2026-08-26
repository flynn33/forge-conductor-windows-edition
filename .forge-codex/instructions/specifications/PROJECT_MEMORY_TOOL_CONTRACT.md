# Project memory tool contract

Port schemas and limits from `ProjectMemoryToolPack.swift`, `ProjectMemoryModels.swift`, `ProjectMemoryService.swift`, and `ProjectMemoryRepository.swift`.

Required semantics:

- explicit project ID after initialization;
- canonical path and repository identity resolution;
- stable UUID record IDs;
- kind, title, summary, optional body, tags, session ID, source, importance, confidence;
- secret redaction and private-key rejection;
- bounded item/body/tag/batch/query sizes;
- deterministic search ranking and pagination;
- optimistic version on update;
- tombstone on forget;
- idempotent links and writes;
- transactionally consistent batch and import;
- checksummed export;
- dry-run import;
- health/status including schema, record/tombstone/event counts, database/WAL bytes, FTS capability, and integrity;
- errors: invalid request, project not found, scope mismatch, record not found, conflict, database busy, unsupported version, integrity failure, redaction rejected, limit exceeded.

Port exact schema details from source and freeze a Windows golden snapshot.
