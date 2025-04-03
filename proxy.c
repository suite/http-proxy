#include <stdlib.h>
#include <stdio.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <netinet/in.h>
#include <netdb.h>
#include <string.h>
#include <unistd.h>
#include <signal.h>

#define BUFSIZE 32768
#define MAX_CONNECTIONS 8

static const char BAD_REQUEST[] = "400 Bad Request";

// parse_proxy_request
// determine if valid (parse incoming socket1)
//      validate_proxy_request()
//          HTTP method is valid (GET)
//              if not valid, return 400 Bad Request
//              if valid:
//                  parse into 3 parts: host&port (default port 80 if none is specified), requested path, optional message body (query params)
//                  if host does not resolve ip (using gethostbyname (try getaddrinfo)), return 404 Not Found
//                  if dynamic page (has ?), prob don't want to cache
//                  if in cache and is not timed out, return cached
//                  not in cache, set it in a to_be_cached, and wait for it to be in cache, then send
// send http request to host
//      create second socket (socket2) to send request to host server
//      forward this result along socket1



// cache process (in a seperate forked process)
// need to store in ./cache folder

void construct_request();

int craft_response(char *buf, int http_version, const char *status) {
    const char *version_str = (http_version == 0) ? "HTTP/1.0" : "HTTP/1.1";

    bzero(buf, BUFSIZE);
    return snprintf(buf, BUFSIZE,
             "%s %s\r\n"
             "Content-Length: 0\r\n"
             "Connection: close\r\n"
             "\r\n",
             version_str, status);
}

/*
    Validate's request, and if vaid, returns filepath, else NULL
*/
char* validate_proxy_request(char *buf) {
    char *method = strtok(buf, " \r\n");
    if (!method) { return NULL; }

    // If not GET, fail out early
    if (strcmp(method, "GET") != 0) { return NULL; }

    char *filepath = strtok(NULL, " \r\n");
    if (!filepath) { return NULL; }

    char *http_version_header = strtok(NULL, " \r\n");
    if (!http_version_header) { return NULL; }

    return filepath;    
}


char* construct_cache_path(char* hostname, char* port, char* path) {
    char *cache_dir = "./cache";
    char *host_port = malloc(strlen(hostname) + strlen(port) + 2);
    sprintf(host_port, "%s_%s", hostname, port);
    
    char *cache_path;
    
    // Check if . in string, if not, we assume index.html
    if (strchr(path, '.') == NULL) {
        if (strcmp(path, "/") == 0) {
            cache_path = malloc(strlen(cache_dir) + 1 + strlen(host_port) + strlen("index.html") + 1);
            sprintf(cache_path, "%s/%s/index.html", cache_dir, host_port);
        } else {
            cache_path = malloc(strlen(cache_dir) + 1 + strlen(host_port) + strlen(path) + strlen("/index.html") + 1);
            sprintf(cache_path, "%s/%s%s/index.html", cache_dir, host_port, path);
        }
    } else {
        cache_path = malloc(strlen(cache_dir) + 1 + strlen(host_port) + strlen(path) + 1);
        sprintf(cache_path, "%s/%s%s", cache_dir, host_port, path);
    }

    free(host_port);
    return cache_path;
}

int parse_url(char *filepath, char **hostname, char **port, char **path, int *has_query) {
    char *url = filepath;

    // Must be http
    if (strncmp(url, "http://", 7) != 0) { printf("PARSE ERR: Not http\n"); return -1; }
    url += 7;

    // Parse hostname
    size_t host_len = strcspn(url, ":/?"); // Length until port, path, or query
    if (host_len == 0) {  printf("PARSE ERR: No :/?\n"); return - 1; }
    *hostname = strndup(url, host_len); // copies host_len bytes of url into hostname
    url += host_len;

    // ex: http://derp.derp.example.com:30/kek?derp=true

    // Parse port
    if (strncmp(url, ":", 1) == 0) {
        url++;
        size_t port_len = strcspn(url, "/");
        *port = strndup(url, port_len);
        url += port_len;        
    } else {
        *port = "80";
    }

    size_t path_len = strcspn(url, "?");
    *path = strndup(url, path_len);

    url += path_len;
    size_t query_len = strlen(url);
    if (query_len > 0) { *has_query = 1; } else { *has_query = 0; }

    return 1;
}

