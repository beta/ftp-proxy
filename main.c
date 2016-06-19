#define TRUE 1
#define FALSE 0
#define BUFFSIZE 2048

#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <netdb.h>
#include <arpa/inet.h>
#include <string.h>
#include <sys/stat.h>

/**
 * Creates a socket and binds it onto the given port, then makes it listen for new connections.
 * Returns the file descriptor of the created socket.
 */
int bind_and_listen_socket(int port_number) {
    struct sockaddr_in server_address, client_address;

    // Create a new socket
    int socket_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (socket_fd < 0) {
        perror("Error opening socket");
        exit(1);
    }

    setsockopt(socket_fd, SOL_SOCKET, SO_REUSEADDR, &(int) {1}, sizeof(int));

    // Prepare server address
    bzero((char *) &server_address, sizeof(server_address));
    server_address.sin_family = AF_INET;
    server_address.sin_addr.s_addr = INADDR_ANY;
    server_address.sin_port = htons(port_number);

    // Bind socket on the given port
    if (bind(socket_fd, (struct sockaddr *) &server_address, sizeof(server_address)) < 0) {
        perror("Error binding");
        exit(1);
    }

    // Set the socket to listen for new connections
    listen(socket_fd, 5);

    return socket_fd;
}

/**
 * Accepts an incoming connection.
 * Returns a new socket created for the connection.
 */
int accept_connection(int sockfd, struct sockaddr_in *addr) {
    socklen_t client_length = sizeof(*addr);

    int command_socket_fd = accept(sockfd, addr, &client_length);
    if (command_socket_fd < 0) {
        perror("Error accepting connection");
        exit(1);
    }

    return command_socket_fd;
}

/**
 * Creates a new connection to the target address.
 * Returns a new socket created for the connection.
 */
int create_connection(struct sockaddr_in addr) {
    int socket_fd = socket(AF_INET, SOCK_STREAM, 0);

    if (socket_fd < 0) {
        perror("Error opening socket");
        exit(1);
    }

    if (connect(socket_fd, (struct sockaddr *) &addr, sizeof(addr)) < 0) {
        perror("Error creating connection");
        exit(1);
    }

    return socket_fd;
}

/**
 * Creates a new connection to the target host name and port.
 * Returns a new socket created for the connection.
 */
int create_connection_by_host_name(const char *host_name, int port) {
    struct hostent *server = gethostbyname(host_name);

    if (server == NULL) {
        perror("No such host");
        exit(0);
    }

    struct sockaddr_in address;

    bzero((char *) &address, sizeof(address));
    address.sin_family = AF_INET;
    bcopy(server->h_addr, (char *) &address.sin_addr.s_addr, server->h_length);
    address.sin_port = htons(port);

    return create_connection(address);
}

/**
 * Sends data to the server.
 */
void send_to_server(int socket_fd, const char *buffer) {
    printf("Send to server: %s", buffer);
    write(socket_fd, buffer, strlen(buffer));
}

/**
 * Sends data to the server.
 */
void send_to_client(int socket_fd, const char *buffer) {
    printf("Send to client: %s", buffer);
    write(socket_fd, buffer, strlen(buffer));
}

