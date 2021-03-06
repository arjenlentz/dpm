/*
 *  Copyright 2008 Dormando (dormando@rydia.net).  All rights reserved.
 *
 *  Use and distribution licensed under the BSD license.  See
 *  the LICENSE file for full text.
*/

/* Dormando's Proxy for MySQL, with lua scripting! */

/* Internal headers */
#include "proxy.h"
#include "sha1.h"
#include "luaobj.h"

/* Internal defines */

#undef DBUG

#define BUF_SIZE 2048

#define VERSION "5"

const char *my_state_name[]={
    "Server connect", 
    "Client connect", 
    "Server sent handshake", 
    "Client wait handshake", 
    "Client waiting", 
    "Server waiting", 
    "Client sent command", 
    "Server sending fields", 
    "Server sending rows", 
    "Server waiting auth", 
    "Server sending OK", 
    "Server waiting command", 
    "Server sending resultset", 
    "Client waiting auth", 
    "Server sending handshake", 
    "Server got error",
    "Closing",
    "Server sending stats",
    "Server got command",
    "Server sending eof",
    "Server sent resultset",
    "Server sent fields",
};

/* Global structures/values. These will need to be thread local. */

struct lua_State *L;

int urandom_sock = 0;
int verbose      = 0;

/* Track connections which have had their write buffers appended to.
 * This is walked during run_protocol() */
conn *dpm_conn_flush_list = NULL;

/* Declarations */
static void sig_hup(const int sig);
int set_sock_nonblock(int fd);
static int handle_accept(int fd);
static int handle_read(conn *c);
static int handle_write(conn *c);
static conn *init_conn(int newfd);
static void handle_event(int fd, short event, void *arg);
static int add_conn_event(conn *c, const int new_flags);
//static int del_conn_event(conn *c, const int new_flags);
static int update_conn_event(conn *c, const int new_flags);
static int run_protocol(conn *c, int read, int written);

static int my_next_packet_start(conn *c);
static int grow_write_buffer(conn *c, int newsize);
static int sent_packet(conn *c, void **p, int ptype, int field_count);
static int received_packet(conn *c, void **p, int *ptype, int field_count);

/* Packet managers */
static void *my_consume_handshake_packet(conn *c);
static void my_free_handshake_packet(void *p);
static int my_wire_handshake_packet(conn *c, void *pkt);

static void *my_consume_auth_packet(conn *c);
static void my_free_auth_packet(void *p);
static int my_wire_auth_packet(conn *c, void *pkt);

static void *my_consume_ok_packet(conn *c);
static void my_free_ok_packet(void *p);
static int my_wire_ok_packet(conn *c, void *pkt);

static void *my_consume_err_packet(conn *c);
static void my_free_err_packet(void *pkt);
static int my_wire_err_packet(conn *c, void *pkt);

static void *my_consume_cmd_packet(conn *c);
static void my_free_cmd_packet(void *pkt);
static int my_wire_cmd_packet(conn *c, void *pkt);

static void *my_consume_rset_packet(conn *c);
static void my_free_rset_packet(void *pkt);
static int my_wire_rset_packet(conn *c, void *pkt);

static void *my_consume_row_packet(conn *c);
static void my_free_row_packet(void *pkt);
static int my_wire_row_packet(conn *c, void *pkt);

static void *my_consume_field_packet(conn *c);
static void my_free_field_packet(void *pkt);
static int my_wire_field_packet(conn *c, void *pkt);

static void *my_consume_eof_packet(conn *c);
static void my_free_eof_packet(void *pkt);
static int my_wire_eof_packet(conn *c, void *pkt);

static uint8_t my_char_val(uint8_t X);
static void my_hex2octet(uint8_t *dst, const char *src, unsigned int len);
static void my_crypt(char *dst, const unsigned char *s1, const unsigned char *s2, uint len);
static void my_scramble(char *dst, const char *random, const char *pass);
static int my_check_scramble(const char *remote_scram, const char *random, const char *stored_hash);

/* Lua related forward declarations. */
static int new_listener(lua_State *L);
static int new_connect(lua_State *L);
static int close_conn(lua_State *L);
static int check_pass(lua_State *L);
static int crypt_pass(lua_State *L);
static int wire_packet(lua_State *L);
static int run_lua_callback(conn *c, int nargs);
static int proxy_connect(lua_State *L);
static int proxy_disconnect(lua_State *L);

/* Wrappers for string handling. Replaceable with GString or more buffer
 * functions later.
 */

cbuffer_t *cbuffer_new(size_t len, const char *src)
{
    cbuffer_t *buf = (cbuffer_t *)malloc(sizeof(cbuffer_t) + len);
    if (buf == NULL) {
        perror("malloc");
        return NULL;
    }

    memcpy(buf->data, src, len);
    buf->data[len] = '\0';
    return buf;
}

void cbuffer_free(cbuffer_t *buf)
{
    free(buf);
}

/* Start of access functions. This thing should be more or less... write once.
 * if we're to rewrite, free/create a new one.
 */
inline size_t cbuffer_size(cbuffer_t *buf)
{
    return buf->len;
}

inline const char *cbuffer_data(cbuffer_t *buf)
{
    return buf->data;
}

/* Stack a connection for flushing later. */
static void _dpm_add_to_flush_list(conn *c)
{
    if (dpm_conn_flush_list)
        c->nextconn = (struct conn *)dpm_conn_flush_list;
    dpm_conn_flush_list = c;
}

/* Stub function. In the future, should set a flag to reload or dump stuff */
static void sig_hup(const int sig)
{
    fprintf(stdout, "Got reload request.\n");
}

int set_sock_nonblock(int fd)
{
    int flags = 1;

    if ( (flags = fcntl(fd, F_GETFL, 0)) < 0 ||
        fcntl(fd, F_SETFL, flags | O_NONBLOCK) < 0) {
        perror("Could not set O_NONBLOCK");
        close(fd);
        return -1;
    }

    return 0;
}

static int add_conn_event(conn *c, const int new_flags)
{
    int ret;
    ret = update_conn_event(c, c->ev_flags | new_flags);
    return ret;
}

/* FIXME: Logic is wrong */
/*static int del_conn_event(conn *c, const int new_flags)
{
    int ret;
    ret = update_conn_event(c, c->ev_flags & ~new_flags);
    return ret;
}*/

static int update_conn_event(conn *c, const int new_flags)
{
    if (c->ev_flags == new_flags) return 1;
    if (event_del(&c->ev) == -1) return 0;

    c->ev_flags = new_flags;
    event_set(&c->ev, c->fd, new_flags, handle_event, (void *)c);

    if (event_add(&c->ev, 0) == -1) return 0;
    return 1;
}

static int handle_accept(int fd)
{
    struct sockaddr_in addr;
    socklen_t addrlen = 0;
    int newfd;

    if ( (newfd = accept(fd, (struct sockaddr *)&addr, &addrlen)) == -1) {
        if (errno == EAGAIN || errno == EWOULDBLOCK) {
            if (verbose)
                fprintf(stderr, "Blocking error on accept. ignore?\n");
        } else if (errno == EMFILE) {
            fprintf(stderr, "Holy crap out of FDs!\n");
        } else {
            perror("Died on accept");
        }
    }

    return newfd;
}

void handle_close(conn *c)
{
    conn *remote;
    assert(c != 0);

    c->dpmstate = MY_CLOSING;
    run_lua_callback(c, 0);
    event_del(&c->ev);

    /* Release a connected remote connection.
     * FIXME: Is this detectable from within lua?
     */
    if (c->remote) {
        remote = (conn *)c->remote;
        remote->remote = NULL;
        c->remote = NULL;
    }

    close(c->fd);
    if (verbose)
        fprintf(stdout, "Closed connection for %llu listener: %s\n", (unsigned long long) c->id, c->listener ? "yes" : "no");
    if (c->rbuf) free(c->rbuf);
    if (c->wbuf) free(c->wbuf);
    c->alive = 0;
}

/* Generic "Grow my write buffer" function. */
static int grow_write_buffer(conn *c, int newsize)
{
    unsigned char *new_wbuf;

    if (c->wbufsize < newsize) {
        /* Figure the next power of two using bit magic */
        int nextsize = newsize - 1;
        int i        = 1;
        int intbits  = sizeof(nextsize) * 4;
        for (; i != intbits; i *= 2) {
           nextsize |= nextsize >> i; 
        }
        nextsize++;

        if (verbose)
            fprintf(stdout, "Reallocating write buffer from %d to %d\n", c->wbufsize, nextsize);
        new_wbuf = realloc(c->wbuf, nextsize);

        if (new_wbuf == NULL) {
            perror("Realloc output buffer");
            return -1;
        }

        c->wbuf     = new_wbuf;
        c->wbufsize = nextsize;
    }

    return 0;
}

/* handle buffering writes... we're looking for EAGAIN until we stop
 * transmitting.
 * We're assuming the write data was pre-populated.
 * NOTE: Need to support changes in written between calls
 */
static int handle_write(conn *c)
{
    int wbytes;
    int written = 0;

    /* Short circuit for outbound connections. */
    if (c->towrite < 1) {
        return written;
    }

    for(;;) {
        if (c->written == c->towrite) {
            c->mystate = my_reading;
            c->written = 0;
            c->towrite = 0;
            update_conn_event(c, EV_READ | EV_PERSIST);
            break;
        }

        wbytes = send(c->fd, c->wbuf + c->written, c->towrite - c->written, 0);

        if (wbytes == 0) {
            return -1;
        } else if (wbytes == -1) {
            if (errno == EAGAIN || errno == EWOULDBLOCK) {
                if (add_conn_event(c, EV_WRITE) == 0) {
                    fprintf(stderr, "Couldn't add write watch to %d\n", c->fd);
                    return -1;
                }
                /* Transient error. Come back later. */
                return 0;
            } else {
                perror("Unhandled write error");
                return -1;
            }
        }

        c->written += wbytes;
        written    += wbytes;
    }

    return written;
}

/* Handle buffered read events. Read into the buffer until we would block.
 * returns the total number of bytes read in the session. */
