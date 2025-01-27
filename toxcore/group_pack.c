/* SPDX-License-Identifier: GPL-3.0-or-later
 * Copyright © 2016-2020 The TokTok team.
 * Copyright © 2015 Tox project.
 */

/**
 * Packer and unpacker functions for saving and loading groups.
 */

#include "group_pack.h"

#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "bin_pack.h"
#include "bin_unpack.h"
#include "ccompat.h"
#include "util.h"

non_null()
static bool load_unpack_state_values(GC_Chat *chat, Bin_Unpack *bu)
{
    if (!bin_unpack_array_fixed(bu, 8)) {
        LOGGER_ERROR(chat->log, "Group state values array malformed");
        return false;
    }

    bool manually_disconnected = false;
    uint8_t privacy_state = 0;
    uint8_t voice_state = 0;

    if (!(bin_unpack_bool(bu, &manually_disconnected)
            && bin_unpack_u16(bu, &chat->shared_state.group_name_len)
            && bin_unpack_u08(bu, &privacy_state)
            && bin_unpack_u16(bu, &chat->shared_state.maxpeers)
            && bin_unpack_u16(bu, &chat->shared_state.password_length)
            && bin_unpack_u32(bu, &chat->shared_state.version)
            && bin_unpack_u32(bu, &chat->shared_state.topic_lock)
            && bin_unpack_u08(bu, &voice_state))) {
        LOGGER_ERROR(chat->log, "Failed to unpack state value");
        return false;
    }

    chat->connection_state = manually_disconnected ? CS_DISCONNECTED : CS_CONNECTING;
    chat->shared_state.privacy_state = (Group_Privacy_State)privacy_state;
    chat->shared_state.voice_state = (Group_Voice_State)voice_state;

    // we always load saved groups as private in case the group became private while we were offline.
    // this will have no detrimental effect if the group is public, as the correct privacy
    // state will be set via sync.
    chat->join_type = HJ_PRIVATE;

    return true;
}

non_null()
static bool load_unpack_state_bin(GC_Chat *chat, Bin_Unpack *bu)
{
    if (!bin_unpack_array_fixed(bu, 5)) {
        LOGGER_ERROR(chat->log, "Group state binary array malformed");
        return false;
    }

    if (!(bin_unpack_bin_fixed(bu, chat->shared_state_sig, SIGNATURE_SIZE)
            && bin_unpack_bin_fixed(bu, chat->shared_state.founder_public_key, EXT_PUBLIC_KEY_SIZE)
            && bin_unpack_bin_fixed(bu, chat->shared_state.group_name, chat->shared_state.group_name_len)
            && bin_unpack_bin_fixed(bu, chat->shared_state.password, chat->shared_state.password_length)
            && bin_unpack_bin_fixed(bu, chat->shared_state.mod_list_hash, MOD_MODERATION_HASH_SIZE))) {
        LOGGER_ERROR(chat->log, "Failed to unpack state binary data");
        return false;
    }

    return true;
}

non_null()
static bool load_unpack_topic_info(GC_Chat *chat, Bin_Unpack *bu)
{
    if (!bin_unpack_array_fixed(bu, 6)) {
        LOGGER_ERROR(chat->log, "Group topic array malformed");
        return false;
    }

    if (!(bin_unpack_u32(bu, &chat->topic_info.version)
            && bin_unpack_u16(bu, &chat->topic_info.length)
            && bin_unpack_u16(bu, &chat->topic_info.checksum)
            && bin_unpack_bin_fixed(bu, chat->topic_info.topic, chat->topic_info.length)
            && bin_unpack_bin_fixed(bu, chat->topic_info.public_sig_key, SIG_PUBLIC_KEY_SIZE)
            && bin_unpack_bin_fixed(bu, chat->topic_sig, SIGNATURE_SIZE))) {
        LOGGER_ERROR(chat->log, "Failed to unpack topic info");
        return false;
    }

    return true;
}

