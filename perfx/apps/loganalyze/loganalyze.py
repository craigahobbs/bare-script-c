# loganalyze - web server log analysis (regular expressions, string splitting, hash maps)
#
# Generates n lines of an Apache-style access log with a deterministic PRNG, then parses every
# line with a regular expression and aggregates: unique client addresses, status code counts,
# requests per hour, bytes per path prefix, and the busiest hour and heaviest paths.

import re
import sys
import time

WORDS = ['perf', 'bare', 'script', 'lua', 'ruby', 'perl', 'python', 'node']
SLUGS = ['hello-world', 'release-notes', 'faq', 'roadmap', 'benchmarks']
LINE_RE = re.compile(r'^(\S+) \S+ \S+ \[(\d+)/(\w+)/(\d+):(\d+):(\d+):(\d+) [^\]]+\] "(\w+) (\S+) [^"]+" (\d+) (\d+)$')

seed = 42


def rand_int(n):
    global seed
    seed = (seed * 48271) % 2147483647
    return seed % n


def pad2(n):
    return ('0' + str(n)) if n < 10 else str(n)


def generate_log(count):
    lines = []
    for _ in range(count):
        ip = '10.' + str(rand_int(8)) + '.' + str(rand_int(256)) + '.' + str(rand_int(256))
        day = 1 + rand_int(30)
        hour = rand_int(24)
        minute = rand_int(60)
        second = rand_int(60)
        method = 'POST' if rand_int(10) == 0 else 'GET'
        kind = rand_int(12)
        if kind == 0:
            path = '/'
        elif kind == 1:
            path = '/index.html'
        elif kind == 2:
            path = '/api/users'
        elif kind == 3:
            path = '/api/users/' + str(rand_int(1000))
        elif kind == 4:
            path = '/api/orders'
        elif kind == 5:
            path = '/static/app.js'
        elif kind == 6:
            path = '/static/style.css'
        elif kind == 7:
            path = '/images/logo.png'
        elif kind == 8:
            path = '/login'
        elif kind == 9:
            path = '/logout'
        elif kind == 10:
            path = '/search?q=' + WORDS[rand_int(8)]
        else:
            path = '/blog/' + SLUGS[rand_int(5)]
        pick = rand_int(100)
        if pick < 80:
            status = 200
        elif pick < 88:
            status = 304
        elif pick < 95:
            status = 404
        elif pick < 98:
            status = 302
        else:
            status = 500
        size = rand_int(50000)
        if status == 304:
            size = 0
        lines.append(ip + ' - - [' + pad2(day) + '/Sep/2026:' + pad2(hour) + ':' + pad2(minute) + ':' + pad2(second) +
                     ' +0000] "' + method + ' ' + path + ' HTTP/1.1" ' + str(status) + ' ' + str(size))
    return '\n'.join(lines)


def path_key(path):
    q = path.find('?')
    if q >= 0:
        path = path[:q]
    parts = path.split('/')
    if len(parts) > 3:
        path = '/' + parts[1] + '/' + parts[2]
    return path


def analyze(text):
    total = 0
    bad = 0
    posts = 0
    errors = 0
    total_bytes = 0
    ips = {}
    statuses = {}
    hours = [0] * 24
    path_bytes = {}
    for line in text.split('\n'):
        m = LINE_RE.match(line)
        if m is None:
            bad += 1
            continue
        ip = m.group(1)
        hour = int(m.group(5))
        method = m.group(8)
        path = m.group(9)
        status = int(m.group(10))
        size = int(m.group(11))
        total += 1
        ips[ip] = True
        statuses[status] = statuses.get(status, 0) + 1
        hours[hour] += 1
        if method == 'POST':
            posts += 1
        if status >= 500:
            errors += 1
        total_bytes += size
        key = path_key(path)
        path_bytes[key] = path_bytes.get(key, 0) + size

    peak_hour = 0
    for hour in range(24):
        if hours[hour] > hours[peak_hour]:
            peak_hour = hour
    top = sorted(path_bytes.items(), key=lambda kv: (-kv[1], kv[0]))[:3]
    parts = [total, bad, len(ips), posts, errors, total_bytes, statuses.get(200, 0), statuses.get(304, 0),
             statuses.get(404, 0), str(peak_hour) + ':' + str(hours[peak_hour])]
    parts.extend(key + ':' + str(size) for key, size in top)
    return ','.join(str(part) for part in parts)


def main():
    n = int(sys.argv[1]) if len(sys.argv) > 1 else 300000
    t0 = time.perf_counter()
    text = generate_log(n)
    result = analyze(text)
    t1 = time.perf_counter()
    print('result: ' + result)
    print('time: ' + str(round((t1 - t0) * 1000, 3)))


main()
