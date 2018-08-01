#include <stdio.h>
#include "csapp.h"

/* Recommended max cache and object sizes */
#define MAX_CACHE_SIZE 1049000
#define MAX_OBJECT_SIZE 102400
#define MAX_NUM  1000 

typedef struct http_request
{
    char method[10];
    char hostname[200];
    char path[1000];
} http_request;

typedef struct{
    int valid ;
    char request[MAXLINE];
    char object[MAX_OBJECT_SIZE];
    int recently_used;
    int object_size;
} block;

static int current_time = 0;
static int cache_size ;
static int readcnt = 0;
block cache[MAX_NUM];
sem_t add_mutex,w_mutex;
/* You won't lose style points for including this long line in your code */
static const char *user_agent_hdr = "User-Agent: Mozilla/5.0 (X11; Linux x86_64; rv:10.0.3) Gecko/20120305 Firefox/10.0.3\r\n";
static const char *connection_hdr = "Connection: close\r\n";
static const char *proxy_Connection_hdr = "Proxy-Connection: close\r\n";
block * find_cache(char * command);
void init_cache();
void * thread(void * vargp);
void doit(int fd);
int parse_uri(char *uri,char* host,char*path,char* port);
void read_request_header(rio_t *rp,char * host_header,char * append_header);
void build_request(char * command_line,char * path, char * host_name , char * host_header,char * append_header);
//void serve_static(int fd, char *filename, int filesize);
//void get_filetype(char *filename, char *filetype);
//void serve_dynamic(int fd, char *filename, char *cgiargs);
void clienterror(int fd, char *cause, char *errnum, 
		 char *shortmsg, char *longmsg);
void replace_block(block * new);
block * find_replace_block();
void before_read();
void after_read();
int evict_block();

int main(int argc, char **argv) 
{
    int listenfd, * connfdp;
    char hostname[MAXLINE], port[MAXLINE];
    socklen_t clientlen;
    struct sockaddr_storage clientaddr;
    pthread_t tid; 
    signal(SIGPIPE,SIG_IGN);
    /* Check command line args */
    if (argc != 2) {
	fprintf(stderr, "usage: %s <port>\n", argv[0]);
	exit(1);
    }
    init_cache();
    listenfd = Open_listenfd(argv[1]);
    while (1) {
    	clientlen = sizeof(clientaddr);
        connfdp = Malloc(sizeof(int));
    	*connfdp = Accept(listenfd, (SA *)&clientaddr, &clientlen); //line:netp:tiny:accept
    
        Getnameinfo((SA *) &clientaddr, clientlen, hostname, MAXLINE, 
                    port, MAXLINE, 0);
        printf("Accepted connection from (%s, %s)\n", hostname, port);
        Pthread_create(&tid, NULL, thread, connfdp);
	
    }
}

void * thread(void * vargp){
    int connfd = *((int *)vargp);
    Pthread_detach(pthread_self()); 
    doit(connfd);                                             //line:netp:tiny:doit
    Close(connfd);                                            //line:netp:tiny:close
    return NULL;
}

/*
 * doit - handle one HTTP request/response transaction
 */
