/* Experimental PHY diagnostic-bus explorer. No processing in the live IQ path.
 * Reuses the four register states previously tested in c5vrx2 diagnostics.
 * A CPU GPIO snapshot is NOT a 40 MS/s synchronous capture: a plausible
 * candidate requires a later source-clocked PARLIO validation. */
#include "phy_phase_tap_probe.h"

#include <inttypes.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>

#include "esp_attr.h"
#include "esp_rom_gpio.h"
#include "esp_timer.h"
#include "soc/gpio_reg.h"
#include "soc/gpio_sig_map.h"
#include "modem/modem_syscon_reg.h"

#define DIAG_FIX_REG      0x600a9404u
#define DIAG_EXCHANGE_REG 0x600a9408u
#define TRACE_SAMPLES     512u
#define DIAG_LANES        32u
#define CAPTURE_WIDTH     8u

#define REG32(address) (*(volatile uint32_t *)(uintptr_t)(address))

/* Same eight routed XIAO pins and raw-Q4/I4 lanes as main/rf.c. */
static const uint8_t s_pins[CAPTURE_WIDTH] = {1, 0, 25, 7, 10, 5, 3, 4};
static const uint8_t s_iq_lanes[CAPTURE_WIDTH] = {6, 7, 8, 9, 16, 17, 18, 19};
static uint8_t s_trace[TRACE_SAMPLES];

static inline void io_fence(void)
{
    __asm__ __volatile__("fence iorw, iorw" ::: "memory");
}

static void route_lanes(unsigned first, bool reference)
{
    for (unsigned bit = 0; bit < CAPTURE_WIDTH; ++bit) {
        const unsigned lane = reference ? s_iq_lanes[bit] : first + bit;
        esp_rom_gpio_connect_out_signal(s_pins[bit], MODEM_DIAG0_IDX + lane,
                                         false, false);
    }
    io_fence();
}

static void capture_and_print(unsigned config, unsigned first, bool reference)
{
    /* Discard samples taken while changing the output-matrix selection. */
    for (unsigned i = 0; i < 128; ++i) (void)REG32(GPIO_IN_REG);
    const int64_t start = esp_timer_get_time();
    for (unsigned i = 0; i < TRACE_SAMPLES; ++i) {
        const uint32_t gpio = REG32(GPIO_IN_REG);
        uint8_t packed = 0;
        for (unsigned bit = 0; bit < CAPTURE_WIDTH; ++bit)
            packed |= ((gpio >> s_pins[bit]) & 1u) << bit;
        s_trace[i] = packed;
    }
    const int64_t elapsed = esp_timer_get_time() - start;
    uint32_t transitions[CAPTURE_WIDTH] = {0};
    uint32_t ever_high = 0, ever_low = 0;
    for (unsigned i = 0; i < TRACE_SAMPLES; ++i) {
        const uint8_t value = s_trace[i];
        ever_high |= value;
        ever_low |= (uint8_t)~value;
        if (i) {
            for (unsigned bit = 0; bit < CAPTURE_WIDTH; ++bit)
                transitions[bit] += ((value ^ s_trace[i - 1]) >> bit) & 1u;
        }
    }

    printf("PHY_TAP config=%u lanes=%s%u..%u samples=%u elapsed_us=%lld "
           "vary=0x%02lx transitions=", config, reference ? "IQ_REF/" : "",
           first, first + CAPTURE_WIDTH - 1u, TRACE_SAMPLES,
           (long long)elapsed, (unsigned long)(ever_high & ever_low));
    for (unsigned bit = 0; bit < CAPTURE_WIDTH; ++bit)
        printf("%s%lu", bit ? "," : "", (unsigned long)transitions[bit]);
    printf(" hex=");
    for (unsigned i = 0; i < TRACE_SAMPLES; ++i) printf("%02x", s_trace[i]);
    printf("\n");
}

#define DUMP_CTRL        0x600a9004u
#define DUMP_PTR_MODE    0x600a9008u
#define HP_SRAM_USAGE    0x60095004u
#define DUMP_BASE_ADDR   0x40830000u
#define DUMP_WORDS       8192u
#define Q6_GPIO_SAMPLES  512u
#define CTRL_ENABLE      0x80000000u
#define CTRL_DUMP_FIRST  0x00020000u
#define CHUNK_BYTES      1024u

