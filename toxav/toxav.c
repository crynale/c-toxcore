/* SPDX-License-Identifier: GPL-3.0-or-later
 * Copyright © 2016-2018 The TokTok team.
 * Copyright © 2013-2015 Tox project.
 */
#ifdef HAVE_CONFIG_H
#include "config.h"
#endif /* HAVE_CONFIG_H */

#include "toxav.h"
#include "../toxcore/tox.h"

#include "msi.h"
#include "rtp.h"
#include "video.h"

#include "../toxcore/logger.h"
#include "../toxcore/util.h"

#include <assert.h>
#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include <pthread.h>

/*
 * Zoff: disable logging in ToxAV for now
 */

#include <stdio.h>

// TODO(zoff99): don't hardcode this, let the application choose it
// VPX Info: Time to spend encoding, in microseconds (it's a *soft* deadline)
#define WANTED_MAX_ENCODER_FPS (40)
#define MAX_ENCODE_TIME_US (1000000 / WANTED_MAX_ENCODER_FPS) // to allow x fps

#define VIDEO_SEND_X_KEYFRAMES_FIRST 7 // force the first n frames to be keyframes!

/*
 * VPX_DL_REALTIME       (1)       deadline parameter analogous to VPx REALTIME mode.
 * VPX_DL_GOOD_QUALITY   (1000000) deadline parameter analogous to VPx GOOD QUALITY mode.
 * VPX_DL_BEST_QUALITY   (0)       deadline parameter analogous to VPx BEST QUALITY mode.
 */

#define VIDEO_ACCEPTABLE_LOSS (0.08f) /* if loss is less than this (8%), then don't do anything */
#define AUDIO_ITERATATIONS_WHILE_VIDEO (2)
#define VIDEO_MIN_SEND_KEYFRAME_INTERVAL 5000

#if defined(AUDIO_DEBUGGING_SKIP_FRAMES)
uint32_t _debug_count_sent_audio_frames = 0;
uint32_t _debug_skip_every_x_audio_frame = 10;
#endif


typedef struct ToxAVCall_s {
    ToxAV *av;

    pthread_mutex_t mutex_audio[1];
    RTPSession *audio_rtp;
    ACSession *audio;

    pthread_mutex_t mutex_video[1];
    RTPSession *video_rtp;
    VCSession *video;

    BWController *bwc;

    uint8_t skip_video_flag;

    bool active;
    MSICall *msi_call;
    uint32_t friend_number;

    uint32_t audio_bit_rate; /* Sending audio bit rate */
    uint32_t video_bit_rate; /* Sending video bit rate */

    uint64_t last_incoming_video_frame_rtimestamp;
    uint64_t last_incoming_video_frame_ltimestamp;

    uint64_t last_incoming_audio_frame_rtimestamp;
    uint64_t last_incoming_audio_frame_ltimestamp;

    uint64_t reference_rtimestamp;
    uint64_t reference_ltimestamp;
    int64_t reference_diff_timestamp;
    uint8_t reference_diff_timestamp_set;

    /** Required for monitoring changes in states */
    uint8_t previous_self_capabilities;

    pthread_mutex_t toxav_call_mutex[1];

    struct ToxAVCall_s *prev;
    struct ToxAVCall_s *next;
} ToxAVCall;

struct ToxAV {
    Tox *tox;
    MSISession *msi;

    /* Two-way storage: first is array of calls and second is list of calls with head and tail */
    ToxAVCall **calls;
    uint32_t calls_tail;
    uint32_t calls_head;
    pthread_mutex_t mutex[1];

    PAIR(toxav_call_cb *, void *) ccb; /* Call callback */
    PAIR(toxav_call_state_cb *, void *) scb; /* Call state callback */
    PAIR(toxav_audio_receive_frame_cb *, void *) acb; /* Audio frame receive callback */
    PAIR(toxav_video_receive_frame_cb *, void *) vcb; /* Video frame receive callback */
    PAIR(toxav_bit_rate_status_cb *, void *) bcb; /* Bit rate control callback */

    /** Decode time measures */
    int32_t dmssc; /** Measure count */
    int32_t dmsst; /** Last cycle total */
    int32_t dmssa; /** Average decoding time in ms */

    uint32_t interval; /** Calculated interval */

    Mono_Time *toxav_mono_time; // ToxAV's own mono_time instance
};

static void callback_bwc(BWController *bwc, uint32_t friend_number, float loss, void *user_data);

static int callback_invite(void *toxav_inst, MSICall *call);
static int callback_start(void *toxav_inst, MSICall *call);
static int callback_end(void *toxav_inst, MSICall *call);
static int callback_error(void *toxav_inst, MSICall *call);
static int callback_capabilites(void *toxav_inst, MSICall *call);

static bool audio_bit_rate_invalid(uint32_t bit_rate);
static bool video_bit_rate_invalid(uint32_t bit_rate);
static bool invoke_call_state_callback(ToxAV *av, uint32_t friend_number, uint32_t state);
static ToxAVCall *call_new(ToxAV *av, uint32_t friend_number, Toxav_Err_Call *error);
static ToxAVCall *call_remove(ToxAVCall *call);
static bool call_prepare_transmission(ToxAVCall *call);
static void call_kill_transmission(ToxAVCall *call);

MSISession *tox_av_msi_get(ToxAV *av);
int toxav_friend_exists(const Tox *tox, int32_t friendnumber);
Mono_Time *toxav_get_av_mono_time(ToxAV *toxav);
ToxAVCall *call_get(ToxAV *av, uint32_t friend_number);
RTPSession *rtp_session_get(void *call, int payload_type);

MSISession *tox_av_msi_get(ToxAV *av)
{
    if (!av) {
        return nullptr;
    }

    return av->msi;
}

ToxAVCall *call_get(ToxAV *av, uint32_t friend_number)
{
    if (av == nullptr) {
        return nullptr;
    }

    /* Assumes mutex locked */
    if (av->calls == nullptr || av->calls_tail < friend_number) {
        return nullptr;
    }

    return av->calls[friend_number];
}

RTPSession *rtp_session_get(void *call, int payload_type)
{
    if (((ToxAVCall *)call) == nullptr) {
        return nullptr;
    }

    if (payload_type == RTP_TYPE_VIDEO) {
        return ((ToxAVCall *)call)->video_rtp;
    } else {
        return ((ToxAVCall *)call)->audio_rtp;
    }

    return nullptr;
}

ToxAV *toxav_new(Tox *tox, Toxav_Err_New *error)
{
    TOXAV_ERR_NEW rc = TOXAV_ERR_NEW_OK;
    ToxAV *av = NULL;
    Messenger *m = (Messenger *)tox;

    if (tox == NULL) {
        rc = TOXAV_ERR_NEW_NULL;
        goto RETURN;
    }

    av = (ToxAV *)calloc(sizeof(ToxAV), 1);

    if (av == nullptr) {
        rc = TOXAV_ERR_NEW_MALLOC;
        goto RETURN;
    }

    if (create_recursive_mutex(av->mutex) != 0) {
        rc = TOXAV_ERR_NEW_MALLOC;
        goto RETURN;
    }

    av->tox = tox;
    av->msi = msi_new(av->tox);

    av->toxav_mono_time = mono_time_new();

    if (av->msi == NULL) {
        pthread_mutex_destroy(av->mutex);
        rc = TOXAV_ERR_NEW_MALLOC;
        goto RETURN;
    }

    av->interval = 200;
    av->msi->av = av;

    // save Tox object into toxcore
    tox_set_av_object(av->tox, (void *)av);

    msi_register_callback(av->msi, callback_invite, MSI_ON_INVITE);
    msi_register_callback(av->msi, callback_start, MSI_ON_START);
    msi_register_callback(av->msi, callback_end, MSI_ON_END);
    msi_register_callback(av->msi, callback_error, MSI_ON_ERROR);
    msi_register_callback(av->msi, callback_error, MSI_ON_PEERTIMEOUT);
    msi_register_callback(av->msi, callback_capabilites, MSI_ON_CAPABILITIES);

RETURN:

    if (error) {
        *error = rc;
    }

    if (rc != TOXAV_ERR_NEW_OK) {
        free(av);
        av = NULL;
    }

    return av;
}

