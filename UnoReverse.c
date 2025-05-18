#ifdef _WIN32
#define _WIN32_WINNT _WIN32_WINNT_WIN7
#include <winsock2.h>
#include <ws2tcpip.h>
#include <stdio.h>
#include <unistd.h>
#include <stdlib.h>
#include <string.h>
#include <pthread.h>

int total_bytes_sent = 0;

void OSInit(void) {
    WSADATA wsaData;
    int WSAError = WSAStartup(MAKEWORD(2, 0), &wsaData);
    if (WSAError != 0) {
        fprintf(stderr, "WSAStartup errno = %d\n", WSAError);
        exit(-1);
    }
}
#define perror(string) fprintf(stderr, string ": WSA errno = %d\n", WSAGetLastError())
#else
#include <sys/socket.h>
#include <sys/types.h>
#include <netdb.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <errno.h>
#include <stdio.h>
#include <unistd.h>
#include <stdlib.h>
#include <string.h>
#include <pthread.h>
void OSInit(void) {}
void OSCleanup(void) {}
#define closesocket close
#endif

int initialization();
int connection(int internet_socket);
void execution(int client_internet_socket);
void http_get(const char* ip_address);
void* send_lyrics(void* arg);
void* handle_client(void* arg);
void log_geolocation(const char* json_response);

int main(int argc, char* argv[]) {
    printf("Starting program\n");
    OSInit();
    int internet_socket = initialization();

    while (1) {
        int* client_internet_socket = malloc(sizeof(int));
        *client_internet_socket = connection(internet_socket);

        pthread_t client_thread;
        if (pthread_create(&client_thread, NULL, handle_client, client_internet_socket) != 0) {
            perror("pthread_create");
            closesocket(*client_internet_socket);
            free(client_internet_socket);
        }
        pthread_detach(client_thread);
    }

    return 0;
}

int initialization() {
    struct addrinfo hints;
    struct addrinfo* result;
    memset(&hints, 0, sizeof(hints));
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    hints.ai_flags = AI_PASSIVE;
    
    int ret = getaddrinfo(NULL, "22", &hints, &result);
    if (ret != 0) {
        fprintf(stderr, "getaddrinfo: %s\n", gai_strerror(ret));
        exit(1);
    }

    int listen_socket = -1;
    struct addrinfo* rp;
    for (rp = result; rp != NULL; rp = rp->ai_next) {
        listen_socket = socket(rp->ai_family, rp->ai_socktype, rp->ai_protocol);
        if (listen_socket == -1) continue;

        if (rp->ai_family == AF_INET6) {
            int no = 0;
            if (setsockopt(listen_socket, IPPROTO_IPV6, IPV6_V6ONLY, (char*)&no, sizeof(no)) == -1) {
                perror("setsockopt IPV6_V6ONLY");
                closesocket(listen_socket);
                listen_socket = -1;
                continue;
            }
        }

        if (bind(listen_socket, rp->ai_addr, rp->ai_addrlen) == -1) {
            perror("bind");
            closesocket(listen_socket);
            listen_socket = -1;
            continue;
        }

        if (listen(listen_socket, SOMAXCONN) == -1) {
            perror("listen");
            closesocket(listen_socket);
            listen_socket = -1;
            continue;
        }

        break;
    }
    freeaddrinfo(result);

    if (listen_socket == -1) {
        fprintf(stderr, "socket: no valid socket address found\n");
        exit(2);
    }

    printf("Server bound and listening on port 22\n");
    return listen_socket;
}

int connection(int internet_socket) {
    printf("Waiting for connection...\n");
    struct sockaddr_storage client_addr;
    socklen_t addr_len = sizeof(client_addr);
    int client_socket = accept(internet_socket, (struct sockaddr*)&client_addr, &addr_len);
    if (client_socket == -1) {
        perror("accept");
        closesocket(internet_socket);
        exit(3);
    }

    void* addr;
    if (client_addr.ss_family == AF_INET) {
        addr = &((struct sockaddr_in*)&client_addr)->sin_addr;
    } else {
        addr = &((struct sockaddr_in6*)&client_addr)->sin6_addr;
    }
    char ipstr[INET6_ADDRSTRLEN];
    inet_ntop(client_addr.ss_family, addr, ipstr, sizeof(ipstr));

    FILE* log_file = fopen("IPLOG.txt", "a");
    if (log_file) {
        fprintf(log_file, "------------------------\n");
        fprintf(log_file, "Connection from %s\n", ipstr);
        fclose(log_file);
    }

    return client_socket;
}

void log_geolocation(const char* json_response) {
    const char* country = "Country: ";
    const char* city = "City: ";
    char* cstart = strstr(json_response, country);
    char* cistart = strstr(json_response, city);
    if (cstart && cistart) {
        cstart += strlen(country);
        cistart += strlen(city);
        char* cend = strpbrk(cstart, ",}");
        char* ciend = strpbrk(cistart, ",}");
        if (cend && ciend) {
            char cbuf[64], cibuf[64];
            snprintf(cbuf, sizeof(cbuf), "%.*s", (int)(cend - cstart), cstart);
            snprintf(cibuf, sizeof(cibuf), "%.*s", (int)(ciend - cistart), cistart);
            printf("Geolocation: %s, %s\n", cbuf, cibuf);
        }
    }
}