static int handle_read(conn *c)
{
    int rbytes;
    int newdata = 0;
    unsigned char *new_rbuf;

    for(;;) {
        /* We're in trouble if read is larger than rbufsize, right? ;) 
         * Anyhoo, if so, we want to realloc up the buffer.
         * TODO: Share buffers so we don't realloc so often... */
        if (c->read >= c->rbufsize) {
            /* I'd prefer 1.5... */
            if (verbose)
                fprintf(stdout, "Reallocing input buffer from %d to %d\n",
                    c->rbufsize, c->rbufsize * 2);
            new_rbuf = realloc(c->rbuf, c->rbufsize * 2);

            if (new_rbuf == NULL) {
                perror("Realloc input buffer");
                return -1;
            }

            /* The start of the new buffer might've changed: realloc(2) */
            c->rbuf = new_rbuf;
            c->rbufsize *= 2;
        }

        /* while bytes from read, pack into buffer. return when would block */
        rbytes = read(c->fd, c->rbuf + c->read, c->rbufsize - c->read);

        /* If signaled for reading and got zero bytes, close it up 
         * FIXME : Should we flush the command? */
        if (rbytes == 0 && newdata) {
            break;
        } else if (rbytes == 0) {
            return -1;
        } else if (rbytes == -1) {
            if (errno == EAGAIN || errno == EWOULDBLOCK) {
                return 0;
            } else {
                return -1;
            }
        }

        /* Successfuly read. Mark our progress */
        c->read += rbytes;
        newdata += rbytes;
    }

    /* Allows caller to arbitrarily measure progress, since we use a binary
     * protocol. "Did we get enough bytes to satisfy len? No? Yawn. Nap."
     */
    return newdata;
}

static conn *init_conn(int newfd)
{
    conn *newc;
    static int my_connection_counter = 1; /* NOTE: Not positive if this should be global or not */

    /* client typedef init should be its own function */
    newc = (conn *)malloc( sizeof(conn) ); /* error handling */
    memset(newc, 0, sizeof(conn));
    newc->fd = newfd;
    newc->id = my_connection_counter++;
    newc->ev_flags = EV_READ | EV_PERSIST;
    newc->mystate = my_reading;
    newc->dpmstate = my_waiting;

    /* Misc inits, for clarity. */
    newc->read        = 0;
    newc->readto      = 0;
    newc->written     = 0;
    newc->towrite     = 0;
    newc->my_type     = MY_CLIENT;
    newc->packetsize  = 0;
    newc->field_count = 0;
    newc->last_cmd    = 0;
    newc->packet_seq  = 0;
    newc->listener    = 0;
    newc->nextconn    = NULL;

    /* Set up the buffers. */
    newc->rbufsize = BUF_SIZE;
    newc->wbufsize = BUF_SIZE;

    newc->rbuf = (unsigned char *)malloc( (size_t)newc->rbufsize );
    newc->wbuf = (unsigned char *)malloc( (size_t)newc->wbufsize );

    /* Cleaner way to do this? I guess not with C */
    if (newc->rbuf == 0 || newc->wbuf == 0) {
        if (newc->rbuf != 0) free(newc->rbuf);
        if (newc->wbuf != 0) free(newc->wbuf);
        free(newc);
        perror("Could not malloc()");
        return NULL;
    }

    newc->remote  = NULL;

    /* Callback structures. Rest are zero from above memset. */
    newc->package_callback = NULL;

    event_set(&newc->ev, newfd, newc->ev_flags, handle_event, (void *)newc);
    event_add(&newc->ev, NULL); /* error handling */

    if (verbose)
        fprintf(stdout, "Made new conn structure for %d\n", newfd);

    return newc;
}

static void handle_event(int fd, short event, void *arg)
{
    conn *c = arg;
    conn *newc = NULL;
    struct linger l = {0, 0};
    int newfd, rbytes, wbytes;
    int flags = 1;
    int err   = 0;

    /* if we're the server socket, it's a new conn */
    if (c->listener) {
        newfd = handle_accept(fd); /* error handling */
        if (verbose)
            fprintf(stdout, "Got new client sock %d\n", newfd);

        set_sock_nonblock(newfd); /* error handling on this and below */
        setsockopt(newfd, IPPROTO_TCP, TCP_NODELAY, (void *)&flags, sizeof(flags));
        setsockopt(newfd, SOL_SOCKET, SO_LINGER, (void *)&l, sizeof(l));
        newc = init_conn(newfd);

        if (newc == NULL)
            return;

        newc->dpmstate  = MYC_WAIT_HANDSHAKE;
        newc->my_type   = MY_CLIENT;
        newc->alive++;

        /* Pass the object up into lua for later inspection. */
        new_obj(L, newc, "dpm.conn");
        /* And the id of our listener object. */
        lua_pushinteger(L, c->id);

        c->dpmstate = MYC_CONNECT;
        run_lua_callback(c, 2);

        /* The callback might've written packets to the wire. */
        if (newc->towrite) {
            if (handle_write(newc) == -1)
                return;
        }
        return;
   }
   
   if (event & EV_READ) {
        /* Client socket. */
        rbytes = handle_read(c);
        /* FIXME : Should we do the error handling at this level? Or lower? */
        if (rbytes < 0) {
            handle_close(c);
            return;
        }

    }

    if (event & EV_WRITE) {
        if (c->mystate != my_connect) {
          wbytes = handle_write(c);

          if (wbytes < 0) {
              handle_close(c);
              return;
          }
        }
    }

    err = run_protocol(c, rbytes, wbytes);
    if (err == -1) {
        handle_close(c);
    }
}

/* MySQL Protocol support routines */

/* Read a length encoded binary field into a uint64_t */
uint64_t my_read_binary_field(unsigned char *buf, int *base)
{
    uint64_t ret = 0;

    if (buf[*base] < 251) {
        (*base)++;
        return (uint64_t) buf[*base - 1];
    }

    (*base)++;
    switch (buf[*base - 1]) {
        case 251:
            return MYSQL_NULL;
        case 252:
            ret = uint2korr(&buf[*base]);
            (*base) += 2;
            break;
        case 253:
            /* NOTE: Docs say this is 32-bit. libmysqlnd says 24-bit? */
            ret = uint4korr(&buf[*base]);
            (*base) += 4;
            break;
        case 254:
            ret = uint8korr(&buf[*base]);
            (*base) += 8;
    }

    return ret;
}

/* Same as above, but writes the binary field into buffer. */
void my_write_binary_field(unsigned char *buf, int *base, uint64_t length)
{
    if (length < (uint64_t) 251) {
        *buf = length;
        (*base)++;
        return;
    }

    if (length < (uint64_t) 65536) {
        *buf++ = 252;
        int2store(buf, (uint16_t) length);
        (*base) += 2;
        return;
    }

    if (length < (uint64_t) 16777216) {
        *buf++ = 253;
        int3store(buf, (uint32_t) length);
        (*base) += 3;
        return;
    }

    if (length == MYSQL_NULL) {
        *buf = 251;
        (*base)++;
        return;
    }

    *buf++ = 254;
    int8store(buf, length);
    (*base) += 8;
}

/* Returns the binary size of a field, for use in pre-allocating wire buffers
 */
int my_size_binary_field(uint64_t length)
{
    if (length < (uint64_t) 251) 
        return 1;

    if (length < (uint64_t) 65536)
        return 3;

    if (length < (uint64_t) 16777216)
        return 5;

    if (length == MYSQL_NULL)
        return 1;

    return 9;
}

static uint8_t my_char_val(uint8_t X)
{
  return (unsigned int) (X >= '0' && X <= '9' ? X-'0' :
      X >= 'A' && X <= 'Z' ? X-'A'+10 : X-'a'+10);
}

static void my_hex2octet(uint8_t *dst, const char *src, unsigned int len)
{   
  const char *str_end= src + len;
  while (src < str_end) {
      char tmp = my_char_val(*src++);
      *dst++ = (tmp << 4) | my_char_val(*src++);
  }
}

static void my_crypt(char *dst, const unsigned char *s1, const unsigned char *s2, uint len)
{
  const uint8_t *s1_end= s1 + len;
  while (s1 < s1_end)
    *dst++ = *s1++ ^ *s2++;
}
/* End. */

/* Client scramble
 * random is 20 byte random scramble from the server.
 * pass is plaintext password supplied from client
 * dst is a 20 byte buffer to receive the jumbled mess. */
static void my_scramble(char *dst, const char *random, const char *pass)
{
    SHA1_CTX context;
    uint8_t hash1[SHA1_DIGEST_LENGTH];
    uint8_t hash2[SHA1_DIGEST_LENGTH];
    /* Make sure the null terminator's in the right spot. */
    dst[SHA1_DIGEST_LENGTH] = '\0';

    /* First hash the password. */
    SHA1Init(&context);
    SHA1Update(&context, (const uint8_t *) pass, strlen(pass));
    SHA1Final(hash1, &context);

    /* Second, hash the hash. */
    SHA1Init(&context);
    SHA1Update(&context, hash1, SHA1_DIGEST_LENGTH);
    SHA1Final(hash2, &context);

    /* Now we have the equivalent of SELECT PASSWORD('whatever') */
    /* Now SHA1 the random message against hash2, then xor it against hash1 */
    SHA1Init(&context);
    SHA1Update(&context, (const uint8_t *) random, SHA1_DIGEST_LENGTH);
    SHA1Update(&context, hash2, SHA1_DIGEST_LENGTH);
    SHA1Final((uint8_t *) dst, &context);

    my_crypt((char *)dst, (const unsigned char *) dst, hash1, SHA1_DIGEST_LENGTH);

    /* The sha1 context has temporary data that needs to disappear. */
    memset(&context, 0, sizeof(SHA1_CTX));
}

/* Server side check. */
static int my_check_scramble(const char *remote_scram, const char *random, const char *stored_hash)
{
    uint8_t pass_hash[SHA1_DIGEST_LENGTH];
    uint8_t rand_hash[SHA1_DIGEST_LENGTH];
    uint8_t pass_orig[SHA1_DIGEST_LENGTH];
    uint8_t pass_check[SHA1_DIGEST_LENGTH];
    SHA1_CTX context;

    /* Parse string into bytes... */
    my_hex2octet(pass_hash, stored_hash, strlen(stored_hash));

    /* Muck up our view of the password against our original random num */
    SHA1Init(&context);
    SHA1Update(&context, (const uint8_t *) random, SHA1_DIGEST_LENGTH);
    SHA1Update(&context, pass_hash, SHA1_DIGEST_LENGTH);
    SHA1Final(rand_hash, &context);

    /* Pull out the client sha1 */
    my_crypt((char *) pass_orig, (const unsigned char *) rand_hash, (const unsigned char *) remote_scram, SHA1_DIGEST_LENGTH);

    /* Update it to be more like our own */
    SHA1Init(&context);
    SHA1Update(&context, pass_orig, SHA1_DIGEST_LENGTH);
    SHA1Final(pass_check, &context);
    memset(&context, 0, sizeof(SHA1_CTX));

    /* Compare */
    return memcmp(pass_hash, pass_check, SHA1_DIGEST_LENGTH);
}

