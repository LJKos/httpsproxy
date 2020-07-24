
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <string.h>
#include <signal.h>
#include <fcntl.h>

#include <sys/types.h>
#include <sys/socket.h>
#include <arpa/inet.h>
#include <netinet/in.h>
#include <netdb.h>

#include <sys/event.h>
#include <sys/time.h>

#define LISTEN_QUEUE 1
#define MAX_EVENTS 401
#define MAX_SOCKET_PAIRS 200



struct socket_pair_t {
    int id;
    int socket_in;
    int socket_out;
};



int assure(int ret, char c, int cmp, char *message, void (*fptr)()) {
    int terminate = 0;
    if (c == '=' && ret != cmp) terminate = 1;
    else if (c == '!' && ret == cmp) terminate = 1;
    else if (c == '<' && ret >= cmp) terminate = 1;
    else if (c == '>' && ret <= cmp) terminate = 1;
    
    if (terminate) {
        printf("[error]: %s\n", message);
        printf("terminating...\n");
        if (fptr != NULL)
            fptr();
        exit(0);
    }
    
    return ret;
}



int notify(int ret, char c, int cmp, char *message, void (*fptr)()) {
    int notify = 0;
    if (c == '=' && ret == cmp) notify = 1;
    else if (c == '!' && ret != cmp) notify = 1;
    else if (c == '<' && ret < cmp) notify = 1;
    else if (c == '>' && ret > cmp) notify = 1;
    
    if (notify) {
        printf("[notify]: %s\n", message);
        if (fptr != NULL)
            fptr();
    }
    
    return ret;
}



int socket_to_socket(int socket_in, int socket_out) {
    // TODO: error
    char buffer[1024];
    int bytes = read(socket_in, buffer, 1024);
    write(socket_out, buffer, bytes);
    
    return 0;
}



int dns(char *ip, char *hostname) {
    struct addrinfo hints, *res, *res0;
    int error;
    char host[NI_MAXHOST];
    
    memset(&hints, 0, sizeof hints);
    hints.ai_family = PF_INET;
    hints.ai_socktype = SOCK_STREAM;
    
    error = getaddrinfo(hostname, NULL, &hints, &res0);
    if (error) {
        fprintf(stderr, "%s\n", gai_strerror(error));
        return -1;
    }
    
    for (res = res0; res; res = res->ai_next) {
        error = getnameinfo(res->ai_addr, res->ai_addrlen, host, sizeof host, NULL, 0, NI_NUMERICHOST);
        if (error) {
            fprintf(stderr, "%s\n", gai_strerror(error));
        } else {
            strcpy(ip, host);
        }
    }
    
    freeaddrinfo(res0);
    
    return 0;
}



int hostname_and_port(char *hostname, int *port, char *buffer) {
    char *ptr = strstr(buffer, "CONNECT");
    if (!ptr)
        return -1;
    
    ptr += 8;
    while (*ptr != ':' && *ptr != '\0')
        *hostname++ = *ptr++;
    *hostname = '\0';
    
    // TODO: read port
    *port = 443;
    
    return 0;
}



int init_server(int port, int queue) {
    struct sockaddr_in server;
    int server_socket = -1;
    
    server.sin_family = AF_INET;
    server.sin_port = htons(port);
    server.sin_addr.s_addr = htonl(INADDR_ANY);
    
    server_socket = socket(PF_INET, SOCK_STREAM, 0);
    if (server_socket < 0) {
        return -1;
    }
    
    if (bind(server_socket, (struct sockaddr *) &server, sizeof(server)) < 0) {
        return -1;
    }
    
    if (listen(server_socket, queue) < 0) {
        return -1;
    }
    
    return server_socket;
}



