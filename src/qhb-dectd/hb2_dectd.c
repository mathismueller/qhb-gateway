/*
 * hb2_dectd: DECT ULE / HAN-FUN to Home Assistant MQTT Bridge Daemon
 * Target: Telekom Qivicon Home Base 2 (HiSilicon SD5650T + DSP Group DCX81)
 *
 * Communicates with /bin/voiper over UDP 127.0.0.1:3490 and bridges to MQTT:
 *  - Discovers devices via GET_DEV_TABLE and DEV_REGISTERED notifications
 *  - Exposes Pairing switch to Home Assistant (OPEN_REG / CLOSE_REG)
 *  - Publishes Home Assistant MQTT Auto-Discovery topics for:
 *      * Switches (HAN-FUN 0x0200)
 *      * Binary Sensors / Contacts (HAN-FUN 0x0202)
 *      * Thermostats / Climate (HAN-FUN 0x0204)
 *      * Power / Energy Meters (HAN-FUN 0x0205, 0x0206)
 *  - Handles bidirectional control (HA MQTT command -> FUN_MSG -> DECT device)
 *  - Exposes raw debug topics: homebase/dect/raw/rx, homebase/dect/raw/tx
 */

#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <stdbool.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <signal.h>
#include <poll.h>
#include <time.h>
#include <stdarg.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <arpa/inet.h>
#include <netdb.h>

#define DECT_UDP_PORT         3490
#define DECT_UDP_HOST         "127.0.0.1"

#define DEFAULT_MQTT_HOST     "192.168.188.1"
#define DEFAULT_MQTT_PORT     1883
#define DEFAULT_MQTT_PREFIX   "homebase/dect"
#define DEFAULT_HA_PREFIX     "homeassistant"
#define DEFAULT_CLIENT_ID     "hb2_dect_bridge"

#define MAX_DEVICES           32
#define MAX_UNITS             8
#define BUFFER_SIZE           8192

/* --------------------------------------------------------------------------
 * Structures & Enums
 * -------------------------------------------------------------------------- */

typedef struct {
    uint16_t dev_id;
    uint8_t  unit_id;
    uint16_t profile;         /* e.g. 0x0200 = Switch, 0x0202 = Contact */
    bool     registered;
    bool     state_known;
    bool     bool_state;      /* ON / OFF */
    float    num_state;       /* temp, power, etc. */
} dect_unit_t;

typedef struct {
    uint16_t   dev_id;
    char       ipui[32];
    char       name[32];
    uint8_t    unit_count;
    dect_unit_t units[MAX_UNITS];
    bool       valid;
} dect_device_t;

typedef struct {
    char mqtt_host[128];
    int  mqtt_port;
    char mqtt_user[64];
    char mqtt_pass[64];
    char mqtt_prefix[64];
    char ha_prefix[64];
    char client_id[64];
    int  keepalive_sec;
    bool verbose;
    bool foreground;
} config_t;

/* --------------------------------------------------------------------------
 * Global State
 * -------------------------------------------------------------------------- */

static volatile bool g_running = true;
static config_t      g_cfg;
static dect_device_t g_devs[MAX_DEVICES];
static int           g_dev_count = 0;
static bool          g_pairing_active = false;
static uint64_t      g_pairing_end_mono = 0;

static uint64_t get_monotonic_sec(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec;
}

static int           g_dect_fd = -1;
static struct sockaddr_in g_dect_server_addr;

static int           g_mqtt_fd = -1;
static bool          g_mqtt_connected = false;
static uint64_t      g_last_ping_sent = 0;
static uint64_t      g_last_pkt_recv = 0;
static uint16_t      g_msg_seq = 1;

/* --------------------------------------------------------------------------
 * Signal Handler
 * -------------------------------------------------------------------------- */

static void sig_handler(int sig) {
    (void)sig;
    g_running = false;
}

/* --------------------------------------------------------------------------
 * Forward Declarations
 * -------------------------------------------------------------------------- */

static int  mqtt_connect(void);
static void mqtt_disconnect(void);
static int  mqtt_publish(const char *topic, const char *payload, bool retain);
static int  mqtt_subscribe(const char *topic);
static void publish_ha_base_discovery(void);
static void publish_ha_device_discovery(dect_device_t *dev, dect_unit_t *u);
static void send_dect_cmd(const char *cmd_fmt, ...);
static void handle_dect_packet(char *buf, int len);
static void handle_mqtt_publish(const char *topic, const char *payload);

/* --------------------------------------------------------------------------
 * Configuration Parser
 * -------------------------------------------------------------------------- */

static void load_config_file(const char *path) {
    FILE *f = fopen(path, "r");
    if (!f) return;

    char line[256];
    while (fgets(line, sizeof(line), f)) {
        char *p = line;
        while (*p == ' ' || *p == '\t') p++;
        if (*p == '#' || *p == ';' || *p == '\n' || *p == '\0') continue;

        char *nl = strchr(p, '\n');
        if (nl) *nl = '\0';
        char *cr = strchr(p, '\r');
        if (cr) *cr = '\0';

        char *eq = strchr(p, '=');
        if (!eq) continue;
        *eq = '\0';
        char *key = p;
        char *val = eq + 1;
        while (*val == ' ' || *val == '\t') val++;

        if (strcmp(key, "MQTT_HOST") == 0) {
            strncpy(g_cfg.mqtt_host, val, sizeof(g_cfg.mqtt_host) - 1);
        } else if (strcmp(key, "MQTT_PORT") == 0) {
            g_cfg.mqtt_port = atoi(val);
        } else if (strcmp(key, "MQTT_USER") == 0) {
            strncpy(g_cfg.mqtt_user, val, sizeof(g_cfg.mqtt_user) - 1);
        } else if (strcmp(key, "MQTT_PASS") == 0) {
            strncpy(g_cfg.mqtt_pass, val, sizeof(g_cfg.mqtt_pass) - 1);
        } else if (strcmp(key, "MQTT_PREFIX") == 0) {
            strncpy(g_cfg.mqtt_prefix, val, sizeof(g_cfg.mqtt_prefix) - 1);
        } else if (strcmp(key, "HA_DISCOVERY_PREFIX") == 0) {
            strncpy(g_cfg.ha_prefix, val, sizeof(g_cfg.ha_prefix) - 1);
        }
    }
    fclose(f);
}

/* --------------------------------------------------------------------------
 * Lightweight MQTT 3.1.1 Implementation (Zero Dependencies)
 * -------------------------------------------------------------------------- */

static int mqtt_encode_length(uint8_t *buf, int len) {
    int bytes = 0;
    do {
        uint8_t d = len % 128;
        len /= 128;
        if (len > 0) d |= 0x80;
        buf[bytes++] = d;
    } while (len > 0);
    return bytes;
}

