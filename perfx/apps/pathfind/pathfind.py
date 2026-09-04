# pathfind - shortest path across a weighted grid (arrays, a binary heap, integer arithmetic)
#
# Builds an n x n grid of terrain costs with a fifth of the cells blocked, then runs Dijkstra's
# algorithm from the top-left corner to the bottom-right with a hand-written binary heap and
# reports the path cost, the number of settled cells, and the path length.

import sys
import time

INF = 10 ** 15

seed = 42


def rand_int(n):
    global seed
    seed = (seed * 48271) % 2147483647
    return seed % n


def generate_grid(width, height):
    cells = width * height
    grid = [0] * cells
    for i in range(cells):
        cost = 1 + rand_int(9)
        if rand_int(100) < 20:
            cost = -1
        grid[i] = cost
    # The corners and their neighbors stay open so the search always leaves the start
    for i in (0, 1, width, cells - 1, cells - 2, cells - 1 - width):
        if grid[i] < 0:
            grid[i] = 1
    return grid


def heap_push(hd, hn, d, node):
    hd.append(d)
    hn.append(node)
    i = len(hd) - 1
    while i > 0:
        p = (i - 1) // 2
        if hd[p] <= hd[i]:
            break
        hd[p], hd[i] = hd[i], hd[p]
        hn[p], hn[i] = hn[i], hn[p]
        i = p


def heap_pop(hd, hn):
    top_d = hd[0]
    top_n = hn[0]
    last_d = hd.pop()
    last_n = hn.pop()
    size = len(hd)
    if size > 0:
        hd[0] = last_d
        hn[0] = last_n
        i = 0
        while True:
            left = 2 * i + 1
            right = left + 1
            m = i
            if left < size and hd[left] < hd[m]:
                m = left
            if right < size and hd[right] < hd[m]:
                m = right
            if m == i:
                break
            hd[m], hd[i] = hd[i], hd[m]
            hn[m], hn[i] = hn[i], hn[m]
            i = m
    return top_d, top_n


def shortest_path(grid, width, height):
    cells = width * height
    goal = cells - 1
    dist = [INF] * cells
    prev = [-1] * cells
    dist[0] = 0
    hd = []
    hn = []
    heap_push(hd, hn, 0, 0)
    settled = 0
    while len(hd) > 0:
        d, u = heap_pop(hd, hn)
        if d > dist[u]:
            continue
        settled += 1
        if u == goal:
            break
        x = u % width
        y = u // width
        if y > 0:
            relax(grid, dist, prev, hd, hn, d, u, u - width)
        if y < height - 1:
            relax(grid, dist, prev, hd, hn, d, u, u + width)
        if x > 0:
            relax(grid, dist, prev, hd, hn, d, u, u - 1)
        if x < width - 1:
            relax(grid, dist, prev, hd, hn, d, u, u + 1)
    if dist[goal] == INF:
        return 'unreachable,' + str(settled)
    length = 0
    node = goal
    while node >= 0:
        length += 1
        node = prev[node]
    return str(dist[goal]) + ',' + str(settled) + ',' + str(length)


def relax(grid, dist, prev, hd, hn, d, u, v):
    cost = grid[v]
    if cost < 0:
        return
    nd = d + cost
    if nd < dist[v]:
        dist[v] = nd
        prev[v] = u
        heap_push(hd, hn, nd, v)


def main():
    n = int(sys.argv[1]) if len(sys.argv) > 1 else 800
    t0 = time.perf_counter()
    grid = generate_grid(n, n)
    result = shortest_path(grid, n, n)
    t1 = time.perf_counter()
    print('result: ' + result)
    print('time: ' + str(round((t1 - t0) * 1000, 3)))


main()
