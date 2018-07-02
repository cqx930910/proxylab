#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include "csapp.h"

#define MAX_CACHE_SIZE 1049000
#define MAX_OBJECT_SIZE 102400

//#define DEBUG

/* local structs */
typedef struct http_request
{
    char method[10];
    char hostname[200];
    char path[1000];
} http_request;

typedef struct http_header
{
    char key[200];
    char value[1000];
    struct http_header* next;
} http_header;

typedef struct cache_entry
{
    char hostname[200];
    char path[1000];
    unsigned long long size;
    char* data;
    clock_t usage;
    struct cache_entry* next;
} cache_entry;

/* global variables */
cache_entry* cache;
int cache_scale;

/* local functions */
void help_message();
void *handle_request(void* vargp);
http_request* parse_request(char* line);
http_header* parse_header(char* line, http_header* current);
void free_http_metadata(http_request* request_ptr, http_header* header_head);
void process_header(http_header* root, http_request* request);
cache_entry* search_cache(cache_entry* root, char* hostname, char* path);
void remove_cache(cache_entry* root, cache_entry* to_remove);
cache_entry* insert_cache(cache_entry* root);
void update_timetag(cache_entry* current);
cache_entry* search_least_use(cache_entry* root);

/* display usage message */
void help_message()
{
    const char* help_message_content = "Usage: ./proxy <port-number>\n";
    printf("%s", help_message_content);
    return;
}

/* main loop and dispatcher */
int main(int argc, char **argv)
{
    /* initialize cache */
    cache = malloc(sizeof(cache_entry));
    cache_scale = 0;
    cache -> next = NULL;
    cache -> data = NULL;
    cache -> size = 0;
    strcpy(cache -> hostname, "");
    strcpy(cache -> path, "");
    cache -> usage = clock();

    /* parse input message */
    int port_number;
    pthread_t tid;
    if (argc != 2)
    {
        help_message();
        return 0;
    }
    port_number = atoi(argv[1]);

    /* open listen port */
    int listenfd, *connfd;
    unsigned int clientlen;
    struct sockaddr_in clientaddr;
    listenfd = Open_listenfd(port_number);

    /* wait for client requests */
    while (1)
    {
        connfd = malloc(sizeof(int));
	    clientlen = sizeof(clientaddr);
	    *connfd = Accept(listenfd, (SA *)&clientaddr, &clientlen);
        Pthread_create(&tid, NULL, handle_request, connfd);
    }

    return 0;
}

