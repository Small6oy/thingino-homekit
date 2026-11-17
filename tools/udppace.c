// udppace: send N small UDP packets at a fixed interval, to measure network jitter from the camera.
//   udppace <ip> <port> [count=300] [interval_ms=20] [tos=0]
// SPDX-License-Identifier: GPL-3.0-or-later
#include <arpa/inet.h>
#include <netinet/in.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

int main(int argc, char** argv) {
    if (argc < 3) {
        fprintf(stderr, "usage: %s ip port [count] [interval_ms] [tos]\n", argv[0]);
        return 1;
    }
    int count = argc > 3 ? atoi(argv[3]) : 300, ms = argc > 4 ? atoi(argv[4]) : 20, tos = argc > 5 ? atoi(argv[5]) : 0;
    int fd = socket(AF_INET, SOCK_DGRAM, 0);
    if (tos)
        setsockopt(fd, IPPROTO_IP, IP_TOS, &tos, sizeof tos);
    struct sockaddr_in a = { .sin_family = AF_INET, .sin_port = htons((uint16_t) atoi(argv[2])) };
    inet_pton(AF_INET, argv[1], &a.sin_addr);
    struct timespec next;
    clock_gettime(CLOCK_MONOTONIC, &next);
    char buf[100];
    memset(buf, 0, sizeof buf);
    for (int i = 0; i < count; i++) {
        snprintf(buf, sizeof buf, "%d", i);
        sendto(fd, buf, 100, 0, (struct sockaddr*) &a, sizeof a);
        next.tv_nsec += ms * 1000000L;
        while (next.tv_nsec >= 1000000000L) {
            next.tv_nsec -= 1000000000L;
            next.tv_sec++;
        }
        clock_nanosleep(CLOCK_MONOTONIC, TIMER_ABSTIME, &next, NULL);
    }
    return 0;
}
