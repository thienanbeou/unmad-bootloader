#pragma once

// Same core settings as libhmk's include/tusb_config.h, which runs on this
// board: OTG_HS core (rhport 1) with its embedded full-speed PHY.
#define CFG_TUSB_OS OPT_OS_NONE
#define CFG_TUSB_DEBUG 0
#define CFG_TUSB_MEM_SECTION
#define CFG_TUSB_MEM_ALIGN __attribute__((aligned(4)))

#define CFG_TUD_ENABLED 1
#define CFG_TUD_ENDPOINT0_SIZE 64

#define CFG_TUD_HID 1
#define CFG_TUD_HID_EP_BUFSIZE 64

#define BOARD_TUD_RHPORT 1