/* handle incoming request */
void *handle_request(void* vargp)
{
    int fd = *((int *)vargp);
    Pthread_detach(pthread_self());
    free(vargp);
    /* start parsing request */
    /* pointers for metadata */
    http_header* header_root = NULL;
    http_header* header_curr = NULL;
    http_header* header_temp = NULL;
    http_request* request_info = NULL;
    /* rio init */
    char buf[MAXLINE];
    rio_t rio;
    Rio_readinitb(&rio, fd);
    /* parse http request */
    Rio_readlineb(&rio, buf, MAXLINE);
    request_info = parse_request(buf);
    if (!request_info)
    {
        #ifdef DEBUG
        //printf("Error parsing request\n");
        return 0;
        #endif
        /* TODO: implement error handling here */
    }
    /* parse request header */
    Rio_readlineb(&rio, buf, MAXLINE);
    header_temp = parse_header(buf, header_curr);
    if (!header_temp)
    {
        /* TODO: implement error handling here */
        #ifdef DEBUG
        //printf("Error parsing header (out of loop)\n");
        #endif
    }
    else
    {
        header_root = header_temp;
        header_curr = header_temp;
        while(strcmp(buf, "\r\n"))
        {
            Rio_readlineb(&rio, buf, MAXLINE);
            if (strcmp(buf, "\r\n") == 0)
            {
                break;
            }
            header_temp = parse_header(buf, header_curr);
            if (!header_temp)
            {
                /* TODO: implement error handling here. Should exit rather then break */
                #ifdef DEBUG
                //printf("Error parsing header (inside loop)\n");
                #endif
            }
            header_curr = header_temp;
        }
    }

    /* modify header and add necessary entries */
    process_header(header_root, request_info);
    #ifdef DEBUG
    printf("Requested file is %s\n", request_info -> path);
    #endif

    /* check cache */
    #ifdef DEBUG
    printf("Looking for cache\n");
    #endif
    cache_entry* existing = search_cache(cache, request_info -> hostname, request_info -> path);
    if (existing != NULL)
    {
        #ifdef DEBUG
        printf("Accessing cache\n");
        #endif
        /* found in cache */
        /* TODO: update last usage time */
        Rio_writen(fd, existing -> data, existing -> size);
        update_timetag(existing);
        Close(fd);
        return 0;
    }

    /* connecting remote */
    /* parse remote address and port */
    int remote_port = 80;
    char remote_domain[200];
    strcpy(remote_domain, request_info -> hostname);
    char* port_start = strstr(remote_domain, ":");
    if (port_start)
    {
        /* parse new port */
        remote_port = atoi(port_start + 1);
        /* cut original domain name */
        *port_start = '\0';
    }

    /* establish connection to remote */
    int remotefd;
    #ifdef DEBUG
    //printf("Connecting %s at %d\n", remote_domain, remote_port);
    printf("Fetching %s\n", request_info -> path);
    #endif
    remotefd = Open_clientfd(remote_domain, remote_port);

    /* send HTTP request */
    char request_buffer[10000];
    request_buffer[0] = '\0';
    strcat(request_buffer, "GET ");
    strcat(request_buffer, request_info -> path);
    strcat(request_buffer, " HTTP/1.0\r\n");
    header_temp = header_root;
    while (header_temp != NULL)
    {
        strcat(request_buffer, header_temp -> key);
        strcat(request_buffer, ": ");
        strcat(request_buffer, header_temp -> value);
        strcat(request_buffer, "\r\n");
        header_temp = header_temp -> next;
    }
    strcat(request_buffer, "\r\n");
    Rio_writen(remotefd, request_buffer, strlen(request_buffer));

    /* catch response */
    rio_t rio_remote;
    Rio_readinitb(&rio_remote, remotefd);
    int read_len;
    /* prepare write to cache */
    char cache_candidate[MAX_OBJECT_SIZE];
    char* cache_ptr = cache_candidate;
    int cache_ok = 1;
    while ((read_len = rio_readnb(&rio_remote, buf, MAXLINE)) > 0)
    {
        Rio_writen(fd, buf, read_len);
        if (cache_ok)
        {
            /* check whether exceeds max object size */
            if ((read_len + (cache_ptr - cache_candidate)) <= MAX_OBJECT_SIZE)
            {
                memcpy(cache_ptr, buf, read_len);
                cache_ptr += read_len;
            }
            else
            {
                cache_ok = 0;
            }
        }
    }
    /* write to cache if meet requirements */
    /* TODO: update last use time */
    if (cache_ok)
    {
        int this_size = cache_ptr - cache_candidate;
        if ((cache_scale + this_size) <= MAX_CACHE_SIZE)
        {
            #ifdef DEBUG
            printf("Writing to cache\n");
            #endif
            /* no eviction */
            cache_entry* new_block = insert_cache(cache);
            new_block -> size = this_size;
            strcpy(new_block -> hostname, request_info -> hostname);
            strcpy(new_block -> path, request_info -> path);
            new_block -> data = malloc(this_size);
            memcpy(new_block -> data, cache_candidate, this_size);
            cache_scale += this_size;
            update_timetag(new_block);
        }
        else
        {
            #ifdef DEBUG
            printf("Writing to cache, with eviction\n");
            #endif
            /* need eviction */
            //while ((cache_scale + this_size) > MAX_CACHE_SIZE)
            //{
                cache_entry* to_evict = search_least_use(cache);
                #ifdef DEBUG
                //printf("Removing %s\n", to_evict -> path);
                #endif
                remove_cache(cache, to_evict);
            //}
            #ifdef DEBUG
            printf("Eviction completed\n");
            #endif
            cache_entry* new_block = insert_cache(cache);
            new_block -> size = this_size;
            strcpy(new_block -> hostname, request_info -> hostname);
            strcpy(new_block -> path, request_info -> path);
            new_block -> data = malloc(this_size);
            memcpy(new_block -> data, cache_candidate, this_size);
            cache_scale += this_size;
            update_timetag(new_block);
        }
    }


    Close(remotefd);
    Close(fd);
    /* TODO: call free_http_metadata() here to free metadata tables */
    /* TODO: close remote connection. Client connection will be closed by main() */
    return 0;
}

