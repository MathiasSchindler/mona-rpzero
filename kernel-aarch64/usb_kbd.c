#include "usb_kbd.h"

#include "console_in.h"
#include "mmu.h"
#include "time.h"
#include "uart_pl011.h"

#ifdef ENABLE_USB_KBD_DEBUG
#define USB_KBD_DEBUG 1
#else
#define USB_KBD_DEBUG 0
#endif

/*
 * DWC2 register model (subset).
 * This is intentionally minimal and polled; no IRQs, no hubs.
 */

#define DWC2_BASE (0x3F000000ull + 0x00980000ull)

static inline volatile uint32_t *dwc2_reg(uint32_t off) {
    return (volatile uint32_t *)(uintptr_t)(DWC2_BASE + (uint64_t)off);
}

/* Global regs */
#define GOTGCTL   0x000u
#define GAHBCFG   0x008u
#define GUSBCFG   0x00Cu
#define GRSTCTL   0x010u
#define GINTSTS   0x014u
#define GINTMSK   0x018u
#define GRXSTSP   0x020u
#define GRXFSIZ   0x024u
#define GNPTXFSIZ 0x028u
#define GNPTXSTS  0x02Cu

#define GINTSTS_RXFLVL (1u << 4)

/* Host regs */
#define HCFG      0x400u
#define HFIR      0x404u
#define HPRT      0x440u

/* Host channel regs */
#define HC_BASE   0x500u
#define HC_STRIDE 0x20u
#define HCCHAR(n) (HC_BASE + (uint32_t)(n) * HC_STRIDE + 0x00u)
#define HCSPLT(n) (HC_BASE + (uint32_t)(n) * HC_STRIDE + 0x04u)
#define HCINT(n)  (HC_BASE + (uint32_t)(n) * HC_STRIDE + 0x08u)
#define HCINTMSK(n) (HC_BASE + (uint32_t)(n) * HC_STRIDE + 0x0Cu)
#define HCTSIZ(n) (HC_BASE + (uint32_t)(n) * HC_STRIDE + 0x10u)
#define HCDMA(n)  (HC_BASE + (uint32_t)(n) * HC_STRIDE + 0x14u)

/* FIFOs */
#define DFIFO_BASE 0x1000u
static inline volatile uint32_t *dwc2_fifo(uint32_t n) {
    return (volatile uint32_t *)(uintptr_t)(DWC2_BASE + (uint64_t)DFIFO_BASE + (uint64_t)n * 0x1000ull);
}

/* Bit helpers (subset) */
#define GRSTCTL_CSRST   (1u << 0)
#define GRSTCTL_RXFFLSH (1u << 4)
#define GRSTCTL_TXFFLSH (1u << 5)
#define GRSTCTL_TXFNUM_SHIFT 6
#define GRSTCTL_AHBIDL  (1u << 31)

/* GAHBCFG (subset) */
#define GAHBCFG_GLBLINTRMSK (1u << 0)
#define GAHBCFG_DMAEN       (1u << 5)

#define GUSBCFG_FHMOD (1u << 29)
#define GUSBCFG_FDMOD (1u << 30)

#define HPRT_CONNSTS  (1u << 0)
#define HPRT_CONNDET  (1u << 1)
#define HPRT_ENA      (1u << 2)
#define HPRT_ENCHNG   (1u << 3)
#define HPRT_OVRCURRCHNG (1u << 5)
#define HPRT_RST      (1u << 8)
#define HPRT_PWR      (1u << 12)
#define HPRT_SPD_SHIFT 17
#define HPRT_SPD_MASK (3u << HPRT_SPD_SHIFT)

/* Host channel */
#define HCCHAR_MPS_MASK 0x7FFu
#define HCCHAR_EPNUM_SHIFT 11
#define HCCHAR_EPDIR (1u << 15)
#define HCCHAR_LSDEV (1u << 17)
#define HCCHAR_EPTYP_SHIFT 18
#define HCCHAR_EPTYP_MASK (3u << HCCHAR_EPTYP_SHIFT)
#define HCCHAR_DEVADDR_SHIFT 22
#define HCCHAR_CHDIS (1u << 30)
#define HCCHAR_CHENA (1u << 31)

#define EPTYP_CTRL 0u
#define EPTYP_ISO  1u
#define EPTYP_BULK 2u
#define EPTYP_INTR 3u

/* HCINT bits (subset) */
#define HCINT_XFERCOMPL (1u << 0)
#define HCINT_CHHLTD    (1u << 1)
#define HCINT_STALL     (1u << 3)
#define HCINT_NAK       (1u << 4)
#define HCINT_ACK       (1u << 5)
#define HCINT_XACTERR   (1u << 7)
#define HCINT_BBLERR    (1u << 8)
#define HCINT_FRMOVRUN  (1u << 9)
#define HCINT_DATATGLERR (1u << 10)

/* HCTSIZ */
#define HCTSIZ_XFERSIZE_MASK 0x7FFFFu
#define HCTSIZ_PKTCNT_SHIFT 19
#define HCTSIZ_DPID_SHIFT 29
#define DPID_DATA0 0u
#define DPID_DATA1 2u
#define DPID_SETUP 3u

