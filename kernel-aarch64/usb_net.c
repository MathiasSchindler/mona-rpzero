#include "usb_net.h"

#include "net.h"
#include "time.h"
#include "uart_pl011.h"

/*
 * Minimal USB "Ethernet-like" device support.
 *
 * This is intentionally best-effort: we look for a BULK IN + BULK OUT endpoint pair
 * and treat bulk payloads as raw Ethernet frames.
 *
 * Works for CDC-ECM-like devices; may not work for RNDIS/NCM.
 */

typedef struct {
    int bound;
    uint8_t addr;
    int low_speed;
    usb_ep_t ep_in;
    usb_ep_t ep_out;
    uint32_t in_pid;
    uint32_t out_pid;

    int has_ctrl_if;
    uint8_t ctrl_if_num;

    enum {
        USBNET_MODE_RAW_BULK = 0,
        USBNET_MODE_RNDIS = 1,
    } mode;

    uint32_t rndis_request_id;

    netif_t nif;
} usb_net_state_t;

static usb_net_state_t g_usbnet;

static int usbnet_tx_frame(netif_t *nif, const uint8_t *frame, size_t len);

static const netif_ops_t g_usbnet_ops = {
    .tx_frame = usbnet_tx_frame,
};

static int usbnet_tx_frame(netif_t *nif, const uint8_t *frame, size_t len) {
    (void)nif;
    if (!g_usbnet.bound) return -1;

    /* Simple MTU sanity: Ethernet frame is typically <= 1514 (+ VLAN). Allow a bit more. */
    if (len == 0 || len > 2048) return -1;

    const uint8_t *payload = frame;
    uint32_t payload_len = (uint32_t)len;
    uint8_t rndis_buf[2048 + 64];

    if (g_usbnet.mode == USBNET_MODE_RNDIS) {
        /* Wrap the Ethernet frame in a RNDIS_PACKET_MSG. */
        typedef struct __attribute__((packed)) {
            uint32_t msg_type;
            uint32_t msg_len;
            uint32_t data_offset;
            uint32_t data_len;
            uint32_t oob_offset;
            uint32_t oob_len;
            uint32_t num_oob;
            uint32_t pkt_offset;
            uint32_t pkt_len;
            uint32_t vc_handle;
            uint32_t reserved;
        } rndis_packet_msg_t;

        if (len + sizeof(rndis_packet_msg_t) > sizeof(rndis_buf)) return -1;
        rndis_packet_msg_t *p = (rndis_packet_msg_t *)rndis_buf;
        p->msg_type = 0x00000001u;
        p->msg_len = (uint32_t)(sizeof(rndis_packet_msg_t) + len);
        p->data_offset = (uint32_t)(sizeof(rndis_packet_msg_t) - 8u);
        p->data_len = (uint32_t)len;
        p->oob_offset = 0;
        p->oob_len = 0;
        p->num_oob = 0;
        p->pkt_offset = 0;
        p->pkt_len = 0;
        p->vc_handle = 0;
        p->reserved = 0;

        for (uint32_t i = 0; i < (uint32_t)len; i++) {
            rndis_buf[sizeof(rndis_packet_msg_t) + i] = frame[i];
        }
        payload = rndis_buf;
        payload_len = p->msg_len;
    }

    int rc = usb_host_out_xfer(g_usbnet.addr, g_usbnet.low_speed, g_usbnet.ep_out, g_usbnet.out_pid, payload, payload_len);
    if (rc == 0) {
        g_usbnet.out_pid = (g_usbnet.out_pid == USB_PID_DATA0) ? USB_PID_DATA1 : USB_PID_DATA0;
    }
    return rc;
}

static int hex_nibble(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return 10 + (c - 'a');
    if (c >= 'A' && c <= 'F') return 10 + (c - 'A');
    return -1;
}

static int parse_mac_from_hexstr(const char *s, uint8_t out_mac[6]) {
    /* Expect 12 hex chars (e.g. "525400123456"). */
    if (!s || !out_mac) return -1;
    for (int i = 0; i < 6; i++) {
        char c0 = s[i * 2 + 0];
        char c1 = s[i * 2 + 1];
        int hi = hex_nibble(c0);
        int lo = hex_nibble(c1);
        if (hi < 0 || lo < 0) return -1;
        out_mac[i] = (uint8_t)((hi << 4) | lo);
    }
    return 0;
}

