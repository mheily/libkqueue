#!/usr/bin/env python3
"""
Pivot --list-gated TSV output into a Markdown gate-matrix table.

Each argument must be "platform:path/to/file.tsv".  The TSV format is the
three-column output of `libkqueue-test --list-gated=PLATFORM`:

    suite TAB test_name TAB reason

Usage:
    libkqueue-test --list-gated=linux       > linux.tsv
    libkqueue-test --list-gated=windows     > windows.tsv
    ...
    gen_gate_matrix.py linux:linux.tsv windows:windows.tsv ...

Output: Markdown table on stdout.
Rows: one per (suite, test) pair seen across all inputs.
Columns: Suite, Test, then one column per platform in argument order.
Gated cell: [⊘](# "reason") - renders as a symbol with hover tooltip.
Not-gated cell: empty.
"""
import sys
from collections import defaultdict


def escape_cell(text):
    """Escape pipe and backtick characters that break Markdown table cells."""
    return text.replace('|', '&#124;').replace('`', '&#96;')


def escape_title(text):
    """Escape double-quotes inside a Markdown link title attribute."""
    return text.replace('"', '&quot;')


def main():
    platforms = []
    rows_seen = []
    rows_set = set()
    gates = defaultdict(dict)  # gates[(suite, test)][platform] = reason

    for arg in sys.argv[1:]:
        if ':' not in arg:
            print(f"error: expected platform:path, got {arg!r}", file=sys.stderr)
            sys.exit(1)
        platform, path = arg.split(':', 1)
        platforms.append(platform)
        with open(path) as fh:
            for line in fh:
                line = line.rstrip('\n')
                if not line:
                    continue
                parts = line.split('\t', 2)
                if len(parts) != 3:
                    continue
                suite, test, reason = parts
                key = (suite, test)
                if key not in rows_set:
                    rows_set.add(key)
                    rows_seen.append(key)
                gates[key][platform] = reason

    if not platforms:
        print("usage: gen_gate_matrix.py platform:file.tsv ...", file=sys.stderr)
        sys.exit(1)

    rows_seen.sort()

    header = ['Suite', 'Test'] + platforms
    sep    = ['---',   '---']  + ['---'] * len(platforms)

    out = []
    out.append('| ' + ' | '.join(header) + ' |')
    out.append('| ' + ' | '.join(sep)    + ' |')

    for (suite, test) in rows_seen:
        cells = [escape_cell(suite), escape_cell(test)]
        for p in platforms:
            reason = gates[(suite, test)].get(p)
            if reason:
                cells.append(f'[⊘](# "{escape_title(reason)}")')
            else:
                cells.append('')
        out.append('| ' + ' | '.join(cells) + ' |')

    print('\n'.join(out))


if __name__ == '__main__':
    main()