static int mqtt_connect(void) {
    mqtt_disconnect();

    struct in_addr in;
    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port = htons(g_cfg.mqtt_port);

    if (inet_aton(g_cfg.mqtt_host, &in)) {
        addr.sin_addr = in;
    } else {
        struct hostent *he = gethostbyname(g_cfg.mqtt_host);
        if (!he) {
            if (g_cfg.verbose) fprintf(stderr, "[-] Failed to resolve MQTT host: %s\n", g_cfg.mqtt_host);
            return -1;
        }
        memcpy(&addr.sin_addr, he->h_addr_list[0], he->h_length);
    }

    g_mqtt_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (g_mqtt_fd < 0) {
        if (g_cfg.verbose) perror("[-] Failed to create MQTT socket");
        return -1;
    }

    /* Set non-blocking for connect timeout */
    int flags = fcntl(g_mqtt_fd, F_GETFL, 0);
    fcntl(g_mqtt_fd, F_SETFL, flags | O_NONBLOCK);

    int res = connect(g_mqtt_fd, (struct sockaddr *)&addr, sizeof(addr));
    if (res < 0 && errno != EINPROGRESS) {
        close(g_mqtt_fd);
        g_mqtt_fd = -1;
        return -1;
    }

    /* Wait up to 3s for connection */
    struct pollfd pfd = { .fd = g_mqtt_fd, .events = POLLOUT, .revents = 0 };
    if (poll(&pfd, 1, 3000) <= 0) {
        close(g_mqtt_fd);
        g_mqtt_fd = -1;
        return -1;
    }

    int err = 0;
    socklen_t errlen = sizeof(err);
    getsockopt(g_mqtt_fd, SOL_SOCKET, SO_ERROR, &err, &errlen);
    if (err != 0) {
        close(g_mqtt_fd);
        g_mqtt_fd = -1;
        return -1;
    }

    /* Build MQTT CONNECT packet */
    uint8_t payload[1024];
    int p_len = 0;

    /* Client ID */
    int cid_len = strlen(g_cfg.client_id);
    payload[p_len++] = (cid_len >> 8) & 0xFF;
    payload[p_len++] = cid_len & 0xFF;
    memcpy(&payload[p_len], g_cfg.client_id, cid_len);
    p_len += cid_len;

    /* Will topic & message: homebase/dect/status -> "offline" */
    char will_topic[128];
    snprintf(will_topic, sizeof(will_topic), "%s/status", g_cfg.mqtt_prefix);
    const char *will_msg = "offline";
    int wt_len = strlen(will_topic);
    int wm_len = strlen(will_msg);

    uint8_t conn_flags = 0x02; /* Clean Session */
    conn_flags |= 0x04;        /* Will Flag */
    conn_flags |= 0x20;        /* Will Retain */

    payload[p_len++] = (wt_len >> 8) & 0xFF;
    payload[p_len++] = wt_len & 0xFF;
    memcpy(&payload[p_len], will_topic, wt_len);
    p_len += wt_len;

    payload[p_len++] = (wm_len >> 8) & 0xFF;
    payload[p_len++] = wm_len & 0xFF;
    memcpy(&payload[p_len], will_msg, wm_len);
    p_len += wm_len;

    /* Username & Password if present */
    if (g_cfg.mqtt_user[0]) {
        conn_flags |= 0x80;
        int u_len = strlen(g_cfg.mqtt_user);
        payload[p_len++] = (u_len >> 8) & 0xFF;
        payload[p_len++] = u_len & 0xFF;
        memcpy(&payload[p_len], g_cfg.mqtt_user, u_len);
        p_len += u_len;
    }
    if (g_cfg.mqtt_pass[0]) {
        conn_flags |= 0x40;
        int pw_len = strlen(g_cfg.mqtt_pass);
        payload[p_len++] = (pw_len >> 8) & 0xFF;
        payload[p_len++] = pw_len & 0xFF;
        memcpy(&payload[p_len], g_cfg.mqtt_pass, pw_len);
        p_len += pw_len;
    }

    /* Variable Header: Protocol "MQTT", Level 4, Flags, Keepalive */
    uint8_t vhdr[10] = {
        0x00, 0x04, 'M', 'Q', 'T', 'T',
        0x04,
        conn_flags,
        (uint8_t)((g_cfg.keepalive_sec >> 8) & 0xFF),
        (uint8_t)(g_cfg.keepalive_sec & 0xFF)
    };

    uint8_t pkt[1200];
    pkt[0] = 0x10; /* CONNECT */
    int rem_len = sizeof(vhdr) + p_len;
    int len_bytes = mqtt_encode_length(&pkt[1], rem_len);
    memcpy(&pkt[1 + len_bytes], vhdr, sizeof(vhdr));
    memcpy(&pkt[1 + len_bytes + sizeof(vhdr)], payload, p_len);
    int total_len = 1 + len_bytes + rem_len;

    if (write(g_mqtt_fd, pkt, total_len) != total_len) {
        close(g_mqtt_fd);
        g_mqtt_fd = -1;
        return -1;
    }

    /* Wait for CONNACK */
    uint8_t ack[4];
    pfd.events = POLLIN;
    if (poll(&pfd, 1, 3000) <= 0 || read(g_mqtt_fd, ack, sizeof(ack)) != 4) {
        close(g_mqtt_fd);
        g_mqtt_fd = -1;
        return -1;
    }

    if (ack[0] != 0x20 || ack[3] != 0x00) {
        if (g_cfg.verbose) fprintf(stderr, "[-] MQTT CONNACK rejected with code: %d\n", ack[3]);
        close(g_mqtt_fd);
        g_mqtt_fd = -1;
        return -1;
    }

    g_mqtt_connected = true;
    g_last_ping_sent = time(NULL);
    g_last_pkt_recv = time(NULL);
    printf("[+] Connected to MQTT Broker %s:%d\n", g_cfg.mqtt_host, g_cfg.mqtt_port);

    /* Publish online status */
    char stat_topic[128];
    snprintf(stat_topic, sizeof(stat_topic), "%s/status", g_cfg.mqtt_prefix);
    mqtt_publish(stat_topic, "online", true);

    /* Publish pairing state */
    char pair_state_topic[128];
    snprintf(pair_state_topic, sizeof(pair_state_topic), "%s/pairing/state", g_cfg.mqtt_prefix);
    mqtt_publish(pair_state_topic, g_pairing_active ? "ON" : "OFF", true);

    /* Subscribe to command topics:
     *   homebase/dect/pairing/set
     *   homebase/dect/+/+/set
     *   homebase/dect/raw/tx
     */
    char sub_topic[128];
    snprintf(sub_topic, sizeof(sub_topic), "%s/pairing/set", g_cfg.mqtt_prefix);
    mqtt_subscribe(sub_topic);

    snprintf(sub_topic, sizeof(sub_topic), "%s/+/+/set", g_cfg.mqtt_prefix);
    mqtt_subscribe(sub_topic);

    snprintf(sub_topic, sizeof(sub_topic), "%s/raw/cmd", g_cfg.mqtt_prefix);
    mqtt_subscribe(sub_topic);

    /* Publish Base Station HA Discovery */
    publish_ha_base_discovery();

    return 0;
}

