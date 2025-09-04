#include "taskQueue.h"

#ifdef _WIN32
#include <stdlib.h>

wchar_t* utf8_to_wide(const char* utf8) {
    int wlen = MultiByteToWideChar(CP_UTF8, 0, utf8, -1, NULL, 0);

    if (wlen == 0) {
        fprintf(stderr, RED "Cannot calculate wide string size from utf8 \"%s\"\n" RESET, utf8);
        return NULL;
    }

    wchar_t* wstr = (wchar_t*)malloc(wlen * sizeof(wchar_t));
    if (!wstr) {
        fprintf(stderr, RED "Cannot allocate memory for wide string\n" RESET);
        return NULL;
    }

    if (!MultiByteToWideChar(CP_UTF8, 0, utf8, -1, wstr, wlen)) {
        fprintf(stderr, RED "Cannot convert utf8 string \"%s\" to wchar_t\n" RESET, utf8);
        free(wstr);
        return NULL;
    }

    return wstr;
}

char* wide_to_utf8(const wchar_t* wstr) {
    int len = WideCharToMultiByte(CP_UTF8, 0, wstr, -1, NULL, 0, NULL, NULL);

    if (len == 0) {
        fprintf(stderr, RED "Cannot calculate UTF-8 size from wide string\n" RESET);
        return NULL;
    }

    char* utf8 = (char*)malloc(len);
    if (!utf8) {
        fprintf(stderr, RED "Cannot allocate memory for UTF-8 string\n" RESET);
        return NULL;
    }

    if (!WideCharToMultiByte(CP_UTF8, 0, wstr, -1, utf8, len, NULL, NULL)) {
        fprintf(stderr, RED "Cannot convert wide string to UTF-8\n" RESET);
        free(utf8);
        return NULL;
    }

    return utf8;
}

DWORD write_all(HANDLE h, const char *buf, DWORD len) {
    DWORD total = 0;
    while (total < len) {
        DWORD written = 0;
        if (!WriteFile(h, buf + total, len - total, &written, NULL)) {
            return GetLastError();
        }
        if (written == 0) {
            return ERROR_WRITE_FAULT;
        }
        total += written;
    }
    return 0;
}

struct file_lock_t {
    HANDLE handle;
};

int lock_file(file_lock_t* lock, const char* path) {
    wchar_t* wpath = utf8_to_wide(path);
    if (!wpath) return -1;

    lock->handle = CreateFileW(
        wpath,
        GENERIC_READ,
        FILE_SHARE_READ,
        NULL,
        OPEN_EXISTING,
        FILE_ATTRIBUTE_NORMAL,
        NULL
    );
    free(wpath);

    if (lock->handle == INVALID_HANDLE_VALUE) {
        DWORD err = GetLastError();
        fprintf(stderr, RED "Cannot open file \"%s\": %lu\n" RESET, path, err);
        return -1;
    }
    /*
    OVERLAPPED ov = {0};
    if (!LockFileEx(
        lock->handle,
        LOCKFILE_EXCLUSIVE_LOCK,
        0,
        MAXDWORD, MAXDWORD,
        &ov
    )) {
        DWORD err = GetLastError();
        fprintf(stderr, "LockFileEx failed: %lu\n Closing handle...\n", err);
        CloseHandle(lock->handle);
        lock->handle = INVALID_HANDLE_VALUE;
        return -1;
    }
    */
    return FALSE;
}

int unlock_file(file_lock_t *lock) {
    if (lock->handle == INVALID_HANDLE_VALUE) return -1;
    /*
    OVERLAPPED ov = {0};
    if (!UnlockFileEx(lock->handle, 0, MAXDWORD, MAXDWORD, &ov)) {
        DWORD err = GetLastError();
        fprintf(stderr, "UnlockFileEx failed: %lu\n", err);
    }
    */

    CloseHandle(lock->handle);
    lock->handle = INVALID_HANDLE_VALUE;

    return FALSE;
}

