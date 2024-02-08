/**
* dht.c defines a Distributed Hash Table (DHT) implementation using a ring-based structure where each node is responsible for a range of IDs.
*/



#include "dht.h"

#include <assert.h>
#include <limits.h>
#include <math.h>
#include <string.h>
#include <stdbool.h>
#include <stdio.h>
#include <sys/types.h>
#include <time.h>

#include <openssl/sha.h>

#define LOOKUP_CACHE_ENTRIES 30
#define LOOKUP_CACHE_VALIDIY_MS 2000


struct peer predecessor; 
struct peer self;
struct peer successor;
struct peer anchor;
int dht_socket;


/**
 * Table for the most recent lookup replies.
 */
struct {
    unsigned long entry;
    dht_id predecessor;
    struct peer peer;
} lookup_cache[LOOKUP_CACHE_ENTRIES];


/**
 * Return the current time in milliseconds
 */
unsigned long time_ms(void) {
    struct timespec spec;
    clock_gettime(CLOCK_REALTIME, &spec);
    return 1000 * spec.tv_sec + round(spec.tv_nsec / 1.0e6);
}


/**
 * Deserialize a DHT message received from the network
 */
static void dht_deserialize(struct dht_message* msg) {
    msg->hash = ntohs(msg->hash);
    msg->peer.id = ntohs(msg->peer.id);
    msg->peer.ip.s_addr = ntohl(msg->peer.ip.s_addr);
    msg->peer.port = ntohs(msg->peer.port);
}


/**
 * Serialize a DHT message for transmission via the network
 */
static void dht_serialize(struct dht_message* msg) {
    dht_deserialize(msg);  // Serialization is self-inverse
}


/**
 * Check whether a cache entry is outdated
 */
static bool outdated(unsigned long entry) {
    return (time_ms() - entry) >= LOOKUP_CACHE_VALIDIY_MS;
}


/**
 *  Compare two peers for equality
 */
static bool peer_cmp(const struct peer* a, const struct peer* b) {
    return a && b && (memcmp(a, b, sizeof(struct peer)) == 0);
}


/**
 * Send the given DHT message to the given peer
 */
static void dht_send(struct dht_message* msg, const struct peer* peer) {
    dht_serialize(msg);

    struct sockaddr_in addr;
    peer_to_sockaddr(peer, &addr);

    if (sendto(dht_socket, msg, sizeof(struct dht_message), 0, (struct sockaddr*) &addr, sizeof(struct sockaddr_in)) == -1) {
        perror("sendto");
        exit(1);
    }
}

void send_join(const struct peer peer){
    struct dht_message join = {
            .flags = JOIN,
            .hash = 0,
            .peer = self,
    };
    dht_send(&join, &peer);
}


/**
 * Process the given lookup
 *
 * If our successor is responsible for the requested ID, a reply is sent to the
 * originator. Otherwise, the message is forwarded to our successor.
 */
static void process_lookup(struct dht_message* lookup) {
    if (!peer_cmp(&successor, dht_responsible(lookup->hash))) {
        dht_send(lookup, &successor);
        return;
    }

    struct dht_message reply = {
        .flags = REPLY,
        .hash = self.id,
        .peer = successor,
    };
    dht_send(&reply, &(lookup->peer));
}


/**
* Process the given reply
*
* The information about the peer is entered into the the `lookup_cache`,
* replacing a previous entry for the same hash, the first empty entry, or the
* first outdated one, in this order.
*/
static void process_reply(const struct dht_message* reply) {
    // Try to replace existing value
    for (size_t i = 0; i < LOOKUP_CACHE_ENTRIES; i += 1) {
        const bool peer_match = peer_cmp(&lookup_cache[i].peer, &reply->peer);
        const bool more_recent = lookup_cache[i].entry < time_ms();

        if (peer_match && more_recent) {
            lookup_cache[i].entry = time_ms();
            lookup_cache[i].predecessor = reply->hash;
            return;
        }
    }

    unsigned long oldest_time = ULONG_MAX;
    size_t oldest_idx = 0;
    for (size_t i = 0; i < LOOKUP_CACHE_ENTRIES; i += 1) {
        if (lookup_cache[i].entry < oldest_time) {
            oldest_idx = i;
        }
    }

    // Since the table is zero-initialized, empty values are implicitly the
    // oldest ones. Moreover, any outdated value is older than any non-outdated
    // one, so no explicit check is required.
    lookup_cache[oldest_idx].entry = time_ms();
    lookup_cache[oldest_idx].predecessor = reply->hash;
    lookup_cache[oldest_idx].peer = reply->peer;
}


/**
 * Process an incoming DHT message
 */
static void dht_process_message(struct dht_message* msg) {
    if (msg->flags == LOOKUP) {
        process_lookup(msg);
    } else if (msg->flags == REPLY) {
        process_reply(msg);
    } else {
        printf("Received invalid DHT Message\n");
    }
}


/**
 * Receive a DHT message from the `dht_socket`
 */
static ssize_t dht_recv(struct dht_message* msg, struct sockaddr* address, socklen_t* address_length) {
    ssize_t result = recvfrom(dht_socket, msg, sizeof(struct dht_message), 0, address, address_length);

    if (result < 0) {
        perror("recv");
        exit(EXIT_FAILURE);
    }

    dht_deserialize(msg);

    return result;
}


/**
 * Check whether the given peer is responsible for the given ID
 *
 * Note that this returning false does not imply the passed peer's predecessor is
 * responsible for the ID, this is not generally the case. 
 */
static bool is_responsible(dht_id peer_predecessor, dht_id peer, dht_id id) {
    // Gotta store differences explicitly as unsigned since C promotes them to signed otherwise...
    const dht_id distance_peer_predecessor = peer_predecessor - id;
    const dht_id distance_peer = peer - id;
    return (peer_predecessor == peer) || (distance_peer < distance_peer_predecessor);
}


dht_id hash(const string str) {
    uint8_t digest[SHA256_DIGEST_LENGTH];
    SHA256((uint8_t*) str, strlen(str), digest);
    return htons(*((dht_id*) digest));  // We only use the first two bytes here
}


struct peer* dht_responsible(dht_id id) {
    if (is_responsible(predecessor.id, self.id, id)) {
        return &self;
    } else if (is_responsible(self.id, successor.id, id)) {
        return &successor;
    }

    // Check for recent lookup replies that match the datum
    for (size_t i = 0; i < LOOKUP_CACHE_ENTRIES; i += 1) {
        const bool match = is_responsible(lookup_cache[i].predecessor, lookup_cache[i].peer.id, id);

        if (match && !outdated(lookup_cache[i].entry)) {
            return &lookup_cache[i].peer;
        }
    }

    return NULL;
}


void peer_to_sockaddr(const struct peer* peer, struct sockaddr_in* addr) {
    addr->sin_family = AF_INET;
    addr->sin_addr.s_addr = htonl(peer->ip.s_addr);
    addr->sin_port = htons(peer->port);
}


void dht_lookup(dht_id id) {
    struct dht_message msg = {
        .flags = LOOKUP,
        .hash = id,
        .peer = self,
    };
    dht_send(&msg, &successor);
}


void dht_handle_socket(void) {
    struct sockaddr address = {0};
    socklen_t address_length = sizeof(struct sockaddr);
    struct dht_message msg = {0};

    dht_recv(&msg, &address, &address_length);
    dht_process_message(&msg);
}
