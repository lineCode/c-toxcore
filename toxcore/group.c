/*
 * Slightly better groupchats implementation.
 */

/*
 * Copyright © 2016-2017 The TokTok team.
 * Copyright © 2014 Tox project.
 *
 * This file is part of Tox, the free peer to peer instant messenger.
 *
 * Tox is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * Tox is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with Tox.  If not, see <http://www.gnu.org/licenses/>.
 */
#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include "group.h"

#include <stdlib.h>
#include <string.h>

#include "mono_time.h"
#include "util.h"

/**
 * Packet type IDs as per the protocol specification.
 */
typedef enum Group_Message_Id {
    GROUP_MESSAGE_PING_ID        = 0,
    GROUP_MESSAGE_NEW_PEER_ID    = 16,
    GROUP_MESSAGE_KILL_PEER_ID   = 17,
    GROUP_MESSAGE_NAME_ID        = 48,
    GROUP_MESSAGE_TITLE_ID       = 49,
} Group_Message_Id;

#define GROUP_MESSAGE_NEW_PEER_LENGTH (sizeof(uint16_t) + CRYPTO_PUBLIC_KEY_SIZE * 2)
#define GROUP_MESSAGE_KILL_PEER_LENGTH (sizeof(uint16_t))

#define MAX_GROUP_MESSAGE_DATA_LEN (MAX_CRYPTO_DATA_SIZE - (1 + MIN_MESSAGE_PACKET_LEN))

typedef enum Invite_Id {
    INVITE_ID             = 0,
    INVITE_RESPONSE_ID    = 1,
} Invite_Id;

#define INVITE_PACKET_SIZE (1 + sizeof(uint16_t) + GROUP_IDENTIFIER_LENGTH)
#define INVITE_RESPONSE_PACKET_SIZE (1 + sizeof(uint16_t) * 2 + GROUP_IDENTIFIER_LENGTH)

#define ONLINE_PACKET_DATA_SIZE (sizeof(uint16_t) + GROUP_IDENTIFIER_LENGTH)

typedef enum Peer_Id {
    PEER_KILL_ID      = 1,
    PEER_QUERY_ID     = 8,
    PEER_RESPONSE_ID  = 9,
    PEER_TITLE_ID     = 10,
} Peer_Id;

#define MIN_MESSAGE_PACKET_LEN (sizeof(uint16_t) * 2 + sizeof(uint32_t) + 1)

/* return false if the groupnumber is not valid.
 * return true if the groupnumber is valid.
 */
static bool is_groupnumber_valid(const Group_Chats *g_c, uint32_t groupnumber)
{
    if (groupnumber >= g_c->num_chats) {
        return false;
    }

    if (g_c->chats == nullptr) {
        return false;
    }

    if (g_c->chats[groupnumber].status == GROUPCHAT_STATUS_NONE) {
        return false;
    }

    return true;
}


/* Set the size of the groupchat list to num.
 *
 *  return false if realloc fails.
 *  return true if it succeeds.
 */
static bool realloc_conferences(Group_Chats *g_c, uint16_t num)
{
    if (num == 0) {
        free(g_c->chats);
        g_c->chats = nullptr;
        return true;
    }

    Group_c *newgroup_chats = (Group_c *)realloc(g_c->chats, num * sizeof(Group_c));

    if (newgroup_chats == nullptr) {
        return false;
    }

    g_c->chats = newgroup_chats;
    return true;
}

static void setup_conference(Group_c *g)
{
    memset(g, 0, sizeof(Group_c));
}

/* Create a new empty groupchat connection.
 *
 * return -1 on failure.
 * return groupnumber on success.
 */
static int32_t create_group_chat(Group_Chats *g_c)
{
    for (uint16_t i = 0; i < g_c->num_chats; ++i) {
        if (g_c->chats[i].status == GROUPCHAT_STATUS_NONE) {
            return i;
        }
    }

    int32_t id = -1;

    if (realloc_conferences(g_c, g_c->num_chats + 1)) {
        id = g_c->num_chats;
        ++g_c->num_chats;
        setup_conference(&g_c->chats[id]);
    }

    return id;
}


/* Wipe a groupchat.
 *
 * return -1 on failure.
 * return 0 on success.
 */
static int wipe_group_chat(Group_Chats *g_c, uint32_t groupnumber)
{
    if (!is_groupnumber_valid(g_c, groupnumber)) {
        return -1;
    }

    uint16_t i;
    crypto_memzero(&g_c->chats[groupnumber], sizeof(Group_c));

    for (i = g_c->num_chats; i != 0; --i) {
        if (g_c->chats[i - 1].status != GROUPCHAT_STATUS_NONE) {
            break;
        }
    }

    if (g_c->num_chats != i) {
        g_c->num_chats = i;
        realloc_conferences(g_c, g_c->num_chats);
    }

    return 0;
}

static Group_c *get_group_c(const Group_Chats *g_c, uint32_t groupnumber)
{
    if (!is_groupnumber_valid(g_c, groupnumber)) {
        return nullptr;
    }

    return &g_c->chats[groupnumber];
}

/*
 * check if peer with real_pk is in peer array.
 *
 * return peer index if peer is in chat.
 * return -1 if peer is not in chat.
 *
 * TODO(irungentoo): make this more efficient.
 */

static int peer_in_chat(const Group_c *chat, const uint8_t *real_pk)
{
    for (uint32_t i = 0; i < chat->numpeers; ++i) {
        if (id_equal(chat->group[i].real_pk, real_pk)) {
            return i;
        }
    }

    return -1;
}

/*
 * check if group with identifier is in group array.
 *
 * return group number if peer is in list.
 * return -1 if group is not in list.
 *
 * TODO(irungentoo): make this more efficient and maybe use constant time comparisons?
 */
static int32_t get_group_num(const Group_Chats *g_c, const uint8_t *identifier)
{
    for (uint16_t i = 0; i < g_c->num_chats; ++i) {
        if (crypto_memcmp(g_c->chats[i].identifier, identifier, GROUP_IDENTIFIER_LENGTH) == 0) {
            return i;
        }
    }

    return -1;
}

int32_t conference_by_uid(const Group_Chats *g_c, const uint8_t *uid)
{
    for (uint16_t i = 0; i < g_c->num_chats; ++i) {
        if (crypto_memcmp(g_c->chats[i].identifier + 1, uid, GROUP_IDENTIFIER_LENGTH - 1) == 0) {
            return i;
        }
    }

    return -1;
}

/*
 * check if peer with peer_number is in peer array.
 *
 * return peer number if peer is in chat.
 * return -1 if peer is not in chat.
 *
 * TODO(irungentoo): make this more efficient.
 */
static int get_peer_index(Group_c *g, uint16_t peer_number)
{
    for (uint32_t i = 0; i < g->numpeers; ++i) {
        if (g->group[i].peer_number == peer_number) {
            return i;
        }
    }

    return -1;
}


static uint64_t calculate_comp_value(const uint8_t *pk1, const uint8_t *pk2)
{
    uint64_t cmp1 = 0, cmp2 = 0;

    for (size_t i = 0; i < sizeof(uint64_t); ++i) {
        cmp1 = (cmp1 << 8) + (uint64_t)pk1[i];
        cmp2 = (cmp2 << 8) + (uint64_t)pk2[i];
    }

    return cmp1 - cmp2;
}

typedef enum Groupchat_Closest {
    GROUPCHAT_CLOSEST_NONE,
    GROUPCHAT_CLOSEST_ADDED,
    GROUPCHAT_CLOSEST_REMOVED
} Groupchat_Closest;

static int friend_in_close(Group_c *g, int friendcon_id);
static int add_conn_to_groupchat(Group_Chats *g_c, int friendcon_id, uint32_t groupnumber, uint8_t closest,
                                 uint8_t lock);

static int add_to_closest(Group_Chats *g_c, uint32_t groupnumber, const uint8_t *real_pk, const uint8_t *temp_pk)
{
    Group_c *g = get_group_c(g_c, groupnumber);

    if (!g) {
        return -1;
    }

    if (public_key_cmp(g->real_pk, real_pk) == 0) {
        return -1;
    }

    unsigned int i;
    unsigned int index = DESIRED_CLOSE_CONNECTIONS;

    for (i = 0; i < DESIRED_CLOSE_CONNECTIONS; ++i) {
        if (g->closest_peers[i].entry && public_key_cmp(real_pk, g->closest_peers[i].real_pk) == 0) {
            return 0;
        }
    }

    for (i = 0; i < DESIRED_CLOSE_CONNECTIONS; ++i) {
        if (g->closest_peers[i].entry == 0) {
            index = i;
            break;
        }
    }

    if (index == DESIRED_CLOSE_CONNECTIONS) {
        uint64_t comp_val = calculate_comp_value(g->real_pk, real_pk);
        uint64_t comp_d = 0;

        for (i = 0; i < (DESIRED_CLOSE_CONNECTIONS / 2); ++i) {
            uint64_t comp;
            comp = calculate_comp_value(g->real_pk, g->closest_peers[i].real_pk);

            if (comp > comp_val && comp > comp_d) {
                index = i;
                comp_d = comp;
            }
        }

        comp_val = calculate_comp_value(real_pk, g->real_pk);

        for (i = (DESIRED_CLOSE_CONNECTIONS / 2); i < DESIRED_CLOSE_CONNECTIONS; ++i) {
            uint64_t comp = calculate_comp_value(g->closest_peers[i].real_pk, g->real_pk);

            if (comp > comp_val && comp > comp_d) {
                index = i;
                comp_d = comp;
            }
        }
    }

    if (index == DESIRED_CLOSE_CONNECTIONS) {
        return -1;
    }

    uint8_t old_real_pk[CRYPTO_PUBLIC_KEY_SIZE];
    uint8_t old_temp_pk[CRYPTO_PUBLIC_KEY_SIZE];
    uint8_t old = 0;

    if (g->closest_peers[index].entry) {
        memcpy(old_real_pk, g->closest_peers[index].real_pk, CRYPTO_PUBLIC_KEY_SIZE);
        memcpy(old_temp_pk, g->closest_peers[index].temp_pk, CRYPTO_PUBLIC_KEY_SIZE);
        old = 1;
    }

    g->closest_peers[index].entry = 1;
    memcpy(g->closest_peers[index].real_pk, real_pk, CRYPTO_PUBLIC_KEY_SIZE);
    memcpy(g->closest_peers[index].temp_pk, temp_pk, CRYPTO_PUBLIC_KEY_SIZE);

    if (old) {
        add_to_closest(g_c, groupnumber, old_real_pk, old_temp_pk);
    }

    if (!g->changed) {
        g->changed = GROUPCHAT_CLOSEST_ADDED;
    }

    return 0;
}