int main(int argc, char *argv[]) {
    // TODO: error
    if (argc != 2) {
        printf("%s <port>\n", argv[0]);
        return 0;
    }
    
    int server_port = 0;
    
    if ((server_port = atoi(argv[1])) == 0) {
        printf("Invalid port!\n");
        return 0;
    }
    
    struct socket_pair_t sockets[MAX_SOCKET_PAIRS];
    int socket_pair_count = 0;
    
    int server_socket = assure(init_server(server_port, LISTEN_QUEUE), '>', -1, "server initialization", NULL);
    
    struct kevent ke;
    struct kevent evlist[MAX_EVENTS] = { 0 };
    struct timespec timeout = { 30, 0 };
    int kq, nev, sockets_i = 0;
    
    kq = kqueue();
    
    EV_SET(&ke, server_socket, EVFILT_READ, EV_ADD | EV_ENABLE, 0, 0, NULL);
    assure(kevent(kq, &ke, 1, NULL, 0, NULL), '>', -1, "kevent", NULL);
    
    for (int i = 0; i < MAX_SOCKET_PAIRS; i++) {
        sockets[i].socket_in = -1;
        sockets[i].socket_out = -1;
    }
    
    for (;;) {
        nev = kevent(kq, NULL, 0, evlist, MAX_EVENTS, &timeout);
        
        assure(nev, '>', -1, "kevent", NULL);
        if (notify(nev, '=', 0, "timeout", NULL) == 0)
            continue;
        
        for (int i = 0; i < MAX_EVENTS; i++) {
            if (evlist[i].flags & EV_ERROR) {
                printf("[error]: EV_ERROR: %s\n", strerror(evlist[i].data));
            } else if (evlist[i].flags & EV_EOF) {
                struct socket_pair_t *ptr = (struct socket_pair_t *)evlist[i].udata;
                
                if (ptr->socket_in != -1 && ptr->socket_out != -1) {
                    EV_SET(&ke, ptr->socket_in, EVFILT_READ, EV_DELETE, 0, 0, NULL);
                    kevent(kq, &ke, 1, NULL, 0, NULL);
                    
                    EV_SET(&ke, ptr->socket_out, EVFILT_READ, EV_DELETE, 0, 0, NULL);
                    kevent(kq, &ke, 1, NULL, 0, NULL);
                    
                    notify(close(ptr->socket_in), '!', 0, "abnormal close", NULL);
                    notify(close(ptr->socket_out), '!', 0, "adnormal close", NULL);
                    
                    ptr->socket_in = -1;
                    ptr->socket_out = -1;
                    
                    socket_pair_count--;
                    printf("[info]: socket pairs: %d\n", socket_pair_count);
                }
            } else if (evlist[i].ident == server_socket) {
                int connection_socket = notify(accept(server_socket, NULL, NULL), '=', -1, "could not accept", NULL);
                if (connection_socket == -1)
                    continue;
                
                if (socket_pair_count < MAX_SOCKET_PAIRS) {
                    struct sockaddr_in client;
                    int client_socket;
                    int buffer[2048];
                    
                    recv(connection_socket, buffer, 2048, 0);
                
                    char ip[16];
                    char hostname[512];
                    int port = 0;
                    
                    hostname_and_port(hostname, &port, (char *)buffer);
                    dns(ip, hostname);
                    printf("[info]: hostname: %s ip: %s port: %d\n", hostname, ip, port);
                    
                    client.sin_family = AF_INET;
                    client.sin_port = htons(port);
                    client.sin_addr.s_addr = inet_addr(ip);
                    
                    client_socket = socket(PF_INET, SOCK_STREAM, 0);
                    
                    connect(client_socket, (struct sockaddr *) &client, sizeof(client));
                    
                    fcntl(connection_socket, F_SETFL, O_NONBLOCK);
                    fcntl(client_socket, F_SETFL, O_NONBLOCK);
                    
                    for (;;) {
                        if (sockets_i < MAX_SOCKET_PAIRS) sockets_i++;
                        else sockets_i = 0;
                        
                        if (sockets[sockets_i].socket_in == -1 && sockets[sockets_i].socket_out == -1) {
                            sockets[sockets_i].socket_in = connection_socket;
                            sockets[sockets_i].socket_out = client_socket;
                            sockets[sockets_i].id = sockets_i;
                            break;
                        }
                    }
                    printf("[info]: sockets_i: %d\n", sockets_i);
                    
                    EV_SET(&ke, connection_socket, EVFILT_READ, EV_ADD | EV_ENABLE, 0, 0, &sockets[sockets_i]);
                    kevent(kq, &ke, 1, NULL, 0, NULL);
                    
                    EV_SET(&ke, client_socket, EVFILT_READ, EV_ADD | EV_ENABLE, 0, 0, &sockets[sockets_i]);
                    kevent(kq, &ke, 1, NULL, 0, NULL);
                    
                    send(connection_socket, "HTTP/1.1 200 OK\r\n\r\n", 19, 0);
                    
                    socket_pair_count++;
                    printf("[info]: socket pairs: %d\n", socket_pair_count);
                    
                } else {
                    close(connection_socket);
                }
                
            } else if (evlist[i].filter == EVFILT_READ) {
                int socket_read = -1, socket_write = -1;
                socket_read = evlist[i].ident;
                struct socket_pair_t *ptr = (struct socket_pair_t *)evlist[i].udata;
                
                if (socket_read == ptr->socket_in) {
                    socket_write = ptr->socket_out;
                    socket_to_socket(socket_read, socket_write);
                } else if (socket_read == ptr->socket_out) {
                    socket_write = ptr->socket_in;
                    socket_to_socket(socket_read, socket_write);
                }
            }
        }
    }
    
    return 0;
}