non_null()
static bool load_unpack_mod_list(GC_Chat *chat, Bin_Unpack *bu)
{
    if (!bin_unpack_array_fixed(bu, 2)) {
        LOGGER_ERROR(chat->log, "Group mod list array malformed");
        return false;
    }

    if (!bin_unpack_u16(bu, &chat->moderation.num_mods)) {
        LOGGER_ERROR(chat->log, "Failed to unpack mod list value");
        return false;
    }

    if (chat->moderation.num_mods == 0) {
        bin_unpack_nil(bu);
        return true;
    }

    if (chat->moderation.num_mods > MOD_MAX_NUM_MODERATORS) {
        LOGGER_ERROR(chat->log, "moderation count %u exceeds maximum %u", chat->moderation.num_mods, MOD_MAX_NUM_MODERATORS);
        return false;
    }

    uint8_t *packed_mod_list = (uint8_t *)malloc(chat->moderation.num_mods * MOD_LIST_ENTRY_SIZE);

    if (packed_mod_list == nullptr) {
        LOGGER_ERROR(chat->log, "Failed to allocate memory for packed mod list");
        return false;
    }

    const size_t packed_size = chat->moderation.num_mods * MOD_LIST_ENTRY_SIZE;

    if (!bin_unpack_bin_fixed(bu, packed_mod_list, packed_size)) {
        LOGGER_ERROR(chat->log, "Failed to unpack mod list binary data");
        free(packed_mod_list);
        return false;
    }

    if (mod_list_unpack(&chat->moderation, packed_mod_list, packed_size, chat->moderation.num_mods) == -1) {
        LOGGER_ERROR(chat->log, "Failed to unpack mod list info");
        free(packed_mod_list);
        return false;
    }

    free(packed_mod_list);

    return true;
}

non_null()
static bool load_unpack_keys(GC_Chat *chat, Bin_Unpack *bu)
{
    if (!bin_unpack_array_fixed(bu, 4)) {
        LOGGER_ERROR(chat->log, "Group keys array malformed");
        return false;
    }

    if (!(bin_unpack_bin_fixed(bu, chat->chat_public_key, EXT_PUBLIC_KEY_SIZE)
            && bin_unpack_bin_fixed(bu, chat->chat_secret_key, EXT_SECRET_KEY_SIZE)
            && bin_unpack_bin_fixed(bu, chat->self_public_key, EXT_PUBLIC_KEY_SIZE)
            && bin_unpack_bin_fixed(bu, chat->self_secret_key, EXT_SECRET_KEY_SIZE))) {
        LOGGER_ERROR(chat->log, "Failed to unpack keys");
        return false;
    }

    return true;
}

non_null()
static bool load_unpack_self_info(GC_Chat *chat, Bin_Unpack *bu)
{
    if (!bin_unpack_array_fixed(bu, 4)) {
        LOGGER_ERROR(chat->log, "Group self info array malformed");
        return false;
    }

    uint8_t self_nick[MAX_GC_NICK_SIZE];
    uint16_t self_nick_len = 0;
    uint8_t self_role = GR_USER;
    uint8_t self_status = GS_NONE;

    if (!(bin_unpack_u16(bu, &self_nick_len)
            && bin_unpack_u08(bu, &self_role)
            && bin_unpack_u08(bu, &self_status))) {
        LOGGER_ERROR(chat->log, "Failed to unpack self values");
        return false;
    }

    assert(self_nick_len <= MAX_GC_NICK_SIZE);

    if (!bin_unpack_bin_fixed(bu, self_nick, self_nick_len)) {
        LOGGER_ERROR(chat->log, "Failed to unpack self nick bytes");
        return false;
    }

    // we have to add ourself before setting self info
    if (peer_add(chat, nullptr, chat->self_public_key) != 0) {
        LOGGER_ERROR(chat->log, "Failed to add self to peer list");
        return false;
    }

    assert(chat->numpeers > 0);

    GC_Peer *self = &chat->group[0];

    memcpy(self->gconn.addr.public_key, chat->self_public_key, EXT_PUBLIC_KEY_SIZE);
    memcpy(self->nick, self_nick, self_nick_len);
    self->nick_length = self_nick_len;
    self->role = (Group_Role)self_role;
    self->status = (Group_Peer_Status)self_status;
    self->gconn.confirmed = true;

    return true;
}