static unsigned int pk_in_closest_peers(Group_c *g, uint8_t *real_pk)
{
    unsigned int i;

    for (i = 0; i < DESIRED_CLOSE_CONNECTIONS; ++i) {
        if (!g->closest_peers[i].entry) {
            continue;
        }

        if (public_key_cmp(g->closest_peers[i].real_pk, real_pk) == 0) {
            return 1;
        }
    }

    return 0;
}

static int send_packet_online(Friend_Connections *fr_c, int friendcon_id, uint16_t group_num, uint8_t *identifier);

static int connect_to_closest(Group_Chats *g_c, uint32_t groupnumber, void *userdata)
{
    Group_c *g = get_group_c(g_c, groupnumber);

    if (!g) {
        return -1;
    }

    if (!g->changed) {
        return 0;
    }

    if (g->changed == GROUPCHAT_CLOSEST_REMOVED) {
        for (uint32_t i = 0; i < g->numpeers; ++i) {
            add_to_closest(g_c, groupnumber, g->group[i].real_pk, g->group[i].temp_pk);
        }
    }

    for (uint32_t i = 0; i < MAX_GROUP_CONNECTIONS; ++i) {
        if (g->close[i].type == GROUPCHAT_CLOSE_NONE) {
            continue;
        }

        if (!g->close[i].closest) {
            continue;
        }

        uint8_t real_pk[CRYPTO_PUBLIC_KEY_SIZE];
        uint8_t dht_temp_pk[CRYPTO_PUBLIC_KEY_SIZE];
        get_friendcon_public_keys(real_pk, dht_temp_pk, g_c->fr_c, g->close[i].number);

        if (!pk_in_closest_peers(g, real_pk)) {
            g->close[i].closest = false;

            if (!g->close[i].introducer && !g->close[i].introduced) {
                g->close[i].type = GROUPCHAT_CLOSE_NONE;
                kill_friend_connection(g_c->fr_c, g->close[i].number);
            }
        }
    }

    for (uint32_t i = 0; i < DESIRED_CLOSE_CONNECTIONS; ++i) {
        if (!g->closest_peers[i].entry) {
            continue;
        }

        int friendcon_id = getfriend_conn_id_pk(g_c->fr_c, g->closest_peers[i].real_pk);

        uint8_t lock = 1;

        if (friendcon_id == -1) {
            friendcon_id = new_friend_connection(g_c->fr_c, g->closest_peers[i].real_pk);
            lock = 0;

            if (friendcon_id == -1) {
                continue;
            }

            set_dht_temp_pk(g_c->fr_c, friendcon_id, g->closest_peers[i].temp_pk, userdata);
        }

        add_conn_to_groupchat(g_c, friendcon_id, groupnumber, 1, lock);

        if (friend_con_connected(g_c->fr_c, friendcon_id) == FRIENDCONN_STATUS_CONNECTED) {
            send_packet_online(g_c->fr_c, friendcon_id, groupnumber, g->identifier);
        }
    }

    g->changed = GROUPCHAT_CLOSEST_NONE;

    return 0;
}

/* Add a peer to the group chat.
 *
 * do_gc_callback indicates whether we want to trigger callbacks set by the client
 * via the public API. This should be set to false if this function is called
 * from outside of the tox_iterate() loop.
 *
 * return peer_index if success or peer already in chat.
 * return -1 if error.
 */
static int addpeer(Group_Chats *g_c, uint32_t groupnumber, const uint8_t *real_pk, const uint8_t *temp_pk,
                   uint16_t peer_number, void *userdata, bool do_gc_callback)
{
    Group_c *g = get_group_c(g_c, groupnumber);

    if (!g) {
        return -1;
    }

    // TODO(irungentoo):
    int peer_index = peer_in_chat(g, real_pk);

    if (peer_index != -1) {
        id_copy(g->group[peer_index].temp_pk, temp_pk);

        if (g->group[peer_index].peer_number != peer_number) {
            return -1;
        }

        return peer_index;
    }

    peer_index = get_peer_index(g, peer_number);

    if (peer_index != -1) {
        return -1;
    }

    Group_Peer *temp = (Group_Peer *)realloc(g->group, sizeof(Group_Peer) * (g->numpeers + 1));

    if (temp == nullptr) {
        return -1;
    }

    memset(&temp[g->numpeers], 0, sizeof(Group_Peer));
    g->group = temp;

    id_copy(g->group[g->numpeers].real_pk, real_pk);
    id_copy(g->group[g->numpeers].temp_pk, temp_pk);
    g->group[g->numpeers].peer_number = peer_number;

    g->group[g->numpeers].last_recv = unix_time();
    ++g->numpeers;

    add_to_closest(g_c, groupnumber, real_pk, temp_pk);

    if (do_gc_callback && g_c->peer_list_changed_callback) {
        g_c->peer_list_changed_callback(g_c->m, groupnumber, userdata);
    }

    if (g->peer_on_join) {
        g->peer_on_join(g->object, groupnumber, g->numpeers - 1);
    }

    return g->numpeers - 1;
}

static int remove_close_conn(Group_Chats *g_c, uint32_t groupnumber, int friendcon_id)
{
    Group_c *g = get_group_c(g_c, groupnumber);

    if (!g) {
        return -1;
    }

    uint32_t i;

    for (i = 0; i < MAX_GROUP_CONNECTIONS; ++i) {
        if (g->close[i].type == GROUPCHAT_CLOSE_NONE) {
            continue;
        }

        if (g->close[i].number == (unsigned int)friendcon_id) {
            g->close[i].type = GROUPCHAT_CLOSE_NONE;
            kill_friend_connection(g_c->fr_c, friendcon_id);
            return 0;
        }
    }

    return -1;
}


/*
 * Delete a peer from the group chat.
 *
 * return 0 if success
 * return -1 if error.
 */
static int delpeer(Group_Chats *g_c, uint32_t groupnumber, int peer_index, void *userdata)
{
    Group_c *g = get_group_c(g_c, groupnumber);

    if (!g) {
        return -1;
    }

    uint32_t i;

    for (i = 0; i < DESIRED_CLOSE_CONNECTIONS; ++i) { /* If peer is in closest_peers list, remove it. */
        if (g->closest_peers[i].entry && id_equal(g->closest_peers[i].real_pk, g->group[peer_index].real_pk)) {
            g->closest_peers[i].entry = 0;
            g->changed = GROUPCHAT_CLOSEST_REMOVED;
            break;
        }
    }

    int friendcon_id = getfriend_conn_id_pk(g_c->fr_c, g->group[peer_index].real_pk);

    if (friendcon_id != -1) {
        remove_close_conn(g_c, groupnumber, friendcon_id);
    }

    --g->numpeers;

    void *peer_object = g->group[peer_index].object;

    if (g->numpeers == 0) {
        free(g->group);
        g->group = nullptr;
    } else {
        if (g->numpeers != (uint32_t)peer_index) {
            memcpy(&g->group[peer_index], &g->group[g->numpeers], sizeof(Group_Peer));
        }

        Group_Peer *temp = (Group_Peer *)realloc(g->group, sizeof(Group_Peer) * (g->numpeers));

        if (temp == nullptr) {
            return -1;
        }

        g->group = temp;
    }

    if (g_c->peer_list_changed_callback) {
        g_c->peer_list_changed_callback(g_c->m, groupnumber, userdata);
    }

    if (g->peer_on_leave) {
        g->peer_on_leave(g->object, groupnumber, peer_object);
    }

    return 0;
}

/* Set the nick for a peer.
 *
 * do_gc_callback indicates whether we want to trigger callbacks set by the client
 * via the public API. This should be set to false if this function is called
 * from outside of the tox_iterate() loop.
 *
 * return 0 on success.
 * return -1 if error.
 */
static int setnick(Group_Chats *g_c, uint32_t groupnumber, int peer_index, const uint8_t *nick, uint16_t nick_len,
                   void *userdata, bool do_gc_callback)
{
    if (nick_len > MAX_NAME_LENGTH) {
        return -1;
    }

    Group_c *g = get_group_c(g_c, groupnumber);

    if (!g) {
        return -1;
    }

    g->group[peer_index].nick_updated = true;

    /* same name as already stored? */
    if (g->group[peer_index].nick_len == nick_len) {
        if (nick_len == 0 || !memcmp(g->group[peer_index].nick, nick, nick_len)) {
            return 0;
        }
    }

    if (nick_len) {
        memcpy(g->group[peer_index].nick, nick, nick_len);
    }

    g->group[peer_index].nick_len = nick_len;

    if (do_gc_callback && g_c->peer_name_callback) {
        g_c->peer_name_callback(g_c->m, groupnumber, peer_index, nick, nick_len, userdata);
    }

    return 0;
}

static int settitle(Group_Chats *g_c, uint32_t groupnumber, int peer_index, const uint8_t *title, uint8_t title_len,
                    void *userdata)
{
    if (title_len > MAX_NAME_LENGTH || title_len == 0) {
        return -1;
    }

    Group_c *g = get_group_c(g_c, groupnumber);

    if (!g) {
        return -1;
    }

    /* same as already set? */
    if (g->title_len == title_len && !memcmp(g->title, title, title_len)) {
        return 0;
    }

    memcpy(g->title, title, title_len);
    g->title_len = title_len;

    if (g_c->title_callback) {
        g_c->title_callback(g_c->m, groupnumber, peer_index, title, title_len, userdata);
    }

    return 0;
}

