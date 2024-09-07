/*
 * http-server.c
*/

// import libraries
#include <stdio.h>      /* for printf() and fprintf() */
#include <sys/socket.h> /* for socket(), bind(), and connect() */
#include <arpa/inet.h>  /* for sockaddr_in and inet_ntoa() */
#include <stdlib.h>     /* for atoi() and exit() */
#include <string.h>     /* for memset() */
#include <unistd.h>     /* for close() */
#include <time.h>       /* for time() */
#include <netdb.h>      /* for gethostbyname() */
#include <signal.h>     /* for signal() */
#include <sys/stat.h>   /* for stat() */

#define MAXPENDING 5
#define MAX_BUF_SIZE 4096
#define MAX_HOSTNAME_LEN 256

static void die(const char *msg) 
{
    perror(msg);
    exit(1);
}

static struct {
    char hostName[MAX_HOSTNAME_LEN];
    unsigned short port;
} localServerInfo;

static int createMdbSocketConnection(const char *mdbHost, unsigned short mdbPort)
{
    int sock;
    struct sockaddr_in servAddr;
    struct hostent *he;

    // get server ip from server name
    if((he = gethostbyname(mdbHost)) == NULL)
        die("gethostbyname() failed");

    char *serverIP = inet_ntoa(*(struct in_addr *)he->h_addr);

    // creating socket
    if((sock = socket(PF_INET, SOCK_STREAM, IPPROTO_TCP)) < 0) 
        die("socket() failed");

    // construct server address
    memset(&servAddr, 0, sizeof(servAddr));         /* Zero out structure */
    servAddr.sin_family = AF_INET;                  /* Internet address family */
    servAddr.sin_addr.s_addr = inet_addr(serverIP); /* Any incoming interface */
    servAddr.sin_port = htons(mdbPort);             /* Local port */

    // connect
    if(connect(sock, (struct sockaddr *)&servAddr, sizeof(servAddr)) < 0)
        die("connect() failed");
    
    return sock;
}

static int createServerSocket(unsigned short port)
{
    int servSock;
    struct sockaddr_in servAddr;
    
    // create incoming connecton
    if((servSock = socket(PF_INET, SOCK_STREAM, IPPROTO_TCP)) < 0)
        die("socket() failed");

    // construct local address structure
    memset(&servAddr, 0, sizeof(servAddr));      
    servAddr.sin_family = AF_INET;                
    servAddr.sin_addr.s_addr = htonl(INADDR_ANY); 
    servAddr.sin_port = htons(port);              

    // bind to local address
    if(bind(servSock, (struct sockaddr *)&servAddr, sizeof(servAddr)) < 0)
        die("bind() failed"); 

    // listen for incoming connection
    if(listen(servSock, MAXPENDING) < 0)
        die("listen() failed");

    return servSock;
}

static struct {
    int status;
    char *reason;
} HTTP_StatusCodes[] = {
    { 200, "OK" },
    { 201, "Created" },
    { 202, "Accepted" },
    { 204, "No Content" },
    { 301, "Moved Permanently" },
    { 302, "Moved Temporarily" },
    { 304, "Not Modified" },
    { 400, "Bad Request" },
    { 401, "Unauthorized" },
    { 403, "Forbidden" },
    { 404, "Not Found" },
    { 500, "Internal Server Error" },
    { 501, "Not Implemented" },
    { 502, "Bad Gateway" },
    { 503, "Service Unavailable" },
    { 0, NULL } // marks the end of the list
};

static inline const char *getReason(int statusCode) 
{
    int i = 0;
    while(HTTP_StatusCodes[i].status > 0) {
        if(HTTP_StatusCodes[i].status == statusCode)
            return HTTP_StatusCodes[i].reason;
        i++;
    }
    return "Unknown Status Code";
}

static void sendStatusLine(int clntSock, int statusCode)
{
    char buf[1000];
    const char *reason = getReason(statusCode);
    sprintf(buf, "HTTP/1.0 %d %s\r\n", statusCode, reason);
    if(send(clntSock, buf, strlen(buf), 0) != strlen(buf))
        perror("send() failed");
}

static void sendErrorStatus(int clntSock, int statusCode)
{
    sendStatusLine(clntSock, statusCode);

    if(send(clntSock, "\r\n", strlen("\r\n"), 0) != strlen("\r\n"))
        perror("send() failed");

    char buf[1000];
    const char *reason = getReason(statusCode);
    sprintf(buf, 
            "<html><body>\n"
            "<h1>%d %s</h1>\n"
            "</body></html>\n",
            statusCode, reason);

    if(send(clntSock, buf, strlen(buf), 0) != strlen(buf))
        perror("send() failed");
}

