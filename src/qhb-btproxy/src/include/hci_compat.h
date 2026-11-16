/**
 * @file hci_compat.h
 * @brief Self-contained Bluetooth HCI socket and protocol definitions
 *
 * Provides all standard Linux Bluetooth HCI definitions without requiring
 * external BlueZ header installations.
 */

#ifndef HCI_COMPAT_H
#define HCI_COMPAT_H

#include <stdint.h>
#include <sys/socket.h>

#ifndef AF_BLUETOOTH
#define AF_BLUETOOTH 31
#endif

#define BTPROTO_HCI 1
#define SOL_HCI     0
#define HCI_FILTER  2

#define HCI_CHANNEL_RAW  0
#define HCI_CHANNEL_USER 1

#define HCI_COMMAND_PKT 0x01
#define HCI_ACLDATA_PKT 0x02
#define HCI_SCODATA_PKT 0x03
#define HCI_EVENT_PKT   0x04

#define EVT_CMD_COMPLETE   0x0e
#define EVT_CMD_STATUS     0x0f
#define EVT_LE_META_EVENT  0x3e

#define HCI_EVT_LE_ADVERTISING_REPORT 0x02

#define HCI_OP_LE_SET_SCAN_PARAM  0x200b
#define HCI_OP_LE_SET_SCAN_ENABLE 0x200c

#define LE_SCAN_PASSIVE 0x00
#define LE_SCAN_ACTIVE  0x01

#define LE_SCAN_DISABLE 0x00
#define LE_SCAN_ENABLE  0x01

#define LE_SCAN_FILTER_DUP_DISABLE 0x00
#define LE_SCAN_FILTER_DUP_ENABLE  0x01

struct sockaddr_hci {
    sa_family_t hci_family;
    uint16_t    hci_dev;
    uint16_t    hci_channel;
};

struct hci_filter {
    uint32_t type_mask;
    uint32_t event_mask[2];
    uint16_t opcode;
};

#define hci_set_bit(nr, addr)   ((addr)[(nr) >> 5] |=  (1U << ((nr) & 31)))
#define hci_clear_bit(nr, addr) ((addr)[(nr) >> 5] &= ~(1U << ((nr) & 31)))

static inline void hci_filter_clear(struct hci_filter *f) {
    f->type_mask = 0;
    f->event_mask[0] = 0;
    f->event_mask[1] = 0;
    f->opcode = 0;
}

static inline void hci_filter_set_ptype(int t, struct hci_filter *f) {
    f->type_mask |= (1U << (t & 31));
}

static inline void hci_filter_set_event(int e, struct hci_filter *f) {
    hci_set_bit(e, f->event_mask);
}

struct hci_command_hdr {
    uint16_t opcode;
    uint8_t  plen;
} __attribute__((packed));

struct hci_event_hdr {
    uint8_t evt;
    uint8_t plen;
} __attribute__((packed));

struct evt_cmd_complete {
    uint8_t  ncmd;
    uint16_t opcode;
    uint8_t  status;
} __attribute__((packed));

struct evt_cmd_status {
    uint8_t  status;
    uint8_t  ncmd;
    uint16_t opcode;
} __attribute__((packed));

struct evt_le_meta_event {
    uint8_t subevent;
    uint8_t data[0];
} __attribute__((packed));

struct le_advertising_info {
    uint8_t evt_type;
    uint8_t bdaddr_type;
    uint8_t bdaddr[6];
    uint8_t length;
    uint8_t data[0];
} __attribute__((packed));

struct hci_cp_le_set_scan_param {
    uint8_t  type;
    uint16_t interval;
    uint16_t window;
    uint8_t  own_address_type;
    uint8_t  filter_policy;
} __attribute__((packed));

struct hci_cp_le_set_scan_enable {
    uint8_t enable;
    uint8_t filter_dup;
} __attribute__((packed));

/* L2CAP definitions */
#ifndef BTPROTO_L2CAP
#define BTPROTO_L2CAP 0
#endif

#ifndef BDADDR_BREDR
#define BDADDR_BREDR     0x00
#define BDADDR_LE_PUBLIC 0x01
#define BDADDR_LE_RANDOM 0x02
#endif

#ifndef L2CAP_CID_ATT
#define L2CAP_CID_ATT 0x0004
#endif

#ifndef htobs
#if defined(__BYTE_ORDER__) && __BYTE_ORDER__ == __ORDER_BIG_ENDIAN__
#define htobs(d) __builtin_bswap16((uint16_t)(d))
#define btohs(d) __builtin_bswap16((uint16_t)(d))
#else
#define htobs(d) ((uint16_t)(d))
#define btohs(d) ((uint16_t)(d))
#endif
#endif

typedef struct {
    uint8_t b[6];
} __attribute__((packed)) bdaddr_t;

struct sockaddr_l2 {
    sa_family_t    l2_family;
    unsigned short l2_psm;
    bdaddr_t       l2_bdaddr;
    unsigned short l2_cid;
    uint8_t        l2_bdaddr_type;
};

/* ATT Protocol Opcodes */
#define ATT_OP_ERROR_RSP           0x01
#define ATT_OP_MTU_REQ             0x02
#define ATT_OP_MTU_RSP             0x03
#define ATT_OP_FIND_INFO_REQ       0x04
#define ATT_OP_FIND_INFO_RSP       0x05
#define ATT_OP_READ_BY_TYPE_REQ    0x08
#define ATT_OP_READ_BY_TYPE_RSP    0x09
#define ATT_OP_READ_REQ            0x0a
#define ATT_OP_READ_RSP            0x0b
#define ATT_OP_READ_BLOB_REQ       0x0c
#define ATT_OP_READ_BLOB_RSP       0x0d
#define ATT_OP_READ_BY_GROUP_REQ   0x10
#define ATT_OP_READ_BY_GROUP_RSP   0x11
#define ATT_OP_WRITE_REQ           0x12
#define ATT_OP_WRITE_RSP           0x13
#define ATT_OP_WRITE_CMD           0x52
#define ATT_OP_HANDLE_VAL_NOTIF    0x1b
#define ATT_OP_HANDLE_VAL_IND      0x1d
#define ATT_OP_HANDLE_VAL_CONF     0x1e

#define GATT_PRIMARY_SERVICE_UUID  0x2800
#define GATT_CHARACTERISTIC_UUID   0x2803
#define GATT_CLIENT_CHARAC_CFG_UUID 0x2902

#endif /* HCI_COMPAT_H */
