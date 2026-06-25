#include <array>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <limits>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>

namespace {

constexpr uint32_t kSlotCount = 32;
constexpr uint32_t kSampleRate = 44100;

struct Row {
  uint64_t sample = 0;
  uint32_t slot = 0;
  uint32_t active = 0;
  uint32_t muted = 0;
  uint32_t pcm8 = 0;
  uint32_t ssctl = 0;
  uint32_t lpctl = 0;
  uint32_t backwards = 0;
  uint64_t cur_addr_before = 0;
  uint64_t nxt_addr_before = 0;
  uint64_t cur_addr_after = 0;
  uint64_t nxt_addr_after = 0;
  uint64_t addr1 = 0;
  uint64_t addr2 = 0;
  int64_t fraction = 0;
  int64_t modulation = 0;
  uint32_t mdl = 0;
  uint32_t mdxsl = 0;
  uint32_t mdysl = 0;
  int32_t eg_state = 0;
  int64_t eg_volume = 0;
  uint64_t lfo_phase = 0;
  int64_t pre_level_output = 0;
  uint64_t env_level = 0;
  uint64_t tl_level = 0;
  uint64_t alfo_level = 0;
  uint64_t final_level = 0;
  uint64_t level_mul = 0;
  uint64_t level_shift = 0;
  int64_t output = 0;
  bool has_level_trace = false;
};

struct SlotSummary {
  uint64_t rows = 0;
  uint64_t first_sample = std::numeric_limits<uint64_t>::max();
  uint64_t last_sample = 0;
  uint64_t active_rows = 0;
  uint64_t muted_rows = 0;
  uint64_t pcm8_rows = 0;
  uint64_t backwards_rows = 0;
  uint64_t output_nonzero = 0;
  uint64_t address_jumps = 0;
  uint64_t address_rewinds = 0;
  uint64_t lfo_wraps = 0;
  uint32_t ssctl_mask = 0;
  uint32_t lpctl_mask = 0;
  uint32_t eg_state_mask = 0;
  uint64_t min_addr = std::numeric_limits<uint64_t>::max();
  uint64_t max_addr = 0;
  int64_t min_modulation = std::numeric_limits<int64_t>::max();
  int64_t max_modulation = std::numeric_limits<int64_t>::min();
  int64_t min_output = std::numeric_limits<int64_t>::max();
  int64_t max_output = std::numeric_limits<int64_t>::min();
  int64_t min_pre_level_output = std::numeric_limits<int64_t>::max();
  int64_t max_pre_level_output = std::numeric_limits<int64_t>::min();
  uint64_t min_final_level = std::numeric_limits<uint64_t>::max();
  uint64_t max_final_level = 0;
  uint64_t min_alfo_level = std::numeric_limits<uint64_t>::max();
  uint64_t max_alfo_level = 0;
  uint64_t min_level_mul = std::numeric_limits<uint64_t>::max();
  uint64_t max_level_mul = 0;
  uint64_t min_level_shift = std::numeric_limits<uint64_t>::max();
  uint64_t max_level_shift = 0;
  long double output_sum_sq = 0.0L;
  long double pre_level_sum_sq = 0.0L;
  uint64_t level_trace_rows = 0;
  bool has_previous = false;
  uint64_t previous_cur_addr_after = 0;
  uint64_t previous_lfo_phase = 0;
};

static std::vector<std::string> split_csv_line(const std::string& line) {
  std::vector<std::string> fields;
  std::string field;
  std::stringstream ss(line);
  while (std::getline(ss, field, ',')) fields.push_back(field);
  return fields;
}

static uint64_t parse_u64(const std::vector<std::string>& fields, size_t index) {
  if (index >= fields.size()) throw std::runtime_error("short CSV row");
  return static_cast<uint64_t>(std::strtoull(fields[index].c_str(), nullptr, 0));
}

static int64_t parse_i64(const std::vector<std::string>& fields, size_t index) {
  if (index >= fields.size()) throw std::runtime_error("short CSV row");
  return static_cast<int64_t>(std::strtoll(fields[index].c_str(), nullptr, 0));
}

static Row parse_row(const std::string& line) {
  const auto f = split_csv_line(line);
  if (f.size() != 23 && f.size() != 30) throw std::runtime_error("expected 23 or 30 CSV columns");
  Row r;
  r.sample = parse_u64(f, 0);
  r.slot = static_cast<uint32_t>(parse_u64(f, 1));
  r.active = static_cast<uint32_t>(parse_u64(f, 2));
  r.muted = static_cast<uint32_t>(parse_u64(f, 3));
  r.pcm8 = static_cast<uint32_t>(parse_u64(f, 4));
  r.ssctl = static_cast<uint32_t>(parse_u64(f, 5));
  r.lpctl = static_cast<uint32_t>(parse_u64(f, 6));
  r.backwards = static_cast<uint32_t>(parse_u64(f, 7));
  r.cur_addr_before = parse_u64(f, 8);
  r.nxt_addr_before = parse_u64(f, 9);
  r.cur_addr_after = parse_u64(f, 10);
  r.nxt_addr_after = parse_u64(f, 11);
  r.addr1 = parse_u64(f, 12);
  r.addr2 = parse_u64(f, 13);
  r.fraction = parse_i64(f, 14);
  r.modulation = parse_i64(f, 15);
  r.mdl = static_cast<uint32_t>(parse_u64(f, 16));
  r.mdxsl = static_cast<uint32_t>(parse_u64(f, 17));
  r.mdysl = static_cast<uint32_t>(parse_u64(f, 18));
  r.eg_state = static_cast<int32_t>(parse_i64(f, 19));
  r.eg_volume = parse_i64(f, 20);
  r.lfo_phase = parse_u64(f, 21);
  if (f.size() == 30) {
    r.pre_level_output = parse_i64(f, 22);
    r.env_level = parse_u64(f, 23);
    r.tl_level = parse_u64(f, 24);
    r.alfo_level = parse_u64(f, 25);
    r.final_level = parse_u64(f, 26);
    r.level_mul = parse_u64(f, 27);
    r.level_shift = parse_u64(f, 28);
    r.output = parse_i64(f, 29);
    r.has_level_trace = true;
  } else {
    r.pre_level_output = 0;
    r.output = parse_i64(f, 22);
  }
  return r;
}

static void update_summary(SlotSummary& s, const Row& r) {
  ++s.rows;
  if (r.active) ++s.active_rows;
  if (r.muted) ++s.muted_rows;
  if (r.pcm8) ++s.pcm8_rows;
  if (r.backwards) ++s.backwards_rows;
  if (r.output != 0) ++s.output_nonzero;

  s.first_sample = std::min(s.first_sample, r.sample);
  s.last_sample = std::max(s.last_sample, r.sample);
  s.ssctl_mask |= 1u << (r.ssctl & 3u);
  s.lpctl_mask |= 1u << (r.lpctl & 3u);
  if (r.eg_state >= 0 && r.eg_state < 32) s.eg_state_mask |= 1u << r.eg_state;
  s.min_addr = std::min(s.min_addr, r.cur_addr_before);
  s.max_addr = std::max(s.max_addr, r.cur_addr_before);
  s.min_addr = std::min(s.min_addr, r.cur_addr_after);
  s.max_addr = std::max(s.max_addr, r.cur_addr_after);
  s.min_modulation = std::min(s.min_modulation, r.modulation);
  s.max_modulation = std::max(s.max_modulation, r.modulation);
  s.min_output = std::min(s.min_output, r.output);
  s.max_output = std::max(s.max_output, r.output);
  s.output_sum_sq += static_cast<long double>(r.output) * r.output;
  if (r.has_level_trace) {
    ++s.level_trace_rows;
    s.min_pre_level_output = std::min(s.min_pre_level_output, r.pre_level_output);
    s.max_pre_level_output = std::max(s.max_pre_level_output, r.pre_level_output);
    s.pre_level_sum_sq += static_cast<long double>(r.pre_level_output) * r.pre_level_output;
    s.min_final_level = std::min(s.min_final_level, r.final_level);
    s.max_final_level = std::max(s.max_final_level, r.final_level);
    s.min_alfo_level = std::min(s.min_alfo_level, r.alfo_level);
    s.max_alfo_level = std::max(s.max_alfo_level, r.alfo_level);
    s.min_level_mul = std::min(s.min_level_mul, r.level_mul);
    s.max_level_mul = std::max(s.max_level_mul, r.level_mul);
    s.min_level_shift = std::min(s.min_level_shift, r.level_shift);
    s.max_level_shift = std::max(s.max_level_shift, r.level_shift);
  }

  if (s.has_previous) {
    if (r.cur_addr_before != s.previous_cur_addr_after) ++s.address_jumps;
    if (r.cur_addr_after < r.cur_addr_before) ++s.address_rewinds;
    if (r.lfo_phase < s.previous_lfo_phase) ++s.lfo_wraps;
  }
  s.previous_cur_addr_after = r.cur_addr_after;
  s.previous_lfo_phase = r.lfo_phase;
  s.has_previous = true;
}

static double pct(uint64_t value, uint64_t total) {
  return total ? 100.0 * static_cast<double>(value) / static_cast<double>(total) : 0.0;
}

static double seconds(uint64_t sample) {
  return static_cast<double>(sample) / static_cast<double>(kSampleRate);
}

static double rms(const SlotSummary& s) {
  return s.rows ? std::sqrt(static_cast<double>(s.output_sum_sq / s.rows)) : 0.0;
}

static double pre_level_rms(const SlotSummary& s) {
  return s.level_trace_rows ? std::sqrt(static_cast<double>(s.pre_level_sum_sq / s.level_trace_rows)) : 0.0;
}

static double db_ratio(double value, double reference) {
  return value > 0.0 && reference > 0.0 ? 20.0 * std::log10(value / reference) : -999.0;
}

static std::string mask_list(uint32_t mask) {
  std::string out;
  for (uint32_t bit = 0; bit < 32; ++bit) {
    if ((mask & (1u << bit)) == 0) continue;
    if (!out.empty()) out += "|";
    out += std::to_string(bit);
  }
  return out.empty() ? "-" : out;
}

static void usage(const char* argv0) {
  std::fprintf(stderr, "Usage: %s libvgm-slot-trace.csv [summary.csv]\n", argv0);
  std::fprintf(stderr, "Create traces with LIBVGM_SCSP_SLOT_TRACE_CSV=trace.csv vgm2wav ...\n");
}

}  // namespace