/*
 * redirecting browser to requestURI 
 * with '/' appended
*/
static void send301Status(int clntSock, const char *requestURI) 
{
    sendStatusLine(clntSock, 301);

    char *buf = malloc(2*(strlen(localServerInfo.hostName) + strlen(requestURI)) + 1000);
    if(buf == NULL)
        die("malloc() failed");
    
    // send header and format redirection link
    sprintf(buf,
            "Location: http://%s:%d%s/\r\n"
            "\r\n"
            "<html><body>\n"
            "<h1>301 Moved Permanently</h1>\n"
            "<p>The document has moved "
            "<a href=\"http://%s:%d%s/\">here</a>.</p>\n"
            "</body></html>\n",
            localServerInfo.hostName, localServerInfo.port, requestURI,
            localServerInfo.hostName, localServerInfo.port, requestURI);
    
    if(send(clntSock, buf, strlen(buf), 0) != strlen(buf))
        perror("send() failed");
    free(buf);
}

/*
 * handle /mdb-lookup and /mdb-lookup?key= requests
 * returns HTTP status code
*/
static int handleMdbRequest(const char *requestURI, FILE *mdbFp, int mdbSock, int clntSock)
{
    int statusCode = 200;

    const char *form =
        "<html><center><body>\n"
        "<h1>mdb-lookup</h1>\n"
        "<p>\n"
        "<form method=GET action=/mdb-lookup>\n"
        "lookup: <input type=text name=key>\n"
        "<input type=submit>\n"
        "</form>\n"
        "<p>\n"
        ;

    const char *keyURI = "/mdb-lookup?key=";

    // execute lookup if /mdb-lookup?key= request
    if(strncmp(requestURI, keyURI, strlen(keyURI)) == 0) 
    {
        const char *key = requestURI + strlen(keyURI);
        fprintf(stderr, "looking up [%s]: ", key);
        if(send(mdbSock, key, strlen(key), 0) != strlen(key) || 
                send(mdbSock, "\n", 1, 0) != strlen("\n")) {
            statusCode = 500; 
            sendErrorStatus(clntSock, statusCode);
            perror("\nmdb-lookup-server connection failed");
            return statusCode;
        }

        // send status line
        sendStatusLine(clntSock, statusCode);
        if(send(clntSock, "\r\n", strlen("\r\n"), 0) != strlen("\r\n"))
            perror("send() failed");
        
        // send HTML form
        if(send(clntSock, form, strlen(form), 0) != strlen(form))
            return statusCode;

        // read lines from mdb-lookup-server 
        // and send to browser, in HTML table
        char line[1000];
        char *table_header = "<p><table border>";
        if(send(clntSock, table_header, strlen(table_header), 0) != strlen(table_header))
            return statusCode;
        
        int row = 1;
        for(;;) {
            // read from mdb-lookup-server
            if(fgets(line, sizeof(line), mdbFp) == NULL) {
                if(ferror(mdbFp))
                    perror("\nmdb-lookup-server connection failed");
                else
                    fprintf(stderr, "\nmdb-lookup-server connection terminated");
                return statusCode;
            }

            // blank line - exit loop
            if(strcmp("\n", line) == 0)
                break;
            
            // format line as table row
            char *table_row;
            if(row++ % 2)
                table_row = "\n<tr><td bgcolor=#acbcc2>";
            else
                table_row = "\n<tr><td bgcolor=#8facb8>";
            
            if(send(clntSock, table_row, strlen(table_row), 0) != strlen(table_row))
                return statusCode;
            if(send(clntSock, line, strlen(line), 0) != strlen(line))
                return statusCode;
        }   

        char *table_footer = "\n</table>\n";
        if(send(clntSock, table_footer, strlen(table_footer), 0) != strlen(table_footer))
            return statusCode;
    } 
    else {
        // send only form
        sendStatusLine(clntSock, statusCode);
        send(clntSock, "\r\n", strlen("\r\n"), 0);
        if(send(clntSock, form, strlen(form), 0) != strlen(form))
            return statusCode;
    }

    // close HTML page
    send(clntSock, "</body></center></html>\n", strlen("</body></html>\n"), 0);

    return statusCode;
}

/*
 * handle static file requests
 * returns HTTP status code for browser
*/
static int handleFileRequest(const char *webRoot, const char *requestURI, int clntSock) 
{
    int statusCode;
    FILE *fp = NULL;

    // create file path
    char *file = (char *)malloc(strlen(webRoot) + strlen(requestURI) + 100);
    if(file == NULL)
        die("malloc() failed");
    strcpy(file, webRoot);
    strcat(file, requestURI);
    if (file[strlen(file)-1] == '/')
        strcat(file, "index.html");

    // check filepath is valid
    struct stat st;
    if (stat(file, &st) == 0 && S_ISDIR(st.st_mode)) {
        statusCode = 301; // "Moved Permanently"
        send301Status(clntSock, requestURI);
        goto func_end;
    }
    fp = fopen(file, "rb");
    if (fp == NULL) {
        statusCode = 404; 
        sendErrorStatus(clntSock, statusCode);
        goto func_end;
    }

    // send 200 ok for valid filepath
    statusCode = 200; 
    sendStatusLine(clntSock, statusCode);
    send(clntSock, "\r\n", strlen("\r\n"), 0); 

    // send file content
    size_t n;
    char buf[MAX_BUF_SIZE];
    while ((n = fread(buf, 1, sizeof(buf), fp)) > 0) {
        if (send(clntSock, buf, n, 0) != n) 
            break;
    }
    if (ferror(fp))
        perror("fread() failed");

func_end:
    free(file);
    if(fp)
        fclose(fp);
    return statusCode;
}

