// Copyright (C) 2026 Piers Finlayson <piers@piers.rocks> (original host-control)
// NeoPixel slot indicator additions by Nathen (Doogie Bowser)
//
// MIT License
//
// RBCP (ROM Bus Control Protocol) device-side plugin for One ROM,
// with a NeoPixel slot-colour indicator driven from a bank-select (SEL) pad.
//
// On boot the LED is set off; thereafter each kernal switch sets the NeoPixel
// to a colour from a fixed palette, then control falls through to the standard
// RBCP loop.
//
// NeoPixel wiring (One ROM Fire 24 E):
//   DIN  -> a bank-select SEL pad. The SEL pads are free for output once the
//           firmware has read the image-select jumpers at boot. On this board
//           SEL0 = GPIO25 and SEL1 = GPIO24; this code drives BOTH (see below).
//   VDD  -> One ROM J1 (3.3V) for an SK6812, or 5V for a WS2812B
//   GND  -> One ROM J2 / bank-select GND pad (common ground required)
//
// Use an SK6812 powered from 3.3V - no level shifter required. For a WS2812B on
// 5V, add a 74AHCT125 level shifter on the data line (AHCT for the TTL input
// threshold). NOTE: this particular LED uses RGB byte order, not the usual GRB.
//
// KNOWN LIMITATION: the colour is bit-banged by the CPU and is corrupted when
// the C64 is actively hammering the ROM bus (bus contention stretches the
// pulse timing). It is correct with the host idle; under load colours come out
// hue-shifted/washed-out. A fully robust fix needs hardware-timed (PIO) output,
// which is not achievable from a plugin on this firmware. See the repo README
// for the full investigation.

#include <stdint.h>
#include <stdbool.h>
#include "plugin.h"
#include "flash_erase.h"

#define RP235X
#define MCU_FLASH_SIZE_KB 2048
#define MCU_RAM_SIZE_KB 520
#define RP2350A
#include "enums.h"
#include "reg-rp235x.h"

// ---------------------------------------------------------------------------
// Plugin header
// ---------------------------------------------------------------------------

ORA_DEFINE_USER_PLUGIN(
    rbcp_main,
    MAJOR_VERSION,
    MINOR_VERSION,
    PATCH_VERSION,
    BUILD_VERSION,
    0, 6, 9
);

// ---------------------------------------------------------------------------
// NeoPixel / GPIO definitions
// ---------------------------------------------------------------------------

// The NeoPixel data line is wired to a bank-select ("SEL") pad, which is free
// for use once the firmware has read the image-select jumpers at boot. On the
// Fire 24 E the SEL pads are GPIO25 (SEL0) and GPIO24 (SEL1). We were not able
// to pin down which physical pad the data wire lands on, so we drive BOTH and
// leave the unused one harmlessly toggling - both are < 32 so they share the
// low SIO bank and can be driven from one mask. If you confirm your pad, you
// can drop the other and keep a single pin.
#define NEOPIXEL_PIN        24u     // SEL1
#define NEOPIXEL_PIN_ALT    25u     // SEL0

#define FUNCSEL_SIO         5u

// WS2812/SK6812 pulse widths in nanoseconds.
// T0H is deliberately short and T1H clearly long to maximise the gap between
// a "0" and "1" high-time. Phase-locked bus contention from ROM serving can
// stretch a high pulse by tens of ns; the wide margin keeps a stretched "0"
// well below the ~half-way decision threshold so it doesn't read as a "1"
// (which showed up as washed-out / desaturated colours).
#define T0H_NS  250u
#define T0L_NS  800u
#define T1H_NS  800u
#define T1L_NS  450u
#define RST_US   60u

// ---------------------------------------------------------------------------
// Slot-to-colour palette
//
// IMPORTANT: indexed by FLASH slot (with ORA_FLASH_SLOT_FLAG_EXCLUDE_PLUGINS),
// NOT by RAM slot. The flash slot is the kernal's identity; the RAM slot is
// just whichever scratch buffer the bootloader loaded it into.
//
// Flash slot numbering (excluding plugins) on this setup:
//   0 = c64_bootloader  (LED off while bootloader ROM is being served)
//   1 = stock 901227-03 (red, per requirement)
//   2 = JiffyDOS
//   3 = JaffyDOS
//   4 = EXOS V3
//   5 = DesTestKernal
//   ... add more as you add slots; colours follow the palette order below
// ---------------------------------------------------------------------------

#define PALETTE_SIZE 22u
#define FLASH_SLOT_UNKNOWN 0xFFu

static const uint8_t s_palette[PALETTE_SIZE][3] = {
    {  0,   0,   0},   // Flash 0  - Bootloader: off
    {255,   0,   0},   // Flash 1  - Red (stock)
    {255, 128,   0},   // Flash 2  - Orange
    {255, 255,   0},   // Flash 3  - Yellow
    {  0, 255,   0},   // Flash 4  - Green
    {  0, 255, 255},   // Flash 5  - Cyan
    {  0,   0, 255},   // Flash 6  - Blue
    {128,   0, 255},   // Flash 7  - Purple
    {255, 255, 255},   // Flash 8  - White
    {255,   0, 128},   // Flash 9  - Pink
    {255,  64,   0},   // Flash 10 - Amber
    {  0, 128, 255},   // Flash 11 - Sky Blue
    {128, 255,   0},   // Flash 12 - Lime
    {255,   0, 255},   // Flash 13 - Magenta
    {  0, 255, 128},   // Flash 14 - Mint
    {255, 128, 128},   // Flash 15 - Salmon
    {128,   0,   0},   // Flash 16 - Maroon
    {  0,   0, 128},   // Flash 17 - Navy
    {128, 128,   0},   // Flash 18 - Olive
    {  0, 128,   0},   // Flash 19 - Dark Green
    {128,   0, 128},   // Flash 20 - Dark Purple
    {  0, 128, 128},   // Flash 21 - Teal
};

// Tracks which flash slot was last loaded into each RAM slot, so that a
// SWITCH_SLOT (which only names a RAM slot) can be translated back to a
// kernal identity. 8 entries covers the maximum RAM slot count for 64KB
// flash slot images (per the API docs, "up to 7 slots").
#define MAX_RAM_SLOTS 8u
static uint8_t s_ram_to_flash[MAX_RAM_SLOTS];

// ---------------------------------------------------------------------------
// NeoPixel driver - static state
// ---------------------------------------------------------------------------

static uint32_t s_np_pin_mask;
static uint32_t s_t0h, s_t0l, s_t1h, s_t1l, s_rst;