int main(int argc, char** argv) {
  if (argc != 2 && argc != 3) {
    usage(argv[0]);
    return 2;
  }

  try {
    std::ifstream input(argv[1]);
    if (!input) throw std::runtime_error("could not open input CSV");

    std::ofstream csv;
    if (argc == 3) {
      csv.open(argv[2]);
      if (!csv) throw std::runtime_error("could not open output CSV");
      csv << "slot,rows,first_s,last_s,active_pct,muted_pct,pcm8_pct,backwards_pct,"
             "ssctl_modes,lpctl_modes,eg_states,min_addr,max_addr,address_jumps,"
             "address_rewinds,lfo_wraps,min_modulation,max_modulation,rms,peak,"
             "output_nonzero_pct,pre_level_rms,level_db,min_final_level,max_final_level,"
             "min_alfo_level,max_alfo_level,min_level_mul,max_level_mul,min_level_shift,"
             "max_level_shift\n";
    }

    std::array<SlotSummary, kSlotCount> slots{};
    std::string line;
    uint64_t line_number = 0;
    while (std::getline(input, line)) {
      ++line_number;
      if (line.empty()) continue;
      if (line_number == 1 && line.rfind("sample,", 0) == 0) continue;
      const Row row = parse_row(line);
      if (row.slot >= kSlotCount) throw std::runtime_error("slot index out of range");
      update_summary(slots[row.slot], row);
    }

    std::printf("slot rows      first_s  last_s   pre_rms out_rms lvl_db  peak  mod_min mod_max jumps rewinds ssctl lpctl eg\n");
    for (uint32_t slot = 0; slot < kSlotCount; ++slot) {
      const SlotSummary& s = slots[slot];
      if (s.rows == 0) continue;
      const int64_t peak = std::max(std::llabs(s.min_output), std::llabs(s.max_output));
      const double pre_rms = pre_level_rms(s);
      const double out_rms = rms(s);
      std::printf("%02u   %-9llu %-8.3f %-8.3f %-7.1f %-7.1f %-7.2f %-5lld %-7lld %-7lld %-5llu %-7llu %-5s %-5s %s\n",
                  slot,
                  static_cast<unsigned long long>(s.rows),
                  seconds(s.first_sample),
                  seconds(s.last_sample),
                  pre_rms,
                  out_rms,
                  db_ratio(out_rms, pre_rms),
                  static_cast<long long>(peak),
                  static_cast<long long>(s.min_modulation),
                  static_cast<long long>(s.max_modulation),
                  static_cast<unsigned long long>(s.address_jumps),
                  static_cast<unsigned long long>(s.address_rewinds),
                  mask_list(s.ssctl_mask).c_str(),
                  mask_list(s.lpctl_mask).c_str(),
                  mask_list(s.eg_state_mask).c_str());

      if (csv) {
        csv << slot << "," << s.rows << "," << seconds(s.first_sample) << ","
            << seconds(s.last_sample) << "," << pct(s.active_rows, s.rows) << ","
            << pct(s.muted_rows, s.rows) << "," << pct(s.pcm8_rows, s.rows) << ","
            << pct(s.backwards_rows, s.rows) << "," << mask_list(s.ssctl_mask) << ","
            << mask_list(s.lpctl_mask) << "," << mask_list(s.eg_state_mask) << ","
            << s.min_addr << "," << s.max_addr << "," << s.address_jumps << ","
            << s.address_rewinds << "," << s.lfo_wraps << "," << s.min_modulation << ","
            << s.max_modulation << "," << out_rms << "," << peak << ","
            << pct(s.output_nonzero, s.rows) << "," << pre_rms << ","
            << db_ratio(out_rms, pre_rms) << ","
            << (s.level_trace_rows ? s.min_final_level : 0) << ","
            << (s.level_trace_rows ? s.max_final_level : 0) << ","
            << (s.level_trace_rows ? s.min_alfo_level : 0) << ","
            << (s.level_trace_rows ? s.max_alfo_level : 0) << ","
            << (s.level_trace_rows ? s.min_level_mul : 0) << ","
            << (s.level_trace_rows ? s.max_level_mul : 0) << ","
            << (s.level_trace_rows ? s.min_level_shift : 0) << ","
            << (s.level_trace_rows ? s.max_level_shift : 0) << "\n";
      }
    }
    return 0;
  } catch (const std::exception& ex) {
    std::fprintf(stderr, "scsp_slot_trace_summary: %s\n", ex.what());
    return 1;
  }
}