/* GRXSTSP packet status (host mode) */
#define GRXSTSP_PKTSTS_SHIFT 17
#define GRXSTSP_PKTSTS_MASK (0xFu << GRXSTSP_PKTSTS_SHIFT)
#define GRXSTSP_BCNT_SHIFT 4
#define GRXSTSP_BCNT_MASK (0x7FFu << GRXSTSP_BCNT_SHIFT)
#define GRXSTSP_CHNUM_SHIFT 0
#define GRXSTSP_CHNUM_MASK 0xFu

/* Common host receive statuses (documented by Synopsys; subset used) */
#define PKTSTS_IN_DATA  (2u << GRXSTSP_PKTSTS_SHIFT)
#define PKTSTS_IN_COMP  (3u << GRXSTSP_PKTSTS_SHIFT)
#define PKTSTS_DATA_TOGGLE_ERR (5u << GRXSTSP_PKTSTS_SHIFT)
#define PKTSTS_CH_HALTED (7u << GRXSTSP_PKTSTS_SHIFT)

typedef struct {
    int inited;
    int ready;
    uint8_t dev_addr;
    uint8_t intr_in_ep;
    uint16_t intr_in_mps;
    int low_speed;
    uint32_t intr_in_pid;
    uint8_t last_report[8];
} usb_kbd_state_t;

static usb_kbd_state_t g;

/* QEMU's DWC2 model appears to behave best in host DMA mode. */
#define USB_KBD_USE_DMA 1

static uint64_t usb_kbd_virt_to_phys(const void *p) {
    uint64_t va = (uint64_t)(uintptr_t)p;
    if (va >= KERNEL_VA_BASE) return va - KERNEL_VA_BASE;
    return va;
}

static uint64_t deadline_ns(uint64_t delta_ns) {
    uint64_t now = time_now_ns();
    if (now == 0) return 0;
    return now + delta_ns;
}

static int time_before_deadline(uint64_t dl) {
    if (dl == 0) return 1; /* if time not inited, avoid timeouts */
    return time_now_ns() < dl;
}

static void udelay_ns(uint64_t ns) {
    uint64_t dl = deadline_ns(ns);
    while (time_before_deadline(dl)) {
        /* spin */
    }
}

static int dwc2_core_reset(void) {
    /* Wait for AHB idle before resetting. */
    uint64_t dl = deadline_ns(20000000ull);
    while (time_before_deadline(dl)) {
        if ((*dwc2_reg(GRSTCTL) & GRSTCTL_AHBIDL) != 0) break;
    }

    *dwc2_reg(GRSTCTL) = GRSTCTL_CSRST;
    dl = deadline_ns(20000000ull);
    while (time_before_deadline(dl)) {
        uint32_t v = *dwc2_reg(GRSTCTL);
        if ((v & GRSTCTL_CSRST) == 0 && (v & GRSTCTL_AHBIDL) != 0) return 0;
    }
    return -1;
}

static void dwc2_flush_fifos(void) {
    /* Flush RX FIFO */
    *dwc2_reg(GRSTCTL) = GRSTCTL_RXFFLSH;
    uint64_t dl = deadline_ns(20000000ull);
    while (time_before_deadline(dl)) {
        if ((*dwc2_reg(GRSTCTL) & GRSTCTL_RXFFLSH) == 0) break;
    }

    /* Flush all TX FIFOs: set TXFNUM=0x10 (all) + TXFFLSH */
    *dwc2_reg(GRSTCTL) = ((0x10u << GRSTCTL_TXFNUM_SHIFT) | GRSTCTL_TXFFLSH);
    dl = deadline_ns(20000000ull);
    while (time_before_deadline(dl)) {
        if ((*dwc2_reg(GRSTCTL) & GRSTCTL_TXFFLSH) == 0) break;
    }
}

static void dwc2_force_host_mode(void) {
    uint32_t v = *dwc2_reg(GUSBCFG);
    v |= GUSBCFG_FHMOD;
    v &= ~GUSBCFG_FDMOD;
    *dwc2_reg(GUSBCFG) = v;
    (void)*dwc2_reg(GUSBCFG);
}

static void dwc2_fifo_init_defaults(void) {
    /* Conservative FIFO sizing (words). Values are not performance critical here. */
    *dwc2_reg(GRXFSIZ) = 256u;
    /* Non-periodic Tx FIFO: start after Rx FIFO, depth 256 words. */
    *dwc2_reg(GNPTXFSIZ) = (256u << 16) | 256u;
}

#if !USB_KBD_USE_DMA
static uint32_t dwc2_nptx_space_words(void) {
    /* GNPTXSTS.NPTxFSpcAvail [15:0] (words) */
    return (*dwc2_reg(GNPTXSTS)) & 0xFFFFu;
}

static int dwc2_wait_nptx_space(uint32_t words, uint64_t timeout_ns) {
    uint64_t dl = deadline_ns(timeout_ns);
    while (time_before_deadline(dl)) {
        if (dwc2_nptx_space_words() >= words) return 0;
    }
    return -1;
}
#endif

