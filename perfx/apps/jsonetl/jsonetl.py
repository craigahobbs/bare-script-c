# jsonetl - JSON extract, transform, load (JSON encode and decode, nested containers, grouping)
#
# Builds n customer orders with nested item lists, serializes them to JSON, parses the text back,
# then groups the parsed orders by customer and by SKU to rank revenue and demand.

import json
import sys
import time

NOTES = ['', 'Leave at door', 'Gift wrap', 'Call on arrival']

seed = 42


def rand_int(n):
    global seed
    seed = (seed * 48271) % 2147483647
    return seed % n


def pad2(n):
    return ('0' + str(n)) if n < 10 else str(n)


def pad3(n):
    if n < 10:
        return '00' + str(n)
    if n < 100:
        return '0' + str(n)
    return str(n)


def generate_orders(count):
    orders = []
    for i in range(count):
        customer = 'C' + pad3(rand_int(500))
        day = 1 + rand_int(30)
        item_count = 1 + rand_int(5)
        items = []
        for _ in range(item_count):
            sku = 'SKU' + pad3(rand_int(200))
            qty = 1 + rand_int(5)
            price_cents = 100 + rand_int(9900)
            items.append({'sku': sku, 'qty': qty, 'priceCents': price_cents})
        shipping = 'express' if rand_int(4) == 0 else 'standard'
        note = NOTES[rand_int(4)]
        orders.append({'id': i + 1, 'customer': customer, 'date': '2026-09-' + pad2(day), 'items': items,
                       'shipping': shipping, 'note': note})
    return orders


def summarize(orders):
    revenue_by_customer = {}
    qty_by_sku = {}
    express = 0
    total_revenue = 0
    item_lines = 0
    for order in orders:
        revenue = 0
        for item in order['items']:
            revenue += item['qty'] * item['priceCents']
            qty_by_sku[item['sku']] = qty_by_sku.get(item['sku'], 0) + item['qty']
            item_lines += 1
        customer = order['customer']
        revenue_by_customer[customer] = revenue_by_customer.get(customer, 0) + revenue
        total_revenue += revenue
        if order['shipping'] == 'express':
            express += 1
    top_customers = sorted(revenue_by_customer.items(), key=lambda kv: (-kv[1], kv[0]))[:5]
    top_skus = sorted(qty_by_sku.items(), key=lambda kv: (-kv[1], kv[0]))[:3]
    parts = [len(orders), item_lines, total_revenue, express, len(revenue_by_customer)]
    parts.extend(customer + ':' + str(revenue) for customer, revenue in top_customers)
    parts.extend(sku + ':' + str(qty) for sku, qty in top_skus)
    return ','.join(str(part) for part in parts)


def main():
    n = int(sys.argv[1]) if len(sys.argv) > 1 else 50000
    t0 = time.perf_counter()
    orders = generate_orders(n)
    text = json.dumps(orders, separators=(',', ':'))
    parsed = json.loads(text)
    result = summarize(parsed)
    t1 = time.perf_counter()
    print('result: ' + result)
    print('time: ' + str(round((t1 - t0) * 1000, 3)))


main()
