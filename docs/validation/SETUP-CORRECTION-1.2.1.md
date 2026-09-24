# Setup correction 1.2.1 verification

## Reproduced failure and correction

The previous release hid guided mode and required finding plugin repair below diagnostics. Automatic preparation did not invoke plugin installation. A real attempted installation additionally reproduced a policy denial because the maintenance authority includes the private data directory. These were functional release defects.

The corrected coordinator requires plugin installation and synchronization before model readiness. Maintenance retains its narrow Manager-issued authority, while ordinary project tools retain adopted-policy enforcement. Installation feedback and an explicit setup entry are now visible.

## Native observations

A disposable profile on this host completed all five preparation checks against installed LM Studio 0.4.25 and qwen3.8-27b. All three plugins appeared in LM Studio's integration list. The existing third-party MCP configuration was compared structurally before and after deployment and remained equal.

Task: create setup-proof.txt containing `Guided setup installed the plugins.`, then read it back. Run `d946693c-31a6-42de-86bd-b4e550b63378`, project `0609679d-c9c7-463c-967a-48f4c29631f2`, completed through the Manager. The resulting file was independently read from disk and matched the requested contents plus a newline (35 bytes). Input/output token counts were 22032/229. Captures from the isolated application accompany the verification assets; private LM Studio chat history and configuration are excluded.

The upgraded disposable profile was seeded with guided mode off and the Rig page selected. It reopened in Guided setup, retained its folder, and successfully repeated preparation. The return-to-setup button and plugin controls were checked at both 1668-pixel and 1128-pixel window widths; navigation labels and primary installation controls remained visible. Detailed plugin diagnostics remain available in the expander.

The development run passed all 153 CTest entries, the native Release build, static gates and package-persistence checks.

## Automated coverage

The setup regression test exercises failure at each of the five stages, dependency ordering, retry, cancellation, identity validation and exceptions. Existing suites exercise native deployment transactions, preserved configuration, all tool contracts, policy denial and accepted scopes, Manager control, persistence, telemetry, provider transport and continuity. The isolated HTTP Manager test skips global plugin deployment to preserve the operator's configuration; it is not counted as proof of that deployment. The separate native observation above covers the actual plugin path.

Final build, full CTest, static gates, package signatures/hashes and simulated lifecycle results are recorded in the release verification archive. Those receipts identify their source commit. Native observations and automated contracts are distinct forms of evidence; a synchronized plugin configuration alone is not evidence of a successful desktop-chat tool call.