static int dwc2_host_port_power_and_reset(void) {
    uint32_t p = *dwc2_reg(HPRT);
    /* Clear W1C bits by writing them back as 1 where set. */
    p |= HPRT_PWR;
    p |= (p & (HPRT_CONNDET | HPRT_ENCHNG | HPRT_OVRCURRCHNG));
    *dwc2_reg(HPRT) = p;

    /* Wait for connect */
    uint64_t dl = deadline_ns(2000000000ull);
    while (time_before_deadline(dl)) {
        uint32_t s = *dwc2_reg(HPRT);
        if (s & HPRT_CONNSTS) break;
    }
    if (((*dwc2_reg(HPRT)) & HPRT_CONNSTS) == 0) return -1;

    /* Port reset pulse */
    uint32_t s0 = *dwc2_reg(HPRT);
    s0 |= HPRT_RST;
    s0 |= (s0 & (HPRT_CONNDET | HPRT_ENCHNG | HPRT_OVRCURRCHNG));
    *dwc2_reg(HPRT) = s0;
    udelay_ns(60000000ull);

    uint32_t s1 = *dwc2_reg(HPRT);
    s1 &= ~HPRT_RST;
    s1 |= (s1 & (HPRT_CONNDET | HPRT_ENCHNG | HPRT_OVRCURRCHNG));
    *dwc2_reg(HPRT) = s1;

    udelay_ns(20000000ull);
    return 0;
}

static int dwc2_port_is_low_speed(void) {
    uint32_t s = *dwc2_reg(HPRT);
    uint32_t spd = (s & HPRT_SPD_MASK) >> HPRT_SPD_SHIFT;
    /* 1=low, 2=full (common encoding) */
    return (spd == 1u);
}

static void dwc2_host_configure_fsls_clock(void) {
    /* HCFG.FSLSPclkSel = 1 (48MHz). */
    uint32_t hcfg = *dwc2_reg(HCFG);
    hcfg &= ~0x3u;
    hcfg |= 0x1u;
    *dwc2_reg(HCFG) = hcfg;
}

static void hc_clear_ints(uint32_t ch) {
    *dwc2_reg(HCINT(ch)) = 0xFFFFFFFFu;
}

static int hc_wait_xfer(uint32_t ch, uint64_t timeout_ns) {
    uint64_t dl = deadline_ns(timeout_ns);
    for (;;) {
        uint32_t ints = *dwc2_reg(HCINT(ch));
        if (ints & (HCINT_XFERCOMPL | HCINT_CHHLTD | HCINT_NAK | HCINT_STALL | HCINT_XACTERR | HCINT_BBLERR | HCINT_FRMOVRUN | HCINT_DATATGLERR)) {
            return (int)ints;
        }
        if (!time_before_deadline(dl)) {
            return -1;
        }
    }
}

static void hc_halt(uint32_t ch) {
    uint32_t hcchar = *dwc2_reg(HCCHAR(ch));
    hcchar |= HCCHAR_CHDIS;
    hcchar |= HCCHAR_CHENA;
    *dwc2_reg(HCCHAR(ch)) = hcchar;
}

static int dwc2_out_xfer(uint32_t ch, uint8_t dev_addr, uint8_t ep, uint8_t ep_type, uint16_t mps, int low_speed, uint32_t pid, const uint8_t *data, uint32_t len) {
    if (len > 0 && !data) return -1;

    hc_clear_ints(ch);
    *dwc2_reg(HCINTMSK(ch)) = 0xFFFFFFFFu;

    uint32_t hcchar = 0;
    hcchar |= (uint32_t)(mps & HCCHAR_MPS_MASK);
    hcchar |= ((uint32_t)ep << HCCHAR_EPNUM_SHIFT);
    /* OUT => EPDIR=0 */
    hcchar |= ((uint32_t)ep_type << HCCHAR_EPTYP_SHIFT);
    if (low_speed) hcchar |= HCCHAR_LSDEV;
    hcchar |= ((uint32_t)dev_addr << HCCHAR_DEVADDR_SHIFT);
    *dwc2_reg(HCCHAR(ch)) = hcchar;

    uint32_t pktcnt = (len + mps - 1u) / mps;
    if (pktcnt == 0) pktcnt = 1;
    uint32_t hctsiz = 0;
    hctsiz |= (len & HCTSIZ_XFERSIZE_MASK);
    hctsiz |= (pktcnt << HCTSIZ_PKTCNT_SHIFT);
    hctsiz |= ((pid & 0x3u) << HCTSIZ_DPID_SHIFT);
    *dwc2_reg(HCTSIZ(ch)) = hctsiz;

#if USB_KBD_USE_DMA
    *dwc2_reg(HCDMA(ch)) = (uint32_t)usb_kbd_virt_to_phys(data);
#else

    /* Push payload into non-periodic FIFO (fifo 0). Wait for space to avoid corruption. */
    uint32_t words = (len + 3u) / 4u;
    if (words != 0) {
        if (dwc2_wait_nptx_space(words, 200000000ull) != 0) {
            return -1;
        }
    }
    for (uint32_t i = 0; i < words; i++) {
        uint32_t w = 0;
        uint32_t base = i * 4u;
        for (uint32_t b = 0; b < 4u; b++) {
            uint32_t off = base + b;
            uint8_t v = (off < len) ? data[off] : 0u;
            w |= ((uint32_t)v) << (8u * b);
        }
        *dwc2_fifo(0) = w;
    }
#endif

    /* Enable channel */
    hcchar = *dwc2_reg(HCCHAR(ch));
    hcchar |= HCCHAR_CHENA;
    hcchar &= ~HCCHAR_CHDIS;
    *dwc2_reg(HCCHAR(ch)) = hcchar;

    int rc = hc_wait_xfer(ch, 200000000ull);
    if (rc < 0) {
        hc_halt(ch);
        return -1;
    }
    if (rc & (HCINT_STALL | HCINT_XACTERR | HCINT_BBLERR | HCINT_FRMOVRUN | HCINT_DATATGLERR)) {
        hc_halt(ch);
        return -1;
    }
    return 0;
}