static void mqtt_disconnect(void) {
    if (g_mqtt_fd >= 0) {
        uint8_t disc[2] = { 0xE0, 0x00 };
        (void)write(g_mqtt_fd, disc, sizeof(disc));
        close(g_mqtt_fd);
        g_mqtt_fd = -1;
    }
    g_mqtt_connected = false;
}

static int mqtt_publish(const char *topic, const char *payload, bool retain) {
    if (!g_mqtt_connected || g_mqtt_fd < 0) return -1;

    int t_len = strlen(topic);
    int p_len = payload ? strlen(payload) : 0;
    int rem_len = 2 + t_len + p_len;

    uint8_t pkt[BUFFER_SIZE];
    pkt[0] = 0x30 | (retain ? 0x01 : 0x00); /* QoS 0 */
    int len_bytes = mqtt_encode_length(&pkt[1], rem_len);

    int idx = 1 + len_bytes;
    pkt[idx++] = (t_len >> 8) & 0xFF;
    pkt[idx++] = t_len & 0xFF;
    memcpy(&pkt[idx], topic, t_len);
    idx += t_len;

    if (p_len > 0) {
        memcpy(&pkt[idx], payload, p_len);
        idx += p_len;
    }

    if (write(g_mqtt_fd, pkt, idx) != idx) {
        mqtt_disconnect();
        return -1;
    }

    if (g_cfg.verbose) {
        printf("[MQTT PUB] %s -> %s\n", topic, payload ? payload : "");
    }
    return 0;
}

static int mqtt_subscribe(const char *topic) {
    if (!g_mqtt_connected || g_mqtt_fd < 0) return -1;

    int t_len = strlen(topic);
    int rem_len = 2 + 2 + t_len + 1; /* packet ID (2) + topic len (2) + topic + qos (1) */

    uint8_t pkt[512];
    pkt[0] = 0x82; /* SUBSCRIBE */
    int len_bytes = mqtt_encode_length(&pkt[1], rem_len);

    int idx = 1 + len_bytes;
    pkt[idx++] = 0x00;
    pkt[idx++] = 0x01; /* Packet ID 1 */
    pkt[idx++] = (t_len >> 8) & 0xFF;
    pkt[idx++] = t_len & 0xFF;
    memcpy(&pkt[idx], topic, t_len);
    idx += t_len;
    pkt[idx++] = 0x00; /* QoS 0 */

    if (write(g_mqtt_fd, pkt, idx) != idx) {
        mqtt_disconnect();
        return -1;
    }
    return 0;
}

static void mqtt_ping(void) {
    if (!g_mqtt_connected || g_mqtt_fd < 0) return;
    uint8_t ping[2] = { 0xC0, 0x00 };
    (void)write(g_mqtt_fd, ping, sizeof(ping));
    g_last_ping_sent = time(NULL);
}

static void handle_mqtt_incoming_bytes(void) {
    uint8_t buf[BUFFER_SIZE];
    int n = read(g_mqtt_fd, buf, sizeof(buf));
    if (n <= 0) {
        if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) return;
        if (g_cfg.verbose) fprintf(stderr, "[-] MQTT connection closed by broker\n");
        mqtt_disconnect();
        return;
    }

    g_last_pkt_recv = time(NULL);
    int offset = 0;
    while (offset < n) {
        uint8_t type = buf[offset] & 0xF0;
        int pos = offset + 1;
        int multiplier = 1;
        int rem_len = 0;
        uint8_t byte;
        do {
            if (pos >= n) return;
            byte = buf[pos++];
            rem_len += (byte & 127) * multiplier;
            multiplier *= 128;
        } while ((byte & 128) != 0);

        if (pos + rem_len > n) {
            /* Truncated frame, skip */
            break;
        }

        if (type == 0x30) { /* PUBLISH */
            int t_len = (buf[pos] << 8) | buf[pos + 1];
            pos += 2;
            char topic[256];
            int copy_t = t_len < (int)sizeof(topic) - 1 ? t_len : (int)sizeof(topic) - 1;
            memcpy(topic, &buf[pos], copy_t);
            topic[copy_t] = '\0';
            pos += t_len;

            int p_len = rem_len - 2 - t_len;
            char payload[1024];
            int copy_p = p_len < (int)sizeof(payload) - 1 ? p_len : (int)sizeof(payload) - 1;
            if (p_len > 0) {
                memcpy(payload, &buf[pos], copy_p);
                payload[copy_p] = '\0';
            } else {
                payload[0] = '\0';
            }

            handle_mqtt_publish(topic, payload);
        }

        offset = pos + rem_len;
    }
}

/* --------------------------------------------------------------------------
 * Home Assistant MQTT Auto-Discovery
 * -------------------------------------------------------------------------- */

static void publish_ha_base_discovery(void) {
    /* 1. Pairing switch */
    char topic[256], payload[1024];
    snprintf(topic, sizeof(topic), "%s/switch/hb2_dect/pairing/config", g_cfg.ha_prefix);
    snprintf(payload, sizeof(payload),
        "{"
          "\"name\":\"DECT ULE Pairing\","
          "\"unique_id\":\"hb2_dect_pairing\","
          "\"state_topic\":\"%s/pairing/state\","
          "\"command_topic\":\"%s/pairing/set\","
          "\"payload_on\":\"ON\","
          "\"payload_off\":\"OFF\","
          "\"icon\":\"mdi:radio-tower\","
          "\"device\":{"
            "\"identifiers\":[\"hb2_dect_base\"],"
            "\"name\":\"HB2 DECT Base Station\","
            "\"model\":\"DCX81 (Baseband 3.67.6)\","
            "\"manufacturer\":\"Telekom / Synaptics\","
            "\"sw_version\":\"1.0.0\""
          "}"
        "}",
        g_cfg.mqtt_prefix, g_cfg.mqtt_prefix
    );
    mqtt_publish(topic, payload, true);

    /* 2. Paired Devices Count Sensor */
    snprintf(topic, sizeof(topic), "%s/sensor/hb2_dect/paired_devices/config", g_cfg.ha_prefix);
    snprintf(payload, sizeof(payload),
        "{"
          "\"name\":\"Paired DECT Devices\","
          "\"unique_id\":\"hb2_dect_paired_devices\","
          "\"state_topic\":\"%s/devices/count\","
          "\"unit_of_measurement\":\"devices\","
          "\"state_class\":\"measurement\","
          "\"icon\":\"mdi:devices\","
          "\"device\":{"
            "\"identifiers\":[\"hb2_dect_base\"],"
            "\"name\":\"HB2 DECT Base Station\","
            "\"model\":\"DCX81 (Baseband 3.67.6)\","
            "\"manufacturer\":\"Telekom / Synaptics\","
            "\"sw_version\":\"1.0.0\""
          "}"
        "}",
        g_cfg.mqtt_prefix
    );
    mqtt_publish(topic, payload, true);

    /* Publish initial count */
    char cnt_str[16];
    snprintf(cnt_str, sizeof(cnt_str), "%d", g_dev_count);
    char cnt_topic[128];
    snprintf(cnt_topic, sizeof(cnt_topic), "%s/devices/count", g_cfg.mqtt_prefix);
    mqtt_publish(cnt_topic, cnt_str, true);
}