void toxav_kill(ToxAV *av)
{
    if (av == NULL) {
        return;
    }

    pthread_mutex_lock(av->mutex);

    // unregister callbacks
    for (uint8_t i = PACKET_ID_RANGE_LOSSY_AV_START; i <= PACKET_ID_RANGE_LOSSY_AV_END; ++i) {
        tox_callback_friend_lossy_packet_per_pktid(av->tox, nullptr, i);
    }

    /* To avoid possible deadlocks */
    while (av->msi && msi_kill(av->tox, av->msi, nullptr) != 0) {
        pthread_mutex_unlock(av->mutex);
        pthread_mutex_lock(av->mutex);
    }

    /* Msi kill will hang up all calls so just clean these calls */
    if (av->calls) {
        ToxAVCall *it = call_get(av, av->calls_head);

        while (it) {
            call_kill_transmission(it);
            it->msi_call = NULL; /* msi_kill() frees the call's msi_call handle; which causes #278 */
            it = call_remove(it); /* This will eventually free av->calls */
        }
    }

    mono_time_free(av->toxav_mono_time);

    pthread_mutex_unlock(av->mutex);
    pthread_mutex_destroy(av->mutex);

    free(av);
}

Tox *toxav_get_tox(const ToxAV *av)
{
    return (Tox *) av->m;
}

uint32_t toxav_iteration_interval(const ToxAV *av)
{
    /* If no call is active interval is 200 */
    return av->calls ? av->interval : 200;
}

void toxav_iterate(ToxAV *av)
{
    pthread_mutex_lock(av->mutex);

    if (av->calls == NULL) {
        pthread_mutex_unlock(av->mutex);
        return;
    }

    uint64_t start = current_time_monotonic(av->toxav_mono_time);
    int32_t rc = 500;
    uint32_t audio_iterations = 0;

    ToxAVCall *i = av->calls[av->calls_head];

    for (; i; i = i->next) {

        audio_iterations = 0;

        if (i->active) {
            pthread_mutex_lock(i->toxav_call_mutex);
            pthread_mutex_unlock(av->mutex);

            uint32_t fid = i->friend_number;

            ac_iterate(i->audio);
            vc_iterate(i->video);




            // LOGGER_WARNING(av->m->log, "XXXXXXXXXXXXXXXXXX=================");
            if (i->msi_call->self_capabilities & msi_CapRAudio &&
                    i->msi_call->peer_capabilities & msi_CapSAudio) {
                // use 4ms less than the actual audio frame duration, to have still some time left
                // LOGGER_WARNING(av->m->log, "lp_frame_duration=%d", (int)i->audio.second->lp_frame_duration);
                rc = MIN((i->audio.second->lp_frame_duration - 4), rc);
            }

            if (i->msi_call->self_capabilities & msi_CapRVideo &&
                    i->msi_call->peer_capabilities & msi_CapSVideo) {
                // LOGGER_WARNING(av->m->log, "lcfd=%d", (int)i->video.second->lcfd);
                rc = MIN(i->video.second->lcfd, (uint32_t) rc);
            }

            pthread_mutex_unlock(i->mutex);
            pthread_mutex_lock(av->mutex);

            /* In case this call is popped from container stop iteration */
            if (call_get(av, fid) != i) {
                break;
            }

            if ((i->last_incoming_audio_frame_ltimestamp != 0)
                    &&
                    (i->last_incoming_video_frame_ltimestamp != 0)) {
                if (i->reference_diff_timestamp_set == 0) {
                    i->reference_rtimestamp = i->last_incoming_audio_frame_rtimestamp;
                    i->reference_ltimestamp = i->last_incoming_audio_frame_ltimestamp;
                    // this is the difference between local and remote clocks in "ms"
                    i->reference_diff_timestamp = (int64_t)(i->reference_ltimestamp - i->reference_rtimestamp);
                    i->reference_diff_timestamp_set = 1;
                } else {
                    int64_t latency_ms = (int64_t)(
                                             (i->last_incoming_video_frame_rtimestamp - i->last_incoming_audio_frame_rtimestamp) -
                                             (i->last_incoming_video_frame_ltimestamp - i->last_incoming_audio_frame_ltimestamp)
                                         );


                    LOGGER_DEBUG(av->m->log, "VIDEO:delay-to-audio-in-ms=%lld", (long long)latency_ms);
                    // LOGGER_INFO(av->m->log, "CLOCK:delay-to-rmote-in-ms=%lld", (long long)(i->reference_diff_timestamp));
                    LOGGER_DEBUG(av->m->log, "VIDEO:delay-to-refnc-in-ms=%lld",
                                 (long long) - ((i->last_incoming_video_frame_ltimestamp - i->reference_diff_timestamp) -
                                                i->last_incoming_video_frame_rtimestamp));
                    LOGGER_DEBUG(av->m->log, "AUDIO:delay-to-refnc-in-ms=%lld",
                                 (long long) - ((i->last_incoming_audio_frame_ltimestamp - i->reference_diff_timestamp) -
                                                i->last_incoming_audio_frame_rtimestamp));

                    // LOGGER_INFO(av->m->log, "VIDEO latency in ms=%lld", (long long)(i->last_incoming_video_frame_ltimestamp - i->last_incoming_video_frame_rtimestamp));
                    // LOGGER_INFO(av->m->log, "AUDIO latency in ms=%lld", (long long)(i->last_incoming_audio_frame_ltimestamp - i->last_incoming_audio_frame_rtimestamp));

                    // LOGGER_INFO(av->m->log, "VIDEO:3-latency-in-ms=%lld", (long long)(i->last_incoming_audio_frame_rtimestamp - i->last_incoming_video_frame_rtimestamp));

                    //LOGGER_INFO(av->m->log, "AUDIO (to video):latency in a=%lld b=%lld c=%lld d=%lld",
                    //(long long)i->last_incoming_video_frame_rtimestamp,
                    //(long long)i->last_incoming_audio_frame_rtimestamp,
                    //(long long)i->last_incoming_video_frame_ltimestamp,
                    //(long long)i->last_incoming_audio_frame_ltimestamp
                    //);
                }
            }
        }
    }

    av->interval = rc < av->dmssa ? 0 : (rc - av->dmssa);
    av->dmsst += current_time_monotonic(av->toxav_mono_time) - start;

    if (++av->dmssc == 3) {
        av->dmssa = av->dmsst / 3 + 5; /* NOTE Magic Offset 5 for precision */
        av->dmssc = 0;
        av->dmsst = 0;
    }

    pthread_mutex_unlock(av->mutex);
}

bool toxav_call(ToxAV *av, uint32_t friend_number, uint32_t audio_bit_rate, uint32_t video_bit_rate,
                Toxav_Err_Call *error)
{
    Toxav_Err_Call rc = TOXAV_ERR_CALL_OK;
    ToxAVCall *call;

    pthread_mutex_lock(av->mutex);

    if ((audio_bit_rate && audio_bit_rate_invalid(audio_bit_rate))
            || (video_bit_rate && video_bit_rate_invalid(video_bit_rate))) {
        rc = TOXAV_ERR_CALL_INVALID_BIT_RATE;
        goto RETURN;
    }

    call = call_new(av, friend_number, &rc);

    if (call == NULL) {
        goto END;
    }

    call->audio_bit_rate = audio_bit_rate;
    call->video_bit_rate = video_bit_rate;

    call->previous_self_capabilities = MSI_CAP_R_AUDIO | MSI_CAP_R_VIDEO;

    call->previous_self_capabilities |= audio_bit_rate > 0 ? MSI_CAP_S_AUDIO : 0;
    call->previous_self_capabilities |= video_bit_rate > 0 ? MSI_CAP_S_VIDEO : 0;

    if (msi_invite(av->msi, &call->msi_call, friend_number, call->previous_self_capabilities) != 0) {
        call_remove(call);
        rc = TOXAV_ERR_CALL_SYNC;
        goto RETURN;
    }

    call->msi_call->av_call = call;

RETURN:
    pthread_mutex_unlock(av->mutex);

    if (error) {
        *error = rc;
    }

    return rc == TOXAV_ERR_CALL_OK;
}