static int dwc2_in_xfer(uint32_t ch, uint8_t dev_addr, uint8_t ep, uint8_t ep_type, uint16_t mps, int low_speed, uint32_t pid, uint8_t *out, uint32_t len, uint32_t *out_got, int nak_ok) {
    if (len > 0 && !out) return -1;
    if (out_got) *out_got = 0;

    hc_clear_ints(ch);
    *dwc2_reg(HCINTMSK(ch)) = 0xFFFFFFFFu;

    uint32_t hcchar = 0;
    hcchar |= (uint32_t)(mps & HCCHAR_MPS_MASK);
    hcchar |= ((uint32_t)ep << HCCHAR_EPNUM_SHIFT);
    hcchar |= HCCHAR_EPDIR;
    hcchar |= ((uint32_t)ep_type << HCCHAR_EPTYP_SHIFT);
    if (low_speed) hcchar |= HCCHAR_LSDEV;
    hcchar |= ((uint32_t)dev_addr << HCCHAR_DEVADDR_SHIFT);
    *dwc2_reg(HCCHAR(ch)) = hcchar;

    uint32_t pktcnt = (len + mps - 1u) / mps;
    if (pktcnt == 0) pktcnt = 1;
    uint32_t hctsiz = 0;
    hctsiz |= (len & HCTSIZ_XFERSIZE_MASK);
    hctsiz |= (pktcnt << HCTSIZ_PKTCNT_SHIFT);
    hctsiz |= ((pid & 0x3u) << HCTSIZ_DPID_SHIFT);
    *dwc2_reg(HCTSIZ(ch)) = hctsiz;

#if USB_KBD_USE_DMA
    *dwc2_reg(HCDMA(ch)) = (uint32_t)usb_kbd_virt_to_phys(out);
#endif

    /* Enable channel */
    hcchar = *dwc2_reg(HCCHAR(ch));
    hcchar |= HCCHAR_CHENA;
    hcchar &= ~HCCHAR_CHDIS;
    *dwc2_reg(HCCHAR(ch)) = hcchar;

    int rc = hc_wait_xfer(ch, 200000000ull);
    if (rc < 0) {
        hc_halt(ch);
        return -1;
    }
    if (rc & HCINT_NAK) {
        hc_halt(ch);
        if (out_got) *out_got = 0;
        return nak_ok ? 0 : -1;
    }
    if (rc & (HCINT_STALL | HCINT_XACTERR | HCINT_BBLERR | HCINT_FRMOVRUN | HCINT_DATATGLERR)) {
        hc_halt(ch);
        return -1;
    }

#if USB_KBD_USE_DMA
    /* Bytes received = requested - remaining. */
    uint32_t rem = (*dwc2_reg(HCTSIZ(ch))) & HCTSIZ_XFERSIZE_MASK;
    uint32_t got = (len >= rem) ? (len - rem) : 0;
    if (out_got) *out_got = got;
    return 0;
#else
    /* FIFO/non-DMA path (not currently used in QEMU). */
    uint32_t got = 0;
    uint64_t dl = deadline_ns(200000000ull);
    while (time_before_deadline(dl)) {
        if ((*dwc2_reg(GINTSTS) & GINTSTS_RXFLVL) != 0) {
            uint32_t st = *dwc2_reg(GRXSTSP);
            uint32_t pktsts = st & GRXSTSP_PKTSTS_MASK;
            uint32_t bcnt = (st & GRXSTSP_BCNT_MASK) >> GRXSTSP_BCNT_SHIFT;

            if (pktsts == PKTSTS_IN_DATA) {
                uint32_t words = (bcnt + 3u) / 4u;
                for (uint32_t i = 0; i < words; i++) {
                    uint32_t w = *dwc2_fifo(0);
                    for (uint32_t b = 0; b < 4u; b++) {
                        if (got < len && (i * 4u + b) < bcnt) {
                            out[got++] = (uint8_t)((w >> (8u * b)) & 0xFFu);
                        }
                    }
                }
            } else {
                if (bcnt != 0) {
                    uint32_t words = (bcnt + 3u) / 4u;
                    for (uint32_t i = 0; i < words; i++) {
                        (void)*dwc2_fifo(0);
                    }
                }
            }
        }

        uint32_t ints = *dwc2_reg(HCINT(ch));
        if (ints & (HCINT_XFERCOMPL | HCINT_CHHLTD | HCINT_STALL | HCINT_XACTERR | HCINT_BBLERR | HCINT_FRMOVRUN | HCINT_DATATGLERR)) {
            if (out_got) *out_got = got;
            if (ints & (HCINT_STALL | HCINT_XACTERR | HCINT_BBLERR | HCINT_FRMOVRUN | HCINT_DATATGLERR)) {
                hc_halt(ch);
                return -1;
            }
            return 0;
        }
    }

    hc_halt(ch);
    return -1;
#endif
}

