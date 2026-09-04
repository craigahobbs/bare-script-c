# salesreport - CSV parsing, per-group statistics, and a fixed-width text report
#
# Generates n rows of sales records as CSV text (with a quoted field), parses it with a
# character-level CSV reader, computes revenue, mean, standard deviation, and the top product and
# sales rep per region, and renders an aligned text report with formatted money amounts.

REGIONS = ['North', 'South', 'East', 'West', 'Central', 'Overseas'].freeze
PRODUCTS = ['Anvil', 'Bolt Kit', 'Cable Tie', 'Drill', 'Epoxy', 'Fan Belt', 'Gasket', 'Hinge', 'Impeller', 'Jack',
            'Kettle', 'Lamp', 'Motor', 'Nozzle', 'O-Ring', 'Pump', 'Quill', 'Rivet', 'Spring', 'Valve'].freeze
FIRSTS = ['Ada', 'Ben', 'Cleo', 'Dev', 'Eve', 'Finn', 'Gus', 'Hana'].freeze
LASTS = ['Ng', 'Ortiz', 'Patel', 'Quinn', 'Rossi'].freeze
REPS = (0...40).map { |k| "#{LASTS[k % 5]}, #{FIRSTS[k / 5]}" }.freeze

$seed = 42

def rand_int(n)
  $seed = ($seed * 48271) % 2147483647
  $seed % n
end

def pad2(n)
  n < 10 ? "0#{n}" : n.to_s
end

def generate_csv(count)
  lines = ['id,date,region,product,units,price_cents,rep']
  count.times do |i|
    month = 1 + rand_int(12)
    day = 1 + rand_int(28)
    region = REGIONS[rand_int(6)]
    product = PRODUCTS[rand_int(20)]
    units = 1 + rand_int(50)
    price_cents = 500 + rand_int(49500)
    rep = REPS[rand_int(40)]
    lines << "#{i + 1},2026-#{pad2(month)}-#{pad2(day)},#{region},#{product},#{units},#{price_cents},\"#{rep}\""
  end
  lines.join("\n")
end

def parse_csv(text)
  rows = []
  fields = []
  field = +''
  in_quotes = false
  size = text.length
  i = 0
  while i < size
    ch = text[i]
    if in_quotes
      if ch == '"'
        if i + 1 < size && text[i + 1] == '"'
          field << '"'
          i += 1
        else
          in_quotes = false
        end
      else
        field << ch
      end
    elsif ch == '"'
      in_quotes = true
    elsif ch == ','
      fields << field
      field = +''
    elsif ch == "\n"
      fields << field
      rows << fields
      fields = []
      field = +''
    else
      field << ch
    end
    i += 1
  end
  if field != '' || fields.length > 0
    fields << field
    rows << fields
  end
  rows
end

def pad_left(text, width)
  text = text.to_s
  text.length < width ? ' ' * (width - text.length) + text : text
end

def pad_right(text, width)
  text = text.to_s
  text.length < width ? text + ' ' * (width - text.length) : text
end

def money(cents)
  dollars = (cents / 100).to_s
  out = +''
  count = 0
  (dollars.length - 1).downto(0) do |i|
    out = dollars[i] + out
    count += 1
    out = ',' + out if count % 3 == 0 && i > 0
  end
  "$#{out}.#{pad2(cents % 100)}"
end

def top_entry(counts)
  best = nil
  best_value = -1
  counts.each do |key, value|
    if value > best_value || (value == best_value && key < best)
      best = key
      best_value = value
    end
  end
  best
end

def report(rows)
  stats = {}
  REGIONS.each do |region|
    stats[region] = {rows: 0, units: 0, revenue: 0, revenues: [], products: Hash.new(0), reps: Hash.new(0)}
  end
  data_rows = 0
  (1...rows.length).each do |ix|
    fields = rows[ix]
    region = fields[2]
    product = fields[3]
    units = fields[4].to_i
    price_cents = fields[5].to_i
    rep = fields[6]
    revenue = units * price_cents
    s = stats[region]
    s[:rows] += 1
    s[:units] += units
    s[:revenue] += revenue
    s[:revenues] << revenue
    s[:products][product] += revenue
    s[:reps][rep] += revenue
    data_rows += 1
  end

  lines = ["Sales report: #{data_rows} rows", '',
           pad_right('Region', 10) + pad_left('Rows', 7) + pad_left('Units', 8) + pad_left('Revenue', 16) +
           pad_left('Mean', 12) + pad_left('Std dev', 12) + '  ' + pad_right('Top product', 12) + 'Top rep']
  total_rows = 0
  total_units = 0
  total_revenue = 0
  REGIONS.each do |region|
    s = stats[region]
    next if s[:rows] == 0

    mean_f = s[:revenue].to_f / s[:rows]
    variance = 0.0
    s[:revenues].each do |revenue|
      diff = revenue - mean_f
      variance += diff * diff
    end
    variance /= s[:rows]
    std = Math.sqrt(variance).floor
    mean = s[:revenue] / s[:rows]
    lines << pad_right(region, 10) + pad_left(s[:rows], 7) + pad_left(s[:units], 8) + pad_left(money(s[:revenue]), 16) +
             pad_left(money(mean), 12) + pad_left(money(std), 12) + '  ' + pad_right(top_entry(s[:products]), 12) +
             top_entry(s[:reps])
    total_rows += s[:rows]
    total_units += s[:units]
    total_revenue += s[:revenue]
  end
  lines << pad_right('Total', 10) + pad_left(total_rows, 7) + pad_left(total_units, 8) + pad_left(money(total_revenue), 16)
  lines.join("\n")
end

def hash_text(text)
  h = 5381
  text.each_byte { |b| h = (h * 33 + b) % 4294967296 }
  h
end

def main
  n = ARGV.length > 0 ? ARGV[0].to_i : 100000
  t0 = Process.clock_gettime(Process::CLOCK_MONOTONIC)
  text = generate_csv(n)
  rows = parse_csv(text)
  output = report(rows)
  t1 = Process.clock_gettime(Process::CLOCK_MONOTONIC)
  puts "result: #{rows.length - 1},#{hash_text(output)},#{output.length}"
  puts format('time: %.3f', (t1 - t0) * 1000)
end

main
