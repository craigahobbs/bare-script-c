# jsonetl - JSON extract, transform, load (JSON encode and decode, nested containers, grouping)
#
# Builds n customer orders with nested item lists, serializes them to JSON, parses the text back,
# then groups the parsed orders by customer and by SKU to rank revenue and demand.

require 'json'

NOTES = ['', 'Leave at door', 'Gift wrap', 'Call on arrival'].freeze

$seed = 42

def rand_int(n)
  $seed = ($seed * 48271) % 2147483647
  $seed % n
end

def pad2(n)
  n < 10 ? "0#{n}" : n.to_s
end

def pad3(n)
  return "00#{n}" if n < 10
  return "0#{n}" if n < 100

  n.to_s
end

def generate_orders(count)
  orders = []
  count.times do |i|
    customer = "C#{pad3(rand_int(500))}"
    day = 1 + rand_int(30)
    item_count = 1 + rand_int(5)
    items = []
    item_count.times do
      sku = "SKU#{pad3(rand_int(200))}"
      qty = 1 + rand_int(5)
      price_cents = 100 + rand_int(9900)
      items << {'sku' => sku, 'qty' => qty, 'priceCents' => price_cents}
    end
    shipping = rand_int(4) == 0 ? 'express' : 'standard'
    note = NOTES[rand_int(4)]
    orders << {'id' => i + 1, 'customer' => customer, 'date' => "2026-09-#{pad2(day)}", 'items' => items,
               'shipping' => shipping, 'note' => note}
  end
  orders
end

def rank(map, count)
  map.to_a.sort_by { |key, value| [-value, key] }[0, count]
end

def summarize(orders)
  revenue_by_customer = Hash.new(0)
  qty_by_sku = Hash.new(0)
  express = 0
  total_revenue = 0
  item_lines = 0
  orders.each do |order|
    revenue = 0
    order['items'].each do |item|
      revenue += item['qty'] * item['priceCents']
      qty_by_sku[item['sku']] += item['qty']
      item_lines += 1
    end
    revenue_by_customer[order['customer']] += revenue
    total_revenue += revenue
    express += 1 if order['shipping'] == 'express'
  end
  parts = [orders.length, item_lines, total_revenue, express, revenue_by_customer.length]
  rank(revenue_by_customer, 5).each { |customer, revenue| parts << "#{customer}:#{revenue}" }
  rank(qty_by_sku, 3).each { |sku, qty| parts << "#{sku}:#{qty}" }
  parts.join(',')
end

def main
  n = ARGV.length > 0 ? ARGV[0].to_i : 50000
  t0 = Process.clock_gettime(Process::CLOCK_MONOTONIC)
  orders = generate_orders(n)
  text = JSON.generate(orders)
  parsed = JSON.parse(text)
  result = summarize(parsed)
  t1 = Process.clock_gettime(Process::CLOCK_MONOTONIC)
  puts "result: #{result}"
  puts format('time: %.3f', (t1 - t0) * 1000)
end

main