// fetch from origin
// serve from cache

void handle_client_request(char *buf) {
    char *filepath = validate_proxy_request(buf);
    
    if (filepath == NULL) {
        // set 400 BAD Request, send back to client
        printf("ERORR: Could not get filepath\n");
        return;
    }

    printf("filepath: %s\n", filepath);

    char *hostname;
    char *port;
    char *path;
    int has_query;

    int parse_res = parse_url(filepath, &hostname, &port, &path, &has_query);

    if (parse_res < 0) {
        // set 400 BAD Request, send back to client
        printf("ERROR: Failed to parse URL\n");
        return;
    }

    printf("Hostname: %s\n", hostname);
    printf("Port: %s\n", port);
    printf("Path: %s\n", path);
    printf("Has query params: %s\n", has_query ? "yes" : "no");

    // Check if hostname resolves
    struct hostent *host = gethostbyname(hostname);
    if (host == NULL) {
        printf("ERROR: Could not resolve host %s\n", hostname);
        // TODO: Send 404 Not Found response to client
        return;
    }

    printf("Host resolved successfully to IP\n");

    char *cachepath = construct_cache_path(hostname, port, path);

    printf("Cache path: %s\n", cachepath);

    // TODO: if dynamic, dont cache
    // else, cache

    // if in cache and is not timed out, return cached
    // not in cache, set it in a to_be_cached, and wait for it to be in cache, then send


    // check if in cache
    // create ./cache folder if does not exist
    // derive path for content: ./cache/[host_port]/[path]

    // i

    // need to handle urls like example.com/ with no file specified, I think its safe to assume itll be index.html

    // http:// myhost.example.com :30/?derp=123


    //

    // get host, check if it resolves, 

    // Parse into 3 parts, 
    // host&port (default port 80 if none is specified), requested path, optional message body (query params)

    // char *request_body;
    // construct_request(request_body);
}




// Creates proxy listening server
int main (int argc, char **argv) {
    int portno, connfd, n, optval, listenfd;
    pid_t childpid;
    socklen_t clilen;
    char buf[BUFSIZE];
    struct sockaddr_in cliaddr, servaddr;

    // TODO: Set up signal handler

    if (argc != 3) {
       fprintf(stderr,"usage: %s <port> <timeout>\n", argv[0]);
       exit(0);
    }

    // Read in port
    portno = atoi(argv[1]);

    // Create a socket for the soclet
    // If sockfd<0 there was an error in the creation of the socket
    if ((listenfd = socket (AF_INET, SOCK_STREAM, 0)) < 0) {
        perror("Problem in creating the socket");
        exit(2);
    }

    optval = 1;
    setsockopt(listenfd, SOL_SOCKET, SO_REUSEADDR, 
	     (const void *)&optval , sizeof(int));
   
    // Preparation of the socket address
    servaddr.sin_family = AF_INET;
    servaddr.sin_addr.s_addr = htonl(INADDR_ANY);
    servaddr.sin_port = htons(portno);

    // Bind the socket
    bind(listenfd, (struct sockaddr*) &servaddr, sizeof(servaddr));

    // Listen to the socket by creating a connection queue, then wait for clients
    listen(listenfd, MAX_CONNECTIONS);

    printf("Server running on port %d...waiting for connections.\n", portno);

    while (1)  {
        clilen = sizeof(cliaddr);

        // Accept a connection
        connfd = accept(listenfd, (struct sockaddr *) &cliaddr, &clilen);

        printf("Received request...\n");

        // If it's 0, it's child process
        if ((childpid = fork ()) == 0 ) {
            printf ("Child created for dealing with client requests\n");

            // Close listening socket
            close (listenfd);

            n = recv(connfd, buf, BUFSIZE, 0);
            if (n > 0) {
                printf("String received from and resent to the client:\n");
                // puts(buf);

                handle_client_request(buf);

                // int response_len = parse_request(buf, connfd);
                // printf("parsed buf %s\n", buf);

                // If error response (!= -1), send it
                // -1 means we got a file to send, and we're chunking it out and sending elsewhere
                // if (response_len != -1) {
                //     send(connfd, buf, response_len, 0);
                // }
            }

            if (n < 0)
                printf("Read error\n");
            exit(0);
        }

        // Close socket of the server
        close(connfd);
    }
}