/* $begin doit */
void doit(int fd) 
{
    char buf[MAXLINE], method[MAXLINE], uri[MAXLINE], version[MAXLINE];
    char host[MAXLINE], path[MAXLINE], port[MAXLINE],request_line[MAXLINE];
    rio_t rio, server_rio;
    int server_fd;
    char host_header[MAXLINE],append_header[MAXLINE];
    memset(path,'\0',MAXLINE);
    /* Read request line and headers */
    
    Rio_readinitb(&rio, fd);
    if (!Rio_readlineb(&rio, buf, MAXLINE))  //line:netp:doit:readrequest
        return;
    printf("%s", buf);
    sscanf(buf, "%s %s %s", method, uri, version);       //line:netp:doit:parserequest
    if (strcasecmp(method, "GET")) {                     //line:netp:doit:beginrequesterr
        clienterror(fd, method, "501", "Not Implemented",
                    "Tiny does not implement this method");
        return;
    }                                                    //line:netp:doit:endrequesterr

    if(!parse_uri(uri,host,path,port)){
        printf("Error parsing request\n");
        return;
    }
    printf("port is %s\n",port);
    read_request_header(&rio,host_header,append_header);  //line:netp:doit:readrequesthdrs
    printf("port is %s\n",port);
    build_request(request_line,path,host,host_header,append_header);
    printf("host is %s\n",host);
    printf("port is %s\n",port);
    printf("request_line is %s\n",request_line);
    

    before_read();
    block * cache_block= find_cache(request_line);
    if (cache_block){
        Rio_writen(fd,cache_block->object,cache_block->object_size);
        printf("FOUND IT\n");
        after_read();
        return;
    }
    after_read();
    printf("NO HIT\n");
    server_fd = Open_clientfd(host,port);
    
    Rio_readinitb(&server_rio,server_fd);
    Rio_writen(server_fd,request_line,strlen(request_line));
    cache_block = Malloc(sizeof(block));
    cache_block->object_size = 0;
    strcpy(cache_block->request,request_line);

    int read_count;
    char * temp_buf_point = cache_block->object;
    while ((read_count=Rio_readnb(&server_rio,temp_buf_point,MAXLINE))>0){
        Rio_writen(fd,temp_buf_point,read_count);
        cache_block->object_size += read_count;
        if(cache_block->object_size < MAX_OBJECT_SIZE)
        {
            temp_buf_point += read_count;
       }
        
    }

    if(cache_block->object_size < MAX_OBJECT_SIZE)
    { 
        P(&w_mutex);
        replace_block(cache_block);
        V(&w_mutex);
    }
    close(server_fd);
    return;
}
/* $end doit */
int parse_uri(char *uri,char* host,char*path,char* port){
    char *ifport;
    strcpy(port,"80\0") ;
    printf("uri is %s\n",uri);
    char * host_start = strstr(uri,"http://")+7;
    if(!host_start) return 0;
    char * host_end = strstr(host_start,"/");
    if(!host_end){
        strcpy(host,host_start);
        strcpy(path,"/\0");
    }
    else{
        strncpy(host,host_start,host_end-host_start);
        ifport=strstr(host,":");
        if(ifport){
            strcpy(ifport,"\0");
            strcpy(port,ifport+1);
        }
        printf("host is %s\n",host);
        printf("port is %s\n",port);
        strncpy(path,host_end,strlen(host_end));
        printf("path is %s\n",path);
    }
    return 1;   
}

void read_request_header(rio_t *rp,char * host_header,char * append_header){
    char buf[MAXLINE];

    Rio_readlineb(rp, buf, MAXLINE);
    printf("%s", buf);
    while(strcmp(buf, "\r\n")) {          //line:netp:readhdrs:checkterm
        int len = strlen("Host:");
        if(!strncmp(buf,"Host:",len))
        {
            strcpy(host_header,buf);
        }
        else if(!strncmp(buf,"User-Agent:",strlen("User-Agent:")))
        {
                 Rio_readlineb(rp, buf, MAXLINE);
                 printf("%s", buf);
                continue;
        }
        else if(!strncmp(buf,"Connection:",strlen("Connection:")))
        {

            Rio_readlineb(rp, buf, MAXLINE);
            printf("%s", buf);
            continue;
        }
        else if(!strncmp(buf,"Proxy-Connection:",strlen("Proxy-Connection:")))
        {
             Rio_readlineb(rp, buf, MAXLINE);
             printf("%s", buf);
             continue;
        }
        else
        {
            int append_len = strlen(buf);
            strcpy(append_header , buf);
            append_header += append_len ;   
        }
        Rio_readlineb(rp, buf, MAXLINE);
        printf("%s", buf);
    }
    return;
} 

