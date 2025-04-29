#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

double get_process_cpu_time(pid_t pid)
{
    char path[256];
    FILE *fp;
    unsigned long utime, stime;
    snprintf(path, sizeof(path), "/proc/%d/stat", pid);
    fp = fopen(path, "r");
    if (!fp) {
        perror("fopen");
        return -1;
    }

    char buf[4096];
    fscanf(fp, "%4095[^\n]", buf);
    fclose(fp);

    char *token;
    int i;
    token = strtok(buf, " ");
    for (i = 1; i < 14; i++) {  // skip first 13 tokens
        token = strtok(NULL, " ");
    }

    utime = strtoul(token, NULL, 10);
    token = strtok(NULL, " ");
    stime = strtoul(token, NULL, 10);

    long clk_ticks = sysconf(_SC_CLK_TCK);
    return (utime + stime) / (double) clk_ticks;
}

int main()
{
    pid_t pid = getpid();
    printf("My PID: %d\n", pid);

    double prev_cpu_time = get_process_cpu_time(pid);
    time_t prev_time = time(NULL);

    while (1) {
        printf("Eason\n");

        sleep(1);  // wait 1 second

        double curr_cpu_time = get_process_cpu_time(pid);
        time_t curr_time = time(NULL);

        double delta_cpu = curr_cpu_time - prev_cpu_time;
        double delta_time = difftime(curr_time, prev_time);

        double cpu_usage = (delta_cpu / delta_time) * 100.0;

        printf("CPU Usage: %.2f%%\n", cpu_usage);

        prev_cpu_time = curr_cpu_time;
        prev_time = curr_time;
    }

    return 0;
}