non_null()
static bool load_unpack_saved_peers(GC_Chat *chat, Bin_Unpack *bu)
{
    if (!bin_unpack_array_fixed(bu, 2)) {
        LOGGER_ERROR(chat->log, "Group saved peers array malformed");
        return false;
    }

    // Saved peers
    uint16_t saved_peers_size = 0;

    if (!bin_unpack_u16(bu, &saved_peers_size)) {
        LOGGER_ERROR(chat->log, "Failed to unpack saved peers value");
        return false;
    }

    if (saved_peers_size == 0) {
        bin_unpack_nil(bu);
        return true;
    }

    uint8_t *saved_peers = (uint8_t *)malloc(saved_peers_size * GC_SAVED_PEER_SIZE);

    if (saved_peers == nullptr) {
        LOGGER_ERROR(chat->log, "Failed to allocate memory for saved peer list");
        return false;
    }

    if (!bin_unpack_bin_fixed(bu, saved_peers, saved_peers_size)) {
        LOGGER_ERROR(chat->log, "Failed to unpack saved peers binary data");
        free(saved_peers);
        return false;
    }

    if (unpack_gc_saved_peers(chat, saved_peers, saved_peers_size) == -1) {
        LOGGER_ERROR(chat->log, "Failed to unpack saved peers");  // recoverable error
    }

    free(saved_peers);

    return true;
}

bool gc_load_unpack_group(GC_Chat *chat, Bin_Unpack *bu)
{
    if (!bin_unpack_array_fixed(bu, 7)) {
        LOGGER_ERROR(chat->log, "Group info array malformed");
        return false;
    }

    return load_unpack_state_values(chat, bu)
           && load_unpack_state_bin(chat, bu)
           && load_unpack_topic_info(chat, bu)
           && load_unpack_mod_list(chat, bu)
           && load_unpack_keys(chat, bu)
           && load_unpack_self_info(chat, bu)
           && load_unpack_saved_peers(chat, bu);
}

non_null()
static void save_pack_state_values(const GC_Chat *chat, Bin_Pack *bp)
{
    bin_pack_array(bp, 8);
    bin_pack_bool(bp, chat->connection_state == CS_DISCONNECTED); // 1
    bin_pack_u16(bp, chat->shared_state.group_name_len); // 2
    bin_pack_u08(bp, chat->shared_state.privacy_state); // 3
    bin_pack_u16(bp, chat->shared_state.maxpeers); // 4
    bin_pack_u16(bp, chat->shared_state.password_length); // 5
    bin_pack_u32(bp, chat->shared_state.version); // 6
    bin_pack_u32(bp, chat->shared_state.topic_lock); // 7
    bin_pack_u08(bp, chat->shared_state.voice_state); // 8
}

non_null()
static void save_pack_state_bin(const GC_Chat *chat, Bin_Pack *bp)
{
    bin_pack_array(bp, 5);

    bin_pack_bin(bp, chat->shared_state_sig, SIGNATURE_SIZE); // 1
    bin_pack_bin(bp, chat->shared_state.founder_public_key, EXT_PUBLIC_KEY_SIZE); // 2
    bin_pack_bin(bp, chat->shared_state.group_name, chat->shared_state.group_name_len); // 3
    bin_pack_bin(bp, chat->shared_state.password, chat->shared_state.password_length); // 4
    bin_pack_bin(bp, chat->shared_state.mod_list_hash, MOD_MODERATION_HASH_SIZE); // 5
}

non_null()
static void save_pack_topic_info(const GC_Chat *chat, Bin_Pack *bp)
{
    bin_pack_array(bp, 6);

    bin_pack_u32(bp, chat->topic_info.version); // 1
    bin_pack_u16(bp, chat->topic_info.length); // 2
    bin_pack_u16(bp, chat->topic_info.checksum); // 3
    bin_pack_bin(bp, chat->topic_info.topic, chat->topic_info.length); // 4
    bin_pack_bin(bp, chat->topic_info.public_sig_key, SIG_PUBLIC_KEY_SIZE); // 5
    bin_pack_bin(bp, chat->topic_sig, SIGNATURE_SIZE); // 6
}

non_null()
static void save_pack_mod_list(const GC_Chat *chat, Bin_Pack *bp)
{
    bin_pack_array(bp, 2);

    const uint16_t num_mods = min_u16(chat->moderation.num_mods, MOD_MAX_NUM_MODERATORS);

    if (num_mods == 0) {
        bin_pack_u16(bp, num_mods); // 1
        bin_pack_nil(bp); // 2
        return;
    }

    uint8_t *packed_mod_list = (uint8_t *)malloc(num_mods * MOD_LIST_ENTRY_SIZE);

    // we can still recover without the mod list
    if (packed_mod_list == nullptr) {
        bin_pack_u16(bp, 0); // 1
        bin_pack_nil(bp); // 2
        LOGGER_ERROR(chat->log, "Failed to allocate memory for moderation list");
        return;
    }

    bin_pack_u16(bp, num_mods); // 1

    mod_list_pack(&chat->moderation, packed_mod_list);

    const size_t packed_size = num_mods * MOD_LIST_ENTRY_SIZE;

    bin_pack_bin(bp, packed_mod_list, packed_size); // 2

    free(packed_mod_list);
}

