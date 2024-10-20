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
#include "protocol.h"
#include "request.h"
#include "asgn2_helper_funcs.h"

// typedef struct Request {
//     char *method;
//     char *uri;
//     char *version;
//     char *request_id;
//     int status_code;
//     int content_length;
//     char *content;
//     int total_bytes;
//     int bytes_read;
//     int connfd;

// } Request;

int parse_request(char *buf, Request *R) {
    char *copy_buf = buf;
    regmatch_t matches[4], h_matches[4];
    R->request_id = strdup("0");
    regex_t req_regex, hed_regex;
    int rc = regcomp(&req_regex, REQUEST_LINE_REGEX, REG_EXTENDED);
    rc = regexec(&req_regex, buf, 4, matches, 0);
    if (rc != 0) {
        // printf("buf:%s\n", "hi");
        regfree(&req_regex);
        return (1);
    }

    R->method = buf + matches[1].rm_so;
    R->method[matches[1].rm_eo - matches[1].rm_so] = '\0';
    R->uri = buf + matches[2].rm_so;
    R->uri[matches[2].rm_eo - matches[2].rm_so] = '\0';
    R->version = buf + matches[3].rm_so;
    R->version[matches[3].rm_eo - matches[3].rm_so] = '\0';

    buf += matches[3].rm_eo + 2;

    R->content_length = 0;
    fflush(stdout);
    R->content = strstr(buf, "\r\n\r\n");
    if (R->content == NULL) {
        fflush(stdout);
        return 400;
    }

    fflush(stdout);
    R->content += 4;
    R->total_bytes = R->bytes_read - (R->content - copy_buf);

    int rch = regcomp(&hed_regex, HEADER_FIELD_REGEX, REG_EXTENDED);
    rch = regexec(&hed_regex, buf, 3, h_matches, 0);
    if (rch != 0) {
        // printf("%s", "re");
        fflush(stdout);
        regfree(&hed_regex);
        return (1);
    }
    while (rch == 0) {
        buf[h_matches[1].rm_eo] = '\0';
        buf[h_matches[2].rm_eo] = '\0';
        if (strncmp(buf, "Content-Length", 14) == 0) {
            int value = strtol(buf + h_matches[2].rm_so, NULL, 10);
            if (errno == EINVAL) {
                dprintf(R->connfd,
                    "HTTP/1.1 400 Bad Request\r\nContent-Length: 12\r\n\r\nBad Request\n");
            }
            R->content_length = value;
        } else if (strncmp(buf + h_matches[1].rm_so, "Request-Id", 10) == 0) {
            R->request_id = strdup(buf + h_matches[2].rm_so);
        }

        buf += h_matches[2].rm_eo + 2;
        rch = regexec(&hed_regex, buf, 3, h_matches, 0);
    }
    fflush(stdout);
    return 0;
}

int put_nolock(Request *R) {
    if (R->content_length == 0) {
        dprintf(R->connfd, "HTTP/1.1 400 Bad Request\r\nContent-Length: 12\r\n\r\nBad Request\n");
        fflush(stdout);
        R->status_code = 400;
        return 400;
    }

    int fd;
    if ((fd = open(R->uri, O_WRONLY | O_DIRECTORY, 0666)) != -1) {
        close(fd);
        dprintf(R->connfd, "HTTP/1.1 403 Forbidden\r\nContent-Length: 10\r\n\r\nForbidden\n");
        fflush(stdout);
        R->status_code = 403;
        return 403;
    }

    int s_code = 0;
    if ((fd = open(R->uri, O_WRONLY | O_CREAT | O_EXCL, 0666)) == -1) {
        if (errno == EEXIST) {
            s_code = 200;
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
    } else {
        s_code = 201;
    }

    if (s_code == 200) {
        if ((fd = open(R->uri, O_WRONLY | O_CREAT | O_TRUNC, 0666)) == -1) {
            dprintf(R->connfd, "HTTP/1.1 403 Forbidden\r\nContent-Length: 10\r\n\r\nForbidden\n");
            fflush(stdout);
            R->status_code = 403;
            return 403;
        }
    }

    if (R->content_length <= R->total_bytes) {
        int bytes_written = write_n_bytes(fd, R->content, R->content_length);
        if (bytes_written == -1) {
            dprintf(R->connfd, "HTTP/1.1 500 Internal Server Error\r\nContent-Length: "
                               "22\r\n\r\nInternal Server Error\n");
            fflush(stdout);
            close(fd);
            R->status_code = 500;
            return 500;
        }
    } else if (R->content_length > R->total_bytes) {
        if (R->total_bytes == 0) {
            pass_n_bytes(R->connfd, fd, R->content_length);
        } else {
            int bytes_written = write_n_bytes(fd, R->content, R->total_bytes);
            if (bytes_written == -1) {
                dprintf(R->connfd, "HTTP/1.1 500 Internal Server Error\r\nContent-Length: "
                                   "22\r\n\r\nInternal Server Error\n");
                fflush(stdout);
                close(fd);
                R->status_code = 500;
                return 500;
            }
            pass_n_bytes(R->connfd, fd, R->content_length - bytes_written);
        }
    }

    close(fd);

    if (s_code == 201) {
        dprintf(R->connfd, "HTTP/1.1 201 Created\r\nContent-Length: 8\r\n\r\nCreated\n");
        fflush(stdout);
        R->status_code = 201;
        return 201;
    } else {
        dprintf(R->connfd, "HTTP/1.1 200 OK\r\nContent-Length: 2\r\n\r\nOK\n");
        fflush(stdout);
        R->status_code = 200;
        return 200;
    }
}
