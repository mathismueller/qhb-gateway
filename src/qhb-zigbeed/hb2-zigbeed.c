/*
 * hb2-zigbeed: ASHv2-to-EZSP-SPI Translation Daemon for Qivicon Home Base 2
 *
 * Bridges Home Assistant (ZHA / bellows) and Zigbee2MQTT speaking ASHv2 over
 * TCP to the Silicon Labs EM3587 NCP connected via SPI (/dev/ttyspi0).
 *
 * Hardware: HiSilicon SD5650T SoC + Silabs EM3587 (Cortex-M3, EmberZNet 5.8.1)
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <stdbool.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <signal.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <arpa/inet.h>
#include <poll.h>
#include <time.h>

#define HB2_IOCTL_NCP_RESET   0x00007300
#define HB2_IOCTL_SYNC_CHECK  0x80047302

#define ASH_FLAG_BYTE         0x7E
#define ASH_ESCAPE_BYTE       0x7D
#define ASH_CANCEL_BYTE       0x1A
#define ASH_SUBST_BYTE        0x18
#define ASH_XON_BYTE          0x11
#define ASH_XOFF_BYTE         0x13
#define ASH_STUFF_MASK        0x20

#define ASH_CTRL_RST          0xC0
#define ASH_CTRL_RSTACK       0xC1
#define ASH_CTRL_ERROR        0xC2

#define LFSR_POLY             0xB8
#define LFSR_SEED             0x42

#define BUFFER_SIZE           1024

static volatile bool g_running = true;
static int g_verbose = 0;

static void sig_handler(int sig) {
    (void)sig;
    g_running = false;
}

static void log_hexdump(const char *prefix, const uint8_t *data, size_t len) {
    if (!g_verbose) return;
    printf("%s [%zu B]:", prefix, len);
    for (size_t i = 0; i < len; i++) {
        printf(" %02x", data[i]);
    }
    printf("\n");
    fflush(stdout);
}

/* LFSR pseudo-random sequence generator used by ASH */
static uint8_t ash_randomize(uint8_t seed, uint8_t *buf, size_t len) {
    if (seed == 0) {
        seed = LFSR_SEED;
    }
    for (size_t i = 0; i < len; i++) {
        buf[i] ^= seed;
        seed = (seed & 1) ? ((seed >> 1) ^ LFSR_POLY) : (seed >> 1);
    }
    return seed;
}

/* 16-bit CRC-CCITT (hqx) bitwise calculation */
static uint16_t ash_crc16(const uint8_t *data, size_t len) {
    uint16_t crc = 0xFFFF;
    for (size_t i = 0; i < len; i++) {
        uint8_t x = ((crc >> 8) ^ data[i]) & 0xFF;
        x ^= x >> 4;
        crc = (crc << 8) ^ ((uint16_t)x << 12) ^ ((uint16_t)x << 5) ^ (uint16_t)x;
    }
    return crc;
}

static bool ash_is_reserved(uint8_t b) {
    return (b == ASH_FLAG_BYTE || b == ASH_ESCAPE_BYTE ||
            b == ASH_XON_BYTE || b == ASH_XOFF_BYTE ||
            b == ASH_SUBST_BYTE || b == ASH_CANCEL_BYTE);
}

/* Hardware reset and synchronization with EM3587 NCP over SPI */
static int hb2_reset_ncp(int spi_fd) {
    if (g_verbose) printf("[+] Triggering EM3587 hardware reset (ioctl 0x7300)...\n");
    if (ioctl(spi_fd, HB2_IOCTL_NCP_RESET, 0) < 0) {
        perror("[-] ioctl HB2_IOCTL_NCP_RESET failed");
        return -1;
    }
    usleep(150000); // 150ms NCP startup time

    uint32_t sync_val = 0;
    for (int i = 0; i < 25; i++) {
        if (ioctl(spi_fd, HB2_IOCTL_SYNC_CHECK, &sync_val) == 0) {
            if (sync_val == 0x00010001) {
                if (g_verbose) printf("[+] NCP synchronized successfully (sync_val=0x%08x)\n", sync_val);
                return 0;
            }
        }
        usleep(20000); // 20ms poll interval
    }
    fprintf(stderr, "[-] NCP sync timeout! Last sync_val=0x%08x\n", sync_val);
    return -1;
}

