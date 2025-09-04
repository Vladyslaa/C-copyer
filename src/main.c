#include "core.h"
#include "taskQueue.h"

#include <stdio.h>
#include <string.h> 
#include <errno.h> 
#include <time.h>
#include <pthread.h>

#ifdef _WIN32
#include <windows.h>
#else //POSIX
#include <unistd.h>
#include <dirent.h>
#include <sys/stat.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#endif

int main(int argc, char *argv[]) {
    if (argc != 4){
        printf(BOLD RED "Usage: " RESET CYN "<source_dir> <destination_dir> <extension_filter> " WEAK "(\"all\" - for all types)\n" RESET);
        return 1;                                                           
    }                                                                       

    const char* source_dir = argv[1];
    const char* destination_dir = argv[2];
    const char* filter = argv[3];
    
    int num_threads;
#ifdef _WIN32
    SYSTEM_INFO sysInfo;
    GetSystemInfo(&sysInfo);
    num_threads = sysInfo.dwNumberOfProcessors; 
#else //POSIX
    long nproc = sysconf(_SC_NPROCESSORS_ONLN);
    if (nproc < 1) {
        perror("sysconf");
        return 1;
    }

    num_threads = nproc;
#endif
    printf(BLU "CPU Threads: Using %d worker threads\n" RESET, num_threads);
    
#ifdef _WIN32
    printf(PRP "Attempting to create directory \"%s\" using WinAPI...\n" RESET, destination_dir);
    if (CreateDirectoryA(destination_dir, NULL) == 0) { 
        DWORD error = GetLastError();
        if (error == ERROR_ALREADY_EXISTS) {
            printf(BLU "Directory '%s' already exists.\n" RESET, destination_dir);
        } else {
            fprintf(stderr, RED "Error creating directory \"%s\": %lu\n" RESET, destination_dir, error);
            return 1; 
        }
    } else {
        printf(GRN "Directory \"%s\" created with WinAPI successfully.\n" RESET, destination_dir);
    }
#else //POSIX
    printf(PRP "Attempting to create directory \"%s\" using POSIX mkdir...\n" RESET, destination_dir);
    if (mkdir(destination_dir, 0755) != 0) {
        if (errno == EEXIST) {
            printf(BLU "Directory '%s' already exists.\n" RESET, destination_dir);
        } else {
            perror("mkdir");
            return 1;
        }
    } else {
        printf(GRN "Directory \"%s\" created with POSIX mkdir successfully.\n" RESET, destination_dir);
    }
#endif
    int num_workers = num_threads;
    int num_producers = 1;
    int queue_capacity = BATCH_SIZE * num_threads;

    size_t total_files_checked = 0;

    printf(PRP "Creating a task queue, with %d capacity...\n" RESET, queue_capacity);
    task_queue_t* queue = queue_create(queue_capacity);
    if(!queue) {
        fprintf(stderr, RED "Critical error: Cannot create task queue\n" RESET);
        return 1;
    } else {
        printf(GRN "Queue created succesfully\n" RESET);
    }

    printf(PRP "Creating %d producers and contexts...\n" RESET, num_producers);
    producer_context_t producer_contexts[num_producers];
    pthread_t producers[num_producers];
    for (int i = 0; i < num_producers; ++i) {
        producer_contexts[i].id = i;
        producer_contexts[i].filter = filter;
        producer_contexts[i].dest_dir = destination_dir;
        producer_contexts[i].src_dir = source_dir;
        producer_contexts[i].queue = queue;
        producer_contexts[i].files_counter = &total_files_checked;

        if (pthread_create(&producers[i], NULL, producer_thread, &producer_contexts[i]) != 0) {
            fprintf(stderr, RED "Cannot create producer #%d\n" RESET, i);
            return -1;
        } else {
            printf(GRN "Producer #%d created succesfully\n" RESET, i);
        }
        pthread_detach(producers[i]);
    }
    
    printf(PRP "Creating %d workers and contexts...\n" RESET, num_workers);
    pthread_t workers[num_workers];
    thread_context_t contexts[num_workers];

    for (int i = 0; i < num_workers; ++i) {
        contexts[i].id = i;
        contexts[i].queue = queue;
        contexts[i].stats = calloc(1, sizeof(worker_stats_t));
        
        if (pthread_create(&workers[i], NULL, worker_thread, &contexts[i]) != 0) {
            fprintf(stderr, RED "Cannot create worker #%d\n" RESET, i);
            return -1;
        } else {
            printf(GRN "Worker #%d created succesfully\n" RESET, i);
        }
    }

    printf(PRP "Starting copy\n" RESET);
    clock_t start = clock();

    for (int i = 0; i < num_workers; i++) {
        pthread_join(workers[i], NULL); 
    }

    size_t total_bytes = 0;
    size_t total_files = 0;
    for (int i = 0; i < num_workers; i++) {
        total_bytes += contexts[i].stats->total_bytes;
        total_files += contexts[i].stats->total_files;
        free(contexts[i].stats);
    }

    clock_t end = clock();  
    double elapsed_time = (double)(end - start) / CLOCKS_PER_SEC;

    printf (
        GRN "\nSearch and copy in %s completed.\n"  
        YEL "Total %s files copied: %zu\n"
        BLU "Total checked files count: %zu\n"
        CYN "Total copied files size: %zu bytes\n"
        PRP "Total time: %.2f sec\n"
        RESET, source_dir, filter, total_files, total_files_checked, total_bytes, elapsed_time
    );
    
    return 0;
}