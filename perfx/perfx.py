#!/usr/bin/env python3
# Licensed under the MIT License
# https://github.com/craigahobbs/bare-script-c/blob/main/LICENSE

"""
perfx - a cross-language performance testing framework

Runs each application under perfx/apps in every language port, measures each process's wall
time, CPU time, and peak memory, checks that every port computed the same result, and writes a
Markdown report that explains the numbers. Run "perfx.py --help" for the options.
"""

import argparse
import datetime
import json
import math
import os
import platform
import shutil
import subprocess
import sys
import tempfile
import time


# The applications: name, default workload size, the work's growth with the size (1 = linear, 2 =
# quadratic - the pathfinder's size is a grid side), and a description of the workload
APPS = [
    {'name': 'nbody', 'size': 300000, 'dim': 1, 'unit': 'steps',
     'title': 'N-body simulation',
     'desc': 'Advances the five-body Jovian planet system n steps with a symplectic integrator: floating-point '
             'arithmetic on records, the compute-heavy test.'},
    {'name': 'loganalyze', 'size': 300000, 'dim': 1, 'unit': 'log lines',
     'title': 'Web log analysis',
     'desc': 'Generates n access-log lines, then parses each with a regular expression and aggregates unique '
             'clients, status codes, requests per hour, and bytes per path: regular expressions, string '
             'building and splitting, hash maps, and sorting.'},
    {'name': 'jsonetl', 'size': 50000, 'dim': 1, 'unit': 'orders',
     'title': 'JSON extract, transform, load',
     'desc': 'Builds n customer orders with nested item lists, serializes them to JSON, parses the text back, '
             'and ranks customers and SKUs: JSON encoding and decoding, nested containers, grouping. Lua carries '
             'a pure-Lua codec and Perl uses its core JSON::PP, both written in the language itself.'},
    {'name': 'pathfind', 'size': 800, 'dim': 2, 'unit': 'grid side',
     'title': 'Grid pathfinding',
     'desc': 'Runs Dijkstra\'s algorithm corner to corner across an n x n weighted grid with a fifth of the cells '
             'blocked, using a hand-written binary heap: arrays, integer arithmetic, a priority queue.'},
    {'name': 'salesreport', 'size': 100000, 'dim': 1, 'unit': 'rows',
     'title': 'Sales report',
     'desc': 'Generates n CSV sales rows with a quoted field, parses them with a character-level CSV reader, '
             'computes per-region revenue, mean, standard deviation, and top product and rep, and renders an '
             'aligned money-formatted text report: character loops, string formatting, statistics.'},
]

# The languages: key (for --langs), display name, source extension, and the command line
LANGUAGES = [
    {'key': 'bare', 'name': 'BareScript', 'ext': 'bare',
     'cmd': lambda opt, path, n: [opt.bare, path, '-v', 'vN', str(n)],
     'version': lambda opt: [opt.bare, '--version']},
    {'key': 'js', 'name': 'JavaScript (V8 JIT)', 'ext': 'js',
     'cmd': lambda opt, path, n: ['node', path, str(n)],
     'version': lambda opt: ['node', '--version']},
    {'key': 'jsless', 'name': 'JavaScript (V8 jitless)', 'ext': 'js',
     'cmd': lambda opt, path, n: ['node', '--jitless', path, str(n)],
     'version': lambda opt: ['node', '--version']},
    {'key': 'py', 'name': 'Python', 'ext': 'py',
     'cmd': lambda opt, path, n: ['python3', path, str(n)],
     'version': lambda opt: ['python3', '--version']},
    {'key': 'lua', 'name': 'Lua', 'ext': 'lua',
     'cmd': lambda opt, path, n: ['lua', path, str(n)],
     'version': lambda opt: ['lua', '-v']},
    {'key': 'rb', 'name': 'Ruby', 'ext': 'rb',
     'cmd': lambda opt, path, n: ['ruby', path, str(n)],
     'version': lambda opt: ['ruby', '--version']},
    {'key': 'pl', 'name': 'Perl', 'ext': 'pl',
     'cmd': lambda opt, path, n: ['perl', path, str(n)],
     'version': lambda opt: ['perl', '-e', 'print "perl $^V\\n"']},
]