static void set_conns_type_close(Group_Chats *g_c, uint32_t groupnumber, int friendcon_id, uint8_t type)
{
    Group_c *g = get_group_c(g_c, groupnumber);

    if (!g) {
        return;
    }

    uint32_t i;

    for (i = 0; i < MAX_GROUP_CONNECTIONS; ++i) {
        if (g->close[i].type == GROUPCHAT_CLOSE_NONE) {
            continue;
        }

        if (g->close[i].number != (unsigned int)friendcon_id) {
            continue;
        }

        if (type == GROUPCHAT_CLOSE_ONLINE) {
            send_packet_online(g_c->fr_c, friendcon_id, groupnumber, g->identifier);
        } else {
            g->close[i].type = type;
        }
    }
}
/* Set the type for all close connections with friendcon_id */
static void set_conns_status_groups(Group_Chats *g_c, int friendcon_id, uint8_t type)
{
    for (uint16_t i = 0; i < g_c->num_chats; ++i) {
        set_conns_type_close(g_c, i, friendcon_id, type);
    }
}

static int g_handle_status(void *object, int friendcon_id, uint8_t status, void *userdata)
{
    Group_Chats *g_c = (Group_Chats *)object;

    if (status) { /* Went online */
        set_conns_status_groups(g_c, friendcon_id, GROUPCHAT_CLOSE_ONLINE);
    } else { /* Went offline */
        set_conns_status_groups(g_c, friendcon_id, GROUPCHAT_CLOSE_CONNECTION);
        // TODO(irungentoo): remove timedout connections?
    }

    return 0;
}

static int g_handle_packet(void *object, int friendcon_id, const uint8_t *data, uint16_t length, void *userdata);
static int handle_lossy(void *object, int friendcon_id, const uint8_t *data, uint16_t length, void *userdata);

/* Add friend to group chat.
 *
 * return close index on success
 * return -1 on failure.
 */
static int add_conn_to_groupchat(Group_Chats *g_c, int friendcon_id, uint32_t groupnumber, uint8_t closest,
                                 uint8_t lock)
{
    Group_c *g = get_group_c(g_c, groupnumber);

    if (!g) {
        return -1;
    }

    uint16_t i, ind = MAX_GROUP_CONNECTIONS;

    for (i = 0; i < MAX_GROUP_CONNECTIONS; ++i) {
        if (g->close[i].type == GROUPCHAT_CLOSE_NONE) {
            ind = i;
            continue;
        }

        if (g->close[i].number == (uint32_t)friendcon_id) {
            g->close[i].closest |= closest;
            return i; /* Already in list. */
        }
    }

    if (ind == MAX_GROUP_CONNECTIONS) {
        return -1;
    }

    if (lock) {
        friend_connection_lock(g_c->fr_c, friendcon_id);
    }

    g->close[ind].type = GROUPCHAT_CLOSE_CONNECTION;
    g->close[ind].number = friendcon_id;
    g->close[ind].closest = closest;
    g->close[ind].introducer = false;
    g->close[ind].introduced = false;
    // TODO(irungentoo):
    friend_connection_callbacks(g_c->m->fr_c, friendcon_id, GROUPCHAT_CALLBACK_INDEX, &g_handle_status, &g_handle_packet,
                                &handle_lossy, g_c, friendcon_id);

    return ind;
}

/* Creates a new groupchat and puts it in the chats array.
 *
 * type is one of GROUPCHAT_TYPE_*
 *
 * return group number on success.
 * return -1 on failure.
 */
int add_groupchat(Group_Chats *g_c, uint8_t type)
{
    int32_t groupnumber = create_group_chat(g_c);

    if (groupnumber == -1) {
        return -1;
    }

    Group_c *g = &g_c->chats[groupnumber];

    g->status = GROUPCHAT_STATUS_CONNECTED;
    g->number_joined = -1;
    new_symmetric_key(g->identifier + 1);
    g->identifier[0] = type;
    g->peer_number = 0; /* Founder is peer 0. */
    memcpy(g->real_pk, nc_get_self_public_key(g_c->m->net_crypto), CRYPTO_PUBLIC_KEY_SIZE);
    int peer_index = addpeer(g_c, groupnumber, g->real_pk, dht_get_self_public_key(g_c->m->dht), 0, nullptr, false);

    if (peer_index == -1) {
        return -1;
    }

    setnick(g_c, groupnumber, peer_index, g_c->m->name, g_c->m->name_length, nullptr, false);

    return groupnumber;
}

static int group_kill_peer_send(const Group_Chats *g_c, uint32_t groupnumber, uint16_t peer_num);
/* Delete a groupchat from the chats array.
 *
 * return 0 on success.
 * return -1 if groupnumber is invalid.
 */
int del_groupchat(Group_Chats *g_c, uint32_t groupnumber)
{
    Group_c *g = get_group_c(g_c, groupnumber);

    if (!g) {
        return -1;
    }

    group_kill_peer_send(g_c, groupnumber, g->peer_number);

    unsigned int i;

    for (i = 0; i < MAX_GROUP_CONNECTIONS; ++i) {
        if (g->close[i].type == GROUPCHAT_CLOSE_NONE) {
            continue;
        }

        g->close[i].type = GROUPCHAT_CLOSE_NONE;
        kill_friend_connection(g_c->fr_c, g->close[i].number);
    }

    if (g->peer_on_leave) {
        g->peer_on_leave(g->object, groupnumber, g->group[i].object);
    }

    free(g->group);

    if (g->group_on_delete) {
        g->group_on_delete(g->object, groupnumber);
    }

    return wipe_group_chat(g_c, groupnumber);
}

/* Copy the public key of peernumber who is in groupnumber to pk.
 * pk must be CRYPTO_PUBLIC_KEY_SIZE long.
 *
 * return 0 on success
 * return -1 if groupnumber is invalid.
 * return -2 if peernumber is invalid.
 */
int group_peer_pubkey(const Group_Chats *g_c, uint32_t groupnumber, int peernumber, uint8_t *pk)
{
    Group_c *g = get_group_c(g_c, groupnumber);

    if (!g) {
        return -1;
    }

    if ((uint32_t)peernumber >= g->numpeers) {
        return -2;
    }

    memcpy(pk, g->group[peernumber].real_pk, CRYPTO_PUBLIC_KEY_SIZE);
    return 0;
}

/*
 * Return the size of peernumber's name.
 *
 * return -1 if groupnumber is invalid.
 * return -2 if peernumber is invalid.
 */
int group_peername_size(const Group_Chats *g_c, uint32_t groupnumber, int peernumber)
{
    Group_c *g = get_group_c(g_c, groupnumber);

    if (!g) {
        return -1;
    }

    if ((uint32_t)peernumber >= g->numpeers) {
        return -2;
    }

    if (g->group[peernumber].nick_len == 0) {
        return 0;
    }

    return g->group[peernumber].nick_len;
}

/* Copy the name of peernumber who is in groupnumber to name.
 * name must be at least MAX_NAME_LENGTH long.
 *
 * return length of name if success
 * return -1 if groupnumber is invalid.
 * return -2 if peernumber is invalid.
 */
int group_peername(const Group_Chats *g_c, uint32_t groupnumber, int peernumber, uint8_t *name)
{
    Group_c *g = get_group_c(g_c, groupnumber);

    if (!g) {
        return -1;
    }

    if ((uint32_t)peernumber >= g->numpeers) {
        return -2;
    }

    if (g->group[peernumber].nick_len == 0) {
        return 0;
    }

    memcpy(name, g->group[peernumber].nick, g->group[peernumber].nick_len);
    return g->group[peernumber].nick_len;
}

/* List all the peers in the group chat.
 *
 * Copies the names of the peers to the name[length][MAX_NAME_LENGTH] array.
 *
 * Copies the lengths of the names to lengths[length]
 *
 * returns the number of peers on success.
 *
 * return -1 on failure.
 */
int group_names(const Group_Chats *g_c, uint32_t groupnumber, uint8_t names[][MAX_NAME_LENGTH], uint16_t lengths[],
                uint16_t length)
{
    Group_c *g = get_group_c(g_c, groupnumber);

    if (!g) {
        return -1;
    }

    unsigned int i;

    for (i = 0; i < g->numpeers && i < length; ++i) {
        lengths[i] = group_peername(g_c, groupnumber, i, names[i]);
    }

    return i;
}

/* Return the number of peers in the group chat on success.
 * return -1 if groupnumber is invalid.
 */
int group_number_peers(const Group_Chats *g_c, uint32_t groupnumber)
{
    Group_c *g = get_group_c(g_c, groupnumber);

    if (!g) {
        return -1;
    }

    return g->numpeers;
}

/* return 1 if the peernumber corresponds to ours.
 * return 0 if the peernumber is not ours.
 * return -1 if groupnumber is invalid.
 * return -2 if peernumber is invalid.
 * return -3 if we are not connected to the group chat.
 */
int group_peernumber_is_ours(const Group_Chats *g_c, uint32_t groupnumber, int peernumber)
{
    Group_c *g = get_group_c(g_c, groupnumber);

    if (!g) {
        return -1;
    }

    if ((uint32_t)peernumber >= g->numpeers) {
        return -2;
    }

    if (g->status != GROUPCHAT_STATUS_CONNECTED) {
        return -3;
    }

    return g->peer_number == g->group[peernumber].peer_number;
}

/* return the type of groupchat (GROUPCHAT_TYPE_) that groupnumber is.
 *
 * return -1 on failure.
 * return type on success.
 */
int group_get_type(const Group_Chats *g_c, uint32_t groupnumber)
{
    const Group_c *g = get_group_c(g_c, groupnumber);

    if (!g) {
        return -1;
    }

    return g->identifier[0];
}

/* Copies the unique id of group_chat[groupnumber] into uid.
*
* return false on failure.
* return true on success.
*/
bool conference_get_uid(const Group_Chats *g_c, uint32_t groupnumber, uint8_t *uid)
{
    const Group_c *g = get_group_c(g_c, groupnumber);

    if (!g) {
        return false;
    }

    if (uid != nullptr) {
        memcpy(uid, g->identifier + 1, sizeof(g->identifier) - 1);
    }

    return true;
}

/* Send a group packet to friendcon_id.
 *
 *  return 1 on success
 *  return 0 on failure
 */
static unsigned int send_packet_group_peer(Friend_Connections *fr_c, int friendcon_id, uint8_t packet_id,
        uint16_t group_num, const uint8_t *data, uint16_t length)
{
    if (1 + sizeof(uint16_t) + length > MAX_CRYPTO_DATA_SIZE) {
        return 0;
    }

    group_num = net_htons(group_num);
    VLA(uint8_t, packet, 1 + sizeof(uint16_t) + length);
    packet[0] = packet_id;
    memcpy(packet + 1, &group_num, sizeof(uint16_t));
    memcpy(packet + 1 + sizeof(uint16_t), data, length);
    return write_cryptpacket(friendconn_net_crypto(fr_c), friend_connection_crypt_connection_id(fr_c, friendcon_id), packet,
                             SIZEOF_VLA(packet), 0) != -1;
}

