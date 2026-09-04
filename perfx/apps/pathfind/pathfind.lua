-- pathfind - shortest path across a weighted grid (arrays, a binary heap, integer arithmetic)
--
-- Builds an n x n grid of terrain costs with a fifth of the cells blocked, then runs Dijkstra's
-- algorithm from the top-left corner to the bottom-right with a hand-written binary heap and
-- reports the path cost, the number of settled cells, and the path length.
--
-- Cells are numbered from zero as in the other ports; Lua's one-based tables are offset by one.

local INF = 1e15

local seed = 42

local function randInt(n)
    seed = (seed * 48271) % 2147483647
    return seed % n
end

local function generateGrid(width, height)
    local cells = width * height
    local grid = {}
    for i = 1, cells do
        local cost = 1 + randInt(9)
        if randInt(100) < 20 then
            cost = -1
        end
        grid[i] = cost
    end
    -- The corners and their neighbors stay open so the search always leaves the start
    for _, i in ipairs({0, 1, width, cells - 1, cells - 2, cells - 1 - width}) do
        if grid[i + 1] < 0 then
            grid[i + 1] = 1
        end
    end
    return grid
end

local function heapPush(hd, hn, d, node)
    local i = #hd + 1
    hd[i] = d
    hn[i] = node
    i = i - 1
    while i > 0 do
        local p = (i - 1) // 2
        if hd[p + 1] <= hd[i + 1] then
            break
        end
        hd[p + 1], hd[i + 1] = hd[i + 1], hd[p + 1]
        hn[p + 1], hn[i + 1] = hn[i + 1], hn[p + 1]
        i = p
    end
end

local function heapPop(hd, hn)
    local topD = hd[1]
    local topN = hn[1]
    local size = #hd
    local lastD = hd[size]
    local lastN = hn[size]
    hd[size] = nil
    hn[size] = nil
    size = size - 1
    if size > 0 then
        hd[1] = lastD
        hn[1] = lastN
        local i = 0
        while true do
            local left = 2 * i + 1
            local right = left + 1
            local m = i
            if left < size and hd[left + 1] < hd[m + 1] then
                m = left
            end
            if right < size and hd[right + 1] < hd[m + 1] then
                m = right
            end
            if m == i then
                break
            end
            hd[m + 1], hd[i + 1] = hd[i + 1], hd[m + 1]
            hn[m + 1], hn[i + 1] = hn[i + 1], hn[m + 1]
            i = m
        end
    end
    return topD, topN
end

local function relax(grid, dist, prev, hd, hn, d, u, v)
    local cost = grid[v + 1]
    if cost < 0 then
        return
    end
    local nd = d + cost
    if nd < dist[v + 1] then
        dist[v + 1] = nd
        prev[v + 1] = u
        heapPush(hd, hn, nd, v)
    end
end

local function shortestPath(grid, width, height)
    local cells = width * height
    local goal = cells - 1
    local dist = {}
    local prev = {}
    for i = 1, cells do
        dist[i] = INF
        prev[i] = -1
    end
    dist[1] = 0
    local hd = {}
    local hn = {}
    heapPush(hd, hn, 0, 0)
    local settled = 0
    while #hd > 0 do
        local d, u = heapPop(hd, hn)
        if d <= dist[u + 1] then
            settled = settled + 1
            if u == goal then
                break
            end
            local x = u % width
            local y = u // width
            if y > 0 then
                relax(grid, dist, prev, hd, hn, d, u, u - width)
            end
            if y < height - 1 then
                relax(grid, dist, prev, hd, hn, d, u, u + width)
            end
            if x > 0 then
                relax(grid, dist, prev, hd, hn, d, u, u - 1)
            end
            if x < width - 1 then
                relax(grid, dist, prev, hd, hn, d, u, u + 1)
            end
        end
    end
    if dist[goal + 1] == INF then
        return 'unreachable,' .. settled
    end
    local length = 0
    local node = goal
    while node >= 0 do
        length = length + 1
        node = prev[node + 1]
    end
    return dist[goal + 1] .. ',' .. settled .. ',' .. length
end

local function main()
    local n = tonumber(arg[1]) or 800
    local t0 = os.clock()
    local grid = generateGrid(n, n)
    local result = shortestPath(grid, n, n)
    local t1 = os.clock()
    print('result: ' .. result)
    print(string.format('time: %.3f', (t1 - t0) * 1000))
end

main()