PERFX_DIR = os.path.dirname(os.path.abspath(__file__))
REPO_DIR = os.path.dirname(PERFX_DIR)


def main():
    parser = argparse.ArgumentParser(description='Run the perfx cross-language performance suite')
    parser.add_argument('--bare', default=os.path.join(REPO_DIR, 'build', 'release', 'bare'),
                        help='the BareScript CLI to test (default: the release build)')
    parser.add_argument('--apps', help='comma-separated application names (default: all)')
    parser.add_argument('--langs', help='comma-separated language keys: ' + ', '.join(l['key'] for l in LANGUAGES))
    parser.add_argument('--runs', type=int, default=3, help='runs per application and language; the best is reported')
    parser.add_argument('--scale', type=float, default=1.0, help='workload scale factor (default: 1.0)')
    parser.add_argument('--out', default=os.path.join(REPO_DIR, 'build', 'perfx'), help='the output directory')
    parser.add_argument('--quick', action='store_true', help='one run at a fifth of the workload')
    parser.add_argument('--check', action='store_true',
                        help='verify only: run every port at a small size and report whether the results agree')
    parser.add_argument('--list', action='store_true', help='list the applications and languages and exit')
    parser.add_argument('--merge', action='store_true',
                        help='keep the previous results for the applications and languages not measured now')
    parser.add_argument('--quiet', action='store_true', help='do not print the report to standard output')
    opt = parser.parse_args()

    if opt.list:
        for app in APPS:
            print(f"{app['name']:<12} n = {app['size']:>7} {app['unit']:<10} {app['title']}")
        for lang in LANGUAGES:
            print(f"{lang['key']:<8} {lang['name']}")
        return 0
    if opt.quick:
        opt.runs = 1
        opt.scale = 0.2
    if opt.check:
        opt.runs = 1
        opt.scale = 0.02

    apps = select(APPS, opt.apps, 'name', 'application')
    langs = select(LANGUAGES, opt.langs, 'key', 'language')
    measuredKeys = {lang['key'] for lang in langs}
    if not os.path.exists(opt.bare):
        sys.exit(f'perfx: {opt.bare} not found - run "make release" or pass --bare')
    missing = [lang['name'] for lang in langs if lang['key'] != 'bare' and shutil.which(lang['cmd'](opt, '', 0)[0]) is None]
    if missing:
        sys.exit('perfx: interpreter not found for ' + ', '.join(missing))

    # The previous run's measurements, for the applications and languages this one does not cover
    prior = {}
    priorVersions = {}
    if opt.merge:
        priorPath = os.path.join(opt.out, 'results.json')
        if os.path.exists(priorPath):
            with open(priorPath, encoding='utf-8') as fh:
                stored = json.load(fh)
            if stored.get('scale') != opt.scale:
                sys.exit(f"perfx: {priorPath} was measured at scale {stored.get('scale')}, not {opt.scale}")
            prior = stored.get('runs', {})
            priorVersions = stored.get('versions', {})

    # Measure every port, interleaving the languages round by round so drift affects them alike
    results = {}
    for app in [EMPTY_APP] + apps:
        n = workload(app, opt.scale)
        for run in range(opt.runs):
            for lang in langs:
                path = os.path.join(PERFX_DIR, 'apps', app['name'], app['name'] + '.' + lang['ext'])
                measurement = measure(lang['cmd'](opt, path, n))
                results.setdefault(app['name'], {}).setdefault(lang['key'], []).append(measurement)
                status = f"{measurement['app_ms']:.0f} ms" if measurement['error'] is None else 'FAILED'
                print(f"{app['name']:<12} {lang['name']:<26} run {run + 1}/{opt.runs}  {status}", file=sys.stderr)

    # Fill in what was not measured, and report on everything there is data for
    for appName, byLang in prior.items():
        for key, runs in byLang.items():
            results.setdefault(appName, {}).setdefault(key, runs)
    if prior:
        apps = [app for app in APPS if app['name'] in results]
        langs = [lang for lang in LANGUAGES if any(lang['key'] in byLang for byLang in results.values())]

    summary = summarize(apps, langs, results, opt)
    if opt.check:
        return report_check(summary)

    os.makedirs(opt.out, exist_ok=True)
    versions = {lang['key']: version(lang['version'](opt)) if lang['key'] in measuredKeys
                else priorVersions.get(lang['key']) for lang in langs}
    text = report(summary, apps, langs, versions, opt)
    with open(os.path.join(opt.out, 'report.md'), 'w', encoding='utf-8') as fh:
        fh.write(text)
    with open(os.path.join(opt.out, 'results.json'), 'w', encoding='utf-8') as fh:
        json.dump({'summary': summary, 'runs': results, 'versions': versions,
                   'scale': opt.scale, 'runs_per_test': opt.runs}, fh, indent=2)
    with open(os.path.join(opt.out, 'results.csv'), 'w', encoding='utf-8') as fh:
        fh.write('app,language,n,app_ms,wall_ms,user_ms,sys_ms,peak_rss_mb,result_ok\n')
        for app in apps:
            for lang in langs:
                s = summary[app['name']][lang['key']]
                fh.write(f"{app['name']},{lang['name']},{s['n']},{s['app_ms']:.3f},{s['wall_ms']:.3f},"
                         f"{s['user_ms']:.3f},{s['sys_ms']:.3f},{s['rss_mb']:.2f},{s['ok']}\n")
    if not opt.quiet:
        print(text)
    print(f"perfx: wrote {os.path.join(opt.out, 'report.md')}", file=sys.stderr)
    return 0 if all(summary[app['name']][lang['key']]['ok'] for app in apps for lang in langs) else 1


