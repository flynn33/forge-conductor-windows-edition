# Unpublished 1.3.16 source, installation and repair evidence

This historical record is for product **1.3.16** / Windows MSIX **1.3.16.0**, with 104 Primary/Fallback tools, ten specialist playbooks and five CLU tools. The candidate was installed but was not tagged or published. Original/current-chat workflow qualification did not complete. The higher [1.3.17 candidate](HOST-CAPABILITIES-1.3.17.md) requires fresh final-source and installed checks; these results are not reassigned.

## Actual source and CI checks

Source commit `25d15f922744c464fdf660fd991f1cdfe1542323`, tree `1b23dfa072ea60e7da0f02dd4bd20f7028ac4dc9`, passed **162/162 configured CTest entries in 78.19 seconds**, exit 0, with all three static gates, package persistence and Product All passing. The committed local run is `out/verification/capability-full-tests-1316-final-source-retry1.log`; its actual summary appears at lines 740 and 783. Static results are in `capability-static-gates-1316-final-source-retry1.log`. Earlier failing checks remain separate from that complete run.

[Windows CI run 37640187548](https://github.com/flynn33/forge-conductor-windows-edition/actions/runs/37640187548) completed successfully for that exact source: complete Release product, **162/162 tests in 92.20 seconds**, all three static gates and staged-product upload. The captured CI summary SHA-256 is `9ce9f5c15b5c2ca611c0452ca725bf90c493cef696d57659d495a13d8dfef131`; its full log SHA-256 is `f7ff2b35011a4b3e871744daf5e979c2b999c6e9ce6dbf38f5a2b05f17051fe5`. CI and local elapsed times identify distinct runs.

The source retains automatic background deployment only when the Manager's actual data root equals the ordinary persistent owner root. Alternate isolated profiles retain read-only inspection and explicitly authorized repair but skip automatic deployment before authorization. An unavailable optional persistent-root identity disables automatic host repair; ordinary required-root validation remains fatal earlier in owner startup. The guarded real-process regression uses fake USERPROFILE/LOCALAPPDATA and exact MCP/owned bridge preservation checks.

## Actual signed upgrade and preservation

The signed 1.3.16.0 candidate installed at **2026-10-07 14:58:43Z**. The complete offline installed verifier passed before launch, matching all four installed/staged/MSIX streams and preserving **7,991 owner-profile files, 388,861,727 bytes**, plus protected conversation, selection, configuration and MCP snapshots. That observation applies at installation, before explicit repair or subsequent owner setting choices.

MSIX SHA-256: `17e33e2d4f7c2dec11e51ad0e739f8aaa1d301c3427cbdfc8dedd38f9ae51596`. Payload-manifest SHA-256: `07fa55bd3a6da164814ff0470f4f9ec59b1e4430c1fad180a315ff5e42053ebf`. The valid development signer retains thumbprint `4B290FFFF895EAAC959DC343255F0DA043DC12A2` and publisher `CN=Forge Conductor Development`.

| Executable | Actual installed/staged/MSIX SHA-256 |
|---|---|
| ForgeConductorApp.exe | `92a520ab882e34a44008e0a51f31b149a163a922e569ad236255c91d40b6fbf2` |
| forge-conductor.exe | `3b017db01c331c67fb5f35061b337dc7427ce8bbf6050ae3a54e791cca722c01` |
| ForgeConductor.Manager.exe | `203d3ce95c2bf49fe7a1f8605adff8672df1542ccad1491b39059af717603035` |
| ForgeConductor.SessionHost.exe | `d811553c77115f092e3498ad922fab55ef40cc3b8328e690cb49fc72351e84a6` |

The subsequent explicit installed-App repair completed in **62.857 seconds**, registering all three version-1.3.16 routes. Before/after readbacks preserved foreign MCP semantics and all **730 foreign plugin files**. Registration and preserved file state do not establish three concurrently ready connections or productive current-chat inference.

## Superseding startup observation and limits

A later observed cold Manager child exited while the CLI continued its bounded startup wait. The 1.3.17 source change retains the owned startup handle, checks its exit and reports that exit when no competing profile owner exists; its tests and final delivery remain pending in the new record. The signed 1.3.16 package remains an actual installed historical artifact, with no original/current-chat workflow qualification, release tag or published assets.

Earlier [1.3.15 isolation/deployment failure](HOST-CAPABILITIES-1.3.15.md) and [1.3.14 native/model investigation](HOST-CAPABILITIES-1.3.14.md) remain preserved. No original project gate, new rollover, physical context exhaustion, exhaustive GUI walkthrough or cloud-provider parity was established by the source/CI/install/repair checks above. Private profile/chat, process and request bodies remain local.
