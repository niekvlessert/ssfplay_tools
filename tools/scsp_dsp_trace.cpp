#include "../src/ssfplay_capture.h"

#include <algorithm>
#include <cmath>
#include <cctype>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <stdexcept>
#include <sstream>
#include <string>
#include <vector>

#include <zlib.h>

namespace {

struct ParsedVGM {
  std::vector<uint8_t> ram;
  std::vector<ssfplay_capture_event> events;
  uint64_t samples = 0;
};

static uint16_t get_u16(const std::vector<uint8_t>& data, size_t pos) {
  if (pos + 2 > data.size()) throw std::runtime_error("truncated u16");
  return static_cast<uint16_t>(data[pos] | (data[pos + 1] << 8));
}

static uint32_t get_u32(const std::vector<uint8_t>& data, size_t pos) {
  if (pos + 4 > data.size()) throw std::runtime_error("truncated u32");
  return static_cast<uint32_t>(data[pos] | (data[pos + 1] << 8) |
                               (data[pos + 2] << 16) | (data[pos + 3] << 24));
}

static bool ends_with_ci(const std::string& text, const char* suffix) {
  const size_t n = std::strlen(suffix);
  if (text.size() < n) return false;
  for (size_t i = 0; i < n; ++i) {
    const char a = static_cast<char>(std::tolower(
        static_cast<unsigned char>(text[text.size() - n + i])));
    const char b = static_cast<char>(std::tolower(
        static_cast<unsigned char>(suffix[i])));
    if (a != b) return false;
  }
  return true;
}

static std::vector<uint8_t> read_file(const std::string& path) {
  if (ends_with_ci(path, ".vgz")) {
    gzFile file = gzopen(path.c_str(), "rb");
    if (!file) throw std::runtime_error("could not open VGZ");
    std::vector<uint8_t> data;
    uint8_t buffer[65536];
    for (;;) {
      const int got = gzread(file, buffer, sizeof(buffer));
      if (got < 0) {
        int err = Z_OK;
        const char* msg = gzerror(file, &err);
        gzclose(file);
        throw std::runtime_error(msg ? msg : "VGZ read failed");
      }
      if (!got) break;
      data.insert(data.end(), buffer, buffer + got);
    }
    if (gzclose(file) != Z_OK) throw std::runtime_error("VGZ close failed");
    return data;
  }
  std::ifstream in(path.c_str(), std::ios::binary);
  if (!in) throw std::runtime_error("could not open VGM");
  return std::vector<uint8_t>(std::istreambuf_iterator<char>(in),
                              std::istreambuf_iterator<char>());
}

static void add_event(std::vector<ssfplay_capture_event>& events,
                      uint64_t sample, uint32_t address, uint8_t value,
                      bool ram) {
  ssfplay_capture_event e = {};
  e.sample = sample;
  e.address = address;
  e.value = value;
  e.type = ram ? SSFPLAY_CAPTURE_RAM_WRITE : SSFPLAY_CAPTURE_REGISTER_WRITE;
  events.push_back(e);
}

static ParsedVGM parse_vgm(const std::string& path, uint64_t limit_samples) {
  const std::vector<uint8_t> data = read_file(path);
  if (data.size() < 0x40 || std::memcmp(data.data(), "Vgm ", 4))
    throw std::runtime_error("not a VGM file");
  ParsedVGM out;
  out.ram.assign(0x80000, 0);
  out.samples = get_u32(data, 0x18);
  if (limit_samples && out.samples > limit_samples)
    out.samples = limit_samples;
  size_t pos = 0x40;
  const uint32_t data_offset = get_u32(data, 0x34);
  if (data_offset) pos = 0x34 + data_offset;
  uint64_t sample = 0;
  while (pos < data.size()) {
    const uint8_t cmd = data[pos++];
    if (cmd == 0x66) break;
    if (cmd == 0x61) {
      sample += get_u16(data, pos);
      pos += 2;
    } else if (cmd == 0x62) {
      sample += 735;
    } else if (cmd == 0x63) {
      sample += 882;
    } else if (cmd >= 0x70 && cmd <= 0x7F) {
      sample += (cmd & 0x0F) + 1;
    } else if (cmd == 0x67) {
      if (pos + 6 > data.size() || data[pos++] != 0x66)
        throw std::runtime_error("bad data block");
      const uint8_t type = data[pos++];
      const uint32_t size = get_u32(data, pos);
      pos += 4;
      if (pos + size > data.size()) throw std::runtime_error("truncated data block");
      if (type == 0xE0) {
        if (size < 4) throw std::runtime_error("bad SCSP RAM block");
        const uint32_t address = get_u32(data, pos);
        for (uint32_t i = 0; i < size - 4; ++i) {
          const uint32_t a = address + i;
          const uint8_t v = data[pos + 4 + i];
          if (a < out.ram.size()) out.ram[a] = v;
          if (sample <= out.samples) add_event(out.events, sample, a, v, true);
        }
      }
      pos += size;
    } else if (cmd == 0xC5) {
      if (pos + 3 > data.size()) throw std::runtime_error("truncated SCSP write");
      const uint32_t address = (static_cast<uint32_t>(data[pos]) << 8) | data[pos + 1];
      const uint8_t value = data[pos + 2];
      pos += 3;
      if (sample <= out.samples)
        add_event(out.events, sample, address & 0xFFF, value, false);
    } else {
      char msg[64];
      std::snprintf(msg, sizeof(msg), "unsupported VGM command 0x%02X", cmd);
      throw std::runtime_error(msg);
    }
    if (limit_samples && sample > limit_samples) break;
  }
  return out;
}

struct TraceRow {
  uint64_t sample = 0;
  std::vector<double> values;
};

static bool read_trace_row(std::istream& in, TraceRow& row) {
  std::string line;
  if (!std::getline(in, line)) return false;
  std::stringstream ss(line);
  std::string field;
  if (!std::getline(ss, field, ',')) return false;
  row.sample = static_cast<uint64_t>(std::strtoull(field.c_str(), nullptr, 10));
  row.values.clear();
  while (std::getline(ss, field, ','))
    row.values.push_back(std::strtod(field.c_str(), nullptr));
  return true;
}

static int compare_csv(const char* a_path, const char* b_path, const char* out_path) {
  std::ifstream a(a_path), b(b_path);
  std::ofstream out(out_path);
  if (!a || !b || !out) return 1;
  std::string ah, bh;
  std::getline(a, ah);
  std::getline(b, bh);
  out << "field,max_abs_diff,rms_diff,first_diff_sample\n";
  std::vector<std::string> fields;
  {
    std::stringstream ss(ah);
    std::string f;
    while (std::getline(ss, f, ',')) fields.push_back(f);
  }
  const size_t nfields = fields.size() > 1 ? fields.size() - 1 : 0;
  std::vector<double> max_abs(nfields, 0), sum_sq(nfields, 0);
  std::vector<int64_t> first(nfields, -1);
  uint64_t rows = 0;
  TraceRow ar, br;
  while (read_trace_row(a, ar) && read_trace_row(b, br)) {
    const size_t n = std::min(nfields, std::min(ar.values.size(), br.values.size()));
    for (size_t i = 0; i < n; ++i) {
      const double d = br.values[i] - ar.values[i];
      const double ad = std::abs(d);
      max_abs[i] = std::max(max_abs[i], ad);
      sum_sq[i] += d * d;
      if (ad && first[i] < 0) first[i] = static_cast<int64_t>(ar.sample);
    }
    ++rows;
  }
  for (size_t i = 0; i < nfields; ++i) {
    out << fields[i + 1] << ',' << max_abs[i] << ','
        << (rows ? std::sqrt(sum_sq[i] / rows) : 0) << ','
        << first[i] << '\n';
  }
  return 0;
}

static void usage(const char* argv0) {
  std::fprintf(stderr,
      "Usage:\n"
      "  %s mednafen [--limit-ms N] input.vgm|vgz trace.csv\n"
      "  %s compare trace-a.csv trace-b.csv report.csv\n",
      argv0, argv0);
}

}  // namespace