static const uint8_t s_q6_lanes[CAPTURE_WIDTH] = {4, 5, 6, 7, 8, 9, 16, 17};
static RTC_DATA_ATTR uint8_t s_gpio_packed[Q6_GPIO_SAMPLES];

static inline uint32_t get_cycle_count(void)
{
    uint32_t c;
    __asm__ __volatile__("rdcycle %0" : "=r"(c));
    return c;
}

typedef uint8_t (*ring_pack_fn_t)(uint32_t w);

typedef struct {
    const char *name;
    uint8_t lanes[CAPTURE_WIDTH];
    ring_pack_fn_t pack_fn;
} sweep_pass_t;

/* Pass 0: Q_BUS_0_5
 * Pins 0..5: DIAG[0..5] <-> Dump Q[0..5] (w bits 0..5)
 * Pins 6..7: DIAG[8..9] <-> Dump Q[8..9] (w bits 8..9) */
static inline uint8_t pack_pass0(uint32_t w)
{
    return (uint8_t)((w & 0x3fu) | (((w >> 8) & 0x03u) << 6));
}

/* Pass 1: I_BUS_0_5
 * Pins 0..5: DIAG[10..15] <-> Dump I[0..5] (w bits 10..15)
 * Pins 6..7: DIAG[8..9]   <-> Dump Q[8..9] (w bits 8..9) */
static inline uint8_t pack_pass1(uint32_t w)
{
    return (uint8_t)(((w >> 10) & 0x3fu) | (((w >> 8) & 0x03u) << 6));
}

/* Pass 2: IQ_BUS_6_9
 * Pins 0..3: DIAG[16..19] <-> Dump I[6..9] (w bits 16..19)
 * Pins 4..5: DIAG[6..7]   <-> Dump Q[6..7] (w bits 6..7)
 * Pins 6..7: DIAG[8..9]   <-> Dump Q[8..9] (w bits 8..9) */
static inline uint8_t pack_pass2(uint32_t w)
{
    return (uint8_t)(((w >> 16) & 0x0fu) | (((w >> 6) & 0x03u) << 4) | (((w >> 8) & 0x03u) << 6));
}

/* Pass 3: CTRL_20_25
 * Pins 0..5: DIAG[20..25] <-> Dump[20..25] (w bits 20..25)
 * Pins 6..7: DIAG[8..9]   <-> Dump Q[8..9] (w bits 8..9) */
static inline uint8_t pack_pass3(uint32_t w)
{
    return (uint8_t)(((w >> 20) & 0x3fu) | (((w >> 8) & 0x03u) << 6));
}

/* Pass 4: CTRL_26_31
 * Pins 0..5: DIAG[26..31] <-> Dump[26..31] (w bits 26..31)
 * Pins 6..7: DIAG[8..9]   <-> Dump Q[8..9] (w bits 8..9) */
static inline uint8_t pack_pass4(uint32_t w)
{
    return (uint8_t)(((w >> 26) & 0x3fu) | (((w >> 8) & 0x03u) << 6));
}

/* Pass 5: CAND_123 -- exactly the issue #123 lanes. Six proven bits give
 * a strong alignment reference for the two candidates Q[5] and I[5].
 * Pins 0..3: DIAG[5,6,7,9]    <-> Dump Q[5,6,7,9]  (w bits 5,6,7,9)
 * Pins 4..7: DIAG[15,16,17,19] <-> Dump I[5,6,7,9] (w bits 15,16,17,19) */
static inline uint8_t pack_pass5(uint32_t w)
{
    return (uint8_t)(((w >> 5) & 0x07u) | (((w >> 9) & 1u) << 3) |
                     (((w >> 15) & 0x07u) << 4) | (((w >> 19) & 1u) << 7));
}

/* Pass 6: I_BUS_4_9 -- the I bus aligned on its own proven bits (pass 1 used
 * only Q[8:9], which cannot lock fast I bits to the right dump word).
 * Pins 0..5: DIAG[14..19] <-> Dump I[4..9] (w bits 14..19)
 * Pins 6..7: DIAG[8..9]   <-> Dump Q[8..9] (w bits 8..9) */
