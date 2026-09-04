# pathfind - shortest path across a weighted grid (arrays, a binary heap, integer arithmetic)
#
# Builds an n x n grid of terrain costs with a fifth of the cells blocked, then runs Dijkstra's
# algorithm from the top-left corner to the bottom-right with a hand-written binary heap and
# reports the path cost, the number of settled cells, and the path length.

INF = 10**15

$seed = 42

def rand_int(n)
  $seed = ($seed * 48271) % 2147483647
  $seed % n
end

def generate_grid(width, height)
  cells = width * height
  grid = Array.new(cells, 0)
  cells.times do |i|
    cost = 1 + rand_int(9)
    cost = -1 if rand_int(100) < 20
    grid[i] = cost
  end
  # The corners and their neighbors stay open so the search always leaves the start
  [0, 1, width, cells - 1, cells - 2, cells - 1 - width].each do |i|
    grid[i] = 1 if grid[i] < 0
  end
  grid
end

def heap_push(hd, hn, d, node)
  hd << d
  hn << node
  i = hd.length - 1
  while i > 0
    p = (i - 1) / 2
    break if hd[p] <= hd[i]

    hd[p], hd[i] = hd[i], hd[p]
    hn[p], hn[i] = hn[i], hn[p]
    i = p
  end
end

def heap_pop(hd, hn)
  top_d = hd[0]
  top_n = hn[0]
  last_d = hd.pop
  last_n = hn.pop
  size = hd.length
  if size > 0
    hd[0] = last_d
    hn[0] = last_n
    i = 0
    loop do
      left = 2 * i + 1
      right = left + 1
      m = i
      m = left if left < size && hd[left] < hd[m]
      m = right if right < size && hd[right] < hd[m]
      break if m == i

      hd[m], hd[i] = hd[i], hd[m]
      hn[m], hn[i] = hn[i], hn[m]
      i = m
    end
  end
  [top_d, top_n]
end

def relax(grid, dist, prev, hd, hn, d, u, v)
  cost = grid[v]
  return if cost < 0

  nd = d + cost
  if nd < dist[v]
    dist[v] = nd
    prev[v] = u
    heap_push(hd, hn, nd, v)
  end
end

def shortest_path(grid, width, height)
  cells = width * height
  goal = cells - 1
  dist = Array.new(cells, INF)
  prev = Array.new(cells, -1)
  dist[0] = 0
  hd = []
  hn = []
  heap_push(hd, hn, 0, 0)
  settled = 0
  while hd.length > 0
    d, u = heap_pop(hd, hn)
    next if d > dist[u]

    settled += 1
    break if u == goal

    x = u % width
    y = u / width
    relax(grid, dist, prev, hd, hn, d, u, u - width) if y > 0
    relax(grid, dist, prev, hd, hn, d, u, u + width) if y < height - 1
    relax(grid, dist, prev, hd, hn, d, u, u - 1) if x > 0
    relax(grid, dist, prev, hd, hn, d, u, u + 1) if x < width - 1
  end
  return "unreachable,#{settled}" if dist[goal] == INF

  length = 0
  node = goal
  while node >= 0
    length += 1
    node = prev[node]
  end
  "#{dist[goal]},#{settled},#{length}"
end

def main
  n = ARGV.length > 0 ? ARGV[0].to_i : 800
  t0 = Process.clock_gettime(Process::CLOCK_MONOTONIC)
  grid = generate_grid(n, n)
  result = shortest_path(grid, n, n)
  t1 = Process.clock_gettime(Process::CLOCK_MONOTONIC)
  puts "result: #{result}"
  puts format('time: %.3f', (t1 - t0) * 1000)
end

main
