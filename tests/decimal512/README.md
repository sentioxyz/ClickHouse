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
tests/decimal512/run_checks.sh --tier nightly --binary <clickhouse> --out <empty dir> [--source-sha <sha>]
tests/decimal512/run_checks.sh --tier release --binary <clickhouse> --out <empty dir> --source-sha <sha> --source-clean \
    --buggy-binary <previous production build> --image <local image> \
    --keeper-binary <the production Keeper build> [--keeper-expected-sha256 <sha256> --keeper-basis <where it comes from>] \
    [--protocol-evidence <native_matrix.tsv with identities.tsv next to it>] [--perf-rounds 7]
tests/decimal512/ci_harness.sh      # binary-free checks of the checks themselves (what CI runs, see below)
```

`--instance a|b|c` selects the isolated server slot for the stateless tests (ports 39000/49000/59000 and up; the
default is `c`). Parallel runs need different slots.

Some fork tests write into the source tree (the `10303`-`10307` data files, `.stdout`/`.stderr` of failed tests);
every file written under `tests/` during the stateless run is listed in `<out>/logs/tree_side_effects.txt`. The
script never deletes them. Format schemas are copied per instance (`tools/isolated_ch.sh` exports
`CLICKHOUSE_SCHEMA_FILES` to that private copy), so `02483_capnp_decimals` writes neither into the tree nor into the
host's `/var/lib/clickhouse`.

| tier | when | what runs | typical time |
|---|---|---|---|
| quick | every commit that touches 512-bit code | `midpoint`/`avg2` matrix (1192), operations matrix (7114), key matrix (400), application-SQL matrix (20) | minutes |
| full | periodically, and before a release | quick + vector form (1192) + fixed-seed random differential matrix (`random`, 3000, seed 20260925) + 256-bit dispatch scan against `dispatch_scan_baseline.json` + the stateless tests in `stateless_tests.txt` (229: the fork's tests, 10310-10319, and the tests of the cherry-picked fixes; tests that need pyarrow run outside the gate) | about 20 minutes |
| nightly | scheduled | full + a second fixed-seed random matrix (`random_nightly`, 9000, seed 20260926) | about 30 minutes |
| release | before an image is built or deployed | nightly + regression proofs against `--buggy-binary` (the baseline) for the keys, midpoint, operations and application-SQL matrices + on-disk and aggregate-state compatibility with the baseline in both directions + mixed-version Keeper/ReplicatedMergeTree replication with rollback (`tools/replication/mixed_replication.sh`, the production Keeper build) + mixed-version distributed GROUP BY over shards of both builds, both coordinator directions, against locked oracle cases (`tools/replication/mixed_distributed_groupby.sh`) + a synthetic performance comparison with the baseline (`tools/perf/perf_compare.py`) + image identity | about an hour |

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
- **Keeper/replication**: Keeper and two replicas on 127.0.0.1 only (the script proves it and stops everything
  otherwise); replica r2 goes baseline -> candidate -> baseline while r1 stays on the baseline and Keeper stays on its
  production build. Every required step (part fetches both ways, mutations initiated on either build, a merge by the
  candidate downloaded by the baseline, reads after the upgrade and after the rollback, replication after the rollback)
  must be present once with rc 0 and SAME/OK, and BASELINE/CANDIDATE must be the gated binaries.
- **Mixed-version GROUP BY**: four loopback instances, two shards with a baseline and a candidate replica each; every
  case of `tools/replication/groupby_cases.lock.json` (key shapes including nullable 29..64-byte keys, single- and
  two-level aggregation, candidate-only and both mixed coordinator directions) must be there once. PASS/FAIL is
  recomputed from the sha256 of each result file against the expectation that `groupby_oracle.py` computes from the
  data formulas, never from a cluster's output. `old_only` rows (the baseline's own behaviour) are reported, not gated.
- **Performance**: the gate recomputes medians, ratios and verdicts from the raw timings (at least 5 rounds, build
  order alternating). A compared query is SLOWER when it takes more than 1.10x the baseline and more than 10 ms longer;
  the run is NOISY when a non-decimal control differs by more than 10% and 10 ms. These thresholds are an initial
  engineering judgment, not an agreed SLO; the gate refuses a summary with other thresholds or without that label.
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

`known_defects.json` lists the defects that the checks expose, open and fixed. The expectations encode the intended
semantics and are never edited to make a binary pass. A failure that matches an open defect is reported as
known-open; any other failure is unexpected. An open defect whose checks all pass is stale and fails the gate until the
entry is marked fixed, with evidence, in the same change. A fixed defect that names a proof needs that regression proof
(fails on the baseline, passes on the candidate) in every release run. While any defect is open, a release is BLOCKED.
The entries also record the visible behaviour changes of the fixes (`visible_change`).

## Files

- `run_checks.sh`: the entry point.
- `stateless_tests.txt`: the fork's added or modified stateless tests, selected by exact name. Exclusions are
  annotated.
- `known_defects.json`: the defect registry.
- `dispatch_scan_baseline.json`: the ratchet for `tools/scan_wide_dispatch.py`. Candidates that existed at
  `cd0d6088c5a` are recorded as `legacy`, which counts as debt and never as reviewed; known-defect sites carry their
  defect id. A new candidate fails the full tier. A release also fails on any lost 512-bit site of a port without a
  review decision.
- `ci_harness.sh`: the binary-free checks CI runs (see "CI").
- `tools/`: byte-identical copies of the `clickhouse-decimal512-upgrade` skill scripts. `tools/VENDORED.sha256`
  records their hashes. `tools/gen_stateless_references.py` is the one repository-only tool: it writes the stateless
  tests `10310`-`10316` from an independent Python oracle, and CI requires the committed files to be what it writes.
  - `tools/matrix/gen_app_sql.py`: the application-SQL matrix. Queries that sentio-core's event-log segmentation adaptor
    generates (recorded verbatim in `tools/matrix/appsql_sentio_core.json` with the sentio-core commit, test and patch
    hashes) run on inline data against an independent Python oracle. Target category `appsql-keysnullmap`: the queries
    that the 26.3 production build answers wrongly (NULL merged with 0, a value moved into another key column) or aborts
    on (`KD-KEYSNULLMAP-512-APPSQL`); controls: the same queries with the proposed application patch's compatibility
    key, hand-made extensions of that technique, and shapes outside the affected key range.
  - `tools/matrix/gen_random_decimal512.py`: fixed-seed random differential cases (plus/minus/multiply/divide,
    comparisons, least/greatest, casts between scales, text, round) with an exact Python oracle; locked like the
    other matrices.
  - `tools/scale154/probe_scale154.py`: every path that needs 10^154 (82 cases, Decimal256 controls), per binary.
  - `tools/historical/`: a historical dataset written by an old build and frozen by hash, read/rewrite/rollback
    checks against an oracle (`hist_run.sh`, `hist_check.py`), part-byte comparison (`compare_part_bytes.py`) and
    BACKUP/RESTORE across builds (`hist_backup.sh`).
  - `tools/upstream/triage_upstream_tests.py`: applicability triage of upstream fixes by running their own
    regression tests on the fork build and on a build that contains the fix.
  - `tools/upstream/triage_server_tests.py`: the same for tests that need a server (shell tests, clusters,
    ZooKeeper via the embedded loopback Keeper, pyarrow fixtures with `--python-bin`), on one isolated instance.
    Rows whose test came from the upstream PR instead of the 26.3 backport are marked `-upstream-test`: leads to
    review, not evidence.
  - `tools/replication/mixed_replication.sh`: the mixed-version Keeper/ReplicatedMergeTree check.
  - `tools/replication/mixed_distributed_groupby.sh`: distributed GROUP BY over shards of both builds (rolling
    upgrade) in both coordinator directions, against the locked oracle cases (`groupby_oracle.py`,
    `groupby_cases.lock.json`); a mandatory release check (suite `mixed-groupby`). Its finding for nullable
    29..64-byte keys is the open defect KD-D512-MIXED-GROUPBY-NULLABLE.
  - `tools/replication/upgrade_topology_check.sh`: isolated validation of the replica-group upgrade topology that
    keeps every distributed query on one build, and of the ways it breaks (cross-group failover, an old
    coordinator). It validates a procedure; it changes no deployment and approves no rollout.
  - `tools/parquet/parquet_decimal_writer.py`: dependency-free Parquet writer and oracle for Decimal fixtures of any
    physical layout (used by 10317 and 10318).
  - `tools/perf/perf_compare.py`: the performance comparison.
  - `tools/gate_mutation_test.py`: every fail-closed rule of the gate, on synthetic evidence.
  - `tools/matrix/`: generators with independent oracles, `run_matrix.py`, `lock_cases.py`, and `cases.lock.json`
    (the pinned case lock; a generator change needs a new lock in the same change).
  - `tools/regression_proof.py`: the buggy binary must FAIL and the fixed binary must PASS.
  - `tools/check_gate.py`: the gate.
  - `tools/scan_wide_dispatch.py`: the 256-bit dispatch scan.
  - `tools/isolated_ch.sh`: the isolated loopback server (`CH_ISO_KEEPER=1`: with a private single-node Keeper on
    loopback ports that are part of the isolation proof; the stateless tier uses it).
  - `tools/compat/`: on-disk, aggregate-state and protocol compatibility. `native_compat.sh` needs
    `CH_ISO_SCRIPT=tests/decimal512/tools/isolated_ch.sh`.

## Relation to the production daily inspection

The daily inspection (`ch_daily_inspection.py` in the skill) reads production, and only reads it. It reports version
drift, restarts, crash signatures, disk and error logs, and cannot tell whether a build is correct. These checks are
the opposite: they only run on isolated copies of a build before it reaches production, and they decide whether the
build is correct and compatible. Passing one says nothing about the other.

## CI

`.github/workflows/decimal512_checks.yml` runs on pushes to `26.3-lts-decimal512` and `claude/26.3-lts-decimal512-*`
and on pull requests into `26.3-lts-decimal512`, with a read-only token and no secrets:

- `decimal512-harness` (GitHub-hosted): `ci_harness.sh`. It proves the checks are intact, not that a build is correct.
- `decimal512-binary` (manual dispatch, self-hosted runner labelled `sentio-decimal512`): `run_checks.sh --tier
  quick|full|nightly|release` with a build of the commit. GitHub runs scheduled workflows only from the default
  branch, which this change does not touch, so "nightly" is a manual or externally scheduled dispatch. A GitHub-hosted runner cannot build ClickHouse within its limits.
  As of 2026-09-25 no such runner is registered, so the binary tiers still run on the build host with this script.

## Not covered yet

- The protocol compatibility run (`tools/compat/native_compat.sh`) needs two isolated servers and is passed in as
  evidence.
- Performance is a synthetic comparison on the build host, not a replay of production queries.
- Upstream regression tests that need external services (S3, Kafka, HDFS, MySQL/PostgreSQL servers, ...) cannot run
  in isolation; their fixes stay "unknown" in the applicability triage.
- No sanitizer build of the full binary (disk and shared-host limits); see the release report.
