-- Two CTEs are both MATERIALIZED, the outer one's body reads the inner one, and
-- the outer one is itself pulled into an IN-set. Optimizing the outer plan runs
-- index analysis, which builds the IN-set in place and reads the inner CTE's
-- table while it is still empty:
--
--   Reading from materialized CTE '<name>' before it has been materialized
--
-- On a debug build that aborts the server; production saw a user-data cluster
-- restart five times in eleven minutes from exactly this shape.

SET enable_analyzer = 1;
SET enable_materialized_cte = 1;

DROP TABLE IF EXISTS t_04712;
CREATE TABLE t_04712 (id UInt64, v String) ENGINE = MergeTree ORDER BY id;
INSERT INTO t_04712 SELECT number, concat('v', number % 5) FROM numbers(200);

SELECT '--- two levels, outer pulled into an IN-set ---';

WITH
    inner_cte AS MATERIALIZED (SELECT id FROM t_04712 WHERE v = 'v1'),
    outer_cte AS MATERIALIZED (SELECT id FROM t_04712 WHERE id IN (SELECT id FROM inner_cte))
SELECT count() FROM outer_cte WHERE id IN (SELECT id FROM outer_cte);

SELECT '--- three levels ---';

WITH
    a AS MATERIALIZED (SELECT id FROM t_04712 WHERE v = 'v1'),
    b AS MATERIALIZED (SELECT id FROM t_04712 WHERE id IN (SELECT id FROM a)),
    c AS MATERIALIZED (SELECT id FROM t_04712 WHERE id IN (SELECT id FROM b))
SELECT count() FROM t_04712 WHERE id IN (SELECT id FROM c);

SELECT '--- a bloom_filter skipping index on the same shape ---';

DROP TABLE IF EXISTS t_04712_bf;
CREATE TABLE t_04712_bf (id UInt64, v String, INDEX bf v TYPE bloom_filter GRANULARITY 1)
ENGINE = MergeTree ORDER BY id SETTINGS index_granularity = 1;
INSERT INTO t_04712_bf SELECT number, concat('v', number % 5) FROM numbers(200);

WITH
    inner_cte AS MATERIALIZED (SELECT id FROM t_04712_bf WHERE v = 'v1'),
    outer_cte AS MATERIALIZED (SELECT id FROM t_04712_bf WHERE id IN (SELECT id FROM inner_cte))
SELECT count() FROM outer_cte WHERE id IN (SELECT id FROM outer_cte);

DROP TABLE t_04712_bf;
DROP TABLE t_04712;

SELECT '--- a reused CTE still prunes by primary key ---';

DROP TABLE IF EXISTS t_04712_pk;
CREATE TABLE t_04712_pk (id UInt64, v String) ENGINE = MergeTree ORDER BY id
SETTINGS index_granularity = 100;
INSERT INTO t_04712_pk SELECT number, toString(number) FROM numbers(10000);

-- Deferring index analysis while planning a CTE body must not drop the filters
-- it collects: this would otherwise read 100/100 granules and fail under
-- force_primary_key.
WITH c AS MATERIALIZED (SELECT * FROM t_04712_pk WHERE id = 10)
SELECT count() FROM c a, c b SETTINGS force_primary_key = 1;

SELECT '--- projection analysis is deferred too ---';

DROP TABLE IF EXISTS t_04712_pj;
CREATE TABLE t_04712_pj (id UInt64, v UInt64, PROJECTION by_v (SELECT * ORDER BY v))
ENGINE = MergeTree ORDER BY id SETTINGS index_granularity = 10;
INSERT INTO t_04712_pj SELECT number, 1000 - number FROM numbers(1000);

-- optimizeUseNormalProjections -> selectRangesToRead -> buildIndexes reaches
-- buildOrderedSetInplace as well, so it needs the same treatment.
WITH
    inner_cte AS MATERIALIZED (SELECT v FROM t_04712_pj WHERE id < 5),
    outer_cte AS MATERIALIZED (SELECT * FROM t_04712_pj WHERE v IN (SELECT v FROM inner_cte))
SELECT count() FROM outer_cte a, outer_cte b;

DROP TABLE t_04712_pj;
DROP TABLE t_04712_pk;

