-- jsonetl - JSON extract, transform, load (JSON encode and decode, nested containers, grouping)
--
-- Builds n customer orders with nested item lists, serializes them to JSON, parses the text back,
-- then groups the parsed orders by customer and by SKU to rank revenue and demand.
--
-- Lua has no JSON in its standard library, so this port carries the small pure-Lua codec that a
-- Lua program would vendor.

local NOTES = {'', 'Leave at door', 'Gift wrap', 'Call on arrival'}

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

local function pad3(n)
    if n < 10 then
        return '00' .. n
    elseif n < 100 then
        return '0' .. n
    end
    return '' .. n
end

-- A minimal JSON codec: objects are tables with string keys, arrays are sequences
local ESCAPES = {['"'] = '\\"', ['\\'] = '\\\\', ['\n'] = '\\n', ['\r'] = '\\r', ['\t'] = '\\t'}

local function encodeValue(value, out)
    local kind = type(value)
    if kind == 'table' then
        if value[1] ~= nil or next(value) == nil then
            out[#out + 1] = '['
            for ix, item in ipairs(value) do
                if ix > 1 then
                    out[#out + 1] = ','
                end
                encodeValue(item, out)
            end
            out[#out + 1] = ']'
        else
            out[#out + 1] = '{'
            local first = true
            for key, item in pairs(value) do
                if not first then
                    out[#out + 1] = ','
                end
                first = false
                out[#out + 1] = '"' .. key .. '":'
                encodeValue(item, out)
            end
            out[#out + 1] = '}'
        end
    elseif kind == 'string' then
        out[#out + 1] = '"' .. string.gsub(value, '["\\\n\r\t]', ESCAPES) .. '"'
    elseif kind == 'number' then
        out[#out + 1] = tostring(value)
    elseif kind == 'boolean' then
        out[#out + 1] = tostring(value)
    else
        out[#out + 1] = 'null'
    end
end

local function jsonEncode(value)
    local out = {}
    encodeValue(value, out)
    return table.concat(out)
end

local UNESCAPES = {['"'] = '"', ['\\'] = '\\', ['/'] = '/', b = '\b', f = '\f', n = '\n', r = '\r', t = '\t'}

local decodeValue

local function decodeString(text, pos)
    local out = {}
    local ix = pos + 1
    while true do
        local ch = string.sub(text, ix, ix)
        if ch == '"' then
            return table.concat(out), ix + 1
        elseif ch == '\\' then
            local esc = string.sub(text, ix + 1, ix + 1)
            if esc == 'u' then
                out[#out + 1] = utf8.char(tonumber(string.sub(text, ix + 2, ix + 5), 16))
                ix = ix + 6
            else
                out[#out + 1] = UNESCAPES[esc]
                ix = ix + 2
            end
        elseif ch == '' then
            error('unterminated string')
        else
            local stop = string.find(text, '["\\]', ix) or (#text + 1)
            out[#out + 1] = string.sub(text, ix, stop - 1)
            ix = stop
        end
    end
end

local function skipSpace(text, pos)
    return string.find(text, '%S', pos) or (#text + 1)
end

decodeValue = function(text, pos)
    pos = skipSpace(text, pos)
    local ch = string.sub(text, pos, pos)
    if ch == '{' then
        local object = {}
        pos = skipSpace(text, pos + 1)
        if string.sub(text, pos, pos) == '}' then
            return object, pos + 1
        end
        while true do
            local key
            key, pos = decodeString(text, skipSpace(text, pos))
            pos = skipSpace(text, pos)
            if string.sub(text, pos, pos) ~= ':' then
                error('expected colon')
            end
            object[key], pos = decodeValue(text, pos + 1)
            pos = skipSpace(text, pos)
            local next_ = string.sub(text, pos, pos)
            if next_ == '}' then
                return object, pos + 1
            elseif next_ ~= ',' then
                error('expected comma')
            end
            pos = pos + 1
        end
    elseif ch == '[' then
        local array = {}
        pos = skipSpace(text, pos + 1)
        if string.sub(text, pos, pos) == ']' then
            return array, pos + 1
        end
        while true do
            array[#array + 1], pos = decodeValue(text, pos)
            pos = skipSpace(text, pos)
            local next_ = string.sub(text, pos, pos)
            if next_ == ']' then
                return array, pos + 1
            elseif next_ ~= ',' then
                error('expected comma')
            end
            pos = pos + 1
        end
    elseif ch == '"' then
        return decodeString(text, pos)
    elseif string.sub(text, pos, pos + 3) == 'true' then
        return true, pos + 4
    elseif string.sub(text, pos, pos + 4) == 'false' then
        return false, pos + 5
    elseif string.sub(text, pos, pos + 3) == 'null' then
        return nil, pos + 4
    end
    local numberText = string.match(text, '^-?%d+%.?%d*[eE]?[-+]?%d*', pos)
    if numberText == nil or numberText == '' then
        error('unexpected character at ' .. pos)
    end
    return tonumber(numberText), pos + #numberText
end

local function jsonDecode(text)
    local value = decodeValue(text, 1)
    return value
end

local function generateOrders(count)
    local orders = {}
    for i = 1, count do
        local customer = 'C' .. pad3(randInt(500))
        local day = 1 + randInt(30)
        local itemCount = 1 + randInt(5)
        local items = {}
        for j = 1, itemCount do
            local sku = 'SKU' .. pad3(randInt(200))
            local qty = 1 + randInt(5)
            local priceCents = 100 + randInt(9900)
            items[j] = {sku = sku, qty = qty, priceCents = priceCents}
        end
        local shipping = 'standard'
        if randInt(4) == 0 then
            shipping = 'express'
        end
        local note = NOTES[randInt(4) + 1]
        orders[i] = {id = i, customer = customer, date = '2026-09-' .. pad2(day), items = items, shipping = shipping, note = note}
    end
    return orders
end

local function rank(map, count)
    local entries = {}
    for key, value in pairs(map) do
        entries[#entries + 1] = {key, value}
    end
    table.sort(entries, function(a, b)
        if a[2] ~= b[2] then
            return a[2] > b[2]
        end
        return a[1] < b[1]
    end)
    local top = {}
    for ix = 1, math.min(count, #entries) do
        top[ix] = entries[ix]
    end
    return top
end

local function summarize(orders)
    local revenueByCustomer = {}
    local customerCount = 0
    local qtyBySku = {}
    local express = 0
    local totalRevenue = 0
    local itemLines = 0
    for _, order in ipairs(orders) do
        local revenue = 0
        for _, item in ipairs(order.items) do
            revenue = revenue + item.qty * item.priceCents
            qtyBySku[item.sku] = (qtyBySku[item.sku] or 0) + item.qty
            itemLines = itemLines + 1
        end
        local customer = order.customer
        if revenueByCustomer[customer] == nil then
            customerCount = customerCount + 1
            revenueByCustomer[customer] = 0
        end
        revenueByCustomer[customer] = revenueByCustomer[customer] + revenue
        totalRevenue = totalRevenue + revenue
        if order.shipping == 'express' then
            express = express + 1
        end
    end
    local parts = {#orders, itemLines, totalRevenue, express, customerCount}
    for _, entry in ipairs(rank(revenueByCustomer, 5)) do
        parts[#parts + 1] = entry[1] .. ':' .. entry[2]
    end
    for _, entry in ipairs(rank(qtyBySku, 3)) do
        parts[#parts + 1] = entry[1] .. ':' .. entry[2]
    end
    return table.concat(parts, ',')
end

local function main()
    local n = tonumber(arg[1]) or 50000
    local t0 = os.clock()
    local orders = generateOrders(n)
    local text = jsonEncode(orders)
    local parsed = jsonDecode(text)
    local result = summarize(parsed)
    local t1 = os.clock()
    print('result: ' .. result)
    print(string.format('time: %.3f', (t1 - t0) * 1000))
end

main()