static inline void np_delay_cycles(uint32_t cycles) {
    uint32_t n = cycles / 3u;
    __asm volatile (
        "1: subs %0, %0, #1\n"
        "   bne 1b\n"
        : "+r"(n) :: "cc"
    );
}

// CRITICAL: the bit-bang must touch NO SRAM. Under host load the One ROM's
// ROM-serving DMA saturates the SRAM bus; any stack access (a function call)
// or static reload mid-pulse stalls the CPU and stretches the bit, scrambling
// the colour. (Confirmed: perfect with the host powered off, corrupt with it
// running.) So preload the pin mask and timings into locals (registers),
// inline the per-bit code, and use only register ops + SIO peripheral writes
// inside the loop - then the serving DMA can pound SRAM without stalling us.
//
// This LED uses RGB byte order, MSB first (not the usual WS2812 GRB).
__attribute__((noinline))
static void np_send_pixel(uint8_t r, uint8_t g, uint8_t b) {
    const uint32_t mask = s_np_pin_mask;
    const uint32_t t0h = s_t0h, t0l = s_t0l, t1h = s_t1h, t1l = s_t1l;
    uint32_t rgb = ((uint32_t)r << 16) | ((uint32_t)g << 8) | b;
    for (int i = 23; i >= 0; i--) {
        if ((rgb >> i) & 1u) {
            SIO_GPIO_OUT_SET = mask; np_delay_cycles(t1h);
            SIO_GPIO_OUT_CLR = mask; np_delay_cycles(t1l);
        } else {
            SIO_GPIO_OUT_SET = mask; np_delay_cycles(t0h);
            SIO_GPIO_OUT_CLR = mask; np_delay_cycles(t0l);
        }
    }
}

static void np_reset(void) {
    SIO_GPIO_OUT_CLR = s_np_pin_mask;
    np_delay_cycles(s_rst);
}

// ---------------------------------------------------------------------------
// NeoPixel init and slot colour display
// ---------------------------------------------------------------------------

static void neopixel_init(uint32_t sysclk_mhz) {
    s_t0h = (T0H_NS * sysclk_mhz) / 1000u;
    s_t0l = (T0L_NS * sysclk_mhz) / 1000u;
    s_t1h = (T1H_NS * sysclk_mhz) / 1000u;
    s_t1l = (T1L_NS * sysclk_mhz) / 1000u;
    s_rst = RST_US * sysclk_mhz;

    // Mask covers both candidate SEL pads (GPIO24 + GPIO25); see the pin
    // definitions above. Both are < 32 so they share the low SIO_GPIO_OUT_SET/
    // CLR bank and toggle together from a single mask.
    s_np_pin_mask = (1u << NEOPIXEL_PIN) | (1u << NEOPIXEL_PIN_ALT);

    // Configure both pads: SIO function, 8mA drive, fast slew. Driven as a
    // permanent output, idle LOW. A WS2812/SK6812 DIN must be held at a
    // defined level between updates - if left floating it drifts to the
    // 74AHCT125 input threshold (~1.5V), whose output oscillates and feeds
    // noise to the LED, causing flicker. Holding the pin low fixes that.
    GPIO_PAD(NEOPIXEL_PIN)     = PAD_DRIVE(PAD_DRIVE_8MA) | PAD_SLEW_FAST;
    GPIO_CTRL(NEOPIXEL_PIN)    = FUNCSEL_SIO;
    GPIO_PAD(NEOPIXEL_PIN_ALT) = PAD_DRIVE(PAD_DRIVE_8MA) | PAD_SLEW_FAST;
    GPIO_CTRL(NEOPIXEL_PIN_ALT) = FUNCSEL_SIO;
    SIO_GPIO_OUT_CLR = s_np_pin_mask;
    SIO_GPIO_OE_SET  = s_np_pin_mask;
}

static void neopixel_show_flash_slot(uint8_t flash_slot) {
    uint8_t r, g, b;
    if (flash_slot == FLASH_SLOT_UNKNOWN) {
        // RAM slot with no recorded flash origin - dim white so it's
        // visibly "something unexpected" rather than dark or a wrong colour
        r = 16; g = 16; b = 16;
    } else if (flash_slot < PALETTE_SIZE) {
        r = s_palette[flash_slot][0];
        g = s_palette[flash_slot][1];
        b = s_palette[flash_slot][2];
    } else {
        r = 16; g = 16; b = 16;
    }

    // The pin is already a permanent output (see neopixel_init); keep it
    // driven throughout and leave it idle LOW afterwards so the level
    // shifter input has a defined level and the NeoPixel holds its colour.
    //
    // The 24-bit transmission is cycle-timed and must NOT be interrupted -
    // we send the colour only once per slot switch, so a single stretched
    // bit (e.g. from a USB ISR in the system plugin) latches the wrong
    // colour permanently. Disable interrupts for the ~30us transmission so
    // the WS2812 timing is exact. The reset/latch pulses are just long LOW
    // periods and are interrupt-tolerant, so they stay outside the critical
    // section to keep IRQs off for as short a time as possible.
    SIO_GPIO_OUT_CLR = s_np_pin_mask;
    np_reset();
    // Disable this core's interrupts for the cycle-timed transmission so a USB
    // (or other) ISR can't stretch a bit. The register-only np_send_pixel (see
    // above) handles the cross-core DMA contention; together they give clean
    // timing under host load. The reset/latch pulses are interrupt-tolerant
    // long LOWs, so they stay outside the critical section.
    __asm volatile ("cpsid i" ::: "memory");   // IRQs off
    np_send_pixel(r, g, b);
    __asm volatile ("cpsie i" ::: "memory");   // IRQs on
    np_reset();                       // latch the colour
    SIO_GPIO_OUT_CLR = s_np_pin_mask; // idle low - hold the line stable
}

// Translate a RAM slot to its last-loaded flash slot and show that colour
static void neopixel_show_ram_slot(uint8_t ram_slot) {
    uint8_t flash = (ram_slot < MAX_RAM_SLOTS)
                    ? s_ram_to_flash[ram_slot]
                    : FLASH_SLOT_UNKNOWN;
    neopixel_show_flash_slot(flash);
}

// ---------------------------------------------------------------------------
// Everything below here is the original host-control RBCP plugin, unmodified
// ---------------------------------------------------------------------------

#define RING_ENTRIES_LOG2   6u
#define RING_DATA_SIZE      32u
#define RING_MASK           ((1u << RING_ENTRIES_LOG2) - 1u)
#define RING_BUF_TYPE       uint32_t
_Static_assert(sizeof(RING_BUF_TYPE) * 8 == RING_DATA_SIZE, "RING_BUF_TYPE must match RING_DATA_SIZE");