/* If we're ready to send the next packet along, prep the header and
 * return the starting position. */
static int my_next_packet_start(conn *c)
{
    int seq = 0;
    /* A couple sanity checks... First is that we must have enough bytes
     * readable to try consuming a header. */
    if (c->readto + 3 > c->read)
        return -1;

    c->packetsize = uint3korr(&c->rbuf[c->readto]);
    seq           = uint1korr(&c->rbuf[c->readto + 3]);
    c->packetsize += 4;

    /* Don't handle large packets right now.
     * TODO: This actually shouldn't be too hard. Keep spooling with this
     * function until we can scan to the end of the function within the buffer
     * structure. Ugly, but the protocol's ugly anyway.
     */
    if (c->packetsize == 0 && seq == 255) {
        fprintf(stderr, "***WARNING*** DPM does not support packet sizes larger than 16M currently. If you report this warning it will probably be fixed.");
        handle_close(c);
        return -1;
    }

    /* If we've read a packet header, see if we have the whole packet. */
    if (c->read - c->readto >= c->packetsize) {
        /* Test the packet header. Is it out of sequence? */
        /* FIXME: The MY_CLIENT hack is because we're not fully tracking client
         * state. So if the consumer is a client and the header's zero for no
         * reason, it's probably a new command packet and will get fixed
         * later.
         */
        if (c->packet_seq != seq && !(c->my_type == MY_CLIENT && seq == 0)) {
            fprintf(stderr, "***WARNING*** Packets appear to be out of order: type [%d] conn [%d], header [%d]\n", c->my_type, c->packet_seq, seq);
        }
        return c->readto;
    }

    return -1;
}

/* TODO: In another life this should be some crazy struct buffer. */
static void my_free_handshake_packet(void *p)
{
    /* No allocated memory, easy. */
    free(p);
}

/* Takes handshake packet *p and writes as a packet into c's write buffer. */
static int my_wire_handshake_packet(conn *c, void *pkt)
{
    my_handshake_packet *p = (my_handshake_packet *)pkt;
    int psize = 45;
    size_t my_size = strlen(p->server_version) + 1;
    int base = c->towrite;

    /* We must discover the length of the packet first, so we can size the
     * buffer. HS packets are 45 bytes + strlen(server_version) + 1
     */
    psize += my_size + 4;
    
    if (grow_write_buffer(c, c->towrite + psize) == -1) {
        return -1;
    }

    c->towrite += psize;

    int3store(&c->wbuf[base], psize - 4);
    base += 3;
    int1store(&c->wbuf[base], c->packet_seq);
    base++;

    c->wbuf[base] = p->protocol_version;
    base++;

    memcpy(&c->wbuf[base], p->server_version, my_size);
    base += my_size;

    int4store(&c->wbuf[base], p->thread_id);
    base += 4;

    memcpy(&c->wbuf[base], p->scramble_buff, 8);
    base += 8;

    c->wbuf[base] = 0;
    base++;

    int2store(&c->wbuf[base], p->server_capabilities);
    base += 2;

    c->wbuf[base] = p->server_language;
    base++;

    int2store(&c->wbuf[base], p->server_status);
    base += 2;

    memset(&c->wbuf[base], 0, 13);
    base += 13;

    memcpy(&c->wbuf[base], p->scramble_buff + 8, 13);

    return psize;
}

/* Creates an "empty" handshake packet */
void *my_new_handshake_packet()
{
    my_handshake_packet *p;
    int i; char next_rand;
 
    p = (my_handshake_packet *)malloc( sizeof(my_handshake_packet) );
    if (p == NULL) {
        perror("Could not malloc()");
        return NULL;
    }
    memset(p, 0, sizeof(my_handshake_packet));

    p->h.ptype   = dpm_handshake;
    p->h.free_me = my_free_handshake_packet;
    p->h.to_buf  = my_wire_handshake_packet;
    p->protocol_version = 10; /* FIXME: Should be a define? */
    strcpy(p->server_version, "5.0.37"); /* :P */
    p->thread_id = 1; /* Who cares. */
    p->server_capabilities = CLIENT_LONG_PASSWORD | CLIENT_LONG_FLAG | CLIENT_CONNECT_WITH_DB | CLIENT_PROTOCOL_41 | CLIENT_TRANSACTIONS | CLIENT_SECURE_CONNECTION;
    p->server_language = 8;
    p->server_status = SERVER_STATUS_AUTOCOMMIT;

    for (i = 0; i != SHA1_DIGEST_LENGTH; i++) {
        read(urandom_sock, &next_rand, 1);
        p->scramble_buff[i] = next_rand * 94 + 33;
    }

    return p;
}

/* FIXME: If we have the second scramblebuff, it needs to be assembled
 * into a single line for processing.
 */
static void *my_consume_handshake_packet(conn *c)
{
    my_handshake_packet *p;
    int base = c->readto + 4;
    size_t my_size = 0;

    /* Clear out the struct. */
    p = (my_handshake_packet *)malloc( sizeof(my_handshake_packet) );
    if (p == NULL) {
        perror("Could not malloc()");
        return NULL;
    }
    memset(p, 0, sizeof(my_handshake_packet));

    p->h.ptype   = dpm_handshake;
    p->h.free_me = my_free_handshake_packet;
    p->h.to_buf  = my_wire_handshake_packet;

    /* We only support protocol 10 right now... */
    p->protocol_version = c->rbuf[base];
    if (p->protocol_version != 10) {
        fprintf(stderr, "We only support protocol version 10! Closing.\n");
        return NULL;
    }

    base++;

    /* Server version string. Crappy malloc. */
    my_size = strlen((const char *)&c->rbuf[base]);

    /* +1 to account for the \0 */
    my_size++;

    if (my_size > SERVER_VERSION_LENGTH) {
        fprintf(stderr, "Server version string is too long! Closing.\n");
        return NULL;
    }

    memcpy(p->server_version, &c->rbuf[base], my_size);
    base += my_size;

    /* 4 byte thread id */
    p->thread_id = uint4korr(&c->rbuf[base]);
    base += 4;

    /* First 8 bytes of scramble_buff. Sandwich with 12 more + \0 later */
    memcpy(&p->scramble_buff, &c->rbuf[base], 8);
    base += 8;

    /* filler1 should be 0 */
    base++;

    /* Set of flags for server caps. */
    /* TODO: Need to explicitly disable compression, ssl, other features we
     * don't support. */
    p->server_capabilities = uint2korr(&c->rbuf[base]);
    base += 2;

    /* Language setting. Pass-through and/or ignore. */
    p->server_language = c->rbuf[base];
    base++;

    /* Server status flags. AUTOCOMMIT flags and such? */
    p->server_status = uint2korr(&c->rbuf[base]);
    base += 2;

    /* More zeroes. */
    base += 13;

    /* Rest of random number "string" */
    memcpy(&p->scramble_buff[8], &c->rbuf[base], 13);
    base += 13;

    new_obj(L, p, "dpm.handshake");

    return p;
}

static void my_free_auth_packet(void *pkt)
{
    my_auth_packet *p = (my_auth_packet *)pkt;
    if (p->databasename)
        free(p->databasename);
    free(p);
}

void *my_new_auth_packet()
{
    my_auth_packet *p;

    /* Clear out the struct. */
    p = (my_auth_packet *)malloc( sizeof(my_auth_packet) );
    if (p == NULL) {
        perror("Could not malloc()");
        return NULL;
    }
    memset(p, 0, sizeof(my_auth_packet));

    p->h.ptype   = dpm_auth;
    p->h.free_me = my_free_auth_packet;
    p->h.to_buf  = my_wire_auth_packet;

    p->client_flags = CLIENT_LONG_PASSWORD | CLIENT_LONG_FLAG | CLIENT_CONNECT_WITH_DB | CLIENT_PROTOCOL_41 | CLIENT_TRANSACTIONS | CLIENT_SECURE_CONNECTION;

    p->max_packet_size = 16777216; /* FIXME: Double check this. */
    p->charset_number = 8;
    strcpy(p->user, "root"); /* FIXME: Needs to be editable. */
    p->databasename = NULL; /* Don't need a default DB. */
    p->scramble_buff[20] = 1;

    return p;
}

static int my_wire_auth_packet(conn *c, void *pkt)
{
    my_auth_packet *p = (my_auth_packet *)pkt;
    int psize = 32;
    size_t user_size = strlen(p->user) + 1;
    size_t dbname_size = 0;
    int base = c->towrite;

    /* password, or no password. */
    psize += p->scramble_buff[20] == '\0' ? 21 : 1;

    /* databasename, or no databasename. */
    if (p->databasename)
        dbname_size = strlen(p->databasename) + 1;
    /* Add in the username length + header. */
    psize += user_size + dbname_size + 4;

    if (grow_write_buffer(c, c->towrite + psize) == -1) {
        return -1;
    }

    c->towrite += psize;

    int3store(&c->wbuf[base], psize - 4);
    base += 3;
    int1store(&c->wbuf[base], c->packet_seq);
    base++;

    int4store(&c->wbuf[base], p->client_flags);
    base += 4;

    int4store(&c->wbuf[base], p->max_packet_size);
    base += 4;

    c->wbuf[base] = p->charset_number;
    base++;

    memset(&c->wbuf[base], 0, 23);
    base += 23;

    memcpy(&c->wbuf[base], p->user, user_size);
    base += user_size;

    if (p->scramble_buff[20] == '\0') {
        c->wbuf[base] = 20; /* Length of scramble buff. */
        base++;
        memcpy(&c->wbuf[base], p->scramble_buff, 20);
        base += 20;
    } else {
        /* Note this could be an error condition... as far as the docs go
         * the password size is _always_ either 0 or 20. */
        c->wbuf[base] = 0;
        base++;
    }

    if (dbname_size) {
        memcpy(&c->wbuf[base], p->databasename, dbname_size);
        base += dbname_size;
    }

    return 0;
}

/* FIXME: Two stupid optional params. if no scramble buf, and no database
 * name, is that the end of the packet? Should test, instead of strlen'ing
 * random memory.
 */
