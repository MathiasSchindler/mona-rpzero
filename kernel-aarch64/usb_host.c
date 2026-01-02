#include "usb_host.h"

#include "cache.h"

#include "mmu.h"
#include "time.h"
#include "uart_pl011.h"

/*
 * DWC2 register model (subset).
 * Polled transfers only; no IRQs, no hubs beyond a single hub device.
 */

#define DWC2_BASE (0x3F000000ull + 0x00980000ull)

static inline volatile uint32_t *dwc2_reg(uint32_t off) {
    return (volatile uint32_t *)(uintptr_t)(DWC2_BASE + (uint64_t)off);
}

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

#define HCFG      0x400u
#define HPRT      0x440u

#define HC_BASE   0x500u
#define HC_STRIDE 0x20u
#define HCCHAR(n) (HC_BASE + (uint32_t)(n) * HC_STRIDE + 0x00u)
#define HCSPLT(n) (HC_BASE + (uint32_t)(n) * HC_STRIDE + 0x04u)
#define HCINT(n)  (HC_BASE + (uint32_t)(n) * HC_STRIDE + 0x08u)
#define HCINTMSK(n) (HC_BASE + (uint32_t)(n) * HC_STRIDE + 0x0Cu)
#define HCTSIZ(n) (HC_BASE + (uint32_t)(n) * HC_STRIDE + 0x10u)
#define HCDMA(n)  (HC_BASE + (uint32_t)(n) * HC_STRIDE + 0x14u)

#define DFIFO_BASE 0x1000u
static inline volatile uint32_t *dwc2_fifo(uint32_t n) {
    return (volatile uint32_t *)(uintptr_t)(DWC2_BASE + (uint64_t)DFIFO_BASE + (uint64_t)n * 0x1000ull);
}

#define GRSTCTL_CSRST   (1u << 0)
#define GRSTCTL_RXFFLSH (1u << 4)
#define GRSTCTL_TXFFLSH (1u << 5)
#define GRSTCTL_TXFNUM_SHIFT 6
#define GRSTCTL_AHBIDL  (1u << 31)

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

#define HCCHAR_MPS_MASK 0x7FFu
#define HCCHAR_EPNUM_SHIFT 11
#define HCCHAR_EPDIR (1u << 15)
#define HCCHAR_LSDEV (1u << 17)
#define HCCHAR_EPTYP_SHIFT 18
#define HCCHAR_DEVADDR_SHIFT 22
#define HCCHAR_CHDIS (1u << 30)
#define HCCHAR_CHENA (1u << 31)

#define HCINT_XFERCOMPL (1u << 0)
#define HCINT_CHHLTD    (1u << 1)
#define HCINT_STALL     (1u << 3)
#define HCINT_NAK       (1u << 4)
#define HCINT_XACTERR   (1u << 7)
#define HCINT_BBLERR    (1u << 8)
#define HCINT_FRMOVRUN  (1u << 9)
#define HCINT_DATATGLERR (1u << 10)

#define HCTSIZ_XFERSIZE_MASK 0x7FFFFu
#define HCTSIZ_PKTCNT_SHIFT 19
#define HCTSIZ_DPID_SHIFT 29
#define DPID_DATA0 0u
#define DPID_DATA1 2u
#define DPID_SETUP 3u

/* QEMU's DWC2 model appears to behave best in host DMA mode. */
#define USB_USE_DMA 1

static uint64_t usb_virt_to_phys(const void *p) {
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
    if (dl == 0) return 1;
    return time_now_ns() < dl;
}

static void udelay_ns(uint64_t ns) {
    uint64_t dl = deadline_ns(ns);
    while (time_before_deadline(dl)) {
        /* spin */
    }
}

static int dwc2_core_reset(void) {
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
    *dwc2_reg(GRSTCTL) = GRSTCTL_RXFFLSH;
    uint64_t dl = deadline_ns(20000000ull);
    while (time_before_deadline(dl)) {
        if ((*dwc2_reg(GRSTCTL) & GRSTCTL_RXFFLSH) == 0) break;
    }

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
    *dwc2_reg(GRXFSIZ) = 256u;
    *dwc2_reg(GNPTXFSIZ) = (256u << 16) | 256u;
}