__attribute__((section(".ring_buf")))
ORA_RING_BUF_DECLARE_32BIT(ring_buf, RING_ENTRIES_LOG2);

#define RING_BUF_CUR_READ_INDEX()   s_read_idx
#define RING_BUF_ADV_READ_INDEX()   s_read_idx = (s_read_idx + 1u) & RING_MASK
#define RING_BUF_UPDATE_READ_INDEX(X) \
    s_read_idx = (uint32_t)((volatile RING_BUF_TYPE *)(X) - \
                            (volatile RING_BUF_TYPE *)ring_buf) & RING_MASK
#define RING_BUF_RESET_READ_INDEX() RING_BUF_UPDATE_READ_INDEX(*s_write_pos_ptr)
#define RING_BUF_CUR_WRITE_INDEX() \
    ((uint32_t)((volatile RING_BUF_TYPE *)*s_write_pos_ptr - \
                (volatile RING_BUF_TYPE *)ring_buf) & RING_MASK)
#define RING_BUF_GET_ENTRY(X) ((volatile RING_BUF_TYPE *)ring_buf)[(X)]

static volatile uint32_t * volatile *s_write_pos_ptr;
static uint32_t            s_read_idx;

#define KNOCK_LEN  6u
static const uint32_t s_knock_seq[KNOCK_LEN] = {
    '!', 'R', 'B', 'C', 'P', '!'
};

#define RBCP_PROTOCOL_VERSION_MAJOR 0u
#define RBCP_PROTOCOL_VERSION_MINOR 1u
#define RBCP_PROTOCOL_VERSION_PATCH 0u
const uint8_t protocol_version[4] = {
    RBCP_PROTOCOL_VERSION_MAJOR,
    RBCP_PROTOCOL_VERSION_MINOR,
    RBCP_PROTOCOL_VERSION_PATCH,
    0u
};

#define RBCP_DEFAULT_COMPLETE   ((uint8_t)0xBBu)
#define RBCP_DEFAULT_STATUS_OK  ((uint8_t)0xCCu)

#define HDR_LAST_CMD_GROUP  0u
#define HDR_LAST_CMD_CMD    1u
#define HDR_TOKEN_LSB       2u
#define HDR_TOKEN_MSB       3u
#define HDR_PROGRESS        4u
#define HDR_RESPONSE        5u
#define HDR_RESERVED_0      6u
#define HDR_RESERVED_1      7u
#define HDR_SIZE            8u

#define GRP_CONTROL     0x00u
#define GRP_READ        0x01u
#define GRP_MODIFY      0x02u
#define GRP_NV_STORAGE  0x03u
#define GRP_RESET       0xAAu

#define CMD_NOP                         0x00u
#define CMD_ENTER_CMD_RESP              0x01u
#define CMD_EXIT_CMD_RESP_ACK           0x02u
#define CMD_EXIT_CMD_RESP_SILENT        0x03u
#define CMD_SWITCH_AND_EXIT             0x04u

#define CMD_GET_FLASH_FLASH_SLOT_COUNT  0x00u
#define CMD_GET_FLASH_SLOT_INFO         0x01u
#define CMD_GET_FLASH_SLOT_INFO_ALL     0x02u
#define CMD_GET_RAM_SLOT_INFO_ALL       0x03u
#define CMD_GET_DEVICE_TYPE             0x04u
#define CMD_GET_DEVICE_VERSION          0x05u
#define CMD_GET_PROTOCOL_VERSION        0x06u
#define CMD_SLOT_PEEK                   0x07u

#define CMD_SLOT_POKE                   0x00u
#define CMD_SWITCH_SLOT                 0x01u
#define CMD_LOAD_SLOT                   0x02u
#define CMD_SLOT_POKE_ALL_BYTE          0x03u

#define CMD_GET_NV_CAPABILITY           0x00u
#define CMD_NV_PEEK                     0x01u
#define CMD_NV_POKE_BEGIN               0x02u
#define CMD_NV_POKE                     0x03u
#define CMD_NV_POKE_COMMIT              0x04u
#define CMD_NV_POKE_DISCARD             0x05u
#define CMD_NV_POKE_COMMIT_BYTE         0x06u

#define CMD_RBCP_RESET                  0xAAu

#define NV_STORAGE_SIZE 4096u
_Static_assert(NV_STORAGE_SIZE <= 32768u, "Max NV_STORAGE_SIZE is 32KB per the RBCP specification");

extern const uint8_t __nv_storage_start[];
extern const uint8_t __flash_erase_fn_start[];
extern const uint8_t __flash_erase_fn_end[];

typedef struct {
    uint16_t command_page;
    uint8_t  complete;
    uint8_t  status_ok;
    uint32_t region_offset;
    uint32_t region_end;
    uint32_t data_size;
} rbcp_cfg_t;

typedef struct {
    bool       active;
    rbcp_cfg_t cfg;
    uint8_t    token_lsb;
    uint8_t    token_msb;
    uint8_t    active_slot;
} rbcp_state_t;

typedef struct {
    bool     active;
    uint8_t  staging_slot;
    uint32_t staging_base;
    uint32_t staging_size;
} nv_state_t;

static rbcp_state_t s_state;
static nv_state_t s_nv_state;

static ora_lookup_fn_t                      s_lookup;
static ora_log_fn_t                         s_log;
static ora_demangle_addr_fn_t               s_demangle;
static ora_reprogram_ram_rom_slot_fn_t      s_reprogram;
static ora_get_ram_slot_info_fn_t           s_get_ram_slot_info;
static ora_get_ram_slot_count_fn_t          s_get_ram_slot_count;
static ora_get_active_ram_slot_fn_t         s_get_active_ram_slot;
static ora_set_active_ram_slot_fn_t         s_set_active_ram_slot;
static ora_get_flash_slot_count_fn_t        s_get_flash_slot_count;
static ora_get_flash_slot_info_fn_t         s_get_flash_slot_info;
static ora_copy_flash_slot_to_ram_slot_fn_t s_copy_flash_to_ram;
static ora_get_chip_size_from_type_fn_t     s_get_chip_size;
static ora_get_device_version_fn_t          s_get_device_version;
static ora_map_addr_to_phys_fn_t            s_map_addr_to_phys;
static ora_map_data_to_phys_fn_t            s_map_data_to_phys;
static ora_demangle_data_fn_t               s_demangle_data;

