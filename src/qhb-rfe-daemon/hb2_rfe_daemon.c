/*
 * hb2_rfe_daemon.c
 *
 * Standalone HB-RF-ETH Raw-UART UDP Bridge Daemon for Home Base 2.
 * Bridges the onboard Silicon Labs Si1002 DualCoPro RF module on /dev/ttyAMA1
 * to OpenCCU / RaspberryMatic over UDP port 3008 using the HB-RF-ETH protocol.
 *
 * Enables simultaneous support for:
 *   - Classic HomeMatic (BidCos-RF 868 MHz)
 *   - Homematic IP (HmIP 868 MHz)
 *
 * Compatible with OpenCCU hb_rf_eth kernel driver (Protocol Version 2).
 */

#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <stdbool.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <signal.h>
#include <time.h>
#include <termios.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <sys/poll.h>
#include <netinet/in.h>
#include <arpa/inet.h>

#define HB_RF_ETH_PORT 3008
#define HB_RF_ETH_PROTOCOL_VERSION 2
#define BUFFER_SIZE 2048

static volatile sig_atomic_t g_running = 1;
static bool g_verbose = false;

static void sig_handler(int sig) {
    (void)sig;
    g_running = 0;
}

/* CRC-16 used by HB-RF-ETH (Poly: 0x8005, Init: 0xd77f) */
static uint16_t calc_crc(const unsigned char *buf, size_t len) {
    uint16_t crc = 0xd77f;
    size_t i;
    int b;

    for (i = 0; i < len; i++) {
        crc ^= ((uint16_t)buf[i]) << 8;
        for (b = 0; b < 8; b++) {
            if (crc & 0x8000) {
                crc = (crc << 1) ^ 0x8005;
            } else {
                crc = (crc << 1);
            }
        }
    }
    return crc;
}

static void dump_hex(const char *prefix, const unsigned char *buf, size_t len) {
    if (!g_verbose || len == 0) return;
    printf("%s (%zu bytes):", prefix, len);
    for (size_t i = 0; i < len; i++) {
        printf(" %02X", buf[i]);
    }
    printf("\n");
}

static int open_serial(const char *device, int baud) {
    int fd = open(device, O_RDWR | O_NOCTTY | O_NONBLOCK);
    if (fd < 0) {
        perror("open serial");
        return -1;
    }

    struct termios tty;
    memset(&tty, 0, sizeof(tty));
    if (tcgetattr(fd, &tty) != 0) {
        perror("tcgetattr");
        close(fd);
        return -1;
    }

    speed_t spd = B115200;
    if (baud == 57600) spd = B57600;
    else if (baud == 38400) spd = B38400;
    else if (baud == 19200) spd = B19200;
    else if (baud == 9600)  spd = B9600;

    cfsetispeed(&tty, spd);
    cfsetospeed(&tty, spd);

    /* 8N1 raw mode, no flow control */
    tty.c_cflag &= ~PARENB;
    tty.c_cflag &= ~CSTOPB;
    tty.c_cflag &= ~CSIZE;
    tty.c_cflag |= CS8;
    tty.c_cflag &= ~CRTSCTS;
    tty.c_cflag |= (CREAD | CLOCAL);

    tty.c_lflag &= ~(ICANON | ECHO | ECHOE | ISIG);
    tty.c_iflag &= ~(IXON | IXOFF | IXANY | IGNBRK | BRKINT | PARMRK | ISTRIP | INLCR | IGNCR | ICRNL);
    tty.c_oflag &= ~OPOST;

    tty.c_cc[VMIN]  = 0;
    tty.c_cc[VTIME] = 0;

    if (tcsetattr(fd, TCSANOW, &tty) != 0) {
        perror("tcsetattr");
        close(fd);
        return -1;
    }

    tcflush(fd, TCIOFLUSH);
    return fd;
}

static uint64_t get_time_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000 + (uint64_t)ts.tv_nsec / 1000000;
}

static void pulse_hardware_reset(int serial_fd) {
    if (g_verbose) printf("Asserting hardware reset on GPIO 12 (Pin 4 / C2CK)...\n");
    tcflush(serial_fd, TCIOFLUSH);
    system("/bin/hi_cli /home/cli/api/ecs/gpio/writebit -v gpio 12 level 0 >/dev/null 2>&1");
    usleep(50000); /* 50ms active-low pulse */
    system("/bin/hi_cli /home/cli/api/ecs/gpio/writebit -v gpio 12 level 1 >/dev/null 2>&1");
    usleep(50000); /* 50ms stabilization delay */
    tcflush(serial_fd, TCIOFLUSH);
    if (g_verbose) printf("Hardware reset released (GPIO 12 = 1).\n");
}

