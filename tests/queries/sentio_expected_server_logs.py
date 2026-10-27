#!/usr/bin/env python3
"""Strictly validate preserved expected Set/Join server log events."""
import argparse
import pathlib
import re
import sys
AMBIGUOUS = 'Binary type index 0x39 is ambiguous without a declared origin: the Decimal512 fork wrote it for a 512-bit type, upstream does not'
HEADER = re.compile('^\\[[^]\\n]+\\] \\d{4}[.]\\d{2}[.]\\d{2} \\d{2}:\\d{2}:\\d{2}[.]\\d+ \\[ \\d+ \\] \\{[0-9a-f-]+\\} <Error> ([A-Za-z]+): (.*)$')
UUID = '[0-9a-f]{8}-[0-9a-f]{4}-[0-9a-f]{4}-[0-9a-f]{4}-[0-9a-f]{12}'

def validate(text, database, table, logger, count):
    assert logger in ('StorageSetOrJoinBase', 'StorageSystemTables')
    assert re.fullmatch('[A-Za-z0-9_]+', database) and re.fullmatch('t_[a-z_]+', table)
    name = re.escape(database + '.' + table)
    identity = 'Table ' + name + ' \\(' + UUID + '\\)'
    if logger == 'StorageSetOrJoinBase':
        body = identity + re.escape(' needs the registration of its stored binary type origin to read its data: ' + AMBIGUOUS)
    else:
        body = re.escape('Code: 117. DB::Exception: ' + AMBIGUOUS + ': ') + identity + re.escape(" was loaded without its data, which needs a declared stored binary type origin: register it with ALTER TABLE ... MODIFY SENTIO BINARY TYPE ORIGIN 'sentio_legacy_v0'. (INCORRECT_DATA), Stack trace (when copying this message, always include the lines below):")
    events = []
    frames = 0
    version = False
    active = False

    def finish():
        if active and logger == 'StorageSystemTables':
            assert frames > 0 and version, 'missing expected exception stack/version'
    for line in text.splitlines():
        if not line.strip():
            continue
        m = HEADER.fullmatch(line)
        if m:
            finish()
            assert m[1] == logger and re.fullmatch(body, m[2]), 'unexpected logger/table/code/message'
            events.append(line)
            active = True
            frames = 0
            version = False
        elif active and logger == 'StorageSystemTables':
            stack = re.fullmatch('(\\d+)[.] .+ @ 0x[0-9a-f]+', line)
            if stack:
                assert not version and int(stack[1]) == frames, 'noncontiguous or late stack frame'
                frames += 1
            else:
                assert re.fullmatch(' \\(version [0-9]+(?:[.][0-9]+){2,3}\\)', line) and frames > 0 and (not version), 'unknown exception tail'
                version = True
        else:
            raise AssertionError('unexpected server log content')
    finish()
    assert len(events) == count, f'expected {count} events, got {len(events)}'
    return len(events)

def main():
    p = argparse.ArgumentParser()
    p.add_argument('--log', type=pathlib.Path, required=True)
    p.add_argument('--database', required=True)
    p.add_argument('--table', required=True)
    p.add_argument('--logger', required=True)
    p.add_argument('--count', type=int, required=True)
    a = p.parse_args()
    text = a.log.read_text()
    try:
        validate(text, a.database, a.table, a.logger, a.count)
    except (AssertionError, ValueError) as e:
        print('UNEXPECTED preserved server log: ' + str(e))
        print(text, end='' if text.endswith('\n') else '\n')
        return 1
    return 0
if __name__ == '__main__':
    raise SystemExit(main())
