/**
 * @file ble_scanner_hci.c
 * @brief Native Linux AF_BLUETOOTH raw HCI BLE scanner
 *
 * Implements the ble_scanner.h interface using direct Linux HCI sockets
 * (hci0). Does not require D-Bus, BlueZ daemon, or C++ libraries.
 *
 * Part of Home Base 2 Bluetooth Proxy (hb2-btproxy).
 */

#include "include/ble_scanner.h"
#include "include/hci_compat.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <poll.h>
#include <pthread.h>
#include <sys/socket.h>

#define LOG_PREFIX "[ble-hci] "

struct ble_scanner {
    int hci_sock;
    int dev_id;
    ble_advert_callback_t callback;
    void *user_data;
    pthread_t thread;
    volatile bool running;
};

/* Helper: Send HCI command via raw socket */
static int send_hci_cmd(int sock, uint16_t opcode, const void *param, uint8_t plen) {
    uint8_t buf[1 + sizeof(struct hci_command_hdr) + 255];
    buf[0] = HCI_COMMAND_PKT;
    
    struct hci_command_hdr *hdr = (struct hci_command_hdr *)(buf + 1);
    hdr->opcode = opcode;
    hdr->plen = plen;
    
    if (plen > 0 && param) {
        memcpy(buf + 1 + sizeof(struct hci_command_hdr), param, plen);
    }
    
    ssize_t total = 1 + sizeof(struct hci_command_hdr) + plen;
    if (write(sock, buf, total) != total) {
        fprintf(stderr, LOG_PREFIX "Failed to write HCI cmd 0x%04x: %s\n", opcode, strerror(errno));
        return -1;
    }
    return 0;
}

static int set_le_scan_parameters(int sock) {
    struct hci_cp_le_set_scan_param cp;
    memset(&cp, 0, sizeof(cp));
    cp.type = LE_SCAN_ACTIVE;          /* Active scan to request scan response */
    cp.interval = 0x0010;              /* 10 ms interval */
    cp.window = 0x0010;                /* 10 ms window (100% duty cycle) */
    cp.own_address_type = 0x00;        /* Public address */
    cp.filter_policy = 0x00;           /* Accept all advertisements */

    return send_hci_cmd(sock, HCI_OP_LE_SET_SCAN_PARAM, &cp, sizeof(cp));
}

static int set_le_scan_enable(int sock, uint8_t enable, uint8_t filter_dup) {
    struct hci_cp_le_set_scan_enable cp;
    memset(&cp, 0, sizeof(cp));
    cp.enable = enable;
    cp.filter_dup = filter_dup;

    return send_hci_cmd(sock, HCI_OP_LE_SET_SCAN_ENABLE, &cp, sizeof(cp));
}

/* Background thread reading HCI events */
static void *ble_hci_worker(void *arg) {
    ble_scanner_t *scanner = (ble_scanner_t *)arg;
    uint8_t buf[1024];

    printf(LOG_PREFIX "BLE scan worker thread running\n");

    while (scanner->running) {
        struct pollfd pfd = {
            .fd = scanner->hci_sock,
            .events = POLLIN,
            .revents = 0
        };

        int ret = poll(&pfd, 1, 200); /* 200ms timeout for clean stop */
        if (ret < 0) {
            if (errno == EINTR) continue;
            fprintf(stderr, LOG_PREFIX "poll() error: %s\n", strerror(errno));
            break;
        }
        if (ret == 0) {
            continue; /* Timeout, check running flag */
        }

        ssize_t len = read(scanner->hci_sock, buf, sizeof(buf));
        if (len < 0) {
            if (errno == EAGAIN || errno == EINTR) continue;
            if (scanner->running) {
                fprintf(stderr, LOG_PREFIX "read() error: %s\n", strerror(errno));
            }
            break;
        }

        /* Verify HCI Event packet */
        if (len < (ssize_t)(1 + sizeof(struct hci_event_hdr))) {
            continue;
        }
        if (buf[0] != HCI_EVENT_PKT) {
            continue;
        }

        struct hci_event_hdr *hdr = (struct hci_event_hdr *)(buf + 1);
        if (hdr->evt != EVT_LE_META_EVENT) {
            continue;
        }

        if (hdr->plen < sizeof(struct evt_le_meta_event)) {
            continue;
        }

        struct evt_le_meta_event *meta = (struct evt_le_meta_event *)(buf + 1 + sizeof(struct hci_event_hdr));
        if (meta->subevent != HCI_EVT_LE_ADVERTISING_REPORT) {
            continue;
        }

        uint8_t num_reports = meta->data[0];
        uint8_t *ptr = meta->data + 1;
        uint8_t *end = (buf + 1 + sizeof(struct hci_event_hdr) + hdr->plen);

        for (uint8_t i = 0; i < num_reports; i++) {
            if (ptr + sizeof(struct le_advertising_info) > end) {
                break;
            }

            struct le_advertising_info *info = (struct le_advertising_info *)ptr;
            if (ptr + sizeof(struct le_advertising_info) + info->length + 1 > end) {
                break;
            }

            ble_advertisement_t advert;
            memset(&advert, 0, sizeof(advert));

            /* Convert little-endian BD_ADDR to big-endian MAC string format */
            for (int k = 0; k < 6; k++) {
                advert.address[k] = info->bdaddr[5 - k];
            }

            advert.address_type = info->bdaddr_type;
            
            size_t copy_len = info->length;
            if (copy_len > BLE_ADV_DATA_MAX) {
                copy_len = BLE_ADV_DATA_MAX;
            }
            memcpy(advert.data, info->data, copy_len);
            advert.data_len = copy_len;

            /* RSSI byte is located immediately after info->data[info->length] */
            int8_t rssi = (int8_t)(*(ptr + sizeof(struct le_advertising_info) + info->length));
            advert.rssi = rssi;

            if (scanner->callback) {
                scanner->callback(&advert, scanner->user_data);
            }

            ptr += sizeof(struct le_advertising_info) + info->length + 1;
        }
    }

    printf(LOG_PREFIX "BLE scan worker thread exiting\n");
    return NULL;
}

