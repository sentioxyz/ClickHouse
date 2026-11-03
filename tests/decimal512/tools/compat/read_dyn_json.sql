-- Dynamic/JSON values (binary type headers). One query per line: run_compat2.sh runs each line on its own. Across the builds
-- each must read exactly as the writer did or fail closed (Code 117 / Code 50, check_gate.py fail_closed); never differ.
SELECT 'dyn', dynamicType(v) AS t, count(), toString(sum(toFloat64(toString(v)))) FROM compat.t_dyn GROUP BY t ORDER BY t;
SELECT 'dyn_rows', id, toString(v), dynamicType(v) FROM compat.t_dyn WHERE id IN (0, 1, 2, 27, 28, 29) ORDER BY id;
SELECT 'shared', dynamicType(v) AS t, isDynamicElementInSharedData(v) AS in_shared, count() FROM compat.t_dyn_shared GROUP BY t, in_shared ORDER BY t, in_shared;
SELECT 'shared_rows', id, toString(v), dynamicType(v) FROM compat.t_dyn_shared WHERE id IN (0, 1, 2, 29) ORDER BY id;
SELECT 'json', id, toString(j.a), toTypeName(j.a), toString(j.b), dynamicType(j.b) FROM compat.t_json WHERE id IN (0, 9) ORDER BY id;
SELECT 'dyn_wide', dynamicType(v) AS t, count(), toString(sum(toFloat64(toString(v)))) FROM compat.t_dyn_wide GROUP BY t ORDER BY t;
SELECT 'shared_wide', dynamicType(v) AS t, isDynamicElementInSharedData(v) AS in_shared, count(), toString(sum(toFloat64(toString(v)))) FROM compat.t_dyn_shared_wide GROUP BY t, in_shared ORDER BY t, in_shared;
SELECT 'nested', id, toString(a), toString(m), toString(t), dynamicType(a[1]), dynamicType(m['k']), dynamicType(t.x) FROM compat.t_nested WHERE id IN (0, 1, 2) ORDER BY id;
