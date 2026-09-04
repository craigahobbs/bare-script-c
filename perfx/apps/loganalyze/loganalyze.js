// loganalyze - web server log analysis (regular expressions, string splitting, hash maps)
//
// Generates n lines of an Apache-style access log with a deterministic PRNG, then parses every
// line with a regular expression and aggregates: unique client addresses, status code counts,
// requests per hour, bytes per path prefix, and the busiest hour and heaviest paths.

'use strict';

const WORDS = ['perf', 'bare', 'script', 'lua', 'ruby', 'perl', 'python', 'node'];
const SLUGS = ['hello-world', 'release-notes', 'faq', 'roadmap', 'benchmarks'];
const LINE_RE = /^(\S+) \S+ \S+ \[(\d+)\/(\w+)\/(\d+):(\d+):(\d+):(\d+) [^\]]+\] "(\w+) (\S+) [^"]+" (\d+) (\d+)$/;

let seed = 42;

function randInt(n) {
    seed = (seed * 48271) % 2147483647;
    return seed % n;
}

function pad2(n) {
    return n < 10 ? '0' + n : '' + n;
}

function generateLog(count) {
    const lines = [];
    for (let i = 0; i < count; i++) {
        const ip = '10.' + randInt(8) + '.' + randInt(256) + '.' + randInt(256);
        const day = 1 + randInt(30);
        const hour = randInt(24);
        const minute = randInt(60);
        const second = randInt(60);
        const method = randInt(10) === 0 ? 'POST' : 'GET';
        const kind = randInt(12);
        let path;
        if (kind === 0) {
            path = '/';
        } else if (kind === 1) {
            path = '/index.html';
        } else if (kind === 2) {
            path = '/api/users';
        } else if (kind === 3) {
            path = '/api/users/' + randInt(1000);
        } else if (kind === 4) {
            path = '/api/orders';
        } else if (kind === 5) {
            path = '/static/app.js';
        } else if (kind === 6) {
            path = '/static/style.css';
        } else if (kind === 7) {
            path = '/images/logo.png';
        } else if (kind === 8) {
            path = '/login';
        } else if (kind === 9) {
            path = '/logout';
        } else if (kind === 10) {
            path = '/search?q=' + WORDS[randInt(8)];
        } else {
            path = '/blog/' + SLUGS[randInt(5)];
        }
        const pick = randInt(100);
        let status;
        if (pick < 80) {
            status = 200;
        } else if (pick < 88) {
            status = 304;
        } else if (pick < 95) {
            status = 404;
        } else if (pick < 98) {
            status = 302;
        } else {
            status = 500;
        }
        let size = randInt(50000);
        if (status === 304) {
            size = 0;
        }
        lines.push(ip + ' - - [' + pad2(day) + '/Sep/2026:' + pad2(hour) + ':' + pad2(minute) + ':' + pad2(second) +
                   ' +0000] "' + method + ' ' + path + ' HTTP/1.1" ' + status + ' ' + size);
    }
    return lines.join('\n');
}

function pathKey(path) {
    const q = path.indexOf('?');
    if (q >= 0) {
        path = path.slice(0, q);
    }
    const parts = path.split('/');
    if (parts.length > 3) {
        path = '/' + parts[1] + '/' + parts[2];
    }
    return path;
}

function analyze(text) {
    let total = 0;
    let bad = 0;
    let posts = 0;
    let errors = 0;
    let totalBytes = 0;
    const ips = new Map();
    const statuses = new Map();
    const hours = new Array(24).fill(0);
    const pathBytes = new Map();
    for (const line of text.split('\n')) {
        const m = LINE_RE.exec(line);
        if (m === null) {
            bad++;
            continue;
        }
        const ip = m[1];
        const hour = parseInt(m[5], 10);
        const method = m[8];
        const path = m[9];
        const status = parseInt(m[10], 10);
        const size = parseInt(m[11], 10);
        total++;
        ips.set(ip, true);
        statuses.set(status, (statuses.get(status) || 0) + 1);
        hours[hour]++;
        if (method === 'POST') {
            posts++;
        }
        if (status >= 500) {
            errors++;
        }
        totalBytes += size;
        const key = pathKey(path);
        pathBytes.set(key, (pathBytes.get(key) || 0) + size);
    }

    let peakHour = 0;
    for (let hour = 0; hour < 24; hour++) {
        if (hours[hour] > hours[peakHour]) {
            peakHour = hour;
        }
    }
    const top = [...pathBytes.entries()].sort((a, b) => (b[1] - a[1]) || (a[0] < b[0] ? -1 : (a[0] > b[0] ? 1 : 0))).slice(0, 3);
    const parts = [total, bad, ips.size, posts, errors, totalBytes, statuses.get(200) || 0, statuses.get(304) || 0,
                   statuses.get(404) || 0, peakHour + ':' + hours[peakHour]];
    for (const [key, size] of top) {
        parts.push(key + ':' + size);
    }
    return parts.join(',');
}

function main() {
    const n = process.argv.length > 2 ? parseInt(process.argv[2], 10) : 300000;
    const t0 = performance.now();
    const text = generateLog(n);
    const result = analyze(text);
    const t1 = performance.now();
    console.log('result: ' + result);
    console.log('time: ' + (t1 - t0).toFixed(3));
}

main();