#else // POSIX
#include <unistd.h>
#include <fcntl.h>
#include <dirent.h>
#include <sys/stat.h>
#include <stdlib.h>
#include <errno.h>
#include <string.h>
#include <ctype.h>

struct file_lock_t {
    int fd;
};

int lock_file(file_lock_t* lock, const char* path) {
    lock->fd = open(path, O_RDONLY);
    if(lock->fd < 0) return -1;

    struct flock fl = {0};
    fl.l_type = F_RDLCK;
    fl.l_whence = SEEK_SET;

    return fcntl(lock->fd, F_SETLK, &fl);
}

int unlock_file(file_lock_t* lock) {
    if (!lock || lock->fd == -1) {
        errno = EBADF;
        return -1;
    }

    int fd = lock->fd;
    lock->fd = -1;

    if (close(fd) == -1) {
        fprintf(stderr, RED "Cannot close fd %d\n" RESET, lock->fd);
        return -1;
    }

    return 0;
}

#endif 

size_t calculate_buffer_size(size_t file_size) {
    if (file_size < MIN_BUFFER) return (file_size + 63) & ~63;

    size_t buffer_size = MIN_BUFFER;
    while (buffer_size < file_size && buffer_size < MAX_BUFFER) buffer_size <<= 1;

    return (buffer_size + 63) & ~63;
}