EMPTY_APP = {'name': 'empty', 'size': 0, 'dim': 1, 'unit': '', 'title': 'Empty program',
             'desc': 'A program that prints its result and exits: interpreter startup and baseline memory.'}


def select(items, arg, key, what):
    if arg is None:
        return items
    wanted = [name.strip() for name in arg.split(',') if name.strip()]
    by_key = {item[key]: item for item in items}
    for name in wanted:
        if name not in by_key:
            sys.exit(f'perfx: unknown {what} "{name}"')
    return [by_key[name] for name in wanted]


def workload(app, scale):
    if app['size'] == 0:
        return 0
    return max(1, round(app['size'] * scale ** (1.0 / app['dim'])))


def measure(cmd):
    """Run a command, returning its wall and CPU times, peak memory, and parsed output"""
    with tempfile.TemporaryFile() as out, tempfile.TemporaryFile() as err:
        t0 = time.perf_counter()
        proc = subprocess.Popen(cmd, stdout=out, stderr=err)
        _, status, rusage = os.wait4(proc.pid, 0)
        wall = time.perf_counter() - t0
        proc.returncode = os.waitstatus_to_exitcode(status)
        out.seek(0)
        err.seek(0)
        stdout = out.read().decode('utf-8', 'replace')
        stderr = err.read().decode('utf-8', 'replace')
    rss = rusage.ru_maxrss if sys.platform == 'darwin' else rusage.ru_maxrss * 1024
    result = None
    app_ms = None
    for line in stdout.splitlines():
        if line.startswith('result: '):
            result = line[8:].strip()
        elif line.startswith('time: '):
            app_ms = float(line[6:].strip())
    error = None
    if proc.returncode != 0:
        error = f'exit status {proc.returncode}: {stderr.strip()[-300:]}'
    elif result is None or app_ms is None:
        error = 'no result or time line: ' + (stdout.strip() or stderr.strip())[-300:]
    return {'wall_ms': wall * 1000, 'user_ms': rusage.ru_utime * 1000, 'sys_ms': rusage.ru_stime * 1000,
            'rss_bytes': rss, 'result': result, 'app_ms': app_ms if app_ms is not None else wall * 1000,
            'error': error}


def summarize(apps, langs, results, opt):
    """Reduce the runs to one entry per application and language, and check that the ports agree"""
    summary = {}
    for app in [EMPTY_APP] + apps:
        entries = {}
        for lang in langs:
            runs = results[app['name']][lang['key']]
            good = [run for run in runs if run['error'] is None]
            best = min(good, key=lambda run: run['app_ms']) if good else min(runs, key=lambda run: run['wall_ms'])
            entries[lang['key']] = {
                'n': workload(app, opt.scale),
                'app_ms': best['app_ms'],
                'wall_ms': min(run['wall_ms'] for run in runs),
                'user_ms': best['user_ms'],
                'sys_ms': best['sys_ms'],
                'rss_mb': max(run['rss_bytes'] for run in runs) / (1024 * 1024),
                'result': best['result'],
                'error': None if good else runs[0]['error'],
                'runs': len(runs),
            }
        # The expected result is the one most ports agree on
        votes = {}
        for entry in entries.values():
            if entry['error'] is None:
                votes[entry['result']] = votes.get(entry['result'], 0) + 1
        expected = max(votes, key=votes.get) if votes else None
        for entry in entries.values():
            entry['ok'] = entry['error'] is None and entry['result'] == expected
            entry['expected'] = expected
        summary[app['name']] = entries
    return summary


