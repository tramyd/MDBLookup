/*
 * mdb-lookup-server.c
 */

#include <stdio.h>
#include <stdlib.h>  
#include <string.h>
#include <assert.h>  
#include <unistd.h>
#include <sys/wait.h>  
#include <sys/stat.h>
#include <arpa/inet.h>  
#include <sys/types.h>
#include <sys/socket.h>  

#include "mylist.h"
#include "mdb.h"

#define KeyMax 5

static void die(const char *s) { perror(s); exit(1); }

int main(int argc, char **argv)
{   
    if (argc != 3) {
        fprintf(stderr, "usage: %s <db_file> <server-port>\n", argv[0]);
        exit(1);
    }

    // assign port and filename to correct command line arguments
    const char *filename = argv[1];
    unsigned short port = atoi(argv[2]);
    
    // create a listening socket (also called server socket) 
    int servsock;
    if ((servsock = socket(AF_INET, SOCK_STREAM, 0)) < 0)
        die("socket failed");

    // construct local address structure
    struct sockaddr_in servaddr;
    memset(&servaddr, 0, sizeof(servaddr));
    servaddr.sin_family = AF_INET;
    servaddr.sin_addr.s_addr = htonl(INADDR_ANY); // any network interface
    servaddr.sin_port = htons(port);

    // bind to the local address
    if (bind(servsock, (struct sockaddr *) &servaddr, sizeof(servaddr)) < 0)
        die("bind failed");

    // start listening for incoming connections
    if (listen(servsock, 5 /* queue size for connection requests */ ) < 0)
        die("listen failed");

    int clntsock;
    socklen_t clntlen;
    struct sockaddr_in clntaddr;

    while (1) {

        // accept an incoming connection
        clntlen = sizeof(clntaddr); // initialize the in-out parameter

        if ((clntsock = accept(servsock,
                        (struct sockaddr *) &clntaddr, &clntlen)) < 0)
            die("accept failed");

        // accept() returned a connected socket (also called client socket)
        // and filled in the client's address into clntaddr

        // print out IP address of client
        fprintf(stderr, "\nconnection started from: %s\n",
                inet_ntoa(clntaddr.sin_addr));
        
        /*
         * open the database file specified in the argument
         */

        FILE *fp = fopen(filename, "rb"); // open in read, binary mode
        if (fp == NULL) 
            die(filename);

        /*
         * read all records into memory
         */

        struct List list;
        initList(&list);

        struct MdbRec r;
        struct Node *node = NULL;

        while (fread(&r, sizeof(r), 1, fp) == 1) {

            // allocate memory for a new record and copy into it the one
            // that was just read from the database.
            struct MdbRec *rec = (struct MdbRec *)malloc(sizeof(r));
            if (!rec)
                die("malloc failed");
            memcpy(rec, &r, sizeof(r));

            // add the record to the linked list.
            node = addAfter(&list, node, rec);
            if (node == NULL) 
                die("addAfter failed");
        }

        // see if fread() produced error
        if (ferror(fp)) 
            die("fread failed");

        // close the database file
        fclose(fp);

        /*
         * lookup loop
         */

        // wrap the socket with a FILE* using fdopen()
        FILE *input = fdopen(clntsock, "r");
        if (input == NULL) 
            die("fdopen failed");

        char line[1000];
        char key[KeyMax + 1];

        char buf[4096];
        int size;

        while (fgets(line, sizeof(line), input) != NULL) {

            // must null-terminate the string manually after strncpy().
            strncpy(key, line, sizeof(key) - 1);
            key[sizeof(key) - 1] = '\0';

            // if newline is there, remove it.
            size_t last = strlen(key) - 1;
            if (key[last] == '\n')
                key[last] = '\0';

            // traverse the list, printing out the matching records
            struct Node *node = list.head;
            int recNo = 1;
            while (node) {
                struct MdbRec *rec = (struct MdbRec *)node->data;
                if (strstr(rec->name, key) || strstr(rec->msg, key)) {
                    size = sprintf(buf, "%4d: {%s} said {%s}\n", 
                            recNo, rec->name, rec->msg);
                    if (send(clntsock, buf, size, 0) != size) {
                        perror("send content failed");
                        break;
                    }
                }
                node = node->next;
                recNo++;
            }

            // send a blank line to indicate the end of search result
            size = sprintf(buf, "\n");
            if (send(clntsock, buf, size, 0) != size)
                perror("send content failed");
        }

        // check if fgets() produced error
        if (ferror(input)) {
            perror("fgets failed to read from input");
        }

        /*
         * clean up and quit
         */

        traverseList(&list, &free);
        removeAllNodes(&list);

        // close the socket by closing the FILE* wrapper
        fclose(input);

        // print a msg to report that one client is done
        fprintf(stderr, "connection terminated from: %s\n", 
                inet_ntoa(clntaddr.sin_addr));
    }
}