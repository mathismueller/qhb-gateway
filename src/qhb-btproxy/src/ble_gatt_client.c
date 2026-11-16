/**
 * @file ble_gatt_client.c
 * @brief Active Bluetooth LE GATT Client using Linux L2CAP ATT Sockets
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <pthread.h>
#include <sys/socket.h>
#include <poll.h>

#include "include/ble_gatt_client.h"
#include "include/hci_compat.h"

#define LOG_PREFIX "[ble_gatt] "

typedef struct {
    bool active;
    uint64_t address;
    uint8_t address_type;
    int fd;
    uint16_t mtu;
    uint16_t pending_read_handle;
    uint16_t pending_write_handle;
    pthread_t thread;
    bool running;
} ble_conn_t;

static ble_conn_t s_conns[BLE_MAX_CONNECTIONS];
static pthread_mutex_t s_lock = PTHREAD_MUTEX_INITIALIZER;
static ble_gatt_callbacks_t s_cbs;
static void *s_user_data = NULL;
static bool s_initialized = false;

static ble_conn_t *find_conn(uint64_t address) {
    for (int i = 0; i < BLE_MAX_CONNECTIONS; i++) {
        if (s_conns[i].active && s_conns[i].address == address) {
            return &s_conns[i];
        }
    }
    return NULL;
}

static ble_conn_t *find_free_slot(void) {
    for (int i = 0; i < BLE_MAX_CONNECTIONS; i++) {
        if (!s_conns[i].active) {
            return &s_conns[i];
        }
    }
    return NULL;
}

static void notify_free_slots(void) {
    if (!s_cbs.on_connections_free) return;
    uint64_t allocated[BLE_MAX_CONNECTIONS];
    uint32_t free_cnt = ble_gatt_get_free_connections(allocated, BLE_MAX_CONNECTIONS);
    s_cbs.on_connections_free(free_cnt, BLE_MAX_CONNECTIONS, allocated, BLE_MAX_CONNECTIONS - free_cnt, s_user_data);
}

static void *conn_worker_thread(void *arg) {
    ble_conn_t *conn = (ble_conn_t *)arg;
    uint8_t buf[1024];

    printf(LOG_PREFIX "Worker started for device 0x%012llx (fd=%d)\n",
           (unsigned long long)conn->address, conn->fd);

    while (conn->running) {
        struct pollfd pfd = {
            .fd = conn->fd,
            .events = POLLIN,
            .revents = 0
        };

        int ret = poll(&pfd, 1, 200);
        if (ret < 0) {
            if (errno == EINTR) continue;
            break;
        }
        if (ret == 0) continue;

        ssize_t n = read(conn->fd, buf, sizeof(buf));
        if (n <= 0) {
            printf(LOG_PREFIX "Device 0x%012llx disconnected (read %zd)\n",
                   (unsigned long long)conn->address, n);
            break;
        }

        uint8_t opcode = buf[0];
        if (opcode == ATT_OP_MTU_RSP && n >= 3) {
            uint16_t server_mtu = buf[1] | (buf[2] << 8);
            if (server_mtu > 23) conn->mtu = server_mtu;
            printf(LOG_PREFIX "MTU exchanged: %u\n", conn->mtu);
        } else if (opcode == ATT_OP_READ_RSP && s_cbs.on_gatt_read_response) {
            s_cbs.on_gatt_read_response(conn->address, conn->pending_read_handle,
                                        buf + 1, n - 1, s_user_data);
        } else if (opcode == ATT_OP_WRITE_RSP && s_cbs.on_gatt_write_response) {
            s_cbs.on_gatt_write_response(conn->address, conn->pending_write_handle, s_user_data);
        } else if (opcode == ATT_OP_HANDLE_VAL_NOTIF && n >= 3 && s_cbs.on_gatt_notify_data) {
            uint16_t handle = buf[1] | (buf[2] << 8);
            s_cbs.on_gatt_notify_data(conn->address, handle, buf + 3, n - 3, s_user_data);
        } else if (opcode == ATT_OP_READ_BY_GROUP_RSP || opcode == ATT_OP_ERROR_RSP) {
            if (s_cbs.on_gatt_services_done) {
                s_cbs.on_gatt_services_done(conn->address, s_user_data);
            }
        }
    }

    /* Connection ended */
    pthread_mutex_lock(&s_lock);
    uint64_t addr = conn->address;
    if (conn->fd >= 0) {
        close(conn->fd);
        conn->fd = -1;
    }
    conn->active = false;
    conn->running = false;
    pthread_mutex_unlock(&s_lock);

    if (s_cbs.on_device_connection) {
        s_cbs.on_device_connection(addr, false, 0, 0, s_user_data);
    }
    notify_free_slots();

    return NULL;
}