int copy_file(const char* src, const char* dest, worker_stats_t* thread_stat, size_t buff_size) {
#ifdef _WIN32
    HANDLE destination_file = INVALID_HANDLE_VALUE; 
    char* buffer = NULL;
    file_lock_t *source_lock = NULL;
    DWORD error_code = 0;
    size_t total_bytes_copied = 0;

    wchar_t *destW = utf8_to_wide(dest);
    if (!destW) return 1;

    source_lock = malloc(sizeof(file_lock_t));
    if (!source_lock) { error_code = ERROR_NOT_ENOUGH_MEMORY; goto cleanup; }
    source_lock->handle = INVALID_HANDLE_VALUE;
    
    if (lock_file(source_lock, src) != 0) {
        fprintf(stderr, RED "copy_file error : Cannot lock source file \"%s\"\n" RESET, src);

        error_code = 1;
        goto cleanup;
    }

    destination_file = CreateFileW(
        destW,
        GENERIC_WRITE,
        0, 
        NULL,
        CREATE_NEW, 
        FILE_ATTRIBUTE_NORMAL,
        NULL
    );
    if (destination_file == INVALID_HANDLE_VALUE) {
        error_code = GetLastError();
        fprintf(stderr, RED "copy_file error : Cannot open destination file \"%s\", err code: \"%lu\"\n" RESET, dest, error_code);

        goto cleanup;
    }

    buffer = (char*)malloc(buff_size);
    if (buffer == NULL) {
        error_code = ERROR_NOT_ENOUGH_MEMORY;
        fprintf(stderr, RED "copy_file error: Cannot allocate buffer memory for file \"%s\"\n" RESET, src);

        goto cleanup;
    }

    for (;;) {
        DWORD bytes_readed = 0;

        if (!ReadFile(source_lock->handle, buffer, buff_size, &bytes_readed, NULL)) {        
            error_code = GetLastError();
            fprintf(stderr, RED "copy_file error: Read failed for \"%s\"\n" RESET, src);
            break; 
        }
        if (bytes_readed == 0) break;      

        DWORD write_err = write_all(destination_file, buffer, bytes_readed);
        if (write_err != 0) {
            error_code = write_err;
            fprintf(stderr, RED "copy_file error: Write failed for \"%s\", error code: %lu\n" RESET, dest, error_code);
            break; 
        }

        total_bytes_copied += (size_t)bytes_readed;
    }

cleanup:

    if (error_code == 0) {
        ++thread_stat->total_files;
        thread_stat->total_bytes += total_bytes_copied;
    }

    unlock_file(source_lock);
    free(source_lock);

    if (destination_file != INVALID_HANDLE_VALUE) CloseHandle(destination_file);

    free(buffer);
    free(destW);

    return error_code;

#else //POSIX
    int error_code = 0;
    size_t total_bytes_copied = 0;
    file_lock_t* source_lock = NULL;
    int src_fd = -1;
    int dest_fd = -1;
    char* buffer = NULL;
    struct stat src_stat;

    source_lock = malloc(sizeof(file_lock_t));
    if (!source_lock) { error_code = 1; goto cleanup; }
    source_lock->fd = -1;

    if (lock_file(source_lock, src) != 0) {
        fprintf(stderr, RED "copy_file error: Cannot lock source file \"%s\"\n" RESET, src);
        error_code = errno;
        goto cleanup;
    }

    if (fstat(source_lock->fd, &src_stat) != 0) {
        fprintf(stderr, RED "copy_file error: Cannot get source file \"%s\" info\n" RESET, src);
        error_code = errno;
        goto cleanup;
    }

    dest_fd = open(dest, O_WRONLY | O_CREAT | O_EXCL, src_stat.st_mode);
    if (dest_fd == -1) {
        fprintf(stderr, RED "copy_file error: Cannot open destination file \"%s\"\n" RESET, dest);
        error_code = errno;
        goto cleanup;
    }

    buffer = malloc(buff_size);
    if (!buffer) {
        fprintf(stderr, RED "copy_file error: Cannot allocate buffer memory for file \"%s\"\n" RESET, src);
        error_code = ENOMEM;
        goto cleanup;
    }

    ssize_t bytes_read = 0;
    while ((bytes_read = read(source_lock->fd, buffer, buff_size)) > 0) {
        ssize_t bytes_written = write(dest_fd, buffer, bytes_read);
        
        if (bytes_written != bytes_read) {
            fprintf(stderr, RED "copy_file error: Write failed for \"%s\"\n" RESET, dest);
            error_code = errno;
            break;
        }
        
        total_bytes_copied += bytes_written;
    }

    if (bytes_read < 0) {
        fprintf(stderr, RED "copy_file error: Read failed for \"%s\"\n" RESET, src);
        error_code = errno;
    }

cleanup:
    
    if (error_code == 0) {
        ++thread_stat->total_files;
        thread_stat->total_bytes += total_bytes_copied;
    }

    if (source_lock) {
        if (source_lock->fd != -1) {
            unlock_file(source_lock);
        }
        free(source_lock);
    }

    if (dest_fd != -1) {
        close(dest_fd);
    }

    free(buffer);

    return error_code;
#endif
}

void* worker_thread(void* arg) {
    thread_context_t *cont = (thread_context_t*)arg;
    copy_task_t current_tasks_batck[WORKER_BATCH_SIZE];
    for (;;) {
        int batch_count = queue_pop_batch(cont->queue, current_tasks_batck, WORKER_BATCH_SIZE);
        if ( batch_count < 0) {
            fprintf(stdout, GRN "worker #%d finished work\n" RESET, cont->id);
            pthread_exit((void*)0);
        }

        for (int i = 0; i < batch_count; ++i) {
            int res = copy_file(current_tasks_batck[i].source_path, current_tasks_batck[i].dest_path, cont->stats, current_tasks_batck[i].buffer_size);
            if (res != 0) {
                fprintf(
                    stderr, RED "worker #%d error : Cannot copy \"%s\" in \"%s\", err code: %d \n" RESET, 
                    cont->id, current_tasks_batck[i].source_path, current_tasks_batck[i].dest_path, res
                );
            }

            free(current_tasks_batck[i].source_path);
            free(current_tasks_batck[i].dest_path);
        }
    }
}