int main(int argc, char *argv[])
{
    if(argc != 5)
    {
        fprintf(stderr, "usage: %s <server_port> <web_root> <mdb-lookup-host> <mdb-lookup-port>\n", argv[0]);
        exit(1);
    }

    unsigned short servPort = atoi(argv[1]);
    const char *webRoot = argv[2];
    const char *mdbHost = argv[3];
    unsigned short mdbPort = atoi(argv[4]);


    // creating mdb-lookup socket
    int mdbSock = createMdbSocketConnection(mdbHost, mdbPort);
    FILE *mdbFp = fdopen(mdbSock, "r");
    if(mdbFp == NULL)
        die("fdopen() failed");

    // creating server socket
    int servSock = createServerSocket(servPort);

    char hostname[MAX_HOSTNAME_LEN];
    hostname[MAX_HOSTNAME_LEN - 1] = '\0';
    gethostname(hostname, MAX_HOSTNAME_LEN - 1);
    struct hostent *he;
    he = gethostbyname(hostname);

    strncpy(localServerInfo.hostName, he->h_name, MAX_HOSTNAME_LEN-1);
    localServerInfo.hostName[MAX_HOSTNAME_LEN-1] = '\0';
    localServerInfo.port = servPort;

    char line[1000];
    char requestLine[1000];
    int statusCode;
    struct sockaddr_in clntAddr;

    for(;;) 
    {
        unsigned int clntLen = sizeof(clntAddr);
        int clntSock;
        if((clntSock = accept(servSock, (struct sockaddr *) &clntAddr, &clntLen)) < 0)
            die("accept() failed");

        FILE *clntFp = fdopen(clntSock, "r");
        if(clntFp == NULL)
            die("fdopen failed");
        
        char *method = "";
        char *requestURI = "";
        char *httpVersion = "";

        if(fgets(requestLine, sizeof(requestLine), clntFp) == NULL) {
            statusCode = 400;
            goto loop_end;
        }

        char *token_separators = "\t \r\n"; // tab, space, new line
        method = strtok(requestLine, token_separators);
        requestURI = strtok(NULL, token_separators);
        httpVersion = strtok(NULL, token_separators);
        char *restOfRequestLine = strtok(NULL, token_separators);

        if(!method || !requestURI || !httpVersion || restOfRequestLine) {
            statusCode = 400;
            sendErrorStatus(clntSock, statusCode);
            goto loop_end;
        }

        // only support GET requests
        if(strcmp(method, "GET") != 0) {
            statusCode = 501;
            sendErrorStatus(clntSock, statusCode);
            goto loop_end; 
        }

        // only support HTTP/1.0 and 1.1
        if(strcmp(httpVersion, "HTTP/1.0") != 0 && strcmp(httpVersion, "HTTP/1.1") != 0) {
            statusCode = 501;
            sendErrorStatus(clntSock, statusCode);
            goto loop_end;
        }

        // requestLine must begin with /
        if(!requestURI || *requestURI != '/') {
            statusCode = 400; // "Bad Request"
            sendErrorStatus(clntSock, statusCode);
            goto loop_end;
        }

        // check requestURI doesn't contain "/../"
        // check requestURI doesn't end with "/.."
        int uriLen = strlen(requestURI);
        if(uriLen >= 3) {
            char *end = requestURI + (uriLen-3);
            if(strcmp(end, "/..") == 0 || strstr(requestURI, "/../") != NULL) {
                statusCode = 400;
                sendErrorStatus(clntSock, statusCode);
                goto loop_end;
            }
        }

        // skip all headers
        while(1) {
            if(fgets(line, sizeof(line), clntFp) == NULL) {
                statusCode = 400;
                goto loop_end;
            }
            if (strcmp("\r\n", line) == 0 || strcmp("\n", line) == 0) 
                break;
        }

        // request complete, handle
        char *mdbURI_1 = "/mdb-lookup";
        char *mdbURI_2 = "/mdb-lookup?";

        if(strcmp(requestURI, mdbURI_1) == 0 || 
                strncmp(requestURI, mdbURI_2, strlen(mdbURI_2)) == 0)
            statusCode = handleMdbRequest(requestURI, mdbFp, mdbSock, clntSock);
        else
            statusCode = handleFileRequest(webRoot, requestURI, clntSock);

    loop_end:
        fprintf(stderr, "%s \"%s %s %s\" %d %s\n",
                inet_ntoa(clntAddr.sin_addr),
                method, 
                requestURI,
                httpVersion,
                statusCode,
                getReason(statusCode));

        fclose(clntFp);
    }
}