/* Transmit framed and escaped ASH packet to TCP client */
static int send_ash_frame(int client_fd, const uint8_t *payload, size_t payload_len) {
    uint8_t out[BUFFER_SIZE * 2];
    size_t out_idx = 0;

    out[out_idx++] = ASH_FLAG_BYTE;
    for (size_t i = 0; i < payload_len; i++) {
        uint8_t b = payload[i];
        if (ash_is_reserved(b)) {
            out[out_idx++] = ASH_ESCAPE_BYTE;
            out[out_idx++] = b ^ ASH_STUFF_MASK;
        } else {
            out[out_idx++] = b;
        }
    }
    out[out_idx++] = ASH_FLAG_BYTE;

    log_hexdump("[TCP TX Raw]", out, out_idx);
    ssize_t sent = write(client_fd, out, out_idx);
    if (sent != (ssize_t)out_idx) {
        perror("[-] write to client failed");
        return -1;
    }
    return 0;
}

/* Send ASH RSTACK (Reset Acknowledge) packet */
static int send_ash_rstack(int client_fd, uint8_t reset_reason) {
    uint8_t raw[5];
    raw[0] = ASH_CTRL_RSTACK;
    raw[1] = 0x02; // ASH protocol version 2
    raw[2] = reset_reason; // e.g. 0x02 = RESET_POWER_ON, 0x03 = RESET_SOFTWARE

    uint16_t crc = ash_crc16(raw, 3);
    raw[3] = (crc >> 8) & 0xFF;
    raw[4] = crc & 0xFF;

    if (g_verbose) printf("[+] Sending ASH RSTACK (v2, reason 0x%02x, CRC 0x%04x)\n", reset_reason, crc);
    return send_ash_frame(client_fd, raw, 5);
}

/* Send ASH ACK packet */
static int send_ash_ack(int client_fd, uint8_t rx_seq) {
    uint8_t raw[3];
    raw[0] = 0x80 | (rx_seq & 0x07);
    uint16_t crc = ash_crc16(raw, 1);
    raw[1] = (crc >> 8) & 0xFF;
    raw[2] = crc & 0xFF;

    if (g_verbose) printf("[+] Sending ASH ACK (rx_seq=%u, CRC=0x%04x)\n", rx_seq, crc);
    return send_ash_frame(client_fd, raw, 3);
}

/* Send ASH NAK packet */
static int send_ash_nak(int client_fd, uint8_t rx_seq) {
    uint8_t raw[3];
    raw[0] = 0xA0 | (rx_seq & 0x07);
    uint16_t crc = ash_crc16(raw, 1);
    raw[1] = (crc >> 8) & 0xFF;
    raw[2] = crc & 0xFF;

    if (g_verbose) printf("[!] Sending ASH NAK (rx_seq=%u, CRC=0x%04x)\n", rx_seq, crc);
    return send_ash_frame(client_fd, raw, 3);
}

/* Forward raw EZSP payload to EM3587 over SPI via hisi_spi2tty */
static int forward_to_spi(int spi_fd, const uint8_t *ezsp_payload, size_t ezsp_len) {
    if (ezsp_len > 250) {
        fprintf(stderr, "[-] EZSP frame too long (%zu B)\n", ezsp_len);
        return -1;
    }

    uint8_t spi_tx[BUFFER_SIZE];
    spi_tx[0] = 0xFE; // SPI command prefix
    spi_tx[1] = (uint8_t)ezsp_len;
    memcpy(&spi_tx[2], ezsp_payload, ezsp_len);
    spi_tx[2 + ezsp_len] = 0xA7; // SPI frame terminator

    size_t total_len = ezsp_len + 3;
    log_hexdump("[SPI TX]", spi_tx, total_len);

    ssize_t written = write(spi_fd, spi_tx, total_len);
    if (written != (ssize_t)total_len) {
        perror("[-] write to /dev/ttyspi0 failed");
        return -1;
    }
    return 0;
}

static uint32_t get_time_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint32_t)(ts.tv_sec * 1000 + ts.tv_nsec / 1000000);
}