static inline uint8_t pack_pass6(uint32_t w)
{
    return (uint8_t)(((w >> 14) & 0x3fu) | (((w >> 8) & 0x03u) << 6));
}

static const sweep_pass_t s_sweep_passes[7] = {
    {
        .name = "Q_BUS_0_5",
        .lanes = {0, 1, 2, 3, 4, 5, 8, 9},
        .pack_fn = pack_pass0,
    },
    {
        .name = "I_BUS_0_5",
        .lanes = {10, 11, 12, 13, 14, 15, 8, 9},
        .pack_fn = pack_pass1,
    },
    {
        .name = "IQ_BUS_6_9",
        .lanes = {16, 17, 18, 19, 6, 7, 8, 9},
        .pack_fn = pack_pass2,
    },
    {
        .name = "CTRL_20_25",
        .lanes = {20, 21, 22, 23, 24, 25, 8, 9},
        .pack_fn = pack_pass3,
    },
    {
        .name = "CTRL_26_31",
        .lanes = {26, 27, 28, 29, 30, 31, 8, 9},
        .pack_fn = pack_pass4,
    },
    {
        .name = "CAND_123",
        .lanes = {5, 6, 7, 9, 15, 16, 17, 19},
        .pack_fn = pack_pass5,
    },
    {
        .name = "I_BUS_4_9",
        .lanes = {14, 15, 16, 17, 18, 19, 8, 9},
        .pack_fn = pack_pass6,
    },
};

static void q6_dump_probe_run(void)
{
    /* Route DIAG[4, 5, 6, 7, 8, 9, 16, 17] to the 8 XIAO GPIO pins.
     * Candidate Q[4:5] on bits 0..1, proven Q[6:9] on bits 2..5,
     * proven I[6:7] on bits 6..7. */
    for (unsigned bit = 0; bit < CAPTURE_WIDTH; ++bit) {
        esp_rom_gpio_connect_out_signal(s_pins[bit],
                                        MODEM_DIAG0_IDX + s_q6_lanes[bit],
                                        false, false);
    }
    io_fence();

    /* Discard settling samples after matrix remap. */
    for (unsigned i = 0; i < 128; ++i) (void)REG32(GPIO_IN_REG);

    /* Save and disable machine interrupts during the ~100 us hardware dump window. */
    uint32_t saved_mstatus;
    __asm__ __volatile__("csrrc %0, mstatus, %1"
                         : "=r"(saved_mstatus)
                         : "r"(0x8u)
                         : "memory");

    /* Grant MAC ownership of HP SRAM dump bank (0x40830000). */
    const uint32_t saved_sram_usage = REG32(HP_SRAM_USAGE);
    REG32(HP_SRAM_USAGE) = (saved_sram_usage & 0xfffef0ffu) | 0x00010200u;
    io_fence();

    /* Arm dump engine with DUMP_WORDS in circular pre-trigger mode. */
    uint32_t ctrl = REG32(DUMP_CTRL);
    ctrl &= ~(CTRL_ENABLE | 0x00080000u | 0x00040000u); /* clear ENABLE, START, DONE */
    ctrl |= CTRL_DUMP_FIRST;
    ctrl = (ctrl & ~0x0001ffffu) | DUMP_WORDS;
    REG32(DUMP_CTRL) = ctrl | CTRL_ENABLE;
    io_fence();

    /* Sample GPIO_IN_REG and pack directly into RTC SRAM (0x50000000).
     * RTC SRAM is unaffected while HP SRAM is MAC-owned. */
    const uint32_t t_begin = get_cycle_count();
    for (unsigned i = 0; i < Q6_GPIO_SAMPLES; ++i) {
        const uint32_t gpio = REG32(GPIO_IN_REG);
        uint8_t packed = 0;
        for (unsigned bit = 0; bit < CAPTURE_WIDTH; ++bit) {
            packed |= ((gpio >> s_pins[bit]) & 1u) << bit;
        }
        s_gpio_packed[i] = packed;
    }
    const uint32_t t_end = get_cycle_count();
    io_fence();

    /* Read write pointer at halt and disable dump. */
    const uint32_t stop_ptr = REG32(DUMP_PTR_MODE) & (DUMP_WORDS - 1u);
    REG32(DUMP_CTRL) = ctrl; /* clear CTRL_ENABLE */
    io_fence();

    /* Restore CPU ownership of HP SRAM. */
    REG32(HP_SRAM_USAGE) = saved_sram_usage;
    io_fence();

    /* Restore machine interrupt state. */
    if ((saved_mstatus & 0x8u) != 0u) {
        __asm__ __volatile__("csrs mstatus, %0" : : "r"(0x8u) : "memory");
    }

    const uint32_t elapsed_us = (t_end - t_begin) / 240u;

    /* Print header delimiter and metadata for analysis tool. */
    printf("Q6_DUMP BEGIN samples=%u dump_words=%u stop_ptr=%" PRIu32 " elapsed_us=%" PRIu32 "\n",
           Q6_GPIO_SAMPLES, DUMP_WORDS, stop_ptr, elapsed_us);

    /* Print packed GPIO trace. */
    printf("Q6_GPIO hex=");
    for (unsigned i = 0; i < Q6_GPIO_SAMPLES; ++i) {
        printf("%02x", s_gpio_packed[i]);
    }
    printf("\n");

    /* Print post-stop dump ring bytes from 0x40830000 in 1 KiB chunks.
     * Each 32-bit word unpacks:
     *   dump Q[4:9] in bits 0..5
     *   dump I[6:7] in bits 6..7 */
    volatile const uint32_t *dump_sram = (volatile const uint32_t *)DUMP_BASE_ADDR;
    const unsigned num_chunks = DUMP_WORDS / CHUNK_BYTES;
    for (unsigned c = 0; c < num_chunks; ++c) {
        printf("Q6_RING chunk=%u hex=", c);
        const unsigned start_idx = c * CHUNK_BYTES;
        for (unsigned i = 0; i < CHUNK_BYTES; ++i) {
            const uint32_t w = dump_sram[start_idx + i];
            const uint8_t packed = ((w >> 4) & 0x3fu) | (((w >> 16) & 0x03u) << 6);
            printf("%02x", packed);
        }
        printf("\n");
    }
    printf("Q6_DUMP END\n");

    /* Restore reference IQ routing. */
    route_lanes(0, true);
}