/* Send a group lossy packet to friendcon_id.
 *
 *  return 1 on success
 *  return 0 on failure
 */
static unsigned int send_lossy_group_peer(Friend_Connections *fr_c, int friendcon_id, uint8_t packet_id,
        uint16_t group_num, const uint8_t *data, uint16_t length)
{
    if (1 + sizeof(uint16_t) + length > MAX_CRYPTO_DATA_SIZE) {
        return 0;
    }

    group_num = net_htons(group_num);
    VLA(uint8_t, packet, 1 + sizeof(uint16_t) + length);
    packet[0] = packet_id;
    memcpy(packet + 1, &group_num, sizeof(uint16_t));
    memcpy(packet + 1 + sizeof(uint16_t), data, length);
    return send_lossy_cryptpacket(friendconn_net_crypto(fr_c), friend_connection_crypt_connection_id(fr_c, friendcon_id),
                                  packet, SIZEOF_VLA(packet)) != -1;
}

/* invite friendnumber to groupnumber.
 *
 * return 0 on success.
 * return -1 if groupnumber is invalid.
 * return -2 if invite packet failed to send.
 * return -3 if we are not connected to the group chat.
 */
int invite_friend(Group_Chats *g_c, uint32_t friendnumber, uint32_t groupnumber)
{
    Group_c *g = get_group_c(g_c, groupnumber);

    if (!g) {
        return -1;
    }

    if (g->status != GROUPCHAT_STATUS_CONNECTED) {
        return -3;
    }

    uint8_t invite[INVITE_PACKET_SIZE];
    invite[0] = INVITE_ID;
    uint16_t groupchat_num = net_htons((uint16_t)groupnumber);
    memcpy(invite + 1, &groupchat_num, sizeof(groupchat_num));
    memcpy(invite + 1 + sizeof(groupchat_num), g->identifier, GROUP_IDENTIFIER_LENGTH);

    if (send_conference_invite_packet(g_c->m, friendnumber, invite, sizeof(invite))) {
        return 0;
    }

    return -2;
}

static unsigned int send_peer_query(Group_Chats *g_c, int friendcon_id, uint16_t group_num);

/* Join a group (you need to have been invited first.)
 *
 * expected_type is the groupchat type we expect the chat we are joining is.
 *
 * return group number on success.
 * return -1 if data length is invalid.
 * return -2 if group is not the expected type.
 * return -3 if friendnumber is invalid.
 * return -4 if client is already in this group.
 * return -5 if group instance failed to initialize.
 * return -6 if join packet fails to send.
 */
int join_groupchat(Group_Chats *g_c, uint32_t friendnumber, uint8_t expected_type, const uint8_t *data, uint16_t length)
{
    if (length != sizeof(uint16_t) + GROUP_IDENTIFIER_LENGTH) {
        return -1;
    }

    if (data[sizeof(uint16_t)] != expected_type) {
        return -2;
    }

    int friendcon_id = getfriendcon_id(g_c->m, friendnumber);

    if (friendcon_id == -1) {
        return -3;
    }

    if (get_group_num(g_c, data + sizeof(uint16_t)) != -1) {
        return -4;
    }

    int groupnumber = create_group_chat(g_c);

    if (groupnumber == -1) {
        return -5;
    }

    Group_c *g = &g_c->chats[groupnumber];

    uint16_t group_num = net_htons(groupnumber);
    g->status = GROUPCHAT_STATUS_VALID;
    g->number_joined = -1;
    memcpy(g->real_pk, nc_get_self_public_key(g_c->m->net_crypto), CRYPTO_PUBLIC_KEY_SIZE);

    uint8_t response[INVITE_RESPONSE_PACKET_SIZE];
    response[0] = INVITE_RESPONSE_ID;
    memcpy(response + 1, &group_num, sizeof(uint16_t));
    memcpy(response + 1 + sizeof(uint16_t), data, sizeof(uint16_t) + GROUP_IDENTIFIER_LENGTH);

    if (send_conference_invite_packet(g_c->m, friendnumber, response, sizeof(response))) {
        uint16_t other_groupnum;
        memcpy(&other_groupnum, data, sizeof(other_groupnum));
        other_groupnum = net_ntohs(other_groupnum);
        memcpy(g->identifier, data + sizeof(uint16_t), GROUP_IDENTIFIER_LENGTH);
        int close_index = add_conn_to_groupchat(g_c, friendcon_id, groupnumber, 0, 1);

        if (close_index != -1) {
            g->close[close_index].group_number = other_groupnum;
            g->close[close_index].type = GROUPCHAT_CLOSE_ONLINE;
            g->number_joined = friendcon_id;
            g->close[close_index].introducer = true;
        }

        send_peer_query(g_c, friendcon_id, other_groupnum);
        return groupnumber;
    }

    g->status = GROUPCHAT_STATUS_NONE;
    return -6;
}

/* Set handlers for custom lossy packets.
 *
 * NOTE: Handler must return 0 if packet is to be relayed, -1 if the packet should not be relayed.
 *
 * Function(void *group object (set with group_set_object), uint32_t groupnumber, uint32_t friendgroupnumber, void *group peer object (set with group_peer_set_object), const uint8_t *packet, uint16_t length)
 */
void group_lossy_packet_registerhandler(Group_Chats *g_c, uint8_t byte, lossy_packet_cb *function)
{
    g_c->lossy_packethandlers[byte].function = function;
}

/* Set the callback for group invites.
 *
 *  Function(Group_Chats *g_c, int32_t friendnumber, uint8_t type, uint8_t *data, size_t length, void *userdata)
 *
 *  data of length is what needs to be passed to join_groupchat().
 */
void g_callback_group_invite(Group_Chats *g_c, g_conference_invite_cb *function)
{
    g_c->invite_callback = function;
}

// TODO(sudden6): function signatures in comments are incorrect
/* Set the callback for group messages.
 *
 *  Function(Group_Chats *g_c, uint32_t groupnumber, uint32_t friendgroupnumber, uint8_t * message, size_t length, void *userdata)
 */
void g_callback_group_message(Group_Chats *g_c, g_conference_message_cb *function)
{
    g_c->message_callback = function;
}

/* Set callback function for peer nickname changes.
 *
 * It gets called every time a peer changes their nickname.
 *  Function(Group_Chats *g_c, uint32_t groupnumber, uint32_t peernumber, const uint8_t *nick, size_t nick_len, void *userdata)
 */
void g_callback_peer_name(Group_Chats *g_c, peer_name_cb *function)
{
    g_c->peer_name_callback = function;
}

/* Set callback function for peer list changes.
 *
 * It gets called every time the name list changes(new peer, deleted peer)
 *  Function(Group_Chats *g_c, uint32_t groupnumber, void *userdata)
 */
void g_callback_peer_list_changed(Group_Chats *g_c, peer_list_changed_cb *function)
{
    g_c->peer_list_changed_callback = function;
}

// TODO(sudden6): function signatures are incorrect
/* Set callback function for title changes.
 *
 * Function(Group_Chats *g_c, int groupnumber, int friendgroupnumber, uint8_t * title, uint8_t length, void *userdata)
 * if friendgroupnumber == -1, then author is unknown (e.g. initial joining the group)
 */
void g_callback_group_title(Group_Chats *g_c, title_cb *function)
{
    g_c->title_callback = function;
}

/* Set a function to be called when a new peer joins a group chat.
 *
 * Function(void *group object (set with group_set_object), uint32_t groupnumber, uint32_t friendgroupnumber)
 *
 * return 0 on success.
 * return -1 on failure.
 */
int callback_groupchat_peer_new(const Group_Chats *g_c, uint32_t groupnumber, peer_on_join_cb *function)
{
    Group_c *g = get_group_c(g_c, groupnumber);

    if (!g) {
        return -1;
    }

    g->peer_on_join = function;
    return 0;
}

/* Set a function to be called when a peer leaves a group chat.
 *
 * Function(void *group object (set with group_set_object), uint32_t groupnumber, uint32_t friendgroupnumber, void *group peer object (set with group_peer_set_object))
 *
 * return 0 on success.
 * return -1 on failure.
 */
int callback_groupchat_peer_delete(Group_Chats *g_c, uint32_t groupnumber, peer_on_leave_cb *function)
{
    Group_c *g = get_group_c(g_c, groupnumber);

    if (!g) {
        return -1;
    }

    g->peer_on_leave = function;
    return 0;
}

/* Set a function to be called when the group chat is deleted.
 *
 * Function(void *group object (set with group_set_object), int groupnumber)
 *
 * return 0 on success.
 * return -1 on failure.
 */
int callback_groupchat_delete(Group_Chats *g_c, uint32_t groupnumber, group_on_delete_cb *function)
{
    Group_c *g = get_group_c(g_c, groupnumber);

    if (!g) {
        return -1;
    }

    g->group_on_delete = function;
    return 0;
}

static int send_message_group(const Group_Chats *g_c, uint32_t groupnumber, uint8_t message_id, const uint8_t *data,
                              uint16_t len);

static int group_ping_send(const Group_Chats *g_c, uint32_t groupnumber)
{
    if (send_message_group(g_c, groupnumber, GROUP_MESSAGE_PING_ID, nullptr, 0) > 0) {
        return 0;
    }

    return -1;
}

/* send a new_peer message
 * return 0 on success
 * return -1 on failure
 */
static int group_new_peer_send(const Group_Chats *g_c, uint32_t groupnumber, uint16_t peer_num, const uint8_t *real_pk,
                               uint8_t *temp_pk)
{
    uint8_t packet[GROUP_MESSAGE_NEW_PEER_LENGTH];

    peer_num = net_htons(peer_num);
    memcpy(packet, &peer_num, sizeof(uint16_t));
    memcpy(packet + sizeof(uint16_t), real_pk, CRYPTO_PUBLIC_KEY_SIZE);
    memcpy(packet + sizeof(uint16_t) + CRYPTO_PUBLIC_KEY_SIZE, temp_pk, CRYPTO_PUBLIC_KEY_SIZE);

    if (send_message_group(g_c, groupnumber, GROUP_MESSAGE_NEW_PEER_ID, packet, sizeof(packet)) > 0) {
        return 0;
    }

    return -1;
}