static void send_cb_req(int spi_fd, bool *cb_in_flight, uint32_t *cb_sent_ms) {
    uint8_t cb_cmd[] = { 0xFE, 0x03, 0xFF, 0x00, 0x06, 0xA7 };
    ssize_t written = write(spi_fd, cb_cmd, sizeof(cb_cmd));
    if (written == (ssize_t)sizeof(cb_cmd)) {
        *cb_in_flight = true;
        *cb_sent_ms = get_time_ms();
        if (g_verbose > 1) printf("[*] Sent EZSP callback request to NCP\n");
    }
}


int main(int argc, char *argv[]) {

    const char *spi_dev = "/dev/ttyspi0";
    int tcp_port = 2002;
    int opt;

    setvbuf(stdout, NULL, _IONBF, 0);
    setvbuf(stderr, NULL, _IONBF, 0);

    while ((opt = getopt(argc, argv, "d:p:vh")) != -1) {

        switch (opt) {
            case 'd': spi_dev = optarg; break;
            case 'p': tcp_port = atoi(optarg); break;
            case 'v': g_verbose++; break;
            case 'h':
            default:
                fprintf(stderr, "Usage: %s [-d <device>] [-p <port>] [-v]\n", argv[0]);
                fprintf(stderr, "  -d <device>   SPI TTY device (default: /dev/ttyspi0)\n");
                fprintf(stderr, "  -p <port>     TCP listen port (default: 2002)\n");
                fprintf(stderr, "  -v            Verbose output\n");
                return 1;
        }
    }

    signal(SIGINT, sig_handler);
    signal(SIGTERM, sig_handler);
    signal(SIGPIPE, SIG_IGN);

    printf("[*] Starting hb2-zigbeed on port %d -> %s (verbose=%d)\n", tcp_port, spi_dev, g_verbose);

    // Open SPI TTY device
    int spi_fd = open(spi_dev, O_RDWR | O_NOCTTY | O_NONBLOCK);
    if (spi_fd < 0) {
        perror("[-] Failed to open SPI device");
        return 1;
    }

    // Initial hardware reset
    hb2_reset_ncp(spi_fd);

    // Create TCP listening socket
    int listen_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (listen_fd < 0) {
        perror("[-] socket creation failed");
        close(spi_fd);
        return 1;
    }

    int reuse = 1;
    setsockopt(listen_fd, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse));

    struct sockaddr_in serv_addr;
    memset(&serv_addr, 0, sizeof(serv_addr));
    serv_addr.sin_family = AF_INET;
    serv_addr.sin_addr.s_addr = htonl(INADDR_ANY);
    serv_addr.sin_port = htons((uint16_t)tcp_port);

    if (bind(listen_fd, (struct sockaddr *)&serv_addr, sizeof(serv_addr)) < 0) {
        perror("[-] bind failed");
        close(listen_fd);
        close(spi_fd);
        return 1;
    }

    if (listen(listen_fd, 1) < 0) {
        perror("[-] listen failed");
        close(listen_fd);
        close(spi_fd);
        return 1;
    }

    printf("[*] Listening on 0.0.0.0:%d, ready for connections.\n", tcp_port);

    int client_fd = -1;
    uint8_t tcp_rx_raw[BUFFER_SIZE];
    size_t tcp_rx_raw_len = 0;

    uint8_t spi_rx_buf[BUFFER_SIZE];
    size_t spi_rx_len = 0;

    uint8_t tx_seq = 0;
    uint8_t rx_seq = 0;
    bool cb_in_flight = false;
    uint32_t cb_sent_ms = 0;

    while (g_running) {
        uint32_t now_ms = get_time_ms();
        if (cb_in_flight && (now_ms - cb_sent_ms > 1000)) {
            if (g_verbose) printf("[!] Callback request timed out after 1000ms\n");
            cb_in_flight = false;
        }

        struct pollfd fds[3];
        int nfds = 0;

        // fds[0]: Listen socket
        fds[nfds].fd = listen_fd;
        fds[nfds].events = POLLIN;
        int listen_idx = nfds++;

        // fds[1]: Client socket (if connected and not waiting for SPI callback response)
        int client_idx = -1;
        if (client_fd >= 0 && !cb_in_flight) {
            fds[nfds].fd = client_fd;
            fds[nfds].events = POLLIN;
            client_idx = nfds++;
        }

        // fds[2]: SPI TTY socket
        fds[nfds].fd = spi_fd;
        fds[nfds].events = POLLIN;
        int spi_idx = nfds++;

        int poll_timeout = (client_fd >= 0 && !cb_in_flight) ? 200 : 1000;
        int ret = poll(fds, nfds, poll_timeout);
        if (ret < 0) {
            if (errno == EINTR) continue;
            perror("[-] poll failed");
            break;
        }

        if (ret == 0 && client_fd >= 0 && !cb_in_flight) {
            send_cb_req(spi_fd, &cb_in_flight, &cb_sent_ms);
            continue;
        }

        // 1. New incoming TCP client
        if (fds[listen_idx].revents & POLLIN) {
            struct sockaddr_in client_addr;
            socklen_t addr_len = sizeof(client_addr);
            int new_fd = accept(listen_fd, (struct sockaddr *)&client_addr, &addr_len);
            if (new_fd >= 0) {
                char ip_str[INET_ADDRSTRLEN];
                inet_ntop(AF_INET, &client_addr.sin_addr, ip_str, sizeof(ip_str));
                printf("[+] Client connected from %s:%d\n", ip_str, ntohs(client_addr.sin_port));

                // Kick old client if present
                if (client_fd >= 0) {
                    printf("[!] Evicting previous client\n");
                    close(client_fd);
                }
                client_fd = new_fd;

                int flag = 1;
                setsockopt(client_fd, IPPROTO_TCP, TCP_NODELAY, &flag, sizeof(flag));

                // Reset state
                tcp_rx_raw_len = 0;
                spi_rx_len = 0;
                tx_seq = 0;
                rx_seq = 0;
                cb_in_flight = false;
                // Wait for explicit ASH RST frame from client before resetting hardware
            }

        }

        // 2. Data from TCP client
        if (client_idx >= 0 && (fds[client_idx].revents & (POLLIN | POLLERR | POLLHUP))) {
            if (fds[client_idx].revents & (POLLERR | POLLHUP)) {
                printf("[-] Client disconnected (HUP/ERR)\n");
                close(client_fd);
                client_fd = -1;
                cb_in_flight = false;
            } else {
                uint8_t chunk[256];
                ssize_t n = read(client_fd, chunk, sizeof(chunk));
                if (n <= 0) {
                    printf("[-] Client closed connection\n");
                    close(client_fd);
                    client_fd = -1;
                    cb_in_flight = false;
                } else {
                    log_hexdump("[TCP RX Chunk]", chunk, n);
                    for (ssize_t i = 0; i < n; i++) {
                        uint8_t b = chunk[i];
                        if (b == ASH_CANCEL_BYTE) {
                            if (g_verbose) printf("[!] Received ASH CANCEL byte\n");
                            tcp_rx_raw_len = 0;
                            continue;
                        }
                        if (b == ASH_SUBST_BYTE) {
                            if (g_verbose) printf("[!] Received ASH SUBSTITUTE byte\n");
                            tcp_rx_raw_len = 0;
                            continue;
                        }
                        if (b == ASH_FLAG_BYTE) {
                            if (tcp_rx_raw_len == 0) {
                                // Leading flag or empty delimiter
                                continue;
                            }
                            // We have a complete ASH frame in tcp_rx_raw[0 .. tcp_rx_raw_len - 1]
                            uint8_t unescaped[BUFFER_SIZE];
                            size_t unescaped_len = 0;
                            bool escape_next = false;
                            bool frame_err = false;

                            for (size_t j = 0; j < tcp_rx_raw_len; j++) {
                                uint8_t fb = tcp_rx_raw[j];
                                if (escape_next) {
                                    unescaped[unescaped_len++] = fb ^ ASH_STUFF_MASK;
                                    escape_next = false;
                                } else if (fb == ASH_ESCAPE_BYTE) {
                                    escape_next = true;
                                } else {
                                    unescaped[unescaped_len++] = fb;
                                }
                            }
                            tcp_rx_raw_len = 0; // Reset buffer for next frame

                            if (frame_err || escape_next) {
                                if (g_verbose) printf("[-] Escaping error in received frame\n");
                                continue;
                            }

                            if (unescaped_len < 3) {
                                if (g_verbose) printf("[-] Frame too short (%zu B)\n", unescaped_len);
                                continue;
                            }

                            log_hexdump("[ASH Frame Unescaped]", unescaped, unescaped_len);

                            // Verify CRC
                            uint16_t expected_crc = (unescaped[unescaped_len - 2] << 8) | unescaped[unescaped_len - 1];
                            uint16_t calc_crc = ash_crc16(unescaped, unescaped_len - 2);
                            if (calc_crc != expected_crc) {
                                printf("[-] ASH CRC mismatch: expected 0x%04x, calculated 0x%04x\n", expected_crc, calc_crc);
                                send_ash_nak(client_fd, rx_seq);
                                continue;
                            }

                            uint8_t ctrl = unescaped[0];
                            if (ctrl == ASH_CTRL_RST) {
                                printf("[+] Received ASH RST frame from host!\n");
                                hb2_reset_ncp(spi_fd);
                                tx_seq = 0;
                                rx_seq = 0;
                                cb_in_flight = false;
                                send_ash_rstack(client_fd, 0x02); // 0x02 = RESET_POWER_ON
                            } else if ((ctrl & 0x80) == 0) {
                                // DATA Frame
                                uint8_t frm_num = (ctrl >> 4) & 0x07;
                                uint8_t re_tx = (ctrl >> 3) & 0x01;
                                uint8_t ack_num = ctrl & 0x07;

                                size_t payload_len = unescaped_len - 3; // Subtract ctrl and 2 CRC bytes
                                uint8_t *payload = &unescaped[1];

                                if (g_verbose) {
                                    printf("[+] ASH DATA Frame: frm_num=%u, re_tx=%u, ack_num=%u, len=%zu\n",
                                           frm_num, re_tx, ack_num, payload_len);
                                }

                                if (frm_num == rx_seq) {
                                    rx_seq = (rx_seq + 1) & 0x07;
                                    // De-randomize payload
                                    ash_randomize(0, payload, payload_len);
                                    log_hexdump("[EZSP Payload Decoded]", payload, payload_len);

                                    // Forward to EM3587 over SPI
                                    forward_to_spi(spi_fd, payload, payload_len);

                                    // Send ACK back to host
                                    send_ash_ack(client_fd, rx_seq);
                                } else if (re_tx || frm_num == ((rx_seq - 1) & 0x07)) {
                                    if (g_verbose) printf("[!] Duplicate/retransmit DATA frame %u (expected %u), resending ACK\n", frm_num, rx_seq);
                                    send_ash_ack(client_fd, rx_seq);
                                } else {
                                    printf("[!] Unexpected DATA frm_num %u (expected %u), sending NAK\n", frm_num, rx_seq);
                                    send_ash_nak(client_fd, rx_seq);
                                }
                            } else if ((ctrl & 0xE0) == 0x80) {
                                // ACK Frame
                                uint8_t ack_num = ctrl & 0x07;
                                if (g_verbose) printf("[+] Received ASH ACK (ack_num=%u)\n", ack_num);
                            } else if ((ctrl & 0xE0) == 0xA0) {
                                // NAK Frame
                                uint8_t ack_num = ctrl & 0x07;
                                printf("[!] Received ASH NAK (ack_num=%u)\n", ack_num);
                            } else {
                                printf("[?] Unknown ASH control byte: 0x%02x\n", ctrl);
                            }
                        } else {
                            if (tcp_rx_raw_len < sizeof(tcp_rx_raw) - 1) {
                                tcp_rx_raw[tcp_rx_raw_len++] = b;
                            } else {
                                printf("[-] TCP RX buffer overflow, resetting\n");
                                tcp_rx_raw_len = 0;
                            }
                        }
                    }
                }
            }
        }

        // 3. Data from SPI TTY (/dev/ttyspi0)
        if (fds[spi_idx].revents & POLLIN) {
            uint8_t chunk[256];
            ssize_t n = read(spi_fd, chunk, sizeof(chunk));
            if (n > 0) {
                log_hexdump("[SPI RX Chunk]", chunk, n);
                if (spi_rx_len + n < sizeof(spi_rx_buf)) {
                    memcpy(&spi_rx_buf[spi_rx_len], chunk, n);
                    spi_rx_len += n;
                } else {
                    printf("[-] SPI RX buffer overflow, resetting\n");
                    spi_rx_len = 0;
                }

                // Process complete SPI frames: [0xFE, len, payload..., 0xA7]
                while (spi_rx_len >= 3) {
                    // Search for frame start byte (0xFE or 0xFD)
                    size_t start_idx = 0;
                    while (start_idx < spi_rx_len && spi_rx_buf[start_idx] != 0xFE && spi_rx_buf[start_idx] != 0xFD) {
                        start_idx++;
                    }
                    if (start_idx > 0) {
                        // Drop junk preceding frame start
                        memmove(spi_rx_buf, &spi_rx_buf[start_idx], spi_rx_len - start_idx);
                        spi_rx_len -= start_idx;
                    }

                    if (spi_rx_len < 3) break;

                    uint8_t frame_len = spi_rx_buf[1];
                    size_t total_expected = frame_len + 3;

                    if (spi_rx_len < total_expected) {
                        // Incomplete frame, wait for more data
                        break;
                    }

                    if (spi_rx_buf[total_expected - 1] != 0xA7) {
                        printf("[-] SPI frame missing 0xA7 terminator! (byte=0x%02x), discarding header\n",
                               spi_rx_buf[total_expected - 1]);
                        memmove(spi_rx_buf, &spi_rx_buf[1], spi_rx_len - 1);
                        spi_rx_len -= 1;
                        continue;
                    }

                    // Valid SPI frame extracted
                    uint8_t ezsp_payload[256];
                    memcpy(ezsp_payload, &spi_rx_buf[2], frame_len);

                    // Consume frame from buffer
                    memmove(spi_rx_buf, &spi_rx_buf[total_expected], spi_rx_len - total_expected);
                    spi_rx_len -= total_expected;

                    log_hexdump("[EZSP Response from NCP]", ezsp_payload, frame_len);

                    bool is_cb_resp = cb_in_flight;
                    cb_in_flight = false;

                    if (is_cb_resp) {
                        // If NCP responded with noCallbacks (0x07) or invalidCommand (0x58), discard
                        if (frame_len >= 3 && (ezsp_payload[2] == 0x07 || ezsp_payload[2] == 0x58)) {
                            if (g_verbose > 1) {
                                printf("[*] No callbacks pending on NCP (0x%02x)\n", ezsp_payload[2]);
                            }
                            continue;
                        }
                    }

                    if (client_fd >= 0) {
                        // Construct ASH DATA frame
                        uint8_t ash_payload[BUFFER_SIZE];
                        ash_payload[0] = (tx_seq << 4) | (0 << 3) | (rx_seq & 0x07);
                        tx_seq = (tx_seq + 1) & 0x07;

                        // Copy and randomize payload
                        memcpy(&ash_payload[1], ezsp_payload, frame_len);
                        ash_randomize(0, &ash_payload[1], frame_len);

                        // Compute CRC over control byte + randomized payload
                        uint16_t crc = ash_crc16(ash_payload, frame_len + 1);
                        ash_payload[frame_len + 1] = (crc >> 8) & 0xFF;
                        ash_payload[frame_len + 2] = crc & 0xFF;

                        if (g_verbose) {
                            printf("[+] Forwarding EZSP %s to TCP client (frame_id=0x%02x, tx_seq=%u, rx_seq=%u, len=%u, CRC=0x%04x)\n",
                                   is_cb_resp ? "Callback" : "Response",
                                   frame_len >= 3 ? ezsp_payload[2] : 0,
                                   (tx_seq - 1) & 7, rx_seq, frame_len, crc);
                        }

                        send_ash_frame(client_fd, ash_payload, frame_len + 3);

                        // If response indicates callbacks are pending (bit 2 of frame control byte), request next callback immediately!
                        if (frame_len >= 2 && (ezsp_payload[1] & 0x04)) {
                            if (g_verbose) printf("[*] NCP has pending callback, requesting\n");
                            send_cb_req(spi_fd, &cb_in_flight, &cb_sent_ms);
                        }
                    } else {
                        if (g_verbose) printf("[!] Received SPI response but no TCP client connected, dropping\n");
                    }
                }
            }
        }
    }

    printf("[*] Shutting down hb2-zigbeed...\n");
    if (client_fd >= 0) close(client_fd);
    close(listen_fd);
    close(spi_fd);
    return 0;
}
