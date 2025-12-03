/**
 * Tony Givargis
 * Copyright (C), 2023-2025
 * University of California, Irvine
 *
 * CS 238P - Operating Systems
 * main.c
 */

#include <signal.h>
#include "system.h"
#include <pthread.h>

/**
 * Needs:
 *   signal()
 */

static volatile int done;

static void
_signal_(int signum)
{
	assert( SIGINT == signum );

	done = 1;
}

void *network_throughput_fn(void *arg) {
    const char *iface = (const char *)arg;
    static unsigned long prev_rx = 0, prev_tx = 0;
    unsigned long curr_rx, curr_tx;
    double rx_rate, tx_rate;

    FILE *file;
    char line[512], dev[32];

    while (!done) {
        file = fopen("/proc/net/dev", "r");
        if (!file) { perror("fopen"); return NULL; }

        if (fgets(line, sizeof(line), file) == NULL) {
			fclose(file);
			return NULL;
		}
		if (fgets(line, sizeof(line), file) == NULL) {
			fclose(file);
			return NULL;
		}
        while (fgets(line, sizeof(line), file)) {
            unsigned long rx_bytes, tx_bytes;
            sscanf(line, "%31s %lu %*u %*u %*u %*u %*u %*u %*u %lu",
                   dev, &rx_bytes, &tx_bytes);

            dev[strlen(dev)-1] = '\0';
            if (strcmp(dev, iface) == 0) {
                curr_rx = rx_bytes;
                curr_tx = tx_bytes;

                rx_rate = (curr_rx - prev_rx) / 0.5;
                tx_rate = (curr_tx - prev_tx) / 0.5;

                printf("\rNet %s: RX %.2f KB/s | TX %.2f KB/s",
                       dev, rx_rate/1024.0, tx_rate/1024.0);
                fflush(stdout);

                prev_rx = curr_rx;
                prev_tx = curr_tx;
                break;
            }
        }
        fclose(file);
        us_sleep(500000);
    }
    return NULL;
}

void *disk_throughput_fn(void *arg) {
    FILE *file = (FILE *)arg;
    static unsigned long prev_read = 0;
    unsigned long curr_read;
    const int sector_size = 512;

    char line[256], dev[32];
    unsigned long reads, rd_merged, rd_sectors, rd_time;
    unsigned long writes, wr_merged, wr_sectors, wr_time;
    unsigned long in_progress, io_time, weighted_io_time;
    int major, minor;
    double throughput;

    while (fgets(line, sizeof(line), file)) {
        int fields = sscanf(line,
            "%d %d %31s %lu %lu %lu %lu %lu %lu %lu %lu %lu %lu %lu",
            &major, &minor, dev,
            &reads, &rd_merged, &rd_sectors, &rd_time,
            &writes, &wr_merged, &wr_sectors, &wr_time,
            &in_progress, &io_time, &weighted_io_time);

        if (fields >= 14 && strcmp(dev, "sda") == 0) {
            curr_read = rd_sectors;
            throughput = (curr_read - prev_read) * sector_size / 0.5;
            printf("\rDisk Read Throughput: %.2f KB/s", throughput / 1024.0);
            fflush(stdout);
            prev_read = curr_read;
            break;
        }
    }
    return NULL;
}

int main(void) {
    const char * const PROC_STAT = "/proc/diskstats";
    pthread_t disk_thread;
	pthread_t net_thread;
    FILE *file;

    if (SIG_ERR == signal(SIGINT, _signal_)) {
        perror("signal");
        return -1;
    }

    while (!done) {
        file = fopen(PROC_STAT, "r");
        if (!file) { perror("fopen"); return -1; }
		pthread_create(&net_thread, NULL, network_throughput_fn, "ens3");
		printf("\n");
        pthread_create(&disk_thread, NULL, disk_throughput_fn, file);
        pthread_join(disk_thread, NULL);
		pthread_join(net_thread, NULL);
        fclose(file);
        us_sleep(500000);
    }

    printf("\nDone!\n");
    return 0;
}