int main(int argc, const char *argv[]) {
    // Enable auto flushing of stdout
    setvbuf(stdout, NULL, _IONBF, 0);

    // Check arguments
    if (argc < 3) {
        fprintf(stderr, "Missing argument.\n");
        exit(1);
    }
    if (argc > 3) {
        fprintf(stderr, "Too many arguments.\n");
        exit(1);
    }

    // Get server address from argument.
    const char *server_address = argv[1];
    int proxy_address[4];
    sscanf(argv[2], "%d.%d.%d.%d", &proxy_address[0], &proxy_address[1], &proxy_address[2], &proxy_address[3]);

    // Create directory for cached files.
    mkdir("cache", 0775);

    // File descriptor sets
    fd_set master_set;              // Set for all file descriptors
    fd_set working_set;             // Set for file descriptors to be listened to

    int proxy_cmd_socket = 0;       // Socket of listening for command connection
    int client_command_socket = 0;  // Socket of accepting command connection from client
    int server_command_socket = 0;  // Socket of creating command connection to server
    int proxy_data_socket = 0;      // Socket of listening for data connection
    int income_data_socket = 0;     // Socket of accepting data connection
    int outcome_data_socket = 0;    // Socket of creating data connection
    int selectResult = 0;           // Result of select
    int select_sd = 10;             // Maximum file descriptor for select

    int mode = 0;                   // 0 for active mode and 1 for passive mode
    int waiting_for_server_data_port = 0;

    char cache_file_path[100] = {0};
    int cache_hit = 0;
    int should_send_cache_file = 0;
    int should_save_cache_file = 0;
    int file_transfer_mode = 0;     // 0 for downloading and 1 for uploading

    struct in_addr *client_address;
    int active_client_data_port;
    int passive_server_data_port;

    // Clean master_set
    FD_ZERO(&master_set);

    // Bind on port 21 and listen for connections from client.
    proxy_cmd_socket = bind_and_listen_socket(21);
    printf("Listening for command connection on port 21...\n");
    // Add the socket into master set of file descriptors
    FD_SET(proxy_cmd_socket, &master_set);

    struct timeval timeout; // Timeout structs
    bzero(&timeout, sizeof(timeout));
    timeout.tv_sec = 120; // Timeout interval for select
    timeout.tv_usec = 0; // Use ms

    while (TRUE) {
        FD_ZERO(&working_set);
        memcpy(&working_set, &master_set, sizeof(master_set));

        selectResult = select(select_sd, &working_set, NULL, NULL, &timeout);

        // fail
        if (selectResult < 0) {
            perror("select() failed\n");
            return 1;
        }

        // timeout
        if (selectResult == 0) {
            printf("select() timed out\n");
            return 1;
        }

        for (int i = 0; i < select_sd; i += 1) {
            // Check if the ith file descriptor exists in working_set
            if (FD_ISSET(i, &working_set)) {
                if (i == proxy_cmd_socket) {
                    if (client_command_socket > 0) {
                        FD_CLR(client_command_socket, &master_set);
                        close(client_command_socket);
                    }
                    if (server_command_socket > 0) {
                        FD_CLR(server_command_socket, &master_set);
                        close(server_command_socket);
                    }
                    // New incoming command connection from client
                    struct sockaddr_in client;

                    client_command_socket = accept_connection(proxy_cmd_socket, &client);
                    client_address = &client.sin_addr;
                    printf("Accepted new command connection from client.\n");

                    server_command_socket = create_connection_by_host_name(server_address, 21);
                    printf("New command connection to server created.\n");

                    // Add newly created sockets into master set
                    FD_SET(client_command_socket, &master_set);
                    FD_SET(server_command_socket, &master_set);
                }

                if (i == client_command_socket) {
                    char buff[BUFFSIZE] = {0};

                    if (read(client_command_socket, buff, BUFFSIZE) == 0) {
                        // Close command connections if nothing received
                        FD_CLR(client_command_socket, &master_set);
                        close(client_command_socket);

                        FD_CLR(server_command_socket, &master_set);
                        close(server_command_socket);

                        printf("Client disconnected\n");
                    } else {
                        printf("Received from client: %s\n", buff);

                        char command[5];
                        strncpy(command, buff, 4);
                        command[4] = '\0';

                        if (command[3] == ' ') {
                            command[3] = '\0';
                        }

                        if (strcmp(command, "PORT") == 0) {
                            // Active mode
                            mode = 0;

                            // Get server address and data port
                            int client_ip[4];
                            int client_data_port[2];
                            sscanf(buff, "PORT %d,%d,%d,%d,%d,%d",
                                   &client_ip[0], &client_ip[1], &client_ip[2], &client_ip[3],
                                   &client_data_port[0], &client_data_port[1]);

                            active_client_data_port = client_data_port[0] * 256 + client_data_port[1];

                            // Listen for new data connection from server
                            if (proxy_data_socket > 0) {
                                FD_CLR(proxy_data_socket, &master_set);
                                close(proxy_data_socket);
                            }
                            proxy_data_socket = bind_and_listen_socket(active_client_data_port);
                            printf("Listening for data connection on port %d...\n", active_client_data_port);
                            FD_SET(proxy_data_socket, &master_set);

                            char command[100] = {0};
                            sprintf(command, "PORT %d,%d,%d,%d,%d,%d\n",
                                    proxy_address[0], proxy_address[1], proxy_address[2], proxy_address[3],
                                    client_data_port[0], client_data_port[1]);

                            // Send the PORT command to server
                            send_to_server(server_command_socket, command);
                        } else if (strcmp(command, "PASV") == 0) {
                            // Passive mode
                            mode = 1;
                            waiting_for_server_data_port = 1;

                            send_to_server(server_command_socket, buff);
                        } else if (strcmp(command, "RETR") == 0) {
                            // Download a file
                            file_transfer_mode = 0;

                            // Get file name
                            const char *filename = buff + 5;
                            memset(cache_file_path, 0, sizeof(cache_file_path));
                            sprintf(cache_file_path, "cache/%s", filename);
                            for (int j = 0; j < strlen(cache_file_path); j += 1) {
                                if (cache_file_path[j] == '\n' || cache_file_path[j] == '\r') {
                                    cache_file_path[j] = '\0';
                                    break;
                                }
                            }

                            // Check if file exists in cache
                            if (access(cache_file_path, F_OK) != -1) {
                                // Cache hit
                                printf("Cache hit: %s\n", cache_file_path);

                                cache_hit = 1;
                                should_send_cache_file = 1;
                                should_save_cache_file = 0;
                            } else {
                                // Cache miss
                                printf("Cache miss\n");

                                cache_hit = 0;
//                                rewind(cache_file);

                                should_send_cache_file = 0;
                                should_save_cache_file = 1;
                            }

                            send_to_server(server_command_socket, buff);
                        } else if (strcmp(command, "STOR") == 0) {
                            // Upload a file
                            file_transfer_mode = 1;

                            // Get file name
                            const char *filename = buff + 5;
                            memset(cache_file_path, 0, sizeof(cache_file_path));
                            sprintf(cache_file_path, "cache/%s", filename);
                            for (int j = 0; j < strlen(cache_file_path); j += 1) {
                                if (cache_file_path[j] == '\n' || cache_file_path[j] == '\r') {
                                    cache_file_path[j] = '\0';
                                    break;
                                }
                            }

                            // Check if file exists in cache
                            if (access(cache_file_path, F_OK) != -1) {
                                // Cache hit
                                printf("Cache hit: %s\n", cache_file_path);

                                cache_hit = 1;
                                should_send_cache_file = 1;
                                should_save_cache_file = 0;
                            } else {
                                // Cache miss
                                printf("Cache miss\n");

                                cache_hit = 0;
//                                rewind(cache_file);

                                should_send_cache_file = 0;
                                should_save_cache_file = 1;
                            }

                            send_to_server(server_command_socket, buff);
                        } else {
                            send_to_server(server_command_socket, buff);
                        }
                    }
                }

                if (i == server_command_socket) {
                    char buff[BUFFSIZE] = {0};

                    if (read(server_command_socket, buff, BUFFSIZE) == 0) {
                        // Close command connections if nothing received
                        FD_CLR(server_command_socket, &master_set);
                        close(server_command_socket);

                        FD_CLR(client_command_socket, &master_set);
                        close(client_command_socket);

                        printf("Server disconnected\n");
                    } else {
                        printf("Received from server: %s\n", buff);

                        if (mode == 1 && waiting_for_server_data_port == 1 && strncmp(buff, "227", 3) == 0) {
                            // Enter passive mode
                            int server_ip[4];
                            int server_data_port[2];

                            // Get server address and data port
                            sscanf(buff, "227 Entering Passive Mode (%d,%d,%d,%d,%d,%d)",
                                   &server_ip[0], &server_ip[1], &server_ip[2], &server_ip[3],
                                   &server_data_port[0], &server_data_port[1]);

                            passive_server_data_port = server_data_port[0] * 256 + server_data_port[1];
                            waiting_for_server_data_port = 0;

                            // Close existed connection
                            if (proxy_data_socket > 0) {
                                FD_CLR(proxy_data_socket, &master_set);
                                close(proxy_data_socket);
                            }

                            // Listen for new data connection from client
                            proxy_data_socket = bind_and_listen_socket(passive_server_data_port);
                            printf("Listening for data connection on port %d...\n", passive_server_data_port);
                            FD_SET(proxy_data_socket, &master_set);

                            // Send 227 response to client
                            char response[60] = {0};
                            sprintf(response, "227 Entering Passive Mode (%d,%d,%d,%d,%d,%d)\n",
                                    proxy_address[0], proxy_address[1], proxy_address[2], proxy_address[3],
                                    server_data_port[0], server_data_port[1]);
                            send_to_client(client_command_socket, response);
                        } else {
                            send_to_client(client_command_socket, buff);
                        }
                    }
                }

                if (i == proxy_data_socket) {
                    if (income_data_socket > 0) {
                        FD_CLR(income_data_socket, &master_set);
                        close(income_data_socket);
                    }
                    if (outcome_data_socket > 0) {
                        FD_CLR(outcome_data_socket, &master_set);
                        close(outcome_data_socket);
                    }

                    if (mode == 0) {
                        // Active mode
                        // Receive data connection from server
                        struct sockaddr_in server;
                        income_data_socket = accept_connection(proxy_data_socket, &server);
                        printf("Accepted data connection from server\n");

                        // Create data connection to client
//                        struct sockaddr_in client;
//                        client.sin_family = AF_INET;
//                        client.sin_addr = *client_address;
//                        client.sin_port = htons(active_client_data_port);
//                        outcome_data_socket = create_connection(client);
                        outcome_data_socket = create_connection_by_host_name(server_address, active_client_data_port);
                        printf("Data connection to server created\n");

                        if (should_send_cache_file && cache_hit) {
                            // Send cached file
                            FILE *cache_file = fopen(cache_file_path, "r");

                            if (cache_file != NULL) {

                                char buffer[BUFFSIZE] = {0};

                                while (!feof(cache_file)) {
                                    size_t read_file_size = fread(buffer, sizeof(char), BUFFSIZE - 1, cache_file);
                                    printf("Read %d bytes from cache file\n", (int) read_file_size);

                                    if (file_transfer_mode == 0) {
                                        write(outcome_data_socket, buffer, read_file_size);
                                    } else if (file_transfer_mode == 1) {
                                        write(income_data_socket, buffer, read_file_size);
                                    }
                                }

                                fclose(cache_file);

                                close(income_data_socket);
                                close(outcome_data_socket);
                            } else {
                                FD_SET(income_data_socket, &master_set);
                                FD_SET(outcome_data_socket, &master_set);
                            }

                            should_send_cache_file = 0;
                            cache_hit = 0;
                        } else {
                            FD_SET(income_data_socket, &master_set);
                            FD_SET(outcome_data_socket, &master_set);
                        }
                    } else if (mode == 1) {
                        // Passive mode
                        // Receive data connection from client
                        struct sockaddr_in client;
                        income_data_socket = accept_connection(proxy_data_socket, &client);
                        printf("Accepted data connection from client\n");

                        // Create data connection to server
                        outcome_data_socket = create_connection_by_host_name(server_address, passive_server_data_port);
                        printf("Data connection to server created\n");

                        if (should_send_cache_file && cache_hit) {
                            // Send cached file
                            FILE *cache_file = fopen(cache_file_path, "r");

                            if (cache_file != NULL) {

                                char file_buffer[BUFFSIZE] = {0};

                                while (!feof(cache_file)) {
                                    size_t read_file_size = fread(file_buffer, sizeof(char), BUFFSIZE - 1, cache_file);
                                    printf("Read %d bytes from cache file\n", (int) read_file_size);

                                    if (file_transfer_mode == 0) {
                                        write(income_data_socket, file_buffer, read_file_size);
                                    } else if (file_transfer_mode == 1) {
                                        write(outcome_data_socket, file_buffer, read_file_size);
                                    }
                                }

                                fclose(cache_file);

                                close(income_data_socket);
                                close(outcome_data_socket);
                            } else {
                                // Add the newly created sockets into master_set
                                FD_SET(income_data_socket, &master_set);
                                FD_SET(outcome_data_socket, &master_set);
                            }

                            should_send_cache_file = 0;
                            cache_hit = 0;
                        } else {
                            // Add the newly created sockets into master_set
                            FD_SET(income_data_socket, &master_set);
                            FD_SET(outcome_data_socket, &master_set);
                        }
                    }
                }

                if (i == income_data_socket) {
                    char buff[BUFFSIZE] = {0};

                    ssize_t read_size = read(income_data_socket, buff, BUFFSIZE);
                    if (read_size == 0) {
                        // Close data connections if nothing received
                        FD_CLR(income_data_socket, &master_set);
                        close(income_data_socket);

                        FD_CLR(outcome_data_socket, &master_set);
                        close(outcome_data_socket);

                        if (should_save_cache_file) {
                            should_save_cache_file = 0;
                        }
                    } else {
                        printf("Received data: %d bytes\n", (int) read_size);

                        write(outcome_data_socket, buff, read_size);

                        if (should_save_cache_file) {
                            FILE *cache_file = fopen(cache_file_path, "a");
                            if (cache_file != NULL) {
                                fwrite(buff, sizeof(char), read_size, cache_file);
                                fclose(cache_file);
                            } else {
                                printf("Cannot open cache file %s\n", cache_file_path);
                                should_save_cache_file = 0;
                            }
                        }
                    }
                }

                if (i == outcome_data_socket) {
                    char buff[BUFFSIZE] = {0};

                    ssize_t read_size = read(outcome_data_socket, buff, BUFFSIZE);
                    if (read_size == 0) {
                        // Close data connections if nothing received
                        FD_CLR(income_data_socket, &master_set);
                        close(income_data_socket);

                        FD_CLR(outcome_data_socket, &master_set);
                        close(outcome_data_socket);

                        if (should_save_cache_file) {
                            should_save_cache_file = 0;
                        }
                    } else {
                        printf("Received data: %d bytes\n", (int) read_size);

                        write(income_data_socket, buff, read_size);

                        if (should_save_cache_file) {
                            FILE *cache_file = fopen(cache_file_path, "a");
                            if (cache_file != NULL) {
                                fwrite(buff, sizeof(char), read_size, cache_file);
                                fclose(cache_file);
                            } else {
                                should_save_cache_file = 0;
                            }
                        }
                    }
                }
            }
        }
    }

    return 0;
}