static uint8_t ring_read_byte(void) {
    for (;;) {
        if (RING_BUF_CUR_READ_INDEX() == RING_BUF_CUR_WRITE_INDEX()) continue;
        uint32_t phys = (uint32_t)RING_BUF_GET_ENTRY(s_read_idx);
        RING_BUF_ADV_READ_INDEX();
        uint32_t logical;
        if (s_demangle(phys, &logical, 1) == ORA_RESULT_OK) {
            if (s_state.active &&
                ((logical >> 8u) != (uint32_t)s_state.cfg.command_page)) {
                continue;
            }
            return (uint8_t)(logical & 0xFFu);
        }
    }
}

static inline uint8_t pending_val(void) { return (uint8_t)(~s_state.cfg.complete); }
static inline uint8_t failed_val(void)  { return (uint8_t)(~s_state.cfg.status_ok); }

static void hdr_write(uint8_t slot, uint32_t hdr_offset, uint8_t val, bool reset_ring) {
    uint32_t phys_addr = s_map_addr_to_phys(s_state.cfg.region_offset + hdr_offset);
    uint8_t phys_data = s_map_data_to_phys(val);
    uint32_t slot_base, slot_size;
    if (s_get_ram_slot_info(slot, &slot_base, &slot_size, NULL) != ORA_RESULT_OK) return;
    if (phys_addr >= slot_size) return;
    if (reset_ring) RING_BUF_RESET_READ_INDEX();
    ((volatile uint8_t *)slot_base)[phys_addr] = phys_data;
}

static ora_result_t hdr_read(uint8_t slot, uint32_t hdr_offset, uint8_t *val_out) {
    uint32_t slot_base, slot_size;
    if (s_get_ram_slot_info(slot, &slot_base, &slot_size, NULL) != ORA_RESULT_OK)
        return ORA_RESULT_INVALID_SLOT;
    uint32_t phys_offset = s_map_addr_to_phys(s_state.cfg.region_offset + hdr_offset);
    uint8_t raw = ((const uint8_t *)slot_base)[phys_offset];
    if (s_demangle_data(raw, val_out) != ORA_RESULT_OK) return ORA_RESULT_ERROR;
    return ORA_RESULT_OK;
}

static void data_write(uint8_t slot, uint32_t data_offset, const uint8_t *buf, uint32_t len) {
    if (data_offset >= s_state.cfg.data_size) return;
    if (data_offset + len > s_state.cfg.data_size)
        len = s_state.cfg.data_size - data_offset;
    s_reprogram(slot, s_state.cfg.region_offset + HDR_SIZE + data_offset, buf, len, 1u);
}

static void cmd_begin(uint8_t slot, uint8_t group, uint8_t cmd) {
    hdr_write(slot, HDR_PROGRESS, pending_val(), false);
    s_state.token_lsb++;
    if (s_state.token_lsb == 0u) s_state.token_msb++;
    hdr_write(slot, HDR_TOKEN_LSB, s_state.token_lsb, false);
    hdr_write(slot, HDR_TOKEN_MSB, s_state.token_msb, false);
    hdr_write(slot, HDR_LAST_CMD_GROUP, group, false);
    hdr_write(slot, HDR_LAST_CMD_CMD, cmd, false);
}

static void cmd_end(uint8_t slot, bool ok) {
    hdr_write(slot, HDR_RESPONSE, ok ? s_state.cfg.status_ok : failed_val(), false);
    hdr_write(slot, HDR_PROGRESS, s_state.cfg.complete, true);
}

static void init_back_channel(uint8_t slot) {
    static const uint8_t zeros[HDR_SIZE] = {0u};
    s_reprogram(slot, s_state.cfg.region_offset, zeros, HDR_SIZE, 1u);
}

static void zero_bytes(uint8_t *p, uint8_t n) {
    volatile uint8_t *vp = p;
    while (n--) *vp++ = 0u;
}

static void init_nv_state(void) {
    s_nv_state.active = false;
    s_nv_state.staging_slot = 0u;
    s_nv_state.staging_base = 0u;
    s_nv_state.staging_size = 0u;
}

static void init_rbcp(bool reset_slot_info) {
    s_state.active              = false;
    s_state.active_slot         = 0u;
    s_state.cfg.command_page    = 0u;
    s_state.cfg.complete        = RBCP_DEFAULT_COMPLETE;
    s_state.cfg.status_ok       = RBCP_DEFAULT_STATUS_OK;
    s_state.cfg.region_offset   = 0u;
    s_state.cfg.region_end      = 0u;
    s_state.cfg.data_size       = 0u;
    s_state.token_lsb           = 0u;
    s_state.token_msb           = 0u;
    s_read_idx                  = 0u;
    init_nv_state();
    if (reset_slot_info) s_state.active_slot = 0u;
}

static bool exec_nop(void) { return true; }

static bool exec_enter_cmd_resp(void) {
    uint8_t cp_lo     = ring_read_byte();
    uint8_t cp_hi     = ring_read_byte();
    uint8_t bc_a0     = ring_read_byte();
    uint8_t bc_a1     = ring_read_byte();
    uint8_t bc_a2     = ring_read_byte();
    uint8_t bc_sz_lo  = ring_read_byte();
    uint8_t bc_sz_hi  = ring_read_byte();
    uint8_t complete  = ring_read_byte();
    uint8_t status_ok = ring_read_byte();

    uint16_t command_page  = (uint16_t)cp_lo | ((uint16_t)cp_hi << 8u);
    uint32_t region_offset = (uint32_t)bc_a0 | ((uint32_t)bc_a1 << 8u) | ((uint32_t)bc_a2 << 16u);
    uint16_t region_size   = (uint16_t)bc_sz_lo | ((uint16_t)bc_sz_hi << 8u);

    if (s_state.active) return false;
    if (complete == 0xAAu || status_ok == 0xAAu) return false;
    if (region_offset & 0x3u) return false;
    if ((uint32_t)region_size < HDR_SIZE) return false;

    uint8_t active_slot;
    if (s_get_active_ram_slot(&active_slot) != ORA_RESULT_OK) return false;
    uint32_t slot_size;
    if (s_get_ram_slot_info(active_slot, NULL, &slot_size, NULL) != ORA_RESULT_OK) return false;
    if (((uint32_t)command_page << 8u) >= slot_size) return false;
    uint32_t region_end = region_offset + (uint32_t)region_size;
    if (region_end > slot_size) return false;

    s_state.cfg.region_offset = region_offset;
    if (hdr_read(active_slot, HDR_TOKEN_LSB, &s_state.token_lsb) != ORA_RESULT_OK ||
        hdr_read(active_slot, HDR_TOKEN_MSB, &s_state.token_msb) != ORA_RESULT_OK)
        return false;

    s_state.cfg.command_page  = command_page;
    s_state.cfg.complete      = complete;
    s_state.cfg.status_ok     = status_ok;
    s_state.cfg.region_end    = region_end;
    s_state.cfg.data_size     = (uint32_t)region_size - HDR_SIZE;
    s_state.active_slot       = active_slot;
    init_back_channel(active_slot);
    s_state.active = true;
    return true;
}