static void send_udp_msg(int sock_fd, const struct sockaddr_in *client_addr, uint8_t cmd, uint8_t *counter, const uint8_t *payload, size_t len) {
    if (!client_addr || client_addr->sin_port == 0) return;

    unsigned char buf[BUFFER_SIZE];
    buf[0] = cmd;
    buf[1] = (*counter)++;
    if (len > 0 && payload) {
        memcpy(buf + 2, payload, len);
    }
    size_t total_len = len + 4;
    uint16_t crc = calc_crc(buf, len + 2);
    buf[len + 2] = (crc >> 8) & 0xFF;
    buf[len + 3] = crc & 0xFF;

    sendto(sock_fd, buf, total_len, 0, (const struct sockaddr *)client_addr, sizeof(*client_addr));
}

/*
 * Calculate complete wire length of an eQ-3 / HomeMatic frame starting with 0xFD.
 * Properly accounts for eQ-3 byte escaping where 0xFD / 0xFC bytes are escaped
 * with the 0xFC escape prefix.
 *
 * Returns:
 *   > 0 : Total raw wire frame length in bytes
 *     0 : Incomplete frame, more bytes needed
 *    -1 : Invalid frame header (not 0xFD)
 */
static ssize_t get_complete_frame_len(const unsigned char *buf, size_t len) {
    if (len < 1 || buf[0] != 0xFD) return -1;

    size_t raw_pos = 1;
    size_t unescaped_count = 1; /* for buf[0] == 0xFD */
    size_t payload_len = 0;
    uint8_t len_bytes[2];
    size_t len_bytes_collected = 0;

    /* Decode the 2-byte payload length field (accounting for possible 0xFC escape) */
    while (raw_pos < len && len_bytes_collected < 2) {
        if (buf[raw_pos] == 0xFC) {
            if (raw_pos + 1 >= len) return 0; /* Incomplete escape sequence */
            len_bytes[len_bytes_collected++] = buf[raw_pos + 1] | 0x80;
            raw_pos += 2;
        } else {
            len_bytes[len_bytes_collected++] = buf[raw_pos];
            raw_pos += 1;
        }
        unescaped_count++;
    }

    if (len_bytes_collected < 2) return 0; /* Need more bytes for length field */

    payload_len = ((size_t)len_bytes[0] << 8) | len_bytes[1];
    size_t total_unescaped_needed = payload_len + 5; /* 1(FD) + 2(len) + payload + 2(CRC) */

    /* Scan until total_unescaped_needed bytes are collected */
    while (raw_pos < len && unescaped_count < total_unescaped_needed) {
        if (buf[raw_pos] == 0xFC) {
            if (raw_pos + 1 >= len) return 0; /* Incomplete escape sequence */
            raw_pos += 2;
        } else {
            raw_pos += 1;
        }
        unescaped_count++;
    }

    if (unescaped_count == total_unescaped_needed) {
        return (ssize_t)raw_pos;
    }

    return 0; /* Incomplete frame */
}

