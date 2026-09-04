# salesreport - CSV parsing, per-group statistics, and a fixed-width text report
#
# Generates n rows of sales records as CSV text (with a quoted field), parses it with a
# character-level CSV reader, computes revenue, mean, standard deviation, and the top product and
# sales rep per region, and renders an aligned text report with formatted money amounts.

import math
import sys
import time

REGIONS = ['North', 'South', 'East', 'West', 'Central', 'Overseas']
PRODUCTS = ['Anvil', 'Bolt Kit', 'Cable Tie', 'Drill', 'Epoxy', 'Fan Belt', 'Gasket', 'Hinge', 'Impeller', 'Jack',
            'Kettle', 'Lamp', 'Motor', 'Nozzle', 'O-Ring', 'Pump', 'Quill', 'Rivet', 'Spring', 'Valve']
FIRSTS = ['Ada', 'Ben', 'Cleo', 'Dev', 'Eve', 'Finn', 'Gus', 'Hana']
LASTS = ['Ng', 'Ortiz', 'Patel', 'Quinn', 'Rossi']
REPS = [LASTS[k % 5] + ', ' + FIRSTS[k // 5] for k in range(40)]

seed = 42


def rand_int(n):
    global seed
    seed = (seed * 48271) % 2147483647
    return seed % n


def pad2(n):
    return ('0' + str(n)) if n < 10 else str(n)


def generate_csv(count):
    lines = ['id,date,region,product,units,price_cents,rep']
    for i in range(count):
        month = 1 + rand_int(12)
        day = 1 + rand_int(28)
        region = REGIONS[rand_int(6)]
        product = PRODUCTS[rand_int(20)]
        units = 1 + rand_int(50)
        price_cents = 500 + rand_int(49500)
        rep = REPS[rand_int(40)]
        lines.append(str(i + 1) + ',2026-' + pad2(month) + '-' + pad2(day) + ',' + region + ',' + product + ',' +
                     str(units) + ',' + str(price_cents) + ',"' + rep + '"')
    return '\n'.join(lines)


def parse_csv(text):
    rows = []
    fields = []
    field = ''
    in_quotes = False
    size = len(text)
    i = 0
    while i < size:
        ch = text[i]
        if in_quotes:
            if ch == '"':
                if i + 1 < size and text[i + 1] == '"':
                    field += '"'
                    i += 1
                else:
                    in_quotes = False
            else:
                field += ch
        elif ch == '"':
            in_quotes = True
        elif ch == ',':
            fields.append(field)
            field = ''
        elif ch == '\n':
            fields.append(field)
            rows.append(fields)
            fields = []
            field = ''
        else:
            field += ch
        i += 1
    if field != '' or len(fields) > 0:
        fields.append(field)
        rows.append(fields)
    return rows


def pad_left(text, width):
    text = str(text)
    return ' ' * (width - len(text)) + text if len(text) < width else text


def pad_right(text, width):
    text = str(text)
    return text + ' ' * (width - len(text)) if len(text) < width else text


def money(cents):
    dollars = str(cents // 100)
    out = ''
    count = 0
    for i in range(len(dollars) - 1, -1, -1):
        out = dollars[i] + out
        count += 1
        if count % 3 == 0 and i > 0:
            out = ',' + out
    return '$' + out + '.' + pad2(cents % 100)


def top_entry(counts):
    best = None
    best_value = -1
    for key, value in counts.items():
        if value > best_value or (value == best_value and key < best):
            best = key
            best_value = value
    return best


def report(rows):
    stats = {}
    for region in REGIONS:
        stats[region] = {'rows': 0, 'units': 0, 'revenue': 0, 'revenues': [], 'products': {}, 'reps': {}}
    data_rows = 0
    for ix in range(1, len(rows)):
        fields = rows[ix]
        region = fields[2]
        product = fields[3]
        units = int(fields[4])
        price_cents = int(fields[5])
        rep = fields[6]
        revenue = units * price_cents
        s = stats[region]
        s['rows'] += 1
        s['units'] += units
        s['revenue'] += revenue
        s['revenues'].append(revenue)
        s['products'][product] = s['products'].get(product, 0) + revenue
        s['reps'][rep] = s['reps'].get(rep, 0) + revenue
        data_rows += 1

    lines = ['Sales report: ' + str(data_rows) + ' rows', '',
             pad_right('Region', 10) + pad_left('Rows', 7) + pad_left('Units', 8) + pad_left('Revenue', 16) +
             pad_left('Mean', 12) + pad_left('Std dev', 12) + '  ' + pad_right('Top product', 12) + 'Top rep']
    total_rows = 0
    total_units = 0
    total_revenue = 0
    for region in REGIONS:
        s = stats[region]
        if s['rows'] == 0:
            continue
        mean_f = s['revenue'] / s['rows']
        variance = 0.0
        for revenue in s['revenues']:
            diff = revenue - mean_f
            variance += diff * diff
        variance = variance / s['rows']
        std = math.floor(math.sqrt(variance))
        mean = s['revenue'] // s['rows']
        lines.append(pad_right(region, 10) + pad_left(s['rows'], 7) + pad_left(s['units'], 8) +
                     pad_left(money(s['revenue']), 16) + pad_left(money(mean), 12) + pad_left(money(std), 12) + '  ' +
                     pad_right(top_entry(s['products']), 12) + top_entry(s['reps']))
        total_rows += s['rows']
        total_units += s['units']
        total_revenue += s['revenue']
    lines.append(pad_right('Total', 10) + pad_left(total_rows, 7) + pad_left(total_units, 8) +
                 pad_left(money(total_revenue), 16))
    return '\n'.join(lines)


def hash_text(text):
    h = 5381
    for ch in text:
        h = (h * 33 + ord(ch)) % 4294967296
    return h


def main():
    n = int(sys.argv[1]) if len(sys.argv) > 1 else 100000
    t0 = time.perf_counter()
    text = generate_csv(n)
    rows = parse_csv(text)
    output = report(rows)
    t1 = time.perf_counter()
    print('result: ' + str(len(rows) - 1) + ',' + str(hash_text(output)) + ',' + str(len(output)))
    print('time: ' + str(round((t1 - t0) * 1000, 3)))


main()