static uint16_t dev_vid(const usb_device_t *dev) {
    return (uint16_t)dev->dev_desc[8] | ((uint16_t)dev->dev_desc[9] << 8);
}

static uint16_t dev_pid(const usb_device_t *dev) {
    return (uint16_t)dev->dev_desc[10] | ((uint16_t)dev->dev_desc[11] << 8);
}

static void usbnet_delay_ns(uint64_t ns) {
    uint64_t now = time_now_ns();
    if (now == 0) {
        for (volatile uint64_t i = 0; i < 200000; i++) {
            /* best-effort */
        }
        return;
    }
    uint64_t dl = now + ns;
    while (time_now_ns() < dl) {
        /* spin */
    }
}

/* --- RNDIS control plane (for QEMU usb-net 0525:a4a2) --- */

#define RNDIS_SEND_ENCAPSULATED_COMMAND 0x00u
#define RNDIS_GET_ENCAPSULATED_RESPONSE 0x01u

#define RNDIS_MSG_INITIALIZE      0x00000002u
#define RNDIS_MSG_INITIALIZE_CMPL 0x80000002u
#define RNDIS_MSG_QUERY           0x00000004u
#define RNDIS_MSG_QUERY_CMPL      0x80000004u
#define RNDIS_MSG_SET             0x00000005u
#define RNDIS_MSG_SET_CMPL        0x80000005u

#define RNDIS_STATUS_SUCCESS 0x00000000u

#define OID_GEN_CURRENT_PACKET_FILTER 0x0001010Eu
#define OID_802_3_CURRENT_ADDRESS     0x01010102u

typedef struct __attribute__((packed)) {
    uint32_t msg_type;
    uint32_t msg_len;
} rndis_hdr_t;

typedef struct __attribute__((packed)) {
    uint32_t msg_type;
    uint32_t msg_len;
    uint32_t request_id;
    uint32_t major;
    uint32_t minor;
    uint32_t max_xfer_size;
} rndis_init_msg_t;

typedef struct __attribute__((packed)) {
    uint32_t msg_type;
    uint32_t msg_len;
    uint32_t request_id;
    uint32_t status;
    uint32_t major;
    uint32_t minor;
    uint32_t device_flags;
    uint32_t medium;
    uint32_t max_packets_per_xfer;
    uint32_t max_xfer_size;
    uint32_t packet_alignment;
    uint32_t af_list_offset;
    uint32_t af_list_size;
} rndis_init_cmplt_t;

typedef struct __attribute__((packed)) {
    uint32_t msg_type;
    uint32_t msg_len;
    uint32_t request_id;
    uint32_t oid;
    uint32_t info_len;
    uint32_t info_offset;
    uint32_t dev_vc_handle;
} rndis_query_msg_t;

typedef struct __attribute__((packed)) {
    uint32_t msg_type;
    uint32_t msg_len;
    uint32_t request_id;
    uint32_t status;
    uint32_t info_len;
    uint32_t info_offset;
} rndis_query_cmplt_t;

typedef struct __attribute__((packed)) {
    uint32_t msg_type;
    uint32_t msg_len;
    uint32_t request_id;
    uint32_t oid;
    uint32_t info_len;
    uint32_t info_offset;
    uint32_t dev_vc_handle;
} rndis_set_msg_t;

typedef struct __attribute__((packed)) {
    uint32_t msg_type;
    uint32_t msg_len;
    uint32_t request_id;
    uint32_t status;
} rndis_set_cmplt_t;

typedef struct __attribute__((packed)) {
    uint32_t msg_type;
    uint32_t msg_len;
    uint32_t data_offset;
    uint32_t data_len;
    uint32_t oob_offset;
    uint32_t oob_len;
    uint32_t num_oob;
    uint32_t pkt_offset;
    uint32_t pkt_len;
    uint32_t vc_handle;
    uint32_t reserved;
} rndis_packet_msg_t;