static void *my_consume_auth_packet(conn *c)
{
    my_auth_packet *p;
    int base = c->readto + 4;
    size_t my_size = 0;

    /* Clear out the struct. */
    p = (my_auth_packet *)malloc( sizeof(my_auth_packet) );
    if (p == NULL) {
        perror("Could not malloc()");
        return NULL;
    }
    memset(p, 0, sizeof(my_auth_packet));

    p->h.ptype   = dpm_auth;
    p->h.free_me = my_free_auth_packet;
    p->h.to_buf  = my_wire_auth_packet;

    /* Client flags. Same as server_flags with some crap added/removed.
     * at this point in packet processing we should take out unsupported
     * options.
     */
    p->client_flags = uint4korr(&c->rbuf[base]);
    base += 4;

    /* Should we short circuit this to something more reasonable for latency?
     */
    p->max_packet_size = uint4korr(&c->rbuf[base]);
    base += 4;

    p->charset_number = c->rbuf[base];
    base++;

    /* Skip the filler crap. */
    base += 23;

    /* Supplied username. */
    /* FIXME: This string reading crap should be a helper function. */
    my_size = strlen((const char *)&c->rbuf[base]);

    if (my_size - 1 > USERNAME_LENGTH) {
        fprintf(stderr, "Username too long!\n");
        return NULL;
    }

    memcpy(p->user, &c->rbuf[base], my_size + 1);
    /* +1 to account for the \0 */
    base += my_size + 1;

    /* FIXME: scramble_buf is random, so this can be zero?
     * figure out a better way of parsing the data.
     */
    /* If we don't have a scramble, leave it all zeroes. */
    if (c->rbuf[base] > 0) {
        memcpy(&p->scramble_buff, &c->rbuf[base + 1], 21);
        base += 21;
    } else {
        /* I guess this "filler" is only here if there's no scramble. */
        base++;
    }

    if (c->packetsize > base) {
        my_size = strlen((const char *)&c->rbuf[base]);
        p->databasename = (char *)malloc( my_size );

        if (p->databasename == 0) {
            perror("Could not malloc()");
            return NULL;
        }
        memcpy(p->databasename, &c->rbuf[base], my_size + 1);
        /* +1 to account for the \0 */
        base += my_size + 1;
    }

    new_obj(L, p, "dpm.auth");

    return p;
}

static void my_free_ok_packet(void *pkt)
{
    my_ok_packet *p = (my_ok_packet *)pkt;
    if (p->message)
        free(p->message);

    free(p);
}

void *my_new_ok_packet()
{
    my_ok_packet *p;

    /* Clear out the struct. */
    p = (my_ok_packet *)malloc( sizeof(my_ok_packet) );
    if (p == NULL) {
        perror("Could not malloc()");
        return NULL;
    }
    memset(p, 0, sizeof(my_ok_packet));

    p->h.ptype   = dpm_ok;
    p->h.free_me = my_free_ok_packet;
    p->h.to_buf  = my_wire_ok_packet;

    p->server_status = SERVER_STATUS_AUTOCOMMIT; /* default autocommit mode */

    p->message = NULL;

    return p;
}

static int my_wire_ok_packet(conn *c, void *pkt)
{
    my_ok_packet *p = (my_ok_packet *)pkt;
    int base = c->towrite;

    int psize = 9; /* misc chunks + header */
    psize += my_size_binary_field(p->affected_rows);
    psize += my_size_binary_field(p->insert_id);
    if (p->message_len) {
        psize += my_size_binary_field(p->message_len);
        psize += p->message_len;
    }

    if (grow_write_buffer(c, c->towrite + psize) == -1) {
        return -1;
    }

    c->towrite += psize;

    int3store(&c->wbuf[base], psize - 4);
    base += 3;
    int1store(&c->wbuf[base], c->packet_seq);
    base++;

    c->wbuf[base] = p->field_count;
    base++;

    my_write_binary_field(&c->wbuf[base], &base, p->affected_rows);
    my_write_binary_field(&c->wbuf[base], &base, p->insert_id);

    int2store(&c->wbuf[base], p->server_status);
    base += 2;

    int2store(&c->wbuf[base], p->warning_count);
    base += 2;

    if (p->message_len) {
        my_write_binary_field(&c->wbuf[base], &base, p->message_len);
        memcpy(&c->wbuf[base], p->message, p->message_len);
    }

    return 0;
}

static void *my_consume_ok_packet(conn *c)
{
    my_ok_packet *p;
    int base = c->readto + 4;
    uint64_t my_size = 0;

    /* Clear out the struct. */
    p = (my_ok_packet *)malloc( sizeof(my_ok_packet) );
    if (p == NULL) {
        perror("Could not malloc()");
        return NULL;
    }
    memset(p, 0, sizeof(my_ok_packet));

    p->h.ptype = dpm_ok;
    p->h.free_me = my_free_ok_packet;
    p->h.to_buf  = my_wire_ok_packet;

    p->affected_rows = my_read_binary_field(c->rbuf, &base);

    p->insert_id = my_read_binary_field(c->rbuf, &base);

    p->server_status = uint2korr(&c->rbuf[base]);
    base += 2;

    p->warning_count = uint2korr(&c->rbuf[base]);
    base += 2;

    if (c->packetsize > base - c->readto && (my_size = my_read_binary_field(c->rbuf, &base))) {
        p->message = (char *)malloc( my_size );
        if (p->message == 0) {
            perror("Could not malloc()");
            return NULL;
        }
        p->message_len = my_size;
        memcpy(p->message, &c->rbuf[base], my_size);
    } else {
        p->message = NULL;
    }

    new_obj(L, p, "dpm.ok");

    return p;
}

static void my_free_err_packet(void *p)
{
    free(p);
}

void *my_new_err_packet()
{
    my_err_packet *p;

    /* Clear out the struct. */
    p = (my_err_packet *)malloc( sizeof(my_err_packet) );
    if (p == NULL) {
        perror("Could not malloc()");
        return NULL;
    }
    memset(p, 0, sizeof(my_err_packet));

    p->h.ptype = dpm_err;
    p->h.free_me = my_free_err_packet;
    p->h.to_buf = my_wire_err_packet;

    p->field_count = 255; /* Always 255 */

    /* FIXME: Defaulting this to the "Access denied" error codes */
    p->errnum = 1045;
    strcpy(p->sqlstate, "28000");
    strcpy(p->message, "Access denied for user 'whatever'@'whatever'");
    
    return p;
}

static int my_wire_err_packet(conn *c, void *pkt)
{
    my_err_packet *p = (my_err_packet *)pkt;
    int base = c->towrite;
    size_t my_size = strlen(p->message) + 1;

    int psize = 13; /* misc chunks + header */
    psize += my_size;

    if (grow_write_buffer(c, c->towrite + psize) == -1) {
        return -1;
    }

    c->towrite += psize;

    int3store(&c->wbuf[base], psize - 4);
    base += 3;
    int1store(&c->wbuf[base], c->packet_seq);
    base++;

    c->wbuf[base] = p->field_count;
    base++;

    int2store(&c->wbuf[base], p->errnum);
    base += 2;

    c->wbuf[base] = '#';
    base++;

    memcpy(&c->wbuf[base], p->sqlstate, 5);
    base += 5;

    memcpy(&c->wbuf[base], p->message, my_size);

    return 0;
}

/* FIXME: There might be an "unknown error" state which changes the packet
 * payload.
 */
static void *my_consume_err_packet(conn *c)
{
    my_err_packet *p;
    int base = c->readto + 4;
    size_t my_size = 0;

    /* Clear out the struct. */
    p = (my_err_packet *)malloc( sizeof(my_err_packet) );
    if (p == NULL) {
        perror("Could not malloc()");
        return NULL;
    }
    memset(p, 0, sizeof(my_err_packet));

    p->h.ptype = dpm_err;
    p->h.free_me = my_free_err_packet;
    p->h.to_buf = my_wire_err_packet;

    p->field_count = c->rbuf[base]; /* Always 255... */
    base++;

    p->errnum = uint2korr(&c->rbuf[base]);
    base += 2;

    p->marker = c->rbuf[base];
    base++;

    memcpy(&p->sqlstate, &c->rbuf[base], 5);
    base += 5;

    /* Have to add our own null termination... */
    p->sqlstate[6] = '\0';

    /* Why couldn't they just use a packed string? Or a null terminated
     * string? Was it really worth saving one byte when it should be numeric
     * anyway?
     */
    my_size = c->packetsize - (base - c->readto);

    if (my_size > MYSQL_ERRMSG_SIZE - 1) {
        fprintf(stderr, "Error message too large! [%d]\n", (int) my_size);
        return NULL;
    }

    memcpy(p->message, &c->rbuf[base], my_size);
    p->message[my_size] = '\0';

    new_obj(L, p, "dpm.err");

    return p;
}

void *my_new_cmd_packet()
{
    my_cmd_packet *p;

    /* Clear out the struct. */
    p = (my_cmd_packet *)malloc( sizeof(my_cmd_packet) );
    if (p == NULL) {
        perror("Could not malloc()");
        return NULL;
    }
    memset(p, 0, sizeof(my_cmd_packet));

    p->h.ptype   = dpm_cmd;
    p->h.free_me = my_free_cmd_packet;
    p->h.to_buf  = my_wire_cmd_packet;

    p->command = COM_QUERY;

    /* FIXME: Stupid default. */
    p->argument = (char *)malloc( 75 );
    if (p->argument == 0) {
        perror("Could not malloc()");
        return NULL;
    }

    strcpy(p->argument, "select @@version limit 1");

    return p;
}

static void my_free_cmd_packet(void *pkt)
{
    my_cmd_packet *p = (my_cmd_packet *)pkt;
    free(p->argument);
    free(p);
}

/* NOTE: This "trims" the null byte off of our argument, since supposedly
 * this is normal.
 */
static int my_wire_cmd_packet(conn *c, void *pkt)
{
    my_cmd_packet *p = (my_cmd_packet *)pkt;
    int base         = c->towrite;
    size_t mysize    = 0;

    int psize = 5; /* misc chunks + header */

    if (p->argument) {
        /* 'argument' is normally not null terminated, but we should process
         * it as so from lua. Guess we should also cut it back off.
         */
        mysize = strlen(p->argument);
    }

    psize += mysize;

    if (grow_write_buffer(c, c->towrite + psize) == -1) {
        return -1;
    }

    c->towrite += psize;

    int3store(&c->wbuf[base], psize - 4);
    base += 3;
    int1store(&c->wbuf[base], 0);
    base++;

    c->wbuf[base] = p->command;
    base++;

    if (p->argument) {
        memcpy(&c->wbuf[base], p->argument, psize - 5);
    }

    return 0;
}

static void *my_consume_cmd_packet(conn *c)
{
    my_cmd_packet *p;
    int base = c->readto + 4;
    size_t my_size = 0;

    /* Clear out the struct. */
    p = (my_cmd_packet *)malloc( sizeof(my_cmd_packet) );
    if (p == NULL) {
        perror("Could not malloc()");
        return NULL;
    }
    memset(p, 0, sizeof(my_cmd_packet));

    p->h.ptype   = dpm_cmd;
    p->h.free_me = my_free_cmd_packet;
    p->h.to_buf  = my_wire_cmd_packet;

    p->command = c->rbuf[base];
    base++;

    my_size = c->packetsize - (base - c->readto);

    p->argument = (char *)malloc( my_size + 1 );
    if (p->argument == 0) {
        perror("Could not malloc()");
        return NULL;
    }
    memcpy(p->argument, &c->rbuf[base], my_size);
    p->argument[my_size] = '\0';

    new_obj(L, p, "dpm.cmd");

    return p;
}

