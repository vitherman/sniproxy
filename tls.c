/*
 * This is a minimal TLS impleimentation indented only to parse the server name extension.
 * This was created based primarily on wireshark disection of a TLS handshake and RFC4366.
 */
#include <stdio.h>
#include <string.h> /* strncpy() */
#include <unistd.h>
#include <sys/socket.h>
#include "tls.h"

#define SERVER_NAME_LEN 256
#define TLS_HEADER_LEN 5
#define TLS_HANDSHAKE_CONTENT_TYPE 0x16
#define TLS_HANDSHAKE_TYPE_CLIENT_HELLO 0x01

static const uint8_t tls_alert[] = {
    0x15, /* TLS Alert */
    0x03, 0x01, /* TLS version  */
    0x00, 0x02, /* Payload length */
    0x02, 0x28, /* Fatal, handshake failure */
};


static const char *parse_server_name_extension(const uint8_t *, int);


/* Send a TLS handshake failure alert and close a socket */
void
close_tls_socket(int sockfd) {
    send(sockfd, tls_alert, sizeof(tls_alert), 0);
    close(sockfd);
}

/* Parse a TLS packet for the Server Name Indication extension in the client hello
 * handshake, returning the first servername found (pointer to static array) */
const char *
parse_tls_header(const uint8_t* data, int data_len) {
    uint8_t tls_content_type;
    uint8_t tls_version_major;
    uint8_t tls_version_minor;
    uint16_t tls_length;
    const uint8_t* p = data;
    int len;

    /* Check that our TCP payload is atleast large enough for a TLS header */
    if (data_len < TLS_HEADER_LEN)
        return NULL;

    tls_content_type = p[0];
    if (tls_content_type != TLS_HANDSHAKE_CONTENT_TYPE) {
        fprintf(stderr, "Did not receive TLS handshake\n");
        return NULL;
    }

    tls_version_major = p[1];
    tls_version_minor = p[2];
    if (tls_version_major < 3) {
        fprintf(stderr, "Receved pre SSL 3.0 handshake\n\n");
        return NULL;
    }

    if (tls_version_major == 3 && tls_version_minor < 1) {
        fprintf(stderr, "Receved SSL 3.0 handshake\n\n");
        return NULL;
    }

    tls_length = (p[3] << 8) + p[4];
    if (data_len < tls_length + TLS_HEADER_LEN) {
        fprintf(stderr, "Did not receive complete TLS handshake header: %d\n", __LINE__);
        return NULL;
    }



    /* Advance to first TLS payload */
    p += TLS_HEADER_LEN;

    if (p - data >= data_len) {
        fprintf(stderr, "Did not receive complete TLS handshake header: %d\n", __LINE__);
        return NULL;
    }

    if (*p != TLS_HANDSHAKE_TYPE_CLIENT_HELLO) {
        fprintf(stderr, "Not a client hello\n");
        return NULL;
    }

    /* Skip past:
       1	Handshake Type
       3	Length
       2	Version (again)
       32	Random
       to	Session ID Length
     */
    p += 38;
    if (p - data >= data_len) {
        fprintf(stderr, "Did not receive complete TLS handshake header: %d\n", __LINE__);
        return NULL;
    }

    len = *p; /* Session ID Length */
    p += 1 + len; /* Skip session ID block */
    if (p - data >= data_len) {
        fprintf(stderr, "Did not receive complete TLS handshake header: %d\n", __LINE__);
        return NULL;
    }

    len = *p << 8; /* Cipher Suites length high byte */
    p ++;
    if (p - data >= data_len) {
        fprintf(stderr, "Did not receive complete TLS handshake header: %d\n", __LINE__);
        return NULL;
    }
    len += *p; /* Cipher Suites length low byte */

    p += 1 + len;

    if (p - data >= data_len) {
        fprintf(stderr, "Did not receive complete TLS handshake header: %d\n", __LINE__);
        return NULL;
    }
    len = *p; /* Compression Methods length */

    p += 1 + len;


    if (p - data >= data_len) {
        fprintf(stderr, "No extensions present in TLS handshake header: %d\n", __LINE__);
        return NULL;
    }


    len = *p << 8; /* Extensions length high byte */
    p++;
    if (p - data >= data_len) {
        fprintf(stderr, "Did not receive complete TLS handshake header: %d\n", __LINE__);
        return NULL;
    }
    len += *p; /* Extensions length low byte */
    p++;

    while (1) {
        if (p - data + 4 >= data_len) { /* 4 bytes for the extension header */
            fprintf(stderr, "No more TLS handshake extensions: %d\n", __LINE__);
            return NULL;
        }

        /* Parse our extension header */
        len = (p[2] << 8) + p[3]; /* Extension length */
        if (p[0] == 0x00 && p[1] == 0x00) { /* Check if it's a server name extension */
            /* There can be only one extension of each type, so we break
               our state and move p to beinging of the extension here */
            p += 4;
            if (p - data + len > data_len) {
                fprintf(stderr, "Did not receive complete TLS handshake header: %d\n", __LINE__);
                return NULL;
            }
            return parse_server_name_extension(p, len);
        }
        p += 4 + len; /* Advance to the next extension header */
    }
    return NULL;
}

static const char *
parse_server_name_extension(const uint8_t* buf, int buf_len) {
    static char server_name[SERVER_NAME_LEN];
    const uint8_t* p = buf;
    int ext_len;
    uint8_t name_type;
    int name_len;

    if (p - buf + 1 > buf_len) {
        fprintf(stderr, "Incomplete server name extension: %d\n", __LINE__);
        return NULL;
    }

    ext_len = (p[0] << 8) + p[1];
    p += 2;

    while(1) {
        if (p - buf >= buf_len) {
            fprintf(stderr, "Incomplete server name extension: %d\n", __LINE__);
            return NULL;
        }
        name_type = *p;
        p ++;
        switch(name_type) {
            case(0x00):
                if (p - buf + 1 > buf_len) {
                    fprintf(stderr, "Incomplete server name extension: %d\n", __LINE__);
                    return NULL;
                }
                name_len = (p[0] << 8) + p[1];
                p += 2;
                if (p - buf + name_len > buf_len) {
                    fprintf(stderr, "Incomplete server name extension: %d\n", __LINE__);
                    return NULL;
                }
                if (name_len >= SERVER_NAME_LEN - 1) {
                    fprintf(stderr, "Server name is too long\n");
                    return NULL;
                }
                strncpy (server_name, (char *)p, name_len);
                server_name[name_len] = '\0';
                return server_name;
            default:
                fprintf(stderr, "Unknown name type in server name extension\n");
        }
    }
}
