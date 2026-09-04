// salesreport - CSV parsing, per-group statistics, and a fixed-width text report
//
// Generates n rows of sales records as CSV text (with a quoted field), parses it with a
// character-level CSV reader, computes revenue, mean, standard deviation, and the top product and
// sales rep per region, and renders an aligned text report with formatted money amounts.

'use strict';

const REGIONS = ['North', 'South', 'East', 'West', 'Central', 'Overseas'];
const PRODUCTS = ['Anvil', 'Bolt Kit', 'Cable Tie', 'Drill', 'Epoxy', 'Fan Belt', 'Gasket', 'Hinge', 'Impeller', 'Jack',
                  'Kettle', 'Lamp', 'Motor', 'Nozzle', 'O-Ring', 'Pump', 'Quill', 'Rivet', 'Spring', 'Valve'];
const FIRSTS = ['Ada', 'Ben', 'Cleo', 'Dev', 'Eve', 'Finn', 'Gus', 'Hana'];
const LASTS = ['Ng', 'Ortiz', 'Patel', 'Quinn', 'Rossi'];
const REPS = [];
for (let k = 0; k < 40; k++) {
    REPS.push(LASTS[k % 5] + ', ' + FIRSTS[Math.floor(k / 5)]);
}

let seed = 42;

function randInt(n) {
    seed = (seed * 48271) % 2147483647;
    return seed % n;
}

function pad2(n) {
    return n < 10 ? '0' + n : '' + n;
}

function generateCsv(count) {
    const lines = ['id,date,region,product,units,price_cents,rep'];
    for (let i = 0; i < count; i++) {
        const month = 1 + randInt(12);
        const day = 1 + randInt(28);
        const region = REGIONS[randInt(6)];
        const product = PRODUCTS[randInt(20)];
        const units = 1 + randInt(50);
        const priceCents = 500 + randInt(49500);
        const rep = REPS[randInt(40)];
        lines.push((i + 1) + ',2026-' + pad2(month) + '-' + pad2(day) + ',' + region + ',' + product + ',' +
                   units + ',' + priceCents + ',"' + rep + '"');
    }
    return lines.join('\n');
}

function parseCsv(text) {
    const rows = [];
    let fields = [];
    let field = '';
    let inQuotes = false;
    const size = text.length;
    let i = 0;
    while (i < size) {
        const ch = text[i];
        if (inQuotes) {
            if (ch === '"') {
                if (i + 1 < size && text[i + 1] === '"') {
                    field += '"';
                    i++;
                } else {
                    inQuotes = false;
                }
            } else {
                field += ch;
            }
        } else if (ch === '"') {
            inQuotes = true;
        } else if (ch === ',') {
            fields.push(field);
            field = '';
        } else if (ch === '\n') {
            fields.push(field);
            rows.push(fields);
            fields = [];
            field = '';
        } else {
            field += ch;
        }
        i++;
    }
    if (field !== '' || fields.length > 0) {
        fields.push(field);
        rows.push(fields);
    }
    return rows;
}

function padLeft(text, width) {
    text = '' + text;
    return text.length < width ? ' '.repeat(width - text.length) + text : text;
}

function padRight(text, width) {
    text = '' + text;
    return text.length < width ? text + ' '.repeat(width - text.length) : text;
}

function money(cents) {
    const dollars = '' + Math.floor(cents / 100);
    let out = '';
    let count = 0;
    for (let i = dollars.length - 1; i >= 0; i--) {
        out = dollars[i] + out;
        count++;
        if (count % 3 === 0 && i > 0) {
            out = ',' + out;
        }
    }
    return '$' + out + '.' + pad2(cents % 100);
}

function topEntry(counts) {
    let best = null;
    let bestValue = -1;
    for (const [key, value] of counts) {
        if (value > bestValue || (value === bestValue && key < best)) {
            best = key;
            bestValue = value;
        }
    }
    return best;
}

function report(rows) {
    const stats = new Map();
    for (const region of REGIONS) {
        stats.set(region, {rows: 0, units: 0, revenue: 0, revenues: [], products: new Map(), reps: new Map()});
    }
    let dataRows = 0;
    for (let ix = 1; ix < rows.length; ix++) {
        const fields = rows[ix];
        const region = fields[2];
        const product = fields[3];
        const units = parseInt(fields[4], 10);
        const priceCents = parseInt(fields[5], 10);
        const rep = fields[6];
        const revenue = units * priceCents;
        const s = stats.get(region);
        s.rows++;
        s.units += units;
        s.revenue += revenue;
        s.revenues.push(revenue);
        s.products.set(product, (s.products.get(product) || 0) + revenue);
        s.reps.set(rep, (s.reps.get(rep) || 0) + revenue);
        dataRows++;
    }

    const lines = ['Sales report: ' + dataRows + ' rows', '',
                   padRight('Region', 10) + padLeft('Rows', 7) + padLeft('Units', 8) + padLeft('Revenue', 16) +
                   padLeft('Mean', 12) + padLeft('Std dev', 12) + '  ' + padRight('Top product', 12) + 'Top rep'];
    let totalRows = 0;
    let totalUnits = 0;
    let totalRevenue = 0;
    for (const region of REGIONS) {
        const s = stats.get(region);
        if (s.rows === 0) {
            continue;
        }
        const meanF = s.revenue / s.rows;
        let variance = 0.0;
        for (const revenue of s.revenues) {
            const diff = revenue - meanF;
            variance += diff * diff;
        }
        variance = variance / s.rows;
        const std = Math.floor(Math.sqrt(variance));
        const mean = Math.floor(s.revenue / s.rows);
        lines.push(padRight(region, 10) + padLeft(s.rows, 7) + padLeft(s.units, 8) + padLeft(money(s.revenue), 16) +
                   padLeft(money(mean), 12) + padLeft(money(std), 12) + '  ' + padRight(topEntry(s.products), 12) +
                   topEntry(s.reps));
        totalRows += s.rows;
        totalUnits += s.units;
        totalRevenue += s.revenue;
    }
    lines.push(padRight('Total', 10) + padLeft(totalRows, 7) + padLeft(totalUnits, 8) + padLeft(money(totalRevenue), 16));
    return lines.join('\n');
}

function hashText(text) {
    let h = 5381;
    for (let i = 0; i < text.length; i++) {
        h = (h * 33 + text.charCodeAt(i)) % 4294967296;
    }
    return h;
}

function main() {
    const n = process.argv.length > 2 ? parseInt(process.argv[2], 10) : 100000;
    const t0 = performance.now();
    const text = generateCsv(n);
    const rows = parseCsv(text);
    const output = report(rows);
    const t1 = performance.now();
    console.log('result: ' + (rows.length - 1) + ',' + hashText(output) + ',' + output.length);
    console.log('time: ' + (t1 - t0).toFixed(3));
}

main();
