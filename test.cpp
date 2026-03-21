#include <array>
#include <chrono>
#include <cstdint>
#include <iomanip>
#include <iostream>
#include <iterator>
#include <map>
#include <string>
#include <tuple>
#include <vector>

namespace {

constexpr std::size_t kNumEntries = 65536;
constexpr std::size_t kOuterIterations = 20;
constexpr std::size_t kSecondaryProbeStride = 97;
constexpr std::size_t kWindowSize = 6;

struct Key {
  std::uint16_t tenant;
  std::uint16_t region;
  std::uint16_t shard;
  std::uint32_t sequence;
};

struct KeyLess {
  bool operator()(const Key &lhs, const Key &rhs) const {
    return std::tie(lhs.tenant, lhs.region, lhs.shard, lhs.sequence) <
           std::tie(rhs.tenant, rhs.region, rhs.shard, rhs.sequence);
  }
};

struct Value {
  std::array<std::uint64_t, 8> counters{};
  std::uint64_t epoch = 0;
  std::uint32_t flags = 0;
  std::uint32_t weight = 0;
};

using RecordMap = std::map<Key, Value, KeyLess>;

struct InputData {
  RecordMap records;
  std::vector<Key> queries;
};

struct WorkloadResult {
  std::uint64_t checksum = 0;
  std::uint64_t touched = 0;
};

volatile std::uint64_t g_sink = 0;

static std::uint64_t mix64(std::uint64_t x) {
  x += 0x9e3779b97f4a7c15ULL;
  x = (x ^ (x >> 30)) * 0xbf58476d1ce4e5b9ULL;
  x = (x ^ (x >> 27)) * 0x94d049bb133111ebULL;
  return x ^ (x >> 31);
}

static std::uint64_t rotl64(std::uint64_t v, unsigned s) {
  s &= 63U;
  if (s == 0)
    return v;
  return (v << s) | (v >> (64U - s));
}

static Key makeKey(std::size_t index) {
  const std::uint64_t seed = mix64(index + 0x123456789abcdef0ULL);
  return Key{
      static_cast<std::uint16_t>(seed & 0x03ffU),
      static_cast<std::uint16_t>((seed >> 10) & 0x00ffU),
      static_cast<std::uint16_t>((seed >> 18) & 0x003fU),
      static_cast<std::uint32_t>(index),
  };
}

static Value makeValue(std::size_t index) {
  const std::uint64_t seed = mix64(index ^ 0x0ddc0ffeebadf00dULL);
  Value v;
  for (std::size_t i = 0; i < v.counters.size(); ++i)
    v.counters[i] = mix64(seed + i * 0x9e3779b97f4a7c15ULL);
  v.epoch = mix64(seed + 17);
  v.flags = static_cast<std::uint32_t>((seed >> 32) & 0xffU);
  v.weight = static_cast<std::uint32_t>((seed >> 8) & 0x03ffU);
  return v;
}

static std::uint64_t consumeEntry(const Key &key, const Value &value,
                                  std::uint64_t salt) {
  std::uint64_t acc = value.counters[(key.shard + salt) & 7U];
  acc += value.counters[(key.region ^ value.flags ^ salt) & 7U] *
         static_cast<std::uint64_t>(value.weight + 1U);

  if ((value.flags & 0x1U) != 0)
    acc ^= rotl64(value.epoch + key.sequence, (key.tenant + salt) & 31U);

  if ((value.flags & 0x6U) == 0x4U)
    acc += (static_cast<std::uint64_t>(key.tenant) << 40) ^
           (static_cast<std::uint64_t>(key.region) << 24) ^
           static_cast<std::uint64_t>(key.sequence);

  return acc;
}

static InputData buildInput() {
  InputData input;

  for (std::size_t i = 0; i < kNumEntries; ++i)
    input.records.emplace(makeKey(i), makeValue(i));

  std::vector<Key> orderedKeys;
  orderedKeys.reserve(input.records.size());
  for (const auto &kv : input.records)
    orderedKeys.push_back(kv.first);

  if (orderedKeys.size() > kWindowSize) {
    for (std::size_t i = 0; i + kWindowSize < orderedKeys.size();
         i += kSecondaryProbeStride) {
      Key query = orderedKeys[i];
      query.sequence ^= static_cast<std::uint32_t>((i * 13U) & 0x3fU);
      input.queries.push_back(query);
    }
  }

  return input;
}

static WorkloadResult runTraversalWorkload(const RecordMap &records) {
  WorkloadResult result;

  for (std::size_t round = 0; round < kOuterIterations; ++round) {
    for (auto it = records.begin(), end = records.end(); it != end; ++it) {
      std::uint64_t local =
          consumeEntry(it->first, it->second, it->second.weight + round);
      ++result.touched;

      if ((it->second.flags & 0x3U) == 0x1U) {
        auto nextIt = std::next(it);
        if (nextIt != end) {
          local ^= consumeEntry(nextIt->first, nextIt->second,
                                it->first.sequence + round);
          ++result.touched;
        }
      }

      result.checksum += local;
    }
  }

  g_sink ^= result.checksum;
  return result;
}

static WorkloadResult runComplexProbeWorkload(const InputData &input) {
  WorkloadResult result;

  for (std::size_t round = 0; round < kOuterIterations; ++round) {
    for (const Key &query : input.queries) {
      auto it = input.records.lower_bound(query);
      if (it == input.records.end())
        continue;

      for (std::size_t i = 0; i < kWindowSize && it != input.records.end();
           ++i, ++it) {
        std::uint64_t local =
            consumeEntry(it->first, it->second, i + round + query.sequence);
        ++result.touched;

        if (((it->second.flags + i + round) & 0x7U) == 0x3U) {
          Key followup = it->first;
          followup.sequence += static_cast<std::uint32_t>(17 + i);
          auto probe = input.records.lower_bound(followup);
          if (probe != input.records.end()) {
            local ^= consumeEntry(probe->first, probe->second,
                                  probe->second.weight + i);
            ++result.touched;
          }
        }

        result.checksum += local;
      }
    }
  }

  g_sink ^= result.checksum;
  return result;
}

template <typename F>
static auto timeCall(F &&fn) {
  const auto start = std::chrono::steady_clock::now();
  auto result = fn();
  const auto end = std::chrono::steady_clock::now();
  return std::make_pair(result, end - start);
}

static double toMilliseconds(std::chrono::steady_clock::duration d) {
  return std::chrono::duration<double, std::milli>(d).count();
}

static double nsPerItem(std::chrono::steady_clock::duration d,
                        std::uint64_t touched) {
  if (touched == 0)
    return 0.0;
  const double ns = std::chrono::duration<double, std::nano>(d).count();
  return ns / static_cast<double>(touched);
}

static void printLine(const std::string &name, double ms, std::uint64_t touched,
                      std::uint64_t checksum) {
  std::cout << std::left << std::setw(18) << name << "  "
            << "time(ms)=" << std::setw(12) << std::fixed << std::setprecision(3)
            << ms << "  touched=" << std::setw(12) << touched
            << "  ns/item=" << std::setw(12) << std::setprecision(3)
            << (touched ? (ms * 1e6 / static_cast<double>(touched)) : 0.0)
            << "  checksum=" << checksum << '\n';
}

} // namespace