/* Full 32-bit RF dump words: Q[9:0] bits 0..9, I[9:0] bits 10..19 and,
 * per ESPARGOS esp-sdr, the RX gain index in bits 20..27 and the native AGC
 * state machine in bits 28..31 -- per sample at 80 MS/s. Answers which state
 * bits mark a native re-acquisition (tools/analyze_agc_words.py). */
#define AGC_WORD_WINDOWS 3u
static void agc_words_run(void)
{
    for (unsigned n = 0; n < AGC_WORD_WINDOWS; ++n) {
        uint32_t saved_mstatus;
        __asm__ __volatile__("csrrc %0, mstatus, %1"
                             : "=r"(saved_mstatus) : "r"(0x8u) : "memory");
        const uint32_t saved_sram_usage = REG32(HP_SRAM_USAGE);
        REG32(HP_SRAM_USAGE) = (saved_sram_usage & 0xfffef0ffu) | 0x00010200u;
        io_fence();
        uint32_t ctrl = REG32(DUMP_CTRL);
        ctrl &= ~(CTRL_ENABLE | 0x00080000u | 0x00040000u);
        ctrl |= CTRL_DUMP_FIRST;
        ctrl = (ctrl & ~0x0001ffffu) | DUMP_WORDS;
        REG32(DUMP_CTRL) = ctrl | CTRL_ENABLE;
        io_fence();
        /* 8192 words at ~80 MS/s = 102 us; wait 150 us so the ring wraps. */
        const uint32_t t0 = get_cycle_count();
        while (get_cycle_count() - t0 < 150u * 240u) { }
        const uint32_t stop_ptr = REG32(DUMP_PTR_MODE) & (DUMP_WORDS - 1u);
        REG32(DUMP_CTRL) = ctrl;
        io_fence();
        REG32(HP_SRAM_USAGE) = saved_sram_usage;
        io_fence();
        if ((saved_mstatus & 0x8u) != 0u)
            __asm__ __volatile__("csrs mstatus, %0" : : "r"(0x8u) : "memory");

        printf("AGC_WORDS BEGIN window=%u words=%u stop_ptr=%" PRIu32 "\n",
               n, DUMP_WORDS, stop_ptr);
        volatile const uint32_t *dump_sram = (volatile const uint32_t *)DUMP_BASE_ADDR;
        for (unsigned c = 0; c < DUMP_WORDS / 256u; ++c) {
            printf("AGC_WORDS_HEX window=%u chunk=%u hex=", n, c);
            for (unsigned i = 0; i < 256u; ++i)
                printf("%08" PRIx32, dump_sram[c * 256u + i]);
            printf("\n");
        }
        printf("AGC_WORDS END window=%u\n", n);
        /* Separate the windows in time so they see different restarts. */
        const uint32_t t1 = get_cycle_count();
        while (get_cycle_count() - t1 < 2000u * 240u) { }
    }
}