/* send a kill_peer message
 * return 0 on success
 * return -1 on failure
 */
static int group_kill_peer_send(const Group_Chats *g_c, uint32_t groupnumber, uint16_t peer_num)
{
    uint8_t packet[GROUP_MESSAGE_KILL_PEER_LENGTH];

    peer_num = net_htons(peer_num);
    memcpy(packet, &peer_num, sizeof(uint16_t));

    if (send_message_group(g_c, groupnumber, GROUP_MESSAGE_KILL_PEER_ID, packet, sizeof(packet)) > 0) {
        return 0;
    }

    return -1;
}

/* send a name message
 * return 0 on success
 * return -1 on failure
 */
static int group_name_send(const Group_Chats *g_c, uint32_t groupnumber, const uint8_t *nick, uint16_t nick_len)
{
    if (nick_len > MAX_NAME_LENGTH) {
        return -1;
    }

    if (send_message_group(g_c, groupnumber, GROUP_MESSAGE_NAME_ID, nick, nick_len) > 0) {
        return 0;
    }

    return -1;
}

/* set the group's title, limited to MAX_NAME_LENGTH
 * return 0 on success
 * return -1 if groupnumber is invalid.
 * return -2 if title is too long or empty.
 * return -3 if packet fails to send.
 */
int group_title_send(const Group_Chats *g_c, uint32_t groupnumber, const uint8_t *title, uint8_t title_len)
{
    Group_c *g = get_group_c(g_c, groupnumber);

    if (!g) {
        return -1;
    }

    if (title_len > MAX_NAME_LENGTH || title_len == 0) {
        return -2;
    }

    /* same as already set? */
    if (g->title_len == title_len && !memcmp(g->title, title, title_len)) {
        return 0;
    }

    memcpy(g->title, title, title_len);
    g->title_len = title_len;

    if (g->numpeers == 1) {
        return 0;
    }

    if (send_message_group(g_c, groupnumber, GROUP_MESSAGE_TITLE_ID, title, title_len) > 0) {
        return 0;
    }

    return -3;
}

/* return the group's title size.
 * return -1 of groupnumber is invalid.
 * return -2 if title is too long or empty.
 */
int group_title_get_size(const Group_Chats *g_c, uint32_t groupnumber)
{
    Group_c *g = get_group_c(g_c, groupnumber);

    if (!g) {
        return -1;
    }

    if (g->title_len == 0 || g->title_len > MAX_NAME_LENGTH) {
        return -2;
    }

    return g->title_len;
}

/* Get group title from groupnumber and put it in title.
 * Title needs to be a valid memory location with a size of at least MAX_NAME_LENGTH (128) bytes.
 *
 * return length of copied title if success.
 * return -1 if groupnumber is invalid.
 * return -2 if title is too long or empty.
 */
int group_title_get(const Group_Chats *g_c, uint32_t groupnumber, uint8_t *title)
{
    Group_c *g = get_group_c(g_c, groupnumber);

    if (!g) {
        return -1;
    }

    if (g->title_len == 0 || g->title_len > MAX_NAME_LENGTH) {
        return -2;
    }

    memcpy(title, g->title, g->title_len);
    return g->title_len;
}

static void handle_friend_invite_packet(Messenger *m, uint32_t friendnumber, const uint8_t *data, uint16_t length,
                                        void *userdata)
{
    Group_Chats *g_c = (Group_Chats *)m->conferences_object;

    if (length <= 1) {
        return;
    }

    const uint8_t *invite_data = data + 1;
    uint16_t invite_length = length - 1;

    switch (data[0]) {
        case INVITE_ID: {
            if (length != INVITE_PACKET_SIZE) {
                return;
            }

            int groupnumber = get_group_num(g_c, data + 1 + sizeof(uint16_t));

            if (groupnumber == -1) {
                if (g_c->invite_callback) {
                    g_c->invite_callback(m, friendnumber, invite_data[sizeof(uint16_t)], invite_data, invite_length, userdata);
                }

                return;
            }

            break;
        }

        case INVITE_RESPONSE_ID: {
            if (length != INVITE_RESPONSE_PACKET_SIZE) {
                return;
            }

            uint16_t other_groupnum, groupnum;
            memcpy(&groupnum, data + 1 + sizeof(uint16_t), sizeof(uint16_t));
            groupnum = net_ntohs(groupnum);

            Group_c *g = get_group_c(g_c, groupnum);

            if (!g) {
                return;
            }

            if (crypto_memcmp(data + 1 + sizeof(uint16_t) * 2, g->identifier, GROUP_IDENTIFIER_LENGTH) != 0) {
                return;
            }

            /* TODO(irungentoo): what if two people enter the group at the same time and
               are given the same peer_number by different nodes? */
            uint16_t peer_number = rand();

            unsigned int tries = 0;

            while (get_peer_index(g, peer_number) != -1) {
                peer_number = rand();
                ++tries;

                if (tries > 32) {
                    return;
                }
            }

            memcpy(&other_groupnum, data + 1, sizeof(uint16_t));
            other_groupnum = net_ntohs(other_groupnum);

            int friendcon_id = getfriendcon_id(m, friendnumber);
            uint8_t real_pk[CRYPTO_PUBLIC_KEY_SIZE], temp_pk[CRYPTO_PUBLIC_KEY_SIZE];
            get_friendcon_public_keys(real_pk, temp_pk, g_c->fr_c, friendcon_id);

            addpeer(g_c, groupnum, real_pk, temp_pk, peer_number, userdata, true);
            int close_index = add_conn_to_groupchat(g_c, friendcon_id, groupnum, 0, 1);

            if (close_index != -1) {
                g->close[close_index].group_number = other_groupnum;
                g->close[close_index].type = GROUPCHAT_CLOSE_ONLINE;
                g->close[close_index].introduced = true;
            }

            group_new_peer_send(g_c, groupnum, peer_number, real_pk, temp_pk);
            break;
        }

        default:
            return;
    }
}

/* Find index of friend in the close list;
 *
 * returns index on success
 * returns -1 on failure.
 */
static int friend_in_close(Group_c *g, int friendcon_id)
{
    unsigned int i;

    for (i = 0; i < MAX_GROUP_CONNECTIONS; ++i) {
        if (g->close[i].type == GROUPCHAT_CLOSE_NONE) {
            continue;
        }

        if (g->close[i].number != (uint32_t)friendcon_id) {
            continue;
        }

        return i;
    }

    return -1;
}

/* return number of connected close connections.
 */
static unsigned int count_close_connected(Group_c *g)
{
    unsigned int i, count = 0;

    for (i = 0; i < MAX_GROUP_CONNECTIONS; ++i) {
        if (g->close[i].type == GROUPCHAT_CLOSE_ONLINE) {
            ++count;
        }
    }

    return count;
}

static int send_packet_online(Friend_Connections *fr_c, int friendcon_id, uint16_t group_num, uint8_t *identifier)
{
    uint8_t packet[1 + ONLINE_PACKET_DATA_SIZE];
    group_num = net_htons(group_num);
    packet[0] = PACKET_ID_ONLINE_PACKET;
    memcpy(packet + 1, &group_num, sizeof(uint16_t));
    memcpy(packet + 1 + sizeof(uint16_t), identifier, GROUP_IDENTIFIER_LENGTH);
    return write_cryptpacket(friendconn_net_crypto(fr_c), friend_connection_crypt_connection_id(fr_c, friendcon_id), packet,
                             sizeof(packet), 0) != -1;
}

static unsigned int send_peer_kill(Group_Chats *g_c, int friendcon_id, uint16_t group_num);

static int handle_packet_online(Group_Chats *g_c, int friendcon_id, const uint8_t *data, uint16_t length)
{
    if (length != ONLINE_PACKET_DATA_SIZE) {
        return -1;
    }

    int groupnumber = get_group_num(g_c, data + sizeof(uint16_t));

    if (groupnumber == -1) {
        return -1;
    }

    uint16_t other_groupnum;
    memcpy(&other_groupnum, data, sizeof(uint16_t));
    other_groupnum = net_ntohs(other_groupnum);

    Group_c *g = get_group_c(g_c, groupnumber);

    if (!g) {
        return -1;
    }

    int index = friend_in_close(g, friendcon_id);

    if (index == -1) {
        return -1;
    }

    if (g->close[index].type == GROUPCHAT_CLOSE_ONLINE) {
        return -1;
    }

    if (count_close_connected(g) == 0) {
        send_peer_query(g_c, friendcon_id, other_groupnum);
    }

    g->close[index].group_number = other_groupnum;
    g->close[index].type = GROUPCHAT_CLOSE_ONLINE;
    send_packet_online(g_c->fr_c, friendcon_id, groupnumber, g->identifier);

    return 0;
}

// we could send title with invite, but then if it changes between sending and accepting inv, joinee won't see it

/* return 1 on success.
 * return 0 on failure
 */
static unsigned int send_peer_kill(Group_Chats *g_c, int friendcon_id, uint16_t group_num)
{
    uint8_t packet[1];
    packet[0] = PEER_KILL_ID;
    return send_packet_group_peer(g_c->fr_c, friendcon_id, PACKET_ID_DIRECT_CONFERENCE, group_num, packet, sizeof(packet));
}


/* return 1 on success.
 * return 0 on failure
 */
static unsigned int send_peer_query(Group_Chats *g_c, int friendcon_id, uint16_t group_num)
{
    uint8_t packet[1];
    packet[0] = PEER_QUERY_ID;
    return send_packet_group_peer(g_c->fr_c, friendcon_id, PACKET_ID_DIRECT_CONFERENCE, group_num, packet, sizeof(packet));
}

/* return number of peers sent on success.
 * return 0 on failure.
 */
