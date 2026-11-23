#include <stdint.h>
#include <naomi/video.h>

#define BIOS_DIMM_CONFIG_ADDR 0x0C01FC04u
#define DIMM_COMMAND_ADDR     0xA05F703Cu
#define DIMM_OFFSET_ADDR      0xA05F7040u
#define DIMM_PARAM_LO_ADDR    0xA05F7044u
#define DIMM_PARAM_HI_ADDR    0xA05F7048u
#define DIMM_STATUS_ADDR      0xA05F704Cu

#define REG32(addr) (*(volatile uint32_t *)(uintptr_t)(addr))
#define REG16(addr) (*(volatile uint16_t *)(uintptr_t)(addr))

int main(void)
{
    video_init(VIDEO_COLOR_1555);
    video_set_background_color(rgb(12, 18, 28));

    for (;;) {
        uint32_t bios_config = REG32(BIOS_DIMM_CONFIG_ADDR);
        uint16_t command = REG16(DIMM_COMMAND_ADDR);
        uint16_t offset = REG16(DIMM_OFFSET_ADDR);
        uint16_t param_lo = REG16(DIMM_PARAM_LO_ADDR);
        uint16_t param_hi = REG16(DIMM_PARAM_HI_ADDR);
        uint16_t status = REG16(DIMM_STATUS_ADDR);

        video_fill_screen(rgb(12, 18, 28));
        video_draw_debug_text(24, 36, rgb(255, 255, 255), "NETDIMM HLE PREFLIGHT");
        video_draw_debug_text(24, 76, rgb(200, 220, 255), "BIOS CFG  [0C01FC04] : %08X", bios_config);
        video_draw_debug_text(24, 100, rgb(200, 220, 255), "DIMM CMD  [A05F703C] : %04X", command);
        video_draw_debug_text(24, 124, rgb(200, 220, 255), "DIMM OFF  [A05F7040] : %04X", offset);
        video_draw_debug_text(24, 148, rgb(200, 220, 255), "PARAM LO  [A05F7044] : %04X", param_lo);
        video_draw_debug_text(24, 172, rgb(200, 220, 255), "PARAM HI  [A05F7048] : %04X", param_hi);
        video_draw_debug_text(24, 196, rgb(200, 220, 255), "DIMM STAT [A05F704C] : %04X", status);

        if (bios_config != 0) {
            video_draw_debug_text(24, 252, rgb(0, 255, 0), "NETDIMM CONFIG PUBLISHED");
        } else {
            video_draw_debug_text(24, 252, rgb(255, 160, 80), "NETDIMM CONFIG NOT PUBLISHED");
        }
        video_draw_debug_text(24, 292, rgb(180, 180, 180), "Read-only probe: no DIMM command is sent.");
        video_display_on_vblank();
    }
}