int ble_gatt_client_init(const ble_gatt_callbacks_t *cbs, void *user_data) {
    pthread_mutex_lock(&s_lock);
    memset(s_conns, 0, sizeof(s_conns));
    for (int i = 0; i < BLE_MAX_CONNECTIONS; i++) {
        s_conns[i].fd = -1;
    }
    if (cbs) s_cbs = *cbs;
    s_user_data = user_data;
    s_initialized = true;
    pthread_mutex_unlock(&s_lock);

    printf(LOG_PREFIX "GATT client initialized (max %d connections)\n", BLE_MAX_CONNECTIONS);
    return 0;
}

void ble_gatt_client_cleanup(void) {
    pthread_mutex_lock(&s_lock);
    for (int i = 0; i < BLE_MAX_CONNECTIONS; i++) {
        if (s_conns[i].active) {
            s_conns[i].running = false;
            if (s_conns[i].fd >= 0) {
                close(s_conns[i].fd);
                s_conns[i].fd = -1;
            }
            pthread_join(s_conns[i].thread, NULL);
            s_conns[i].active = false;
        }
    }
    s_initialized = false;
    pthread_mutex_unlock(&s_lock);
}

uint32_t ble_gatt_get_free_connections(uint64_t *allocated_out, uint32_t max_count) {
    pthread_mutex_lock(&s_lock);
    uint32_t allocated_count = 0;
    for (int i = 0; i < BLE_MAX_CONNECTIONS; i++) {
        if (s_conns[i].active) {
            if (allocated_out && allocated_count < max_count) {
                allocated_out[allocated_count] = s_conns[i].address;
            }
            allocated_count++;
        }
    }
    pthread_mutex_unlock(&s_lock);
    return (BLE_MAX_CONNECTIONS > allocated_count) ? (BLE_MAX_CONNECTIONS - allocated_count) : 0;
}

typedef struct {
    uint64_t address;
    uint8_t address_type;
    ble_conn_t *conn;
} connect_task_t;

static void *connect_task_func(void *arg) {
    connect_task_t *task = (connect_task_t *)arg;
    uint64_t address = task->address;
    uint8_t address_type = task->address_type;
    ble_conn_t *conn = task->conn;
    free(task);

    int fd = socket(AF_BLUETOOTH, SOCK_SEQPACKET, BTPROTO_L2CAP);
    if (fd < 0) {
        fprintf(stderr, LOG_PREFIX "socket(BTPROTO_L2CAP) failed: %s\n", strerror(errno));
        pthread_mutex_lock(&s_lock);
        conn->active = false;
        pthread_mutex_unlock(&s_lock);
        if (s_cbs.on_device_connection) {
            s_cbs.on_device_connection(address, false, 0, errno, s_user_data);
        }
        notify_free_slots();
        return NULL;
    }

    struct sockaddr_l2 src;
    memset(&src, 0, sizeof(src));
    src.l2_family = AF_BLUETOOTH;
    src.l2_cid = htobs(L2CAP_CID_ATT);
    src.l2_bdaddr_type = BDADDR_LE_PUBLIC;
    bind(fd, (struct sockaddr *)&src, sizeof(src));

    struct sockaddr_l2 dst;
    memset(&dst, 0, sizeof(dst));
    dst.l2_family = AF_BLUETOOTH;
    dst.l2_cid = htobs(L2CAP_CID_ATT);
    dst.l2_bdaddr_type = (address_type == 1) ? BDADDR_LE_RANDOM : BDADDR_LE_PUBLIC;
    for (int i = 0; i < 6; i++) {
        dst.l2_bdaddr.b[i] = (uint8_t)((address >> (i * 8)) & 0xff);
    }

    printf(LOG_PREFIX "Connecting to 0x%012llx (type=%u)...\n",
           (unsigned long long)address, address_type);

    if (connect(fd, (struct sockaddr *)&dst, sizeof(dst)) < 0) {
        fprintf(stderr, LOG_PREFIX "connect() failed: %s\n", strerror(errno));
        close(fd);
        pthread_mutex_lock(&s_lock);
        conn->active = false;
        pthread_mutex_unlock(&s_lock);
        if (s_cbs.on_device_connection) {
            s_cbs.on_device_connection(address, false, 0, errno, s_user_data);
        }
        notify_free_slots();
        return NULL;
    }

    /* Send MTU exchange request: 517 bytes */
    uint8_t mtu_req[3] = { ATT_OP_MTU_REQ, 0x05, 0x02 }; /* 517 = 0x0205 */
    write(fd, mtu_req, sizeof(mtu_req));

    pthread_mutex_lock(&s_lock);
    conn->fd = fd;
    conn->mtu = 23;
    conn->running = true;
    if (pthread_create(&conn->thread, NULL, conn_worker_thread, conn) != 0) {
        close(fd);
        conn->fd = -1;
        conn->active = false;
        conn->running = false;
        pthread_mutex_unlock(&s_lock);
        if (s_cbs.on_device_connection) {
            s_cbs.on_device_connection(address, false, 0, errno, s_user_data);
        }
        notify_free_slots();
        return NULL;
    }
    pthread_mutex_unlock(&s_lock);

    if (s_cbs.on_device_connection) {
        s_cbs.on_device_connection(address, true, 23, 0, s_user_data);
    }
    notify_free_slots();

    return NULL;
}