static void publish_ha_device_discovery(dect_device_t *dev, dect_unit_t *u) {
    char disc_topic[256], payload[1024];

    if (u->profile == 0x0200) {
        /* Simple On/Off Switch */
        snprintf(disc_topic, sizeof(disc_topic), "%s/switch/dect_%u_%u/config",
                 g_cfg.ha_prefix, dev->dev_id, u->unit_id);
        snprintf(payload, sizeof(payload),
            "{"
              "\"name\":\"DECT Switch %u.%u\","
              "\"unique_id\":\"dect_%u_%u_switch\","
              "\"state_topic\":\"%s/%u/%u/state\","
              "\"command_topic\":\"%s/%u/%u/set\","
              "\"payload_on\":\"ON\","
              "\"payload_off\":\"OFF\","
              "\"device\":{"
                "\"identifiers\":[\"dect_%u\"],"
                "\"name\":\"DECT Device %u\","
                "\"via_device\":\"hb2_dect_base\""
              "}"
            "}",
            dev->dev_id, u->unit_id,
            dev->dev_id, u->unit_id,
            g_cfg.mqtt_prefix, dev->dev_id, u->unit_id,
            g_cfg.mqtt_prefix, dev->dev_id, u->unit_id,
            dev->dev_id, dev->dev_id
        );
        mqtt_publish(disc_topic, payload, true);
    } else if (u->profile == 0x0202) {
        /* Simple Detector / Contact / Window Sensor */
        snprintf(disc_topic, sizeof(disc_topic), "%s/binary_sensor/dect_%u_%u/config",
                 g_cfg.ha_prefix, dev->dev_id, u->unit_id);
        snprintf(payload, sizeof(payload),
            "{"
              "\"name\":\"DECT Sensor %u.%u\","
              "\"unique_id\":\"dect_%u_%u_sensor\","
              "\"state_topic\":\"%s/%u/%u/state\","
              "\"payload_on\":\"ON\","
              "\"payload_off\":\"OFF\","
              "\"device_class\":\"opening\","
              "\"device\":{"
                "\"identifiers\":[\"dect_%u\"],"
                "\"name\":\"DECT Device %u\","
                "\"via_device\":\"hb2_dect_base\""
              "}"
            "}",
            dev->dev_id, u->unit_id,
            dev->dev_id, u->unit_id,
            g_cfg.mqtt_prefix, dev->dev_id, u->unit_id,
            dev->dev_id, dev->dev_id
        );
        mqtt_publish(disc_topic, payload, true);
    } else if (u->profile == 0x0204) {
        /* Simple Temperature / Climate */
        snprintf(disc_topic, sizeof(disc_topic), "%s/sensor/dect_%u_%u_temp/config",
                 g_cfg.ha_prefix, dev->dev_id, u->unit_id);
        snprintf(payload, sizeof(payload),
            "{"
              "\"name\":\"DECT Temp %u.%u\","
              "\"unique_id\":\"dect_%u_%u_temp\","
              "\"state_topic\":\"%s/%u/%u/temperature\","
              "\"unit_of_measurement\":\"°C\","
              "\"device_class\":\"temperature\","
              "\"device\":{"
                "\"identifiers\":[\"dect_%u\"],"
                "\"name\":\"DECT Device %u\","
                "\"via_device\":\"hb2_dect_base\""
              "}"
            "}",
            dev->dev_id, u->unit_id,
            dev->dev_id, u->unit_id,
            g_cfg.mqtt_prefix, dev->dev_id, u->unit_id,
            dev->dev_id, dev->dev_id
        );
        mqtt_publish(disc_topic, payload, true);
    } else if (u->profile == 0x0111 || u->profile == 273 || u->profile == 0x0304 || u->profile == 772) {
        /* HAN-FUN 0x0111: Simple Button Sensor (e.g. Telekom Wandtaster) */
        /* Expose as Event entity in Home Assistant */
        snprintf(disc_topic, sizeof(disc_topic), "%s/event/dect_%u_%u/config",
                 g_cfg.ha_prefix, dev->dev_id, u->unit_id);
        snprintf(payload, sizeof(payload),
            "{"
              "\"name\":\"DECT Button %u.%u\","
              "\"unique_id\":\"dect_%u_%u_btn\","
              "\"state_topic\":\"%s/%u/%u/event\","
              "\"event_types\":[\"press\",\"double_press\",\"long_press\",\"triple_press\"],"
              "\"device_class\":\"button\","
              "\"icon\":\"mdi:gesture-tap-button\","
              "\"device\":{"
                "\"identifiers\":[\"dect_%u\"],"
                "\"name\":\"DECT Device %u (Wandtaster)\","
                "\"via_device\":\"hb2_dect_base\""
              "}"
            "}",
            dev->dev_id, u->unit_id,
            dev->dev_id, u->unit_id,
            g_cfg.mqtt_prefix, dev->dev_id, u->unit_id,
            dev->dev_id, dev->dev_id
        );
        mqtt_publish(disc_topic, payload, true);

        /* Also expose as binary sensor for simple state triggers */
        char state_top[128];
        snprintf(state_top, sizeof(state_top), "%s/%u/%u/state", g_cfg.mqtt_prefix, dev->dev_id, u->unit_id);
        snprintf(disc_topic, sizeof(disc_topic), "%s/binary_sensor/dect_%u_%u_press/config",
                 g_cfg.ha_prefix, dev->dev_id, u->unit_id);
        snprintf(payload, sizeof(payload),
            "{"
              "\"name\":\"DECT Button %u.%u Press\","
              "\"unique_id\":\"dect_%u_%u_btn_press\","
              "\"state_topic\":\"%s\","
              "\"payload_on\":\"ON\","
              "\"payload_off\":\"OFF\","
              "\"off_delay\":1,"
              "\"device\":{"
                "\"identifiers\":[\"dect_%u\"],"
                "\"name\":\"DECT Device %u (Wandtaster)\","
                "\"via_device\":\"hb2_dect_base\""
              "}"
            "}",
            dev->dev_id, u->unit_id,
            dev->dev_id, u->unit_id,
            state_top,
            dev->dev_id, dev->dev_id
        );
        mqtt_publish(disc_topic, payload, true);

        /* Initialize binary sensor state to OFF */
        mqtt_publish(state_top, "OFF", true);
    }
}

/* --------------------------------------------------------------------------
 * DECT UDP Interface (talks to /bin/voiper on 127.0.0.1:3490)
 * -------------------------------------------------------------------------- */