void* producer_thread(void* arg) {
    producer_context_t* cont = (producer_context_t*)arg;

    int res = scan_directory(cont->src_dir, cont->dest_dir, cont->queue, cont->filter, cont->files_counter);

    fprintf(res == 0 ? stdout : stderr, "producer #%d: scan_directory terminated with code %d\n", cont->id, res);

    pthread_mutex_lock(&cont->queue->mutex);
    cont->queue->shutdown = -1;
    pthread_cond_broadcast(&cont->queue->not_empty);
    pthread_mutex_unlock(&cont->queue->mutex);

    return NULL;
}

int scan_directory(const char* src, const char* dest, task_queue_t* queue, const char* filter, size_t* files_counter) {
#ifdef _WIN32
    int err_code = 0;
    WIN32_FIND_DATAW foundet_data = {0};
    HANDLE h = INVALID_HANDLE_VALUE;

    wchar_t *src_pathW = NULL;
    wchar_t *dst_pathW = NULL;
    wchar_t *srcW = NULL;
    wchar_t *destW = NULL;
     
    copy_task_t *tasks_batch = NULL;
    size_t batch_count = 0;

    src_pathW = malloc(MAX_PATH * sizeof(wchar_t));
    dst_pathW = malloc(MAX_PATH * sizeof(wchar_t));

    if (!src_pathW || !dst_pathW) {
        err_code = ERROR_OUTOFMEMORY;
        goto cleanup;
    }

    srcW  = utf8_to_wide(src);
    destW = utf8_to_wide(dest);

    if (!srcW || !destW) {
        err_code = ERROR_OUTOFMEMORY;
        goto cleanup;
    }

    tasks_batch = calloc(BATCH_SIZE, sizeof(copy_task_t));
    if (!tasks_batch) {
        err_code = ERROR_OUTOFMEMORY;
        goto cleanup;
    }

    size_t sp_len = wcslen(srcW) + 3;
    wchar_t* search_path = malloc(sp_len * sizeof(wchar_t));
    if (!search_path) {
        err_code = ERROR_OUTOFMEMORY;
        goto cleanup;
    }
    swprintf(search_path, sp_len, L"%s\\*", srcW);

    h = FindFirstFileW(search_path, &foundet_data);
    free(search_path);
    if (h == INVALID_HANDLE_VALUE) {
        err_code =  GetLastError();
        goto cleanup;
    }

    do {
        if (wcscmp(foundet_data.cFileName, L".") == 0 || wcscmp(foundet_data.cFileName, L"..") == 0) continue;

        swprintf(src_pathW, MAX_PATH, L"%s\\%s", srcW, foundet_data.cFileName);
        swprintf(dst_pathW, MAX_PATH, L"%s\\%s", destW, foundet_data.cFileName);

        char* src_path  = wide_to_utf8(src_pathW);
        char* dest_path = wide_to_utf8(dst_pathW);

        if (foundet_data.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
            CreateDirectoryW(dst_pathW, NULL);

            err_code = scan_directory(src_path, dest_path, queue, filter, files_counter);
            if (err_code != 0) goto cleanup;
        } else {
            ++*files_counter;

            if (check_extension(src_path, filter)) {
                copy_task_t task;

                ULONGLONG src_size = ((ULONGLONG)foundet_data.nFileSizeHigh << 32) | foundet_data.nFileSizeLow;
                task.buffer_size = calculate_buffer_size((size_t)src_size);

                task.source_path = wide_to_utf8(src_pathW);
                task.dest_path = wide_to_utf8(dst_pathW);

                tasks_batch[batch_count++] = task;

                if (batch_count >= BATCH_SIZE) {
                    queue_enqueue_batch(queue, tasks_batch, batch_count);
                    batch_count = 0;
                }
            }
        }
        free(src_path);
        free(dest_path);
    } while (FindNextFileW(h, &foundet_data));

    if (batch_count != 0) {
        queue_enqueue_batch(queue, tasks_batch, batch_count);
        batch_count = 0;
    }

cleanup:
    free(srcW);
    free(destW);
    free(src_pathW);
    free(dst_pathW);
    free(tasks_batch);

    if (h != INVALID_HANDLE_VALUE) FindClose(h);

    return err_code;
#else //POSIX
    DIR *dir = opendir(src);
    if (!dir) return errno;

    struct dirent *entry;
    int err_code = 0;

    copy_task_t *tasks_batch = NULL;
    size_t batch_count = 0;

    char *src_path = NULL;
    char *dest_path = NULL;

    tasks_batch = calloc(BATCH_SIZE, sizeof(copy_task_t));
    if (!tasks_batch) {
        err_code = ENOMEM;
        goto cleanup;
    }

    while ((entry = readdir(dir))) {
        if (strcmp(entry->d_name, ".") == 0 || strcmp(entry->d_name, "..") == 0) continue;

        size_t src_len = snprintf(NULL, 0, "%s/%s", src, entry->d_name) + 1;
        size_t dest_len = snprintf(NULL, 0, "%s/%s", dest, entry->d_name) + 1;

        src_path = realloc(src_path, src_len);
        dest_path = realloc(dest_path, dest_len);

        if (!src_path || !dest_path) {
            err_code = ENOMEM;
            goto cleanup;
        }

        snprintf(src_path, src_len, "%s/%s", src, entry->d_name);
        snprintf(dest_path, dest_len, "%s/%s", dest, entry->d_name);

        struct stat st;
        if (lstat(src_path, &st) == -1) {
            fprintf(stderr, RED "scan_directory failed: cannot get information for file \"%s\", code: %d\n" RESET, src_path, errno);
            continue;
        }

        if (S_ISDIR(st.st_mode)) {
            if (mkdir(dest_path, 0755) == -1 && errno != EEXIST) {
                fprintf(stderr, RED "scan_directory: failed to create directory %s: %s\n" RESET, dest_path, strerror(errno));
                err_code = errno;
                goto cleanup;
            }
            err_code = scan_directory(src_path, dest_path, queue, filter, files_counter);
            if (err_code != 0) {
                goto cleanup;
            }
        } else {
            ++*files_counter;

            if (check_extension(src_path, filter)) {
                copy_task_t task = {
                    .source_path = strdup(src_path),
                    .dest_path = strdup(dest_path),
                    .buffer_size = calculate_buffer_size((size_t)st.st_size)
                };
                
                if (!task.source_path || !task.dest_path) {
                    free(task.source_path);
                    free(task.dest_path);
                    err_code = ENOMEM;

                    goto cleanup;
                }
                
                tasks_batch[batch_count++] = task;

                if (batch_count >= BATCH_SIZE) {
                    queue_enqueue_batch(queue, tasks_batch, batch_count);
                    batch_count = 0;
                }
            }
        }
    }

    if (batch_count > 0) {
        queue_enqueue_batch(queue, tasks_batch, batch_count);
    }

cleanup:
    closedir(dir);

    free(src_path);
    free(dest_path);
    free(tasks_batch);

    return err_code;
#endif
}

int check_extension(const char *path, const char *filter) {
    if (is_empty(path) || is_empty(filter)) return FALSE;
    else if (strcmp(filter, "all") == 0) return TRUE;

    const char *dot = strrchr(path, '.');
    if (!dot || dot == path) return FALSE;
    ++dot;

    const char *f = filter;
    while (*dot && *f) {
        if (tolower((unsigned char)*dot) != tolower((unsigned char)*f)) return FALSE;
        ++dot, ++f;
    }

    return (*dot == '\0' && *f == '\0');
}

int is_empty(const char *s) {
    if (!s) return TRUE;

    while (*s) {
        if (!isspace((unsigned char)*s)) {
            return FALSE;
        }
        ++s;
    }
    return TRUE;
}