SELECT '--- a text index preprocessor still rewrites ---';

DROP TABLE IF EXISTS t_04712_text;
CREATE TABLE t_04712_text (id UInt64, val String,
    INDEX idx (val) TYPE text(tokenizer = splitByNonAlpha, preprocessor = lower(val)) GRANULARITY 1)
ENGINE = MergeTree ORDER BY id SETTINGS index_granularity = 10;
INSERT INTO t_04712_text VALUES (1, 'Hello World');

-- Text function rewriting runs after index analysis. Deferring the analysis
-- without replaying this pass silently dropped the row: the query returned 0.
WITH c AS MATERIALIZED (SELECT * FROM t_04712_text WHERE hasToken(val, 'hello'))
SELECT count() FROM c a, c b;

DROP TABLE t_04712_text;

SELECT '--- virtual column filters still run ---';

DROP TABLE IF EXISTS t_04712_vc;
CREATE TABLE t_04712_vc (id UInt64, v String) ENGINE = MergeTree ORDER BY id
SETTINGS index_granularity = 10;
INSERT INTO t_04712_vc SELECT number, toString(number) FROM numbers(100);

-- Virtual column filters build their sets and execute the filter immediately,
-- so suppressing the build would leave a not-ready set behind.
WITH c AS MATERIALIZED (SELECT * FROM t_04712_vc WHERE _part IN (SELECT DISTINCT _part FROM t_04712_vc))
SELECT count() FROM c;

SELECT '--- an ordinary IN subquery still prunes ---';

-- Nothing about this CTE depends on another CTE, so its set must still be
-- built during index analysis.
WITH c AS MATERIALIZED (SELECT * FROM t_04712_vc WHERE id IN (SELECT number FROM numbers(10, 1)))
SELECT count() FROM c SETTINGS force_primary_key = 1;

DROP TABLE t_04712_vc;

SELECT '--- an ordinary IN subquery in a CTE still prunes ---';

DROP TABLE IF EXISTS t_04712_ord;
CREATE TABLE t_04712_ord (id UInt64, v String) ENGINE = MergeTree ORDER BY id
SETTINGS index_granularity = 100;
INSERT INTO t_04712_ord SELECT number, toString(number) FROM numbers(10000);

-- This subquery reads no CTE at all, so suppressing its set build would cost
-- pruning for nothing: the condition degraded to `true`, 100/100 granules, and
-- force_primary_key threw INDEX_NOT_USED.
WITH c AS MATERIALIZED (SELECT * FROM t_04712_ord WHERE id IN (SELECT number FROM numbers(10, 1)))
SELECT count() FROM c a, c b SETTINGS force_primary_key = 1;

SELECT '--- virtual column filters are still evaluated ---';

DROP TABLE IF EXISTS t_04712_virt;
CREATE TABLE t_04712_virt (id UInt64) ENGINE = MergeTree ORDER BY id;
INSERT INTO t_04712_virt SELECT number FROM numbers(10);

-- filterBlockWithExpression evaluates the filter right after building its sets,
-- so a set it needs cannot be turned into a no-op: doing so raised
-- "Not-ready Set is passed as the second argument for function 'in'".
WITH c AS MATERIALIZED (SELECT id FROM t_04712_virt WHERE _part IN (SELECT DISTINCT _part FROM t_04712_virt))
SELECT count() FROM c a, c b;

DROP TABLE t_04712_virt;
DROP TABLE t_04712_ord;

SELECT '--- explicit PREWHERE over an unmaterialized CTE ---';

DROP TABLE IF EXISTS t_04712_pw;
CREATE TABLE t_04712_pw (id UInt64, v String) ENGINE = MergeTree ORDER BY id;
INSERT INTO t_04712_pw SELECT number, concat('v', number % 5) FROM numbers(200);

-- applyFilters builds PREWHERE sets synchronously, a separate entry point from
-- the one updatePrewhereInfo uses.
WITH
    inner_cte AS MATERIALIZED (SELECT id FROM t_04712_pw WHERE v = 'v1'),
    outer_cte AS MATERIALIZED (SELECT id FROM t_04712_pw PREWHERE id IN (SELECT id FROM inner_cte))