static int dect_udp_init(void) {
    g_dect_fd = socket(AF_INET, SOCK_DGRAM, 0);
    if (g_dect_fd < 0) {
        perror("[-] Failed to create DECT UDP socket");
        return -1;
    }

    int flags = fcntl(g_dect_fd, F_GETFL, 0);
    fcntl(g_dect_fd, F_SETFL, flags | O_NONBLOCK);

    memset(&g_dect_server_addr, 0, sizeof(g_dect_server_addr));
    g_dect_server_addr.sin_family = AF_INET;
    g_dect_server_addr.sin_port = htons(DECT_UDP_PORT);
    g_dect_server_addr.sin_addr.s_addr = inet_addr(DECT_UDP_HOST);

    printf("[+] DECT UDP socket initialized (connecting to %s:%d)\n", DECT_UDP_HOST, DECT_UDP_PORT);
    return 0;
}

static void send_dect_cmd(const char *cmd_fmt, ...) {
    if (g_dect_fd < 0) return;

    char buf[1024];
    va_list ap;
    va_start(ap, cmd_fmt);
    vsnprintf(buf, sizeof(buf), cmd_fmt, ap);
    va_end(ap);

    int len = strlen(buf) + 1; /* Include null-terminator expected by voiper */
    sendto(g_dect_fd, buf, len, 0, (struct sockaddr *)&g_dect_server_addr, sizeof(g_dect_server_addr));

    if (g_cfg.verbose) {
        printf("[DECT TX] %s\n", buf);
    }

    /* Publish to raw RX/TX MQTT topic for live diagnostics */
    char raw_topic[128];
    snprintf(raw_topic, sizeof(raw_topic), "%s/raw/tx", g_cfg.mqtt_prefix);
    mqtt_publish(raw_topic, buf, false);
}

static dect_device_t* find_or_create_device(uint16_t dev_id) {
    for (int i = 0; i < g_dev_count; i++) {
        if (g_devs[i].valid && g_devs[i].dev_id == dev_id) {
            return &g_devs[i];
        }
    }
    if (g_dev_count < MAX_DEVICES) {
        dect_device_t *d = &g_devs[g_dev_count++];
        memset(d, 0, sizeof(*d));
        d->dev_id = dev_id;
        d->valid = true;
        return d;
    }
    return NULL;
}