static void dwc2_host_configure_fsls_clock(void) {
    uint32_t hcfg = *dwc2_reg(HCFG);
    hcfg &= ~0x3u;
    hcfg |= 0x1u;
    *dwc2_reg(HCFG) = hcfg;
}

static int dwc2_host_port_power_and_reset(void) {
    uint32_t p = *dwc2_reg(HPRT);
    p |= HPRT_PWR;
    p |= (p & (HPRT_CONNDET | HPRT_ENCHNG | HPRT_OVRCURRCHNG));
    *dwc2_reg(HPRT) = p;

    uint64_t dl = deadline_ns(2000000000ull);
    while (time_before_deadline(dl)) {
        uint32_t s = *dwc2_reg(HPRT);
        if (s & HPRT_CONNSTS) break;
    }
    if (((*dwc2_reg(HPRT)) & HPRT_CONNSTS) == 0) return -1;

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
    return (spd == 1u);
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

static int hc_wait_in_xfer(uint32_t ch, uint64_t timeout_ns, int nak_ok) {
    /* For polled IN endpoints, NAK is an expected “no data yet” state.
     * If nak_ok is set, keep the channel running and clear NAK interrupts so
     * the controller can retry and eventually complete the transfer.
     */
    uint64_t dl = deadline_ns(timeout_ns);
    for (;;) {
        uint32_t ints = *dwc2_reg(HCINT(ch));

        if (ints & (HCINT_STALL | HCINT_XACTERR | HCINT_BBLERR | HCINT_FRMOVRUN | HCINT_DATATGLERR)) {
            return (int)ints;
        }

        if (ints & HCINT_XFERCOMPL) {
            return (int)ints;
        }

        if (ints & HCINT_CHHLTD) {
            /* Halted without completion; treat like an error unless NAK-only polling timed out. */
            return (int)ints;
        }

        if (ints & HCINT_NAK) {
            if (!nak_ok) {
                return (int)ints;
            }
            /* Clear NAK and keep waiting so HW can retry. */
            *dwc2_reg(HCINT(ch)) = HCINT_NAK;
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

static int dwc2_out_xfer(uint32_t ch, uint8_t dev_addr, uint8_t ep, uint8_t ep_type,
                        uint16_t mps, int low_speed, uint32_t pid,
                        const uint8_t *data, uint32_t len) {
    if (len > 0 && !data) return -1;

    hc_clear_ints(ch);
    *dwc2_reg(HCINTMSK(ch)) = 0xFFFFFFFFu;

    uint32_t hcchar = 0;
    hcchar |= (uint32_t)(mps & HCCHAR_MPS_MASK);
    hcchar |= ((uint32_t)ep << HCCHAR_EPNUM_SHIFT);
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

#if USB_USE_DMA
    if (len && data) {
        /* Ensure device sees latest bytes. */
        cache_clean_dcache_for_range((uint64_t)(uintptr_t)data, (uint64_t)len);
    }
    *dwc2_reg(HCDMA(ch)) = (uint32_t)usb_virt_to_phys(data);
#else
    (void)dwc2_fifo;
#endif

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
        return -1;
    }
    if (rc & (HCINT_STALL | HCINT_XACTERR | HCINT_BBLERR | HCINT_FRMOVRUN | HCINT_DATATGLERR)) {
        hc_halt(ch);
        return -1;
    }
    return 0;
}

static int dwc2_in_xfer(uint32_t ch, uint8_t dev_addr, uint8_t ep, uint8_t ep_type,
                       uint16_t mps, int low_speed, uint32_t pid,
                       uint8_t *out, uint32_t len, uint32_t *out_got, int nak_ok) {
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

#if USB_USE_DMA
    if (len && out) {
        /* Drop any stale cache lines before device DMA writes into this buffer. */
        cache_invalidate_dcache_for_range((uint64_t)(uintptr_t)out, (uint64_t)len);
    }
    *dwc2_reg(HCDMA(ch)) = (uint32_t)usb_virt_to_phys(out);
#endif

    hcchar = *dwc2_reg(HCCHAR(ch));
    hcchar |= HCCHAR_CHENA;
    hcchar &= ~HCCHAR_CHDIS;
    *dwc2_reg(HCCHAR(ch)) = hcchar;

    /* If polling and NAKs are allowed, wait only briefly.
     * This gives the controller time to retry after NAK without stalling the kernel.
     */
    uint64_t wait_ns = nak_ok ? 2000000ull : 200000000ull;
    int rc = hc_wait_in_xfer(ch, wait_ns, nak_ok);
    if (rc < 0) {
        hc_halt(ch);
        if (out_got) *out_got = 0;
        return nak_ok ? 0 : -1;
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

    uint32_t rem = (*dwc2_reg(HCTSIZ(ch))) & HCTSIZ_XFERSIZE_MASK;
    uint32_t got = (len >= rem) ? (len - rem) : 0;
    if (out_got) *out_got = got;
    return 0;
}

/* USB standard requests */
#define REQ_GET_DESCRIPTOR 6u
#define REQ_SET_ADDRESS 5u
#define REQ_SET_CONFIGURATION 9u
#define REQ_SET_INTERFACE 11u
#define REQ_GET_STATUS 0u
#define REQ_SET_FEATURE 3u

#define DESC_DEVICE 1u
#define DESC_CONFIG 2u
#define DESC_STRING 3u
#define DESC_HUB 0x29u

#define HUB_PORT_RESET 4u
#define HUB_PORT_POWER 8u

#define PORT_STATUS_CONNECTION (1u << 0)
#define PORT_STATUS_ENABLE     (1u << 1)
#define PORT_STATUS_LOW_SPEED  (1u << 9)

static uint16_t le16(const uint8_t *p) {
    return (uint16_t)p[0] | ((uint16_t)p[1] << 8);
}

int usb_host_control_xfer(uint8_t dev_addr, int low_speed, usb_setup_t setup,
                          uint8_t *data, uint32_t data_len_inout, uint32_t *out_got) {
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

    if (dwc2_out_xfer(ch, dev_addr, 0, USB_EPTYP_CTRL, 8, low_speed, DPID_SETUP, setup_bytes, 8) != 0) return -1;

    if (setup.wLength != 0) {
        if (setup.bmRequestType & 0x80u) {
            uint32_t got = 0;
            if (dwc2_in_xfer(ch, dev_addr, 0, USB_EPTYP_CTRL, 8, low_speed, DPID_DATA1,
                             data, data_len_inout, &got, /*nak_ok=*/0) != 0) return -1;
            if (out_got) *out_got = got;
        } else {
            if (dwc2_out_xfer(ch, dev_addr, 0, USB_EPTYP_CTRL, 8, low_speed, DPID_DATA1, data, data_len_inout) != 0) return -1;
        }
    }

    if (setup.bmRequestType & 0x80u) {
        if (dwc2_out_xfer(ch, dev_addr, 0, USB_EPTYP_CTRL, 8, low_speed, DPID_DATA1, 0, 0) != 0) return -1;
    } else {
        uint8_t z[1];
        uint32_t got = 0;
        if (dwc2_in_xfer(ch, dev_addr, 0, USB_EPTYP_CTRL, 8, low_speed, DPID_DATA1, z, 0, &got, /*nak_ok=*/0) != 0) return -1;
    }

    return 0;
}

int usb_host_out_xfer(uint8_t dev_addr, int low_speed, usb_ep_t ep, uint32_t pid,
                      const uint8_t *data, uint32_t len) {
    /* Use channel 2 for OUT transfers to avoid clobbering control (0) and common IN (1). */
    const uint32_t ch = 2;
    return dwc2_out_xfer(ch, dev_addr, ep.ep_num, ep.ep_type, ep.mps, low_speed, pid, data, len);
}

int usb_host_in_xfer(uint8_t dev_addr, int low_speed, usb_ep_t ep, uint32_t pid,
                     uint8_t *out, uint32_t len, uint32_t *out_got, int nak_ok) {
    const uint32_t ch = 1;
    return dwc2_in_xfer(ch, dev_addr, ep.ep_num, ep.ep_type, ep.mps, low_speed, pid, out, len, out_got, nak_ok);
}

int usb_host_set_interface(uint8_t dev_addr, int low_speed, uint8_t if_num, uint8_t alt_setting) {
    usb_setup_t req = {
        .bmRequestType = 0x01u, /* H2D | Standard | Interface */
        .bRequest = REQ_SET_INTERFACE,
        .wValue = alt_setting,
        .wIndex = if_num,
        .wLength = 0,
    };
    return usb_host_control_xfer(dev_addr, low_speed, req, 0, 0, 0);
}

static int is_printable_ascii(char c) {
    return (c >= 0x20 && c <= 0x7eu);
}

int usb_host_get_string_ascii(uint8_t dev_addr, int low_speed, uint8_t str_index,
                              char *out, size_t out_len) {
    if (!out || out_len == 0) return -1;
    out[0] = 0;
    if (str_index == 0) return -1;

    uint8_t buf[64];
    uint32_t got = 0;

    /* Try common English (US) language ID; QEMU typically accepts this.
     * If that fails, retry with wIndex=0.
     */
    uint16_t lang = 0x0409u;
    for (int attempt = 0; attempt < 2; attempt++) {
        usb_setup_t req = {
            .bmRequestType = 0x80u,
            .bRequest = REQ_GET_DESCRIPTOR,
            .wValue = (uint16_t)((DESC_STRING << 8) | str_index),
            .wIndex = lang,
            .wLength = (uint16_t)sizeof(buf),
        };

        got = 0;
        if (usb_host_control_xfer(dev_addr, low_speed, req, buf, sizeof(buf), &got) == 0 && got >= 2) {
            break;
        }
        lang = 0;
    }

    if (got < 2 || buf[1] != DESC_STRING) return -1;

    uint8_t bLength = buf[0];
    if (bLength > got) bLength = (uint8_t)got;
    if (bLength < 2) return -1;

    size_t w = 0;
    for (uint32_t i = 2; i + 1 < bLength && w + 1 < out_len; i += 2) {
        char c = (char)buf[i];
        if (!is_printable_ascii(c)) {
            /* Keep going; some strings include separators.
             * We only emit printable ASCII.
             */
            continue;
        }
        out[w++] = c;
    }
    out[w] = 0;
    return (w > 0) ? 0 : -1;
}

static int usb_hub_get_port_status(uint8_t hub_addr, uint8_t port, uint16_t *out_status, uint16_t *out_change) {
    uint8_t st[4];
    uint32_t got = 0;
    usb_setup_t req = {
        .bmRequestType = 0xA3u,
        .bRequest = REQ_GET_STATUS,
        .wValue = 0,
        .wIndex = port,
        .wLength = 4,
    };
    if (usb_host_control_xfer(hub_addr, /*low_speed=*/0, req, st, sizeof(st), &got) != 0 || got < 4) return -1;
    if (out_status) *out_status = le16(&st[0]);
    if (out_change) *out_change = le16(&st[2]);
    return 0;
}

static int usb_hub_set_port_feature(uint8_t hub_addr, uint8_t port, uint16_t feature) {
    usb_setup_t req = {
        .bmRequestType = 0x23u,
        .bRequest = REQ_SET_FEATURE,
        .wValue = feature,
        .wIndex = port,
        .wLength = 0,
    };
    return usb_host_control_xfer(hub_addr, /*low_speed=*/0, req, 0, 0, 0);
}

static int usb_hub_get_num_ports(uint8_t hub_addr, uint8_t *out_ports) {
    uint8_t d[8];
    uint32_t got = 0;
    usb_setup_t req = {
        .bmRequestType = 0xA0u,
        .bRequest = REQ_GET_DESCRIPTOR,
        .wValue = (uint16_t)((DESC_HUB << 8) | 0),
        .wIndex = 0,
        .wLength = (uint16_t)sizeof(d),
    };
    if (usb_host_control_xfer(hub_addr, /*low_speed=*/0, req, d, sizeof(d), &got) != 0 || got < 3) return -1;
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

static int usb_get_device_descriptor(uint8_t dev_addr, int low_speed, uint8_t *out18) {
    uint32_t got = 0;
    usb_setup_t get_dev = {
        .bmRequestType = 0x80u,
        .bRequest = REQ_GET_DESCRIPTOR,
        .wValue = (uint16_t)((DESC_DEVICE << 8) | 0),
        .wIndex = 0,
        .wLength = 18,
    };
    if (usb_host_control_xfer(dev_addr, low_speed, get_dev, out18, 18, &got) != 0 || got < 8) return -1;
    return 0;
}

static int usb_get_config_blob(uint8_t dev_addr, int low_speed, uint8_t *buf, uint32_t buf_len, uint16_t *out_total, uint8_t *out_cfg_value) {
    uint8_t hdr[9];
    uint32_t got = 0;
    usb_setup_t get_cfg9 = {
        .bmRequestType = 0x80u,
        .bRequest = REQ_GET_DESCRIPTOR,
        .wValue = (uint16_t)((DESC_CONFIG << 8) | 0),
        .wIndex = 0,
        .wLength = 9,
    };
    if (usb_host_control_xfer(dev_addr, low_speed, get_cfg9, hdr, sizeof(hdr), &got) != 0 || got < 9) return -1;

    uint16_t total_len = le16(&hdr[2]);
    if (total_len < 9) return -1;

    /* bConfigurationValue at offset 5. */
    uint8_t cfg_value = hdr[5];

    uint16_t req_len = total_len;
    if (req_len > buf_len) req_len = (uint16_t)buf_len;

    usb_setup_t get_cfg = get_cfg9;
    get_cfg.wLength = req_len;
    got = 0;
    if (usb_host_control_xfer(dev_addr, low_speed, get_cfg, buf, req_len, &got) != 0 || got < 9) return -1;

    if (out_total) *out_total = total_len;
    if (out_cfg_value) *out_cfg_value = cfg_value;
    return 0;
}

static int usb_set_address(uint8_t assigned_addr) {
    usb_setup_t set_addr = {
        .bmRequestType = 0x00u,
        .bRequest = REQ_SET_ADDRESS,
        .wValue = assigned_addr,
        .wIndex = 0,
        .wLength = 0,
    };
    if (usb_host_control_xfer(/*dev_addr=*/0, /*low_speed=*/0, set_addr, 0, 0, 0) != 0) return -1;
    udelay_ns(5000000ull);
    return 0;
}

static int usb_set_configuration(uint8_t dev_addr, int low_speed, uint8_t cfg_value) {
    usb_setup_t set_cfg = {
        .bmRequestType = 0x00u,
        .bRequest = REQ_SET_CONFIGURATION,
        .wValue = cfg_value,
        .wIndex = 0,
        .wLength = 0,
    };
    return usb_host_control_xfer(dev_addr, low_speed, set_cfg, 0, 0, 0);
}

int usb_host_init(void) {
    (void)dwc2_reg(GOTGCTL);

    (void)dwc2_core_reset();
    dwc2_force_host_mode();
    dwc2_fifo_init_defaults();
    dwc2_host_configure_fsls_clock();
    dwc2_flush_fifos();

    uint32_t ahb = *dwc2_reg(GAHBCFG);
#if USB_USE_DMA
    ahb |= GAHBCFG_DMAEN;
#else
    ahb &= ~GAHBCFG_DMAEN;
#endif
    ahb |= GAHBCFG_GLBLINTRMSK;
    *dwc2_reg(GAHBCFG) = ahb;

    *dwc2_reg(GINTMSK) = 0u;
    *dwc2_reg(GINTSTS) = 0xFFFFFFFFu;

    if (dwc2_host_port_power_and_reset() != 0) {
        return -1;
    }

    return 0;
}

/* Enumerate a single device that is currently in default state at address 0.
 * Assigns it `addr`, fetches descriptors, sets configuration.
 */
static int usb_enumerate_device_at_default(uint8_t addr, int low_speed, usb_device_t *out) {
    if (!out) return -1;

    /* Assign address (device is at 0). */
    if (usb_set_address(addr) != 0) return -1;

    out->addr = addr;
    out->low_speed = (uint8_t)(low_speed ? 1 : 0);

    if (usb_get_device_descriptor(addr, low_speed, out->dev_desc) != 0) return -1;

    uint16_t total = 0;
    uint8_t cfg_value = 0;
    if (usb_get_config_blob(addr, low_speed, out->cfg, sizeof(out->cfg), &total, &cfg_value) != 0) return -1;

    out->cfg_value = cfg_value;
    out->cfg_len = (total > sizeof(out->cfg)) ? (uint16_t)sizeof(out->cfg) : total;

    if (usb_set_configuration(addr, low_speed, cfg_value) != 0) return -1;

    return 0;
}

static void usb_dev_copy(usb_device_t *dst, const usb_device_t *src) {
    if (!dst || !src) return;
    dst->addr = src->addr;
    dst->low_speed = src->low_speed;
    for (int i = 0; i < 18; i++) dst->dev_desc[i] = src->dev_desc[i];
    for (int i = 0; i < (int)sizeof(dst->cfg); i++) dst->cfg[i] = src->cfg[i];
    dst->cfg_len = src->cfg_len;
    dst->cfg_value = src->cfg_value;
}

int usb_host_enumerate(usb_device_t *out_devs, int max_devs) {
    if (!out_devs || max_devs <= 0) return 0;

    /* Root device at addr 0 should be present due to usb_host_init.
     * Determine whether it is a hub.
     */
    uint8_t root_desc[18];
    int root_low = dwc2_port_is_low_speed();
    if (usb_get_device_descriptor(/*dev_addr=*/0, root_low, root_desc) != 0) return 0;

    uint8_t root_class = root_desc[4];
    if (root_class != 9u) {
        /* No hub: enumerate single device as addr 1. */
        usb_device_t d;
        if (usb_enumerate_device_at_default(1, root_low, &d) != 0) return 0;
        usb_dev_copy(&out_devs[0], &d);
        return 1;
    }

    /* Root hub: enumerate hub itself as addr 1, then scan downstream ports for devices. */
    usb_device_t hub;
    if (usb_enumerate_device_at_default(1, /*low_speed=*/0, &hub) != 0) return 0;

    /* Hub is now at addr 1. */
    uint8_t nports = 0;
    if (usb_hub_get_num_ports(1, &nports) != 0) {
        nports = 8;
    }

    int dev_count = 0;
    uint8_t next_addr = 2;

    for (uint8_t port = 1; port <= nports && dev_count < max_devs; port++) {
        uint16_t ps = 0, pc = 0;
        if (usb_hub_get_port_status(1, port, &ps, &pc) != 0) continue;
        if ((ps & PORT_STATUS_CONNECTION) == 0) continue;

        int dev_ls = 0;
        if (usb_hub_power_and_reset_port(1, port, &dev_ls) != 0) continue;

        usb_device_t d;
        if (usb_enumerate_device_at_default(next_addr, dev_ls, &d) != 0) {
            next_addr++;
            continue;
        }

        usb_dev_copy(&out_devs[dev_count++], &d);
        next_addr++;
    }

    return dev_count;
}

int usb_host_find_hid_kbd_intr_in(const usb_device_t *dev, usb_ep_t *out_intr_in) {
    if (!dev || !out_intr_in) return -1;
    if (dev->cfg_len < 9) return -1;

    int in_hid_if = 0;
    for (uint32_t i = 0; i + 2 < dev->cfg_len; ) {
        uint8_t bLength = dev->cfg[i];
        uint8_t bDescriptorType = dev->cfg[i + 1];
        if (bLength == 0) break;

        if (bDescriptorType == 4u && bLength >= 9u) {
            uint8_t bInterfaceClass = dev->cfg[i + 5];
            in_hid_if = (bInterfaceClass == 3u);
        } else if (bDescriptorType == 5u && bLength >= 7u) {
            if (in_hid_if) {
                uint8_t bEndpointAddress = dev->cfg[i + 2];
                uint8_t bmAttributes = dev->cfg[i + 3];
                uint16_t wMaxPacketSize = le16(&dev->cfg[i + 4]);
                uint8_t ep_in = (bEndpointAddress & 0x80u) != 0;
                uint8_t ep_num = bEndpointAddress & 0x0Fu;
                uint8_t ep_type = bmAttributes & 0x3u;

                if (ep_in && ep_type == USB_EPTYP_INTR) {
                    out_intr_in->ep_num = ep_num;
                    out_intr_in->ep_type = USB_EPTYP_INTR;
                    out_intr_in->ep_in = 1;
                    out_intr_in->mps = wMaxPacketSize;
                    return 0;
                }
            }
        }

        i += bLength;
    }

    return -1;
}

int usb_host_find_bulk_in_out_pair(const usb_device_t *dev, usb_ep_t *out_in, usb_ep_t *out_out) {
    if (!dev || !out_in || !out_out) return -1;

    /* Best-effort heuristic: look for an interface, then within it pick first BULK IN and BULK OUT.
     * Prefer CDC data interface class 0x0A, but accept anything for now.
     */
    int in_if = 0;
    int preferred_if = 0;
    usb_ep_t bulk_in = {0};
    usb_ep_t bulk_out = {0};

    for (uint32_t i = 0; i + 2 < dev->cfg_len; ) {
        uint8_t bLength = dev->cfg[i];
        uint8_t bDescriptorType = dev->cfg[i + 1];
        if (bLength == 0) break;

        if (bDescriptorType == 4u && bLength >= 9u) {
            uint8_t bInterfaceClass = dev->cfg[i + 5];
            in_if = 1;
            preferred_if = (bInterfaceClass == 0x0Au);
            bulk_in.ep_num = 0;
            bulk_out.ep_num = 0;
        } else if (bDescriptorType == 5u && bLength >= 7u) {
            if (in_if) {
                uint8_t bEndpointAddress = dev->cfg[i + 2];
                uint8_t bmAttributes = dev->cfg[i + 3];
                uint16_t wMaxPacketSize = le16(&dev->cfg[i + 4]);
                uint8_t ep_in = (bEndpointAddress & 0x80u) != 0;
                uint8_t ep_num = bEndpointAddress & 0x0Fu;
                uint8_t ep_type = bmAttributes & 0x3u;

                if (ep_type == USB_EPTYP_BULK) {
                    if (ep_in && bulk_in.ep_num == 0) {
                        bulk_in.ep_num = ep_num;
                        bulk_in.ep_type = USB_EPTYP_BULK;
                        bulk_in.ep_in = 1;
                        bulk_in.mps = wMaxPacketSize;
                    }
                    if (!ep_in && bulk_out.ep_num == 0) {
                        bulk_out.ep_num = ep_num;
                        bulk_out.ep_type = USB_EPTYP_BULK;
                        bulk_out.ep_in = 0;
                        bulk_out.mps = wMaxPacketSize;
                    }
                }

                if (bulk_in.ep_num != 0 && bulk_out.ep_num != 0) {
                    /* If this is a preferred interface, accept immediately; otherwise keep searching
                     * but accept the first found pair.
                     */
                    *out_in = bulk_in;
                    *out_out = bulk_out;
                    if (preferred_if) return 0;
                    return 0;
                }
            }
        }

        i += bLength;
    }

    return -1;
}