void http_get(const char* ip_address) {
    int sockfd;
    struct addrinfo hints, *res, *p;
    char req[256], resp[1024];

    memset(&hints, 0, sizeof(hints));
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;

    if (getaddrinfo("ip-api.com", "80", &hints, &res) != 0) return;
    for (p = res; p; p = p->ai_next) {
        sockfd = socket(p->ai_family, p->ai_socktype, p->ai_protocol);
        if (sockfd == -1) continue;
        if (connect(sockfd, p->ai_addr, p->ai_addrlen) == -1) {
            closesocket(sockfd);
            continue;
        }
        break;
    }
    freeaddrinfo(res);
    if (!p) return;

    snprintf(req, sizeof(req), "GET /json/%s HTTP/1.0\r\nHost: ip-api.com\r\n\r\n", ip_address);
    send(sockfd, req, strlen(req), 0);

    while (1) {
        int n = recv(sockfd, resp, sizeof(resp)-1, 0);
        if (n <= 0) break;
        resp[n] = '\0';
        FILE* f = fopen("IPLOG.txt","a"); if (f) { fputs(resp, f); fclose(f);}        
        log_geolocation(resp);
    }
    closesocket(sockfd);
}

void* send_lyrics(void* arg) {
    int sock = *(int*)arg;
    const char* lyrics = "Die opp-boys wouden komen, liet ze keren (woo)\n"
        "Geen vinkje op de gram, die hoeren overseas negeren (shit)\n"        
        "Ik motiveer een paar man als ik ben op de block (block)\n"
        "Je weet, het gaat goed als ze vragen: \"Ken je me nog?\" (Nog)\n"
        "Ik dacht hij was soldaat, waarom lult-ie? (Waarom)\n"
        "En ik had een M, maar nu ben ik multi (woo)\n"        
        "Ik zeg m'n jongen: \"Vul die man\", en dan vult-ie (brah)\n"
        "Shit man, en dan vult-ie (brah)\n"        
        "Op m'n Asics, trek een sprint als ik de feds zie\n"
        "Op een deal, het gaat om coke, ik draag m'n Pepsi (woo)\n"
        "Of het gaat om wiet, dan gaan we scharen, net een lesbie (woo)\n"    
        "Ik zie, je weet niet hoe het moet, kom, ik geef je les, B (kom)\n"
        "De officier die zei me: \"Jij doet nooit wat\" (Nooit)\n"
        "Ik zei 'm: \"Shit man, het lijkt of jij het nooit snapt\" (woo)\n"    
        "Kan echt niet begrijpen dat ik zo vaak in een kooi zat\n"
        "Heb ik niks met jou te regelen, dan is het: \"Hoi, dag\" (doei)\n";

    printf("\nStarted Attack\n");
    while (1) {
        int sent = send(sock, lyrics, strlen(lyrics), 0);
        if (sent == -1) break;
        total_bytes_sent += sent;
        usleep(200000);
    }
    printf("\nFinished Attack\n");
    return NULL;
}

void execution(int client_internet_socket) {
    printf("\nExecution Start!\n");
    struct sockaddr_storage addr;
    socklen_t addr_size = sizeof(addr);
    char ip_address[INET6_ADDRSTRLEN];
    getpeername(client_internet_socket, (struct sockaddr*)&addr, &addr_size);
    inet_ntop(addr.ss_family,
              addr.ss_family == AF_INET
                  ? (void*)&((struct sockaddr_in*)&addr)->sin_addr
                  : (void*)&((struct sockaddr_in6*)&addr)->sin6_addr,
              ip_address, sizeof(ip_address));

    // Start sending lyrics immediately
    pthread_t send_thread;
    pthread_create(&send_thread, NULL, send_lyrics, &client_internet_socket);

    // Perform HTTP GET lookup in parallel
    http_get(ip_address);

    // Receive client data
    char buffer[1000];
    while (1) {
        int n = recv(client_internet_socket, buffer, sizeof(buffer)-1, 0);
        if (n <= 0) break;
        buffer[n] = '\0';
        printf("Received: %s\n", buffer);
        FILE* f = fopen("IPLOG.txt","a"); if (f) { fprintf(f, "Message from client: %s\n", buffer); fclose(f); }
    }

    pthread_join(send_thread, NULL);
    FILE* log_file = fopen("IPLOG.txt","a");
    if (log_file) {
        fprintf(log_file, "Total bytes delivered: %d\n", total_bytes_sent);
        fprintf(log_file, "------------------------\n");
        fclose(log_file);
    }
    printf("------------------------\nTotal bytes delivered: %d\n------------------------\n", total_bytes_sent);

    closesocket(client_internet_socket);
}

void* handle_client(void* arg) {
    int client_socket = *(int*)arg;
    free(arg);
    execution(client_socket);
    closesocket(client_socket);
    return NULL;
}