non_null()
static void save_pack_keys(const GC_Chat *chat, Bin_Pack *bp)
{
    bin_pack_array(bp, 4);

    bin_pack_bin(bp, chat->chat_public_key, EXT_PUBLIC_KEY_SIZE); // 1
    bin_pack_bin(bp, chat->chat_secret_key, EXT_SECRET_KEY_SIZE); // 2
    bin_pack_bin(bp, chat->self_public_key, EXT_PUBLIC_KEY_SIZE); // 3
    bin_pack_bin(bp, chat->self_secret_key, EXT_SECRET_KEY_SIZE); // 4
}

non_null()
static void save_pack_self_info(const GC_Chat *chat, Bin_Pack *bp)
{
    bin_pack_array(bp, 4);

    const GC_Peer *self = &chat->group[0];

    assert(self->nick_length <= MAX_GC_NICK_SIZE);

    bin_pack_u16(bp, self->nick_length); // 1
    bin_pack_u08(bp, (uint8_t)self->role); // 2
    bin_pack_u08(bp, (uint8_t)self->status); // 3
    bin_pack_bin(bp, self->nick, self->nick_length); // 4
}

non_null()
static void save_pack_saved_peers(const GC_Chat *chat, Bin_Pack *bp)
{
    bin_pack_array(bp, 2);

    uint8_t *saved_peers = (uint8_t *)malloc(GC_MAX_SAVED_PEERS * GC_SAVED_PEER_SIZE);

    // we can still recover without the saved peers list
    if (saved_peers == nullptr) {
        bin_pack_u16(bp, 0); // 1
        bin_pack_nil(bp); // 2
        LOGGER_ERROR(chat->log, "Failed to allocate memory for saved peers list");
        return;
    }

    uint16_t packed_size = 0;
    const int count = pack_gc_saved_peers(chat, saved_peers, GC_MAX_SAVED_PEERS * GC_SAVED_PEER_SIZE, &packed_size);

    if (count < 0) {
        LOGGER_ERROR(chat->log, "Failed to pack saved peers");
    }

    bin_pack_u16(bp, packed_size); // 1

    if (packed_size == 0) {
        bin_pack_nil(bp); // 2
        free(saved_peers);
        return;
    }

    bin_pack_bin(bp, saved_peers, packed_size); // 2

    free(saved_peers);
}

void gc_save_pack_group(const GC_Chat *chat, Bin_Pack *bp)
{
    if (chat->numpeers == 0) {
        LOGGER_ERROR(chat->log, "Failed to pack group: numpeers is 0");
        return;
    }

    bin_pack_array(bp, 7);

#if 0
    // --------------------------------------
    // HINT: dump chat struct with data
#if defined __GNUC__
    #ifdef __MINGW32__
        #define COMPILER "MinGW GCC %d.%d.%d"
    #else
        #define COMPILER "GCC %d.%d.%d"
    #endif

    #if defined(__clang__)
        #define COMP_VERSION __clang_major__, __clang_minor__, __clang_patchlevel__
        #define COMPILER_CLANG "Clang %d.%d.%d"
        printf("  Compiled using " COMPILER_CLANG "\n", COMP_VERSION);
    #else
        #define COMP_VERSION __GNUC__, __GNUC_MINOR__, __GNUC_PATCHLEVEL__
        printf("  Compiled using " COMPILER "\n", COMP_VERSION);
    #endif
#endif

#if defined(__clang__) && (__clang_major__ > 7)
    printf("\n");
    printf("\n");
    printf("========= START =========\n");
    __builtin_dump_struct(chat, &printf);
    printf("=========  END  =========\n");
    printf("\n");
    printf("\n");
#endif
    // HINT: dump chat struct with data
    // ---------------------------------------
#endif

    save_pack_state_values(chat, bp); // 1
    save_pack_state_bin(chat, bp); // 2
    save_pack_topic_info(chat, bp); // 3
    save_pack_mod_list(chat, bp); // 4
    save_pack_keys(chat, bp); // 5
    save_pack_self_info(chat, bp); // 6
    save_pack_saved_peers(chat, bp); // 7
}
