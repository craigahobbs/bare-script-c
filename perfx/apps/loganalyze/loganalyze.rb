# loganalyze - web server log analysis (regular expressions, string splitting, hash maps)
#
# Generates n lines of an Apache-style access log with a deterministic PRNG, then parses every
# line with a regular expression and aggregates: unique client addresses, status code counts,
# requests per hour, bytes per path prefix, and the busiest hour and heaviest paths.

WORDS = ['perf', 'bare', 'script', 'lua', 'ruby', 'perl', 'python', 'node'].freeze
SLUGS = ['hello-world', 'release-notes', 'faq', 'roadmap', 'benchmarks'].freeze
LINE_RE = %r{^(\S+) \S+ \S+ \[(\d+)/(\w+)/(\d+):(\d+):(\d+):(\d+) [^\]]+\] "(\w+) (\S+) [^"]+" (\d+) (\d+)$}

$seed = 42

def rand_int(n)
  $seed = ($seed * 48271) % 2147483647
  $seed % n
end

def pad2(n)
  n < 10 ? "0#{n}" : n.to_s
end

def generate_log(count)
  lines = []
  count.times do
    ip = "10.#{rand_int(8)}.#{rand_int(256)}.#{rand_int(256)}"
    day = 1 + rand_int(30)
    hour = rand_int(24)
    minute = rand_int(60)
    second = rand_int(60)
    method = rand_int(10) == 0 ? 'POST' : 'GET'
    kind = rand_int(12)
    path =
      if kind == 0
        '/'
      elsif kind == 1
        '/index.html'
      elsif kind == 2
        '/api/users'
      elsif kind == 3
        "/api/users/#{rand_int(1000)}"
      elsif kind == 4
        '/api/orders'
      elsif kind == 5
        '/static/app.js'
      elsif kind == 6
        '/static/style.css'
      elsif kind == 7
        '/images/logo.png'
      elsif kind == 8
        '/login'
      elsif kind == 9
        '/logout'
      elsif kind == 10
        "/search?q=#{WORDS[rand_int(8)]}"
      else
        "/blog/#{SLUGS[rand_int(5)]}"
      end
    pick = rand_int(100)
    status =
      if pick < 80
        200
      elsif pick < 88
        304
      elsif pick < 95
        404
      elsif pick < 98
        302
      else
        500
      end
    size = rand_int(50000)
    size = 0 if status == 304
    lines << "#{ip} - - [#{pad2(day)}/Sep/2026:#{pad2(hour)}:#{pad2(minute)}:#{pad2(second)} +0000] \"#{method} #{path} HTTP/1.1\" #{status} #{size}"
  end
  lines.join("\n")
end

def path_key(path)
  q = path.index('?')
  path = path[0, q] if q
  parts = path.split('/', -1)
  path = "/#{parts[1]}/#{parts[2]}" if parts.length > 3
  path
end

def analyze(text)
  total = 0
  bad = 0
  posts = 0
  errors = 0
  total_bytes = 0
  ips = {}
  statuses = Hash.new(0)
  hours = Array.new(24, 0)
  path_bytes = Hash.new(0)
  text.split("\n").each do |line|
    m = LINE_RE.match(line)
    if m.nil?
      bad += 1
      next
    end
    ip = m[1]
    hour = m[5].to_i
    method = m[8]
    path = m[9]
    status = m[10].to_i
    size = m[11].to_i
    total += 1
    ips[ip] = true
    statuses[status] += 1
    hours[hour] += 1
    posts += 1 if method == 'POST'
    errors += 1 if status >= 500
    total_bytes += size
    key = path_key(path)
    path_bytes[key] += size
  end

  peak_hour = 0
  (0...24).each do |hour|
    peak_hour = hour if hours[hour] > hours[peak_hour]
  end
  top = path_bytes.to_a.sort_by { |key, size| [-size, key] }[0, 3]
  parts = [total, bad, ips.length, posts, errors, total_bytes, statuses[200], statuses[304], statuses[404],
           "#{peak_hour}:#{hours[peak_hour]}"]
  top.each { |key, size| parts << "#{key}:#{size}" }
  parts.join(',')
end

def main
  n = ARGV.length > 0 ? ARGV[0].to_i : 300000
  t0 = Process.clock_gettime(Process::CLOCK_MONOTONIC)
  text = generate_log(n)
  result = analyze(text)
  t1 = Process.clock_gettime(Process::CLOCK_MONOTONIC)
  puts "result: #{result}"
  puts format('time: %.3f', (t1 - t0) * 1000)
end

main