int main(int argc, char **argv) {
    const char *serial_dev = "/dev/ttyAMA1";
    int baud = 115200;
    int udp_port = HB_RF_ETH_PORT;

    int opt;
    while ((opt = getopt(argc, argv, "d:b:p:v")) != -1) {
        switch (opt) {
            case 'd': serial_dev = optarg; break;
            case 'b': baud = atoi(optarg); break;
            case 'p': udp_port = atoi(optarg); break;
            case 'v': g_verbose = true; break;
            default:
                fprintf(stderr, "Usage: %s [-d /dev/ttyAMA1] [-b 115200] [-p 3008] [-v]\n", argv[0]);
                return 1;
        }
    }

    signal(SIGTERM, sig_handler);
    signal(SIGINT, sig_handler);
    signal(SIGPIPE, SIG_IGN);

    setlinebuf(stdout);
    setlinebuf(stderr);

    printf("Starting HB-RF-ETH Daemon for Home Base 2...\n");
    printf("  Serial Device: %s @ %d baud\n", serial_dev, baud);
    printf("  UDP Port:      %d\n", udp_port);

    int serial_fd = open_serial(serial_dev, baud);
    if (serial_fd < 0) {
        fprintf(stderr, "Fatal: failed to open serial port %s\n", serial_dev);
        return 1;
    }

    /* Initialize and release hardware reset line */
    pulse_hardware_reset(serial_fd);

    int udp_fd = socket(AF_INET, SOCK_DGRAM, 0);
    if (udp_fd < 0) {
        perror("socket");
        close(serial_fd);
        return 1;
    }

    int reuse = 1;
    setsockopt(udp_fd, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse));

    /* Set non-blocking on UDP socket */
    int flags = fcntl(udp_fd, F_GETFL, 0);
    fcntl(udp_fd, F_SETFL, flags | O_NONBLOCK);

    struct sockaddr_in serv_addr;
    memset(&serv_addr, 0, sizeof(serv_addr));
    serv_addr.sin_family = AF_INET;
    serv_addr.sin_addr.s_addr = htonl(INADDR_ANY);
    serv_addr.sin_port = htons(udp_port);

    if (bind(udp_fd, (struct sockaddr *)&serv_addr, sizeof(serv_addr)) < 0) {
        perror("bind UDP");
        close(udp_fd);
        close(serial_fd);
        return 1;
    }

    printf("Listening on 0.0.0.0:%d (UDP). Ready for OpenCCU!\n", udp_port);

    struct sockaddr_in client_addr;
    memset(&client_addr, 0, sizeof(client_addr));
    bool connected = false;
    bool connection_started = false;
    uint8_t server_counter = 0;
    uint8_t endpoint_identifier = 2;
    uint64_t last_received_keepalive = 0;
    uint64_t next_send_keepalive = 0;

    struct pollfd pfds[2];
    pfds[0].fd = udp_fd;
    pfds[0].events = POLLIN;
    pfds[1].fd = serial_fd;
    pfds[1].events = POLLIN;

    unsigned char rx_buf[BUFFER_SIZE];
    unsigned char s_rx_buf[BUFFER_SIZE];
    size_t s_rx_len = 0;
    uint64_t s_last_byte_time = 0;

    while (g_running) {
        int poll_ret = poll(pfds, 2, 10); /* 10ms responsive timeout */
        uint64_t now = get_time_ms();

        if (poll_ret > 0) {
            /* 1. Handle incoming UDP packets from OpenCCU */
            if (pfds[0].revents & POLLIN) {
                struct sockaddr_in src_addr;
                socklen_t addr_len = sizeof(src_addr);
                ssize_t n = recvfrom(udp_fd, rx_buf, sizeof(rx_buf), 0, (struct sockaddr *)&src_addr, &addr_len);

                if (n >= 4) {
                    /* Verify CRC16 */
                    uint16_t expected_crc = ((uint16_t)rx_buf[n - 2] << 8) | rx_buf[n - 1];
                    uint16_t calculated_crc = calc_crc(rx_buf, n - 2);

                    if (expected_crc == calculated_crc) {
                        uint8_t cmd = rx_buf[0];
                        uint8_t client_cnt = rx_buf[1];

                        if (g_verbose) {
                            char ip_str[INET_ADDRSTRLEN];
                            inet_ntop(AF_INET, &src_addr.sin_addr, ip_str, sizeof(ip_str));
                            printf("[%llu] UDP from %s:%d: cmd=%d, len=%zd\n", (unsigned long long)now, ip_str, ntohs(src_addr.sin_port), cmd, n);
                        }

                        if (cmd == 0) {
                            /* CONNECT PACKET */
                            if (n == 6 && rx_buf[2] == HB_RF_ETH_PROTOCOL_VERSION) {
                                uint8_t req_endpoint = rx_buf[3];
                                if (req_endpoint == 0) {
                                    endpoint_identifier += 2;
                                    connection_started = false;
                                }
                                client_addr = src_addr;
                                connected = true;
                                last_received_keepalive = now;
                                next_send_keepalive = now + 1000;

                                char ip_str[INET_ADDRSTRLEN];
                                inet_ntop(AF_INET, &client_addr.sin_addr, ip_str, sizeof(ip_str));
                                printf("Connected from OpenCCU at %s:%d (Endpoint ID: %d)\n", ip_str, ntohs(client_addr.sin_port), endpoint_identifier);

                                /* Send Connect Response (7 bytes total):
                                 * [cmd=0, server_cnt, proto=2, client_cnt, endpoint_id, crc_hi, crc_lo]
                                 */
                                uint8_t resp[3];
                                resp[0] = HB_RF_ETH_PROTOCOL_VERSION;
                                resp[1] = client_cnt;
                                resp[2] = endpoint_identifier;
                                send_udp_msg(udp_fd, &client_addr, 0, &server_counter, resp, 3);
                            }
                        } else if (connected && src_addr.sin_addr.s_addr == client_addr.sin_addr.s_addr && src_addr.sin_port == client_addr.sin_port) {
                            last_received_keepalive = now;

                            switch (cmd) {
                                case 1: /* DISCONNECT */
                                    printf("OpenCCU disconnected.\n");
                                    connected = false;
                                    connection_started = false;
                                    s_rx_len = 0;
                                    memset(&client_addr, 0, sizeof(client_addr));
                                    break;

                                case 2: /* KEEPALIVE */
                                    /* Keepalive received, timestamp already refreshed */
                                    break;

                                case 4: /* RESET MODULE */
                                    if (g_verbose) printf("Reset / flush requested by driver\n");
                                    pulse_hardware_reset(serial_fd);
                                    s_rx_len = 0;
                                    break;

                                case 5: /* START CONNECTION */
                                    if (g_verbose) printf("Start connection (data streaming enabled)\n");
                                    connection_started = true;
                                    s_rx_len = 0;
                                    break;

                                case 6: /* END CONNECTION */
                                    if (g_verbose) printf("End connection\n");
                                    connection_started = false;
                                    s_rx_len = 0;
                                    break;

                                case 7: /* FRAME (UART data from OpenCCU to module) */
                                    if (n > 4) {
                                        size_t uart_len = n - 4;
                                        ssize_t written = write(serial_fd, rx_buf + 2, uart_len);
                                        if (g_verbose) {
                                            printf("Wrote %zd/%zu bytes to serial\n", written, uart_len);
                                            dump_hex("  TX data", rx_buf + 2, uart_len);
                                        }
                                    }
                                    break;

                                default:
                                    break;
                            }
                        }
                    } else if (g_verbose) {
                        fprintf(stderr, "Warning: CRC mismatch on UDP packet (exp 0x%04X, calc 0x%04X)\n", expected_crc, calculated_crc);
                    }
                }
            }

            /* 2. Handle incoming serial data from Si1002 module */
            if ((pfds[1].revents & POLLIN) && connected && connection_started) {
                ssize_t n = read(serial_fd, s_rx_buf + s_rx_len, sizeof(s_rx_buf) - s_rx_len);
                if (n > 0) {
                    s_rx_len += (size_t)n;
                    s_last_byte_time = now;
                }
            }

            /* Process and dispatch complete frames from s_rx_buf */
            if (connected && connection_started && s_rx_len > 0) {
                while (s_rx_len > 0) {
                    /* Discard or flush any leading noise before 0xFD */
                    if (s_rx_buf[0] != 0xFD) {
                        size_t skip = 1;
                        while (skip < s_rx_len && s_rx_buf[skip] != 0xFD) skip++;
                        if (now - s_last_byte_time > 10) {
                            if (g_verbose) dump_hex("  RX noise/raw", s_rx_buf, skip);
                            send_udp_msg(udp_fd, &client_addr, 7, &server_counter, s_rx_buf, skip);
                        }
                        memmove(s_rx_buf, s_rx_buf + skip, s_rx_len - skip);
                        s_rx_len -= skip;
                        continue;
                    }

                    /* 0xFD header found: check if complete frame is available */
                    ssize_t frame_len = get_complete_frame_len(s_rx_buf, s_rx_len);
                    if (frame_len > 0) {
                        if (g_verbose) {
                            printf("Forwarding complete frame (%zd bytes) to OpenCCU\n", frame_len);
                            dump_hex("  RX data", s_rx_buf, (size_t)frame_len);
                        }
                        send_udp_msg(udp_fd, &client_addr, 7, &server_counter, s_rx_buf, (size_t)frame_len);
                        memmove(s_rx_buf, s_rx_buf + frame_len, s_rx_len - (size_t)frame_len);
                        s_rx_len -= (size_t)frame_len;
                        continue;
                    }

                    /* Incomplete frame: if timeout exceeded, flush to prevent deadlock */
                    if (now - s_last_byte_time > 40) {
                        if (g_verbose) {
                            printf("Flushing partial frame (%zu bytes) after timeout\n", s_rx_len);
                            dump_hex("  RX data (partial)", s_rx_buf, s_rx_len);
                        }
                        send_udp_msg(udp_fd, &client_addr, 7, &server_counter, s_rx_buf, s_rx_len);
                        s_rx_len = 0;
                    }
                    break;
                }
            }
        }

        /* Check connection keepalive & timeouts */
        if (connected) {
            /* 5-second timeout */
            if (now > last_received_keepalive + 5000) {
                printf("OpenCCU connection timed out after 5s inactivity.\n");
                connected = false;
                connection_started = false;
                memset(&client_addr, 0, sizeof(client_addr));
            } else if (now >= next_send_keepalive) {
                /* Send keepalive every 1 second */
                next_send_keepalive = now + 1000;
                send_udp_msg(udp_fd, &client_addr, 2, &server_counter, NULL, 0);
            }
        }
    }

    printf("Shutting down HB-RF-ETH Daemon...\n");
    if (connected) {
        send_udp_msg(udp_fd, &client_addr, 1, &server_counter, NULL, 0);
    }
    close(udp_fd);
    close(serial_fd);
    return 0;
}
