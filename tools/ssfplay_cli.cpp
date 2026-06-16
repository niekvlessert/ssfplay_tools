#include <ssfplay/ssfplay.h>

#include <atomic>
#include <csignal>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>

#if defined(_WIN32)
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <mmsystem.h>
#elif defined(__APPLE__)
#include <AudioToolbox/AudioToolbox.h>
#include <unistd.h>
#elif defined(SSFPLAY_HAVE_ALSA)
#include <alsa/asoundlib.h>
#else
#include <chrono>
#include <thread>
#endif

namespace {

std::atomic<bool> g_running(true);
ssfplay_decoder* g_decoder = nullptr;

constexpr size_t kFramesPerBuffer = 4096;

void handle_signal(int) {
  g_running = false;
}

bool render_frames(std::vector<int16_t>& pcm, size_t frames, size_t* rendered) {
  pcm.resize(frames * 2);
  ssfplay_result r = ssfplay_render(g_decoder, pcm.data(), frames, rendered);
  if (r == SSFPLAY_EOF || *rendered == 0) {
    g_running = false;
    return *rendered != 0;
  }
  if (r != SSFPLAY_OK) {
    std::fprintf(stderr, "ssfplay_cli: render error: %s\n", ssfplay_error(g_decoder));
    g_running = false;
    return false;
  }
  return true;
}

#if defined(_WIN32)
bool play_decoder(uint32_t sample_rate) {
  HWAVEOUT wave = nullptr;
  WAVEFORMATEX fmt = {};
  fmt.wFormatTag = WAVE_FORMAT_PCM;
  fmt.nChannels = 2;
  fmt.nSamplesPerSec = sample_rate;
  fmt.wBitsPerSample = 16;
  fmt.nBlockAlign = 4;
  fmt.nAvgBytesPerSec = sample_rate * fmt.nBlockAlign;
  if (waveOutOpen(&wave, WAVE_MAPPER, &fmt, 0, 0, CALLBACK_NULL) != MMSYSERR_NOERROR) {
    std::fprintf(stderr, "ssfplay_cli: waveOutOpen failed\n");
    return false;
  }
  while (g_running) {
    std::vector<int16_t> pcm;
    size_t rendered = 0;
    if (!render_frames(pcm, kFramesPerBuffer, &rendered) && !rendered) break;
    WAVEHDR hdr = {};
    hdr.lpData = reinterpret_cast<LPSTR>(pcm.data());
    hdr.dwBufferLength = static_cast<DWORD>(rendered * 4);
    if (waveOutPrepareHeader(wave, &hdr, sizeof(hdr)) != MMSYSERR_NOERROR) break;
    if (waveOutWrite(wave, &hdr, sizeof(hdr)) != MMSYSERR_NOERROR) break;
    while (g_running && !(hdr.dwFlags & WHDR_DONE)) Sleep(5);
    waveOutUnprepareHeader(wave, &hdr, sizeof(hdr));
  }
  waveOutReset(wave);
  waveOutClose(wave);
  return true;
}
#elif defined(__APPLE__)
AudioQueueRef g_queue = nullptr;

void fill_audioqueue_buffer(AudioQueueBufferRef buffer) {
  std::vector<int16_t> pcm;
  size_t rendered = 0;
  render_frames(pcm, kFramesPerBuffer, &rendered);
  buffer->mAudioDataByteSize = static_cast<UInt32>(rendered * 4);
  if (rendered)
    std::memcpy(buffer->mAudioData, pcm.data(), rendered * 4);
}

void audioqueue_callback(void*, AudioQueueRef, AudioQueueBufferRef buffer) {
  if (!g_running) return;
  fill_audioqueue_buffer(buffer);
  if (g_running)
    AudioQueueEnqueueBuffer(g_queue, buffer, 0, nullptr);
}

bool play_decoder(uint32_t sample_rate) {
  AudioStreamBasicDescription fmt = {};
  fmt.mSampleRate = sample_rate;
  fmt.mFormatID = kAudioFormatLinearPCM;
  fmt.mFormatFlags = kLinearPCMFormatFlagIsSignedInteger | kLinearPCMFormatFlagIsPacked;
  fmt.mChannelsPerFrame = 2;
  fmt.mBitsPerChannel = 16;
  fmt.mBytesPerFrame = 4;
  fmt.mBytesPerPacket = 4;
  fmt.mFramesPerPacket = 1;
  OSStatus err = AudioQueueNewOutput(&fmt, audioqueue_callback, nullptr, nullptr, nullptr, 0, &g_queue);
  if (err != noErr) {
    std::fprintf(stderr, "ssfplay_cli: AudioQueueNewOutput failed: %d\n", static_cast<int>(err));
    return false;
  }
  AudioQueueBufferRef buffers[3] = {};
  for (auto& buffer : buffers) {
    err = AudioQueueAllocateBuffer(g_queue, kFramesPerBuffer * 4, &buffer);
    if (err != noErr) return false;
    fill_audioqueue_buffer(buffer);
    AudioQueueEnqueueBuffer(g_queue, buffer, 0, nullptr);
  }
  AudioQueueStart(g_queue, nullptr);
  while (g_running) usleep(10000);
  AudioQueueStop(g_queue, true);
  AudioQueueDispose(g_queue, true);
  g_queue = nullptr;
  return true;
}
#elif defined(SSFPLAY_HAVE_ALSA)
bool play_decoder(uint32_t sample_rate) {
  snd_pcm_t* pcm_handle = nullptr;
  if (snd_pcm_open(&pcm_handle, "default", SND_PCM_STREAM_PLAYBACK, 0) < 0) {
    std::fprintf(stderr, "ssfplay_cli: unable to open ALSA default device\n");
    return false;
  }
  if (snd_pcm_set_params(pcm_handle, SND_PCM_FORMAT_S16_LE,
                         SND_PCM_ACCESS_RW_INTERLEAVED, 2, sample_rate, 1,
                         100000) < 0) {
    std::fprintf(stderr, "ssfplay_cli: unable to configure ALSA device\n");
    snd_pcm_close(pcm_handle);
    return false;
  }
  while (g_running) {
    std::vector<int16_t> pcm;
    size_t rendered = 0;
    if (!render_frames(pcm, kFramesPerBuffer, &rendered) && !rendered) break;
    size_t offset = 0;
    while (offset < rendered && g_running) {
      snd_pcm_sframes_t wrote = snd_pcm_writei(
          pcm_handle, pcm.data() + offset * 2, rendered - offset);
      if (wrote < 0) wrote = snd_pcm_recover(pcm_handle, static_cast<int>(wrote), 0);
      if (wrote < 0) {
        std::fprintf(stderr, "ssfplay_cli: ALSA write failed\n");
        g_running = false;
        break;
      }
      offset += static_cast<size_t>(wrote);
    }
  }
  snd_pcm_drain(pcm_handle);
  snd_pcm_close(pcm_handle);
  return true;
}
#else
bool play_decoder(uint32_t sample_rate) {
  std::fprintf(stderr, "ssfplay_cli: no audio backend was built; rendering silently\n");
  while (g_running) {
    std::vector<int16_t> pcm;
    size_t rendered = 0;
    if (!render_frames(pcm, kFramesPerBuffer, &rendered) && !rendered) break;
    std::this_thread::sleep_for(std::chrono::milliseconds(rendered * 1000 / sample_rate));
  }
  return true;
}
#endif

void usage(const char* argv0) {
  std::printf("Usage: %s [--rate hz] [--quality 0-10] file.ssf [...]\n", argv0);
}

}  // namespace

