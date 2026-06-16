#include <ssfplay/ssfplay.h>

#include "../src/ssfplay_capture.h"

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>

static uint16_t read_u16(const uint8_t* p) {
  return static_cast<uint16_t>(p[0] | (p[1] << 8));
}

static uint32_t read_u32(const uint8_t* p) {
  return p[0] | (p[1] << 8) | (p[2] << 16) | (p[3] << 24);
}

static bool parse_vgm(const char* path, std::vector<uint8_t>& ram,
                      std::vector<ssfplay_capture_event>& events,
                      uint32_t& total_samples) {
  std::ifstream file(path, std::ios::binary);
  std::vector<uint8_t> data((std::istreambuf_iterator<char>(file)),
                            std::istreambuf_iterator<char>());
  if (data.size() < 0x100 || std::string(reinterpret_cast<char*>(data.data()), 4) != "Vgm ")
    return false;
  if (read_u32(&data[0x08]) != 0x171 || read_u32(&data[0xB8]) != 22579200)
    return false;
  total_samples = read_u32(&data[0x18]);
  size_t pos = 0x34 + read_u32(&data[0x34]);
  uint64_t sample = 0;
  bool have_initial_ram = false;
  while (pos < data.size()) {
    const uint8_t cmd = data[pos++];
    if (cmd == 0x66) break;
    if (cmd == 0x61) {
      if (pos + 2 > data.size()) return false;
      sample += read_u16(&data[pos]);
      pos += 2;
    } else if (cmd == 0x62) {
      sample += 735;
    } else if (cmd == 0x63) {
      sample += 882;
    } else if ((cmd & 0xF0) == 0x70) {
      sample += (cmd & 0x0F) + 1;
    } else if (cmd == 0xC5) {
      if (pos + 3 > data.size()) return false;
      ssfplay_capture_event event = {};
      event.sample = sample;
      event.type = SSFPLAY_CAPTURE_REGISTER_WRITE;
      event.address = static_cast<uint32_t>((data[pos] << 8) | data[pos + 1]);
      event.value = data[pos + 2];
      events.push_back(event);
      pos += 3;
    } else if (cmd == 0x67) {
      if (pos + 6 > data.size() || data[pos++] != 0x66 || data[pos++] != 0xE0)
        return false;
      const uint32_t size = read_u32(&data[pos]);
      pos += 4;
      if (size < 4 || pos + size > data.size()) return false;
      const uint32_t address = read_u32(&data[pos]);
      pos += 4;
      const uint32_t bytes = size - 4;
      if (!have_initial_ram && sample == 0 && address == 0 && bytes == 0x80000) {
        ram.assign(data.begin() + pos, data.begin() + pos + bytes);
        have_initial_ram = true;
      } else {
        for (uint32_t i = 0; i < bytes; ++i) {
          ssfplay_capture_event event = {};
          event.sample = sample;
          event.type = SSFPLAY_CAPTURE_RAM_WRITE;
          event.address = address + i;
          event.value = data[pos + i];
          events.push_back(event);
        }
      }
      pos += bytes;
    } else {
      return false;
    }
  }
  return have_initial_ram && sample == total_samples;
}

int main(int argc, char** argv) {
  if (argc != 3) return 2;
  std::vector<uint8_t> ram;
  std::vector<ssfplay_capture_event> events;
  uint32_t samples = 0;
  if (!parse_vgm(argv[2], ram, events, samples)) {
    std::fprintf(stderr, "could not parse generated VGM\n");
    return 1;
  }

  ssfplay_config config;
  ssfplay_config_init(&config);
  config.sample_rate = 44100;
  config.length_ms = static_cast<int64_t>(samples) * 1000 / 44100;
  config.fade_ms = 0;
  ssfplay_decoder* decoder = nullptr;
  if (ssfplay_open(argv[1], &config, &decoder) != SSFPLAY_OK) return 1;
  std::vector<int16_t> direct(static_cast<size_t>(samples) * 2);
  size_t position = 0;
  while (position < samples) {
    size_t rendered = 0;
    ssfplay_result result = ssfplay_render(
        decoder, direct.data() + position * 2, samples - position, &rendered);
    position += rendered;
    if (result == SSFPLAY_EOF) break;
    if (result != SSFPLAY_OK) return 1;
  }
  ssfplay_close(decoder);
  if (position != samples) return 1;

  std::vector<int16_t> replay(static_cast<size_t>(samples) * 2);
  if (ssfplay_capture_replay(ram.data(), ram.size(), events.data(), events.size(),
                             samples, replay.data()) != SSFPLAY_OK)
    return 1;
  size_t differences = 0;
  int maximum_delta = 0;
  for (size_t i = 0; i < replay.size(); ++i) {
    if (direct[i] != replay[i]) ++differences;
    maximum_delta = std::max(maximum_delta, std::abs(static_cast<int>(direct[i]) -
                                                     static_cast<int>(replay[i])));
  }
  std::printf("parsed %zu VGM events; waveform differences=%zu max_delta=%d\n",
              events.size(), differences, maximum_delta);
  return 0;
}