static unsigned int send_peers(Group_Chats *g_c, uint32_t groupnumber, int friendcon_id, uint16_t group_num)
{
    Group_c *g = get_group_c(g_c, groupnumber);

    if (!g) {
        return 0;
    }

    uint8_t response_packet[MAX_CRYPTO_DATA_SIZE - (1 + sizeof(uint16_t))];
    response_packet[0] = PEER_RESPONSE_ID;
    uint8_t *p = response_packet + 1;

    uint16_t sent = 0;
    uint32_t i;

    for (i = 0; i < g->numpeers; ++i) {
        if ((p - response_packet) + sizeof(uint16_t) + CRYPTO_PUBLIC_KEY_SIZE * 2 + 1 + g->group[i].nick_len > sizeof(
                    response_packet)) {
            if (send_packet_group_peer(g_c->fr_c, friendcon_id, PACKET_ID_DIRECT_CONFERENCE, group_num, response_packet,
                                       (p - response_packet))) {
                sent = i;
            } else {
                return sent;
            }

            p = response_packet + 1;
        }

        uint16_t peer_num = net_htons(g->group[i].peer_number);
        memcpy(p, &peer_num, sizeof(peer_num));
        p += sizeof(peer_num);
        memcpy(p, g->group[i].real_pk, CRYPTO_PUBLIC_KEY_SIZE);
        p += CRYPTO_PUBLIC_KEY_SIZE;
        memcpy(p, g->group[i].temp_pk, CRYPTO_PUBLIC_KEY_SIZE);
        p += CRYPTO_PUBLIC_KEY_SIZE;
        *p = g->group[i].nick_len;
        p += 1;
        memcpy(p, g->group[i].nick, g->group[i].nick_len);
        p += g->group[i].nick_len;
    }

    if (sent != i) {
        if (send_packet_group_peer(g_c->fr_c, friendcon_id, PACKET_ID_DIRECT_CONFERENCE, group_num, response_packet,
                                   (p - response_packet))) {
            sent = i;
        }
    }

    if (g->title_len) {
        VLA(uint8_t, title_packet, 1 + g->title_len);
        title_packet[0] = PEER_TITLE_ID;
        memcpy(title_packet + 1, g->title, g->title_len);
        send_packet_group_peer(g_c->fr_c, friendcon_id, PACKET_ID_DIRECT_CONFERENCE, group_num, title_packet,
                               SIZEOF_VLA(title_packet));
    }

    return sent;
}

static int handle_send_peers(Group_Chats *g_c, uint32_t groupnumber, const uint8_t *data, uint16_t length,
                             void *userdata)
{
    if (length == 0) {
        return -1;
    }

    Group_c *g = get_group_c(g_c, groupnumber);

    if (!g) {
        return -1;
    }

    const uint8_t *d = data;

    while ((unsigned int)(length - (d - data)) >= sizeof(uint16_t) + CRYPTO_PUBLIC_KEY_SIZE * 2 + 1) {
        uint16_t peer_num;
        memcpy(&peer_num, d, sizeof(peer_num));
        peer_num = net_ntohs(peer_num);
        d += sizeof(uint16_t);

        if (g->status == GROUPCHAT_STATUS_VALID
                && public_key_cmp(d, nc_get_self_public_key(g_c->m->net_crypto)) == 0) {
            g->peer_number = peer_num;
            g->status = GROUPCHAT_STATUS_CONNECTED;
            group_name_send(g_c, groupnumber, g_c->m->name, g_c->m->name_length);
        }

        int peer_index = addpeer(g_c, groupnumber, d, d + CRYPTO_PUBLIC_KEY_SIZE, peer_num, userdata, true);

        if (peer_index == -1) {
            return -1;
        }

        d += CRYPTO_PUBLIC_KEY_SIZE * 2;
        uint8_t name_length = *d;
        d += 1;

        if (name_length > (length - (d - data)) || name_length > MAX_NAME_LENGTH) {
            return -1;
        }

        if (!g->group[peer_index].nick_updated) {
            setnick(g_c, groupnumber, peer_index, d, name_length, userdata, true);
        }

        d += name_length;
    }

    return 0;
}

static void handle_direct_packet(Group_Chats *g_c, uint32_t groupnumber, const uint8_t *data, uint16_t length,
                                 int close_index, void *userdata)
{
    if (length == 0) {
        return;
    }

    switch (data[0]) {
        case PEER_KILL_ID: {
            Group_c *g = get_group_c(g_c, groupnumber);

            if (!g) {
                return;
            }

            if (!g->close[close_index].closest) {
                g->close[close_index].type = GROUPCHAT_CLOSE_NONE;
                kill_friend_connection(g_c->fr_c, g->close[close_index].number);
            }
        }

        break;

        case PEER_QUERY_ID: {
            Group_c *g = get_group_c(g_c, groupnumber);

            if (!g) {
                return;
            }

            send_peers(g_c, groupnumber, g->close[close_index].number, g->close[close_index].group_number);
        }

        break;

        case PEER_RESPONSE_ID: {
            handle_send_peers(g_c, groupnumber, data + 1, length - 1, userdata);
        }

        break;

        case PEER_TITLE_ID: {
            settitle(g_c, groupnumber, -1, data + 1, length - 1, userdata);
        }

        break;
    }
}

/* Send message to all close except receiver (if receiver isn't -1)
 * NOTE: this function appends the group chat number to the data passed to it.
 *
 * return number of messages sent.
 */
static unsigned int send_message_all_close(const Group_Chats *g_c, uint32_t groupnumber, const uint8_t *data,
        uint16_t length, int receiver)
{
    Group_c *g = get_group_c(g_c, groupnumber);

    if (!g) {
        return 0;
    }

    uint16_t i, sent = 0;

    for (i = 0; i < MAX_GROUP_CONNECTIONS; ++i) {
        if (g->close[i].type != GROUPCHAT_CLOSE_ONLINE) {
            continue;
        }

        if ((int)i == receiver) {
            continue;
        }

        if (send_packet_group_peer(g_c->fr_c, g->close[i].number, PACKET_ID_MESSAGE_CONFERENCE, g->close[i].group_number, data,
                                   length)) {
            ++sent;
        }
    }

    return sent;
}

/* Send lossy message to all close except receiver (if receiver isn't -1)
 * NOTE: this function appends the group chat number to the data passed to it.
 *
 * return number of messages sent.
 */
static unsigned int send_lossy_all_close(const Group_Chats *g_c, uint32_t groupnumber, const uint8_t *data,
        uint16_t length,
        int receiver)
{
    Group_c *g = get_group_c(g_c, groupnumber);

    if (!g) {
        return 0;
    }

    unsigned int i, sent = 0, num_connected_closest = 0, connected_closest[DESIRED_CLOSE_CONNECTIONS];

    for (i = 0; i < MAX_GROUP_CONNECTIONS; ++i) {
        if (g->close[i].type != GROUPCHAT_CLOSE_ONLINE) {
            continue;
        }

        if ((int)i == receiver) {
            continue;
        }

        if (g->close[i].closest) {
            connected_closest[num_connected_closest] = i;
            ++num_connected_closest;
            continue;
        }

        if (send_lossy_group_peer(g_c->fr_c, g->close[i].number, PACKET_ID_LOSSY_CONFERENCE, g->close[i].group_number, data,
                                  length)) {
            ++sent;
        }
    }

    if (!num_connected_closest) {
        return sent;
    }

    unsigned int to_send = 0;
    uint64_t comp_val_old = ~0;

    for (i = 0; i < num_connected_closest; ++i) {
        uint8_t real_pk[CRYPTO_PUBLIC_KEY_SIZE] = {0};
        uint8_t dht_temp_pk[CRYPTO_PUBLIC_KEY_SIZE] = {0};
        get_friendcon_public_keys(real_pk, dht_temp_pk, g_c->fr_c, g->close[connected_closest[i]].number);
        uint64_t comp_val = calculate_comp_value(g->real_pk, real_pk);

        if (comp_val < comp_val_old) {
            to_send = connected_closest[i];
            comp_val_old = comp_val;
        }
    }

    if (send_lossy_group_peer(g_c->fr_c, g->close[to_send].number, PACKET_ID_LOSSY_CONFERENCE,
                              g->close[to_send].group_number, data, length)) {
        ++sent;
    }

    unsigned int to_send_other = 0;
    comp_val_old = ~0;

    for (i = 0; i < num_connected_closest; ++i) {
        uint8_t real_pk[CRYPTO_PUBLIC_KEY_SIZE] = {0};
        uint8_t dht_temp_pk[CRYPTO_PUBLIC_KEY_SIZE] = {0};
        get_friendcon_public_keys(real_pk, dht_temp_pk, g_c->fr_c, g->close[connected_closest[i]].number);
        uint64_t comp_val = calculate_comp_value(real_pk, g->real_pk);

        if (comp_val < comp_val_old) {
            to_send_other = connected_closest[i];
            comp_val_old = comp_val;
        }
    }

    if (to_send_other == to_send) {
        return sent;
    }

    if (send_lossy_group_peer(g_c->fr_c, g->close[to_send_other].number, PACKET_ID_LOSSY_CONFERENCE,
                              g->close[to_send_other].group_number, data, length)) {
        ++sent;
    }

    return sent;
}

/* Send data of len with message_id to groupnumber.
 *
 * return number of peers it was sent to on success.
 * return -1 if groupnumber is invalid.
 * return -2 if message is too long.
 * return -3 if we are not connected to the group.
 * reutrn -4 if message failed to send.
 */
static int send_message_group(const Group_Chats *g_c, uint32_t groupnumber, uint8_t message_id, const uint8_t *data,
                              uint16_t len)
{
    Group_c *g = get_group_c(g_c, groupnumber);

    if (!g) {
        return -1;
    }

    if (len > MAX_GROUP_MESSAGE_DATA_LEN) {
        return -2;
    }

    if (g->status != GROUPCHAT_STATUS_CONNECTED) {
        return -3;
    }

    VLA(uint8_t, packet, sizeof(uint16_t) + sizeof(uint32_t) + 1 + len);
    uint16_t peer_num = net_htons(g->peer_number);
    memcpy(packet, &peer_num, sizeof(peer_num));

    ++g->message_number;

    if (!g->message_number) {
        ++g->message_number;
    }

    uint32_t message_num = net_htonl(g->message_number);
    memcpy(packet + sizeof(uint16_t), &message_num, sizeof(message_num));

    packet[sizeof(uint16_t) + sizeof(uint32_t)] = message_id;

    if (len) {
        memcpy(packet + sizeof(uint16_t) + sizeof(uint32_t) + 1, data, len);
    }

    unsigned int ret = send_message_all_close(g_c, groupnumber, packet, SIZEOF_VLA(packet), -1);

    return (ret == 0) ? -4 : ret;
}

