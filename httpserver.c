#include "rwlock.h"
#include "queue.h"
#include "request.h"
#include <pthread.h>
#include <stdlib.h>
#include <stdio.h>
#include <arpa/inet.h>
#include <err.h>
#include <errno.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <stdio.h>
#include <stdlib.h>
#include <signal.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>
#include <regex.h>
#include <pthread.h>
#include "asgn2_helper_funcs.h"
#include "protocol.h"

#define BUFF_SIZE 4096
queue_t *q;
pthread_mutex_t mutex;

typedef struct uri_lock {
    char *uri;
    rwlock_t *lock;
    struct uri_lock *next;
} uri_lock_t;

uri_lock_t *uri_locks = NULL;
pthread_mutex_t uri_locks_mutex = PTHREAD_MUTEX_INITIALIZER;

uri_lock_t *find_or_create_lock(const char *uri) {
    pthread_mutex_lock(&uri_locks_mutex);
    // Search existing lock
    uri_lock_t *current = uri_locks;
    while (current != NULL) {
        if (strcmp(current->uri, uri) == 0) {
            pthread_mutex_unlock(&uri_locks_mutex);
            return current;
        }
        current = current->next;
    }

    //new lock
    uri_lock_t *new_lock = malloc(sizeof(uri_lock_t));
    new_lock->uri = strdup(uri);
    new_lock->lock = rwlock_new(N_WAY, 1);
    new_lock->next = uri_locks;
    uri_locks = new_lock;

    pthread_mutex_unlock(&uri_locks_mutex);
    return new_lock;
}

// how to create rwlock
int get(Request *R) {
    int fd;

    // Lock mutex to protect critical section
    if ((fd = open(R->uri, O_RDONLY | O_DIRECTORY)) != -1) {
        dprintf(R->connfd, "HTTP/1.1 403 Forbidden\r\nContent-Length: 10\r\n\r\nForbidden\n");
        fflush(stdout);
        R->status_code = 403;
        return 403;
    }
    if ((fd = open(R->uri, O_RDONLY)) == -1) {
        if (errno == ENOENT) {
            dprintf(R->connfd, "HTTP/1.1 404 Not Found\r\nContent-Length: 10\r\n\r\nNot Found\n");
            fflush(stdout);
            R->status_code = 404;
            return 404;
        } else if (errno == EACCES) {
            dprintf(R->connfd, "HTTP/1.1 403 Forbidden\r\nContent-Length: 10\r\n\r\nForbidden\n");
            fflush(stdout);
            R->status_code = 403;
            return 403;
        } else {
            dprintf(R->connfd, "HTTP/1.1 500 Internal Server Error\r\nContent-Length: "
                               "22\r\n\r\nInternal Server Error\n");
            fflush(stdout);
            R->status_code = 500;
            return 500;
        }
    }
    struct stat f_stat;
    if (fstat(fd, &f_stat) == -1) {
        printf("%s", "here");
        fflush(stdout);
        close(fd);
        R->status_code = 500;
        return 500;
    }
    off_t bytes = f_stat.st_size;
    fflush(stdout);

    dprintf(R->connfd, "HTTP/1.1 200 OK\r\nContent-Length: %zu\r\n\r\n", bytes);
    int bytes_written = pass_n_bytes(fd, R->connfd, (int) bytes);

    R->status_code = 200;
    if (bytes_written == -1) {
        dprintf(R->connfd, "HTTP/1.1 500 Internal Server Error\r\nContent-Length: "
                           "22\r\n\r\nInternal Server Error\n");
        printf("%s", "here");
        fflush(stdout);
        close(fd);
        R->status_code = 500;
        return 500;
    }
    close(fd);
    return 200;
}

int put(Request *R) {
    // writer lock?
    int status = put_nolock(R);
    // audit log?
    return status;
}
void handle_unsupported() {
}
void *workerThread(void *arg) {
    int status = 0;
    (void) arg;
    while (1) {
        uintptr_t connfd = -1;
        queue_pop(q, (void **) &connfd);
        char buf[BUFF_SIZE];
        int bytes_read = read_until(connfd, buf, BUFF_SIZE, "\r\n\r\n");
        if (bytes_read <= 0) {
            if (bytes_read < 0) {
            }
            close(connfd);
            continue;
        }
        Request R;
        R.connfd = connfd;
        R.bytes_read = bytes_read;
        int check_parse = parse_request(buf, &R);
        if (check_parse != 0) {
            close(connfd);
            continue;
        }
        uri_lock_t *uri_lock = find_or_create_lock(R.uri);

        if (strncmp(R.method, "GET", 3) == 0) {
            reader_lock(uri_lock->lock);
            status = get(&R);
            fprintf(stderr, "%s,%s,%d,%s\n", R.method, R.uri, status, R.request_id);
            reader_unlock(uri_lock->lock);
        } else if (strncmp(R.method, "PUT", 3) == 0) {
            writer_lock(uri_lock->lock);
            status = put(&R);
            fprintf(stderr, "%s,%s,%d,%s\n", R.method, R.uri, status, R.request_id);
            writer_unlock(uri_lock->lock);
        } else {
            handle_unsupported();
            status = 501;
        }
        close(R.connfd);
        fprintf(stdout, "%s,%s,%d,%s\n", R.method, R.uri, status, R.request_id);
    }
    return NULL;
}

int process_args(int argc, char *argv[], int *port_number, int *num_threads) {
    char *endptr;
    int opt;
    while ((opt = getopt(argc, argv, "t:")) != -1) {
        fflush(stdout);
        if (opt == 't') {
            *num_threads = strtol(optarg + 1, &endptr, 10);
            if (*endptr != '\0' || *num_threads <= 0) {
                return EXIT_FAILURE;
            }
            break;
        }
    }
    if (optind >= argc) {
        fprintf(stderr, "Missing port number\n");
        return EXIT_FAILURE;
    }

    *port_number = strtol(argv[optind + 1], &endptr, 10);
    if (*endptr != '\0' || *port_number <= 0) {
        fprintf(stderr, "Invalid port number: %s\n", argv[optind + 1]);
        return EXIT_FAILURE;
    }
    fflush(stdout);
    return EXIT_SUCCESS;
}

int main(int argc, char **argv) {
    int opt = 0;
    int num_threads = 4;

    while ((opt = getopt(argc, argv, "t:")) != -1) {
        if (opt == 't') {
            num_threads = atoi(optarg);
            if (num_threads < 0) {
                fprintf(stderr, "Invalid thread size.\n");
                exit(EXIT_FAILURE);
            }
        }
    }

    char *end = NULL;
    size_t port_number = (size_t) strtoull(argv[optind], &end, 10);
    if (end && *end != '\0') {
        warnx("invalid port number: %s", argv[optind]);
        return EXIT_FAILURE;
    }

    signal(SIGPIPE, SIG_IGN);
    Listener_Socket sock;
    int port_init = listener_init(&sock, port_number);
    if (port_init != 0) {
        fflush(stdout);
        fprintf(stderr, "Invalid Port\n");
        return 1;
    }

    q = queue_new(num_threads);
    pthread_mutex_init(&mutex, NULL);
    pthread_mutex_init(&uri_locks_mutex, NULL);
    pthread_t threads[num_threads];

    for (int i = 0; i < num_threads; i++) {
        pthread_create(&(threads[i]), NULL, workerThread, NULL);
    }

    while (1) {
        fflush(stdout);
        uintptr_t connfd = listener_accept(&sock);
        fflush(stdout);
        queue_push(q, (void *) connfd);
    }
}
