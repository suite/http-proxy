#include <stdlib.h>
#include <stdio.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <netinet/in.h>
#include <string.h>
#include <unistd.h>
#include <signal.h>

#define BUFSIZE 32768
#define MAX_CONNECTIONS 8

// parse_proxy_request
// determine if valid (parse incoming socket1)
//      validate_proxy_request()
//          HTTP method is valid (GET)
//              if not valid, return 400 Bad Request
//              if valid:
//                  parse into 3 parts: host&port (default port 80 if none is specified), requested path, optional message body (query params)
//                  if host does not resolve ip (using gethostbyname), return 404 Not Found
// send http request to host
//      create second socket (socket2) to send request to host server
//      forward this result along socket1



// cache process (in a seperate forked process)


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
                puts(buf);

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