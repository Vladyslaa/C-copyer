#ifndef CORE_H
#define CORE_H

#include <stdio.h>
#include <pthread.h>

#define KILO_BYTE ((size_t)1 << 10)
#define MEGA_BYTE ((size_t)1 << 20)
#define GIGA_BYTE ((size_t)1 << 30)

#define MIN_BUFFER ((size_t)4 * KILO_BYTE)
#define MAX_BUFFER ((size_t)8 * MEGA_BYTE)

#define BATCH_SIZE 32
#define WORKER_BATCH_SIZE 16

#define TRUE 1
#define FALSE 0

#define RESET   "\x1b[0m"
#define BOLD    "\x1b[1m"
#define YEL     "\x1b[33m"
#define CYN     "\x1b[36m"
#define BLU     "\x1b[34m"
#define GRN     "\x1b[32m"
#define RED     "\x1b[31m"
#define PRP     "\x1b[35m"
#define WEAK    "\x1b[2;37m"

#ifdef _WIN32
#define MAX_PATH 260
#else //POSIX
#define MAX_PATH 4096
#endif

#define MAX(a,b) (( (a) > (b) ) ? (a) : (b))
#define MIN(a,b) (( (a) < (b) ) ? (a) : (b))

#ifdef __cplusplus
extern "C" {
#endif

#ifdef _WIN32
#include <windows.h>

wchar_t *utf8_to_wide(const char *utf8);
char *wide_to_utf8(const wchar_t *wstr);

DWORD write_all(HANDLE h, const char *buf, DWORD len);
#endif

typedef struct file_lock_t file_lock_t;
int lock_file(file_lock_t* lock, const char* path);
int unlock_file(file_lock_t* lock);

int check_extension(const char *path, const char *filter);
int is_empty(const char *s);

typedef struct task_queue_t task_queue_t;

typedef struct copy_task_t {
    char* source_path;
    char* dest_path;
    size_t buffer_size;
} copy_task_t;

typedef struct worker_stats_t {
    size_t total_files;
    size_t total_bytes;
} worker_stats_t;

typedef struct thread_context_t {
    int id;
    task_queue_t* queue;
    worker_stats_t* stats;
} thread_context_t;

typedef struct producer_context_t {
    int id;
    size_t* files_counter;
    task_queue_t* queue;
    const char *src_dir;
    const char *dest_dir;
    const char *filter;
} producer_context_t;

void* worker_thread(void* arg);
void* producer_thread(void* arg);

int copy_file(const char* src, const char* dest, worker_stats_t* thread_stat, size_t buff_size);
int scan_directory(const char* src, const char* dest, task_queue_t* queue, const char* filter, size_t* files_counter);

size_t calculate_buffer_size(size_t file_size);

#ifdef __cplusplus
}
#endif

#endif