void *my_new_rset_packet()
{
    my_rset_packet *p;

    p = (my_rset_packet *)malloc( sizeof(my_rset_packet) );
    if (p == NULL) {
        perror("Could not malloc()");
        return NULL;
    }
    memset(p, 0, sizeof(my_rset_packet));

    p->h.ptype   = dpm_rset;
    p->h.free_me = my_free_rset_packet;
    p->h.to_buf  = my_wire_rset_packet;

    p->fields = NULL;

    return p;
}

/* It's _very_ important that anything referred to by 'fields' gets
 * unreferenced. This can happen either as a custom gc handler or in here.
 */
static void my_free_rset_packet(void *pkt)
{
    my_rset_packet *p = (my_rset_packet *)pkt;
    free(p->fields);
    free(p);
}

static void *my_consume_rset_packet(conn *c)
{
    my_rset_packet *p;
    int base = c->readto + 4;

    /* Clear out the struct. */
    p = (my_rset_packet *)malloc( sizeof(my_rset_packet) );
    if (p == NULL) {
        perror("Could not malloc()");
        return NULL;
    }
    memset(p, 0, sizeof(my_rset_packet));

    p->h.ptype   = dpm_rset;
    p->h.free_me = my_free_rset_packet;
    p->h.to_buf  = my_wire_rset_packet;

    p->field_count = my_read_binary_field(c->rbuf, &base);
    c->field_count = p->field_count;

    if (c->packetsize > (base - c->readto)) {
        p->extra = my_read_binary_field(c->rbuf, &base);
    }

    p->fields = malloc( sizeof(my_rset_field_header) * p->field_count );
    if (p->fields == NULL) {
        perror("Could not malloc()");
        return NULL;
    }

    new_obj(L, p, "dpm.rset");

    return p;
}

/* This is a magic packet, but the only thing we need to really send
 * will be the one field. We can send 'extra' once I know what the crap it is.
 */
static int my_wire_rset_packet(conn *c, void *pkt)
{
    my_rset_packet *p = (my_rset_packet *)pkt;
    int base          = c->towrite;

    int psize = 4;
    psize += my_size_binary_field(p->field_count);

    if (grow_write_buffer(c, c->towrite + psize) == -1) {
        return -1;
    }

    c->towrite += psize;

    int3store(&c->wbuf[base], psize - 4);
    base += 3;
    int1store(&c->wbuf[base], c->packet_seq);
    base++;

    my_write_binary_field(&c->wbuf[base], &base, p->field_count);

    return 0;
}

void *my_new_field_packet()
{
    my_field_packet *p;

    p = (my_field_packet *)malloc( sizeof(my_field_packet) );
    if (p == NULL) {
        perror("Could not malloc()");
        return NULL;
    }
    memset(p, 0, sizeof(my_field_packet));

    p->h.ptype   = dpm_field;
    p->h.free_me = my_free_field_packet;
    p->h.to_buf  = my_wire_field_packet;

    p->fields    = NULL;

    /* BS some defaults. */
    p->charsetnr = 63;
    p->length    = 32;
    p->flags     = PRI_KEY_FLAG;

    return p;
}

static void my_free_field_packet(void *pkt)
{
    my_field_packet *p = pkt;
    free(p->fields);
    free(p);
}

static void *my_consume_field_packet(conn *c)
{
    my_field_packet *p;
    int base = c->readto + 4;
    size_t my_size = 0;
    unsigned char *start_ptr;

    /* Clear out the struct. */
    p = (my_field_packet *)malloc( sizeof(my_field_packet) );
    if (p == NULL) {
        perror("Could not malloc()");
        return NULL;
    }
    memset(p, 0, sizeof(my_field_packet));

    p->h.ptype   = dpm_field;
    p->h.free_me = my_free_field_packet;
    p->h.to_buf  = my_wire_field_packet;

    /* This packet type has a ton of dynamic length fields.
     * What we're going to do instead of 6 mallocs is use an offset table
     * and a bunch of pointers into one fat malloc.
     */

    my_size = c->packetsize + 12; /* Extra room for null bytes */

    p->fields = (unsigned char *)malloc( my_size );
    if (p->fields == NULL) {
        perror("Malloc()");
        return NULL;
    }
    start_ptr = p->fields;

    /* This is the basic repetition here.
     * The protocol docs say there might be \0's here, but lets add them
     * anyway... Many of the clients do.
     */
    p->catalog_len = my_read_binary_field(c->rbuf, &base);
    p->catalog = start_ptr;
    memcpy(p->catalog, &c->rbuf[base], p->catalog_len);
    base += p->catalog_len;
    *(start_ptr += p->catalog_len) = '\0';

    p->db_len = my_read_binary_field(c->rbuf, &base);
    p->db = start_ptr + 1;
    memcpy(p->db, &c->rbuf[base], p->db_len);
    base += p->db_len;
    *(start_ptr += p->db_len + 1) = '\0';

    p->table_len = my_read_binary_field(c->rbuf, &base);
    p->table = start_ptr + 1;
    memcpy(p->table, &c->rbuf[base], p->table_len);
    base += p->table_len;
    *(start_ptr += p->table_len + 1) = '\0';

    p->org_table_len = my_read_binary_field(c->rbuf, &base);
    p->org_table = start_ptr + 1;
    memcpy(p->org_table, &c->rbuf[base], p->org_table_len);
    base += p->org_table_len;
    *(start_ptr += p->org_table_len + 1) = '\0';

    p->name_len = my_read_binary_field(c->rbuf, &base);
    p->name = start_ptr + 1;
    memcpy(p->name, &c->rbuf[base], p->name_len);
    base += p->name_len;
    *(start_ptr += p->name_len + 1) = '\0';

    p->org_name_len = my_read_binary_field(c->rbuf, &base);
    p->org_name = start_ptr + 1;
    memcpy(p->org_name, &c->rbuf[base], p->org_name_len);
    base += p->org_name_len;
    *(start_ptr += p->org_name_len + 1) = '\0';

    /* Rest of this packet is straightforward. */

    /* Skip filler field */
    base++;

    p->charsetnr = uint2korr(&c->rbuf[base]);
    base += 2;

    p->length = uint4korr(&c->rbuf[base]);
    base += 4;

    p->type = c->rbuf[base];
    base++;

    p->flags = uint2korr(&c->rbuf[base]);
    base += 2;

    p->decimals = c->rbuf[base];
    base++;

    /* Skip second filler field */
    base += 2;

    /* Default is optional? */
    /* FIXME: I might be confusing this as a length encoded number, when it's
     * a length encoded string of binary data. */
    /* Notes: It's a length encoded string... but the length can also be the
     * NULL value, and thus no data? Complex corner case, fix later. */
    if (c->packetsize > (base - c->readto)) {
        p->my_default = my_read_binary_field(c->rbuf, &base);
        p->has_default++;
    }

    new_obj(L, p, "dpm.field");

    return p;
}

static int my_wire_field_packet(conn *c, void *pkt)
{
    my_field_packet *p = pkt;
    int base           = c->towrite;

    int psize = 4;

    psize += p->catalog_len + p->db_len + p->table_len + p->org_table_len +
             p->name_len + p->org_name_len + 13;

    psize += my_size_binary_field(p->catalog_len);
    psize += my_size_binary_field(p->db_len);
    psize += my_size_binary_field(p->table_len);
    psize += my_size_binary_field(p->org_table_len);
    psize += my_size_binary_field(p->name_len);
    psize += my_size_binary_field(p->org_name_len);

    /* MySQL doesn't seem to mind if this is missing, but:
     * FIXME: Make the default value work.
     */
    /* if (p->has_default)
        psize += my_size_binary_field(p->my_default);*/

    if (grow_write_buffer(c, c->towrite + psize) == -1) {
        return -1;
    }

    c->towrite += psize;

    int3store(&c->wbuf[base], psize - 4);
    base += 3;
    int1store(&c->wbuf[base], c->packet_seq);
    base++;

    my_write_binary_field(&c->wbuf[base], &base, p->catalog_len);
    memcpy(&c->wbuf[base], p->catalog, p->catalog_len);
    base += p->catalog_len;

    my_write_binary_field(&c->wbuf[base], &base, p->db_len);
    memcpy(&c->wbuf[base], p->db, p->db_len);
    base += p->db_len;

    my_write_binary_field(&c->wbuf[base], &base, p->table_len);
    memcpy(&c->wbuf[base], p->table, p->table_len);
    base += p->table_len;

    my_write_binary_field(&c->wbuf[base], &base, p->org_table_len);
    memcpy(&c->wbuf[base], p->org_table, p->org_table_len);
    base += p->org_table_len;

    my_write_binary_field(&c->wbuf[base], &base, p->name_len);
    memcpy(&c->wbuf[base], p->name, p->name_len);
    base += p->name_len;

    my_write_binary_field(&c->wbuf[base], &base, p->org_name_len);
    memcpy(&c->wbuf[base], p->org_name, p->org_name_len);
    base += p->org_name_len;

    /* Filler. Size of rest of data.
     * FIXME: look if this is used in mysql. */
    c->wbuf[base] = 12;
    base++;

    int2store(&c->wbuf[base], p->charsetnr);
    base += 2;

    int4store(&c->wbuf[base], p->length);
    base += 4;

    c->wbuf[base] = p->type;
    base++;

    int2store(&c->wbuf[base], p->flags);
    base += 2;

    c->wbuf[base] = p->decimals;
    base++;

    int2store(&c->wbuf[base], 0);
    base += 2;

    /*if (p->has_default)
        my_write_binary_field(&c->wbuf[base], &base, p->my_default);
        */

    return 0;
}

void *my_new_row_packet()
{
    my_row_packet *p;

    p = malloc( sizeof(my_row_packet) );
    if (p == NULL) {
        perror("Could not malloc()");
        return NULL;
    }
    memset(p, 0, sizeof(my_row_packet));

    p->h.ptype   = dpm_row;
    p->h.free_me = my_free_row_packet;
    p->h.to_buf  = my_wire_row_packet;

    return p;
}

/* The free routine just needs to blow up the lua ref */
static void my_free_row_packet(void *pkt)
{
    my_row_packet *p = pkt;
    luaL_unref(L, LUA_REGISTRYINDEX, p->packed_row_lref);
    free(p);
}

