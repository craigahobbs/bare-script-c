-- loganalyze - web server log analysis (regular expressions, string splitting, hash maps)
--
-- Generates n lines of an Apache-style access log with a deterministic PRNG, then parses every
-- line with a pattern and aggregates: unique client addresses, status code counts, requests per
-- hour, bytes per path prefix, and the busiest hour and heaviest paths.

local WORDS = {'perf', 'bare', 'script', 'lua', 'ruby', 'perl', 'python', 'node'}
local SLUGS = {'hello-world', 'release-notes', 'faq', 'roadmap', 'benchmarks'}
local LINE_PATTERN = '^(%S+) %S+ %S+ %[(%d+)/(%w+)/(%d+):(%d+):(%d+):(%d+) [^%]]+%] "(%w+) (%S+) [^"]+" (%d+) (%d+)$'

local seed = 42

local function randInt(n)
    seed = (seed * 48271) % 2147483647
    return seed % n
end

local function pad2(n)
    if n < 10 then
        return '0' .. n
    end
    return '' .. n
end

local function generateLog(count)
    local lines = {}
    for i = 1, count do
        local ip = '10.' .. randInt(8) .. '.' .. randInt(256) .. '.' .. randInt(256)
        local day = 1 + randInt(30)
        local hour = randInt(24)
        local minute = randInt(60)
        local second = randInt(60)
        local method = 'GET'
        if randInt(10) == 0 then
            method = 'POST'
        end
        local kind = randInt(12)
        local path
        if kind == 0 then
            path = '/'
        elseif kind == 1 then
            path = '/index.html'
        elseif kind == 2 then
            path = '/api/users'
        elseif kind == 3 then
            path = '/api/users/' .. randInt(1000)
        elseif kind == 4 then
            path = '/api/orders'
        elseif kind == 5 then
            path = '/static/app.js'
        elseif kind == 6 then
            path = '/static/style.css'
        elseif kind == 7 then
            path = '/images/logo.png'
        elseif kind == 8 then
            path = '/login'
        elseif kind == 9 then
            path = '/logout'
        elseif kind == 10 then
            path = '/search?q=' .. WORDS[randInt(8) + 1]
        else
            path = '/blog/' .. SLUGS[randInt(5) + 1]
        end
        local pick = randInt(100)
        local status
        if pick < 80 then
            status = 200
        elseif pick < 88 then
            status = 304
        elseif pick < 95 then
            status = 404
        elseif pick < 98 then
            status = 302
        else
            status = 500
        end
        local size = randInt(50000)
        if status == 304 then
            size = 0
        end
        lines[i] = ip .. ' - - [' .. pad2(day) .. '/Sep/2026:' .. pad2(hour) .. ':' .. pad2(minute) .. ':' .. pad2(second) ..
                   ' +0000] "' .. method .. ' ' .. path .. ' HTTP/1.1" ' .. status .. ' ' .. size
    end
    return table.concat(lines, '\n')
end

local function pathKey(path)
    local q = string.find(path, '?', 1, true)
    if q then
        path = string.sub(path, 1, q - 1)
    end
    local parts = {}
    for part in string.gmatch(path .. '/', '([^/]*)/') do
        parts[#parts + 1] = part
    end
    if #parts > 3 then
        path = '/' .. parts[2] .. '/' .. parts[3]
    end
    return path
end

local function analyze(text)
    local total, bad, posts, errors, totalBytes = 0, 0, 0, 0, 0
    local ips = {}
    local ipCount = 0
    local statuses = {}
    local hours = {}
    for hour = 0, 23 do
        hours[hour] = 0
    end
    local pathBytes = {}
    for line in string.gmatch(text, '[^\n]+') do
        local ip, _, _, _, hourText, _, _, method, path, statusText, sizeText = string.match(line, LINE_PATTERN)
        if ip == nil then
            bad = bad + 1
        else
            local hour = tonumber(hourText)
            local status = tonumber(statusText)
            local size = tonumber(sizeText)
            total = total + 1
            if not ips[ip] then
                ips[ip] = true
                ipCount = ipCount + 1
            end
            statuses[status] = (statuses[status] or 0) + 1
            hours[hour] = hours[hour] + 1
            if method == 'POST' then
                posts = posts + 1
            end
            if status >= 500 then
                errors = errors + 1
            end
            totalBytes = totalBytes + size
            local key = pathKey(path)
            pathBytes[key] = (pathBytes[key] or 0) + size
        end
    end

    local peakHour = 0
    for hour = 0, 23 do
        if hours[hour] > hours[peakHour] then
            peakHour = hour
        end
    end
    local entries = {}
    for key, size in pairs(pathBytes) do
        entries[#entries + 1] = {key, size}
    end
    table.sort(entries, function(a, b)
        if a[2] ~= b[2] then
            return a[2] > b[2]
        end
        return a[1] < b[1]
    end)
    local parts = {total, bad, ipCount, posts, errors, totalBytes, statuses[200] or 0, statuses[304] or 0,
                   statuses[404] or 0, peakHour .. ':' .. hours[peakHour]}
    for ix = 1, math.min(3, #entries) do
        parts[#parts + 1] = entries[ix][1] .. ':' .. entries[ix][2]
    end
    return table.concat(parts, ',')
end

local function main()
    local n = tonumber(arg[1]) or 300000
    local t0 = os.clock()
    local text = generateLog(n)
    local result = analyze(text)
    local t1 = os.clock()
    print('result: ' .. result)
    print(string.format('time: %.3f', (t1 - t0) * 1000))
end

main()
