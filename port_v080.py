"""Apply the RGB extension to an otherwise unmodified One ROM v0.8.0 tree."""
from pathlib import Path
import re

here = Path(__file__).resolve().parent
tree = here.parent / 'one-rom-v0.8.0'
target = tree / 'plugins/user/host-control/src/host_control_main.c'
import subprocess
source = subprocess.check_output(
    ['git', '-C', str(tree), 'show', 'v0.8.0:plugins/user/host-control/src/host_control_main.c'],
    text=True,
)
old = (here / 'host_control_rgb_main.c').read_text()
driver = old[old.index('#define NEOPIXEL_PIN '):old.index('// Everything below here')]
# Keep JiffyDOS orange; give JaffyDOS a clearly different hue.
jaffy_colour = '    {255, 255,   0},   // Flash 3  - Yellow'
assert driver.count(jaffy_colour) == 1
driver = driver.replace(jaffy_colour, '    {255,   0, 255},   // Flash 3  - JaffyDOS: magenta')
# Replace firmware-private register headers with the six RP2350 registers used.
registers = '''
// RP2350 SIO and GPIO registers. No PIO/DMA registers are accessed.
#define SIO_GPIO_OUT_SET (*(volatile uint32_t *)0xd0000018u)
#define SIO_GPIO_OUT_CLR (*(volatile uint32_t *)0xd0000020u)
#define SIO_GPIO_OE_SET  (*(volatile uint32_t *)0xd0000038u)
#define GPIO_CTRL(pin) (*(volatile uint32_t *)(0x40028004u + (pin) * 8u))
#define GPIO_PAD(pin)  (*(volatile uint32_t *)(0x40038004u + (pin) * 4u))
#define PAD_DRIVE_8MA 2u
#define PAD_DRIVE(value) ((value) << 4u)
#define PAD_SLEW_FAST 1u
'''
driver = re.sub(r'// Tracks which flash slot.*?static uint8_t s_ram_to_flash\[MAX_RAM_SLOTS\];', '''// C64 2364 only: 8KB per slot, at most 64 fit in main SRAM.
#define MAX_RAM_SLOTS 64u
static uint8_t s_ram_to_flash[MAX_RAM_SLOTS];''', driver, flags=re.S)
driver = driver.replace('noinline, aligned(4)', 'noinline, aligned(4), optimize("Os")')
driver = driver.replace('static uint32_t s_t0h, s_t0l, s_t1h, s_t1l, s_rst;', 'static uint16_t s_t0h, s_t0l, s_t1h, s_t1l, s_rst;')
driver = driver.replace('static uint32_t s_np_pin_mask;', '#define s_np_pin_mask ((1u << NEOPIXEL_PIN) | (1u << NEOPIXEL_PIN_ALT))')
driver = driver.replace('    s_np_pin_mask = (1u << NEOPIXEL_PIN) | (1u << NEOPIXEL_PIN_ALT);', '')
driver = driver.replace('s_t0h, s_t0l, s_t1h, s_t1l, s_rst;', 's_t0h, s_t0l, s_t1l, s_rst;\n#define s_t1h s_t0l\n_Static_assert(T1H_NS == T0L_NS, "Shared timing requires equal intervals");')
driver = driver.replace('    s_t1h = (T1H_NS * sysclk_mhz) / 1000u;\n', '')
driver = re.sub(r'// The RBCP address monitor runs.*?#define MONITOR_SM_MASK[^\n]*\n', '', driver, flags=re.S)
driver = re.sub(r'static void plugin_copy_ramfuncs\(void\) \{.*?\n\}\n', '', driver, flags=re.S)
start = driver.index('    // The pin is already a permanent output', driver.index('static void neopixel_show_rgb'))
end = driver.index('    np_reset();                       // latch', start)
driver = driver[:start] + '''    // v0.8.0 dynamically shares monitor and ROM-serving PIO blocks.
    // Never stop hard-coded SMs. Run the sender from SRAM, as in the working
    // plugin, and leave the firmware's interrupts and peripherals alone.
    SIO_GPIO_OUT_CLR = s_np_pin_mask;
    np_reset();
    np_send_pixel(r, g, b);
''' + driver[end:]
# The source of truth for .ramfunc startup is now upstream init_data_bss().
driver = driver.rsplit('// ---------------------------------------------------------------------------', 1)[0]
source = source.replace('#include "flash_erase.h"', '#include "flash_erase.h"\n' + registers + driver)
source = source.replace('#define RING_ENTRIES_LOG2   6u', '#define RING_ENTRIES_LOG2   4u')
source = source.replace('// 64 entries', '// 16 entries; RAM retained for RGB sender and slot tracking')
source = source.replace('.min_fw_minor_version = 7,', '.min_fw_minor_version = 8,')
source = source.replace('.min_fw_patch_version = 1,', '.min_fw_patch_version = 0,')
source = source.replace('return (s_set_active_ram_slot(target) == ORA_RESULT_OK);', '''bool ok = (s_set_active_ram_slot(target) == ORA_RESULT_OK);
    if (ok) neopixel_show_ram_slot(target);
    return ok;''')
needle = '    return true;\n}\n\nstatic bool exec_load_slot(void)'
assert needle in source
source = source.replace(needle, '''    if (ram_slot < MAX_RAM_SLOTS) s_ram_to_flash[ram_slot] = flash_slot;
    return true;
}

static bool exec_load_slot(void)''', 1)
# A reload of the currently served image also changes the indicated identity.
source = source.replace('bool ok = load_slot_impl("LOAD_AND_EXIT");', '''bool ok = load_slot_impl("LOAD_AND_EXIT");
    uint8_t active;
    if (ok && s_get_active_ram_slot(&active) == ORA_RESULT_OK)
        neopixel_show_ram_slot(active);''')
needle = '    rbcp_setup(ora_lookup_fn, knock);'
source = source.replace(needle, '''    ora_get_sysclk_mhz_fn_t get_sysclk = ora_lookup_fn(ORA_ID_GET_SYSCLK_MHZ);
    __asm volatile ("dsb sy\\nisb sy" ::: "memory");
    neopixel_init(get_sysclk());
    for (uint32_t i = 0; i < MAX_RAM_SLOTS; ++i)
        s_ram_to_flash[i] = FLASH_SLOT_UNKNOWN;

    rbcp_setup(ora_lookup_fn, knock);
    if (s_boot_ram_slot < MAX_RAM_SLOTS)
        s_ram_to_flash[s_boot_ram_slot] = s_boot_flash_slot;
    neopixel_show_flash_slot(s_boot_flash_slot);''', 1)
source = '\n'.join(line.rstrip() for line in source.splitlines()) + '\n'
target.write_text(source, newline='\n')
(here / 'v080').mkdir(exist_ok=True)
(here / 'v080/host_control_rgb_main.c').write_text(source, newline='\n')
print(target)
