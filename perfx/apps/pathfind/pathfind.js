// pathfind - shortest path across a weighted grid (arrays, a binary heap, integer arithmetic)
//
// Builds an n x n grid of terrain costs with a fifth of the cells blocked, then runs Dijkstra's
// algorithm from the top-left corner to the bottom-right with a hand-written binary heap and
// reports the path cost, the number of settled cells, and the path length.

'use strict';

const INF = 1e15;

let seed = 42;

function randInt(n) {
    seed = (seed * 48271) % 2147483647;
    return seed % n;
}

function generateGrid(width, height) {
    const cells = width * height;
    const grid = new Array(cells);
    for (let i = 0; i < cells; i++) {
        let cost = 1 + randInt(9);
        if (randInt(100) < 20) {
            cost = -1;
        }
        grid[i] = cost;
    }
    // The corners and their neighbors stay open so the search always leaves the start
    for (const i of [0, 1, width, cells - 1, cells - 2, cells - 1 - width]) {
        if (grid[i] < 0) {
            grid[i] = 1;
        }
    }
    return grid;
}

function heapPush(hd, hn, d, node) {
    hd.push(d);
    hn.push(node);
    let i = hd.length - 1;
    while (i > 0) {
        const p = Math.floor((i - 1) / 2);
        if (hd[p] <= hd[i]) {
            break;
        }
        let t = hd[p]; hd[p] = hd[i]; hd[i] = t;
        t = hn[p]; hn[p] = hn[i]; hn[i] = t;
        i = p;
    }
}

function heapPop(hd, hn, out) {
    out.d = hd[0];
    out.node = hn[0];
    const lastD = hd.pop();
    const lastN = hn.pop();
    const size = hd.length;
    if (size > 0) {
        hd[0] = lastD;
        hn[0] = lastN;
        let i = 0;
        for (;;) {
            const left = 2 * i + 1;
            const right = left + 1;
            let m = i;
            if (left < size && hd[left] < hd[m]) {
                m = left;
            }
            if (right < size && hd[right] < hd[m]) {
                m = right;
            }
            if (m === i) {
                break;
            }
            let t = hd[m]; hd[m] = hd[i]; hd[i] = t;
            t = hn[m]; hn[m] = hn[i]; hn[i] = t;
            i = m;
        }
    }
}

function relax(grid, dist, prev, hd, hn, d, u, v) {
    const cost = grid[v];
    if (cost < 0) {
        return;
    }
    const nd = d + cost;
    if (nd < dist[v]) {
        dist[v] = nd;
        prev[v] = u;
        heapPush(hd, hn, nd, v);
    }
}

function shortestPath(grid, width, height) {
    const cells = width * height;
    const goal = cells - 1;
    const dist = new Array(cells).fill(INF);
    const prev = new Array(cells).fill(-1);
    dist[0] = 0;
    const hd = [];
    const hn = [];
    heapPush(hd, hn, 0, 0);
    let settled = 0;
    const top = {d: 0, node: 0};
    while (hd.length > 0) {
        heapPop(hd, hn, top);
        const d = top.d;
        const u = top.node;
        if (d > dist[u]) {
            continue;
        }
        settled++;
        if (u === goal) {
            break;
        }
        const x = u % width;
        const y = Math.floor(u / width);
        if (y > 0) {
            relax(grid, dist, prev, hd, hn, d, u, u - width);
        }
        if (y < height - 1) {
            relax(grid, dist, prev, hd, hn, d, u, u + width);
        }
        if (x > 0) {
            relax(grid, dist, prev, hd, hn, d, u, u - 1);
        }
        if (x < width - 1) {
            relax(grid, dist, prev, hd, hn, d, u, u + 1);
        }
    }
    if (dist[goal] === INF) {
        return 'unreachable,' + settled;
    }
    let length = 0;
    let node = goal;
    while (node >= 0) {
        length++;
        node = prev[node];
    }
    return dist[goal] + ',' + settled + ',' + length;
}

function main() {
    const n = process.argv.length > 2 ? parseInt(process.argv[2], 10) : 800;
    const t0 = performance.now();
    const grid = generateGrid(n, n);
    const result = shortestPath(grid, n, n);
    const t1 = performance.now();
    console.log('result: ' + result);
    console.log('time: ' + (t1 - t0).toFixed(3));
}

main();
