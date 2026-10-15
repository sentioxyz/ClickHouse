-- Temporary tables of the Log family and Memory: created, replaced with CREATE OR REPLACE (of an existing and of a new table) and
-- REPLACE, and read back. Each of them is an original CREATE of a new storage (InterpreterCreateQuery::isOriginalCreate).

CREATE TEMPORARY TABLE tmp_log (a Int32, s String) ENGINE = Log;
INSERT INTO tmp_log VALUES (1, 'created');
SELECT 'Log', a, s FROM tmp_log;
CREATE OR REPLACE TEMPORARY TABLE tmp_log ENGINE = Log AS SELECT toInt32(2) AS a, 'create or replace' AS s;
SELECT 'Log', a, s FROM tmp_log;
REPLACE TEMPORARY TABLE tmp_log ENGINE = Log AS SELECT toInt32(3) AS a, 'replace' AS s;
SELECT 'Log', a, s FROM tmp_log;
CREATE OR REPLACE TEMPORARY TABLE tmp_log_new ENGINE = Log AS SELECT toInt32(4) AS a, 'create or replace new' AS s;
SELECT 'Log', a, s FROM tmp_log_new;
DROP TEMPORARY TABLE tmp_log;
DROP TEMPORARY TABLE tmp_log_new;

CREATE TEMPORARY TABLE tmp_tinylog (a Int32, s String) ENGINE = TinyLog;
INSERT INTO tmp_tinylog VALUES (1, 'created');
SELECT 'TinyLog', a, s FROM tmp_tinylog;
CREATE OR REPLACE TEMPORARY TABLE tmp_tinylog ENGINE = TinyLog AS SELECT toInt32(2) AS a, 'create or replace' AS s;
SELECT 'TinyLog', a, s FROM tmp_tinylog;
REPLACE TEMPORARY TABLE tmp_tinylog ENGINE = TinyLog AS SELECT toInt32(3) AS a, 'replace' AS s;
SELECT 'TinyLog', a, s FROM tmp_tinylog;
CREATE OR REPLACE TEMPORARY TABLE tmp_tinylog_new ENGINE = TinyLog AS SELECT toInt32(4) AS a, 'create or replace new' AS s;
SELECT 'TinyLog', a, s FROM tmp_tinylog_new;
DROP TEMPORARY TABLE tmp_tinylog;
DROP TEMPORARY TABLE tmp_tinylog_new;

CREATE TEMPORARY TABLE tmp_stripelog (a Int32, s String) ENGINE = StripeLog;
INSERT INTO tmp_stripelog VALUES (1, 'created');
SELECT 'StripeLog', a, s FROM tmp_stripelog;
CREATE OR REPLACE TEMPORARY TABLE tmp_stripelog ENGINE = StripeLog AS SELECT toInt32(2) AS a, 'create or replace' AS s;
SELECT 'StripeLog', a, s FROM tmp_stripelog;
REPLACE TEMPORARY TABLE tmp_stripelog ENGINE = StripeLog AS SELECT toInt32(3) AS a, 'replace' AS s;
SELECT 'StripeLog', a, s FROM tmp_stripelog;
CREATE OR REPLACE TEMPORARY TABLE tmp_stripelog_new ENGINE = StripeLog AS SELECT toInt32(4) AS a, 'create or replace new' AS s;
SELECT 'StripeLog', a, s FROM tmp_stripelog_new;
DROP TEMPORARY TABLE tmp_stripelog;
DROP TEMPORARY TABLE tmp_stripelog_new;

CREATE TEMPORARY TABLE tmp_memory (a Int32, s String) ENGINE = Memory;
INSERT INTO tmp_memory VALUES (1, 'created');
SELECT 'Memory', a, s FROM tmp_memory;
CREATE OR REPLACE TEMPORARY TABLE tmp_memory ENGINE = Memory AS SELECT toInt32(2) AS a, 'create or replace' AS s;
SELECT 'Memory', a, s FROM tmp_memory;
REPLACE TEMPORARY TABLE tmp_memory ENGINE = Memory AS SELECT toInt32(3) AS a, 'replace' AS s;
SELECT 'Memory', a, s FROM tmp_memory;
CREATE OR REPLACE TEMPORARY TABLE tmp_memory_new ENGINE = Memory AS SELECT toInt32(4) AS a, 'create or replace new' AS s;
SELECT 'Memory', a, s FROM tmp_memory_new;
DROP TEMPORARY TABLE tmp_memory;
DROP TEMPORARY TABLE tmp_memory_new;