static bool exec_get_flash_slot_count(void) {
    uint8_t count = s_get_flash_slot_count(ORA_FLASH_SLOT_FLAG_EXCLUDE_PLUGINS);
    uint8_t resp[1] = { count };
    data_write(s_state.active_slot, 0u, resp, 1u);
    return true;
}

static bool get_flash_slot_info(uint8_t flash_slot, uint8_t record[32]) {
    const char *name = NULL;
    uint32_t rom_type = 0xFFu;
    if (s_get_flash_slot_info(flash_slot, ORA_FLASH_SLOT_FLAG_EXCLUDE_PLUGINS,
                              &name, &rom_type, NULL) != ORA_RESULT_OK) return false;
    record[0] = (uint8_t)(rom_type & 0xFFu);
    zero_bytes(&record[1], 31u);
    if (name != NULL) {
        uint8_t nlen = 0u;
        while (nlen < 30u && name[nlen] != '\0') nlen++;
        for (uint8_t j = 0u; j < nlen; j++) record[1u + j] = (uint8_t)name[j];
    }
    return true;
}

static bool exec_get_flash_slot_info(uint8_t ram_slot) {
    uint8_t flash_slot = ring_read_byte();
    if (flash_slot == 0xAA) return false;
    if (s_state.cfg.data_size < 32u) return false;
    uint8_t record[32];
    if (!get_flash_slot_info(flash_slot, record)) return false;
    data_write(ram_slot, 0, record, 32u);
    return true;
}

static bool exec_get_flash_slot_info_all(uint8_t slot) {
    uint8_t total = s_get_flash_slot_count(ORA_FLASH_SLOT_FLAG_EXCLUDE_PLUGINS);
    if (s_state.cfg.data_size < 4u) {
        uint8_t preamble[4] = { total, 0u, 0u, 0u };
        data_write(slot, 0u, preamble, s_state.cfg.data_size);
        return true;
    }
    uint32_t space       = s_state.cfg.data_size - 4u;
    uint8_t  whole_count = (uint8_t)(space / 32u);
    if (whole_count > total) whole_count = total;
    uint32_t partial_bytes = space - ((uint32_t)whole_count * 32u);
    uint8_t  partial_flag  = (whole_count < total && partial_bytes > 0u) ? 0x01u : 0x00u;
    uint8_t preamble[4] = { total, whole_count, partial_flag, 0u };
    data_write(slot, 0u, preamble, 4u);
    uint32_t data_off      = 4u;
    uint8_t  slots_to_emit = whole_count + (partial_flag ? 1u : 0u);
    for (uint8_t i = 0u; i < slots_to_emit; i++) {
        uint8_t record[32];
        if (!get_flash_slot_info(i, record)) return false;
        uint32_t bytes = (i < whole_count) ? 32u : partial_bytes;
        if (i == whole_count) record[partial_bytes - 1] = 0x00u;
        data_write(slot, data_off, record, bytes);
        data_off += bytes;
    }
    return true;
}

static bool exec_get_ram_slot_info_all(uint8_t slot) {
    uint8_t  total    = s_get_ram_slot_count();
    uint32_t rom_type = 0xFFu;
    s_get_ram_slot_info(slot, NULL, NULL, &rom_type);
    uint8_t resp[4] = { total, slot, (uint8_t)(rom_type & 0xFFu), 0u };
    data_write(slot, 0u, resp, 4u);
    return true;
}

static const char device_type_str[] = "One ROM";
static bool exec_get_device_type(void) {
    uint8_t device_type[24];
    zero_bytes((uint8_t *)device_type, sizeof(device_type));
    for (size_t i = 0; i < sizeof(device_type_str) && i < sizeof(device_type); i++)
        device_type[i] = device_type_str[i];
    data_write(s_state.active_slot, 0u, device_type, sizeof(device_type));
    return true;
}

static bool exec_get_device_version(void) {
    uint8_t device_version[24];
    zero_bytes(device_version, sizeof(device_version));
    if (s_get_device_version(device_version, sizeof(device_version)) != ORA_RESULT_OK) return false;
    data_write(s_state.active_slot, 0u, device_version, sizeof(device_version));
    return true;
}

static bool exec_get_protocol_version(void) {
    data_write(s_state.active_slot, 0u, protocol_version, sizeof(protocol_version));
    return true;
}

static bool exec_slot_peek(void) {
    uint8_t count  = ring_read_byte();
    uint8_t a0     = ring_read_byte();
    uint8_t a1     = ring_read_byte();
    uint8_t a2     = ring_read_byte();
    uint8_t target = ring_read_byte();
    if (target == 0xAAu) return false;
    uint32_t addr       = (uint32_t)a0 | ((uint32_t)a1 << 8u) | ((uint32_t)a2 << 16u);
    uint32_t byte_count = (count == 0u) ? 256u : (uint32_t)count;
    if (byte_count > s_state.cfg.data_size) return false;
    uint32_t slot_base, slot_size;
    if (s_get_ram_slot_info(target, &slot_base, &slot_size, NULL) != ORA_RESULT_OK) return false;
    if (addr + byte_count > slot_size) return false;
#define SLOT_PEEK_BUF_SIZE 32u
    uint8_t  buf[SLOT_PEEK_BUF_SIZE];
    uint32_t remaining = byte_count, data_off = 0u;
    while (remaining > 0u) {
        uint32_t chunk = (remaining > SLOT_PEEK_BUF_SIZE) ? SLOT_PEEK_BUF_SIZE : remaining;
        for (uint32_t i = 0u; i < chunk; i++) {
            uint32_t phys_offset = s_map_addr_to_phys(addr + data_off + i);
            uint8_t  raw         = ((const uint8_t *)slot_base)[phys_offset];
            if (s_demangle_data(raw, &buf[i]) != ORA_RESULT_OK) return false;
        }
        data_write(s_state.active_slot, data_off, buf, chunk);
        data_off += chunk;
        remaining -= chunk;
    }
    return true;
}

static bool exec_slot_poke(void) {
    uint8_t byte   = ring_read_byte();
    uint8_t a0     = ring_read_byte();
    uint8_t a1     = ring_read_byte();
    uint8_t a2     = ring_read_byte();
    uint8_t target = ring_read_byte();
    if (target == 0xAAu) return false;
    uint32_t addr = (uint32_t)a0 | ((uint32_t)a1 << 8u) | ((uint32_t)a2 << 16u);
    return (s_reprogram(target, addr, &byte, 1u, 1u) == ORA_RESULT_OK);
}