/* USB standard requests */
typedef struct __attribute__((packed)) {
    uint8_t bmRequestType;
    uint8_t bRequest;
    uint16_t wValue;
    uint16_t wIndex;
    uint16_t wLength;
} usb_setup_t;

#define REQ_GET_DESCRIPTOR 6u
#define REQ_SET_ADDRESS 5u
#define REQ_SET_CONFIGURATION 9u
#define REQ_GET_STATUS 0u
#define REQ_SET_FEATURE 3u

#define DESC_DEVICE 1u
#define DESC_CONFIG 2u
#define DESC_HUB 0x29u

/* Hub port feature selectors (USB 2.0) */
#define HUB_PORT_RESET 4u
#define HUB_PORT_POWER 8u

/* Hub port status bits (wPortStatus) */
#define PORT_STATUS_CONNECTION (1u << 0)
#define PORT_STATUS_ENABLE     (1u << 1)
#define PORT_STATUS_POWER      (1u << 8)
#define PORT_STATUS_LOW_SPEED  (1u << 9)

static uint16_t le16(const uint8_t *p) {
    return (uint16_t)p[0] | ((uint16_t)p[1] << 8);
}

static int usb_control(usb_setup_t setup, uint8_t *data, uint32_t data_len_inout, uint32_t *out_got) {
    /* Channel 0: control endpoint 0 */
    const uint32_t ch = 0;
    uint8_t setup_bytes[8];
    setup_bytes[0] = setup.bmRequestType;
    setup_bytes[1] = setup.bRequest;
    setup_bytes[2] = (uint8_t)(setup.wValue & 0xFFu);
    setup_bytes[3] = (uint8_t)((setup.wValue >> 8) & 0xFFu);
    setup_bytes[4] = (uint8_t)(setup.wIndex & 0xFFu);
    setup_bytes[5] = (uint8_t)((setup.wIndex >> 8) & 0xFFu);
    setup_bytes[6] = (uint8_t)(setup.wLength & 0xFFu);
    setup_bytes[7] = (uint8_t)((setup.wLength >> 8) & 0xFFu);

    /* Debug: print the control request summary (helps catch garbage SETUP). */
#if USB_KBD_DEBUG
    uart_write("usb-kbd: ctrl req=");
    uart_write_hex_u64(setup_bytes[0]);
    uart_write(" ");
    uart_write_hex_u64(setup_bytes[1]);
    uart_write(" wValue=");
    uart_write_hex_u64((uint64_t)setup.wValue);
    uart_write(" wIndex=");
    uart_write_hex_u64((uint64_t)setup.wIndex);
    uart_write(" wLen=");
    uart_write_hex_u64((uint64_t)setup.wLength);
    uart_write("\n");
#endif

    /* SETUP stage */
    if (dwc2_out_xfer(ch, g.dev_addr, 0, EPTYP_CTRL, 8, g.low_speed, DPID_SETUP, setup_bytes, 8) != 0) return -1;

    /* DATA stage (optional) */
    if (setup.wLength != 0) {
        if (setup.bmRequestType & 0x80u) {
            uint32_t got = 0;
            if (dwc2_in_xfer(ch, g.dev_addr, 0, EPTYP_CTRL, 8, g.low_speed, DPID_DATA1, data, data_len_inout, &got, /*nak_ok=*/0) != 0) return -1;
            if (out_got) *out_got = got;
        } else {
            if (dwc2_out_xfer(ch, g.dev_addr, 0, EPTYP_CTRL, 8, g.low_speed, DPID_DATA1, data, data_len_inout) != 0) return -1;
        }
    }

    /* STATUS stage */
    if (setup.bmRequestType & 0x80u) {
        /* IN data => status OUT */
        if (dwc2_out_xfer(ch, g.dev_addr, 0, EPTYP_CTRL, 8, g.low_speed, DPID_DATA1, 0, 0) != 0) return -1;
    } else {
        /* OUT data => status IN */
        uint8_t z[1];
        uint32_t got = 0;
        if (dwc2_in_xfer(ch, g.dev_addr, 0, EPTYP_CTRL, 8, g.low_speed, DPID_DATA1, z, 0, &got, /*nak_ok=*/0) != 0) return -1;
    }
    return 0;
}

static int usb_hub_get_port_status(uint8_t hub_addr, uint8_t port, uint16_t *out_status, uint16_t *out_change) {
    g.dev_addr = hub_addr;

    uint8_t st[4];
    uint32_t got = 0;
    usb_setup_t req = {
        .bmRequestType = 0xA3u, /* D2H | Class | Other (port) */
        .bRequest = REQ_GET_STATUS,
        .wValue = 0,
        .wIndex = port,
        .wLength = 4,
    };
    if (usb_control(req, st, sizeof(st), &got) != 0 || got < 4) return -1;
    if (out_status) *out_status = le16(&st[0]);
    if (out_change) *out_change = le16(&st[2]);
    return 0;
}