/* send a group message
 * return 0 on success
 * see: send_message_group() for error codes.
 */
int group_message_send(const Group_Chats *g_c, uint32_t groupnumber, const uint8_t *message, uint16_t length)
{
    int ret = send_message_group(g_c, groupnumber, PACKET_ID_MESSAGE, message, length);

    if (ret > 0) {
        return 0;
    }

    return ret;
}

/* send a group action
 * return 0 on success
 * see: send_message_group() for error codes.
 */
int group_action_send(const Group_Chats *g_c, uint32_t groupnumber, const uint8_t *action, uint16_t length)
{
    int ret = send_message_group(g_c, groupnumber, PACKET_ID_ACTION, action, length);

    if (ret > 0) {
        return 0;
    }

    return ret;
}

/* High level function to send custom lossy packets.
 *
 * return -1 on failure.
 * return 0 on success.
 */
int send_group_lossy_packet(const Group_Chats *g_c, uint32_t groupnumber, const uint8_t *data, uint16_t length)
{
    // TODO(irungentoo): length check here?
    Group_c *g = get_group_c(g_c, groupnumber);

    if (!g) {
        return -1;
    }

    VLA(uint8_t, packet, sizeof(uint16_t) * 2 + length);
    uint16_t peer_number = net_htons(g->peer_number);
    memcpy(packet, &peer_number, sizeof(uint16_t));
    uint16_t message_num = net_htons(g->lossy_message_number);
    memcpy(packet + sizeof(uint16_t), &message_num, sizeof(uint16_t));
    memcpy(packet + sizeof(uint16_t) * 2, data, length);

    if (send_lossy_all_close(g_c, groupnumber, packet, SIZEOF_VLA(packet), -1) == 0) {
        return -1;
    }

    ++g->lossy_message_number;
    return 0;
}

static Message_Info *find_message_slot_or_reject(uint32_t message_number, uint8_t message_id, Group_Peer *peer)
{
    const bool ignore_older = (message_id == GROUP_MESSAGE_NAME_ID || message_id == GROUP_MESSAGE_TITLE_ID);

    Message_Info *i;

    for (i = peer->last_message_infos; i < peer->last_message_infos + peer->num_last_message_infos; ++i) {
        if (message_number > i->message_number) {
            break;
        }

        if (message_number == i->message_number) {
            return nullptr;
        }

        if (ignore_older && message_id == i->message_id) {
            return nullptr;
        }
    }

    return i;
}

/* Stores message info in peer->last_message_infos.
 *
 * return true if message should be processed;
 * return false otherwise.
 */
static bool check_message_info(uint32_t message_number, uint8_t message_id, Group_Peer *peer)
{
    Message_Info *const i = find_message_slot_or_reject(message_number, message_id, peer);

    if (i == nullptr) {
        return false;
    }

    if (i == peer->last_message_infos + MAX_LAST_MESSAGE_INFOS) {
        return false;
    }

    if (peer->num_last_message_infos < MAX_LAST_MESSAGE_INFOS) {
        ++peer->num_last_message_infos;
    }

    memmove(i + 1, i, ((peer->last_message_infos + peer->num_last_message_infos - 1) - i) * sizeof(Message_Info));

    i->message_number = message_number;
    i->message_id = message_id;

    return true;
}

static void handle_message_packet_group(Group_Chats *g_c, uint32_t groupnumber, const uint8_t *data, uint16_t length,
                                        int close_index, void *userdata)
{
    if (length < sizeof(uint16_t) + sizeof(uint32_t) + 1) {
        return;
    }

    Group_c *g = get_group_c(g_c, groupnumber);

    if (!g) {
        return;
    }

    uint16_t peer_number;
    memcpy(&peer_number, data, sizeof(uint16_t));
    peer_number = net_ntohs(peer_number);

    int index = get_peer_index(g, peer_number);

    if (index == -1) {
        /* We don't know the peer this packet came from so we query the list of peers from that peer.
          (They would not have relayed it if they didn't know the peer.) */
        send_peer_query(g_c, g->close[close_index].number, g->close[close_index].group_number);
        return;
    }

    if (g->number_joined != -1 && count_close_connected(g) >= DESIRED_CLOSE_CONNECTIONS) {
        const int fr_close_index = friend_in_close(g, g->number_joined);

        if (fr_close_index >= 0 && fr_close_index != close_index && !g->close[fr_close_index].closest) {
            uint8_t real_pk[CRYPTO_PUBLIC_KEY_SIZE];
            get_friendcon_public_keys(real_pk, nullptr, g_c->fr_c, g->close[fr_close_index].number);

            if (id_equal(g->group[index].real_pk, real_pk)) {
                /* Received message from peer relayed via another peer, so
                 * the introduction was successful */
                g->number_joined = -1;
                g->close[fr_close_index].introducer = false;

                if (!g->close[fr_close_index].closest && !g->close[fr_close_index].introduced) {
                    g->close[fr_close_index].type = GROUPCHAT_CLOSE_NONE;
                    send_peer_kill(g_c, g->close[fr_close_index].number, g->close[fr_close_index].group_number);
                    kill_friend_connection(g_c->fr_c, g->close[fr_close_index].number);
                }
            }
        }
    }

    uint32_t message_number;
    memcpy(&message_number, data + sizeof(uint16_t), sizeof(message_number));
    message_number = net_ntohl(message_number);

    uint8_t message_id = data[sizeof(uint16_t) + sizeof(message_number)];
    const uint8_t *msg_data = data + sizeof(uint16_t) + sizeof(message_number) + 1;
    uint16_t msg_data_len = length - (sizeof(uint16_t) + sizeof(message_number) + 1);

    // FIXME(zugz) update discussion of message numbers in the spec
    if (!check_message_info(message_number, message_id, &g->group[index])) {
        return;
    }

    switch (message_id) {
        case GROUP_MESSAGE_PING_ID: {
            if (msg_data_len != 0) {
                return;
            }

            g->group[index].last_recv = unix_time();
        }
        break;

        case GROUP_MESSAGE_NEW_PEER_ID: {
            if (msg_data_len != GROUP_MESSAGE_NEW_PEER_LENGTH) {
                return;
            }

            uint16_t new_peer_number;
            memcpy(&new_peer_number, msg_data, sizeof(uint16_t));
            new_peer_number = net_ntohs(new_peer_number);
            addpeer(g_c, groupnumber, msg_data + sizeof(uint16_t), msg_data + sizeof(uint16_t) + CRYPTO_PUBLIC_KEY_SIZE,
                    new_peer_number, userdata, true);
        }
        break;

        case GROUP_MESSAGE_KILL_PEER_ID: {
            if (msg_data_len != GROUP_MESSAGE_KILL_PEER_LENGTH) {
                return;
            }

            uint16_t kill_peer_number;
            memcpy(&kill_peer_number, msg_data, sizeof(uint16_t));
            kill_peer_number = net_ntohs(kill_peer_number);

            if (peer_number == kill_peer_number) {
                delpeer(g_c, groupnumber, index, userdata);
            } else {
                return;
                // TODO(irungentoo):
            }
        }
        break;

        case GROUP_MESSAGE_NAME_ID: {
            if (setnick(g_c, groupnumber, index, msg_data, msg_data_len, userdata, true) == -1) {
                return;
            }
        }
        break;

        case GROUP_MESSAGE_TITLE_ID: {
            if (settitle(g_c, groupnumber, index, msg_data, msg_data_len, userdata) == -1) {
                return;
            }
        }
        break;

        case PACKET_ID_MESSAGE: {
            if (msg_data_len == 0) {
                return;
            }

            VLA(uint8_t, newmsg, msg_data_len + 1);
            memcpy(newmsg, msg_data, msg_data_len);
            newmsg[msg_data_len] = 0;

            // TODO(irungentoo):
            if (g_c->message_callback) {
                g_c->message_callback(g_c->m, groupnumber, index, 0, newmsg, msg_data_len, userdata);
            }

            break;
        }

        case PACKET_ID_ACTION: {
            if (msg_data_len == 0) {
                return;
            }

            VLA(uint8_t, newmsg, msg_data_len + 1);
            memcpy(newmsg, msg_data, msg_data_len);
            newmsg[msg_data_len] = 0;

            // TODO(irungentoo):
            if (g_c->message_callback) {
                g_c->message_callback(g_c->m, groupnumber, index, 1, newmsg, msg_data_len, userdata);
            }

            break;
        }

        default:
            return;
    }

    send_message_all_close(g_c, groupnumber, data, length, -1/* TODO(irungentoo) close_index */);
}

static int g_handle_packet(void *object, int friendcon_id, const uint8_t *data, uint16_t length, void *userdata)
{
    Group_Chats *g_c = (Group_Chats *)object;

    if (length < 1 + sizeof(uint16_t) + 1) {
        return -1;
    }

    if (data[0] == PACKET_ID_ONLINE_PACKET) {
        return handle_packet_online(g_c, friendcon_id, data + 1, length - 1);
    }

    if (data[0] != PACKET_ID_DIRECT_CONFERENCE && data[0] != PACKET_ID_MESSAGE_CONFERENCE) {
        return -1;
    }

    uint16_t groupnumber;
    memcpy(&groupnumber, data + 1, sizeof(uint16_t));
    groupnumber = net_ntohs(groupnumber);
    Group_c *g = get_group_c(g_c, groupnumber);

    if (!g) {
        return -1;
    }

    int index = friend_in_close(g, friendcon_id);

    if (index == -1) {
        return -1;
    }

    switch (data[0]) {
        case PACKET_ID_DIRECT_CONFERENCE: {
            handle_direct_packet(g_c, groupnumber, data + 1 + sizeof(uint16_t), length - (1 + sizeof(uint16_t)), index, userdata);
            break;
        }

        case PACKET_ID_MESSAGE_CONFERENCE: {
            handle_message_packet_group(g_c, groupnumber, data + 1 + sizeof(uint16_t), length - (1 + sizeof(uint16_t)), index,
                                        userdata);
            break;
        }

        default: {
            return 0;
        }
    }

    return 0;
}