static void *my_consume_row_packet(conn *c)
{
    my_row_packet *p;
    int base = c->readto + 4;

    p = malloc( sizeof(my_row_packet) );
    if (p == NULL) {
        perror("Could not malloc()");
        return NULL;
    }
    memset(p, 0, sizeof(my_row_packet));

    p->h.ptype   = dpm_row;
    p->h.free_me = my_free_row_packet;
    p->h.to_buf  = my_wire_row_packet;

    /* We manage this memory with a lua string reference.
     * This should make it simpler to pass into lua later, and allows tricks
     * when packing and unpacking the rows.
     */
    lua_pushlstring(L, (const char *) &c->rbuf[base], c->packetsize - 4);
    p->packed_row_lref = luaL_ref(L, LUA_REGISTRYINDEX);

    new_obj(L, p, "dpm.row");

    return p;
}

static int my_wire_row_packet(conn *c, void *pkt)
{
    my_row_packet *p = pkt;
    int base         = c->towrite;

    int psize   = 4;
    size_t len  = 0;
    const char *rdata;

    lua_rawgeti(L, LUA_REGISTRYINDEX, p->packed_row_lref);
    rdata  = lua_tolstring(L, -1, &len);
    psize += len;

    if (grow_write_buffer(c, c->towrite + psize) == -1)
        return -1;

    c->towrite += psize;

    int3store(&c->wbuf[base], psize - 4);
    base += 3;
    int1store(&c->wbuf[base], c->packet_seq);
    base++;

    memcpy(&c->wbuf[base], rdata, len);

    lua_pop(L, 1);

    return 0;
}

static int my_wire_eof_packet(conn *c, void *pkt)
{
    my_eof_packet *p = pkt;
    int base = c->towrite;
    
    int psize = 9; /* Packet is a static length. */

    if (grow_write_buffer(c, c->towrite + psize) == -1) {
        return -1;
    }

    c->towrite += psize;

    int3store(&c->wbuf[base], psize - 4);
    base += 3;
    int1store(&c->wbuf[base], c->packet_seq);
    base++;

    c->wbuf[base] = 254; /* This + len signifies eof packet. */
    base++;

    int2store(&c->wbuf[base], p->warning_count);
    base += 2;

    int2store(&c->wbuf[base], p->server_status);

    return 0;
}

/* FIXME: Where do warnings come in, and how? */
static void *my_consume_eof_packet(conn *c)
{
    my_eof_packet *p;
    int base = c->readto + 4;
 
    /* Clear out the struct. */
    p = malloc( sizeof(my_eof_packet) );
    if (p == NULL) {
        perror("Could not malloc()");
        return NULL;
    }
    memset(p, 0, sizeof(my_eof_packet));

    p->h.ptype   = dpm_eof;
    p->h.free_me = my_free_eof_packet;
    p->h.to_buf  = my_wire_eof_packet;

    /* Skip field_count, is always 0xFE */
    base++;

    p->warning_count = uint2korr(&c->rbuf[base]);
    base += 2;

    p->server_status= uint2korr(&c->rbuf[base]);
    base += 2;

    new_obj(L, p, "dpm.eof");

    return p;
}

void *my_new_eof_packet()
{
    my_eof_packet *p;

    p = malloc( sizeof(my_eof_packet) );
    if (p == NULL) {
        perror("Could not malloc()");
        return NULL;
    }
    memset(p, 0, sizeof(my_eof_packet));

    p->h.ptype   = dpm_eof;
    p->h.free_me = my_free_eof_packet;
    p->h.to_buf  = my_wire_eof_packet;

    return p;
}

static void my_free_eof_packet(void *pkt)
{
    my_eof_packet *p = pkt;
    free(p);
}

/* Can't send a packet unless we know what it is.
 * So *p and ptype must be defined.
 */
 /* NOTE: This means the packet was sent _TO_ the wire on this conn */
static int sent_packet(conn *c, void **p, int ptype, int field_count)
{
    int ret = 0;

    #ifdef DBUG
    fprintf(stdout, "TX START State: [%llu] %s\n", (unsigned long long) c->id, my_state_name[c->dpmstate]);
    #endif
    /* This might be overridden during processing, so increase it up here */
    c->packet_seq++;

    switch (c->my_type) {
    case MY_CLIENT:
        /* Doesn't matter what we send to the client right now.
         * The clients maintain their own state based on what command they
         * just sent. We can add state tracking in the future so you can write
         * clients from lua without going crazy and pulling out all your hair.
         */
        switch (c->dpmstate) {
        case MYC_SENT_CMD:
            c->dpmstate = MYC_WAITING; /* FIXME: Should be reading results */
            break;
        case MYC_WAIT_HANDSHAKE:
            assert(ptype == dpm_handshake);
            c->dpmstate = MYC_WAIT_AUTH;
        }
        break;
    case MY_SERVER:
        switch (c->dpmstate) {
        case MYS_WAIT_AUTH:
            assert(ptype == dpm_auth);
            c->dpmstate = MYS_SENDING_OK;
            break;
        case MYS_RECV_ERR:
        case MYS_WAIT_CMD:
            assert(ptype == dpm_cmd);
            {
            my_cmd_packet *cmd = (my_cmd_packet *)*p;
            c->last_cmd = cmd->command;
            c->dpmstate = MYS_GOT_CMD;
            }
            break;
        }
    }

    #ifdef DBUG
    fprintf(stdout, "TX END State: [%llu] %s\n", (unsigned long long) c->id, my_state_name[c->dpmstate]);
    #endif
    if (CALLBACK_AVAILABLE(c)) {
        run_lua_callback(c, 0);
    }
    return ret;
}

/* If we received a packet, we don't necessarily know what it is.
 * So *p can be NULL and ptype can be 0 (dpm_unknown).
 */
 /* NOTE: This means the packet was received _ON_ the wire for this conn */
static int received_packet(conn *c, void **p, int *ptype, int field_count)
{
    int nargs = 0;
    pkt_func consumer = NULL;
    #ifdef DBUG
    fprintf(stdout, "RX START State: [%llu] %s\n", (unsigned long long) c->id, my_state_name[c->dpmstate]);
    #endif

    /* Default *p to NULL */
    *p = NULL;

    /* Increase the packet sequence. We might manually adjust it later. */
    c->packet_seq++;

    switch (c->my_type) {
    case MY_CLIENT:
        switch (c->dpmstate) {
        case MYC_WAIT_AUTH:
            consumer = my_consume_auth_packet;
            *ptype = dpm_auth;
            c->dpmstate = MYC_WAITING;
            break;
        case MYC_WAITING:
            /* command packets must always be consumed. */
            *p = my_consume_cmd_packet(c);
            *ptype = dpm_cmd;
            c->dpmstate = MYC_SENT_CMD;
            /* Kick off the packet sequencer. */
            c->packet_seq = 1;
            nargs++;
            break;
        }
        break;
    case MY_SERVER:
        /* These are transition markers. The last of 'blah' was sent, so
         * start parsing something else.
         */
        switch (c->dpmstate) {
            case MYS_SENT_RSET:
                c->dpmstate = MYS_SENDING_FIELDS;
                break;
            case MYS_SENT_FIELDS:
                c->dpmstate = MYS_SENDING_ROWS;
                break;
        }

        /* If we were just sent a command, flip the state depending on the
         * command sent.
         */
        if (c->dpmstate == MYS_GOT_CMD) {
            switch (c->last_cmd) {
            case COM_QUERY:
                c->dpmstate = MYS_SENDING_RSET;
                break;
            case COM_FIELD_LIST:
                c->dpmstate = MYS_SENDING_FIELDS;
                break;
            case COM_INIT_DB:
            case COM_QUIT:
                c->dpmstate = MYS_SENDING_OK;
                break;
            case COM_STATISTICS:
                c->dpmstate = MYS_SENDING_STATS;
                break;
            default:
                fprintf(stdout, "***WARNING*** UNKNOWN PACKET RESULT SET FOR PACKET TYPE %d\n", c->last_cmd);
                assert(1 == 0);
            }
        }

        /* Primary packet consumption. */
        switch (c->dpmstate) {
        case MYS_CONNECT:
            consumer = my_consume_handshake_packet;
            *ptype = dpm_handshake;
            c->dpmstate = MYS_WAIT_AUTH;
            break;
        case MYS_SENDING_OK:
            switch (field_count) {
            case 0:
                consumer = my_consume_ok_packet;
                *ptype = dpm_ok;
                c->dpmstate = MYS_WAIT_CMD;
                break;
            case 255:
                *ptype = dpm_err;
                break;
            default:
                /* Should never get here. */
                assert(field_count == 0 || field_count == 255);
            }
            break;
        case MYS_SENDING_RSET:
            switch (field_count) {
            case 0:
                consumer = my_consume_ok_packet;
                *ptype = dpm_ok;
                c->dpmstate = MYS_WAIT_CMD;
                break;
            case 255:
                *ptype = dpm_err;
                break;
            default:
                consumer = my_consume_rset_packet;
                *ptype = dpm_rset;
                c->dpmstate = MYS_SENT_RSET;
            }
            break;
        case MYS_SENDING_FIELDS:
            switch (field_count) {
            case 254:
                /* Grr. impossible to tell an EOF apart from a ROW or FIELD
                 * unless it's the right size to be an EOF as well */
                if (c->packetsize < 10) {
                    consumer = my_consume_eof_packet;
                    *ptype = dpm_eof;
                    /* Can change this to another switch, or cuddle a flag under
                     * case 'MYS_WAIT_CMD', if it's really more complex.
                     */
                    if (c->last_cmd == COM_QUERY) {
                        c->dpmstate = MYS_SENT_FIELDS;
                    } else {
                        c->dpmstate = MYS_WAIT_CMD;
                    }
                break;
                }
            case 255:
                *ptype = dpm_err;
                break;
            default:
                consumer = my_consume_field_packet;
                *ptype = dpm_field;
            }
            break;
        case MYS_SENDING_STATS:
            /* Stats packet is obscure. There's no way to get an error from it
             * so you might as well just parse the stupid thing.
             */
            /* FIXME: consumer = my_consume_stats_packet; */
            *ptype = dpm_stats;
            c->dpmstate = MYS_WAIT_CMD;
            break;
        case MYS_SENDING_ROWS:
            switch (field_count) {
            case 254:
                if (c->packetsize < 10) {
                consumer = my_consume_eof_packet;
                *ptype = dpm_eof;
                c->dpmstate = MYS_WAIT_CMD;
                break;
                }
            case 255:
                *ptype = dpm_err;
                break;
            default:
                consumer = my_consume_row_packet;
                *ptype = dpm_row;
                break;
            }
            break;
        case MYS_WAIT_CMD:
            /* Should never get here! Server must have a command when sending
             * results!
             * NOTE: Sometimes _does_ get here if we're sending multiple
             * commands down the pipe at once. Lets leave this open for now.
             */
            /*assert(1 == 0);*/
            break;
        }

        /* Read errors if we detected an error packet. */
        if (*ptype == dpm_err) {
            consumer = my_consume_err_packet;
            c->packet_seq = 0;
            c->dpmstate = MYS_RECV_ERR;
        }

        if (c->dpmstate == MYS_WAIT_CMD) {
            c->packet_seq = 0;
        }
    }

    if (consumer && CALLBACK_AVAILABLE(c)) {
        *p = consumer(c);
        nargs++;
    }

    #ifdef DBUG
    fprintf(stdout, "RX END State: [%llu] %s\n", (unsigned long long) c->id, my_state_name[c->dpmstate]);
    #endif
    return nargs;
}