static int rndis_send_cmd(uint8_t dev_addr, int low_speed, uint8_t ctrl_if, const void *buf, uint16_t len) {
    usb_setup_t req = {
        .bmRequestType = 0x21u, /* H2D | Class | Interface */
        .bRequest = RNDIS_SEND_ENCAPSULATED_COMMAND,
        .wValue = 0,
        .wIndex = ctrl_if,
        .wLength = len,
    };
    return usb_host_control_xfer(dev_addr, low_speed, req, (uint8_t *)buf, len, 0);
}

static int rndis_get_resp(uint8_t dev_addr, int low_speed, uint8_t ctrl_if, void *buf, uint16_t buf_len, uint32_t *out_got) {
    usb_setup_t req = {
        .bmRequestType = 0xA1u, /* D2H | Class | Interface */
        .bRequest = RNDIS_GET_ENCAPSULATED_RESPONSE,
        .wValue = 0,
        .wIndex = ctrl_if,
        .wLength = buf_len,
    };
    uint32_t got = 0;
    int rc = usb_host_control_xfer(dev_addr, low_speed, req, (uint8_t *)buf, buf_len, &got);
    if (out_got) *out_got = got;
    return rc;
}

static int rndis_wait_for_msg(uint8_t dev_addr, int low_speed, uint8_t ctrl_if,
                              uint32_t expect_type, void *buf, uint16_t buf_len, uint32_t *out_got) {
    for (int tries = 0; tries < 50; tries++) {
        uint32_t got = 0;
        if (rndis_get_resp(dev_addr, low_speed, ctrl_if, buf, buf_len, &got) == 0 && got >= sizeof(rndis_hdr_t)) {
            rndis_hdr_t *h = (rndis_hdr_t *)buf;
            if (h->msg_type == expect_type) {
                if (out_got) *out_got = got;
                return 0;
            }
        }
        usbnet_delay_ns(2000000ull);
    }
    return -1;
}

static int rndis_initialize(usb_net_state_t *st) {
    rndis_init_msg_t m;
    m.msg_type = RNDIS_MSG_INITIALIZE;
    m.msg_len = (uint32_t)sizeof(m);
    m.request_id = ++st->rndis_request_id;
    m.major = 1;
    m.minor = 0;
    m.max_xfer_size = 2048;

    if (rndis_send_cmd(st->addr, st->low_speed, st->ctrl_if_num, &m, (uint16_t)sizeof(m)) != 0) return -1;

    rndis_init_cmplt_t resp;
    uint32_t got = 0;
    if (rndis_wait_for_msg(st->addr, st->low_speed, st->ctrl_if_num, RNDIS_MSG_INITIALIZE_CMPL, &resp, (uint16_t)sizeof(resp), &got) != 0) return -1;
    if (got < sizeof(resp)) return -1;
    if (resp.request_id != m.request_id) return -1;
    if (resp.status != RNDIS_STATUS_SUCCESS) return -1;
    return 0;
}

static int rndis_set_packet_filter(usb_net_state_t *st, uint32_t filter) {
    struct __attribute__((packed)) {
        rndis_set_msg_t hdr;
        uint32_t value;
    } m;

    m.hdr.msg_type = RNDIS_MSG_SET;
    m.hdr.msg_len = (uint32_t)sizeof(m);
    m.hdr.request_id = ++st->rndis_request_id;
    m.hdr.oid = OID_GEN_CURRENT_PACKET_FILTER;
    m.hdr.info_len = 4;
    m.hdr.info_offset = 20; /* sizeof(rndis_set_msg_t)=28 => 28-8 */
    m.hdr.dev_vc_handle = 0;
    m.value = filter;

    if (rndis_send_cmd(st->addr, st->low_speed, st->ctrl_if_num, &m, (uint16_t)sizeof(m)) != 0) return -1;

    rndis_set_cmplt_t resp;
    uint32_t got = 0;
    if (rndis_wait_for_msg(st->addr, st->low_speed, st->ctrl_if_num, RNDIS_MSG_SET_CMPL, &resp, (uint16_t)sizeof(resp), &got) != 0) return -1;
    if (got < sizeof(resp)) return -1;
    if (resp.request_id != m.hdr.request_id) return -1;
    if (resp.status != RNDIS_STATUS_SUCCESS) return -1;
    return 0;
}