static bool exec_switch_slot(void) {
    uint8_t target = ring_read_byte();
    if (target == 0xAAu) return false;

    ora_result_t rc = s_set_active_ram_slot(target);
    if (rc == ORA_RESULT_OK) {
        // Show the colour of the kernal that was loaded into this RAM slot
        neopixel_show_ram_slot(target);
    }
    return (rc == ORA_RESULT_OK);
}

static bool exec_load_slot(void) {
    uint8_t ram_slot   = ring_read_byte();
    uint8_t flash_slot = ring_read_byte();
    if ((ram_slot == 0xAAu) || (flash_slot == 0xAAu)) return false;
    bool ok = (s_copy_flash_to_ram(flash_slot, ORA_FLASH_SLOT_FLAG_EXCLUDE_PLUGINS,
                                   ram_slot, 0u) == ORA_RESULT_OK);
    if (ok && ram_slot < MAX_RAM_SLOTS) {
        // Remember which kernal now lives in this RAM slot, so a later
        // SWITCH_SLOT can be translated back to a colour
        s_ram_to_flash[ram_slot] = flash_slot;
    }
    return ok;
}

static bool exec_slot_poke_all_byte(void) {
    uint8_t byte   = ring_read_byte();
    uint8_t target = ring_read_byte();
    if (target == 0xAAu) return false;
    uint32_t rom_type = 0xFFu;
    if (s_get_ram_slot_info(target, NULL, NULL, &rom_type) != ORA_RESULT_OK) return false;
    uint32_t chip_size = s_get_chip_size(rom_type);
    if (chip_size == 0u) return false;
    for (uint32_t i = 0u; i < chip_size; i++) {
        if (s_reprogram(target, i, &byte, 1u, 1u) != ORA_RESULT_OK) return false;
    }
    return true;
}

#define NV_ROM_TABLE_LOOKUP_ADDR    0x00000016u
#define NV_ROM_TABLE_FLAG_ARM_SEC   0x0004u
#define XIP_QMI_BASE        0x400d0000
#define XIP_QMI_M0_TIMING   (*((volatile uint32_t *)(XIP_QMI_BASE + 0x0C)))
#define XIP_QMI_M0_CLKDIV_MASK   0xFF
#define XIP_QMI_M0_CLKDIV_SHIFT  0
#define RP2350_FLASH_BASE   0x10000000u

static void *nv_lookup_boot_fn(char a, char b) {
    typedef void *(*rom_table_lookup_fn)(uint32_t code, uint32_t mask);
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Warray-bounds"
    rom_table_lookup_fn rom_table_lookup =
        (rom_table_lookup_fn)(uintptr_t)*(uint16_t *)(NV_ROM_TABLE_LOOKUP_ADDR);
#pragma GCC diagnostic pop
    uint32_t code = ((uint32_t)(uint8_t)b << 8) | (uint32_t)(uint8_t)a;
    return rom_table_lookup(code, NV_ROM_TABLE_FLAG_ARM_SEC);
}

static void nv_discard_impl(void) { init_nv_state(); }

static bool exec_get_nv_capability(void) {
    bool writable = (s_get_ram_slot_count() > 1u);
    uint8_t resp[4] = {
        (uint8_t)(NV_STORAGE_SIZE & 0xFFu),
        (uint8_t)((NV_STORAGE_SIZE >> 8u) & 0xFFu),
        writable ? 0x01u : 0x00u,
        0x00u
    };
    data_write(s_state.active_slot, 0u, resp, 4u);
    return true;
}

static bool exec_nv_peek(void) {
    uint8_t count   = ring_read_byte();
    uint8_t loc_lsb = ring_read_byte();
    uint8_t loc_msb = ring_read_byte();
    if (loc_msb > 0x7Fu) return false;
    uint32_t location   = (uint32_t)loc_lsb | ((uint32_t)loc_msb << 8u);
    uint32_t byte_count = (count == 0u) ? 256u : (uint32_t)count;
    if (location + byte_count > NV_STORAGE_SIZE) return false;
    if (byte_count > s_state.cfg.data_size) return false;
    data_write(s_state.active_slot, 0u, &__nv_storage_start[location], byte_count);
    return true;
}

static bool nv_poke_begin_impl(uint8_t slot) {
    if (s_nv_state.active) return false;
    if (slot == s_state.active_slot) return false;
    uint32_t slot_base, slot_size;
    if (s_get_ram_slot_info(slot, &slot_base, &slot_size, NULL) != ORA_RESULT_OK) return false;
    uint32_t erase_fn_size = (uint32_t)(__flash_erase_fn_end - __flash_erase_fn_start);
    uint32_t required      = NV_STORAGE_SIZE + erase_fn_size;
    if (slot_size < required) return false;
    volatile uint8_t *staging = (volatile uint8_t *)slot_base;
    for (uint32_t i = 0u; i < NV_STORAGE_SIZE; i++) staging[i] = __nv_storage_start[i];
    volatile uint8_t *erase_dest = staging + NV_STORAGE_SIZE;
    for (uint32_t i = 0u; i < erase_fn_size; i++) erase_dest[i] = __flash_erase_fn_start[i];
    s_nv_state.active       = true;
    s_nv_state.staging_slot = slot;
    s_nv_state.staging_base = slot_base;
    s_nv_state.staging_size = slot_size;
    return true;
}

static bool exec_nv_poke_begin(void) {
    uint8_t slot = ring_read_byte();
    if (slot == 0xAAu) return false;
    return nv_poke_begin_impl(slot);
}

static bool nv_poke_impl(uint8_t byte, uint8_t loc_lsb, uint8_t loc_msb) {
    if (!s_nv_state.active) return false;
    if (loc_msb > 0x7Fu) return false;
    uint32_t location = (uint32_t)loc_lsb | ((uint32_t)loc_msb << 8u);
    if (location >= NV_STORAGE_SIZE) return false;
    ((volatile uint8_t *)s_nv_state.staging_base)[location] = byte;
    return true;
}

static bool exec_nv_poke(void) {
    uint8_t byte    = ring_read_byte();
    uint8_t loc_lsb = ring_read_byte();
    uint8_t loc_msb = ring_read_byte();
    return nv_poke_impl(byte, loc_lsb, loc_msb);
}

static bool exec_nv_poke_discard(void) {
    if (!s_nv_state.active) return false;
    nv_discard_impl();
    return true;
}