/* Run the "MySQL" protocol on a socket. Generic state machine logic.
 * Would've loved to use Ragel, but it doesn't make sense here.
 */
static int run_protocol(conn *c, int read, int written)
{
    int err = 0;
    int next_packet;
    socklen_t errsize = sizeof(err);
    conn *remote = NULL;

    switch (c->mystate) {
    case my_connect:
        /* Socket was connecting. Lets see if it's good now. */
        if (getsockopt(c->fd, SOL_SOCKET, SO_ERROR, &err, &errsize) < 0) {
            perror("Running getsockopt on outbound connect");
            return -1;
        }
        if (err != 0) {
            if (verbose)
                fprintf(stderr, "Error in connecting outbound socket\n");
            return -1;
        }

        /* Neat. we're all good. */
        if (verbose)
            fprintf(stdout, "Successfully connected outbound socket %d\n", c->fd);
        update_conn_event(c, EV_READ | EV_PERSIST);
        c->mystate  = my_reading;
        c->dpmstate = MYS_CONNECT;
    case my_reading:
        /* If we've read the full packet size, we can write it to the
         * other guy
         * FIXME: Making assumptions about remote, duh :P
         */

        while ( (next_packet = my_next_packet_start(c)) != -1 ) {
            int ptype = dpm_none;
            void *p = NULL;
            int ret = 0;
            int cbret = 0;

            #ifdef DBUG
            fprintf(stdout, "Read from %llu packet size %u.\n", (unsigned long long) c->id, c->packetsize);
            #endif

            /* Drive the packet state machine. */
            ret = received_packet(c, &p, &ptype, c->rbuf[c->readto + 4]);

            /* Once all 'received packets' return a type, we can sanity
             * check that a pointer was returned. */
            /* if (p == NULL) return -1; */

            if (CALLBACK_AVAILABLE(c)) {
                cbret = run_lua_callback(c, ret);
            }

            /* Handle writing to a remote if one exists */
            if ( c->remote && ( cbret == DPM_OK || cbret == DPM_FLUSH_DISCONNECT ) ) {
                remote = (conn *)c->remote;
                if (grow_write_buffer(remote, remote->towrite + c->packetsize) == -1) {
                    return -1;
                }

                /* Drive other half of state machine. */
                ret = sent_packet(remote, &p, ptype, c->field_count);
                /* TODO: at this point we could decide not to send a
                 * packet. worth investigating?
                 */
                memcpy(remote->wbuf + remote->towrite, c->rbuf + next_packet, c->packetsize);
                /* We track our own sequence, so overwrite what's there. */
                int1store(&remote->wbuf[remote->towrite + 3], remote->packet_seq - 1);
                remote->towrite += c->packetsize;
                _dpm_add_to_flush_list(remote);
            }

            /* Flush (above) and disconnect the conns */
            if (remote && cbret == DPM_FLUSH_DISCONNECT) {
                remote->remote = NULL;
                c->remote      = NULL;
            }

            /* Copied in the packet; advance to next packet. */
            c->readto += c->packetsize;
        }
        if (c == NULL)
            break;

        /* Reuse the remote pointer and flip through the list of connections
         * to flush. */
        while (dpm_conn_flush_list) {
            handle_write(dpm_conn_flush_list);
            remote = (conn *)dpm_conn_flush_list->nextconn;
            dpm_conn_flush_list->nextconn = NULL;
            dpm_conn_flush_list = (conn *)remote;
        }
        dpm_conn_flush_list = NULL;

        /* Any pending packet reads? If none, reset boofer. */
        if (c->readto == c->read) {
            c->read    = 0;
            c->readto  = 0;
        }
        break;
    }

    return 0;
}

/* Take present state value and attempt a lua callback.
 * callbacks[conn->id][statename]->() in lua's own terms.
 * if there is a "wait for state" value named, short circuit unless that state
 * is matched.
 */
static int run_lua_callback(conn *c, int nargs)
{
    int ret = 0;
    int cb;

    #ifdef DBUG
    fprintf(stdout, "Running callback [%s] on conn id %llu\n", my_state_name[c->dpmstate], (unsigned long long) c->id);
    #endif

    /* If there's a package callback use it, else what's set for the conn. */
    /* Always handle MY_CLOSING from the connection level. */
    if (c->dpmstate == MY_CLOSING && c->main_callback[MY_CLOSING]) {
        cb = c->main_callback[MY_CLOSING];
    } else {
        cb = CALLBACK_AVAILABLE(c);
    }

    /* Short circuit if there's no callback. Fast! */
    if (cb == 0) {
        lua_settop(L, 0);
        return 0;
    }

    /* The conn id is always the last argument (duh, should it be the first?) */
    lua_pushinteger(L, c->id);
    nargs++;

    lua_rawgeti(L, LUA_REGISTRYINDEX, cb);

    /* Now the top o' the stack ought to be a function. */
    if (!lua_isfunction(L, -1)) {
        fprintf(stderr, "ERROR: Callback is null or not a function!\n");
        lua_settop(L, 0);
        return 0;
    }

    lua_insert(L, 1);

    /* Finally, call the function? Push some args too */
    if (lua_pcall(L, nargs, 1, 0) != 0) {
        fprintf(stderr, "ERROR: running callback '%s': %s\n", my_state_name[c->dpmstate], lua_tostring(L, -1));
        lua_pop(L, -1);
    }

    if (lua_isnumber(L, -1)) {
        ret = (int) lua_tointeger(L, -1);
        lua_pop(L, 1);
    } else {
        /* nil gets returned, since we expect one value. */
        lua_pop(L, 1);
        ret = DPM_OK; /* Default to an R_OK response. */
    }

    return ret;
}

/* LUA command to kick off a close of a conn object.
 * Will spew error if conn is already closed.
 */
static int close_conn(lua_State *L)
{
    conn **c = luaL_checkudata(L, 1, "dpm.conn");
    lua_pop(L, 1);

    if ((*c)->alive == 0) {
        lua_pushnil(L);
    } else {
        handle_close(*c);
        lua_pushinteger(L, 1);
    }

    return 1;
}

/* LUA command for verifying a password hash.
 * Takes: Auth packet, handshake packet, password hash (sha1(sha1(plaintext)))
 * returns 0 if they match up.
 */
static int check_pass(lua_State *L)
{
    my_auth_packet **auth = (my_auth_packet **)luaL_checkudata(L, 1, "dpm.auth");
    my_handshake_packet **hs = (my_handshake_packet **)luaL_checkudata(L, 2, "dpm.handshake");
    const char *stored_pass = luaL_checkstring(L, 3);

    lua_pushinteger(L, my_check_scramble((*auth)->scramble_buff, (*hs)->scramble_buff, stored_pass));

    return 1;
}

/* LUA command for encrypting a password for client->server auth.
 * Takes: Auth packet to write scramble into, handshake packet with random
 * seed, plaintext password to scramble.
 * returns nothing.
 */
static int crypt_pass(lua_State *L)
{
    my_auth_packet **auth = (my_auth_packet **)luaL_checkudata(L, 1, "dpm.auth");
    my_handshake_packet **hs = (my_handshake_packet **)luaL_checkudata(L, 2, "dpm.handshake");
    const char *plain_pass = luaL_checkstring(L, 3);

    /* Encrypt the password into the authentication packet. */
    my_scramble((*auth)->scramble_buff, (*hs)->scramble_buff, plain_pass);

    return 0;
}

/* LUA command for attaching a client with a backend. */
static int proxy_connect(lua_State *L)
{
    conn **c = (conn **)luaL_checkudata(L, 1, "dpm.conn");
    conn **r = (conn **)luaL_checkudata(L, 2, "dpm.conn");

    if ((*c)->my_type != MY_CLIENT || (*c)->alive == 0) {
        luaL_error(L, "Arg 1 must be a valid client");
    }
    if ((*r)->my_type != MY_SERVER || (*c)->alive == 0) {
        luaL_error(L, "Arg 2 must be a valid backend");
    }

    (*c)->remote    = (struct conn *)*r;
    (*c)->remote_id = (*r)->id;
    (*r)->remote    = (struct conn *)*c;
    (*r)->remote_id = (*c)->id;

    return 0;
}

/* LUA command for detaching a client and backend. */
static int proxy_disconnect(lua_State *L)
{
    conn **c = (conn **)luaL_checkudata(L, 1, "dpm.conn");
    conn *r = NULL;

    if (!(*c)->remote) {
        luaL_error(L, "Must specify a connected client/server to disconnect.");
    }

    r = (conn *) (*c)->remote;

    r->remote       = NULL;
    r->remote_id    = 0;
    (*c)->remote    = NULL;
    (*c)->remote_id = 0;

    return 0;
}

/* We provide three timer functions. One mimics gettimeofday and returns time,
 * microtime separately. Returns seconds, microseconds.
 * FIXME: Is pushinteger good enough? pushnumber uses double...
 */
static int dpm_gettimeofday(lua_State *L)
{
    struct timeval t;
    gettimeofday(&t, NULL);
    lua_pushinteger(L, t.tv_sec);
    lua_pushinteger(L, t.tv_usec);
    return 2;
}

/* ... returns seconds. */
static int dpm_time(lua_State *L)
{
    struct timeval t;
    gettimeofday(&t, NULL);
    lua_pushinteger(L, t.tv_sec);
    return 1;
}

/* Returns the current time in milliseconds. */
static int dpm_time_hires(lua_State *L)
{
    struct timeval t;
    gettimeofday(&t, NULL);
    lua_pushinteger(L, (t.tv_usec / 1000) + (t.tv_sec * 1000) );
    return 1;
}

