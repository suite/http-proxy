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
#include <arpa/inet.h>
#include <errno.h>

#define BUFSIZE 32768
#define MAX_CONNECTIONS 8

static const char BAD_REQUEST[] = "400 Bad Request";

// Rough outline:
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

int craft_response(char *buf, int http_version, char *status) {
    char *version_str = (http_version == 0) ? "HTTP/1.0" : "HTTP/1.1";

    bzero(buf, BUFSIZE);
    return snprintf(buf, BUFSIZE,
             "%s %s\r\n"
             "Content-Length: 0\r\n"
             "Connection: close\r\n"
             "\r\n",
             version_str, status);
}

char* get_content_type(char *filepath) {
    char *ext = strrchr(filepath, '.');
    if (!ext) return "text/html";
    
    if (strcmp(ext, ".html") == 0 
        || strcmp(ext, ".htm") == 0) return "text/html";
    if (strcmp(ext, ".txt") == 0)    return "text/plain";
    if (strcmp(ext, ".png") == 0)    return "image/png";
    if (strcmp(ext, ".gif") == 0)    return "image/gif";
    if (strcmp(ext, ".jpg") == 0)    return "image/jpg";
    if (strcmp(ext, ".ico") == 0)    return "image/x-icon";
    if (strcmp(ext, ".css") == 0)    return "text/css";
    if (strcmp(ext, ".js") == 0)     return "application/javascript";
    
    return "application/octet-stream";
}

// Util for creating directories recursively (like mkdir -p)
int mkdir_p(char *path) {
    char *p = strdup(path);  // Create a duplicate of path that we can modify
    char *slash = p;

    // While another slash exists in the path
    while ((slash = strchr(slash, '/')) != NULL) {
        *slash = '\0';  // Temporarily terminate string at this slash
        
        // Try to create directory with rwxr-xr-x permissions
        // Continue if directory exists, fail on other errors
        if (strlen(p) > 0) {
            printf("Attempting to create directory: %s\n", p);
            if (mkdir(p, 0755) != 0) {
                if (errno == EEXIST) {
                    printf("Directory already exists: %s\n", p);
                } else {
                    printf("Failed to create directory %s: %s\n", p, strerror(errno));
                    free(p);
                    return -1;
                }
            } else {
                printf("Successfully created directory: %s\n", p);
            }
        }
        
        *slash = '/';  // Restore the slash
        slash++;       // Move to next character
    }

    // Create the final directory (the full path)
    if (strlen(p) > 0) {
        printf("Attempting to create final directory: %s\n", p);
        if (mkdir(p, 0755) != 0) {
            if (errno == EEXIST) {
                printf("Final directory already exists: %s\n", p);
            } else {
                printf("Failed to create final directory %s: %s\n", p, strerror(errno));
                free(p);
                return -1;
            }
        } else {
            printf("Successfully created final directory: %s\n", p);
        }
    }
    free(p);
    return 1;
}


// Validate's request, and if vaid, returns filepath, else NULL
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

// TODO: Maybe don't assume index.html, I think it's fine for file name (can be anything), but we're assuming content type..
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

// ripped from https://beej.us/guide/bgnet/examples/client.c
void *get_in_addr(struct sockaddr *sa) {
    if (sa->sa_family == AF_INET) {
        return &(((struct sockaddr_in*)sa)->sin_addr);
    }
    return &(((struct sockaddr_in6*)sa)->sin6_addr);
}

// adapted from https://beej.us/guide/bgnet/examples/client.c
int connect_to_host(char *hostname, char *port) {
    int sockfd;
    struct addrinfo hints, *servinfo, *p;
    int rv;
    char s[INET6_ADDRSTRLEN];

    // Initialize hints structure
    memset(&hints, 0, sizeof hints);
    hints.ai_family = AF_UNSPEC;    // Allow IPv4 or IPv6
    hints.ai_socktype = SOCK_STREAM; // TCP stream sockets

    // Resolve hostname and port to address info
    if ((rv = getaddrinfo(hostname, port, &hints, &servinfo)) != 0) {
        fprintf(stderr, "getaddrinfo: %s\n", gai_strerror(rv));
        return -1;
    }

    // Loop through all results and connect to the first available
    for (p = servinfo; p != NULL; p = p->ai_next) {
        // Create socket
        if ((sockfd = socket(p->ai_family, p->ai_socktype, p->ai_protocol)) == -1) {
            perror("client: socket");
            continue;
        }

        // Attempt to connect
        if (connect(sockfd, p->ai_addr, p->ai_addrlen) == -1) {
            perror("client: connect");
            close(sockfd);
            continue;
        }

        break; // Successfully connected
    }

    // Check if connection failed
    if (p == NULL) {
        fprintf(stderr, "client: failed to connect\n");
        freeaddrinfo(servinfo);
        return -1;
    }

    // Print the IP address we connected to
    inet_ntop(p->ai_family, get_in_addr((struct sockaddr *)p->ai_addr), s, sizeof s);
    printf("client: connecting to %s\n", s);

    // Free address info structure
    freeaddrinfo(servinfo);

    return sockfd; // Return the connected socket file descriptor
}

