-- salesreport - CSV parsing, per-group statistics, and a fixed-width text report
--
-- Generates n rows of sales records as CSV text (with a quoted field), parses it with a
-- character-level CSV reader, computes revenue, mean, standard deviation, and the top product and
-- sales rep per region, and renders an aligned text report with formatted money amounts.

local REGIONS = {'North', 'South', 'East', 'West', 'Central', 'Overseas'}
local PRODUCTS = {'Anvil', 'Bolt Kit', 'Cable Tie', 'Drill', 'Epoxy', 'Fan Belt', 'Gasket', 'Hinge', 'Impeller', 'Jack',
                  'Kettle', 'Lamp', 'Motor', 'Nozzle', 'O-Ring', 'Pump', 'Quill', 'Rivet', 'Spring', 'Valve'}
local FIRSTS = {'Ada', 'Ben', 'Cleo', 'Dev', 'Eve', 'Finn', 'Gus', 'Hana'}
local LASTS = {'Ng', 'Ortiz', 'Patel', 'Quinn', 'Rossi'}
local REPS = {}
for k = 0, 39 do
    REPS[k + 1] = LASTS[k % 5 + 1] .. ', ' .. FIRSTS[k // 5 + 1]
end

local QUOTE = 34
local COMMA = 44
local NEWLINE = 10

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

local function generateCsv(count)
    local lines = {'id,date,region,product,units,price_cents,rep'}
    for i = 1, count do
        local month = 1 + randInt(12)
        local day = 1 + randInt(28)
        local region = REGIONS[randInt(6) + 1]
        local product = PRODUCTS[randInt(20) + 1]
        local units = 1 + randInt(50)
        local priceCents = 500 + randInt(49500)
        local rep = REPS[randInt(40) + 1]
        lines[#lines + 1] = i .. ',2026-' .. pad2(month) .. '-' .. pad2(day) .. ',' .. region .. ',' .. product .. ',' ..
                            units .. ',' .. priceCents .. ',"' .. rep .. '"'
    end
    return table.concat(lines, '\n')
end

local function parseCsv(text)
    local rows = {}
    local fields = {}
    local field = ''
    local inQuotes = false
    local size = #text
    local i = 1
    while i <= size do
        local code = string.byte(text, i)
        if inQuotes then
            if code == QUOTE then
                if i + 1 <= size and string.byte(text, i + 1) == QUOTE then
                    field = field .. '"'
                    i = i + 1
                else
                    inQuotes = false
                end
            else
                field = field .. string.sub(text, i, i)
            end
        elseif code == QUOTE then
            inQuotes = true
        elseif code == COMMA then
            fields[#fields + 1] = field
            field = ''
        elseif code == NEWLINE then
            fields[#fields + 1] = field
            rows[#rows + 1] = fields
            fields = {}
            field = ''
        else
            field = field .. string.sub(text, i, i)
        end
        i = i + 1
    end
    if field ~= '' or #fields > 0 then
        fields[#fields + 1] = field
        rows[#rows + 1] = fields
    end
    return rows
end

local function padLeft(text, width)
    text = '' .. text
    if #text < width then
        return string.rep(' ', width - #text) .. text
    end
    return text
end

local function padRight(text, width)
    text = '' .. text
    if #text < width then
        return text .. string.rep(' ', width - #text)
    end
    return text
end

local function money(cents)
    local dollars = '' .. (cents // 100)
    local out = ''
    local count = 0
    for i = #dollars, 1, -1 do
        out = string.sub(dollars, i, i) .. out
        count = count + 1
        if count % 3 == 0 and i > 1 then
            out = ',' .. out
        end
    end
    return '$' .. out .. '.' .. pad2(cents % 100)
end

local function topEntry(counts)
    local best = nil
    local bestValue = -1
    for key, value in pairs(counts) do
        if value > bestValue or (value == bestValue and key < best) then
            best = key
            bestValue = value
        end
    end
    return best
end

local function report(rows)
    local stats = {}
    for _, region in ipairs(REGIONS) do
        stats[region] = {rows = 0, units = 0, revenue = 0, revenues = {}, products = {}, reps = {}}
    end
    local dataRows = 0
    for ix = 2, #rows do
        local fields = rows[ix]
        local region = fields[3]
        local product = fields[4]
        local units = tonumber(fields[5])
        local priceCents = tonumber(fields[6])
        local rep = fields[7]
        local revenue = units * priceCents
        local s = stats[region]
        s.rows = s.rows + 1
        s.units = s.units + units
        s.revenue = s.revenue + revenue
        s.revenues[#s.revenues + 1] = revenue
        s.products[product] = (s.products[product] or 0) + revenue
        s.reps[rep] = (s.reps[rep] or 0) + revenue
        dataRows = dataRows + 1
    end

    local lines = {'Sales report: ' .. dataRows .. ' rows', '',
                   padRight('Region', 10) .. padLeft('Rows', 7) .. padLeft('Units', 8) .. padLeft('Revenue', 16) ..
                   padLeft('Mean', 12) .. padLeft('Std dev', 12) .. '  ' .. padRight('Top product', 12) .. 'Top rep'}
    local totalRows, totalUnits, totalRevenue = 0, 0, 0
    for _, region in ipairs(REGIONS) do
        local s = stats[region]
        if s.rows > 0 then
            local meanF = s.revenue / s.rows
            local variance = 0.0
            for _, revenue in ipairs(s.revenues) do
                local diff = revenue - meanF
                variance = variance + diff * diff
            end
            variance = variance / s.rows
            local std = math.floor(math.sqrt(variance))
            local mean = s.revenue // s.rows
            lines[#lines + 1] = padRight(region, 10) .. padLeft(s.rows, 7) .. padLeft(s.units, 8) ..
                                padLeft(money(s.revenue), 16) .. padLeft(money(mean), 12) .. padLeft(money(std), 12) ..
                                '  ' .. padRight(topEntry(s.products), 12) .. topEntry(s.reps)
            totalRows = totalRows + s.rows
            totalUnits = totalUnits + s.units
            totalRevenue = totalRevenue + s.revenue
        end
    end
    lines[#lines + 1] = padRight('Total', 10) .. padLeft(totalRows, 7) .. padLeft(totalUnits, 8) .. padLeft(money(totalRevenue), 16)
    return table.concat(lines, '\n')
end

local function hashText(text)
    local h = 5381
    for i = 1, #text do
        h = (h * 33 + string.byte(text, i)) % 4294967296
    end
    return h
end

local function main()
    local n = tonumber(arg[1]) or 100000
    local t0 = os.clock()
    local text = generateCsv(n)
    local rows = parseCsv(text)
    local output = report(rows)
    local t1 = os.clock()
    print('result: ' .. (#rows - 1) .. ',' .. hashText(output) .. ',' .. #output)
    print(string.format('time: %.3f', (t1 - t0) * 1000))
end

main()