static bool exec_nv_poke_commit(void) {
    if (!s_nv_state.active) return false;
    connect_internal_flash_fn_t connect_internal_flash = (connect_internal_flash_fn_t)nv_lookup_boot_fn('I', 'F');
    if (!connect_internal_flash) return false;
    flash_exit_xip_fn_t flash_exit_xip = (flash_exit_xip_fn_t)nv_lookup_boot_fn('E', 'X');
    if (!flash_exit_xip) return false;
    flash_range_erase_fn_t flash_range_erase = (flash_range_erase_fn_t)nv_lookup_boot_fn('R', 'E');
    if (!flash_range_erase) return false;
    flash_flush_cache_fn_t flash_flush_cache = (flash_flush_cache_fn_t)nv_lookup_boot_fn('F', 'C');
    if (!flash_flush_cache) return false;
    flash_select_xip_read_mode_fn_t flash_select_xip_read_mode = (flash_select_xip_read_mode_fn_t)nv_lookup_boot_fn('X', 'M');
    if (!flash_select_xip_read_mode) return false;
    flash_range_program_fn_t flash_range_program = (flash_range_program_fn_t)nv_lookup_boot_fn('R', 'P');
    if (!flash_range_program) return false;
    ora_enter_exclusive_mode_fn_t enter_exclusive = s_lookup(ORA_ID_ENTER_EXCLUSIVE_MODE);
    ora_exit_exclusive_mode_fn_t exit_exclusive   = s_lookup(ORA_ID_EXIT_EXCLUSIVE_MODE);
    if (enter_exclusive() != ORA_RESULT_OK) return false;
    connect_internal_flash();
    uint8_t  clkdiv    = (uint8_t)((XIP_QMI_M0_TIMING >> XIP_QMI_M0_CLKDIV_SHIFT) & XIP_QMI_M0_CLKDIV_MASK);
    uint32_t flash_offs = (uint32_t)__nv_storage_start - RP2350_FLASH_BASE;
    nv_flash_erase_critical_fn_t erase_fn =
        (nv_flash_erase_critical_fn_t)((s_nv_state.staging_base + NV_STORAGE_SIZE) | 1u);
    erase_fn(flash_exit_xip, flash_range_erase, flash_flush_cache,
             flash_select_xip_read_mode, flash_offs, NV_STORAGE_SIZE, clkdiv);
    flash_range_program(flash_offs, (const uint8_t *)s_nv_state.staging_base, NV_STORAGE_SIZE);
    exit_exclusive();
    nv_discard_impl();
    return true;
}

static bool exec_nv_poke_commit_byte(void) {
    uint8_t byte    = ring_read_byte();
    uint8_t loc_lsb = ring_read_byte();
    uint8_t loc_msb = ring_read_byte();
    uint8_t slot    = ring_read_byte();
    if (slot == 0xAAu) return false;
    uint32_t location = (uint32_t)loc_lsb | ((uint32_t)loc_msb << 8u);
    if (loc_msb <= 0x7Fu && location < NV_STORAGE_SIZE &&
        __nv_storage_start[location] == byte) return true;
    if (!nv_poke_begin_impl(slot)) return false;
    if (!nv_poke_impl(byte, loc_lsb, loc_msb)) { nv_discard_impl(); return false; }
    return exec_nv_poke_commit();
}

static bool dispatch(uint8_t group, uint8_t cmd, bool *exit_silent_out) {
    *exit_silent_out = false;
    bool ok = false;
    switch (group) {
        case GRP_CONTROL:
            switch (cmd) {
                case CMD_NOP:                  ok = exec_nop(); break;
                case CMD_ENTER_CMD_RESP:       ok = exec_enter_cmd_resp(); break;
                case CMD_EXIT_CMD_RESP_ACK:    s_state.active = false; ok = true; break;
                case CMD_EXIT_CMD_RESP_SILENT: s_state.active = false; *exit_silent_out = true; ok = true; break;
                case CMD_SWITCH_AND_EXIT:
                    ok = exec_switch_slot();
                    s_state.active = false;
                    *exit_silent_out = true;
                    break;
                default: ok = false; break;
            }
            break;
        case GRP_READ:
            switch (cmd) {
                case CMD_GET_FLASH_FLASH_SLOT_COUNT: ok = exec_get_flash_slot_count(); break;
                case CMD_GET_FLASH_SLOT_INFO:        ok = exec_get_flash_slot_info(s_state.active_slot); break;
                case CMD_GET_FLASH_SLOT_INFO_ALL:    ok = exec_get_flash_slot_info_all(s_state.active_slot); break;
                case CMD_GET_RAM_SLOT_INFO_ALL:      ok = exec_get_ram_slot_info_all(s_state.active_slot); break;
                case CMD_GET_DEVICE_TYPE:            ok = exec_get_device_type(); break;
                case CMD_GET_DEVICE_VERSION:         ok = exec_get_device_version(); break;
                case CMD_GET_PROTOCOL_VERSION:       ok = exec_get_protocol_version(); break;
                case CMD_SLOT_PEEK:                  ok = exec_slot_peek(); break;
                default: ok = false; break;
            }
            break;
        case GRP_MODIFY:
            switch (cmd) {
                case CMD_SLOT_POKE:          ok = exec_slot_poke(); break;
                case CMD_SWITCH_SLOT:
                    ok = exec_switch_slot();
                    if (ok) {
                        uint8_t new_slot;
                        if (s_get_active_ram_slot(&new_slot) == ORA_RESULT_OK)
                            s_state.active_slot = new_slot;
                    }
                    break;
                case CMD_LOAD_SLOT:          ok = exec_load_slot(); break;
                case CMD_SLOT_POKE_ALL_BYTE: ok = exec_slot_poke_all_byte(); break;
                default: ok = false; break;
            }
            break;
        case GRP_NV_STORAGE:
            if (!s_state.active) { ok = false; break; }
            switch (cmd) {
                case CMD_GET_NV_CAPABILITY:    ok = exec_get_nv_capability(); break;
                case CMD_NV_PEEK:              ok = exec_nv_peek(); break;
                case CMD_NV_POKE_BEGIN:        ok = exec_nv_poke_begin(); break;
                case CMD_NV_POKE:              ok = exec_nv_poke(); break;
                case CMD_NV_POKE_COMMIT:       ok = exec_nv_poke_commit(); break;
                case CMD_NV_POKE_DISCARD:      ok = exec_nv_poke_discard(); break;
                case CMD_NV_POKE_COMMIT_BYTE:  ok = exec_nv_poke_commit_byte(); break;
                default: ok = false; break;
            }
            break;
        case GRP_RESET:
            switch (cmd) {
                case CMD_RBCP_RESET:
                    init_rbcp(false);
                    s_state.active = false;
                    *exit_silent_out = true;
                    ok = true;
                    break;
                default: ok = false; break;
            }
            break;
        default: ok = false; break;
    }
    return ok;
}