int ble_gatt_connect(uint64_t address, uint8_t address_type) {
    pthread_mutex_lock(&s_lock);
    ble_conn_t *existing = find_conn(address);
    if (existing) {
        pthread_mutex_unlock(&s_lock);
        if (s_cbs.on_device_connection) {
            s_cbs.on_device_connection(address, true, existing->mtu, 0, s_user_data);
        }
        return 0;
    }

    ble_conn_t *slot = find_free_slot();
    if (!slot) {
        pthread_mutex_unlock(&s_lock);
        fprintf(stderr, LOG_PREFIX "No free connection slots for 0x%012llx\n",
                (unsigned long long)address);
        if (s_cbs.on_device_connection) {
            s_cbs.on_device_connection(address, false, 0, ENOMEM, s_user_data);
        }
        return -1;
    }

    slot->active = true;
    slot->address = address;
    slot->address_type = address_type;
    slot->fd = -1;
    slot->mtu = 23;
    slot->running = false;

    connect_task_t *task = malloc(sizeof(connect_task_t));
    if (!task) {
        slot->active = false;
        pthread_mutex_unlock(&s_lock);
        return -1;
    }
    task->address = address;
    task->address_type = address_type;
    task->conn = slot;

    pthread_t th;
    pthread_create(&th, NULL, connect_task_func, task);
    pthread_detach(th);

    pthread_mutex_unlock(&s_lock);
    notify_free_slots();
    return 0;
}

int ble_gatt_disconnect(uint64_t address) {
    pthread_mutex_lock(&s_lock);
    ble_conn_t *conn = find_conn(address);
    if (!conn) {
        pthread_mutex_unlock(&s_lock);
        if (s_cbs.on_device_connection) {
            s_cbs.on_device_connection(address, false, 0, 0, s_user_data);
        }
        return 0;
    }

    conn->running = false;
    if (conn->fd >= 0) {
        shutdown(conn->fd, SHUT_RDWR);
        close(conn->fd);
        conn->fd = -1;
    }
    pthread_mutex_unlock(&s_lock);
    return 0;
}

int ble_gatt_get_services(uint64_t address) {
    pthread_mutex_lock(&s_lock);
    ble_conn_t *conn = find_conn(address);
    if (!conn || conn->fd < 0) {
        pthread_mutex_unlock(&s_lock);
        return -1;
    }

    /* Read by Group Type: 0x0001 to 0xffff, UUID 0x2800 (Primary Service) */
    uint8_t req[7] = {
        ATT_OP_READ_BY_GROUP_REQ,
        0x01, 0x00, /* start handle 0x0001 */
        0xff, 0xff, /* end handle 0xffff */
        0x00, 0x28  /* UUID 0x2800 */
    };
    write(conn->fd, req, sizeof(req));
    pthread_mutex_unlock(&s_lock);
    return 0;
}

int ble_gatt_read(uint64_t address, uint16_t handle) {
    pthread_mutex_lock(&s_lock);
    ble_conn_t *conn = find_conn(address);
    if (!conn || conn->fd < 0) {
        pthread_mutex_unlock(&s_lock);
        return -1;
    }

    conn->pending_read_handle = handle;
    uint8_t req[3] = {
        ATT_OP_READ_REQ,
        (uint8_t)(handle & 0xff),
        (uint8_t)((handle >> 8) & 0xff)
    };
    write(conn->fd, req, sizeof(req));
    pthread_mutex_unlock(&s_lock);
    return 0;
}

int ble_gatt_write(uint64_t address, uint16_t handle, const uint8_t *data, size_t len, bool response) {
    pthread_mutex_lock(&s_lock);
    ble_conn_t *conn = find_conn(address);
    if (!conn || conn->fd < 0 || len > 512) {
        pthread_mutex_unlock(&s_lock);
        return -1;
    }

    conn->pending_write_handle = handle;
    uint8_t buf[515];
    buf[0] = response ? ATT_OP_WRITE_REQ : ATT_OP_WRITE_CMD;
    buf[1] = (uint8_t)(handle & 0xff);
    buf[2] = (uint8_t)((handle >> 8) & 0xff);
    if (len > 0 && data) {
        memcpy(buf + 3, data, len);
    }
    write(conn->fd, buf, 3 + len);
    pthread_mutex_unlock(&s_lock);
    return 0;
}

int ble_gatt_notify(uint64_t address, uint16_t handle, bool enable) {
    uint8_t cccd[2] = { enable ? 0x01 : 0x00, 0x00 };
    /* Write to CCCD (descriptor handle) with write request */
    return ble_gatt_write(address, handle, cccd, sizeof(cccd), true);
}
