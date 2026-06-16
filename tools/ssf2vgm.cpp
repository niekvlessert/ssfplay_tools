#include <ssfplay/ssfplay.h>

#include "../src/ssfplay_capture.h"

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

#include <zlib.h>

namespace {

struct Range {
  uint32_t start = 0;
  uint32_t end = 0;
};

struct Report {
  unsigned max_polyphony = 0;
  bool memory_pcm = false;
  bool noise = false;
  bool pcm8 = false;
  bool pcm16 = false;
  bool envelope = false;
  bool alfo = false;
  bool plfo = false;
  bool modulation = false;
  bool dsp_program = false;
  bool dsp_routing = false;
  bool loop_modes[4] = {};
  uint32_t ram_write_min = 0xFFFFFFFF;
  uint32_t ram_write_max = 0;
  uint32_t configured_read_min = 0xFFFFFFFF;
  uint32_t configured_read_max = 0;
  uint64_t emitted_register_writes = 0;
  uint64_t emitted_ram_blocks = 0;
  uint64_t emitted_ram_bytes = 0;
  uint64_t skipped_register_noops = 0;
  uint64_t skipped_ram_noops = 0;
  uint64_t skipped_unread_ram_writes = 0;
};

static void put_u16(std::vector<uint8_t>& out, uint16_t value) {
  out.push_back(static_cast<uint8_t>(value));
  out.push_back(static_cast<uint8_t>(value >> 8));
}

static void put_u32(std::vector<uint8_t>& out, uint32_t value) {
  out.push_back(static_cast<uint8_t>(value));
  out.push_back(static_cast<uint8_t>(value >> 8));
  out.push_back(static_cast<uint8_t>(value >> 16));
  out.push_back(static_cast<uint8_t>(value >> 24));
}

static void set_u32(std::vector<uint8_t>& out, size_t offset, uint32_t value) {
  out[offset] = static_cast<uint8_t>(value);
  out[offset + 1] = static_cast<uint8_t>(value >> 8);
  out[offset + 2] = static_cast<uint8_t>(value >> 16);
  out[offset + 3] = static_cast<uint8_t>(value >> 24);
}

static void emit_wait(std::vector<uint8_t>& out, uint64_t samples) {
  while (samples) {
    if (samples == 735) {
      out.push_back(0x62);
      return;
    }
    if (samples == 882) {
      out.push_back(0x63);
      return;
    }
    if (samples <= 16) {
      out.push_back(static_cast<uint8_t>(0x70 + samples - 1));
      return;
    }
    const uint16_t chunk = static_cast<uint16_t>(std::min<uint64_t>(samples, 65535));
    out.push_back(0x61);
    put_u16(out, chunk);
    samples -= chunk;
  }
}

static void append_utf16(std::vector<uint8_t>& out, const char* text) {
  const unsigned char* p = reinterpret_cast<const unsigned char*>(text ? text : "");
  while (*p) {
    uint32_t cp;
    if (*p < 0x80) {
      cp = *p++;
    } else if ((*p & 0xE0) == 0xC0 && p[1]) {
      cp = ((*p & 0x1F) << 6) | (p[1] & 0x3F);
      p += 2;
    } else if ((*p & 0xF0) == 0xE0 && p[1] && p[2]) {
      cp = ((*p & 0x0F) << 12) | ((p[1] & 0x3F) << 6) | (p[2] & 0x3F);
      p += 3;
    } else if ((*p & 0xF8) == 0xF0 && p[1] && p[2] && p[3]) {
      cp = ((*p & 7) << 18) | ((p[1] & 0x3F) << 12) |
           ((p[2] & 0x3F) << 6) | (p[3] & 0x3F);
      p += 4;
    } else {
      cp = 0xFFFD;
      ++p;
    }
    if (cp <= 0xFFFF) {
      put_u16(out, static_cast<uint16_t>(cp));
    } else {
      cp -= 0x10000;
      put_u16(out, static_cast<uint16_t>(0xD800 | (cp >> 10)));
      put_u16(out, static_cast<uint16_t>(0xDC00 | (cp & 0x3FF)));
    }
  }
  put_u16(out, 0);
}

static std::string json_escape(const char* text) {
  std::ostringstream out;
  for (const unsigned char* p = reinterpret_cast<const unsigned char*>(text ? text : "");
       *p; ++p) {
    switch (*p) {
      case '\\': out << "\\\\"; break;
      case '"': out << "\\\""; break;
      case '\n': out << "\\n"; break;
      case '\r': out << "\\r"; break;
      case '\t': out << "\\t"; break;
      default:
        if (*p < 0x20) {
          char buf[8];
          std::snprintf(buf, sizeof(buf), "\\u%04x", *p);
          out << buf;
        } else {
          out << static_cast<char>(*p);
        }
    }
  }
  return out.str();
}

static uint16_t shadow_word(const uint8_t* shadow, unsigned address) {
  return static_cast<uint16_t>((shadow[address] << 8) | shadow[address + 1]);
}

static void add_range(std::vector<Range>& ranges, uint32_t start, uint32_t end) {
  if (start > end || start >= 0x80000)
    return;
  end = std::min<uint32_t>(end, 0x7FFFF);
  for (Range& range : ranges) {
    if (end + 1 < range.start || start > range.end + 1)
      continue;
    range.start = std::min(range.start, start);
    range.end = std::max(range.end, end);
    bool merged = true;
    while (merged) {
      merged = false;
      for (size_t i = 0; i < ranges.size(); ++i) {
        for (size_t j = i + 1; j < ranges.size(); ++j) {
          if (ranges[j].end + 1 < ranges[i].start ||
              ranges[j].start > ranges[i].end + 1)
            continue;
          ranges[i].start = std::min(ranges[i].start, ranges[j].start);
          ranges[i].end = std::max(ranges[i].end, ranges[j].end);
          ranges.erase(ranges.begin() + j);
          merged = true;
          break;
        }
        if (merged)
          break;
      }
    }
    return;
  }
  ranges.push_back(Range{start, end});
}

static bool in_ranges(const std::vector<Range>& ranges, uint32_t address) {
  for (const Range& range : ranges) {
    if (address >= range.start && address <= range.end)
      return true;
  }
  return false;
}

static void collect_sound_ram_ranges_from_shadow(const uint8_t* shadow,
                                                 std::vector<Range>& ranges,
                                                 Report& report) {
  bool any_dsp_routing = false;
  for (unsigned slot = 0; slot < 32; ++slot) {
    const unsigned base = slot * 0x20;
    const uint16_t control = shadow_word(shadow, base);
    any_dsp_routing |= shadow_word(shadow, base + 0x14) != 0;
    if ((control & 0x0800) == 0 || ((control >> 7) & 3) != 0)
      continue;

    const uint32_t start = ((control & 0xF) << 16) |
                           shadow_word(shadow, base + 0x02);
    const uint32_t bytes_per_sample = (control & 0x10) ? 1 : 2;
    const uint32_t loop_end = shadow_word(shadow, base + 0x06);
    if (!loop_end)
      continue;
    const uint32_t end = std::min<uint32_t>(
        0x7FFFF, start + loop_end * bytes_per_sample + 32);
    add_range(ranges, start, end);
    report.configured_read_min = std::min(report.configured_read_min, start);
    report.configured_read_max = std::max(report.configured_read_max, end);
  }

  bool any_dsp_program = false;
  for (unsigned i = 0x700; i < 0xC00; ++i) {
    any_dsp_program |= shadow[i] != 0;
  }
  if (any_dsp_program || any_dsp_routing) {
    const uint16_t rb = shadow_word(shadow, 0x402);
    const uint32_t rbp = rb & 0x7F;
    const uint32_t rbl = (rb >> 7) & 0x3;
    const uint32_t start = rbp << 12;
    const uint32_t words = 0x2000U << rbl;
    add_range(ranges, start * 2, std::min<uint32_t>(0x7FFFF, (start + words) * 2 - 1));
  }
}

static std::vector<Range> collect_sound_ram_ranges(
    const ssfplay_capture_event* events, size_t event_count, Report& report) {
  std::vector<Range> ranges;
  uint8_t shadow[0x1000] = {};
  for (size_t i = 0; i < event_count; ++i) {
    if (events[i].type != SSFPLAY_CAPTURE_REGISTER_WRITE)
      continue;
    shadow[events[i].address & 0xFFF] = events[i].value;
    collect_sound_ram_ranges_from_shadow(shadow, ranges, report);
  }
  return ranges;
}

static bool register_write_has_side_effect(uint32_t address, uint8_t value) {
  const uint32_t reg = address & 0xFFF;
  if (reg < 0x400) {
    const uint32_t word = reg & ~1U;
    if ((word & 0x1F) == 0x00 && (reg & 1) == 0 && (value & 0x10))
      return true;  // KYONEX on slot control high byte.
    return false;
  }
  if (reg >= 0x400 && reg < 0x430) {
    const uint32_t common = (reg >> 1) & 0x1F;
    if (common == 0x03 || common == 0x0B || common == 0x0F ||
        common == 0x10 || common == 0x11 || common == 0x12 ||
        common == 0x13 || common == 0x14 || common == 0x15 ||
        common == 0x16 || common == 0x17 || common == 0x18 ||
        common == 0x19)
      return true;  // MIDI, DMA execute, interrupt acknowledge/enable/status.
  }
  return false;
}

static void inspect_shadow(const uint8_t* shadow, Report& report) {
  unsigned active = 0;
  for (unsigned slot = 0; slot < 32; ++slot) {
    const unsigned base = slot * 0x20;
    const uint16_t control = shadow_word(shadow, base);
    if (control & 0x0800) {
      ++active;
      const uint32_t start = ((control & 0xF) << 16) |
                             shadow_word(shadow, base + 0x02);
      const uint32_t end = std::min<uint32_t>(
          0x7FFFF, start + shadow_word(shadow, base + 0x06) *
                               ((control & 0x10) ? 1 : 2));
      report.configured_read_min = std::min(report.configured_read_min, start);
      report.configured_read_max = std::max(report.configured_read_max, end);
    }
    const unsigned source = (control >> 7) & 3;
    report.memory_pcm |= source == 0;
    report.noise |= source != 0;
    report.loop_modes[(control >> 5) & 3] = true;
    report.pcm8 |= (control & 0x10) != 0;
    report.pcm16 |= (control & 0x10) == 0;
    report.envelope |= shadow_word(shadow, base + 0x08) ||
                       shadow_word(shadow, base + 0x0A);
    report.modulation |= shadow_word(shadow, base + 0x0E) != 0;
    const uint16_t lfo = shadow_word(shadow, base + 0x12);
    report.plfo |= (lfo & 0x0700) != 0;
    report.alfo |= (lfo & 0x0007) != 0;
    report.dsp_routing |= shadow_word(shadow, base + 0x14) != 0;
  }
  report.max_polyphony = std::max(report.max_polyphony, active);
}

static void write_report(const std::string& path, const ssfplay_decoder* decoder,
                         const Report& report, const ssfplay_capture_stats& stats,
                         uint64_t total_samples, size_t event_count,
                         uint64_t waveform_differences, unsigned maximum_delta) {
  std::ofstream out(path.c_str(), std::ios::binary);
  out << "{\n"
      << "  \"format\": \"ssf2vgm reachability report v1\",\n"
      << "  \"title\": \"" << json_escape(ssfplay_metadata(decoder, SSFPLAY_METADATA_TITLE)) << "\",\n"
      << "  \"duration_samples\": " << total_samples << ",\n"
      << "  \"events\": " << event_count << ",\n"
      << "  \"emitted_events\": "
      << (report.emitted_register_writes + report.emitted_ram_bytes) << ",\n"
      << "  \"maximum_polyphony\": " << report.max_polyphony << ",\n"
      << "  \"observed\": {\n"
      << "    \"memory_pcm\": " << (report.memory_pcm ? "true" : "false") << ",\n"
      << "    \"noise_or_non_memory_source\": " << (report.noise ? "true" : "false") << ",\n"
      << "    \"pcm_8_bit\": " << (report.pcm8 ? "true" : "false") << ",\n"
      << "    \"pcm_16_bit\": " << (report.pcm16 ? "true" : "false") << ",\n"
      << "    \"envelopes\": " << (report.envelope ? "true" : "false") << ",\n"
      << "    \"amplitude_lfo\": " << (report.alfo ? "true" : "false") << ",\n"
      << "    \"pitch_lfo\": " << (report.plfo ? "true" : "false") << ",\n"
      << "    \"inter_slot_modulation\": " << (report.modulation ? "true" : "false") << ",\n"
      << "    \"dsp_program_writes\": " << (report.dsp_program ? "true" : "false") << ",\n"
      << "    \"dsp_routing\": " << (report.dsp_routing ? "true" : "false") << "\n"
      << "  },\n"
      << "  \"loop_modes_observed\": [";
  bool first = true;
  for (unsigned i = 0; i < 4; ++i) {
    if (!report.loop_modes[i]) continue;
    out << (first ? "" : ", ") << i;
    first = false;
  }
  out << "],\n"
      << "  \"writes\": {\n"
      << "    \"byte_bus_writes\": " << stats.byte_writes << ",\n"
      << "    \"atomic_16_bit_register_writes_split_for_vgm\": "
      << stats.register_word_writes << ",\n"
      << "    \"16_bit_ram_writes_serialized_as_bytes\": " << stats.ram_word_writes << ",\n"
      << "    \"same_sample_event_collisions\": " << stats.same_sample_collisions << ",\n"
      << "    \"register_bytes\": " << stats.register_writes << ",\n"
      << "    \"ram_bytes\": " << stats.ram_writes << ",\n"
      << "    \"emitted_register_writes\": " << report.emitted_register_writes << ",\n"
      << "    \"emitted_ram_blocks\": " << report.emitted_ram_blocks << ",\n"
      << "    \"emitted_ram_bytes\": " << report.emitted_ram_bytes << ",\n"
      << "    \"skipped_register_noops\": " << report.skipped_register_noops << ",\n"
      << "    \"skipped_ram_noops\": " << report.skipped_ram_noops << ",\n"
      << "    \"skipped_unread_ram_writes\": " << report.skipped_unread_ram_writes << "\n"
      << "  },\n"
      << "  \"compact_vgm_replay_comparison\": {\"different_samples\": "
      << waveform_differences << ", \"maximum_pcm_delta\": " << maximum_delta
      << "},\n"
      << "  \"ram_written_range\": ";
  if (report.ram_write_min == 0xFFFFFFFF)
    out << "null,\n";
  else
    out << "{\"start\": " << report.ram_write_min << ", \"end\": "
        << report.ram_write_max << "},\n";
  out << "  \"configured_sample_read_range_approximation\": ";
  if (report.configured_read_min == 0xFFFFFFFF)
    out << "null,\n";
  else
    out << "{\"start\": " << report.configured_read_min << ", \"end\": "
        << report.configured_read_max << "},\n";
  out << "  \"warnings\": [";
  bool wrote_warning = false;
  if (stats.register_word_writes) {
    out << "\"Atomic 16-bit SCSP writes are represented as two ordered byte writes.\"";
    wrote_warning = true;
  }
  if (report.skipped_unread_ram_writes) {
    out << (wrote_warning ? ", " : "")
        << "\"Runtime RAM writes outside observed SCSP sample/DSP read ranges were omitted from compact VGM output.\"";
  }
  out << "],\n"
      << "  \"reachability_note\": \"Only behavior executed during this playback is reported; dormant or absent game sounds are not inferred.\",\n"
      << "  \"unreachable_classes\": [\"content absent from the SSF/SSFLIB rip\", \"disc-loaded assets\", \"sounds requiring SH-2 or gameplay triggers\", \"CD-DA or external SCSP input\", \"dormant sounds with unknown trigger protocol\"]\n"
      << "}\n";
}

static bool write_vgm(const std::string& path, const ssfplay_decoder* decoder,
                      const uint8_t* ram, size_t ram_size,
                      const ssfplay_capture_event* events, size_t event_count,
                      uint64_t total_samples, bool all_ram_writes,
                      Report& report,
                      std::vector<ssfplay_capture_event>* emitted_events) {
  if (total_samples > 0xFFFFFFFFULL) return false;
  std::vector<uint8_t> out(0x100, 0);
  std::memcpy(out.data(), "Vgm ", 4);
  set_u32(out, 0x08, 0x00000171);
  set_u32(out, 0x18, static_cast<uint32_t>(total_samples));
  set_u32(out, 0x34, 0xCC);
  set_u32(out, 0xB8, 22579200);

  out.push_back(0x67);
  out.push_back(0x66);
  out.push_back(0xE0);
  put_u32(out, static_cast<uint32_t>(ram_size + 4));
  put_u32(out, 0);
  out.insert(out.end(), ram, ram + ram_size);

  const std::vector<Range> sound_ram_ranges =
      all_ram_writes ? std::vector<Range>()
                     : collect_sound_ram_ranges(events, event_count, report);

  uint8_t shadow[0x1000] = {};
  std::vector<uint8_t> ram_shadow(ram, ram + ram_size);
  uint64_t position = 0;
  for (size_t i = 0; i < event_count;) {
    if (events[i].sample > total_samples) break;
    if (events[i].type == SSFPLAY_CAPTURE_RAM_WRITE &&
        (!all_ram_writes && !in_ranges(sound_ram_ranges, events[i].address))) {
      report.skipped_unread_ram_writes++;
      ++i;
      continue;
    }
    if (events[i].type == SSFPLAY_CAPTURE_RAM_WRITE &&
        events[i].address < ram_shadow.size() &&
        ram_shadow[events[i].address] == events[i].value) {
      report.skipped_ram_noops++;
      ++i;
      continue;
    }
    if (events[i].type == SSFPLAY_CAPTURE_REGISTER_WRITE &&
        shadow[events[i].address & 0xFFF] == events[i].value &&
        !register_write_has_side_effect(events[i].address, events[i].value)) {
      report.skipped_register_noops++;
      ++i;
      continue;
    }
    emit_wait(out, events[i].sample - position);
    position = events[i].sample;
    if (events[i].type == SSFPLAY_CAPTURE_RAM_WRITE) {
      const uint64_t sample = events[i].sample;
      const uint32_t start = events[i].address;
      size_t end = i + 1;
      while (end < event_count && events[end].sample == sample &&
             events[end].type == SSFPLAY_CAPTURE_RAM_WRITE &&
             events[end].address == events[end - 1].address + 1 &&
             (all_ram_writes || in_ranges(sound_ram_ranges, events[end].address)) &&
             events[end].address < ram_shadow.size() &&
             ram_shadow[events[end].address] != events[end].value)
        ++end;
      out.push_back(0x67);
      out.push_back(0x66);
      out.push_back(0xE0);
      put_u32(out, static_cast<uint32_t>((end - i) + 4));
      put_u32(out, start);
      for (size_t j = i; j < end; ++j) {
        out.push_back(events[j].value);
        if (events[j].address < ram_shadow.size())
          ram_shadow[events[j].address] = events[j].value;
        if (emitted_events)
          emitted_events->push_back(events[j]);
      }
      report.emitted_ram_blocks++;
      report.emitted_ram_bytes += end - i;
      report.ram_write_min = std::min(report.ram_write_min, start);
      report.ram_write_max = std::max(report.ram_write_max,
                                      events[end - 1].address);
      i = end;
    } else {
      out.push_back(0xC5);
      out.push_back(static_cast<uint8_t>(events[i].address >> 8));
      out.push_back(static_cast<uint8_t>(events[i].address));
      out.push_back(events[i].value);
      shadow[events[i].address & 0xFFF] = events[i].value;
      report.emitted_register_writes++;
      if (emitted_events)
        emitted_events->push_back(events[i]);
      if (events[i].address >= 0x700 && events[i].address < 0xC00)
        report.dsp_program = true;
      inspect_shadow(shadow, report);
      ++i;
    }
  }
  emit_wait(out, total_samples - position);
  out.push_back(0x66);

  const size_t gd3_offset = out.size();
  std::vector<uint8_t> gd3;
  append_utf16(gd3, ssfplay_metadata(decoder, SSFPLAY_METADATA_TITLE));
  append_utf16(gd3, "");
  append_utf16(gd3, ssfplay_metadata(decoder, SSFPLAY_METADATA_GAME));
  append_utf16(gd3, "");
  append_utf16(gd3, "Sega Saturn");
  append_utf16(gd3, "");
  append_utf16(gd3, ssfplay_metadata(decoder, SSFPLAY_METADATA_ARTIST));
  append_utf16(gd3, "");
  append_utf16(gd3, ssfplay_metadata(decoder, SSFPLAY_METADATA_YEAR));
  append_utf16(gd3, "ssf2vgm");
  append_utf16(gd3, "Captured from native Mednafen SCSP activity; fade omitted.");
  out.insert(out.end(), {'G', 'd', '3', ' '});
  put_u32(out, 0x00000100);
  put_u32(out, static_cast<uint32_t>(gd3.size()));
  out.insert(out.end(), gd3.begin(), gd3.end());
  set_u32(out, 0x14, static_cast<uint32_t>(gd3_offset - 0x14));
  set_u32(out, 0x04, static_cast<uint32_t>(out.size() - 4));

  const bool gzip = path.size() >= 4 &&
                    (path.substr(path.size() - 4) == ".vgz" ||
                     path.substr(path.size() - 4) == ".VGZ");
  if (gzip) {
    gzFile file = gzopen(path.c_str(), "wb9");
    if (!file) return false;
    size_t position = 0;
    while (position < out.size()) {
      const unsigned chunk = static_cast<unsigned>(
          std::min<size_t>(out.size() - position, 1U << 30));
      if (gzwrite(file, out.data() + position, chunk) != static_cast<int>(chunk)) {
        gzclose(file);
        return false;
      }
      position += chunk;
    }
    return gzclose(file) == Z_OK;
  } else {
    std::ofstream file(path.c_str(), std::ios::binary);
    file.write(reinterpret_cast<const char*>(out.data()), out.size());
    return file.good();
  }
}

static void usage(const char* argv0) {
  std::fprintf(stderr, "Usage: %s [--length-ms N] [--report report.json] [--all-ram-writes] input.ssf output.vgm|output.vgz\n", argv0);
}

}  // namespace