static int rndis_query(usb_net_state_t *st, uint32_t oid, void *out, uint32_t out_len, uint32_t *out_got) {
    rndis_query_msg_t m;
    m.msg_type = RNDIS_MSG_QUERY;
    m.msg_len = (uint32_t)sizeof(m);
    m.request_id = ++st->rndis_request_id;
    m.oid = oid;
    m.info_len = 0;
    m.info_offset = 0;
    m.dev_vc_handle = 0;

    if (rndis_send_cmd(st->addr, st->low_speed, st->ctrl_if_num, &m, (uint16_t)sizeof(m)) != 0) return -1;

    uint8_t respbuf[128];
    uint32_t got = 0;
    if (rndis_wait_for_msg(st->addr, st->low_speed, st->ctrl_if_num, RNDIS_MSG_QUERY_CMPL, respbuf, (uint16_t)sizeof(respbuf), &got) != 0) return -1;
    if (got < sizeof(rndis_query_cmplt_t)) return -1;

    rndis_query_cmplt_t *r = (rndis_query_cmplt_t *)respbuf;
    if (r->request_id != m.request_id) return -1;
    if (r->status != RNDIS_STATUS_SUCCESS) return -1;

    uint32_t info_len = r->info_len;
    uint32_t info_off = r->info_offset;
    uint8_t *info = respbuf + 8u + info_off;
    if ((uint64_t)(info - respbuf) + info_len > got) return -1;

    uint32_t n = info_len;
    if (n > out_len) n = out_len;
    for (uint32_t i = 0; i < n; i++) ((uint8_t *)out)[i] = info[i];
    if (out_got) *out_got = n;
    return 0;
}

typedef struct {
    int ok;
    uint8_t ctrl_if;
    uint8_t data_if;
    uint8_t data_alt;
    uint8_t mac_str_index;
    usb_ep_t bulk_in;
    usb_ep_t bulk_out;
} cdc_ecm_info_t;

static uint16_t le16(const uint8_t *p) {
    return (uint16_t)p[0] | ((uint16_t)p[1] << 8);
}

static int parse_cdc_ecm(const usb_device_t *dev, cdc_ecm_info_t *out) {
    if (!dev || !out) return -1;
    out->ok = 0;
    out->ctrl_if = 0;
    out->data_if = 0;
    out->data_alt = 0;
    out->mac_str_index = 0;
    out->bulk_in.ep_num = 0;
    out->bulk_out.ep_num = 0;

    /* Descriptor types. */
    const uint8_t DT_INTERFACE = 4u;
    const uint8_t DT_ENDPOINT = 5u;
    const uint8_t DT_CS_INTERFACE = 0x24u;

    int in_ctrl_if = 0;
    int in_data_if = 0;
    uint8_t cur_if = 0;
    uint8_t cur_alt = 0;

    usb_ep_t tmp_in = {0};
    usb_ep_t tmp_out = {0};
    uint8_t tmp_data_if = 0;
    uint8_t tmp_data_alt = 0;

    for (uint32_t i = 0; i + 2 < dev->cfg_len; ) {
        uint8_t bLength = dev->cfg[i];
        uint8_t bDescriptorType = dev->cfg[i + 1];
        if (bLength == 0) break;

        if (bDescriptorType == DT_INTERFACE && bLength >= 9u) {
            cur_if = dev->cfg[i + 2];
            cur_alt = dev->cfg[i + 3];
            uint8_t cls = dev->cfg[i + 5];
            /* CDC Control = 0x02, CDC Data = 0x0A */
            in_ctrl_if = (cls == 0x02u);
            in_data_if = (cls == 0x0Au);

            if (in_ctrl_if) {
                out->ctrl_if = cur_if;
            }

            if (in_data_if) {
                tmp_in.ep_num = 0;
                tmp_out.ep_num = 0;
                tmp_data_if = cur_if;
                tmp_data_alt = cur_alt;
            }
        } else if (bDescriptorType == DT_CS_INTERFACE) {
            if (in_ctrl_if && bLength >= 4u) {
                uint8_t subtype = dev->cfg[i + 2];
                /* Ethernet Networking Functional Descriptor subtype = 0x0F.
                 * iMACAddress is at offset 3.
                 */
                if (subtype == 0x0Fu) {
                    out->mac_str_index = dev->cfg[i + 3];
                }
            }
        } else if (bDescriptorType == DT_ENDPOINT && bLength >= 7u) {
            if (in_data_if) {
                uint8_t bEndpointAddress = dev->cfg[i + 2];
                uint8_t bmAttributes = dev->cfg[i + 3];
                uint16_t wMaxPacketSize = le16(&dev->cfg[i + 4]);

                uint8_t ep_in = (bEndpointAddress & 0x80u) != 0;
                uint8_t ep_num = bEndpointAddress & 0x0Fu;
                uint8_t ep_type = bmAttributes & 0x3u;

                if (ep_type == USB_EPTYP_BULK) {
                    if (ep_in && tmp_in.ep_num == 0) {
                        tmp_in.ep_num = ep_num;
                        tmp_in.ep_type = USB_EPTYP_BULK;
                        tmp_in.ep_in = 1;
                        tmp_in.mps = wMaxPacketSize;
                    }
                    if (!ep_in && tmp_out.ep_num == 0) {
                        tmp_out.ep_num = ep_num;
                        tmp_out.ep_type = USB_EPTYP_BULK;
                        tmp_out.ep_in = 0;
                        tmp_out.mps = wMaxPacketSize;
                    }
                }

                if (tmp_in.ep_num != 0 && tmp_out.ep_num != 0) {
                    out->data_if = tmp_data_if;
                    out->data_alt = tmp_data_alt;
                    out->bulk_in = tmp_in;
                    out->bulk_out = tmp_out;
                    out->ok = 1;
                    return 0;
                }
            }
        }

        (void)cur_if;
        i += bLength;
    }

    return -1;
}