int main(int argc, char** argv) {
  ssfplay_config config;
  ssfplay_config_init(&config);
  int first_file = 1;
  for (; first_file < argc; ++first_file) {
    if (!std::strcmp(argv[first_file], "--help") || !std::strcmp(argv[first_file], "-h")) {
      usage(argv[0]);
      return 0;
    }
    if (!std::strcmp(argv[first_file], "--quality") || !std::strcmp(argv[first_file], "-q")) {
      if (++first_file >= argc) { usage(argv[0]); return 2; }
      config.resampler_quality = static_cast<uint32_t>(std::strtoul(argv[first_file], nullptr, 10));
      continue;
    }
    if (!std::strcmp(argv[first_file], "--rate") || !std::strcmp(argv[first_file], "-r")) {
      if (++first_file >= argc) { usage(argv[0]); return 2; }
      config.sample_rate = static_cast<uint32_t>(std::strtoul(argv[first_file], nullptr, 10));
      continue;
    }
    break;
  }
  if (first_file >= argc) {
    usage(argv[0]);
    return 2;
  }

  std::signal(SIGINT, handle_signal);
  std::signal(SIGTERM, handle_signal);

  for (int i = first_file; i < argc && g_running; ++i) {
    ssfplay_decoder* decoder = nullptr;
    ssfplay_result result = ssfplay_open(argv[i], &config, &decoder);
    if (result != SSFPLAY_OK) {
      std::fprintf(stderr, "ssfplay_cli: %s: %s%s%s\n", argv[i],
                   ssfplay_result_string(result),
                   ssfplay_last_error()[0] ? ": " : "", ssfplay_last_error());
      continue;
    }
    std::printf("Playing: %s\n", ssfplay_metadata(decoder, SSFPLAY_METADATA_TITLE));
    g_decoder = decoder;
    g_running = true;
    play_decoder(ssfplay_sample_rate(decoder));
    g_decoder = nullptr;
    ssfplay_close(decoder);
    g_running = true;
  }
  return 0;
}
