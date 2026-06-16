#include <ssfplay/ssfplay.h>

#include <foobar2000/SDK/foobar2000.h>

namespace {

class input_ssfplay : public input_stubs {
 public:
  void open(service_ptr_t<file> hint, const char* path,
            t_input_open_reason reason, abort_callback& abort) override {
    if (reason == input_open_info_write)
      throw exception_io_data();
    m_path = path;
    m_stats = hint.is_valid() ? hint->get_stats(abort) : filesystem::g_get_stats(path, abort);
    ssfplay_config config;
    ssfplay_config_init(&config);
    ssfplay_decoder* decoder = nullptr;
    ssfplay_result result = ssfplay_open(path, &config, &decoder);
    if (result != SSFPLAY_OK)
      throw exception_io_data(ssfplay_last_error());
    m_decoder = decoder;
  }

  void get_info(file_info& info, abort_callback&) override {
    require_decoder();
    info.set_length(static_cast<double>(ssfplay_length_ms(m_decoder)) / 1000.0);
    set_meta(info, "title", SSFPLAY_METADATA_TITLE);
    set_meta(info, "album", SSFPLAY_METADATA_GAME);
    set_meta(info, "artist", SSFPLAY_METADATA_ARTIST);
    set_meta(info, "date", SSFPLAY_METADATA_YEAR);
    set_meta(info, "genre", SSFPLAY_METADATA_GENRE);
    set_meta(info, "copyright", SSFPLAY_METADATA_COPYRIGHT);
    info.info_set_int("samplerate", ssfplay_sample_rate(m_decoder));
    info.info_set_int("channels", 2);
    info.info_set_int("bitspersample", 16);
    info.info_set("codec", "Sega Saturn Sound Format");
  }

  t_filestats get_file_stats(abort_callback&) override {
    return m_stats;
  }

  void decode_initialize(unsigned, abort_callback&) override {
    require_decoder();
    ssfplay_reset(m_decoder);
  }

  bool decode_run(audio_chunk& chunk, abort_callback&) override {
    require_decoder();
    int16_t pcm[2048 * 2];
    size_t rendered = 0;
    ssfplay_result result = ssfplay_render(m_decoder, pcm, 2048, &rendered);
    if (!rendered)
      return false;
    chunk.set_data_fixedpoint(pcm, rendered * 2, ssfplay_sample_rate(m_decoder), 2, 16, audio_chunk::channel_config_stereo);
    return result == SSFPLAY_OK || result == SSFPLAY_EOF;
  }

  bool decode_can_seek() override { return false; }
  void decode_seek(double, abort_callback&) override { throw exception_io_data(); }
  bool decode_get_dynamic_info(file_info&, double&) override { return false; }
  bool decode_get_dynamic_info_track(file_info&, double&) override { return false; }
  void decode_on_idle(abort_callback&) override {}

  static bool g_is_our_path(const char* path, const char*) {
    const char* ext = strrchr(path, '.');
    return ext && (!stricmp_utf8(ext, ".ssf") || !stricmp_utf8(ext, ".minissf"));
  }

  static bool g_is_our_content_type(const char*) { return false; }
  static void g_get_name(pfc::string_base& out) { out = "SSF"; }
  static GUID g_get_guid() {
    static const GUID guid = {0x1c3d41f8, 0xc3e9, 0x4f6d, {0xa6, 0x95, 0x5e, 0x3d, 0xe7, 0x36, 0x8f, 0x41}};
    return guid;
  }

  ~input_ssfplay() override {
    if (m_decoder)
      ssfplay_close(m_decoder);
  }

 private:
  void require_decoder() {
    if (!m_decoder)
      throw exception_io_data();
  }

  void set_meta(file_info& info, const char* name, ssfplay_metadata_field field) {
    const char* value = ssfplay_metadata(m_decoder, field);
    if (value && value[0])
      info.meta_set(name, value);
  }

  ssfplay_decoder* m_decoder = nullptr;
  pfc::string8 m_path;
  t_filestats m_stats = {};
};

static input_factory_t<input_ssfplay> g_input_ssfplay_factory;

}  // namespace

DECLARE_COMPONENT_VERSION("SSF input", "0.1.0", "Sega Saturn Sound Format input using ssfplay");
VALIDATE_COMPONENT_FILENAME("foo_ssfplay.dll");