static int usb_hub_set_port_feature(uint8_t hub_addr, uint8_t port, uint16_t feature) {
    g.dev_addr = hub_addr;
    usb_setup_t req = {
        .bmRequestType = 0x23u, /* H2D | Class | Other (port) */
        .bRequest = REQ_SET_FEATURE,
        .wValue = feature,
        .wIndex = port,
        .wLength = 0,
    };
    return usb_control(req, 0, 0, 0);
}

static int usb_hub_get_descriptor(uint8_t hub_addr, uint8_t *out, uint32_t out_len, uint32_t *out_got) {
    if (!out || out_len == 0) return -1;
    g.dev_addr = hub_addr;
    usb_setup_t req = {
        .bmRequestType = 0xA0u, /* D2H | Class | Device */
        .bRequest = REQ_GET_DESCRIPTOR,
        .wValue = (uint16_t)((DESC_HUB << 8) | 0),
        .wIndex = 0,
        .wLength = (uint16_t)out_len,
    };
    uint32_t got = 0;
    if (usb_control(req, out, out_len, &got) != 0) return -1;
    if (out_got) *out_got = got;
    return 0;
}

static int usb_hub_get_num_ports(uint8_t hub_addr, uint8_t *out_ports) {
    uint8_t d[8];
    uint32_t got = 0;
    if (usb_hub_get_descriptor(hub_addr, d, sizeof(d), &got) != 0 || got < 3) return -1;
    uint8_t n = d[2];
    if (n == 0 || n > 15) return -1;
    *out_ports = n;
    return 0;
}

static int usb_hub_power_and_reset_port(uint8_t hub_addr, uint8_t port, int *out_low_speed) {
    if (usb_hub_set_port_feature(hub_addr, port, HUB_PORT_POWER) != 0) return -1;
    udelay_ns(200000000ull);
    if (usb_hub_set_port_feature(hub_addr, port, HUB_PORT_RESET) != 0) return -1;

    uint64_t dl = deadline_ns(2000000000ull);
    while (time_before_deadline(dl)) {
        uint16_t ps = 0, pc = 0;
        if (usb_hub_get_port_status(hub_addr, port, &ps, &pc) != 0) continue;
        if ((ps & PORT_STATUS_CONNECTION) != 0 && (ps & PORT_STATUS_ENABLE) != 0) {
            if (out_low_speed) *out_low_speed = ((ps & PORT_STATUS_LOW_SPEED) != 0);
            return 0;
        }
    }
    return -1;
}

static int usb_enumerate_hid_keyboard_at_addr(uint8_t assigned_addr) {
    /* Device is expected to be in default state at address 0. */
    g.dev_addr = 0;

    uint8_t dev_desc[18];
    uint32_t got = 0;
    usb_setup_t get_dev = {
        .bmRequestType = 0x80u,
        .bRequest = REQ_GET_DESCRIPTOR,
        .wValue = (uint16_t)((DESC_DEVICE << 8) | 0),
        .wIndex = 0,
        .wLength = 18,
    };
    if (usb_control(get_dev, dev_desc, sizeof(dev_desc), &got) != 0 || got < 8) return -1;

    /* Assign address */
    usb_setup_t set_addr = {
        .bmRequestType = 0x00u,
        .bRequest = REQ_SET_ADDRESS,
        .wValue = assigned_addr,
        .wIndex = 0,
        .wLength = 0,
    };
    if (usb_control(set_addr, 0, 0, 0) != 0) return -1;
    g.dev_addr = assigned_addr;
    udelay_ns(5000000ull);

    /* Get config header then full config */
    uint8_t cfg_hdr[9];
    usb_setup_t get_cfg9 = {
        .bmRequestType = 0x80u,
        .bRequest = REQ_GET_DESCRIPTOR,
        .wValue = (uint16_t)((DESC_CONFIG << 8) | 0),
        .wIndex = 0,
        .wLength = 9,
    };
    got = 0;
    if (usb_control(get_cfg9, cfg_hdr, sizeof(cfg_hdr), &got) != 0 || got < 9) return -1;

    uint16_t total_len = le16(&cfg_hdr[2]);
    if (total_len < 9 || total_len > 256) return -1;

    uint8_t cfg[256];
    usb_setup_t get_cfg = get_cfg9;
    get_cfg.wLength = total_len;
    got = 0;
    if (usb_control(get_cfg, cfg, total_len, &got) != 0 || got < total_len) return -1;

    /* Parse for a HID keyboard-like interface and an interrupt IN endpoint.
     * QEMU's usb-kbd behind a hub may not always advertise the strict boot tuple.
     */
    uint8_t intr_ep = 0;
    uint16_t intr_mps = 8;
    int in_hid_if = 0;
    for (uint32_t i = 0; i + 2 < total_len; ) {
        uint8_t bLength = cfg[i];
        uint8_t bDescriptorType = cfg[i + 1];
        if (bLength == 0) break;

        if (bDescriptorType == 4u && bLength >= 9u) {
            uint8_t bInterfaceClass = cfg[i + 5];
            uint8_t bInterfaceSubClass = cfg[i + 6];
            uint8_t bInterfaceProtocol = cfg[i + 7];
            in_hid_if = (bInterfaceClass == 3u);

            (void)bInterfaceSubClass;
            (void)bInterfaceProtocol;

#if USB_KBD_DEBUG
            uart_write("usb-kbd: hid if class=");
            uart_write_hex_u64(bInterfaceClass);
            uart_write(" sub=");
            uart_write_hex_u64(bInterfaceSubClass);
            uart_write(" proto=");
            uart_write_hex_u64(bInterfaceProtocol);
            uart_write("\n");
#endif
        } else if (bDescriptorType == 5u && bLength >= 7u) {
            if (in_hid_if) {
                uint8_t bEndpointAddress = cfg[i + 2];
                uint8_t bmAttributes = cfg[i + 3];
                uint16_t wMaxPacketSize = le16(&cfg[i + 4]);
                uint8_t ep_in = (bEndpointAddress & 0x80u) != 0;
                uint8_t ep_num = bEndpointAddress & 0x0Fu;
                uint8_t ep_type = bmAttributes & 0x3u;

#if USB_KBD_DEBUG
                uart_write("usb-kbd: hid ep addr=");
                uart_write_hex_u64(bEndpointAddress);
                uart_write(" attr=");
                uart_write_hex_u64(bmAttributes);
                uart_write(" mps=");
                uart_write_hex_u64(wMaxPacketSize);
                uart_write("\n");
#endif

                if (ep_in && ep_type == 3u /* interrupt */) {
                    intr_ep = ep_num;
                    intr_mps = wMaxPacketSize;
                    break;
                }
            }
        }

        i += bLength;
    }
    if (intr_ep == 0) return -1;

    g.intr_in_ep = intr_ep;
    g.intr_in_mps = intr_mps;

    usb_setup_t set_cfg = {
        .bmRequestType = 0x00u,
        .bRequest = REQ_SET_CONFIGURATION,
        .wValue = 1,
        .wIndex = 0,
        .wLength = 0,
    };
    if (usb_control(set_cfg, 0, 0, 0) != 0) return -1;
    return 0;
}