int main(int argc, char** argv) {
  int64_t length_override = -1;
  std::string report_path;
  bool all_ram_writes = false;
  std::vector<const char*> positional;
  for (int i = 1; i < argc; ++i) {
    if (!std::strcmp(argv[i], "--length-ms") && i + 1 < argc) {
      length_override = std::strtoll(argv[++i], nullptr, 10);
    } else if (!std::strcmp(argv[i], "--report") && i + 1 < argc) {
      report_path = argv[++i];
    } else if (!std::strcmp(argv[i], "--all-ram-writes")) {
      all_ram_writes = true;
    } else {
      positional.push_back(argv[i]);
    }
  }
  if (positional.size() != 2 || length_override < -1) {
    usage(argv[0]);
    return 2;
  }

  ssfplay_config config;
  ssfplay_config_init(&config);
  config.sample_rate = 44100;
  config.fade_ms = 0;
  config.length_ms = length_override;
  ssfplay_decoder* decoder = nullptr;
  ssfplay_result result = ssfplay_open(positional[0], &config, &decoder);
  if (result != SSFPLAY_OK) {
    std::fprintf(stderr, "ssf2vgm: %s: %s\n", ssfplay_result_string(result),
                 ssfplay_last_error());
    return 1;
  }
  if (ssfplay_length_ms(decoder) <= 0) {
    std::fprintf(stderr, "ssf2vgm: input has no tagged duration; use --length-ms\n");
    ssfplay_close(decoder);
    return 1;
  }
  if (length_override >= 0)
    std::fprintf(stderr, "ssf2vgm: duration override active: %lld ms\n",
                 static_cast<long long>(length_override));
  if (ssfplay_capture_begin(decoder) != SSFPLAY_OK) {
    std::fprintf(stderr, "ssf2vgm: capture failed: %s\n", ssfplay_error(decoder));
    ssfplay_close(decoder);
    return 1;
  }

  int16_t pcm[4096 * 2];
  std::vector<int16_t> direct_pcm;
  if (!report_path.empty())
    direct_pcm.reserve(static_cast<size_t>(ssfplay_length_ms(decoder)) * 44100 / 500);
  for (;;) {
    size_t rendered = 0;
    result = ssfplay_render(decoder, pcm, 4096, &rendered);
    if (!report_path.empty())
      direct_pcm.insert(direct_pcm.end(), pcm, pcm + rendered * 2);
    if (result == SSFPLAY_EOF) break;
    if (result != SSFPLAY_OK) {
      std::fprintf(stderr, "ssf2vgm: render failed: %s\n", ssfplay_error(decoder));
      ssfplay_close(decoder);
      return 1;
    }
  }
  ssfplay_capture_end(decoder);

  size_t ram_size = 0, event_count = 0;
  const uint8_t* ram = ssfplay_capture_initial_ram(decoder, &ram_size);
  const ssfplay_capture_event* events = ssfplay_capture_events(decoder, &event_count);
  const ssfplay_capture_stats stats = ssfplay_capture_get_stats(decoder);
  const uint64_t total_samples =
      static_cast<uint64_t>(ssfplay_length_ms(decoder)) * 44100 / 1000;
  Report report;
  std::vector<ssfplay_capture_event> emitted_events;
  if (!write_vgm(positional[1], decoder, ram, ram_size, events, event_count,
                 total_samples, all_ram_writes, report, &emitted_events)) {
    std::fprintf(stderr, "ssf2vgm: could not write VGM (or duration exceeds VGM limit)\n");
    ssfplay_close(decoder);
    return 1;
  }
  if (!report_path.empty()) {
    std::vector<int16_t> replay_pcm(static_cast<size_t>(total_samples) * 2);
    uint64_t differences = 0;
    unsigned maximum_delta = 0;
    const ssfplay_capture_event* replay_events =
        emitted_events.empty() ? nullptr : emitted_events.data();
    if (ssfplay_capture_replay(ram, ram_size, replay_events,
                               emitted_events.size(), total_samples,
                               replay_pcm.data()) == SSFPLAY_OK) {
      const size_t count = std::min(direct_pcm.size(), replay_pcm.size());
      for (size_t i = 0; i < count; ++i) {
        const unsigned delta = static_cast<unsigned>(
            std::abs(static_cast<int>(direct_pcm[i]) -
                     static_cast<int>(replay_pcm[i])));
        differences += delta != 0;
        maximum_delta = std::max(maximum_delta, delta);
      }
      differences += direct_pcm.size() > count ? direct_pcm.size() - count
                                               : replay_pcm.size() - count;
    } else {
      differences = UINT64_MAX;
    }
    write_report(report_path, decoder, report, stats, total_samples, event_count,
                 differences, maximum_delta);
  }

  std::printf("wrote %s: %llu samples, %zu raw events, %zu emitted events, %llu atomic word-write warnings\n",
              positional[1], static_cast<unsigned long long>(total_samples),
              event_count, emitted_events.size(),
              static_cast<unsigned long long>(stats.register_word_writes));
  ssfplay_close(decoder);
  return 0;
}
