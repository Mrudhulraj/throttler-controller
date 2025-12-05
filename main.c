/**
 * Tony Givargis
 * Copyright (C), 2023-2025
 * University of California, Irvine
 *
 * CS 238P - Operating Systems
 * main.c
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <pthread.h>
#include <unistd.h>
#include <signal.h>
#include <assert.h>

#define BUF_SIZE 16
#define IFACE "ens3"
#define DISK  "sda"
#define SECTOR_SIZE 512

static volatile int done = 0;

typedef struct {
    double cpu_usage;
    double rx_kbps;
    double tx_kbps;
    double disk_kbps;
} metrics_t;

metrics_t buffer[BUF_SIZE];
int head = 0, tail = 0, count = 0;

pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;
pthread_cond_t not_empty = PTHREAD_COND_INITIALIZER;
pthread_cond_t not_full  = PTHREAD_COND_INITIALIZER;

static void _signal_(int signum) {
    assert(signum == SIGINT);
    done = 1;
}

/* helper to read a single unsigned long from sysfs */
unsigned long read_stat(const char *path) {
    FILE *f;
    unsigned long val = 0;
    int nread;

    f = fopen(path, "r");
    if (!f) return 0;
    nread = fscanf(f, "%lu", &val);
    fclose(f);

    if (nread != 1) return 0;
    return val;
}

/* CPU usage producer helper */
double get_cpu_usage(void) {
    static unsigned long prev_user=0, prev_nice=0, prev_system=0, prev_idle=0;
    unsigned long user, nice, system, idle;
    unsigned long diff_user, diff_nice, diff_system, diff_idle, total;
    double usage;
    FILE *f;
    int nread;

    f = fopen("/proc/stat", "r");
    if (!f) return 0.0;

    nread = fscanf(f, "cpu %lu %lu %lu %lu", &user, &nice, &system, &idle);
    fclose(f);

    if (nread != 4) return 0.0;

    diff_user   = user   - prev_user;
    diff_nice   = nice   - prev_nice;
    diff_system = system - prev_system;
    diff_idle   = idle   - prev_idle;

    total = diff_user + diff_nice + diff_system + diff_idle;
    usage = total ? (100.0 * (diff_user + diff_nice + diff_system) / total) : 0.0;

    prev_user = user;
    prev_nice = nice;
    prev_system = system;
    prev_idle = idle;

    return usage;
}

/* Producer: network + disk */
void *producer(void *arg) {
    static unsigned long prev_rx=0, prev_tx=0;
    static unsigned long prev_rd=0, prev_wr=0;
    unsigned long rx_bytes, tx_bytes;
    unsigned long rd_sectors, wr_sectors;
    double rx_rate, tx_rate, disk_rate;
    char path_rx[128], path_tx[128], path_stat[128];
    FILE *f;
    int nread;

    (void)arg;

    /* Build sysfs paths with sprintf (safe: buffers are large enough) */
    sprintf(path_rx, "/sys/class/net/%s/statistics/rx_bytes", IFACE);
    sprintf(path_tx, "/sys/class/net/%s/statistics/tx_bytes", IFACE);
    sprintf(path_stat, "/sys/block/%s/stat", DISK);

    while (!done) {
        rx_bytes = read_stat(path_rx);
        tx_bytes = read_stat(path_tx);

        /* disk stats: read first two fields (rd_sectors, wr_sectors) */
        f = fopen(path_stat, "r");
        rd_sectors = wr_sectors = 0;
        if (f) {
            nread = fscanf(f, "%lu %*u %lu", &rd_sectors, &wr_sectors);
            fclose(f);
            if (nread != 2) {
                rd_sectors = wr_sectors = 0;
            }
        }

        rx_rate = (rx_bytes - prev_rx) / 1024.0; /* KB/s */
        tx_rate = (tx_bytes - prev_tx) / 1024.0;
        disk_rate = ((rd_sectors - prev_rd) + (wr_sectors - prev_wr))
                    * (SECTOR_SIZE / 1024.0); /* KB/s */

        prev_rx = rx_bytes;
        prev_tx = tx_bytes;
        prev_rd = rd_sectors;
        prev_wr = wr_sectors;

        pthread_mutex_lock(&lock);
        while (count == BUF_SIZE) pthread_cond_wait(&not_full, &lock);
        buffer[tail].cpu_usage = get_cpu_usage();
        buffer[tail].rx_kbps = rx_rate;
        buffer[tail].tx_kbps = tx_rate;
        buffer[tail].disk_kbps = disk_rate;
        tail = (tail+1) % BUF_SIZE;
        count++;
        pthread_cond_signal(&not_empty);
        pthread_mutex_unlock(&lock);

        sleep(1);
    }
    return NULL;
}

/* Consumer */
void *consumer(void *arg) {
    metrics_t m;

    (void)arg;  /* silence unused warning */

    while (!done) {
        pthread_mutex_lock(&lock);
        while (count == 0) pthread_cond_wait(&not_empty, &lock);
        m = buffer[head];
        head = (head+1) % BUF_SIZE;
        count--;
        pthread_cond_signal(&not_full);
        pthread_mutex_unlock(&lock);

        printf("\rCPU: %.1f%% | Net RX: %.2f KB/s | TX: %.2f KB/s | Disk: %.2f KB/s",
               m.cpu_usage, m.rx_kbps, m.tx_kbps, m.disk_kbps);
        fflush(stdout);

        /* Throttle if disk I/O > 10 MB/s OR CPU > 10% */
        if (m.disk_kbps > 10240.0 || m.cpu_usage > 10.0) {
            printf("\nThrottling: sleeping 5s (Disk %.2f KB/s, CPU %.1f%%)\n",
                   m.disk_kbps, m.cpu_usage);
            sleep(5);
        }
    }
    return NULL;
}

int main(void) {
    pthread_t prod, cons;

    if (SIG_ERR == signal(SIGINT, _signal_)) { perror("signal"); return -1; }

    pthread_create(&prod, NULL, producer, NULL);
    pthread_create(&cons, NULL, consumer, NULL);

    pthread_join(prod, NULL);
    pthread_join(cons, NULL);

    printf("\nDone!\n");
    return 0;
}