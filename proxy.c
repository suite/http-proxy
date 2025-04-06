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
#include <time.h>
#include <fcntl.h>

#define BUFSIZE 32768
#define MAX_CONNECTIONS 8

static const char BAD_REQUEST[] = "400 Bad Request";
static const char NOT_FOUND[] = "404 Not Found";

int timeout_seconds;      // Timeout arg
static int listenfd = -1; // Global listening socket

void handle_sigint(int sig) {
    printf("\nReceived Ctrl+C. Handling graceful shutdown...\n");
    
    if (listenfd != -1) {
        close(listenfd);
    }

    // Wait for any child processes to finish, WNOHANG so we don't block waitpid
    while (waitpid(-1, NULL, WNOHANG) > 0);
    
    printf("Server shutdown complete.\n");
    exit(0);
}

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


/*
    FILE UTILS
*/
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

int file_exists(char *path) {
    return access(path, F_OK) == 0;
}

int is_timed_out(char *path) {
    struct stat sb;
    if (stat(path, &sb) == -1) {
        return 1; // Assume timed out if stat fails
    }
    time_t now;
    time(&now);
    double diff = difftime(now, sb.st_mtime); // sb.st_mtime last modifcation time
    return diff > timeout_seconds;
}

/*
    NETWORK UTILS
*/
// Sends a response with just a status code (no body)
void send_status_response(int connfd, const char *status) {
    char buf[BUFSIZE];
    bzero(buf, BUFSIZE);
    int len = snprintf(buf, BUFSIZE,
             "HTTP/1.0 %s\r\n"
             "Content-Length: 0\r\n"
             "Connection: close\r\n"
             "\r\n",
             status);
    send(connfd, buf, len, 0);
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
    FILE *cache_file = fopen(cache_path, "r");
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
    if (origin_fd < 0) { 
        printf("FETCH ORIGIN ERR: Could not connect to host %s\n", hostname); 
        send_status_response(connfd, BAD_REQUEST);
        return -1;
    }

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
                if (strncmp(buf, "HTTP/1.0 200 OK", 15) != 0 && 
                    strncmp(buf, "HTTP/1.1 200 OK", 15) != 0) {
                    printf("FETCH ORIGIN ERR: Origin server returned non-200 response:\n%.100s\n", buf);
                    send_status_response(connfd, BAD_REQUEST);
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
        printf("ERROR: Could not get filepath\n");
        send_status_response(connfd, BAD_REQUEST);
        return;
    }

    printf("filepath: %s\n", filepath);

    char *hostname;
    char *port;
    char *path;
    int has_query;

    int parse_res = parse_url(filepath, &hostname, &port, &path, &has_query);

    if (parse_res < 0) {
        printf("ERROR: Failed to parse URL\n");
        send_status_response(connfd, BAD_REQUEST);
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
        send_status_response(connfd, NOT_FOUND);
        return;
    }

    printf("Host resolved successfully to IP\n");

    if (has_query) { // if dynamic
        printf("Dynamic URL, fetching from origin\n");
        fetch_from_origin(connfd, hostname, port, path, NULL);
    } else {
        char *cache_path = construct_cache_path(hostname, port, path);

        printf("Cache path: %s\n", cache_path);

        if (file_exists(cache_path) && !is_timed_out(cache_path)) {
            printf("Valid cache, serving from cache: %s\n", cache_path);
            serve_from_cache(connfd, cache_path); // TODO: err check? this goes for all serve's and fetches
        } else {
             // create dir struct if needed
            char *dir_path = strdup(cache_path);
            char *last_slash = strrchr(dir_path, '/');
            if (last_slash) {
                *last_slash = '\0';
                mkdir_p(dir_path);
            }
            free(dir_path);

            // Not in cache, need only this child process to fetch from origin, other's wait
            char *fetching_path = malloc(strlen(cache_path) + 10);
            sprintf(fetching_path, "%s.fetching", cache_path);

            // Create lock file with O_EXCL flag which fails if file already exists
            // 0644 permissions = Owner can read/write, group and others can only read
            int fd = open(fetching_path, O_CREAT | O_EXCL, 0644);

            if (fd != -1) {
                // Successfully created lock file - this process will handle the fetch
                close(fd);  // Close the file descriptor, we just need the file to exist

                FILE *cache_file = fopen(cache_path, "wb");
                if (cache_file) {
                    printf("Fetching and caching: %s\n", cache_path);
                    fetch_from_origin(connfd, hostname, port, path, cache_file);
                } else {
                    printf("Failed to open cache file, treating as dynamic URL\n");
                    fetch_from_origin(connfd, hostname, port, path, NULL);
                }

                unlink(fetching_path);  // Remove lock file when done
            } else {
                // Another process is fetching, wait
                printf("Waiting for cache: %s\n", cache_path);

                // while locked.. sleep
                while (file_exists(fetching_path)) {
                    usleep(100000); // 0.1 seconds
                }

                if (file_exists(cache_path)) {
                    printf("Serving from cache after wait: %s\n", cache_path);
                    serve_from_cache(connfd, cache_path);
                } else {
                    printf("Failed to open cache file, treating as dynamic URL\n");
                    fetch_from_origin(connfd, hostname, port, path, NULL);
                }
            }
            free(fetching_path);
        }
        free(cache_path);
    }
}



/*
  Creates proxy listening server
  Inspired by https://www.cs.dartmouth.edu/~campbell/cs50/socketprogramming.html
*/

int main (int argc, char **argv) {
    int portno, connfd, n, optval;
    pid_t childpid;
    socklen_t clilen;
    char buf[BUFSIZE];
    struct sockaddr_in cliaddr, servaddr;

    // Set up signal handler
    signal(SIGINT, handle_sigint);

    if (argc != 3) {
       fprintf(stderr,"usage: %s <port> <timeout>\n", argv[0]);
       exit(0);
    }

    portno = atoi(argv[1]);             // Read in port
    timeout_seconds = atoi(argv[2]);    // Read in timeout, store as global

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
                handle_client_request(connfd, buf);
            }

            if (n < 0)
                printf("Read error\n");
            exit(0);
        }

        // Close socket of the server
        close(connfd);
    }
}