ble_scanner_t *ble_scanner_init(ble_advert_callback_t callback, void *user_data) {
    ble_scanner_t *scanner = (ble_scanner_t *)calloc(1, sizeof(ble_scanner_t));
    if (!scanner) return NULL;

    scanner->callback = callback;
    scanner->user_data = user_data;
    scanner->hci_sock = -1;
    scanner->dev_id = 0; /* hci0 */
    scanner->running = false;

    return scanner;
}

int ble_scanner_start(ble_scanner_t *scanner) {
    if (!scanner) return -1;
    if (scanner->running) return 0;

    int sock = socket(AF_BLUETOOTH, SOCK_RAW, BTPROTO_HCI);
    if (sock < 0) {
        fprintf(stderr, LOG_PREFIX "Failed to open AF_BLUETOOTH socket: %s\n", strerror(errno));
        return -1;
    }

    struct sockaddr_hci addr;
    memset(&addr, 0, sizeof(addr));
    addr.hci_family = AF_BLUETOOTH;
    addr.hci_dev = scanner->dev_id;
    addr.hci_channel = HCI_CHANNEL_RAW;

    if (bind(sock, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        fprintf(stderr, LOG_PREFIX "Failed to bind to hci%d: %s\n", scanner->dev_id, strerror(errno));
        close(sock);
        return -1;
    }

    /* Configure filter for LE Meta events */
    struct hci_filter nf;
    hci_filter_clear(&nf);
    hci_filter_set_ptype(HCI_EVENT_PKT, &nf);
    hci_filter_set_event(EVT_LE_META_EVENT, &nf);
    if (setsockopt(sock, SOL_HCI, HCI_FILTER, &nf, sizeof(nf)) < 0) {
        fprintf(stderr, LOG_PREFIX "Failed to set HCI_FILTER: %s\n", strerror(errno));
        close(sock);
        return -1;
    }

    /* Configure scan parameters and enable scan */
    set_le_scan_enable(sock, LE_SCAN_DISABLE, LE_SCAN_FILTER_DUP_DISABLE);
    usleep(50000);

    if (set_le_scan_parameters(sock) < 0) {
        fprintf(stderr, LOG_PREFIX "Failed to set LE scan parameters\n");
        close(sock);
        return -1;
    }
    usleep(50000);

    if (set_le_scan_enable(sock, LE_SCAN_ENABLE, LE_SCAN_FILTER_DUP_DISABLE) < 0) {
        fprintf(stderr, LOG_PREFIX "Failed to enable LE scan\n");
        close(sock);
        return -1;
    }

    scanner->hci_sock = sock;
    scanner->running = true;

    if (pthread_create(&scanner->thread, NULL, ble_hci_worker, scanner) != 0) {
        fprintf(stderr, LOG_PREFIX "Failed to create worker thread: %s\n", strerror(errno));
        set_le_scan_enable(sock, LE_SCAN_DISABLE, LE_SCAN_FILTER_DUP_DISABLE);
        close(sock);
        scanner->hci_sock = -1;
        scanner->running = false;
        return -1;
    }

    printf(LOG_PREFIX "Started native BLE scan on hci%d\n", scanner->dev_id);
    return 0;
}

int ble_scanner_stop(ble_scanner_t *scanner) {
    if (!scanner || !scanner->running) return 0;

    scanner->running = false;
    pthread_join(scanner->thread, NULL);

    if (scanner->hci_sock >= 0) {
        set_le_scan_enable(scanner->hci_sock, LE_SCAN_DISABLE, LE_SCAN_FILTER_DUP_DISABLE);
        close(scanner->hci_sock);
        scanner->hci_sock = -1;
    }

    printf(LOG_PREFIX "Stopped BLE scan\n");
    return 0;
}

bool ble_scanner_is_running(ble_scanner_t *scanner) {
    return scanner ? scanner->running : false;
}

void ble_scanner_free(ble_scanner_t *scanner) {
    if (!scanner) return;
    ble_scanner_stop(scanner);
    free(scanner);
}
