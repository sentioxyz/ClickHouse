# Decimal512 / Int512 checks

Checks for the fork's 512-bit types (`Decimal512`, `Int512`, `UInt512`). They exist because the fork's own
tests passed on production builds that return wrong results for 33-64 byte nullable keys, abort on some of them, and
return wrong results or exceptions for `midpoint`/`avg2` over `Decimal512` (2026-09 review). All original tests
in `tests/queries/0_stateless` stay as they are: this directory adds checks and does not replace any of them.

## Production boundary

These checks never talk to production. They run `clickhouse local` (no listening ports) and, for the stateless
tests, one server that `tools/isolated_ch.sh` starts on loopback only and proves isolated before any test runs. If
isolation cannot be proven, the run fails. The scripts never pull or push images, never read credentials and never
deploy anything. `run_checks.sh` refuses to run when `CLICKHOUSE_HOST` points anywhere other than loopback.

## Tiers

```
tests/decimal512/run_checks.sh --tier quick   --binary <clickhouse> --out <empty dir>
tests/decimal512/run_checks.sh --tier full    --binary <clickhouse> --out <empty dir> [--source-sha <sha>]
tests/decimal512/run_checks.sh --tier release --binary <clickhouse> --out <empty dir> --source-sha <sha> --source-clean \
    --buggy-binary <previous production build> --image <local image> \
    [--protocol-evidence <native_matrix.tsv with identities.tsv next to it>]
```

`--instance a|b|c` selects the isolated server slot for the stateless tests (ports 39000/49000/59000 and up; the
default is `c`). Parallel runs need different slots.

Some fork tests write into the source tree (the `10303`-`10307` data files, `.stdout`/`.stderr` of failed tests);
every file written under `tests/` during the stateless run is listed in `<out>/logs/tree_side_effects.txt`. The
script never deletes them. `02483_capnp_decimals` writes outside the tree and is therefore excluded (see
`known_defects.json`).

| tier | when | what runs | typical time |
|---|---|---|---|
| quick | every commit that touches 512-bit code | `midpoint`/`avg2` matrix (1192), operations matrix (7026), key matrix (400) | minutes |
| full | periodically, and before a release | quick + vector form (1192) + 256-bit dispatch scan against `dispatch_scan_baseline.json` + the stateless tests in `stateless_tests.txt` (57 listed, 1 excluded through its known defect) | about 10 minutes |
| release | before an image is built or deployed | full + regression proof against `--buggy-binary` (the baseline) + on-disk and aggregate-state compatibility with the baseline in both directions + image identity; Keeper/replication and performance are declared not run | longer; it stays BLOCKED until those checks exist |

Exit status (from `tools/check_gate.py`):

| exit | meaning |
|---|---|
| 0 | PASS |
| 3 | PASS WITH OPEN KNOWN DEFECTS: nothing unexpected failed, but the build is not releasable |
| 1 | FAIL, or BLOCKED for a release |
| 2 | usage error |

Rules the gate enforces:

- **Missing evidence fails.** So do a check that ran zero cases, a skipped test, an excluded test without an open
  known defect, and a `not_run` entry in a release.
- **Exact matrix coverage.** The SQL and oracle files must be the ones pinned in `tools/matrix/cases.lock.json`
  (written by `tools/matrix/lock_cases.py`). The result must contain every locked case id exactly once and nothing
  else. Each row must carry the locked expectation. PASS/FAIL is recomputed from the expectation and the recorded
  output; a recorded status that disagrees fails.
- **Run-time identity.** The runner's summary must attest the input hashes, the result file's sha256 and the engine
  sha256/build-id measured before the run, and that engine must be the binary under test. Truncated, duplicated,
  foreign or stale case data, a replaced result file, or results from another engine all fail.
- **Regression proofs are recomputed by the gate** from both result files. The buggy side must be the declared
  baseline (`--buggy-binary`), the fixed side the candidate. Every `fixed` entry of `known_defects.json` that names a
  proof must be PROVEN for a release.
- **Compatibility and protocol checks** bind every engine label to a sha256 (`engine=` lines, `identities.tsv`). Both
  directions between baseline and candidate must be present with rc 0 and SAME, and every required protocol step
  must be there.
- **Identity bases are reported, not blurred:**
  - recomputed by the gate: sha256, build-id, and whether the commit id occurs inside the binary;
  - attested by the runner or the operator: the `GIT_HASH` the binary reports, the image id, the sha256 of the binary
    inside the image, and the clean source tree.

  Attestations are required and are cross-checked against the raw outputs kept in `results/`. They are never
  presented as independent or cryptographic proof. The gate itself runs no engine and no container.
- **A release also needs:**
  - the source commit, asserted clean by the operator, which the binary must report as its `GIT_HASH` and contain as
    a string;
  - a baseline;
  - the local image id and the sha256 of `/usr/bin/clickhouse` inside that image, equal to the tested binary, with
    both raw outputs kept.

## Known defects

`known_defects.json` lists the open defects that the checks expose. The expectations encode the intended semantics
and are never edited to make a binary pass. A failure that matches an open defect is reported as known-open; any
other failure is unexpected. An open defect whose checks all pass is stale and fails the gate until the entry is
marked fixed, with evidence, in the same change. While any defect is open, a release is BLOCKED.

## Files

- `run_checks.sh`: the entry point.
- `stateless_tests.txt`: the fork's added or modified stateless tests, selected by exact name. Exclusions are
  annotated.
- `known_defects.json`: the defect registry.
- `dispatch_scan_baseline.json`: the ratchet for `tools/scan_wide_dispatch.py`. Candidates that existed at
  `cd0d6088c5a` are recorded as `legacy`, which counts as debt and never as reviewed; known-defect sites carry their
  defect id. A new candidate fails the full tier. A release also fails on any lost 512-bit site of a port without a
  review decision.
- `tools/`: byte-identical copies of the `clickhouse-decimal512-upgrade` skill scripts. `tools/VENDORED.sha256`
  records their hashes.
  - `tools/matrix/`: generators with independent oracles, `run_matrix.py`, `lock_cases.py`, and `cases.lock.json`
    (the pinned case lock; a generator change needs a new lock in the same change).
  - `tools/regression_proof.py`: the buggy binary must FAIL and the fixed binary must PASS.
  - `tools/check_gate.py`: the gate.
  - `tools/scan_wide_dispatch.py`: the 256-bit dispatch scan.
  - `tools/isolated_ch.sh`: the isolated loopback server.
  - `tools/compat/`: on-disk, aggregate-state and protocol compatibility. `native_compat.sh` needs
    `CH_ISO_SCRIPT=tests/decimal512/tools/isolated_ch.sh`.

## Relation to the production daily inspection

The daily inspection (`ch_daily_inspection.py` in the skill) reads production, and only reads it. It reports version
drift, restarts, crash signatures, disk and error logs, and cannot tell whether a build is correct. These checks are
the opposite: they only run on isolated copies of a build before it reaches production, and they decide whether the
build is correct and compatible. Passing one says nothing about the other.

## Not covered yet (release stays BLOCKED)

- Mixed-version Keeper/`ReplicatedMergeTree` replication and rollback.
- Performance against the previous build.
- The protocol compatibility run (`tools/compat/native_compat.sh`) needs two isolated servers and is passed in as
  evidence.
- No GitHub workflow runs these checks: the fork's release builds are made by hand on the build host, so the entry
  point is this script.