void toxav_callback_call(ToxAV *av, toxav_call_cb *callback, void *user_data)
{
    pthread_mutex_lock(av->mutex);
    av->ccb = callback;
    av->ccb_user_data = user_data;
    pthread_mutex_unlock(av->mutex);
}

int toxav_friend_exists(const Tox *tox, int32_t friendnumber)
{
    if (tox) {
        bool res = tox_friend_exists(tox, friendnumber);

        if (res) {
            return 1;
        } else {
            return 0;
        }
    }

    return 0;
}

bool toxav_answer(ToxAV *av, uint32_t friend_number, uint32_t audio_bit_rate, uint32_t video_bit_rate,
                  Toxav_Err_Answer *error)
{
    pthread_mutex_lock(av->mutex);

    Toxav_Err_Answer rc = TOXAV_ERR_ANSWER_OK;
    ToxAVCall *call;

    if (toxav_friend_exists(av->tox, friend_number) == 0) {
        rc = TOXAV_ERR_ANSWER_FRIEND_NOT_FOUND;
        goto RETURN;
    }

    if ((audio_bit_rate && audio_bit_rate_invalid(audio_bit_rate))
            || (video_bit_rate && video_bit_rate_invalid(video_bit_rate))
       ) {
        rc = TOXAV_ERR_ANSWER_INVALID_BIT_RATE;
        goto RETURN;
    }

    call = call_get(av, friend_number);

    if (call == NULL) {
        rc = TOXAV_ERR_ANSWER_FRIEND_NOT_CALLING;
        goto RETURN;
    }

    if (!call_prepare_transmission(call)) {
        rc = TOXAV_ERR_ANSWER_CODEC_INITIALIZATION;
        goto RETURN;
    }

    call->audio_bit_rate = audio_bit_rate;
    call->video_bit_rate = video_bit_rate;

    call->previous_self_capabilities = MSI_CAP_R_AUDIO | MSI_CAP_R_VIDEO;

    call->previous_self_capabilities |= audio_bit_rate > 0 ? MSI_CAP_S_AUDIO : 0;
    call->previous_self_capabilities |= video_bit_rate > 0 ? MSI_CAP_S_VIDEO : 0;

    if (msi_answer(call->msi_call, call->previous_self_capabilities) != 0) {
        rc = TOXAV_ERR_ANSWER_SYNC;
    }

RETURN:
    pthread_mutex_unlock(av->mutex);

    if (error) {
        *error = rc;
    }

    return rc == TOXAV_ERR_ANSWER_OK;
}

void toxav_callback_call_state(ToxAV *av, toxav_call_state_cb *callback, void *user_data)
{
    pthread_mutex_lock(av->mutex);
    av->scb = callback;
    av->scb_user_data = user_data;
    pthread_mutex_unlock(av->mutex);
}

bool toxav_call_control(ToxAV *av, uint32_t friend_number, Toxav_Call_Control control, Toxav_Err_Call_Control *error)
{
    pthread_mutex_lock(av->mutex);
    Toxav_Err_Call_Control rc = TOXAV_ERR_CALL_CONTROL_OK;
    ToxAVCall *call;

    if (toxav_friend_exists(av->tox, friend_number) == 0) {
        rc = TOXAV_ERR_CALL_CONTROL_FRIEND_NOT_FOUND;
        goto RETURN;
    }

    call = call_get(av, friend_number);

    if (call == NULL || (!call->active && control != TOXAV_CALL_CONTROL_CANCEL)) {
        rc = TOXAV_ERR_CALL_CONTROL_FRIEND_NOT_IN_CALL;
        goto RETURN;
    }

    switch (control) {
        case TOXAV_CALL_CONTROL_RESUME: {
            /* Only act if paused and had media transfer active before */
            if (call->msi_call->self_capabilities == 0 &&
                    call->previous_self_capabilities) {

                if (msi_change_capabilities(call->msi_call,
                                            call->previous_self_capabilities) == -1) {
                    rc = TOXAV_ERR_CALL_CONTROL_SYNC;
                    goto RETURN;
                }

                rtp_allow_receiving(av->tox, call->audio_rtp);
                rtp_allow_receiving(av->tox, call->video_rtp);
            } else {
                rc = TOXAV_ERR_CALL_CONTROL_INVALID_TRANSITION;
                goto RETURN;
            }
        }
        break;

        case TOXAV_CALL_CONTROL_PAUSE: {
            /* Only act if not already paused */
            if (call->msi_call->self_capabilities) {
                call->previous_self_capabilities = call->msi_call->self_capabilities;

                if (msi_change_capabilities(call->msi_call, 0) == -1) {
                    rc = TOXAV_ERR_CALL_CONTROL_SYNC;
                    goto RETURN;
                }

                rtp_stop_receiving(av->tox, call->audio_rtp);
                rtp_stop_receiving(av->tox, call->video_rtp);
            } else {
                rc = TOXAV_ERR_CALL_CONTROL_INVALID_TRANSITION;
                goto RETURN;
            }
        }
        break;

        case TOXAV_CALL_CONTROL_CANCEL: {
            /* Hang up */
            pthread_mutex_lock(call->toxav_call_mutex);

            if (msi_hangup(call->msi_call) != 0) {
                rc = TOXAV_ERR_CALL_CONTROL_SYNC;
                pthread_mutex_unlock(call->toxav_call_mutex);
                goto RETURN;
            }

            call->msi_call = NULL;
            pthread_mutex_unlock(call->mutex);

            /* No mather the case, terminate the call */
            call_kill_transmission(call);
            call_remove(call);
        }
        break;

        case TOXAV_CALL_CONTROL_MUTE_AUDIO: {
            if (call->msi_call->self_capabilities & MSI_CAP_R_AUDIO) {
                if (msi_change_capabilities(call->msi_call, call->
                                            msi_call->self_capabilities ^ MSI_CAP_R_AUDIO) == -1) {
                    rc = TOXAV_ERR_CALL_CONTROL_SYNC;
                    goto RETURN;
                }

                rtp_stop_receiving(av->tox, call->audio_rtp);
            } else {
                rc = TOXAV_ERR_CALL_CONTROL_INVALID_TRANSITION;
                goto RETURN;
            }
        }
        break;

        case TOXAV_CALL_CONTROL_UNMUTE_AUDIO: {
            if (call->msi_call->self_capabilities ^ MSI_CAP_R_AUDIO) {
                if (msi_change_capabilities(call->msi_call, call->
                                            msi_call->self_capabilities | MSI_CAP_R_AUDIO) == -1) {
                    rc = TOXAV_ERR_CALL_CONTROL_SYNC;
                    goto RETURN;
                }

                rtp_allow_receiving(av->tox, call->audio_rtp);
            } else {
                rc = TOXAV_ERR_CALL_CONTROL_INVALID_TRANSITION;
                goto RETURN;
            }
        }
        break;

        case TOXAV_CALL_CONTROL_HIDE_VIDEO: {
            if (call->msi_call->self_capabilities & MSI_CAP_R_VIDEO) {
                if (msi_change_capabilities(call->msi_call, call->
                                            msi_call->self_capabilities ^ MSI_CAP_R_VIDEO) == -1) {
                    rc = TOXAV_ERR_CALL_CONTROL_SYNC;
                    goto RETURN;
                }

                rtp_stop_receiving(av->tox, call->video_rtp);
            } else {
                rc = TOXAV_ERR_CALL_CONTROL_INVALID_TRANSITION;
                goto RETURN;
            }
        }
        break;

        case TOXAV_CALL_CONTROL_SHOW_VIDEO: {
            if (call->msi_call->self_capabilities ^ MSI_CAP_R_VIDEO) {
                if (msi_change_capabilities(call->msi_call, call->
                                            msi_call->self_capabilities | MSI_CAP_R_VIDEO) == -1) {
                    rc = TOXAV_ERR_CALL_CONTROL_SYNC;
                    goto RETURN;
                }

                rtp_allow_receiving(av->tox, call->video_rtp);
            } else {
                rc = TOXAV_ERR_CALL_CONTROL_INVALID_TRANSITION;
                goto RETURN;
            }
        }
        break;
    }

RETURN:
    pthread_mutex_unlock(av->mutex);

    if (error) {
        *error = rc;
    }

    return rc == TOXAV_ERR_CALL_CONTROL_OK;
}