static bool run_command(uint8_t group, uint8_t cmd) {
    bool was_active = s_state.active;
    bool exit_silent;
    if (was_active) cmd_begin(s_state.active_slot, group, cmd);
    bool ok = dispatch(group, cmd, &exit_silent);
    bool now_active = s_state.active;
    if (was_active && !exit_silent) {
        cmd_end(s_state.active_slot, ok);
    } else if (!was_active && now_active) {
        cmd_begin(s_state.active_slot, group, cmd);
        cmd_end(s_state.active_slot, ok);
    }
    if (was_active && !now_active) nv_discard_impl();
    return now_active;
}

__attribute__((noinline)) static void rbcp_setup(
    ora_lookup_fn_t ora_lookup_fn,
    ora_knock_t *knock
) {
    s_lookup               = ora_lookup_fn;
    s_log                  = ora_lookup_fn(ORA_ID_LOG);
    s_demangle             = ora_lookup_fn(ORA_ID_DEMANGLE_ADDR);
    s_reprogram            = ora_lookup_fn(ORA_ID_REPROGRAM_RAM_ROM_SLOT);
    s_get_ram_slot_info    = ora_lookup_fn(ORA_ID_GET_RAM_SLOT_INFO);
    s_get_ram_slot_count   = ora_lookup_fn(ORA_ID_GET_RAM_SLOT_COUNT);
    s_get_active_ram_slot  = ora_lookup_fn(ORA_ID_GET_ACTIVE_RAM_SLOT);
    s_set_active_ram_slot  = ora_lookup_fn(ORA_ID_SET_ACTIVE_RAM_SLOT);
    s_get_flash_slot_count = ora_lookup_fn(ORA_ID_GET_FLASH_SLOT_COUNT);
    s_get_flash_slot_info  = ora_lookup_fn(ORA_ID_GET_FLASH_SLOT_INFO);
    s_copy_flash_to_ram    = ora_lookup_fn(ORA_ID_COPY_FLASH_SLOT_TO_RAM_SLOT);
    s_get_chip_size        = ora_lookup_fn(ORA_ID_GET_CHIP_SIZE_FROM_TYPE);
    s_get_device_version   = ora_lookup_fn(ORA_ID_GET_DEVICE_VERSION);
    s_map_addr_to_phys     = ora_lookup_fn(ORA_ID_MAP_ADDR_TO_PHYS);
    s_map_data_to_phys     = ora_lookup_fn(ORA_ID_MAP_DATA_TO_PHYS);
    s_demangle_data        = ora_lookup_fn(ORA_ID_DEMANGLE_DATA);

    ora_start_address_monitor_fn_t start_address_monitor =
        ora_lookup_fn(ORA_ID_START_ADDRESS_MONITOR);
    ora_init_knock_fn_t init_knock =
        ora_lookup_fn(ORA_ID_INIT_KNOCK);
    ora_setup_address_monitor_fn_t setup_address_monitor =
        ora_lookup_fn(ORA_ID_SETUP_ADDRESS_MONITOR);
    ora_get_address_monitor_ring_write_pos_fn_t get_write_pos =
        ora_lookup_fn(ORA_ID_GET_ADDRESS_MONITOR_RING_WRITE_POS);

    s_log("RBCP+NeoPixel plugin starting");

    init_rbcp(true);

    ora_result_t rc = setup_address_monitor(
        ring_buf, RING_ENTRIES_LOG2,
        ORA_MONITOR_MODE_CONTROL,
        RING_DATA_SIZE, NULL
    );
    if (rc != ORA_RESULT_OK) {
        s_log("RBCP: address monitor setup failed %d", rc);
        return;
    }

    s_write_pos_ptr = get_write_pos();
    if (s_write_pos_ptr == NULL) {
        s_log("RBCP: failed to get ring buffer write position");
        return;
    }

    if (init_knock(s_knock_seq, KNOCK_LEN, 8u, RING_DATA_SIZE, knock) != ORA_RESULT_OK) {
        s_log("RBCP: knock init failed");
        return;
    }

    start_address_monitor();
}

// ---------------------------------------------------------------------------
// Plugin entry point
// ---------------------------------------------------------------------------

void rbcp_main(
    ora_lookup_fn_t         ora_lookup_fn,
    ora_plugin_type_t       plugin_type,
    const ora_entry_args_t *entry_args
) {
    (void)plugin_type;
    (void)entry_args;

    // --- NeoPixel init ---
    ora_get_sysclk_mhz_fn_t get_sysclk = ora_lookup_fn(ORA_ID_GET_SYSCLK_MHZ);
    uint32_t mhz = get_sysclk();
    neopixel_init(mhz);

    // Initialise the RAM-to-flash mapping. RAM slot 0 is pre-populated by the
    // firmware from the first non-plugin flash slot (the bootloader), so map
    // it to flash slot 0. Everything else is unknown until LOAD_SLOT runs.
    for (uint32_t i = 0u; i < MAX_RAM_SLOTS; i++) {
        s_ram_to_flash[i] = FLASH_SLOT_UNKNOWN;
    }
    s_ram_to_flash[0] = 0u;

    // At boot the bootloader ROM is being served: LED off (palette entry 0).
    // The bootloader will shortly LOAD/SWITCH to the saved kernal, which
    // updates the LED via the RBCP command hooks.
    neopixel_show_flash_slot(0u);
    // --- NeoPixel init complete ---

    ORA_KNOCK_DECLARE(knock, KNOCK_LEN);
    rbcp_setup(ora_lookup_fn, knock);

    ora_wait_for_knock_fn_t wait_for_knock = ora_lookup_fn(ORA_ID_WAIT_FOR_KNOCK);

    for (;;) {
        volatile uint32_t *next_read;
        uint32_t preamble[2];
        if (wait_for_knock(knock, ring_buf, RING_ENTRIES_LOG2,
                           ORA_WAIT_FOR_KNOCK_FLAG_DEBOUNCE_CS,
                           preamble, 2u, NULL, &next_read) != ORA_RESULT_OK) {
            continue;
        }

        RING_BUF_UPDATE_READ_INDEX(next_read);

        uint32_t logical;
        if (s_demangle(preamble[0], &logical, 0) != ORA_RESULT_OK) continue;
        uint8_t group = (uint8_t)(logical & 0xFFu);

        if (s_demangle(preamble[1], &logical, 0) != ORA_RESULT_OK) continue;
        uint8_t cmd = (uint8_t)(logical & 0xFFu);

        while (run_command(group, cmd)) {
            group = ring_read_byte();
            cmd   = ring_read_byte();
        }
    }
}