int main() {
  using clock = std::chrono::steady_clock;
  const auto totalStart = clock::now();

  const auto buildStart = clock::now();
  InputData input = buildInput();
  const auto buildEnd = clock::now();

  // Warm-up to reduce one-time effects.
  g_sink ^= runTraversalWorkload(input.records).checksum;
  g_sink ^= runComplexProbeWorkload(input).checksum;

  const auto [traversalResult, traversalTime] =
      timeCall([&] { return runTraversalWorkload(input.records); });
  const auto [complexResult, complexTime] =
      timeCall([&] { return runComplexProbeWorkload(input); });

  const auto totalEnd = clock::now();

  const auto buildTime = buildEnd - buildStart;
  const auto totalTime = totalEnd - totalStart;

  std::cout << "std::map<struct, struct> performance sample\n";
  std::cout << "entries=" << kNumEntries
            << ", outer_iterations=" << kOuterIterations
            << ", secondary_probe_stride=" << kSecondaryProbeStride
            << ", window_size=" << kWindowSize
            << ", queries=" << input.queries.size() << "\n\n";

  printLine("build", toMilliseconds(buildTime), input.records.size(), g_sink);
  printLine("traversal", toMilliseconds(traversalTime), traversalResult.touched,
            traversalResult.checksum);
  printLine("complex_probe", toMilliseconds(complexTime), complexResult.touched,
            complexResult.checksum);

  std::cout << '\n';
  std::cout << std::left << std::setw(18) << "total"
            << "  time(ms)=" << std::fixed << std::setprecision(3)
            << toMilliseconds(totalTime) << '\n';
  std::cout << "combined_checksum="
            << (traversalResult.checksum ^ complexResult.checksum ^ g_sink)
            << '\n';

  return 0;
}