static int cdc_ecm_set_packet_filter(uint8_t dev_addr, int low_speed, uint8_t ctrl_if, uint16_t filter) {
    /* bmRequestType: H2D | Class | Interface */
    usb_setup_t req = {
        .bmRequestType = 0x21u,
        .bRequest = 0x43u, /* SET_ETHERNET_PACKET_FILTER */
        .wValue = filter,
        .wIndex = ctrl_if,
        .wLength = 0,
    };
    return usb_host_control_xfer(dev_addr, low_speed, req, 0, 0, 0);
}

int usb_net_try_bind(const usb_device_t *dev) {
    if (!dev) return -1;
    if (g_usbnet.bound) return -1;

    usb_ep_t bin = {0};
    usb_ep_t bout = {0};
    cdc_ecm_info_t ecm;
    int have_ecm = (parse_cdc_ecm(dev, &ecm) == 0 && ecm.ok);
    if (have_ecm) {
        bin = ecm.bulk_in;
        bout = ecm.bulk_out;
    } else {
        if (usb_host_find_bulk_in_out_pair(dev, &bin, &bout) != 0) {
            return -1;
        }
    }

    g_usbnet.bound = 1;
    g_usbnet.addr = dev->addr;
    g_usbnet.low_speed = (int)dev->low_speed;
    g_usbnet.ep_in = bin;
    g_usbnet.ep_out = bout;
    g_usbnet.in_pid = USB_PID_DATA0;
    g_usbnet.out_pid = USB_PID_DATA0;

    for (int i = 0; i < NETIF_NAME_MAX; i++) g_usbnet.nif.name[i] = 0;
    g_usbnet.nif.name[0] = 'u';
    g_usbnet.nif.name[1] = 's';
    g_usbnet.nif.name[2] = 'b';
    g_usbnet.nif.name[3] = '0';

    g_usbnet.nif.mtu = 1500;
    g_usbnet.nif.ops = &g_usbnet_ops;
    g_usbnet.nif.driver_ctx = 0;

    g_usbnet.has_ctrl_if = 0;
    g_usbnet.ctrl_if_num = 0;
    g_usbnet.mode = USBNET_MODE_RAW_BULK;
    g_usbnet.rndis_request_id = 0;

    /* Default locally-administered placeholder MAC (overridden if CDC-ECM provides one). */
    g_usbnet.nif.mac[0] = 0x02;
    g_usbnet.nif.mac[1] = 0x00;
    g_usbnet.nif.mac[2] = 0x00;
    g_usbnet.nif.mac[3] = 0x00;
    g_usbnet.nif.mac[4] = 0x00;
    g_usbnet.nif.mac[5] = 0x01;

    const uint16_t vid = dev_vid(dev);
    const uint16_t pid = dev_pid(dev);

    if (have_ecm) {
        g_usbnet.has_ctrl_if = 1;
        g_usbnet.ctrl_if_num = ecm.ctrl_if;

        if (ecm.data_alt != 0) {
            (void)usb_host_set_interface(dev->addr, (int)dev->low_speed, ecm.data_if, ecm.data_alt);
        }

        /* QEMU's default `-device usb-net` identifies as 0525:a4a2 (Linux RNDIS gadget).
         * Use RNDIS encapsulated control plane and wrap data packets.
         */
        if (vid == 0x0525u && pid == 0xa4a2u) {
            if (rndis_initialize(&g_usbnet) == 0) {
                uint8_t mac[6];
                uint32_t got = 0;
                if (rndis_query(&g_usbnet, OID_802_3_CURRENT_ADDRESS, mac, sizeof(mac), &got) == 0 && got == 6) {
                    for (int i = 0; i < 6; i++) g_usbnet.nif.mac[i] = mac[i];
                }
                (void)rndis_set_packet_filter(&g_usbnet, 0x0000000Fu);
                g_usbnet.mode = USBNET_MODE_RNDIS;
            }
        } else {
            /* CDC-ECM-ish best-effort: read MAC string and set packet filter.
             * If the device isn't ECM, these may fail harmlessly.
             */
            if (ecm.mac_str_index != 0) {
                char macstr[32];
                if (usb_host_get_string_ascii(dev->addr, (int)dev->low_speed, ecm.mac_str_index, macstr, sizeof(macstr)) == 0) {
                    (void)parse_mac_from_hexstr(macstr, g_usbnet.nif.mac);
                }
            }
            (void)cdc_ecm_set_packet_filter(dev->addr, (int)dev->low_speed, ecm.ctrl_if, 0x000Fu);
        }
    }

    (void)netif_register(&g_usbnet.nif);

    uart_write("usb-net: bound dev addr=");
    uart_write_hex_u64(g_usbnet.addr);
    uart_write(" bulk-in=");
    uart_write_hex_u64(g_usbnet.ep_in.ep_num);
    uart_write(" bulk-out=");
    uart_write_hex_u64(g_usbnet.ep_out.ep_num);
    uart_write(" mode=");
    uart_write(g_usbnet.mode == USBNET_MODE_RNDIS ? "rndis" : "raw");
    uart_write(" mac=");
    for (int i = 0; i < 6; i++) {
        uart_write_hex_u64(g_usbnet.nif.mac[i]);
        if (i != 5) uart_write(":");
    }
    uart_write("\n");

    return 0;
}

