#pragma once

#include <stdint.h>

typedef void (*libvgm_ab_slot_sample_callback)(void* opaque,
                                               uint32_t slot,
                                               int32_t sample);
typedef void (*libvgm_ab_slot_keyon_callback)(void* opaque,
                                              uint32_t slot,
                                              uint32_t sample_address,
                                              uint32_t pitch);

#ifdef __cplusplus
extern "C" {
#endif

void libvgm_scsp_set_slot_sample_callback(
    libvgm_ab_slot_sample_callback callback,
    void* opaque);
void libvgm_scsp_set_slot_keyon_callback(
    libvgm_ab_slot_keyon_callback callback,
    void* opaque);
void libvgm_scsp_set_debug_output_mute_mask(uint32_t mute_mask);

#ifdef __cplusplus
}
#endif
