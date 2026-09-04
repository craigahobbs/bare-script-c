// jsonetl - JSON extract, transform, load (JSON encode and decode, nested containers, grouping)
//
// Builds n customer orders with nested item lists, serializes them to JSON, parses the text back,
// then groups the parsed orders by customer and by SKU to rank revenue and demand.

'use strict';

const NOTES = ['', 'Leave at door', 'Gift wrap', 'Call on arrival'];

let seed = 42;

function randInt(n) {
    seed = (seed * 48271) % 2147483647;
    return seed % n;
}

function pad2(n) {
    return n < 10 ? '0' + n : '' + n;
}

function pad3(n) {
    if (n < 10) {
        return '00' + n;
    }
    if (n < 100) {
        return '0' + n;
    }
    return '' + n;
}

function generateOrders(count) {
    const orders = [];
    for (let i = 0; i < count; i++) {
        const customer = 'C' + pad3(randInt(500));
        const day = 1 + randInt(30);
        const itemCount = 1 + randInt(5);
        const items = [];
        for (let j = 0; j < itemCount; j++) {
            const sku = 'SKU' + pad3(randInt(200));
            const qty = 1 + randInt(5);
            const priceCents = 100 + randInt(9900);
            items.push({sku, qty, priceCents});
        }
        const shipping = randInt(4) === 0 ? 'express' : 'standard';
        const note = NOTES[randInt(4)];
        orders.push({id: i + 1, customer, date: '2026-09-' + pad2(day), items, shipping, note});
    }
    return orders;
}

function rank(map, count) {
    return [...map.entries()].sort((a, b) => (b[1] - a[1]) || (a[0] < b[0] ? -1 : (a[0] > b[0] ? 1 : 0))).slice(0, count);
}

function summarize(orders) {
    const revenueByCustomer = new Map();
    const qtyBySku = new Map();
    let express = 0;
    let totalRevenue = 0;
    let itemLines = 0;
    for (const order of orders) {
        let revenue = 0;
        for (const item of order.items) {
            revenue += item.qty * item.priceCents;
            qtyBySku.set(item.sku, (qtyBySku.get(item.sku) || 0) + item.qty);
            itemLines++;
        }
        revenueByCustomer.set(order.customer, (revenueByCustomer.get(order.customer) || 0) + revenue);
        totalRevenue += revenue;
        if (order.shipping === 'express') {
            express++;
        }
    }
    const parts = [orders.length, itemLines, totalRevenue, express, revenueByCustomer.size];
    for (const [customer, revenue] of rank(revenueByCustomer, 5)) {
        parts.push(customer + ':' + revenue);
    }
    for (const [sku, qty] of rank(qtyBySku, 3)) {
        parts.push(sku + ':' + qty);
    }
    return parts.join(',');
}

function main() {
    const n = process.argv.length > 2 ? parseInt(process.argv[2], 10) : 50000;
    const t0 = performance.now();
    const orders = generateOrders(n);
    const text = JSON.stringify(orders);
    const parsed = JSON.parse(text);
    const result = summarize(parsed);
    const t1 = performance.now();
    console.log('result: ' + result);
    console.log('time: ' + (t1 - t0).toFixed(3));
}

main();