/*
 parse HTTP request line (first line)
 return value: 0 for failed, pointer for success
 */
http_request* parse_request(char* line)
{
    char raw_method[10];
    char raw_url[1500];
    http_request* storage = malloc(sizeof(http_request));
    /* parse input line, we only need first two parameters */
    if (!(sscanf(line, "%s %s", raw_method, raw_url) == 2))
    {
        return 0;
    }

    /* parse method */
    if (strcmp("GET", raw_method) != 0)
    {
        //printf("Unsupported method %s\n", raw_method);
        // printf("Raw request: %s\n", line);
        /* if method is not GET, ignore this request */
        return 0;
    }
    else
    {
        strcpy(storage -> method, raw_method);
    }

    /* parse url */
    char* url_starting;
    char* path_starting;
    url_starting = strstr(raw_url, "http://");
    if (!url_starting)
    {
        /* can't find http scheme */
        return 0;
    }
    /* point to the start of hostname */
    url_starting += 7;
    path_starting = strstr(url_starting, "/");
    if (!path_starting)
    {
        /* no path specified */
        strcpy(storage -> hostname, url_starting);
        strcpy(storage -> path, "/");
    }
    else
    {
        memcpy(storage -> hostname, url_starting, path_starting - url_starting);
        *((storage -> hostname) + (path_starting - url_starting)) = '\0';
        strcpy(storage -> path, path_starting);
    }
    return storage;
}

/* parse a header line, return NULL when error */
http_header* parse_header(char* line, http_header* last_node)
{
    if (strcmp(line, "\r\n") == 0)
    {
        #ifdef DEBUG
        //printf("Parsing empty header, quitting\n");
        #endif
        /* reaching empty line, abort with NULL */
        return NULL;
    }
    char* sperater = strstr(line, ": ");
    char* terminator = strstr(line, "\r\n");
    if ((!sperater) || (!terminator))
    {
        /* bad header format */
        #ifdef DEBUG
        //printf("Bad header format\n");
        //printf("Source: %s\n", line);
        #endif
        return NULL;
    }
    /* allocate space for new node */
    http_header* current = malloc(sizeof(http_header));
    /* fill in the struct */
    current -> next = NULL;
    memcpy(current -> key, line, sperater - line);
    *((current -> key) + (sperater - line)) = '\0';
    memcpy(current -> value, sperater + 2, terminator - sperater - 2);
    *((current -> value) + (terminator - sperater) - 2) = '\0';
    /* link the last node to the current node */
    if (last_node)
    {
        last_node -> next = current;
    }
    return current;
}

/* frees structures allocated in request parsing */
void free_http_metadata(http_request* request_ptr, http_header* header_head)
{
    free(request_ptr);
    http_header* temp;
    while (header_head != NULL)
    {
        temp = header_head -> next;
        free(header_head);
        header_head = temp;
    }
    return;
}