static void all_diag_sweep_run(void)
{
    printf("DIAG_SWEEP_SESSION BEGIN total_passes=7\n");
    for (unsigned p = 0; p < 7; ++p) {
        const sweep_pass_t *pass = &s_sweep_passes[p];

        /* Route lanes for this pass */
        for (unsigned bit = 0; bit < CAPTURE_WIDTH; ++bit) {
            esp_rom_gpio_connect_out_signal(s_pins[bit],
                                            MODEM_DIAG0_IDX + pass->lanes[bit],
                                            false, false);
        }
        io_fence();

        /* Discard settling samples after matrix remap */
        for (unsigned i = 0; i < 128; ++i) (void)REG32(GPIO_IN_REG);

        /* Save and disable machine interrupts during the ~100 us hardware dump window */
        uint32_t saved_mstatus;
        __asm__ __volatile__("csrrc %0, mstatus, %1"
                             : "=r"(saved_mstatus)
                             : "r"(0x8u)
                             : "memory");

        /* Grant MAC ownership of HP SRAM dump bank (0x40830000) */
        const uint32_t saved_sram_usage = REG32(HP_SRAM_USAGE);
        REG32(HP_SRAM_USAGE) = (saved_sram_usage & 0xfffef0ffu) | 0x00010200u;
        io_fence();

        /* Arm dump engine with DUMP_WORDS in circular pre-trigger mode */
        uint32_t ctrl = REG32(DUMP_CTRL);
        ctrl &= ~(CTRL_ENABLE | 0x00080000u | 0x00040000u);
        ctrl |= CTRL_DUMP_FIRST;
        ctrl = (ctrl & ~0x0001ffffu) | DUMP_WORDS;
        REG32(DUMP_CTRL) = ctrl | CTRL_ENABLE;
        io_fence();

        /* Sample GPIO_IN_REG directly into RTC SRAM (0x50000000) */
        const uint32_t t_begin = get_cycle_count();
        for (unsigned i = 0; i < Q6_GPIO_SAMPLES; ++i) {
            const uint32_t gpio = REG32(GPIO_IN_REG);
            uint8_t packed = 0;
            for (unsigned bit = 0; bit < CAPTURE_WIDTH; ++bit) {
                packed |= ((gpio >> s_pins[bit]) & 1u) << bit;
            }
            s_gpio_packed[i] = packed;
        }
        const uint32_t t_end = get_cycle_count();
        io_fence();

        /* Read write pointer at halt and disable dump */
        const uint32_t stop_ptr = REG32(DUMP_PTR_MODE) & (DUMP_WORDS - 1u);
        REG32(DUMP_CTRL) = ctrl; /* clear CTRL_ENABLE */
        io_fence();

        /* Restore CPU ownership of HP SRAM */
        REG32(HP_SRAM_USAGE) = saved_sram_usage;
        io_fence();

        /* Restore machine interrupt state */
        if ((saved_mstatus & 0x8u) != 0u) {
            __asm__ __volatile__("csrs mstatus, %0" : : "r"(0x8u) : "memory");
        }

        const uint32_t elapsed_us = (t_end - t_begin) / 240u;

        /* Print header delimiter and metadata for analysis tool */
        printf("DIAG_SWEEP BEGIN pass=%u name=%s samples=%u dump_words=%u stop_ptr=%" PRIu32 " elapsed_us=%" PRIu32 "\n",
               p, pass->name, Q6_GPIO_SAMPLES, DUMP_WORDS, stop_ptr, elapsed_us);

        /* Print packed GPIO trace */
        printf("DIAG_SWEEP_GPIO hex=");
        for (unsigned i = 0; i < Q6_GPIO_SAMPLES; ++i) {
            printf("%02x", s_gpio_packed[i]);
        }
        printf("\n");

        /* Print post-stop dump ring bytes in 1 KiB chunks */
        volatile const uint32_t *dump_sram = (volatile const uint32_t *)DUMP_BASE_ADDR;
        const unsigned num_chunks = DUMP_WORDS / CHUNK_BYTES;
        for (unsigned c = 0; c < num_chunks; ++c) {
            printf("DIAG_SWEEP_RING pass=%u chunk=%u hex=", p, c);
            const unsigned start_idx = c * CHUNK_BYTES;
            for (unsigned i = 0; i < CHUNK_BYTES; ++i) {
                const uint32_t w = dump_sram[start_idx + i];
                const uint8_t packed = pass->pack_fn(w);
                printf("%02x", packed);
            }
            printf("\n");
        }
        printf("DIAG_SWEEP END pass=%u\n", p);
    }
    printf("DIAG_SWEEP_SESSION COMPLETE passes=7\n");

    /* Also execute baseline Q6 dump format for backwards compatibility */
    q6_dump_probe_run();

    agc_words_run();

    /* Restore reference IQ routing */
    route_lanes(0, true);
}