/* Did we already receive the lossy packet or not.
 *
 * return -1 on failure.
 * return 0 if packet was not received.
 * return 1 if packet was received.
 *
 * TODO(irungentoo): test this
 */
static unsigned int lossy_packet_not_received(Group_c *g, int peer_index, uint16_t message_number)
{
    if (peer_index == -1) {
        // TODO(sudden6): invalid return value
        return -1;
    }

    if (g->group[peer_index].bottom_lossy_number == g->group[peer_index].top_lossy_number) {
        g->group[peer_index].top_lossy_number = message_number;
        g->group[peer_index].bottom_lossy_number = (message_number - MAX_LOSSY_COUNT) + 1;
        g->group[peer_index].recv_lossy[message_number % MAX_LOSSY_COUNT] = 1;
        return 0;
    }

    if ((uint16_t)(message_number - g->group[peer_index].bottom_lossy_number) < MAX_LOSSY_COUNT) {
        if (g->group[peer_index].recv_lossy[message_number % MAX_LOSSY_COUNT]) {
            return 1;
        }

        g->group[peer_index].recv_lossy[message_number % MAX_LOSSY_COUNT] = 1;
        return 0;
    }

    if ((uint16_t)(message_number - g->group[peer_index].bottom_lossy_number) > (1 << 15)) {
        // TODO(sudden6): invalid return value
        return -1;
    }

    uint16_t top_distance = message_number - g->group[peer_index].top_lossy_number;

    if (top_distance >= MAX_LOSSY_COUNT) {
        crypto_memzero(g->group[peer_index].recv_lossy, sizeof(g->group[peer_index].recv_lossy));
        g->group[peer_index].top_lossy_number = message_number;
        g->group[peer_index].bottom_lossy_number = (message_number - MAX_LOSSY_COUNT) + 1;
        g->group[peer_index].recv_lossy[message_number % MAX_LOSSY_COUNT] = 1;
    } else {  // top_distance < MAX_LOSSY_COUNT
        for (unsigned int i = g->group[peer_index].bottom_lossy_number;
                i != g->group[peer_index].bottom_lossy_number + top_distance;
                ++i) {
            g->group[peer_index].recv_lossy[i % MAX_LOSSY_COUNT] = 0;
        }

        g->group[peer_index].top_lossy_number = message_number;
        g->group[peer_index].bottom_lossy_number = (message_number - MAX_LOSSY_COUNT) + 1;
        g->group[peer_index].recv_lossy[message_number % MAX_LOSSY_COUNT] = 1;
    }

    return 0;

}

static int handle_lossy(void *object, int friendcon_id, const uint8_t *data, uint16_t length, void *userdata)
{
    Group_Chats *g_c = (Group_Chats *)object;

    if (length < 1 + sizeof(uint16_t) * 3 + 1) {
        return -1;
    }

    if (data[0] != PACKET_ID_LOSSY_CONFERENCE) {
        return -1;
    }

    uint16_t groupnumber, peer_number, message_number;
    memcpy(&groupnumber, data + 1, sizeof(uint16_t));
    memcpy(&peer_number, data + 1 + sizeof(uint16_t), sizeof(uint16_t));
    memcpy(&message_number, data + 1 + sizeof(uint16_t) * 2, sizeof(uint16_t));
    groupnumber = net_ntohs(groupnumber);
    peer_number = net_ntohs(peer_number);
    message_number = net_ntohs(message_number);

    Group_c *g = get_group_c(g_c, groupnumber);

    if (!g) {
        return -1;
    }

    int index = friend_in_close(g, friendcon_id);

    if (index == -1) {
        return -1;
    }

    if (peer_number == g->peer_number) {
        return -1;
    }

    int peer_index = get_peer_index(g, peer_number);

    if (peer_index == -1) {
        return -1;
    }

    if (lossy_packet_not_received(g, peer_index, message_number)) {
        return -1;
    }

    const uint8_t *lossy_data = data + 1 + sizeof(uint16_t) * 3;
    uint16_t lossy_length = length - (1 + sizeof(uint16_t) * 3);
    uint8_t message_id = lossy_data[0];
    ++lossy_data;
    --lossy_length;

    if (g_c->lossy_packethandlers[message_id].function) {
        if (g_c->lossy_packethandlers[message_id].function(g->object, groupnumber, peer_index, g->group[peer_index].object,
                lossy_data, lossy_length) == -1) {
            return -1;
        }
    } else {
        return -1;
    }

    send_lossy_all_close(g_c, groupnumber, data + 1 + sizeof(uint16_t), length - (1 + sizeof(uint16_t)), index);
    return 0;
}

/* Set the object that is tied to the group chat.
 *
 * return 0 on success.
 * return -1 on failure
 */
int group_set_object(const Group_Chats *g_c, uint32_t groupnumber, void *object)
{
    Group_c *g = get_group_c(g_c, groupnumber);

    if (!g) {
        return -1;
    }

    g->object = object;
    return 0;
}

/* Set the object that is tied to the group peer.
 *
 * return 0 on success.
 * return -1 on failure
 */
int group_peer_set_object(const Group_Chats *g_c, uint32_t groupnumber, int peernumber, void *object)
{
    Group_c *g = get_group_c(g_c, groupnumber);

    if (!g) {
        return -1;
    }

    if ((uint32_t)peernumber >= g->numpeers) {
        return -1;
    }

    g->group[peernumber].object = object;
    return 0;
}

/* Return the object tide to the group chat previously set by group_set_object.
 *
 * return NULL on failure.
 * return object on success.
 */
void *group_get_object(const Group_Chats *g_c, uint32_t groupnumber)
{
    Group_c *g = get_group_c(g_c, groupnumber);

    if (!g) {
        return nullptr;
    }

    return g->object;
}

/* Return the object tide to the group chat peer previously set by group_peer_set_object.
 *
 * return NULL on failure.
 * return object on success.
 */
void *group_peer_get_object(const Group_Chats *g_c, uint32_t groupnumber, int peernumber)
{
    Group_c *g = get_group_c(g_c, groupnumber);

    if (!g) {
        return nullptr;
    }

    if ((uint32_t)peernumber >= g->numpeers) {
        return nullptr;
    }

    return g->group[peernumber].object;
}

/* Interval in seconds to send ping messages */
#define GROUP_PING_INTERVAL 20

static int ping_groupchat(Group_Chats *g_c, uint32_t groupnumber)
{
    Group_c *g = get_group_c(g_c, groupnumber);

    if (!g) {
        return -1;
    }

    if (is_timeout(g->last_sent_ping, GROUP_PING_INTERVAL)) {
        if (group_ping_send(g_c, groupnumber) != -1) { /* Ping */
            g->last_sent_ping = unix_time();
        }
    }

    return 0;
}

static int groupchat_clear_timedout(Group_Chats *g_c, uint32_t groupnumber, void *userdata)
{
    Group_c *g = get_group_c(g_c, groupnumber);

    if (!g) {
        return -1;
    }

    for (uint32_t i = 0; i < g->numpeers; ++i) {
        if (g->peer_number != g->group[i].peer_number && is_timeout(g->group[i].last_recv, GROUP_PING_INTERVAL * 3)) {
            delpeer(g_c, groupnumber, i, userdata);
        }

        if (g->group == nullptr || i >= g->numpeers) {
            break;
        }
    }

    return 0;
}

/* Send current name (set in messenger) to all online groups.
 */
void send_name_all_groups(Group_Chats *g_c)
{
    for (uint16_t i = 0; i < g_c->num_chats; ++i) {
        Group_c *g = get_group_c(g_c, i);

        if (!g) {
            continue;
        }

        if (g->status == GROUPCHAT_STATUS_CONNECTED) {
            group_name_send(g_c, i, g_c->m->name, g_c->m->name_length);
        }
    }
}

/* Create new groupchat instance. */
Group_Chats *new_groupchats(Messenger *m)
{
    if (!m) {
        return nullptr;
    }

    Group_Chats *temp = (Group_Chats *)calloc(1, sizeof(Group_Chats));

    if (temp == nullptr) {
        return nullptr;
    }

    temp->m = m;
    temp->fr_c = m->fr_c;
    m->conferences_object = temp;
    m_callback_conference_invite(m, &handle_friend_invite_packet);

    return temp;
}

/* main groupchats loop. */
void do_groupchats(Group_Chats *g_c, void *userdata)
{
    for (uint16_t i = 0; i < g_c->num_chats; ++i) {
        Group_c *g = get_group_c(g_c, i);

        if (!g) {
            continue;
        }

        if (g->status == GROUPCHAT_STATUS_CONNECTED) {
            connect_to_closest(g_c, i, userdata);
            ping_groupchat(g_c, i);
            groupchat_clear_timedout(g_c, i, userdata);
        }
    }

    // TODO(irungentoo):
}

/* Free everything related with group chats. */
void kill_groupchats(Group_Chats *g_c)
{
    for (uint16_t i = 0; i < g_c->num_chats; ++i) {
        del_groupchat(g_c, i);
    }

    m_callback_conference_invite(g_c->m, nullptr);
    g_c->m->conferences_object = nullptr;
    free(g_c);
}

/* Return the number of chats in the instance m.
 * You should use this to determine how much memory to allocate
 * for copy_chatlist.
 */
uint32_t count_chatlist(const Group_Chats *g_c)
{
    uint32_t ret = 0;

    for (uint16_t i = 0; i < g_c->num_chats; ++i) {
        if (g_c->chats[i].status != GROUPCHAT_STATUS_NONE) {
            ++ret;
        }
    }

    return ret;
}

/* Copy a list of valid chat IDs into the array out_list.
 * If out_list is NULL, returns 0.
 * Otherwise, returns the number of elements copied.
 * If the array was too small, the contents
 * of out_list will be truncated to list_size. */
uint32_t copy_chatlist(const Group_Chats *g_c, uint32_t *out_list, uint32_t list_size)
{
    if (!out_list) {
        return 0;
    }

    if (g_c->num_chats == 0) {
        return 0;
    }

    uint32_t ret = 0;

    for (uint16_t i = 0; i < g_c->num_chats; ++i) {
        if (ret >= list_size) {
            break;  /* Abandon ship */
        }

        if (g_c->chats[i].status > GROUPCHAT_STATUS_NONE) {
            out_list[ret] = i;
            ++ret;
        }
    }

    return ret;
}