def report_check(summary):
    failed = 0
    for name, entries in summary.items():
        for key, entry in entries.items():
            if not entry['ok']:
                failed += 1
                detail = entry['error'] if entry['error'] else f"got {entry['result']}, expected {entry['expected']}"
                print(f'perfx: {name} {key}: {detail}')
    print('perfx: all ports agree' if failed == 0 else f'perfx: {failed} port(s) disagree or failed')
    return 1 if failed else 0


def version(cmd):
    try:
        text = subprocess.run(cmd, capture_output=True, text=True, timeout=30).stdout.strip()
    except (OSError, subprocess.SubprocessError):
        return 'unknown'
    text = text.splitlines()[0] if text else 'unknown'
    # Keep the version, not the copyright notice or build platform that some interpreters append
    return text.split(' Copyright')[0].split(' [')[0].strip()


def machine():
    cpu = ''
    if sys.platform == 'darwin':
        cpu = version(['sysctl', '-n', 'machdep.cpu.brand_string'])
    elif os.path.exists('/proc/cpuinfo'):
        with open('/proc/cpuinfo', encoding='utf-8') as fh:
            for line in fh:
                if line.startswith('model name'):
                    cpu = line.split(':', 1)[1].strip()
                    break
    return f'{cpu or platform.machine()}, {os.cpu_count()} cores, {platform.system()} {platform.release()}'


def scores(table):
    """
    Score each row of a table of measurements (row name -> list of values, None where missing)
    relative to the best row: the row effect of a multiplicative model, value = row x column,
    fitted by least squares on the log scale, so a missing cell neither rewards nor penalizes a
    row. For a complete table this is the geometric mean of each row's ratio to any fixed
    reference. Returns row name -> score, 1.0 for the best row, inf for a row with no values.
    """
    rows = [name for name, values in table.items() if any(v is not None for v in values)]
    if not rows:
        return {name: float('inf') for name in table}
    columns = range(len(table[rows[0]]))
    effect = {name: 0.0 for name in rows}
    scale = [0.0] * len(columns)
    for _ in range(200):
        for name in rows:
            logs = [math.log(table[name][j]) - scale[j] for j in columns if table[name][j] is not None]
            effect[name] = sum(logs) / len(logs)
        for j in columns:
            logs = [math.log(table[name][j]) - effect[name] for name in rows if table[name][j] is not None]
            if logs:
                scale[j] = sum(logs) / len(logs)
    best = min(effect.values())
    result = {name: math.exp(effect[name] - best) for name in rows}
    for name in table:
        result.setdefault(name, float('inf'))
    return result


def fmt_ms(value):
    if value >= 1000:
        return f'{value / 1000:,.2f} s'
    if value >= 100:
        return f'{value:,.0f} ms'
    return f'{value:,.1f} ms'


def fmt_mb(value):
    return f'{value:,.1f} MB'


def bar(ratio, width=24):
    return '#' * max(1, round(ratio * width))


def table(headers, rows, align):
    """A Markdown table; align is a string of 'l' and 'r' per column"""
    widths = [max(len(str(cell)) for cell in column) for column in zip(headers, *rows)]
    def line(cells):
        return '| ' + ' | '.join(str(cell).rjust(w) if a == 'r' else str(cell).ljust(w)
                                 for cell, w, a in zip(cells, widths, align)) + ' |'
    rule = '| ' + ' | '.join(('-' * (w - 1) + ':') if a == 'r' else ('-' * w) for w, a in zip(widths, align)) + ' |'
    return '\n'.join([line(headers), rule] + [line(row) for row in rows])