static void handle_dect_packet(char *buf, int len) {
    buf[len] = '\0';

    if (g_cfg.verbose) {
        printf("[DECT RX %d bytes]\n%s\n", len, buf);
    }

    /* Mirror raw packets to MQTT */
    char raw_topic[128];
    snprintf(raw_topic, sizeof(raw_topic), "%s/raw/rx", g_cfg.mqtt_prefix);
    mqtt_publish(raw_topic, buf, false);

    /* 1. Pairing status updates */
    if (strstr(buf, "OPEN_RES") && strstr(buf, "SUCCEED")) {
        g_pairing_active = true;
        char topic[128];
        snprintf(topic, sizeof(topic), "%s/pairing/state", g_cfg.mqtt_prefix);
        mqtt_publish(topic, "ON", true);
        printf("[DECT] Pairing window open\n");
    } else if (strstr(buf, "CLOSE_RES") || strstr(buf, "REG_CLOSED")) {
        g_pairing_active = false;
        char topic[128];
        snprintf(topic, sizeof(topic), "%s/pairing/state", g_cfg.mqtt_prefix);
        mqtt_publish(topic, "OFF", true);
        printf("[DECT] Pairing window closed\n");
    }

    /* 2. Device Registration / Deletion */
    if (strstr(buf, "DEV_REGISTERED")) {
        int dev_id = 0;
        char *d_ptr = strstr(buf, "DEV_ID:");
        if (d_ptr) {
            dev_id = atoi(d_ptr + 7);
        } else {
            char *p = strstr(buf, "DEV_REGISTERED");
            if (p) {
                if (sscanf(p, "DEV_REGISTERED\n %d", &dev_id) != 1)
                    sscanf(p, "DEV_REGISTERED\r\n %d", &dev_id);
            }
        }
        printf("[+] Device registered: %d\n", dev_id);
        if (dev_id > 0) {
            dect_device_t *dev = find_or_create_device(dev_id);
            if (dev) {
                send_dect_cmd("GET_DEV_INFO\r\nDEV_INDEX: %d\r\n\r\n", dev_id);
            }
        }
        send_dect_cmd("GET_DEV_TABLE\r\nDEV_INDEX: 0\r\nHOW_MANY: 16\r\n\r\n");
    } else if (strstr(buf, "DEV_DELETED")) {
        int dev_id = 0;
        char *d_ptr = strstr(buf, "DEV_ID:");
        if (d_ptr) {
            dev_id = atoi(d_ptr + 7);
        } else {
            char *p = strstr(buf, "DEV_DELETED");
            if (p) {
                if (sscanf(p, "DEV_DELETED\n %d", &dev_id) != 1)
                    sscanf(p, "DEV_DELETED\r\n %d", &dev_id);
            }
        }
        printf("[-] Device deleted: %d\n", dev_id);
        if (dev_id > 0) {
            int valid_count = 0;
            for (int i = 0; i < g_dev_count; i++) {
                if (g_devs[i].valid && g_devs[i].dev_id == dev_id) {
                    g_devs[i].valid = false;
                }
                if (g_devs[i].valid) valid_count++;
            }
            char cnt_str[16];
            snprintf(cnt_str, sizeof(cnt_str), "%d", valid_count);
            char cnt_topic[128];
            snprintf(cnt_topic, sizeof(cnt_topic), "%s/devices/count", g_cfg.mqtt_prefix);
            mqtt_publish(cnt_topic, cnt_str, true);
        }
        send_dect_cmd("GET_DEV_TABLE\r\nDEV_INDEX: 0\r\nHOW_MANY: 16\r\n\r\n");
    }

    /* 3. DEV_TABLE Parsing */
    if (strstr(buf, "DEV_TABLE")) {
        int dev_idx = 0, no_devs = 0;
        char *p = strstr(buf, "DEV_INDEX:");
        if (p) dev_idx = atoi(p + 10);
        p = strstr(buf, "NO_OF_DEVICES:");
        if (p) no_devs = atoi(p + 14);

        if (g_cfg.verbose) {
            printf("[DECT] DEV_TABLE: idx=%d, count=%d\n", dev_idx, no_devs);
        }

        char cnt_str[16];
        snprintf(cnt_str, sizeof(cnt_str), "%d", no_devs);
        char cnt_topic[128];
        snprintf(cnt_topic, sizeof(cnt_topic), "%s/devices/count", g_cfg.mqtt_prefix);
        mqtt_publish(cnt_topic, cnt_str, true);

        /* Parse devices and units listed in DEV_TABLE */
        char *d_ptr = strstr(buf, "DEV_ID:");
        while (d_ptr) {
            int d_id = atoi(d_ptr + 7);
            if (d_id > 0) {
                dect_device_t *dev = find_or_create_device(d_id);
                /* Look for units under this device up to next DEV_ID or end */
                char *next_d = strstr(d_ptr + 7, "DEV_ID:");
                char *u_ptr = strstr(d_ptr, "UNIT_ID:");
                while (u_ptr && (!next_d || u_ptr < next_d)) {
                    int u_id = atoi(u_ptr + 8);
                    int u_type = 0;
                    char *t_ptr = strstr(u_ptr, "UNIT_TYPE:");
                    if (t_ptr && (!next_d || t_ptr < next_d)) {
                        u_type = atoi(t_ptr + 10);
                    }
                    if (u_id > 0 && dev) {
                        dect_unit_t *u = NULL;
                        for (int i = 0; i < dev->unit_count; i++) {
                            if (dev->units[i].unit_id == u_id) {
                                u = &dev->units[i];
                                break;
                            }
                        }
                        if (!u && dev->unit_count < MAX_UNITS) {
                            u = &dev->units[dev->unit_count++];
                            u->dev_id = d_id;
                            u->unit_id = u_id;
                            u->profile = u_type;
                            u->registered = true;
                            publish_ha_device_discovery(dev, u);
                            printf("[+] Discovered unit %d.%d (type 0x%04x) from DEV_TABLE\n", d_id, u_id, u_type);
                        } else if (u) {
                            if (u->profile != u_type) u->profile = u_type;
                            publish_ha_device_discovery(dev, u);
                        }
                    }
                    u_ptr = strstr(u_ptr + 8, "UNIT_ID:");
                }
            }
            d_ptr = strstr(d_ptr + 7, "DEV_ID:");
        }
    }

    /* 4. HAN-FUN Messages (FUN_MSG) */
    if (strstr(buf, "FUN_MSG")) {
        uint32_t src_dev = 0, src_unit = 0, dst_dev = 0, dst_unit = 0;
        uint32_t msg_seq = 0, msgtype = 0, intrf_type = 0, intrf_id = 0, intrf_member = 0;
        uint32_t datalen = 0;
        char data_hex[256] = {0};

        /* Parse line by line using local buffer copy */
        char buf_copy[2048];
        strncpy(buf_copy, buf, sizeof(buf_copy) - 1);
        buf_copy[sizeof(buf_copy) - 1] = '\0';

        char *saveptr = NULL;
        char *line = strtok_r(buf_copy, "\r\n", &saveptr);
        while (line) {
            char *colon = strchr(line, ':');
            if (colon) {
                *colon = '\0';
                char *key = line;
                char *val = colon + 1;
                while (*key == ' ' || *key == '\t') key++;
                char *key_end = key + strlen(key);
                while (key_end > key && (*(key_end - 1) == ' ' || *(key_end - 1) == '\t')) {
                    *(--key_end) = '\0';
                }
                while (*val == ' ' || *val == '\t') val++;
                char *val_end = val + strlen(val);
                while (val_end > val && (*(val_end - 1) == ' ' || *(val_end - 1) == '\t')) {
                    *(--val_end) = '\0';
                }

                if (strcmp(key, "SRC_DEV_ID") == 0) src_dev = strtoul(val, NULL, 10);
                else if (strcmp(key, "SRC_UNIT_ID") == 0) src_unit = strtoul(val, NULL, 10);
                else if (strcmp(key, "DST_DEV_ID") == 0) dst_dev = strtoul(val, NULL, 10);
                else if (strcmp(key, "DST_UNIT_ID") == 0) dst_unit = strtoul(val, NULL, 10);
                else if (strcmp(key, "MSG_SEQ") == 0) msg_seq = strtoul(val, NULL, 10);
                else if (strcmp(key, "MSGTYPE") == 0) msgtype = strtoul(val, NULL, 10);
                else if (strcmp(key, "INTRF_TYPE") == 0) intrf_type = strtoul(val, NULL, 10);
                else if (strcmp(key, "INTRF_ID") == 0) intrf_id = strtoul(val, NULL, 0);
                else if (strcmp(key, "INTRF_MEMBER") == 0) intrf_member = strtoul(val, NULL, 0);
                else if (strcmp(key, "DATALEN") == 0) datalen = strtoul(val, NULL, 10);
                else if (strcmp(key, "DATA") == 0) strncpy(data_hex, val, sizeof(data_hex) - 1);
            }
            line = strtok_r(NULL, "\r\n", &saveptr);
        }
        (void)dst_dev; (void)dst_unit; (void)datalen;

        printf("[DECT MSG] FUN_MSG: src=%u.%u intrf=0x%04x (%u) member=%u type=%u seq=%u\n",
               src_dev, src_unit, intrf_id, intrf_id, intrf_member, msgtype, msg_seq);

        /* Send HAN-FUN Response / ACK back to the device to confirm receipt (turns LED green) */
        if (src_dev > 0 && msgtype == 1) {
            send_dect_cmd(
                "FUN_MSG\r\n"
                " SRC_DEV_ID: 0\r\n"
                " SRC_UNIT_ID: %u\r\n"
                " DST_DEV_ID: %u\r\n"
                " DST_UNIT_ID: %u\r\n"
                " DEST_ADDRESS_TYPE: 1\r\n"
                " MSG_TRANSPORT: 1\r\n"
                " MSG_SEQ: %u\r\n"
                " MSGTYPE: 2\r\n"
                " INTRF_TYPE: %u\r\n"
                " INTRF_ID: %u\r\n"
                " INTRF_MEMBER: %u\r\n"
                " DATALEN: 0\r\n"
                " DATA: \r\n\r\n",
                dst_unit ? dst_unit : 1,
                src_dev,
                src_unit,
                msg_seq,
                intrf_type,
                intrf_id,
                intrf_member
            );
        }

        if (src_dev > 0) {
            dect_device_t *dev = find_or_create_device(src_dev);
            dect_unit_t *u = NULL;
            if (dev) {
                for (int i = 0; i < dev->unit_count; i++) {
                    if (dev->units[i].unit_id == src_unit) {
                        u = &dev->units[i];
                        break;
                    }
                }
                if (!u && dev->unit_count < MAX_UNITS) {
                    u = &dev->units[dev->unit_count++];
                    u->dev_id = src_dev;
                    u->unit_id = src_unit;
                    u->profile = (intrf_id == 772 || intrf_id == 0x0304) ? 0x0111 : intrf_id;
                    u->registered = true;
                    publish_ha_device_discovery(dev, u);
                }
            }

            char state_topic[128];
            snprintf(state_topic, sizeof(state_topic), "%s/%u/%u/state",
                     g_cfg.mqtt_prefix, src_dev, src_unit);

            /* Profile 0x0200: Simple On/Off */
            if (intrf_id == 0x0200 || intrf_id == 512) {
                bool on = (intrf_member == 2); /* 1 = OFF, 2 = ON */
                mqtt_publish(state_topic, on ? "ON" : "OFF", true);
                if (u) {
                    u->state_known = true;
                    u->bool_state = on;
                }
            }
            /* Profile 0x0202: Simple Detector (Door/Window, Motion) */
            else if (intrf_id == 0x0202 || intrf_id == 514) {
                /* Member 1: Alert / status report */
                bool alert = (data_hex[0] == '1' || intrf_member == 1);
                mqtt_publish(state_topic, alert ? "ON" : "OFF", true);
                if (u) {
                    u->state_known = true;
                    u->bool_state = alert;
                }
            }
            /* Profile 0x0204: Simple Temperature */
            else if (intrf_id == 0x0204 || intrf_id == 516) {
                int raw_temp = (int)strtol(data_hex, NULL, 16);
                float temp_c = (float)raw_temp / 10.0f;
                char temp_str[32];
                snprintf(temp_str, sizeof(temp_str), "%.1f", temp_c);
                char temp_topic[128];
                snprintf(temp_topic, sizeof(temp_topic), "%s/%u/%u/temperature",
                         g_cfg.mqtt_prefix, src_dev, src_unit);
                mqtt_publish(temp_topic, temp_str, true);
            }
            /* Profile 0x0111 / Interface 0x0304 (772): Simple Button / Key Event */
            else if (intrf_id == 0x0111 || intrf_id == 273 || intrf_id == 0x0304 || intrf_id == 772) {
                const char *ev_type = "press";
                if (intrf_member == 1) ev_type = "press";
                else if (intrf_member == 2) ev_type = "long_press";
                else if (intrf_member == 4) ev_type = "double_press";
                else if (intrf_member == 3) ev_type = "triple_press";

                printf("[+] Button event: dev=%u, unit=%u, event=%s (member=%u)\n",
                       src_dev, src_unit, ev_type, intrf_member);

                char ev_topic[128], ev_payload[64];
                snprintf(ev_topic, sizeof(ev_topic), "%s/%u/%u/event",
                         g_cfg.mqtt_prefix, src_dev, src_unit);
                snprintf(ev_payload, sizeof(ev_payload), "{\"event_type\":\"%s\"}", ev_type);
                mqtt_publish(ev_topic, ev_payload, false);

                /* Turn binary sensor ON (off_delay in HA will auto-clear it) */
                mqtt_publish(state_topic, "ON", false);
            }
        }
    }
}