SELECT count() FROM outer_cte WHERE id IN (SELECT id FROM outer_cte);

SELECT '--- virtual column filter over an unmaterialized CTE ---';

-- filterPartsByVirtualColumns evaluates its predicate immediately, so the set
-- cannot simply be skipped; the whole pruning step is skipped instead.
WITH
    inner_cte AS MATERIALIZED (SELECT DISTINCT _part AS p FROM t_04712_pw),
    outer_cte AS MATERIALIZED (SELECT id FROM t_04712_pw WHERE _part IN (SELECT p FROM inner_cte))
SELECT count() FROM outer_cte WHERE id IN (SELECT id FROM outer_cte);

DROP TABLE t_04712_pw;

SELECT '--- Merge table _table filter over an unmaterialized CTE ---';

DROP TABLE IF EXISTS t_04712_mrg_src;
DROP TABLE IF EXISTS t_04712_mrg;
CREATE TABLE t_04712_mrg_src (id UInt64) ENGINE = MergeTree ORDER BY id;
INSERT INTO t_04712_mrg_src SELECT number FROM numbers(10);
CREATE TABLE t_04712_mrg AS t_04712_mrg_src ENGINE = Merge(currentDatabase(), '^t_04712_mrg_src$');

-- ReadFromMerge::getSelectedTables filters by _table through its own
-- buildFilterExpression call, separate from the MergeTree entry points.
WITH
    inner_cte AS MATERIALIZED (SELECT 't_04712_mrg_src' AS name FROM numbers(1)),
    outer_cte AS MATERIALIZED (SELECT id FROM t_04712_mrg WHERE _table IN (SELECT name FROM inner_cte))
SELECT count() FROM outer_cte WHERE id IN (SELECT id FROM outer_cte);

SELECT '--- a constant virtual column predicate still prunes ---';

DROP TABLE IF EXISTS t_04712_part;
CREATE TABLE t_04712_part (id UInt64, v String) ENGINE = MergeTree
PARTITION BY intDiv(id, 100) ORDER BY id SETTINGS index_granularity = 100;
INSERT INTO t_04712_part SELECT number, toString(number) FROM numbers(10000);

-- This predicate has no subquery at all, so skipping virtual column pruning for
-- it would read every partition: max_rows_to_read then trips at 1100 rows.
WITH c AS MATERIALIZED (SELECT * FROM t_04712_part WHERE _partition_id = '10')
SELECT count() FROM c a, c b SETTINGS max_rows_to_read = 1000;

SELECT '--- a virtual column predicate mixed with a CTE condition still prunes ---';

-- Only the `_partition_id` half reaches the virtual column filter; the condition
-- on `v` is split off and never evaluated there. Checking the whole predicate for
-- CTE dependencies disabled pruning for the safe half, turning one part into all
-- of them and tripping max_rows_to_read.
WITH
    inner_cte AS MATERIALIZED (SELECT toString(number) AS v FROM numbers(1000, 100)),
    c AS MATERIALIZED (SELECT * FROM t_04712_part WHERE _partition_id = '10' AND v IN (SELECT v FROM inner_cte))
SELECT count() FROM c a, c b SETTINGS max_rows_to_read = 1000, optimize_move_to_prewhere = 0;

DROP TABLE t_04712_part;
DROP TABLE t_04712_mrg;
DROP TABLE t_04712_mrg_src;

SELECT '--- a scalar subquery over a materialized CTE ---';

DROP TABLE IF EXISTS t_04712_scalar;
CREATE TABLE t_04712_scalar (id UInt64, c UInt64) ENGINE = MergeTree ORDER BY id;
INSERT INTO t_04712_scalar SELECT number, number FROM numbers(100);

-- Scalar subqueries are planned with is_subquery, which makes
-- collectMaterializedCTEs return nothing: no materialization step is added and
-- the pipeline reads the CTE's still empty table at run time.
WITH
    a AS MATERIALIZED (SELECT * FROM t_04712_scalar WHERE id = 5),
    b AS MATERIALIZED (SELECT * FROM a)
SELECT count() FROM t_04712_scalar WHERE c <= (SELECT c FROM b);

DROP TABLE t_04712_scalar;