/* modify header to meet the request */
void process_header(http_header* root, http_request* request)
{
    http_header* temp = root;
    if (temp == NULL)
    {
        /* empty header, aborting */
        return;
    }
    http_header* last = NULL;
    int found = 0;
    /* find the last node */
    while (temp -> next != NULL)
    {
        temp = temp -> next;
    }
    last = temp;

    /* process Host tag */
    temp = root;
    while (temp != NULL)
    {
        if (strcmp(temp -> key, "Host") == 0)
        {
            found = 1;
        }
        temp = temp -> next;
    }
    if (!found)
    {
        last -> next = malloc(sizeof(http_header));
        last = last -> next;
        last -> next = NULL;
        strcpy(last -> key, "Host");
        strcpy(last -> value, request -> hostname);
    }

    /* process User-Agent */
    temp = root;
    found = 0;
    while (temp != NULL)
    {
        if (strcmp(temp -> key, "User-Agent") == 0)
        {
            found = 1;
            strcpy(temp -> value, "Mozilla/5.0 (X11; Linux x86_64; rv:10.0.3) Gecko/20120305 Firefox/10.0.3");
        }
        temp = temp -> next;
    }
    if (!found)
    {
        last -> next = malloc(sizeof(http_header));
        last = last -> next;
        last -> next = NULL;
        strcpy(last -> key, "User-Agent");
        strcpy(last -> value, "Mozilla/5.0 (X11; Linux x86_64; rv:10.0.3) Gecko/20120305 Firefox/10.0.3");
    }

    /* process Accept */
    temp = root;
    found = 0;
    while (temp != NULL)
    {
        if (strcmp(temp -> key, "Accept") == 0)
        {
            found = 1;
            strcpy(temp -> value, "text/html,application/xhtml+xml,application/xml;q=0.9,*/*;q=0.8");
        }
        temp = temp -> next;
    }
    if (!found)
    {
        last -> next = malloc(sizeof(http_header));
        last = last -> next;
        last -> next = NULL;
        strcpy(last -> key, "Accept");
        strcpy(last -> value, "text/html,application/xhtml+xml,application/xml;q=0.9,*/*;q=0.8");
    }

    /* process Accept-Encoding */
    temp = root;
    found = 0;
    while (temp != NULL)
    {
        if (strcmp(temp -> key, "Accept-Encoding") == 0)
        {
            found = 1;
            strcpy(temp -> value, "gzip, deflate");
        }
        temp = temp -> next;
    }
    if (!found)
    {
        last -> next = malloc(sizeof(http_header));
        last = last -> next;
        last -> next = NULL;
        strcpy(last -> key, "Accept-Encoding");
        strcpy(last -> value, "gzip, deflate");
    }

    /* process Connection */
    temp = root;
    found = 0;
    while (temp != NULL)
    {
        if (strcmp(temp -> key, "Connection") == 0)
        {
            found = 1;
            strcpy(temp -> value, "close");
        }
        temp = temp -> next;
    }
    if (!found)
    {
        last -> next = malloc(sizeof(http_header));
        last = last -> next;
        last -> next = NULL;
        strcpy(last -> key, "Connection");
        strcpy(last -> value, "close");
    }

    /* process Proxy-Connection */
    temp = root;
    found = 0;
    while (temp != NULL)
    {
        if (strcmp(temp -> key, "Proxy-Connection") == 0)
        {
            found = 1;
            strcpy(temp -> value, "close");
        }
        temp = temp -> next;
    }
    if (!found)
    {
        last -> next = malloc(sizeof(http_header));
        last = last -> next;
        last -> next = NULL;
        strcpy(last -> key, "Proxy-Connection");
        strcpy(last -> value, "close");
    }
    return;
}

/* search cache for certain block */
cache_entry* search_cache(cache_entry* root, char* hostname, char* path)
{
    while (root != NULL)
    {
        if (strcmp(hostname, root -> hostname) == 0)
        {
            if (strcmp(path, root -> path) == 0)
            {
                return root;
            }
        }
        root = root -> next;
    }
    return NULL;
}

/* remove an entry in cache */
void remove_cache(cache_entry* root, cache_entry* to_remove)
{
    if (to_remove == NULL)
    {
        #ifdef DEBUG
        printf("Deleting non-existing entry, aborting\n");
        #endif
    }
    #ifdef DEBUG
    //printf("Removing entry\n");
    #endif
    if (root == NULL)
    {
        return;
    }
    while ((root != NULL) && (root -> next != to_remove))
    {
        root = root -> next;
    }
    #ifdef DEBUG
    //printf("Deleting entry found\n");
    #endif
    if (root == NULL)
    {
        return;
    }
    root -> next = to_remove -> next;
    cache_scale -= to_remove -> size;
    free(to_remove -> data);
    free(to_remove);
    #ifdef DEBUG
    printf("Entry removed\n");
    #endif
    return;
}

/* add a new cache entry. no initialization */
cache_entry* insert_cache(cache_entry* root)
{
    while (root -> next != NULL)
    {
        root = root -> next;
    }
    root -> next = malloc(sizeof(cache_entry));
    root -> next -> next = NULL;
    return root -> next;
}

/* update the timetag of a cache entry */
void update_timetag(cache_entry* current)
{
    current -> usage = clock();
    return;
}

/* search the entry which is used the most far ago */
cache_entry* search_least_use(cache_entry* root)
{
    root = root -> next;
    clock_t least = root -> usage;
    cache_entry* result = root;
    while (root != NULL)
    {
        #ifdef DEBUG
        //printf("Checking cache %s, time tag %d\n", root -> path, root -> usage);
        #endif
        if (least > root -> usage)
        {
            least = root -> usage;
            result = root;
        }
        root = root -> next;
    }
    #ifdef DEBUG
    printf("LR found at %s\n", result -> path);
    #endif
    return result;
}