int serve_from_cache(int connfd, char *cache_path) {
    FILE* cache_file = fopen(cache_path, "r");
    if (cache_file == NULL)  { printf("SERVE CACHE ERR: File not found %s\n", cache_path); return -1; }

    // Get file size
    if (fseek(cache_file, 0, SEEK_END) != 0)  { fclose(cache_file); return -1; }
    long file_size = ftell(cache_file);
    if (file_size == -1)                      { fclose(cache_file); return -1; }
    if (fseek(cache_file, 0, SEEK_SET) != 0)  { fclose(cache_file); return -1; }

    // Construct headers
    char *content_type = get_content_type(cache_path);
    char buf[BUFSIZE];
    int header_len = snprintf(buf, BUFSIZE,
        "HTTP/1.0 200 OK\r\n"
        "Content-Type: %s\r\n"
        "Content-Length: %ld\r\n"
        "Connection: close\r\n"
        "\r\n",
        content_type, file_size);

    // Send headers
    send(connfd, buf, header_len, 0);

    // Send file in chunks
    long remaining = file_size;
    while (remaining > 0) {
        int to_read = (remaining < BUFSIZE) ? remaining : BUFSIZE;
        int bytes_read = fread(buf, 1, to_read, cache_file); // Reuse buf
        send(connfd, buf, bytes_read, 0);
        remaining -= bytes_read;
    }

    fclose(cache_file);
}

int fetch_from_origin(int connfd, char *hostname, char* port, char *path, FILE *cache_file) {
    int origin_fd = connect_to_host(hostname, port);
    if (origin_fd < 0) { printf("FETCH ORIGIN ERR: Could not connect to host %s\n", hostname); return -1; }

    // Send GET request to host
    char request[BUFSIZE];
    snprintf(request, BUFSIZE, "GET %s HTTP/1.0\r\nHost: %s\r\n\r\n", path, hostname);
    send(origin_fd, request, strlen(request), 0);

    // Read in response from host
    char buf[BUFSIZE];
    int n = 0; // Number of bytes recieved in each recv
    int total = 0; // Running total for each chunk
    int headers_done = 0;
    char *body_start = NULL;

    // Receive data into buf, appending at buf + total up to BUFSIZE - total bytes
    while ((n = recv(origin_fd, buf + total, BUFSIZE - total, 0)) > 0) {
        total += n; // Might not need, really only needed for rare case headers are huge
        if (!headers_done) {
            body_start = strstr(buf, "\r\n\r\n");
            if (body_start) {
                headers_done = 1;
                body_start += 4; // Move past \r\n\r\n

                // Check if response is 200 OK
                // TODO: dont check http version
                if (strncmp(buf, "HTTP/1.0 200 OK", 15) != 0 && 
                    strncmp(buf, "HTTP/1.1 200 OK", 15) != 0) {
                    // Not 200 OK, send error to client and exit

                    // TODO:

                    //  char *error = "HTTP/1.0 502 Bad Gateway\r\n\r\n";
                    // send(connfd, error, strlen(error), 0);

                    printf("ETCH ORIGIN ERR: Origin server returned non-200 response:\n%.100s\n", buf);
                    
                    close(origin_fd);
                    if (cache_file) fclose(cache_file);
                    return -1;
                }

                // Headers end here, start processing body
                int header_len = body_start - buf;
                int body_len = total - header_len;

                // Cache the body if we have a cache file
                if (cache_file && body_len > 0) {
                    fwrite(body_start, 1, body_len, cache_file);
                }

                // Construct our own headers
                char *content_type = get_content_type(path);
                char header[BUFSIZE];
                int hlen = snprintf(header, BUFSIZE,
                    "HTTP/1.0 200 OK\r\n"
                    "Content-Type: %s\r\n"
                    "Connection: close\r\n"
                    "\r\n",
                    content_type);

                // Send headers followed by initial body chunk
                send(connfd, header, hlen, 0);
                if (body_len > 0) {
                    send(connfd, body_start, body_len, 0);
                }
                total = 0; // Reset buffer
            }
        } else {
            // Already in body, cache and send to client
            if (cache_file) {
                fwrite(buf, 1, total, cache_file);
            }
            send(connfd, buf, total, 0);
            total = 0;
        }
    }

    return 1;
}

void handle_client_request(int connfd, char *buf) {
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

    char *cache_path = construct_cache_path(hostname, port, path);

    printf("Cache path: %s\n", cache_path);

    // working
    // if in cache do this
    if (0) {
        int serve_res = serve_from_cache(connfd, cache_path);
        if (parse_res < 0) {
            // set 400 BAD Request, send back to client
            printf("ERROR: Failed to serve cache\n");
            return;
        }
    }

    // if dynamic

    // create dir struct if needed
    char* dir_path = strdup(cache_path);
    char* last_slash = strrchr(dir_path, '/');
    if (last_slash) {
        *last_slash = '\0';
        int mkdir_res = mkdir_p(dir_path);
        printf("mkdir result: %d\n", mkdir_res);
    }
    free(dir_path);

    FILE* cache_file = fopen(cache_path, "wb");
    int fetch_res = fetch_from_origin(connfd, hostname, port, path, cache_file);
    printf("Fetch result: %d\n", fetch_res);

    // int conn_res = connect_to_host(hostname, port);
    // printf("Connection result: %d\n", conn_res);


    // serve from cache

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

                handle_client_request(connfd, buf);

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