void build_request(char * command_line,char * path , 
    char * host_name , char * host_header,char * append_header)
{
    
    sprintf(command_line,"GET %s HTTP/1.0\r\n",path);
    if(strlen(host_header) != 0)
    {
        sprintf(command_line,"%s%s",command_line,host_header);
    }
    else
        sprintf(command_line,"%sHOST: %s\r\n",command_line,host_name);

    sprintf(command_line,"%s%s",command_line,user_agent_hdr);
    sprintf(command_line,"%s%s",command_line,connection_hdr);
    sprintf(command_line,"%s%s",command_line,proxy_Connection_hdr);
    sprintf(command_line,"%s%s",command_line,append_header);
    sprintf(command_line,"%s\r\n",command_line);
}

/*
 * clienterror - returns an error message to the client
 */
/* $begin clienterror */
void clienterror(int fd, char *cause, char *errnum, 
		 char *shortmsg, char *longmsg) 
{
    char buf[MAXLINE], body[MAXBUF];

    /* Build the HTTP response body */
    sprintf(body, "<html><title>Tiny Error</title>");
    sprintf(body, "%s<body bgcolor=""ffffff"">\r\n", body);
    sprintf(body, "%s%s: %s\r\n", body, errnum, shortmsg);
    sprintf(body, "%s<p>%s: %s\r\n", body, longmsg, cause);
    sprintf(body, "%s<hr><em>The Tiny Web server</em>\r\n", body);

    /* Print the HTTP response */
    sprintf(buf, "HTTP/1.0 %s %s\r\n", errnum, shortmsg);
    Rio_writen(fd, buf, strlen(buf));
    sprintf(buf, "Content-type: text/html\r\n");
    Rio_writen(fd, buf, strlen(buf));
    sprintf(buf, "Content-length: %d\r\n\r\n", (int)strlen(body));
    Rio_writen(fd, buf, strlen(buf));
    Rio_writen(fd, body, strlen(body));
}
/* $end clienterror */

block * find_cache(char * command){
    // if (cache_size==0) return NULL;
    for (int i=0;i<MAX_NUM;i++){
        if(!cache[i].valid) continue;
        if (!strcmp(command,cache[i].request)){
            return &cache[i];
        }
    }
    return NULL;
}

void init_cache(){ 
    cache_size = 0;
    Sem_init(&add_mutex,0,1);
    Sem_init(&w_mutex,0,1);
    int i;
    for( i = 0; i < MAX_NUM ; ++i ){
        cache[i].request[0] = '\0';
        cache[i].object[0] = '\0';
        cache[i].recently_used = 0;
        cache[i].object_size = 0;
        cache[i].valid = 0;
    }
}

void replace_block(block * new){  

    cache_size += new->object_size;
    while(cache_size >= MAX_CACHE_SIZE){
        cache_size -= evict_block();
    }
    block * old = find_replace_block();
    printf("PUT IN %s\n", new->request);
    strcpy(old->request,new->request);
    memcpy(old->object,new->object,new->object_size);
    old -> object_size = new -> object_size;
    old->valid = 1;
    free(new);
    
}

block * find_replace_block(){
    int index = 0;
    int last_recently = 0xfffffff, replace_number = 0;
    while(index < MAX_NUM){

        if(cache[index].recently_used == 0)
            {
                cache[index].recently_used = current_time ++;
                return &cache[index];
            }
        if(last_recently > cache[index].recently_used){
            last_recently = cache[index].recently_used;
            replace_number = index;
        }

        index ++;
    }
    cache[replace_number].recently_used = current_time ++;
    return &cache[replace_number];

}


void before_read(){
    P(&add_mutex);
    readcnt++;
    if(readcnt == 1)
        P(&w_mutex);
    V(&add_mutex);
}

void after_read(){
    P(&add_mutex);
    readcnt--;
    if(readcnt == 0)
        V(&w_mutex);
    V(&add_mutex);
}

int evict_block(){
    
    int index = 0;
    int last_recently = 0xfffffff, replace_number = 0;
     while(index < MAX_NUM){
        if((last_recently > cache[index].recently_used)&&(cache[index].recently_used!=0)){
            last_recently = cache[index].recently_used;
            replace_number = index;
        }

        index ++;
    }
    cache[replace_number].recently_used = 0;
    cache[replace_number].valid = 0;
    
    return cache[replace_number].object_size;
}