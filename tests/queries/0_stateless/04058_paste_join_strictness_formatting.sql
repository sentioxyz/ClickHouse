SET join_default_strictness = 'ALL';

SET enable_analyzer = 1;
SELECT 'analyzer', count()
FROM (EXPLAIN SYNTAX oneline = 1, run_query_tree_passes = 1
    SELECT * FROM numbers(1) AS lhs PASTE JOIN numbers(1) AS rhs)
WHERE explain ILIKE '%ALL PASTE%' OR explain ILIKE '%ANY PASTE%';

SET enable_analyzer = 0;
SELECT 'legacy_analyzer', count()
FROM (EXPLAIN SYNTAX oneline = 1
    SELECT * FROM numbers(1) AS lhs PASTE JOIN numbers(1) AS rhs)
WHERE explain ILIKE '%ALL PASTE%' OR explain ILIKE '%ANY PASTE%';