void usb_net_poll(void) {
    if (!g_usbnet.bound) return;

    uint8_t buf[2048];
    uint32_t got = 0;

    if (usb_host_in_xfer(g_usbnet.addr, g_usbnet.low_speed, g_usbnet.ep_in, g_usbnet.in_pid,
                         buf, sizeof(buf), &got, /*nak_ok=*/1) != 0) {
        return;
    }

    if (got == 0) return;

    /* Toggle PID only on successful (non-NAK) transactions with data. */
    g_usbnet.in_pid = (g_usbnet.in_pid == USB_PID_DATA0) ? USB_PID_DATA1 : USB_PID_DATA0;

    if (g_usbnet.mode == USBNET_MODE_RNDIS) {
        if (got < sizeof(rndis_packet_msg_t)) return;
        rndis_packet_msg_t *p = (rndis_packet_msg_t *)buf;
        if (p->msg_type != 0x00000001u) return;

        uint32_t data_len = p->data_len;
        uint32_t data_off = p->data_offset;
        uint8_t *data = buf + 8u + data_off;
        if ((uint64_t)(data - buf) + data_len > got) return;

        netif_rx_frame(&g_usbnet.nif, data, (size_t)data_len);
    } else {
        netif_rx_frame(&g_usbnet.nif, buf, (size_t)got);
    }
}