bool toxav_audio_set_bit_rate(ToxAV *av, uint32_t friend_number, uint32_t audio_bit_rate,
                              Toxav_Err_Bit_Rate_Set *error)
{
    Toxav_Err_Bit_Rate_Set rc = TOXAV_ERR_BIT_RATE_SET_OK;
    ToxAVCall *call;

    if (toxav_friend_exists(av->tox, friend_number) == 0) {
        rc = TOXAV_ERR_BIT_RATE_SET_FRIEND_NOT_FOUND;
        goto RETURN;
    }

    if (audio_bit_rate > 0 && audio_bit_rate_invalid(audio_bit_rate)) {
        rc = TOXAV_ERR_BIT_RATE_SET_INVALID_AUDIO_BIT_RATE;
        goto END;
    }

    if (video_bit_rate > 0 && video_bit_rate_invalid(video_bit_rate)) {
        rc = TOXAV_ERR_BIT_RATE_SET_INVALID_VIDEO_BIT_RATE;
        goto END;
    }

    pthread_mutex_lock(av->mutex);
    call = call_get(av, friend_number);

    if (call == NULL || !call->active || call->msi_call->state != msi_CallActive) {
        pthread_mutex_unlock(av->mutex);
        rc = TOXAV_ERR_BIT_RATE_SET_FRIEND_NOT_IN_CALL;
        goto RETURN;
    }

    LOGGER_API_DEBUG(av->tox, "Setting new audio bitrate to: %d", audio_bit_rate);

    if (call->audio_bit_rate == audio_bit_rate) {
        LOGGER_API_DEBUG(av->tox, "Audio bitrate already set to: %d", audio_bit_rate);
    } else if (audio_bit_rate == 0) {
        LOGGER_API_DEBUG(av->tox, "Turned off audio sending");

        if (msi_change_capabilities(call->msi_call, call->msi_call->
                                    self_capabilities ^ MSI_CAP_S_AUDIO) != 0) {
            pthread_mutex_unlock(av->mutex);
            rc = TOXAV_ERR_BIT_RATE_SET_SYNC;
            goto RETURN;
        }

        /* Audio sending is turned off; notify peer */
        call->audio_bit_rate = 0;
    } else {
        pthread_mutex_lock(call->toxav_call_mutex);

        if (call->audio_bit_rate == 0) {
            LOGGER_API_DEBUG(av->tox, "Turned on audio sending");

            /* The audio has been turned off before this */
            if (msi_change_capabilities(call->msi_call, call->
                                        msi_call->self_capabilities | MSI_CAP_S_AUDIO) != 0) {
                pthread_mutex_unlock(call->toxav_call_mutex);
                pthread_mutex_unlock(av->mutex);
                rc = TOXAV_ERR_BIT_RATE_SET_SYNC;
                goto RETURN;
            }
        } else {
            LOGGER_API_DEBUG(av->tox, "Set new audio bit rate %d", audio_bit_rate);
        }

        call->audio_bit_rate = audio_bit_rate;
        pthread_mutex_unlock(call->toxav_call_mutex);
    }

    pthread_mutex_unlock(av->mutex);
RETURN:

    if (error) {
        *error = rc;
    }

    return rc == TOXAV_ERR_BIT_RATE_SET_OK;
}

bool toxav_video_set_bit_rate(ToxAV *av, uint32_t friend_number, uint32_t video_bit_rate,
                              Toxav_Err_Bit_Rate_Set *error)
{
    Toxav_Err_Bit_Rate_Set rc = TOXAV_ERR_BIT_RATE_SET_OK;
    ToxAVCall *call;

    if (toxav_friend_exists(av->tox, friend_number) == 0) {
        rc = TOXAV_ERR_BIT_RATE_SET_FRIEND_NOT_FOUND;
        goto RETURN;
    }

            /* Audio sending is turned off; notify peer */
            call->audio_bit_rate = 0;
        } else {
            pthread_mutex_lock(call->mutex);

    LOGGER_API_DEBUG(av->tox, "Setting new video bitrate to: %d", video_bit_rate);

    if (call->video_bit_rate == video_bit_rate) {
        LOGGER_API_DEBUG(av->tox, "Video bitrate already set to: %d", video_bit_rate);
    } else if (video_bit_rate == 0) {
        LOGGER_API_DEBUG(av->tox, "Turned off video sending");

            call->audio_bit_rate = audio_bit_rate;
            pthread_mutex_unlock(call->mutex);
        }
    }

    if (video_bit_rate >= 0) {
        LOGGER_DEBUG(av->m->log, "Setting new video bitrate to: %d", video_bit_rate);

        if (call->video_bit_rate == 0) {
            LOGGER_API_DEBUG(av->tox, "Turned on video sending");

            /* Video sending is turned off; notify peer */
            if (msi_change_capabilities(call->msi_call, call->msi_call->
                                        self_capabilities ^ msi_CapSVideo) != 0) {
                pthread_mutex_unlock(av->mutex);
                rc = TOXAV_ERR_BIT_RATE_SET_SYNC;
                goto RETURN;
            }

            call->video_bit_rate = 0;
        } else {
            LOGGER_API_DEBUG(av->tox, "Set new video bit rate %d", video_bit_rate);
        }

            call->video_bit_rate = video_bit_rate;
            pthread_mutex_unlock(call->mutex);
        }
    }

    pthread_mutex_unlock(av->mutex);
RETURN:

    if (error) {
        *error = rc;
    }

    return rc == TOXAV_ERR_BIT_RATE_SET_OK;
}

void toxav_callback_audio_bit_rate(ToxAV *av, toxav_audio_bit_rate_cb *callback, void *user_data)
{
    pthread_mutex_lock(av->mutex);
    av->abcb = callback;
    av->abcb_user_data = user_data;
    pthread_mutex_unlock(av->mutex);
}

void toxav_callback_video_bit_rate(ToxAV *av, toxav_video_bit_rate_cb *callback, void *user_data)
{
    pthread_mutex_lock(av->mutex);
    av->vbcb = callback;
    av->vbcb_user_data = user_data;
    pthread_mutex_unlock(av->mutex);
}