def report(summary, apps, langs, versions, opt):
    lines = []
    lines.append('# perfx report')
    lines.append('')
    lines.append(f'Generated {datetime.datetime.now().strftime("%Y-%m-%d %H:%M")} on {machine()}. '
                 f'Each figure is the best of {opt.runs} run{"s" if opt.runs != 1 else ""}'
                 f'{f", workload scale {opt.scale:g}" if opt.scale != 1 else ""}. Lower is better throughout.')
    lines.append('')

    # Interpreters
    lines.append('## Interpreters')
    lines.append('')
    rows = [[lang['name'], '`' + ' '.join(lang['cmd'](opt, 'app.' + lang['ext'], 'n')).replace(REPO_DIR + '/', '') + '`',
             versions[lang['key']]] for lang in langs]
    lines.append(table(['Language', 'Command', 'Version'], rows, 'lll'))
    lines.append('')

    # Summary: application time per language, scored against the best language
    fastest = {app['name']: min(summary[app['name']][lang['key']]['app_ms'] for lang in langs
                                if summary[app['name']][lang['key']]['ok']) for app in apps}
    geomeans = scores({lang['key']: [summary[app['name']][lang['key']]['app_ms']
                                     if summary[app['name']][lang['key']]['ok'] else None for app in apps]
                       for lang in langs})
    order = sorted(langs, key=lambda lang: geomeans[lang['key']])
    slowest_geomean = max(g for g in geomeans.values() if g != float('inf'))

    lines.append('## Summary')
    lines.append('')
    lines.append('Application time is measured inside each program, from its first statement to its result, so it '
                 'excludes interpreter startup. The last column scores each language against the best one: its '
                 'geometric mean across the applications, each application weighted equally, relative to the '
                 'language with the lowest mean. 1.00x is the best language; differences under about 5% are '
                 'within run-to-run drift.')
    lines.append('')
    headers = ['Language'] + [app['name'] for app in apps] + ['vs best']
    rows = []
    for lang in order:
        row = [lang['name']]
        for app in apps:
            entry = summary[app['name']][lang['key']]
            cell = fmt_ms(entry['app_ms']) if entry['ok'] else 'failed'
            if entry['ok'] and entry['app_ms'] == fastest[app['name']]:
                cell = '**' + cell + '**'
            row.append(cell)
        g = geomeans[lang['key']]
        row.append(f'{g:.2f}x' if g != float('inf') else '-')
        rows.append(row)
    lines.append(table(headers, rows, 'l' + 'r' * (len(apps) + 1)))
    lines.append('')
    lines.append('```')
    width = max(len(lang['name']) for lang in langs)
    for lang in order:
        g = geomeans[lang['key']]
        if g != float('inf'):
            lines.append(f"{lang['name']:<{width}}  {bar(g / slowest_geomean)} {g:.2f}x")
    lines.append('```')
    lines.append('')

    # Startup and baseline memory
    lines.append('## Startup')
    lines.append('')
    lines.append('The empty program prints its result and exits. Its wall time is what launching the interpreter '
                 'costs before any work is done, and its peak memory is the interpreter\'s baseline footprint, '
                 'which the application tables below subtract from their own peaks.')
    lines.append('')
    rows = []
    for lang in sorted(langs, key=lambda lang: summary['empty'][lang['key']]['wall_ms']):
        e = summary['empty'][lang['key']]
        rows.append([lang['name'], fmt_ms(e['wall_ms']), fmt_ms(e['user_ms']), fmt_ms(e['sys_ms']), fmt_mb(e['rss_mb'])])
    lines.append(table(['Language', 'Wall', 'User CPU', 'System CPU', 'Peak RSS'], rows, 'lrrrr'))
    lines.append('')

    # Each application
    lines.append('## Applications')
    lines.append('')
    for app in apps:
        entries = summary[app['name']]
        lines.append(f"### {app['name']} - {app['title']}")
        lines.append('')
        lines.append(f"{app['desc']} Workload: n = {entries[langs[0]['key']]['n']:,} {app['unit']}.")
        lines.append('')
        rows = []
        app_order = sorted(langs, key=lambda lang: (not entries[lang['key']]['ok'], entries[lang['key']]['app_ms']))
        for lang in app_order:
            e = entries[lang['key']]
            base = summary['empty'][lang['key']]['rss_mb']
            if not e['ok']:
                rows.append([lang['name'], 'failed', '-', fmt_ms(e['wall_ms']), '-', '-', '-', fmt_mb(e['rss_mb']), '-'])
                continue
            ratio = e['app_ms'] / fastest[app['name']]
            rows.append([lang['name'], fmt_ms(e['app_ms']), f'{ratio:.2f}x', fmt_ms(e['wall_ms']),
                         fmt_ms(e['wall_ms'] - e['app_ms']), fmt_ms(e['user_ms']), fmt_ms(e['sys_ms']),
                         fmt_mb(e['rss_mb']), fmt_mb(max(0.0, e['rss_mb'] - base))])
        lines.append(table(['Language', 'App time', 'vs fastest', 'Wall', 'Wall - app', 'User CPU', 'System CPU',
                            'Peak RSS', 'Above baseline'], rows, 'lrrrrrrrr'))
        lines.append('')
        problems = [lang for lang in langs if not entries[lang['key']]['ok']]
        if problems:
            for lang in problems:
                e = entries[lang['key']]
                detail = e['error'] if e['error'] else f"result `{e['result']}` differs from `{e['expected']}`"
                lines.append(f"- **{lang['name']} failed verification:** {detail}")
        else:
            lines.append(f"All ports computed the same result: `{entries[langs[0]['key']]['result']}`.")
        lines.append('')

    # Memory across applications
    lines.append('## Memory')
    lines.append('')
    lines.append('Peak resident set size per application, in MB. The baseline is the empty program; the '
                 'difference between an application\'s peak and the baseline is the memory the application '
                 'itself needed on that runtime, allocator overhead included. The last column scores each '
                 'language against the smallest, the same way as the summary.')
    lines.append('')
    memory = {lang['key']: [summary['empty'][lang['key']]['rss_mb']] +
              [summary[app['name']][lang['key']]['rss_mb'] for app in apps] for lang in langs}
    memory_scores = scores(memory)
    headers = ['Language', 'Baseline'] + [app['name'] for app in apps] + ['vs best']
    rows = []
    for lang in sorted(langs, key=lambda lang: memory_scores[lang['key']]):
        row = [lang['name'], fmt_mb(summary['empty'][lang['key']]['rss_mb'])]
        for app in apps:
            row.append(fmt_mb(summary[app['name']][lang['key']]['rss_mb']))
        row.append(f"{memory_scores[lang['key']]:.2f}x")
        rows.append(row)
    lines.append(table(headers, rows, 'l' + 'r' * (len(apps) + 2)))
    lines.append('')

    # How to read it
    lines.append('## Reading the report')
    lines.append('')
    lines.append('- **App time** is reported by the program itself from a monotonic clock around all of its work, '
                 'input generation included, so it is the time the language spends running the application. '
                 'Lua has no sub-second wall clock in its standard library, so its app time is CPU time from '
                 '`os.clock`, which for a single-threaded program is the same figure.')
    lines.append('- **Wall** is the whole process as the runner saw it, from spawn to exit. **Wall - app** is what '
                 'remains: interpreter startup, loading and compiling the source, and teardown. Compare it with '
                 'the Startup table.')
    lines.append('- **User CPU** and **System CPU** come from the kernel\'s resource usage for the process. User '
                 'time above wall time means the runtime used more than one thread (V8 compiles and collects '
                 'garbage on background threads). System time is kernel work: mapping memory, reading files.')
    lines.append('- **Peak RSS** is the largest resident set the process reached. **Above baseline** subtracts '
                 'the empty program\'s peak for the same runtime, isolating what the application\'s data cost.')
    lines.append('- **vs fastest** in an application table divides a port\'s app time by the fastest port\'s '
                 'for that application. **vs best** in the summary and memory tables is a score: each language\'s '
                 'geometric mean across the tests relative to the best language, with every test weighted '
                 'equally and each test\'s scale estimated from all the languages that ran it (a least-squares '
                 'fit of measurement = language x test on the log scale), so a failed port does not distort '
                 'the others. 1.00x is the best language.')
    lines.append('- Every port prints its computed result, and the report flags any port whose result differs '
                 'from the others - a timing is only meaningful when the ports did the same work.')
    lines.append('')
    return '\n'.join(lines)


if __name__ == '__main__':
    sys.exit(main())