int main(int argc, char** argv) {
  try {
    if (argc >= 2 && !std::strcmp(argv[1], "compare")) {
      if (argc != 5) { usage(argv[0]); return 2; }
      return compare_csv(argv[2], argv[3], argv[4]);
    }
    if (argc >= 2 && !std::strcmp(argv[1], "mednafen")) {
      uint64_t limit_samples = 0;
      int arg = 2;
      if (arg + 1 < argc && !std::strcmp(argv[arg], "--limit-ms")) {
        const uint64_t ms = static_cast<uint64_t>(std::strtoull(argv[arg + 1], nullptr, 10));
        limit_samples = ms * 44100 / 1000;
        arg += 2;
      }
      if (argc - arg != 2) { usage(argv[0]); return 2; }
      ParsedVGM vgm = parse_vgm(argv[arg], limit_samples);
      std::vector<int16_t> pcm(static_cast<size_t>(vgm.samples) * 2);
      const ssfplay_result r = ssfplay_capture_replay_dsp_trace(
          vgm.ram.data(), vgm.ram.size(), vgm.events.data(), vgm.events.size(),
          vgm.samples, argv[arg + 1], pcm.data());
      if (r != SSFPLAY_OK) {
        std::fprintf(stderr, "mednafen trace failed: %s\n", ssfplay_result_string(r));
        return 1;
      }
      return 0;
    }
    usage(argv[0]);
    return 2;
  } catch (const std::exception& ex) {
    std::fprintf(stderr, "scsp_dsp_trace: %s\n", ex.what());
    return 1;
  }
}
