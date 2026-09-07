#!/usr/bin/env python3
# Licensed under the MIT License
# https://github.com/craigahobbs/bare-script-c/blob/main/LICENSE

"""
Compare regexNew's compile errors with CPython's re module over random patterns

Usage: python3 bin/regexFuzz.py [bare] [count] [seed]

Prints the number of patterns whose result - a successful compile, or the error message and position -
is identical, then each kind of disagreement with its count and three examples. The generator draws on
the syntax both engines share; README's Compatibility section lists the syntax they read differently.
"""

import collections
import json
import random
import re
import subprocess
import sys
import tempfile


TOKENS = [
    'a', 'b', 'x', '1', '2', '0', '.', '*', '+', '?', '{', '}', ',', '|', '(', ')', '[', ']', '^', '$', '-',
    '\\', 'd', 'w', 'n', 'u', '<', '>', ':', '=', '!', '#', ' ', 'é',
    '(?:', '(?=', '(?!', '(?<=', '(?<!', '(a)', '\\1', '\\2', '\\d', '\\w', '\\b', '\\B',
    '{2}', '{2,}', '{2,3}', '[a-z]', '[^a]', '\\x41', '\\u0041', '\\0', '\\01', '\\.', '\\\\'
]


def main():
    bare = sys.argv[1] if len(sys.argv) > 1 else 'build/bare'
    count = int(sys.argv[2]) if len(sys.argv) > 2 else 4000
    seed = int(sys.argv[3]) if len(sys.argv) > 3 else 1
    random.seed(seed)
    patterns = [''.join(random.choice(TOKENS) for _ in range(random.randint(1, 8))) for _ in range(count)]

    # Compile every pattern in one BareScript run; the debug log carries each failure's message
    with tempfile.NamedTemporaryFile('w', suffix='.bare', delete=False) as script:
        for ix, pattern in enumerate(patterns):
            script.write(f'systemLog("#{ix}")\nregexNew({json.dumps(pattern)})\n')
    output = subprocess.run([bare, '-d', script.name], capture_output=True, text=True).stdout
    results = {}
    for line in output.splitlines():
        if line.startswith('#'):
            current = int(line[1:])
            results[current] = 'ok'
        elif 'regexNew" failed with error: ' in line:
            results[current] = 'error: ' + line.split('regexNew" failed with error: ', 1)[1]

    agree = 0
    kinds = collections.Counter()
    examples = collections.defaultdict(list)
    for ix, pattern in enumerate(patterns):
        try:
            re.compile(pattern)
            expected = 'ok'
        except re.error as error:
            expected = f'error: {error}'
        actual = results.get(ix, '?')
        if expected == actual:
            agree += 1
            continue
        kind = tuple(re.sub(r'\d+', 'N', text.split(' at position')[0]) for text in (expected, actual))
        kinds[kind] += 1
        if len(examples[kind]) < 3:
            examples[kind].append((pattern, expected, actual))
    print(f'{agree} of {count} patterns agree (seed {seed})')
    for kind, kindCount in kinds.most_common():
        print(f'{kindCount:5d}  python: {kind[0]}\n       c:      {kind[1]}')
        for pattern, expected, actual in examples[kind]:
            print(f'       {pattern!r}: python={expected!r} c={actual!r}')


if __name__ == '__main__':
    main()