/* LUA command for wiring a packet into a connection. */
static int wire_packet(lua_State *L)
{
    conn **c = luaL_checkudata(L, 1, "dpm.conn");
    my_packet_fuzz **p;

    if (!(*c)->alive)
        luaL_error(L, "Cannot write to invalid connection");

    luaL_checktype(L, 2, LUA_TUSERDATA);

    p = lua_touserdata(L, 2);

    (*p)->h.to_buf(*c, *p);

    /* Link up connections which will need buffers flushed. */
    _dpm_add_to_flush_list(*c);

    if (verbose)
        fprintf(stdout, "Wrote packet of type [%d] to sock [%llu] with server type [%d]\n", (*p)->h.ptype, (unsigned long long)(*c)->id, (*c)->my_type);

    /* FIXME: sent_packet doesn't need the field count at all? */
    lua_settop(L, 0);
    sent_packet(*c, (void **) p, (*p)->h.ptype, 0);

    return 0;
}

static void _init_new_connect(int outsock)
{
    conn *c;

    c = init_conn(outsock);

    /* Special state for outbound requests. */
    c->mystate = my_connect;
    c->my_type = MY_SERVER;
    c->alive++;

    /* We watch for a write to this guy to see if it succeeds */
    add_conn_event(c, EV_WRITE);

    new_obj(L, c, "dpm.conn");

    return;
}

/* Outbound connection function for unix sockets. */
static int new_connect_unix(lua_State *L)
{
    int outsock;
    struct sockaddr_un dest_addr;
    int flags = 1;
    const char *dpath = luaL_checkstring(L, 1);

    outsock = socket(AF_UNIX, SOCK_STREAM, 0); /* check errors */
    set_sock_nonblock(outsock); /* check errors */
    setsockopt(outsock, IPPROTO_TCP, TCP_NODELAY, (void *)&flags, sizeof(flags));

    memset(&dest_addr, 0, sizeof(dest_addr));
    dest_addr.sun_family = AF_UNIX;
    strncpy(dest_addr.sun_path, dpath, 100); /* FIXME: Is UNIX_PATH_MAX portable? */

    /* Lets try a nonblocking connect... */
    if (connect(outsock, (const struct sockaddr *)&dest_addr, sizeof(dest_addr)) == -1) {
        if (errno != EINPROGRESS) {
            close(outsock);
            lua_pushnil(L);
            return 1;
        }
    }

    _init_new_connect(outsock);

    return 1;
}

/* Outbound connection function */
static int new_connect(lua_State *L)
{
    int outsock;
    struct sockaddr_in dest_addr;
    int flags = 1;
    const char *ip_addr = luaL_checkstring(L, 1);
    int port_num     = (int)luaL_checkinteger(L, 2);

    outsock = socket(AF_INET, SOCK_STREAM, 0); /* check errors */

    dest_addr.sin_family = AF_INET;
    dest_addr.sin_port = htons(port_num);
    dest_addr.sin_addr.s_addr = inet_addr(ip_addr);

    set_sock_nonblock(outsock); /* check errors */

    memset(&(dest_addr.sin_zero), '\0', 8);

    setsockopt(outsock, IPPROTO_TCP, TCP_NODELAY, (void *)&flags, sizeof(flags));

    /* Lets try a nonblocking connect... */
    if (connect(outsock, (const struct sockaddr *)&dest_addr, sizeof(dest_addr)) == -1) {
        if (errno != EINPROGRESS) {
            close(outsock);
            lua_pushnil(L);
            return 1;
        }
    }

    _init_new_connect(outsock);

    return 1;
}

static void _init_new_listener(int l_socket, int type)
{
    conn *listener = init_conn(l_socket);

    listener->ev_flags = EV_READ | EV_PERSIST;

    listener->listener = type;
    listener->alive++;

    event_set(&listener->ev, l_socket, listener->ev_flags, handle_event, (void *)listener);
    event_add(&listener->ev, NULL);

    new_obj(L, listener, "dpm.conn");

    return;
}

/* First arg is path, second arg is the mask. Path is non optional. */
static int new_listener_unix(lua_State *L)
{
    struct sockaddr_un addr;
    int flags = 1;
    int l_socket = 0;
    int mask = 0700;
    struct stat mstat;
    int prev_mask;
    const char *spath = luaL_checkstring(L, 1);

    if (!lua_isnoneornil(L, 2))
        mask = strtol(luaL_checkstring(L, 2), NULL, 8);

    if ( (l_socket = socket(AF_UNIX, SOCK_STREAM, 0)) == -1) {
        perror("unix socket");
        return -1;
    }
    set_sock_nonblock(l_socket);
    setsockopt(l_socket, IPPROTO_TCP, TCP_NODELAY, (void *)&flags, sizeof(flags));
    setsockopt(l_socket, SOL_SOCKET, SO_REUSEADDR, (void *)&flags, sizeof(flags));
    
    memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    strncpy(addr.sun_path, spath, 100); /* FIXME: Is UNIX_PATH_MAX portable? */
    prev_mask = umask( ~(mask & 0777));

    if (lstat(spath, &mstat) == 0 && S_ISSOCK(mstat.st_mode))
        unlink(spath);

    if (bind(l_socket, (const struct sockaddr *)&addr, sizeof(addr)) == -1) {
        perror("binding server socket");
        close(l_socket);
        umask(prev_mask);
        return -1;
    }

    umask(prev_mask);
    if (listen(l_socket, 1024) == -1) {
        perror("setting listen on server socket");
        close(l_socket);
        return -1;
    }

    _init_new_listener(l_socket, DPM_UNIX);

    return 1;
}

static int new_listener(lua_State *L)
{
    struct sockaddr_in addr;
    int flags = 1;
    int l_socket = 0;
    const char *ip_addr;
    int port_num = (int)luaL_checkinteger(L, 2);

    /* If nil is passed as the first argument, default to INADDR_ANY */
    if (lua_isnil(L, 1)) {
        ip_addr = NULL;
    } else {
        ip_addr = luaL_checkstring(L, 1);
    }

    if ( (l_socket = socket(AF_INET, SOCK_STREAM, 0)) == -1) {
        perror("socket");
        return -1;
    }

    set_sock_nonblock(l_socket);

    setsockopt(l_socket, SOL_SOCKET, SO_REUSEADDR, (void *)&flags, sizeof(flags));
    setsockopt(l_socket, IPPROTO_TCP, TCP_NODELAY, (void *)&flags, sizeof(flags));
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port_num);

    if (ip_addr) {
        addr.sin_addr.s_addr = inet_addr(ip_addr);
    } else {
        addr.sin_addr.s_addr = INADDR_ANY;
    }

    if (bind(l_socket, (const struct sockaddr *)&addr, sizeof(addr)) == -1) {
        perror("binding server socket");
        close(l_socket);
        return -1;
    }

    if (listen(l_socket, 1024) == -1) {
        perror("setting listen on server socket");
        close(l_socket);
        return -1;
    }

    _init_new_listener(l_socket, DPM_TCP);

    return 1;
}

#ifndef DPMLIBDIR
    #define DPMLIBDIR "."
#endif

int main (int argc, char **argv)
{
    struct sigaction sa;
    static const struct luaL_Reg dpm [] = {
        {"listener", new_listener},
        {"listener_unix", new_listener_unix},
        {"connect", new_connect},
        {"connect_unix", new_connect_unix},
        {"close", close_conn},
        {"wire_packet", wire_packet},
        {"check_pass", check_pass},
        {"crypt_pass", crypt_pass},
        {"proxy_connect", proxy_connect},
        {"proxy_disconnect", proxy_disconnect},
        {"gettimeofday", dpm_gettimeofday},
        {"time", dpm_time},
        {"time_hires", dpm_time_hires},
        {NULL, NULL},
    };
    /* Argument parsing helper. */
    int c;
    char *startfile = DPMLIBDIR "/lua/startup.lua";
    static struct option l_options[] = {
        {"startfile", 1, 0, 's'},
        {"verbose", 2, 0, 'v'},
        {"help", 3, 0, 'h'},
        {0, 0, 0, 0},
    };

    /* Init /dev/urandom socket... */
    if( (urandom_sock = open("/dev/urandom", O_RDONLY)) == -1 ) {
        perror("Opening /dev/urandom");
        return -1;
    }

    /* I thought there was a non-environment method of changing the path, but
     * this is it. The documentation says ';;' is replaced by the default
     * paths. */
    if ( !getenv("LUA_PATH") && -1 == setenv("LUA_PATH",
        ";;lua/?.lua;lua/lib/?.lua;" DPMLIBDIR "/lua/?.lua;" DPMLIBDIR "/lua/lib/?.lua", 1) ) {
        perror("Could not configure paths for DPM's lua libraries");
        return -1;
    }

    /* Initialize the event system. */
    event_init();

    /* Lets ignore SIGPIPE... sorry, just about yanking this from memcached.
     * I tried to use the manpages but it came out exactly the same :P
     */

    sa.sa_handler = SIG_IGN;
    sa.sa_flags   = 0;
    if (sigemptyset(&sa.sa_mask) == -1 || sigaction(SIGPIPE, &sa, 0) == -1) {
        perror("Could not ignore SIGPIPE: sigaction");
        return -1;
    }

    sa.sa_handler = sig_hup;
    if (sigaction(SIGHUP, &sa, 0) == -1) {
        perror("Could not set SIGHUP handler: sigaction");
        return -1;
    }

    L = lua_open();

    if (L == NULL) {
        fprintf(stderr, "Could not create lua state\n");
        return -1;
    }
    luaL_openlibs(L);

    luaL_register(L, "dpm", dpm);
    register_obj_types(L); /* Internal call to fill all custom metatables */

    /* Time to do argument parsing! */
    while ( (c = getopt_long(argc, argv, "s:v:h", l_options, NULL) ) != -1) {
        switch (c) {
        case 's':
            startfile = optarg;
            break;
        case 'v':
            if (optarg) {
                verbose = atoi(optarg);
            } else {
                verbose++;
            }
            break;
        default:
            printf("Dormando's Proxy for MySQL release " VERSION "\n");
            printf("Usage: --startfile startupfile.lua (default 'startup.lua')\n"
                   "       --verbose [num] (increase verbosity)\n");
            return -1;
        }
    }

    if (luaL_dofile(L, startfile)) {
        fprintf(stdout, "Could not run lua initializer: %s\n", lua_tostring(L, -1));
        lua_pop(L, 1);
        return -1;
    }

    if (verbose)
        fprintf(stdout, "Starting event dispatcher...\n");

    event_dispatch();

    return 0;
}