bool toxav_audio_send_frame(ToxAV *av, uint32_t friend_number, const int16_t *pcm, size_t sample_count,
                            uint8_t channels, uint32_t sampling_rate, Toxav_Err_Send_Frame *error)
{
    Toxav_Err_Send_Frame rc = TOXAV_ERR_SEND_FRAME_OK;
    ToxAVCall *call;

    if (toxav_friend_exists(av->tox, friend_number) == 0) {
        rc = TOXAV_ERR_SEND_FRAME_FRIEND_NOT_FOUND;
        goto RETURN;
    }

    if (pthread_mutex_trylock(av->mutex) != 0) {
        rc = TOXAV_ERR_SEND_FRAME_SYNC;
        goto RETURN;
    }

    call = call_get(av, friend_number);

    if (call == NULL || !call->active || call->msi_call->state != msi_CallActive) {
        pthread_mutex_unlock(av->mutex);
        rc = TOXAV_ERR_SEND_FRAME_FRIEND_NOT_IN_CALL;
        goto RETURN;
    }

    if (call->audio_bit_rate == 0 ||
            !(call->msi_call->self_capabilities & MSI_CAP_S_AUDIO) ||
            !(call->msi_call->peer_capabilities & MSI_CAP_R_AUDIO)) {
        pthread_mutex_unlock(av->mutex);
        rc = TOXAV_ERR_SEND_FRAME_PAYLOAD_TYPE_DISABLED;
        goto RETURN;
    }

    pthread_mutex_lock(call->mutex_audio);
    pthread_mutex_unlock(av->mutex);

    if (pcm == NULL) {
        pthread_mutex_unlock(call->mutex_audio);
        rc = TOXAV_ERR_SEND_FRAME_NULL;
        goto RETURN;
    }

    if (channels > 2) {
        pthread_mutex_unlock(call->mutex_audio);
        rc = TOXAV_ERR_SEND_FRAME_INVALID;
        goto RETURN;
    }

    {   /* Encode and send */
        if (ac_reconfigure_encoder(call->audio, call->audio_bit_rate * 1000, sampling_rate, channels) != 0) {
            pthread_mutex_unlock(call->mutex_audio);
            LOGGER_WARNING(av->m->log, "Failed reconfigure audio encoder");
            rc = TOXAV_ERR_SEND_FRAME_INVALID;
            goto RETURN;
        }

        VLA(uint8_t, dest, sample_count + sizeof(sampling_rate)); /* This is more than enough always */

        sampling_rate = net_htonl(sampling_rate);
        memcpy(dest, &sampling_rate, sizeof(sampling_rate));
        int vrc = opus_encode(call->audio->encoder, pcm, sample_count,
                              dest + sizeof(sampling_rate), SIZEOF_VLA(dest) - sizeof(sampling_rate));

        if (vrc < 0) {
            LOGGER_API_WARNING(av->tox, "Failed to encode frame %s", opus_strerror(vrc));
            pthread_mutex_unlock(call->mutex_audio);
            rc = TOXAV_ERR_SEND_FRAME_INVALID;
            goto RETURN;
        }

        if (rtp_send_data(call->audio_rtp, dest, vrc + sizeof(sampling_rate), false, nullptr) != 0) {
            LOGGER_API_WARNING(av->tox, "Failed to send audio packet");
            rc = TOXAV_ERR_SEND_FRAME_RTP_FAILED;
        }

#endif

    }


    pthread_mutex_unlock(call->mutex_audio);

RETURN:

    if (error) {
        *error = rc;
    }

    return rc == TOXAV_ERR_SEND_FRAME_OK;
}

static Toxav_Err_Send_Frame send_frames(ToxAV *av, ToxAVCall *call)
{
    vpx_codec_iter_t iter = nullptr;

    for (const vpx_codec_cx_pkt_t *pkt = vpx_codec_get_cx_data(call->video->encoder, &iter);
            pkt != nullptr;
            pkt = vpx_codec_get_cx_data(call->video->encoder, &iter)) {
        if (pkt->kind != VPX_CODEC_CX_FRAME_PKT) {
            continue;
        }

        const bool is_keyframe = (pkt->data.frame.flags & VPX_FRAME_IS_KEY) != 0;

        // https://www.webmproject.org/docs/webm-sdk/structvpx__codec__cx__pkt.html
        // pkt->data.frame.sz -> size_t
        const uint32_t frame_length_in_bytes = pkt->data.frame.sz;

        const int res = rtp_send_data(
                            call->video_rtp,
                            (const uint8_t *)pkt->data.frame.buf,
                            frame_length_in_bytes,
                            is_keyframe,
                            nullptr);

        if (res < 0) {
            LOGGER_API_WARNING(av->tox, "Could not send video frame: %s", strerror(errno));
            return TOXAV_ERR_SEND_FRAME_RTP_FAILED;
        }
    }

    return TOXAV_ERR_SEND_FRAME_OK;
}

