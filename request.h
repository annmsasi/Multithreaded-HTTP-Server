#ifndef REQUEST_H
#define REQUEST_H

#include <stdio.h>
#include <stdlib.h>

// Define your data structures and function prototypes here

typedef struct Request {
    char *method;
    char *uri;
    char *version;
    char *request_id;
    int status_code;
    int content_length;
    char *content;
    int total_bytes;
    int bytes_read;
    int connfd;

} Request;

int parse_request(char *buf, Request *R);
int put_nolock(Request *R);

#endif /* REQUEST_H */