/* --------------------------------------------------------------------------
 * MQTT Command Router
 * -------------------------------------------------------------------------- */

static void handle_mqtt_publish(const char *topic, const char *payload) {
    if (g_cfg.verbose) {
        printf("[MQTT RECV] %s -> %s\n", topic, payload);
    }

    /* 1. Pairing command */
    char pair_cmd_topic[128];
    snprintf(pair_cmd_topic, sizeof(pair_cmd_topic), "%s/pairing/set", g_cfg.mqtt_prefix);
    if (strcmp(topic, pair_cmd_topic) == 0) {
        if (strcasecmp(payload, "ON") == 0 || strcmp(payload, "1") == 0) {
            send_dect_cmd("OPEN_REG\r\nTIME: 240\r\n\r\n");
            g_pairing_end_mono = get_monotonic_sec() + 240;
        } else if (strcasecmp(payload, "OFF") == 0 || strcmp(payload, "0") == 0) {
            send_dect_cmd("CLOSE_REG\r\n\r\n");
            g_pairing_end_mono = 0;
        } else {
            int sec = atoi(payload);
            if (sec > 0) {
                send_dect_cmd("OPEN_REG\r\nTIME: %d\r\n\r\n", sec);
                g_pairing_end_mono = get_monotonic_sec() + sec;
            }
        }
        return;
    }

    /* 2. Raw command passthrough */
    char raw_cmd_topic[128];
    snprintf(raw_cmd_topic, sizeof(raw_cmd_topic), "%s/raw/cmd", g_cfg.mqtt_prefix);
    if (strcmp(topic, raw_cmd_topic) == 0) {
        send_dect_cmd("%s\r\n\r\n", payload);
        return;
    }

    /* 3. Device commands: homebase/dect/<dev_id>/<unit_id>/set */
    uint32_t dev_id = 0, unit_id = 0;
    char sub_path[64];
    int n = sscanf(topic, "homebase/dect/%u/%u/%63s", &dev_id, &unit_id, sub_path);
    if (n == 3 && strcmp(sub_path, "set") == 0) {
        bool turn_on = (strcasecmp(payload, "ON") == 0 || strcmp(payload, "1") == 0);
        uint32_t member = turn_on ? 2 : 1; /* Member 2 = ON, Member 1 = OFF */

        /* Send FUN_MSG to device */
        send_dect_cmd(
            "FUN_MSG\r\n"
            " SRC_DEV_ID: 0\r\n"
            " SRC_UNIT_ID: 1\r\n"
            " DST_DEV_ID: %u\r\n"
            " DST_UNIT_ID: %u\r\n"
            " DEST_ADDRESS_TYPE: 1\r\n"
            " MSG_TRANSPORT: 1\r\n"
            " MSG_SEQ: %u\r\n"
            " MSGTYPE: 1\r\n"
            " INTRF_TYPE: 0\r\n"
            " INTRF_ID: 512\r\n"
            " INTRF_MEMBER: %u\r\n"
            " DATALEN: 0\r\n"
            " DATA: \r\n\r\n",
            dev_id, unit_id, g_msg_seq++, member
        );

        /* Optimistically publish state */
        char state_topic[128];
        snprintf(state_topic, sizeof(state_topic), "%s/%u/%u/state", g_cfg.mqtt_prefix, dev_id, unit_id);
        mqtt_publish(state_topic, turn_on ? "ON" : "OFF", true);
    }
}

/* --------------------------------------------------------------------------
 * Main Loop
 * -------------------------------------------------------------------------- */

static void print_usage(const char *prog) {
    printf("Usage: %s [options]\n", prog);
    printf("Options:\n");
    printf("  -c <file>      Configuration file (default /etc/dect/dect.conf)\n");
    printf("  -h <host>      MQTT broker host (default %s)\n", DEFAULT_MQTT_HOST);
    printf("  -p <port>      MQTT broker port (default %d)\n", DEFAULT_MQTT_PORT);
    printf("  -u <user>      MQTT username\n");
    printf("  -P <pass>      MQTT password\n");
    printf("  -t <prefix>    MQTT topic prefix (default %s)\n", DEFAULT_MQTT_PREFIX);
    printf("  -d <prefix>    Home Assistant discovery prefix (default %s)\n", DEFAULT_HA_PREFIX);
    printf("  -F             Run in foreground (do not daemonize)\n");
    printf("  -v             Verbose debug output\n");
    printf("  -?             Show this help\n");
}