static int usb_enumerate_hub_at_addr(uint8_t assigned_addr) {
    /* Device is expected to be in default state at address 0. */
    g.dev_addr = 0;

    uint8_t dev_desc[18];
    uint32_t got = 0;
    usb_setup_t get_dev = {
        .bmRequestType = 0x80u,
        .bRequest = REQ_GET_DESCRIPTOR,
        .wValue = (uint16_t)((DESC_DEVICE << 8) | 0),
        .wIndex = 0,
        .wLength = 18,
    };
    if (usb_control(get_dev, dev_desc, sizeof(dev_desc), &got) != 0 || got < 8) return -1;

    usb_setup_t set_addr = {
        .bmRequestType = 0x00u,
        .bRequest = REQ_SET_ADDRESS,
        .wValue = assigned_addr,
        .wIndex = 0,
        .wLength = 0,
    };
    if (usb_control(set_addr, 0, 0, 0) != 0) return -1;
    g.dev_addr = assigned_addr;
    udelay_ns(5000000ull);

    usb_setup_t set_cfg = {
        .bmRequestType = 0x00u,
        .bRequest = REQ_SET_CONFIGURATION,
        .wValue = 1,
        .wIndex = 0,
        .wLength = 0,
    };
    if (usb_control(set_cfg, 0, 0, 0) != 0) return -1;
    return 0;
}

static int usb_enumerate_keyboard(void) {
    /* Probe the device at address 0 to see if it's a hub (common in QEMU's DWC2 model). */
    g.dev_addr = 0;
    uint8_t dev_desc[18];
    uint32_t got = 0;
    usb_setup_t get_dev = {
        .bmRequestType = 0x80u,
        .bRequest = REQ_GET_DESCRIPTOR,
        .wValue = (uint16_t)((DESC_DEVICE << 8) | 0),
        .wIndex = 0,
        .wLength = 18,
    };
    if (usb_control(get_dev, dev_desc, sizeof(dev_desc), &got) != 0 || got < 8) return -1;

    uint8_t bDeviceClass = dev_desc[4];
#if USB_KBD_DEBUG
    uart_write("usb-kbd: root dev class=");
    uart_write_hex_u64(bDeviceClass);
    uart_write("\n");
#endif

    if (bDeviceClass == 9u) {
        /* Enumerate/configure the hub at address 1, then scan ports to find a HID keyboard.
         * With multiple QEMU USB devices (e.g. usb-net + usb-kbd), the keyboard is not
         * guaranteed to be on port 1.
         */
        const uint8_t hub_addr = 1;
        if (usb_enumerate_hub_at_addr(hub_addr) != 0) return -1;

        uint8_t nports = 0;
        if (usb_hub_get_num_ports(hub_addr, &nports) != 0) {
            /* Best-effort fallback for QEMU: try a small number of ports. */
            nports = 8;
        }

        uint8_t next_addr = 2;
        for (uint8_t port = 1; port <= nports && next_addr < 0x7Fu; port++) {
            uint16_t ps = 0, pc = 0;
            if (usb_hub_get_port_status(hub_addr, port, &ps, &pc) != 0) continue;
            if ((ps & PORT_STATUS_CONNECTION) == 0) continue;

            int dev_ls = 0;
            if (usb_hub_power_and_reset_port(hub_addr, port, &dev_ls) != 0) continue;
            g.low_speed = dev_ls;

        #if USB_KBD_DEBUG
            uart_write("usb-kbd: probe port=");
            uart_write_hex_u64(port);
            uart_write(" speed=");
            uart_write(g.low_speed ? "LS\n" : "FS/HS\n");
        #endif

            /* Downstream device now in default state at address 0. */
            if (usb_enumerate_hid_keyboard_at_addr(next_addr) == 0) return 0;
            next_addr++;
        }
        return -1;
    }

    /* No hub: enumerate the device directly as a HID boot keyboard (address 1). */
    return usb_enumerate_hid_keyboard_at_addr(1);
}