void phy_phase_tap_probe_run(void)
{
    const uint32_t saved_fix = REG32(DIAG_FIX_REG);
    const uint32_t saved_exchange = REG32(DIAG_EXCHANGE_REG);
    const uint32_t saved_test = REG32(MODEM_SYSCON_TEST_CONF_REG);

    printf("PHY_TAP BEGIN boot_only=1 cpu_snapshot=1 synchronous=0 "
           "iq_reference_lanes=6,7,8,9,16,17,18,19\n");
    for (unsigned config = 0; config < 4; ++config) {
        uint32_t fix = saved_fix;
        uint32_t exchange = saved_exchange;
        uint32_t test = saved_test;
        if (config == 1 || config == 2) exchange = 2u;
        if (config == 2) fix = (fix & ~0x3ffu) | 0x14eu;
        if (config == 3)
            test |= MODEM_SYSCON_FPGA_DEBUG_CLKSWITCH |
                    MODEM_SYSCON_FPGA_DEBUG_CLK80;
        REG32(DIAG_FIX_REG) = fix;
        REG32(DIAG_EXCHANGE_REG) = exchange;
        REG32(MODEM_SYSCON_TEST_CONF_REG) = test;
        io_fence();
        printf("PHY_TAP selector=%u fix=0x%08" PRIx32 " exchange=0x%08" PRIx32
               " test=0x%08" PRIx32 "\n", config, fix, exchange, test);
        route_lanes(0, true);
        capture_and_print(config, 0, true);
        for (unsigned first = 0; first < DIAG_LANES; first += CAPTURE_WIDTH) {
            route_lanes(first, false);
            capture_and_print(config, first, false);
        }
    }

    REG32(DIAG_FIX_REG) = saved_fix;
    REG32(DIAG_EXCHANGE_REG) = saved_exchange;
    REG32(MODEM_SYSCON_TEST_CONF_REG) = saved_test;
    route_lanes(0, true);
    io_fence();

    /* Execute the comprehensive 32-lane sweep and Q6 baseline probe. */
    all_diag_sweep_run();

    printf("PHY_TAP END restored=1 candidate_status=SWEEP_COMPLETE\n");
}