bool toxav_video_send_frame(ToxAV *av, uint32_t friend_number, uint16_t width, uint16_t height, const uint8_t *y,
                            const uint8_t *u, const uint8_t *v, Toxav_Err_Send_Frame *error)
{
    Toxav_Err_Send_Frame rc = TOXAV_ERR_SEND_FRAME_OK;
    ToxAVCall *call;

    int vpx_encode_flags = 0;

    if (toxav_friend_exists(av->tox, friend_number) == 0) {
        rc = TOXAV_ERR_SEND_FRAME_FRIEND_NOT_FOUND;
        goto RETURN;
    }

    if (pthread_mutex_trylock(av->mutex) != 0) {
        rc = TOXAV_ERR_SEND_FRAME_SYNC;
        goto RETURN;
    }

    call = call_get(av, friend_number);

    if (call == NULL || !call->active || call->msi_call->state != msi_CallActive) {
        pthread_mutex_unlock(av->mutex);
        rc = TOXAV_ERR_SEND_FRAME_FRIEND_NOT_IN_CALL;
        goto RETURN;
    }

    uint64_t ms_to_last_frame = 1;

    if (call->video.second) {
        ms_to_last_frame = current_time_monotonic() - call->video.second->last_encoded_frame_ts;

        if (call->video.second->last_encoded_frame_ts == 0) {
            ms_to_last_frame = 1;
        }
    }

    if (call->video_bit_rate == 0 ||
            !(call->msi_call->self_capabilities & MSI_CAP_S_VIDEO) ||
            !(call->msi_call->peer_capabilities & MSI_CAP_R_VIDEO)) {
        pthread_mutex_unlock(av->mutex);
        rc = TOXAV_ERR_SEND_FRAME_PAYLOAD_TYPE_DISABLED;
        goto RETURN;
    }

    pthread_mutex_lock(call->mutex_video);
    pthread_mutex_unlock(av->mutex);

    if (y == NULL || u == NULL || v == NULL) {
        pthread_mutex_unlock(call->mutex_video);
        rc = TOXAV_ERR_SEND_FRAME_NULL;
        goto RETURN;
    }

    if ((call->video.second->video_encoder_coded_used == TOXAV_ENCODER_CODEC_USED_VP8)
            || (call->video.second->video_encoder_coded_used == TOXAV_ENCODER_CODEC_USED_VP9)) {

        if (vc_reconfigure_encoder(av->m->log, call->video.second, call->video_bit_rate * 1000,
                                   width, height, -1) != 0) {
            pthread_mutex_unlock(call->mutex_video);
            rc = TOXAV_ERR_SEND_FRAME_INVALID;
            goto END;
        }
    } else {
        // HINT: H264
        if (vc_reconfigure_encoder(av->m->log, call->video.second, call->video_bit_rate * 1000,
                                   width, height, -1) != 0) {
            pthread_mutex_unlock(call->mutex_video);
            rc = TOXAV_ERR_SEND_FRAME_INVALID;
            goto END;
        }
    }

    if (call->video_rtp->ssrc < VIDEO_SEND_X_KEYFRAMES_FIRST) {
        // Key frame flag for first frames
        vpx_encode_flags = VPX_EFLAG_FORCE_KF;
        LOGGER_API_INFO(av->tox, "I_FRAME_FLAG:%d only-i-frame mode", call->video_rtp->ssrc);

        ++call->video_rtp->ssrc;
    } else if (call->video_rtp->ssrc == VIDEO_SEND_X_KEYFRAMES_FIRST) {
        // normal keyframe placement
        vpx_encode_flags = 0;
        LOGGER_API_INFO(av->tox, "I_FRAME_FLAG:%d normal mode", call->video_rtp->ssrc);

        ++call->video_rtp->ssrc;
    }
    else
    {
#ifdef VIDEO_ENCODER_SOFT_DEADLINE_AUTOTUNE
    long encode_time_auto_tune = MAX_ENCODE_TIME_US;

    if (call->video.second->last_encoded_frame_ts > 0) {
        encode_time_auto_tune = (current_time_monotonic() - call->video.second->last_encoded_frame_ts) * 1000;
#ifdef VIDEO_CODEC_ENCODER_USE_FRAGMENTS
        encode_time_auto_tune = encode_time_auto_tune * VIDEO_CODEC_FRAGMENT_NUMS;
#endif

        if (encode_time_auto_tune == 0) {
            // if the real delay was 0ms then still use 1ms
            encode_time_auto_tune = 1;
        }

        if (call->video.second->encoder_soft_deadline[call->video.second->encoder_soft_deadline_index] == 0) {
            call->video.second->encoder_soft_deadline[call->video.second->encoder_soft_deadline_index] = 1;
            LOGGER_DEBUG(av->m->log, "AUTOTUNE: delay=[1]");
        } else {
            call->video.second->encoder_soft_deadline[call->video.second->encoder_soft_deadline_index] = encode_time_auto_tune;
            LOGGER_DEBUG(av->m->log, "AUTOTUNE: delay=%d", (int)encode_time_auto_tune);
        }

        call->video.second->encoder_soft_deadline_index = (call->video.second->encoder_soft_deadline_index + 1) %
                VIDEO_ENCODER_SOFT_DEADLINE_AUTOTUNE_ENTRIES;

        // calc mean value
        encode_time_auto_tune = 0;

        for (int k = 0; k < VIDEO_ENCODER_SOFT_DEADLINE_AUTOTUNE_ENTRIES; k++) {
            encode_time_auto_tune = encode_time_auto_tune + call->video.second->encoder_soft_deadline[k];
        }

        encode_time_auto_tune = encode_time_auto_tune / VIDEO_ENCODER_SOFT_DEADLINE_AUTOTUNE_ENTRIES;

        if (encode_time_auto_tune > (1000000 / VIDEO_ENCODER_MINFPS_AUTOTUNE)) {
            encode_time_auto_tune = (1000000 / VIDEO_ENCODER_MINFPS_AUTOTUNE);
        }

        if (encode_time_auto_tune > (VIDEO_ENCODER_LEEWAY_IN_MS_AUTOTUNE * 1000)) {
            encode_time_auto_tune = encode_time_auto_tune - (VIDEO_ENCODER_LEEWAY_IN_MS_AUTOTUNE * 1000); // give x ms more room
        }

        if (encode_time_auto_tune == 0) {
            // if the real delay was 0ms then still use 1ms
            encode_time_auto_tune = 1;
        }
    }

        max_encode_time_in_us = encode_time_auto_tune;
        LOGGER_ERROR(av->m->log, "AUTOTUNE:MAX_ENCODE_TIME_US=%ld us = %.1f fps", (long)encode_time_auto_tune, (float)(1000000.0f / encode_time_auto_tune));


    call->video.second->last_encoded_frame_ts = current_time_monotonic();

    if (call->video.second->send_keyframe_request_received == 1) {
        vpx_encode_flags = VPX_EFLAG_FORCE_KF;
        vpx_encode_flags |= VP8_EFLAG_FORCE_GF;
        // vpx_encode_flags |= VP8_EFLAG_FORCE_ARF;
        call->video.second->send_keyframe_request_received = 0;
    } else {
        if ((call->video.second->last_sent_keyframe_ts + VIDEO_MIN_SEND_KEYFRAME_INTERVAL)
                < current_time_monotonic()) {
            // it's been x seconds without a keyframe, send one now
            vpx_encode_flags = VPX_EFLAG_FORCE_KF;
            vpx_encode_flags |= VP8_EFLAG_FORCE_GF;
            // vpx_encode_flags |= VP8_EFLAG_FORCE_ARF;
        } else {
            // vpx_encode_flags |= VP8_EFLAG_FORCE_GF;
            // vpx_encode_flags |= VP8_EFLAG_NO_REF_GF;
            // vpx_encode_flags |= VP8_EFLAG_NO_REF_ARF;
            // vpx_encode_flags |= VP8_EFLAG_NO_REF_LAST;
            // vpx_encode_flags |= VP8_EFLAG_NO_UPD_GF;
            // vpx_encode_flags |= VP8_EFLAG_NO_UPD_ARF;
        }
    }


    // for the H264 encoder -------
    x264_nal_t *nal;
    int i_frame_size;
    // for the H264 encoder -------

    { /* Encode */


        if ((call->video.second->video_encoder_coded_used == TOXAV_ENCODER_CODEC_USED_VP8)
                || (call->video.second->video_encoder_coded_used == TOXAV_ENCODER_CODEC_USED_VP9)) {


            vpx_image_t img;
            img.w = img.h = img.d_w = img.d_h = 0;
            vpx_img_alloc(&img, VPX_IMG_FMT_I420, width, height, 0);

            /* I420 "It comprises an NxM Y plane followed by (N/2)x(M/2) V and U planes."
             * http://fourcc.org/yuv.php#IYUV
             */
            memcpy(img.planes[VPX_PLANE_Y], y, width * height);
            memcpy(img.planes[VPX_PLANE_U], u, (width / 2) * (height / 2));
            memcpy(img.planes[VPX_PLANE_V], v, (width / 2) * (height / 2));

#if 0
            uint32_t duration = (ms_to_last_frame * 10) + 1;

            if (duration > 10000) {
                duration = 10000;
            }

#else
            // set to hardcoded 24fps (this is only for vpx internal calculations!!)
            uint32_t duration = (41 * 10); // HINT: 24fps ~= 41ms
#endif

            vpx_codec_err_t vrc = vpx_codec_encode(call->video.second->encoder, &img,
                                                   (int64_t)video_frame_record_timestamp, duration,
                                                   vpx_encode_flags,
                                                   VPX_DL_REALTIME);

            vpx_img_free(&img);

        if (vrc != VPX_CODEC_OK) {
            pthread_mutex_unlock(call->mutex_video);
            LOGGER_API_ERROR(av->tox, "Could not encode video frame: %s\n", vpx_codec_err_to_string(vrc));
            rc = TOXAV_ERR_SEND_FRAME_INVALID;
            goto RETURN;
        }
    }

    ++call->video.second->frame_counter;

    LOGGER_DEBUG(av->m->log, "VPXENC:======================\n");
    LOGGER_DEBUG(av->m->log, "VPXENC:frame num=%ld\n", (long)call->video.second->frame_counter);


    { /* Send frames */

        if ((call->video.second->video_encoder_coded_used == TOXAV_ENCODER_CODEC_USED_VP8)
                || (call->video.second->video_encoder_coded_used == TOXAV_ENCODER_CODEC_USED_VP9)) {


            vpx_codec_iter_t iter = NULL;
            const vpx_codec_cx_pkt_t *pkt;

            while ((pkt = vpx_codec_get_cx_data(call->video.second->encoder, &iter)) != NULL) {
                if (pkt->kind == VPX_CODEC_CX_FRAME_PKT) {
                    const int keyframe = (pkt->data.frame.flags & VPX_FRAME_IS_KEY) != 0;

                    if (keyframe) {
                        call->video.second->last_sent_keyframe_ts = current_time_monotonic();
                    }

                    if ((pkt->data.frame.flags & VPX_FRAME_IS_FRAGMENT) != 0) {
                        LOGGER_DEBUG(av->m->log, "VPXENC:VPX_FRAME_IS_FRAGMENT:*yes* size=%lld pid=%d\n",
                                     (long long)pkt->data.frame.sz, (int)pkt->data.frame.partition_id);
                    } else {
                        LOGGER_DEBUG(av->m->log, "VPXENC:VPX_FRAME_IS_FRAGMENT:-no- size=%lld pid=%d\n",
                                     (long long)pkt->data.frame.sz, (int)pkt->data.frame.partition_id);
                    }

                    // use the record timestamp that was actually used for this frame
                    video_frame_record_timestamp = (uint64_t)pkt->data.frame.pts;
                    // LOGGER_DEBUG(av->m->log, "video packet record time: %llu", video_frame_record_timestamp);

                    // https://www.webmproject.org/docs/webm-sdk/structvpx__codec__cx__pkt.html
                    // pkt->data.frame.sz -> size_t
                    const uint32_t frame_length_in_bytes = pkt->data.frame.sz;


                    int res = rtp_send_data
                              (
                                  call->video.first,
                                  (const uint8_t *)pkt->data.frame.buf,
                                  frame_length_in_bytes,
                                  keyframe,
                                  video_frame_record_timestamp,
                                  (int32_t)pkt->data.frame.partition_id,
                                  av->m->log
                              );

                    LOGGER_DEBUG(av->m->log, "+ _sending_FRAME_TYPE_==%s bytes=%d frame_len=%d", keyframe ? "K" : ".",
                                 (int)pkt->data.frame.sz, (int)frame_length_in_bytes);
                    LOGGER_DEBUG(av->m->log, "+ _sending_FRAME_ b0=%d b1=%d", ((const uint8_t *)pkt->data.frame.buf)[0] ,
                                 ((const uint8_t *)pkt->data.frame.buf)[1]);

                    video_frame_record_timestamp++;

                    if (res < 0) {
                        pthread_mutex_unlock(call->mutex_video);
                        LOGGER_WARNING(av->m->log, "Could not send video frame: %s", strerror(errno));
                        rc = TOXAV_ERR_SEND_FRAME_RTP_FAILED;
                        goto END;
                    } else {
                    }
                }
            }

        } else {
            // HINT: H264

            if (i_frame_size > 0) {

                // use the record timestamp that was actually used for this frame
                video_frame_record_timestamp = (uint64_t)call->video.second->h264_in_pic.i_pts;
                const uint32_t frame_length_in_bytes = i_frame_size;
                const int keyframe = (int)call->video.second->h264_out_pic.b_keyframe;

                int res = rtp_send_data
                          (
                              call->video.first,
                              (const uint8_t *)nal->p_payload,
                              frame_length_in_bytes,
                              keyframe,
                              video_frame_record_timestamp,
                              (int32_t)0,
                              av->m->log
                          );

                video_frame_record_timestamp++;

                if (res < 0) {
                    pthread_mutex_unlock(call->mutex_video);
                    LOGGER_WARNING(av->m->log, "Could not send video frame: %s", strerror(errno));
                    rc = TOXAV_ERR_SEND_FRAME_RTP_FAILED;
                    goto END;
                } else {


                }

            }
        }

    rc = send_frames(av, call);

    pthread_mutex_unlock(call->mutex_video);

RETURN:

    if (error) {
        *error = rc;
    }

    return rc == TOXAV_ERR_SEND_FRAME_OK;
}



void toxav_callback_audio_receive_frame(ToxAV *av, toxav_audio_receive_frame_cb *callback, void *user_data)
{
    pthread_mutex_lock(av->mutex);
    av->acb = callback;
    av->acb_user_data = user_data;
    pthread_mutex_unlock(av->mutex);
}
void toxav_callback_video_receive_frame(ToxAV *av, toxav_video_receive_frame_cb *callback, void *user_data)
{
    pthread_mutex_lock(av->mutex);
    av->vcb = callback;
    av->vcb_user_data = user_data;
    pthread_mutex_unlock(av->mutex);
}


/*******************************************************************************
 *
 * :: Internal
 *
 ******************************************************************************/
static void callback_bwc(BWController *bwc, uint32_t friend_number, float loss, void *user_data)
{
    /* Callback which is called when the internal measure mechanism reported packet loss.
     * We report suggested lowered bitrate to an app. If app is sending both audio and video,
     * we will report lowered bitrate for video only because in that case video probably
     * takes more than 90% bandwidth. Otherwise, we report lowered bitrate on audio.
     * The application may choose to disable video totally if the stream is too bad.
     */

    ToxAVCall *call = (ToxAVCall *)user_data;
    assert(call);

    LOGGER_API_DEBUG(call->av->tox, "Reported loss of %f%%", (double)loss * 100);

    /* if less than x% data loss we do nothing! */
    if (loss < VIDEO_ACCEPTABLE_LOSS) {
        return;
    }

    pthread_mutex_lock(call->av->mutex);

    if (call->video_bit_rate) {
        if (!call->av->vbcb) {
            pthread_mutex_unlock(call->av->mutex);
            LOGGER_API_WARNING(call->av->tox, "No callback to report loss on");
            return;
        }

    if (call->video_bit_rate) {
        (*call->av->bcb.first)(call->av, friend_number, call->audio_bit_rate,
                               call->video_bit_rate - (call->video_bit_rate * loss),
                               call->av->bcb.second);
    } else if (call->audio_bit_rate) {
        if (!call->av->abcb) {
            pthread_mutex_unlock(call->av->mutex);
            LOGGER_API_WARNING(call->av->tox, "No callback to report loss on");
            return;
        }

        call->av->abcb(call->av, friend_number,
                       call->audio_bit_rate - (call->audio_bit_rate * loss),
                       call->av->abcb_user_data);
    }

    pthread_mutex_unlock(call->av->mutex);
}

int callback_invite(void *toxav_inst, MSICall *call)
{
    ToxAV *toxav = (ToxAV *)toxav_inst;
    pthread_mutex_lock(toxav->mutex);

    ToxAVCall *av_call = call_new(toxav, call->friend_number, NULL);

    if (av_call == nullptr) {
        LOGGER_API_WARNING(toxav->tox, "Failed to initialize call...");
        pthread_mutex_unlock(toxav->mutex);
        return -1;
    }

    call->av_call = av_call;
    av_call->msi_call = call;

    if (toxav->ccb) {
        toxav->ccb(toxav, call->friend_number, call->peer_capabilities & MSI_CAP_S_AUDIO,
                   call->peer_capabilities & MSI_CAP_S_VIDEO, toxav->ccb_user_data);
    } else {
        /* No handler to capture the call request, send failure */
        pthread_mutex_unlock(toxav->mutex);
        return -1;
    }

    pthread_mutex_unlock(toxav->mutex);
    return 0;
}

int callback_start(void *toxav_inst, MSICall *call)
{
    ToxAV *toxav = (ToxAV *)toxav_inst;
    pthread_mutex_lock(toxav->mutex);

    ToxAVCall *av_call = call_get(toxav, call->friend_number);

    if (av_call == NULL) {
        /* Should this ever happen? */
        pthread_mutex_unlock(toxav->mutex);
        return -1;
    }

    if (!call_prepare_transmission(av_call)) {
        callback_error(toxav_inst, call);
        pthread_mutex_unlock(toxav->mutex);
        return -1;
    }

    if (!invoke_call_state_callback(toxav, call->friend_number, call->peer_capabilities)) {
        callback_error(toxav_inst, call);
        pthread_mutex_unlock(toxav->mutex);
        return -1;
    }

    pthread_mutex_unlock(toxav->mutex);
    return 0;
}

int callback_end(void *toxav_inst, MSICall *call)
{
    ToxAV *toxav = (ToxAV *)toxav_inst;
    pthread_mutex_lock(toxav->mutex);

    invoke_call_state_callback(toxav, call->friend_number, TOXAV_FRIEND_CALL_STATE_FINISHED);

    if (call->av_call) {
        call_kill_transmission(call->av_call);
        call_remove(call->av_call);
    }

    pthread_mutex_unlock(toxav->mutex);
    return 0;
}

int callback_error(void *toxav_inst, MSICall *call)
{
    ToxAV *toxav = (ToxAV *)toxav_inst;
    pthread_mutex_lock(toxav->mutex);

    invoke_call_state_callback(toxav, call->friend_number, TOXAV_FRIEND_CALL_STATE_ERROR);

    if (call->av_call) {
        call_kill_transmission(call->av_call);
        call_remove(call->av_call);
    }

    pthread_mutex_unlock(toxav->mutex);
    return 0;
}

int callback_capabilites(void *toxav_inst, MSICall *call)
{
    ToxAV *toxav = (ToxAV *)toxav_inst;
    pthread_mutex_lock(toxav->mutex);

    if (call->peer_capabilities & MSI_CAP_S_AUDIO) {
        rtp_allow_receiving(toxav->tox, call->av_call->audio_rtp);
    } else {
        rtp_stop_receiving(toxav->tox, call->av_call->audio_rtp);
    }

    if (call->peer_capabilities & MSI_CAP_S_VIDEO) {
        rtp_allow_receiving(toxav->tox, call->av_call->video_rtp);
    } else {
        rtp_stop_receiving(toxav->tox, call->av_call->video_rtp);
    }

    invoke_call_state_callback(toxav, call->friend_number, call->peer_capabilities);

    pthread_mutex_unlock(toxav->mutex);
    return 0;
}

bool audio_bit_rate_invalid(uint32_t bit_rate)
{
    /* Opus RFC 6716 section-2.1.1 dictates the following:
     * Opus supports all bit rates from 6 kbit/s to 510 kbit/s.
     */
    return bit_rate < 6 || bit_rate > 510;
}

bool video_bit_rate_invalid(uint32_t bit_rate)
{
    (void) bit_rate;
    /* TODO(mannol): If anyone knows the answer to this one please fill it up */
    return false;
}

bool invoke_call_state_callback(ToxAV *av, uint32_t friend_number, uint32_t state)
{
    if (av->scb) {
        av->scb(av, friend_number, state, av->scb_user_data);
    } else {
        return false;
    }

    return true;
}

static ToxAVCall *call_new(ToxAV *av, uint32_t friend_number, Toxav_Err_Call *error)
{
    /* Assumes mutex locked */
    TOXAV_ERR_CALL rc = TOXAV_ERR_CALL_OK;
    ToxAVCall *call = NULL;

    if (toxav_friend_exists(av->tox, friend_number) == 0) {
        rc = TOXAV_ERR_CALL_FRIEND_NOT_FOUND;
        goto RETURN;
    }

    TOX_ERR_FRIEND_QUERY f_con_query_error;
    TOX_CONNECTION f_conn_status = tox_friend_get_connection_status(av->tox, friend_number, &f_con_query_error);

    if (f_conn_status == TOX_CONNECTION_NONE) {
        rc = TOXAV_ERR_CALL_FRIEND_NOT_CONNECTED;
        goto RETURN;
    }

    if (call_get(av, friend_number) != NULL) {
        rc = TOXAV_ERR_CALL_FRIEND_ALREADY_IN_CALL;
        goto RETURN;
    }


    call = (ToxAVCall *)calloc(sizeof(ToxAVCall), 1);

    call->last_incoming_video_frame_rtimestamp = 0;
    call->last_incoming_video_frame_ltimestamp = 0;

    call->last_incoming_audio_frame_rtimestamp = 0;
    call->last_incoming_audio_frame_ltimestamp = 0;

    call->reference_rtimestamp = 0;
    call->reference_ltimestamp = 0;
    call->reference_diff_timestamp = 0;
    call->reference_diff_timestamp_set = 0;

    if (call == NULL) {
        rc = TOXAV_ERR_CALL_MALLOC;
        goto RETURN;
    }

    call->av = av;
    call->friend_number = friend_number;

    if (av->calls == NULL) { /* Creating */
        av->calls = (ToxAVCall **)calloc(sizeof(ToxAVCall *), friend_number + 1);

        if (av->calls == NULL) {
            free(call);
            call = NULL;
            rc = TOXAV_ERR_CALL_MALLOC;
            goto RETURN;
        }

        av->calls_tail = friend_number;
        av->calls_head = friend_number;
    } else if (av->calls_tail < friend_number) { /* Appending */
        ToxAVCall **tmp = (ToxAVCall **)realloc(av->calls, sizeof(ToxAVCall *) * (friend_number + 1));

        if (tmp == NULL) {
            free(call);
            call = NULL;
            rc = TOXAV_ERR_CALL_MALLOC;
            goto RETURN;
        }

        av->calls = tmp;

        /* Set fields in between to null */
        uint32_t i = av->calls_tail + 1;

        for (; i < friend_number; i ++) {
            av->calls[i] = NULL;
        }

        call->prev = av->calls[av->calls_tail];
        av->calls[av->calls_tail]->next = call;

        av->calls_tail = friend_number;
    } else if (av->calls_head > friend_number) { /* Inserting at front */
        call->next = av->calls[av->calls_head];
        av->calls[av->calls_head]->prev = call;
        av->calls_head = friend_number;
    }

    av->calls[friend_number] = call;

RETURN:

    if (error) {
        *error = rc;
    }

    return call;
}

static ToxAVCall *call_remove(ToxAVCall *call)
{
    if (call == NULL) {
        return NULL;
    }

    uint32_t friend_number = call->friend_number;
    ToxAV *av = call->av;

    ToxAVCall *prev = call->prev;
    ToxAVCall *next = call->next;

    /* Set av call in msi to NULL in order to know if call if ToxAVCall is
     * removed from the msi call.
     */
    if (call->msi_call) {
        call->msi_call->av_call = NULL;
    }

    pthread_mutex_destroy(call->toxav_call_mutex);
    free(call);

    if (prev) {
        prev->next = next;
    } else if (next) {
        av->calls_head = next->friend_number;
    } else {
        goto CLEAR;
    }

    if (next) {
        next->prev = prev;
    } else if (prev) {
        av->calls_tail = prev->friend_number;
    } else {
        goto CLEAR;
    }

    av->calls[friend_number] = NULL;
    return next;

CLEAR:
    av->calls_head = 0;
    av->calls_tail = 0;
    free(av->calls);
    av->calls = NULL;

    return NULL;
}

static bool call_prepare_transmission(ToxAVCall *call)
{
    /* Assumes mutex locked */

    if (call == NULL) {
        return false;
    }

    ToxAV *av = call->av;

    if (!av->acb && !av->vcb) {
        /* It makes no sense to have CSession without callbacks */
        return false;
    }

    if (call->active) {
        LOGGER_API_WARNING(av->tox, "Call already active!\n");
        return true;
    }

    if (create_recursive_mutex(call->mutex_audio) != 0) {
        return false;
    }

    if (create_recursive_mutex(call->mutex_video) != 0) {
        goto FAILURE_2;
    }

    /* Prepare bwc */
    call->bwc = bwc_new(av->tox, call->friend_number, callback_bwc, call, av->toxav_mono_time);

    { /* Prepare audio */
        call->audio = ac_new(av->toxav_mono_time, nullptr, av, call->friend_number, av->acb, av->acb_user_data);

        if (!call->audio) {
            LOGGER_API_ERROR(av->tox, "Failed to create audio codec session");
            goto FAILURE;
        }

        call->audio_rtp = rtp_new(RTP_TYPE_AUDIO, av->tox, av, call->friend_number, call->bwc,
                                  call->audio, ac_queue_message);

        if (!call->audio_rtp) {
            LOGGER_API_ERROR(av->tox, "Failed to create audio rtp session");
            goto FAILURE;
        }
    }
    { /* Prepare video */
        call->video = vc_new(av->toxav_mono_time, nullptr, av, call->friend_number, av->vcb, av->vcb_user_data);

        if (!call->video) {
            LOGGER_API_ERROR(av->tox, "Failed to create video codec session");
            goto FAILURE;
        }

        call->video_rtp = rtp_new(RTP_TYPE_VIDEO, av->tox, av, call->friend_number, call->bwc,
                                  call->video, vc_queue_message);

        if (!call->video_rtp) {
            LOGGER_API_ERROR(av->tox, "Failed to create video rtp session");
            goto FAILURE;
        }
    }

    call->active = 1;
    return true;

FAILURE:
    bwc_kill(call->bwc);
    rtp_kill(av->tox, call->audio_rtp);
    ac_kill(call->audio);
    call->audio_rtp = nullptr;
    call->audio = nullptr;
    rtp_kill(av->tox, call->video_rtp);
    vc_kill(call->video);
    call->video_rtp = nullptr;
    call->video = nullptr;
    pthread_mutex_destroy(call->mutex_video);
FAILURE_2:
    pthread_mutex_destroy(call->mutex_audio);
    return false;
}

static void call_kill_transmission(ToxAVCall *call)
{
    if (call == NULL || call->active == 0) {
        return;
    }

    call->active = 0;

    pthread_mutex_lock(call->mutex_audio);
    pthread_mutex_unlock(call->mutex_audio);
    pthread_mutex_lock(call->mutex_video);
    pthread_mutex_unlock(call->mutex_video);
    pthread_mutex_lock(call->toxav_call_mutex);
    pthread_mutex_unlock(call->toxav_call_mutex);

    bwc_kill(call->bwc);

    ToxAV *av = call->av;

    rtp_kill(av->tox, call->audio_rtp);
    ac_kill(call->audio);
    call->audio_rtp = nullptr;
    call->audio = nullptr;

    rtp_kill(av->tox, call->video_rtp);
    vc_kill(call->video);
    call->video_rtp = nullptr;
    call->video = nullptr;

    pthread_mutex_destroy(call->mutex_audio);
    pthread_mutex_destroy(call->mutex_video);
}

Mono_Time *toxav_get_av_mono_time(ToxAV *toxav)
{
    if (!toxav) {
        return nullptr;
    }

    return toxav->toxav_mono_time;
}