static char hid_keycode_to_ascii(uint8_t keycode, int shift) {
    /* Minimal US layout, enough for shell. */
    if (keycode >= 0x04 && keycode <= 0x1d) {
        char c = (char)('a' + (keycode - 0x04));
        if (shift) c = (char)('A' + (keycode - 0x04));
        return c;
    }
    if (keycode >= 0x1e && keycode <= 0x27) {
        static const char *no = "1234567890";
        static const char *sh = "!@#$%^&*()";
        return shift ? sh[keycode - 0x1e] : no[keycode - 0x1e];
    }
    switch (keycode) {
        case 0x28: return '\n'; /* Enter */
        case 0x2a: return '\b'; /* Backspace */
        case 0x2b: return '\t'; /* Tab */
        case 0x2c: return ' ';
        case 0x2d: return shift ? '_' : '-';
        case 0x2e: return shift ? '+' : '=';
        case 0x2f: return shift ? '{' : '[';
        case 0x30: return shift ? '}' : ']';
        case 0x31: return shift ? '|' : '\\';
        case 0x33: return shift ? ':' : ';';
        case 0x34: return shift ? '"' : '\'';
        case 0x35: return shift ? '~' : '`';
        case 0x36: return shift ? '<' : ',';
        case 0x37: return shift ? '>' : '.';
        case 0x38: return shift ? '?' : '/';
        default: return 0;
    }
}

void usb_kbd_init(void) {
    g.inited = 1;
    g.ready = 0;
    g.dev_addr = 0;
    g.intr_in_ep = 0;
    g.intr_in_mps = 8;
    g.low_speed = 0;
    g.intr_in_pid = DPID_DATA0;
    for (int i = 0; i < 8; i++) g.last_report[i] = 0;

    uart_write("usb-kbd: init (experimental)\n");

    (void)dwc2_core_reset();
    dwc2_force_host_mode();
    dwc2_fifo_init_defaults();
    dwc2_host_configure_fsls_clock();

    dwc2_flush_fifos();

    /* Ensure host channel programming matches the transfer mode we use below. */
    uint32_t ahb = *dwc2_reg(GAHBCFG);
#if USB_KBD_USE_DMA
    ahb |= GAHBCFG_DMAEN;
#else
    ahb &= ~GAHBCFG_DMAEN;
#endif
    ahb |= GAHBCFG_GLBLINTRMSK;
    *dwc2_reg(GAHBCFG) = ahb;

    /* Polled mode: don't rely on interrupts, but clear any pending status anyway. */
    *dwc2_reg(GINTMSK) = 0u;
    *dwc2_reg(GINTSTS) = 0xFFFFFFFFu;

    if (dwc2_host_port_power_and_reset() != 0) {
        uart_write("usb-kbd: no device\n");
        return;
    }

    g.low_speed = dwc2_port_is_low_speed();
    uart_write("usb-kbd: device connected, speed=");
    uart_write(g.low_speed ? "LS\n" : "FS/HS\n");

    if (usb_enumerate_keyboard() != 0) {
        uart_write("usb-kbd: enumerate failed\n");
        return;
    }

    g.intr_in_pid = DPID_DATA0;

    g.ready = 1;
    uart_write("usb-kbd: ready\n");
}

int usb_kbd_is_ready(void) {
    return g.ready;
}

void usb_kbd_poll(void) {
    if (!g.inited || !g.ready) return;

    /* Channel 1: interrupt IN endpoint */
    const uint32_t ch = 1;
    uint8_t report[8];
    uint32_t got = 0;

    if (dwc2_in_xfer(ch, g.dev_addr, g.intr_in_ep, EPTYP_INTR, g.intr_in_mps, g.low_speed, g.intr_in_pid, report, sizeof(report), &got, /*nak_ok=*/1) != 0) {
        return;
    }
    if (got < 8) return;

    /* Successful transaction with data: toggle DATA PID for the next poll. */
    g.intr_in_pid = (g.intr_in_pid == DPID_DATA0) ? DPID_DATA1 : DPID_DATA0;

    /* Report: [mods][0][k1][k2][k3][k4][k5][k6] */
    uint8_t mods = report[0];
    int shift = (mods & 0x22u) != 0; /* LSHIFT or RSHIFT */

    for (int i = 2; i < 8; i++) {
        uint8_t kc = report[i];
        if (kc == 0) continue;

        /* Only emit on newly pressed keycodes (very simple de-bounce). */
        int already = 0;
        for (int j = 2; j < 8; j++) {
            if (g.last_report[j] == kc) { already = 1; break; }
        }
        if (already) continue;

        char c = hid_keycode_to_ascii(kc, shift);
        if (c) {
            console_in_inject_char(c);
        }
    }

    for (int i = 0; i < 8; i++) g.last_report[i] = report[i];
}