int main(int argc, char **argv) {
    setvbuf(stdout, NULL, _IONBF, 0);
    setvbuf(stderr, NULL, _IONBF, 0);

    memset(&g_cfg, 0, sizeof(g_cfg));
    strncpy(g_cfg.mqtt_host, DEFAULT_MQTT_HOST, sizeof(g_cfg.mqtt_host) - 1);
    g_cfg.mqtt_port = DEFAULT_MQTT_PORT;
    strncpy(g_cfg.mqtt_prefix, DEFAULT_MQTT_PREFIX, sizeof(g_cfg.mqtt_prefix) - 1);
    strncpy(g_cfg.ha_prefix, DEFAULT_HA_PREFIX, sizeof(g_cfg.ha_prefix) - 1);
    strncpy(g_cfg.client_id, DEFAULT_CLIENT_ID, sizeof(g_cfg.client_id) - 1);
    g_cfg.keepalive_sec = 60;
    g_cfg.foreground = true;
    g_cfg.verbose = false;

    const char *cfg_path = "/etc/dect/dect.conf";

    int opt;
    while ((opt = getopt(argc, argv, "c:h:p:u:P:t:d:Fv?")) != -1) {
        switch (opt) {
            case 'c': cfg_path = optarg; break;
            case 'h': strncpy(g_cfg.mqtt_host, optarg, sizeof(g_cfg.mqtt_host) - 1); break;
            case 'p': g_cfg.mqtt_port = atoi(optarg); break;
            case 'u': strncpy(g_cfg.mqtt_user, optarg, sizeof(g_cfg.mqtt_user) - 1); break;
            case 'P': strncpy(g_cfg.mqtt_pass, optarg, sizeof(g_cfg.mqtt_pass) - 1); break;
            case 't': strncpy(g_cfg.mqtt_prefix, optarg, sizeof(g_cfg.mqtt_prefix) - 1); break;
            case 'd': strncpy(g_cfg.ha_prefix, optarg, sizeof(g_cfg.ha_prefix) - 1); break;
            case 'F': g_cfg.foreground = true; break;
            case 'v': g_cfg.verbose = true; break;
            case '?':
            default:
                print_usage(argv[0]);
                return 0;
        }
    }

    load_config_file(cfg_path);

    printf("====================================================\n");
    printf(" hb2_dectd: Home Base 2 DECT ULE <-> MQTT Bridge\n");
    printf(" Broker: %s:%d, Prefix: %s\n", g_cfg.mqtt_host, g_cfg.mqtt_port, g_cfg.mqtt_prefix);
    printf("====================================================\n");

    signal(SIGINT, sig_handler);
    signal(SIGTERM, sig_handler);
    signal(SIGPIPE, SIG_IGN);

    if (dect_udp_init() < 0) {
        return 1;
    }

    /* Initialize DECT baseband client stack */
    send_dect_cmd("INIT\r\nVERSION: 1\r\n\r\n");

    /* Request device table */
    send_dect_cmd("GET_DEV_TABLE\r\nDEV_INDEX: 0\r\nHOW_MANY: 16\r\n\r\n");

    uint64_t last_reconnect_attempt = 0;
    uint64_t last_dev_poll = get_monotonic_sec();

    while (g_running) {
        uint64_t now_mono = get_monotonic_sec();

        /* 1. Maintain MQTT Connection */
        if (!g_mqtt_connected) {
            if (now_mono - last_reconnect_attempt >= 5) {
                last_reconnect_attempt = now_mono;
                if (mqtt_connect() < 0) {
                    if (g_cfg.verbose) fprintf(stderr, "[-] Reconnect to MQTT failed, retry in 5s\n");
                }
            }
        } else {
            /* Keepalive ping */
            if (now_mono - g_last_ping_sent >= (uint64_t)(g_cfg.keepalive_sec / 2)) {
                mqtt_ping();
                g_last_ping_sent = now_mono;
            }
            if (now_mono - g_last_pkt_recv >= (uint64_t)(g_cfg.keepalive_sec * 2)) {
                if (g_cfg.verbose) fprintf(stderr, "[-] MQTT keepalive timeout\n");
                mqtt_disconnect();
            }
        }

        /* 2. Periodic Poll of DECT Device Table (every 60s) */
        if (now_mono - last_dev_poll >= 60) {
            last_dev_poll = now_mono;
            send_dect_cmd("GET_DEV_TABLE\r\nDEV_INDEX: 0\r\nHOW_MANY: 16\r\n\r\n");
        }

        /* 3. Handle pairing timeout */
        if (g_pairing_active && g_pairing_end_mono > 0 && now_mono >= g_pairing_end_mono) {
            g_pairing_active = false;
            g_pairing_end_mono = 0;
            char topic[128];
            snprintf(topic, sizeof(topic), "%s/pairing/state", g_cfg.mqtt_prefix);
            mqtt_publish(topic, "OFF", true);
            printf("[DECT] Pairing window closed (timeout)\n");
        }

        /* 4. Poll Sockets */
        struct pollfd fds[2];
        int nfds = 0;

        /* MQTT Socket */
        int mqtt_idx = -1;
        if (g_mqtt_connected && g_mqtt_fd >= 0) {
            mqtt_idx = nfds++;
            fds[mqtt_idx].fd = g_mqtt_fd;
            fds[mqtt_idx].events = POLLIN;
            fds[mqtt_idx].revents = 0;
        }

        /* DECT UDP Socket */
        int dect_idx = nfds++;
        fds[dect_idx].fd = g_dect_fd;
        fds[dect_idx].events = POLLIN;
        fds[dect_idx].revents = 0;

        int ret = poll(fds, nfds, 1000);
        if (ret < 0) {
            if (errno == EINTR) continue;
            perror("[-] poll error");
            break;
        }

        if (ret == 0) continue;

        /* Process MQTT Socket */
        if (mqtt_idx >= 0 && (fds[mqtt_idx].revents & (POLLIN | POLLERR | POLLHUP))) {
            handle_mqtt_incoming_bytes();
        }

        /* Process DECT UDP Socket */
        if (fds[dect_idx].revents & POLLIN) {
            char rx_buf[BUFFER_SIZE];
            struct sockaddr_in from;
            socklen_t fromlen = sizeof(from);
            int n = recvfrom(g_dect_fd, rx_buf, sizeof(rx_buf) - 1, 0,
                             (struct sockaddr *)&from, &fromlen);
            if (n > 0) {
                handle_dect_packet(rx_buf, n);
            }
        }
    }

    printf("\n[*] Shutting down hb2_dectd...\n");
    if (g_mqtt_connected) {
        char stat_topic[128];
        snprintf(stat_topic, sizeof(stat_topic), "%s/status", g_cfg.mqtt_prefix);
        mqtt_publish(stat_topic, "offline", true);
        mqtt_disconnect();
    }
    if (g_dect_fd >= 0) {
        close(g_dect_fd);
    }
    printf("[+] Stopped cleanly.\n");
    return 0;
}
