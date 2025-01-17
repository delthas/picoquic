/*
* Author: Christian Huitema
* Copyright (c) 2017, Private Octopus, Inc.
* All rights reserved.
*
* Permission to use, copy, modify, and distribute this software for any
* purpose with or without fee is hereby granted, provided that the above
* copyright notice and this permission notice appear in all copies.
*
* THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS" AND
* ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED
* WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE
* DISCLAIMED. IN NO EVENT SHALL Private Octopus, Inc. BE LIABLE FOR ANY
* DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES
* (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES;
* LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND
* ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
* (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS
* SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
*/

#include "picoquic_internal.h"
#include "util.h"
#include "tls_api.h"
#include "picoquictest_internal.h"
#ifdef _WINDOWS
#include "wincompat.h"
#endif
#include <picotls.h>
#include <stddef.h>
#include <stdlib.h>
#include <string.h>
#include <openssl/pem.h>
#include "picoquictest_internal.h"

#define RANDOM_PUBLIC_TEST_SEED 0xDEADBEEFCAFEC001ull

char const * picoquic_test_solution_dir = NULL;

static const uint8_t test_ticket_encrypt_key[32] = {
    0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15,
    16, 17, 18, 19, 20, 21, 22, 23, 24, 25, 26, 27, 28, 29, 30, 31
};

static const uint8_t test_ticket_badcrypt_key[32] = {
    255, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15,
    16, 17, 18, 19, 20, 21, 22, 23, 24, 25, 26, 27, 28, 29, 30, 31
};
/*
 * Generic call back function.
 */

static test_api_stream_desc_t test_scenario_oneway[] = {
    { 4, 0, 257, 0 }
};

static test_api_stream_desc_t test_scenario_q_and_r[] = {
    { 4, 0, 257, 2000 }
};

static test_api_stream_desc_t test_scenario_q2_and_r2[] = {
    { 4, 0, 257, 2000 },
    { 8, 0, 531, 11000 }
};

static test_api_stream_desc_t test_scenario_very_long[] = {
    { 4, 0, 257, 1000000 }
};

static test_api_stream_desc_t test_scenario_quant[] = {
    { 4, 0, 257, 10000 }
};

static test_api_stream_desc_t test_scenario_stop_sending[] = {
    { 4, 0, 257, 1000000 },
    { 8, 4, 531, 11000 }
};

static test_api_stream_desc_t test_scenario_unidir[] = {
    { 2, 0, 4000, 0 },
    { 6, 0, 5000, 0 }
};

static test_api_stream_desc_t test_scenario_mtu_discovery[] = {
    { 2, 0, 100000, 0 }
};

static test_api_stream_desc_t test_scenario_sustained[] = {
    { 4, 0, 257, 1000000 },
    { 8, 4, 257, 1000000 },
    { 12, 8, 257, 1000000 },
    { 16, 12, 257, 1000000 }
};

static test_api_stream_desc_t test_scenario_many_streams[] = {
    { 4, 0, 32, 1000 },
    { 8, 0, 32, 1000 },
    { 12, 0, 32, 1000 },
    { 16, 0, 32, 1000 },
    { 20, 0, 32, 350 },
    { 24, 0, 32, 225 },
    { 28, 0, 32, 700 },
    { 32, 0, 32, 1500 }
};

static test_api_stream_desc_t test_scenario_more_streams[] = {
    { 4, 0, 32, 633 },
    { 8, 0, 32, 633 },
    { 12, 0, 32, 633 },
    { 16, 0, 32, 633 },
    { 20, 0, 32, 633 },
    { 24, 0, 32, 633 },
    { 28, 0, 32, 633 },
    { 32, 0, 32, 633 },
    { 36, 0, 32, 633 },
    { 40, 0, 32, 633 },
    { 44, 0, 32, 633 },
    { 48, 0, 32, 633 },
    { 52, 0, 32, 633 },
    { 56, 0, 32, 633 },
    { 60, 0, 32, 633 },
    { 64, 0, 32, 633 },
    { 68, 0, 32, 633 },
    { 72, 0, 32, 633 }
};

void picoquic_test_set_solution_dir(char const * solution_dir)
{
    picoquic_test_solution_dir = solution_dir;
}

static int test_api_init_stream_buffers(size_t len, uint8_t** src_bytes, uint8_t** rcv_bytes)
{
    int ret = 0;

    *src_bytes = (uint8_t*)malloc(len);
    *rcv_bytes = (uint8_t*)malloc(len);

    if (*src_bytes != NULL && *rcv_bytes != NULL) {
        memset(*rcv_bytes, 0, len);

        for (size_t i = 0; i < len; i++) {
            (*src_bytes)[i] = (uint8_t)(i);
        }
    } else {
        ret = -1;

        if (*src_bytes != NULL) {
            free(*src_bytes);
            *src_bytes = NULL;
        }

        if (*rcv_bytes != NULL) {
            free(*rcv_bytes);
            *rcv_bytes = NULL;
        }
    }

    return ret;
}

static int test_api_init_test_stream(test_api_stream_t* test_stream,
    uint64_t stream_id, uint64_t previous_stream_id, size_t q_len, size_t r_len)
{
    int ret = 0;

    memset(test_stream, 0, sizeof(test_api_stream_t));

    if (q_len != 0) {
        ret = test_api_init_stream_buffers(q_len, &test_stream->q_src, &test_stream->q_rcv);
        if (ret == 0) {
            test_stream->q_len = q_len;
        }
    }

    if (ret == 0 && r_len != 0) {
        ret = test_api_init_stream_buffers(r_len, &test_stream->r_src, &test_stream->r_rcv);
        if (ret == 0) {
            test_stream->r_len = r_len;
        }
    }

    if (ret == 0) {
        test_stream->previous_stream_id = previous_stream_id;
        test_stream->stream_id = stream_id;
    }

    return ret;
}

static void test_api_delete_test_stream(test_api_stream_t* test_stream)
{
    if (test_stream->q_src != NULL) {
        free(test_stream->q_src);
    }

    if (test_stream->q_rcv != NULL) {
        free(test_stream->q_rcv);
    }

    if (test_stream->r_src != NULL) {
        free(test_stream->r_src);
    }

    if (test_stream->r_rcv != NULL) {
        free(test_stream->r_rcv);
    }

    memset(test_stream, 0, sizeof(test_api_stream_t));
}

static void test_api_receive_stream_data(
    const uint8_t* bytes, size_t length, picoquic_call_back_event_t fin_or_event,
    uint8_t* buffer, size_t max_len, const uint8_t* reference, size_t* nb_received,
    picoquic_call_back_event_t* received, int* error_detected)
{
    if (bytes != NULL) {
        if (*nb_received + length > max_len) {
            *error_detected |= test_api_fail_recv_larger_than_sent;
        }
        else {
            memcpy(buffer + *nb_received, bytes, length);

            if (memcmp(reference + *nb_received, bytes, length) != 0) {
                *error_detected |= test_api_fail_data_does_not_match;
            }
        }
    }

    *nb_received += length;

    if (fin_or_event != picoquic_callback_stream_data) {
        if (*received != picoquic_callback_stream_data) {
            *error_detected |= test_api_fail_fin_received_twice;
        }

        *received = fin_or_event;
    }
}

static int test_api_stream0_prepare(picoquic_cnx_t* cnx, picoquic_test_tls_api_ctx_t* ctx, uint8_t * context, size_t space)
{
    int ret = -1;

    if (ctx->stream0_sent < ctx->stream0_target) {
        uint8_t * buffer;
        size_t available = ctx->stream0_target - ctx->stream0_sent;
        int is_fin = 1;

        if (available > space) {
            available = space;
            
            if (ctx->stream0_test_option == 1 && space > 1) {
                available--;
            }
            is_fin = 0;
        }
        else {
            if (ctx->stream0_test_option == 2) {
                is_fin = 0;
            }
        }

        buffer = picoquic_provide_stream_data_buffer(context, available, is_fin, !is_fin);
        if (buffer != NULL) {
            memset(buffer, 0xA5, available);
            ctx->stream0_sent += available;
            ret = 0;
        }
    }
    else if (ctx->stream0_test_option == 2 && ctx->stream0_sent == ctx->stream0_target) {
        (void)picoquic_provide_stream_data_buffer(context, 0, 1, 0);
        ret = 0;
    }

    return ret;
}

static int test_api_queue_initial_queries(picoquic_test_tls_api_ctx_t* test_ctx, uint64_t stream_id)
{
    int ret = 0;
    int more_stream = 0;

    for (size_t i = 0; ret == 0 && i < test_ctx->nb_test_streams; i++) {
        if (test_ctx->test_stream[i].previous_stream_id == stream_id) {
            picoquic_cnx_t* cnx = NULL;

            cnx = IS_CLIENT_STREAM_ID(test_ctx->test_stream[i].stream_id) ? test_ctx->cnx_client : test_ctx->cnx_server;

            ret = picoquic_add_to_stream(cnx, test_ctx->test_stream[i].stream_id,
                test_ctx->test_stream[i].q_src,
                test_ctx->test_stream[i].q_len, 1);

            if (ret == 0) {
                test_ctx->test_stream[i].q_sent = 1;
                more_stream = 1;
            }
        }
    }

    if (test_ctx->stream0_target > 0) {
        ret = picoquic_mark_active_stream(test_ctx->cnx_client, 0, 1, NULL);
    }

    if (more_stream == 0) {
        /* TODO: check whether the test is actually finished */
        test_ctx->streams_finished = 1;
        if (test_ctx->stream0_received >= test_ctx->stream0_target) {
            test_ctx->test_finished = 1;
        }
        else {
            test_ctx->test_finished = 0;
        }
    } else {
        test_ctx->test_finished = 0;
        test_ctx->streams_finished = 0;
    }

    return ret;
}

static int test_api_callback(picoquic_cnx_t* cnx,
    uint64_t stream_id, uint8_t* bytes, size_t length,
    picoquic_call_back_event_t fin_or_event, void* callback_ctx, void* v_stream_ctx)
{
    /* Need to implement the server sending strategy */
    test_api_callback_t* cb_ctx = (test_api_callback_t*)callback_ctx;
    picoquic_test_tls_api_ctx_t* ctx = NULL;
    size_t stream_index;
    picoquic_call_back_event_t stream_finished = picoquic_callback_stream_data;

    if (fin_or_event == picoquic_callback_close ||
        fin_or_event == picoquic_callback_application_close ||
        fin_or_event == picoquic_callback_almost_ready ||
        fin_or_event == picoquic_callback_ready) {
        /* do nothing in our tests */
        return 0;
    }

    if (cb_ctx->client_mode) {
        ctx = (picoquic_test_tls_api_ctx_t*)(((char*)callback_ctx) - offsetof(struct st_picoquic_test_tls_api_ctx_t, client_callback));
    } else {
        ctx = (picoquic_test_tls_api_ctx_t*)(((char*)callback_ctx) - offsetof(struct st_picoquic_test_tls_api_ctx_t, server_callback));
    }


    if (fin_or_event == picoquic_callback_version_negotiation) {
        if (ctx != NULL) {
            ctx->received_version_negotiation = 1;
        }
    }

    if (fin_or_event == picoquic_callback_stateless_reset) {
        /* take note to validate test */
        ctx->reset_received = 1;
        return 0;
    }

    if (fin_or_event == picoquic_callback_prepare_to_send) {
        if (cb_ctx->client_mode && stream_id == 0) {
            return test_api_stream0_prepare(cnx, ctx, bytes, length);
        } else {
            /* unexpected call */
            return -1;
        }
    }

    if (bytes != NULL) {
        if (cb_ctx->client_mode) {
            ctx->sum_data_received_at_client += (int) length;
        } else {
            ctx->sum_data_received_at_server += (int) length;
        }
    }

    if (stream_id == 0 && cb_ctx->client_mode == 0 && 
        (fin_or_event == picoquic_callback_stream_data || fin_or_event == picoquic_callback_stream_fin)) {
        if (bytes == NULL && length != 0) {
            cb_ctx->error_detected = test_api_bad_stream0_data;
        } else {
            for (size_t i = 0; i < length; i++) {
                if (bytes[i] != 0xA5) {
                    cb_ctx->error_detected = test_api_bad_stream0_data;
                }
            }
        }
        ctx->stream0_received += length;
        if (ctx->streams_finished && ctx->stream0_received >= ctx->stream0_target) {
            ctx->test_finished = 1;
        }
    } else {

        for (stream_index = 0; stream_index < ctx->nb_test_streams; stream_index++) {
            if (ctx->test_stream[stream_index].stream_id == stream_id) {
                break;
            }
        }

        if (stream_index >= ctx->nb_test_streams) {
            cb_ctx->error_detected |= test_api_fail_data_on_unknown_stream;
        }
        else if (fin_or_event == picoquic_callback_stop_sending) {
            /* Respond with a reset, no matter what. Should be smarter later */
            picoquic_reset_stream(cnx, stream_id, 0);
        }
        else if (fin_or_event == picoquic_callback_stream_data || fin_or_event == picoquic_callback_stream_fin || fin_or_event == picoquic_callback_stream_reset) {
            if (IS_CLIENT_STREAM_ID(stream_id)) {
                if (cb_ctx->client_mode) {
                    /* this is a response from the server to a client stream */
                    test_api_receive_stream_data(bytes, length, fin_or_event,
                        ctx->test_stream[stream_index].r_rcv,
                        ctx->test_stream[stream_index].r_len,
                        ctx->test_stream[stream_index].r_src,
                        &ctx->test_stream[stream_index].r_recv_nb,
                        &ctx->test_stream[stream_index].r_received,
                        &cb_ctx->error_detected);

                    stream_finished = fin_or_event;
                }
                else {
                    /* this is a query to a server */
                    test_api_receive_stream_data(bytes, length, fin_or_event,
                        ctx->test_stream[stream_index].q_rcv,
                        ctx->test_stream[stream_index].q_len,
                        ctx->test_stream[stream_index].q_src,
                        &ctx->test_stream[stream_index].q_recv_nb,
                        &ctx->test_stream[stream_index].q_received,
                        &cb_ctx->error_detected);

                    if (fin_or_event != 0) {
                        if (ctx->test_stream[stream_index].r_len == 0 || fin_or_event == picoquic_callback_stream_reset) {
                            ctx->test_stream[stream_index].r_received = 1;
                            stream_finished = fin_or_event;
                        }
                        else if (cb_ctx->error_detected == 0) {
                            /* send a response */
                            if (picoquic_add_to_stream(ctx->cnx_server, stream_id,
                                ctx->test_stream[stream_index].r_src,
                                ctx->test_stream[stream_index].r_len, 1)
                                != 0) {
                                cb_ctx->error_detected |= test_api_fail_cannot_send_response;
                            }
                        }
                    }
                }
            }
            else {
                if (cb_ctx->client_mode) {
                    /* this is a query from the server to the client */
                    test_api_receive_stream_data(bytes, length, fin_or_event,
                        ctx->test_stream[stream_index].q_rcv,
                        ctx->test_stream[stream_index].q_len,
                        ctx->test_stream[stream_index].q_src,
                        &ctx->test_stream[stream_index].q_recv_nb,
                        &ctx->test_stream[stream_index].q_received,
                        &cb_ctx->error_detected);

                    if (fin_or_event != 0) {
                        if (ctx->test_stream[stream_index].r_len == 0 || fin_or_event == picoquic_callback_stream_reset) {
                            ctx->test_stream[stream_index].r_received = 1;
                            stream_finished = fin_or_event;
                        }
                        else if (cb_ctx->error_detected == 0) {
                            /* send a response */
                            if (picoquic_add_to_stream(ctx->cnx_client, stream_id,
                                ctx->test_stream[stream_index].r_src,
                                ctx->test_stream[stream_index].r_len, 1)
                                != 0) {
                                cb_ctx->error_detected |= test_api_fail_cannot_send_response;
                            }
                        }
                    }
                }
                else {
                    /* this is a response to the server */
                    test_api_receive_stream_data(bytes, length, fin_or_event,
                        ctx->test_stream[stream_index].r_rcv,
                        ctx->test_stream[stream_index].r_len,
                        ctx->test_stream[stream_index].r_src,
                        &ctx->test_stream[stream_index].r_recv_nb,
                        &ctx->test_stream[stream_index].r_received,
                        &cb_ctx->error_detected);

                    stream_finished = fin_or_event;
                }
            }
        }
        else {
            cb_ctx->error_detected |= test_api_fail_unexpected_frame;
        }

        if (stream_finished != 0
            && cb_ctx->error_detected == 0) {
            /* queue the new queries initiated by that stream */
            if (test_api_queue_initial_queries(ctx, stream_id) != 0) {
                cb_ctx->error_detected |= test_api_fail_cannot_send_query;
            }
        }
    }

    return 0;
}

static int test_api_init_send_recv_scenario(picoquic_test_tls_api_ctx_t* test_ctx,
    test_api_stream_desc_t* stream_desc, size_t size_of_scenarios)
{
    int ret = 0;
    size_t nb_stream_desc = size_of_scenarios / sizeof(test_api_stream_desc_t);

    if (nb_stream_desc > PICOQUIC_TEST_MAX_TEST_STREAMS) {
        ret = -1;
    } else {
        test_ctx->nb_test_streams = nb_stream_desc;
        test_ctx->test_finished = 0;

        for (size_t i = 0; ret == 0 && i < nb_stream_desc; i++) {
            ret = test_api_init_test_stream(&test_ctx->test_stream[i],
                stream_desc[i].stream_id, stream_desc[i].previous_stream_id,
                stream_desc[i].q_len, stream_desc[i].r_len);
        }
    }

    if (ret == 0) {
        ret = test_api_queue_initial_queries(test_ctx, 0);
    }

    return ret;
}

static int verify_transport_extension(picoquic_cnx_t* cnx_client, picoquic_cnx_t* cnx_server)
{
    int ret = 0;

    /* verify that local parameters have a sensible value */
    if (cnx_client->local_parameters.idle_timeout == 0 || cnx_client->local_parameters.initial_max_data == 0 || cnx_client->local_parameters.initial_max_stream_data_bidi_local == 0 || cnx_client->local_parameters.max_packet_size == 0) {
        ret = -1;
    } else if (cnx_server->local_parameters.idle_timeout == 0 || cnx_server->local_parameters.initial_max_data == 0 || cnx_server->local_parameters.initial_max_stream_data_bidi_remote == 0 || cnx_server->local_parameters.max_packet_size == 0) {
        ret = -1;
    }
    /* Verify that the negotiation completed */
    else if (memcmp(&cnx_client->local_parameters, &cnx_server->remote_parameters,
                 sizeof(picoquic_tp_t))
        != 0) {
        ret = -1;
    } else if (memcmp(&cnx_server->local_parameters, &cnx_client->remote_parameters,
                   sizeof(picoquic_tp_t))
        != 0) {
        ret = -1;
    }

    return ret;
}

static int verify_sni(picoquic_cnx_t* cnx_client, picoquic_cnx_t* cnx_server,
    char const* sni)
{
    int ret = 0;
    char const* client_sni = picoquic_tls_get_sni(cnx_client);
    char const* server_sni = picoquic_tls_get_sni(cnx_server);

    if (sni == NULL) {
        if (cnx_client->sni != NULL) {
            ret = -1;
        } else if (client_sni != NULL) {
            ret = -1;
        } else if (server_sni != NULL) {
            ret = -1;
        }
    } else {
        if (cnx_client->sni == NULL) {
            ret = -1;
        } else if (client_sni == NULL) {
            ret = -1;
        } else if (server_sni == NULL) {
            ret = -1;
        } else if (strcmp(cnx_client->sni, sni) != 0) {
            ret = -1;
        } else if (strcmp(client_sni, sni) != 0) {
            ret = -1;
        } else if (strcmp(server_sni, sni) != 0) {
            ret = -1;
        }
    }

    return ret;
}

static int verify_alpn(picoquic_cnx_t* cnx_client, picoquic_cnx_t* cnx_server,
    char const* alpn)
{
    int ret = 0;
    char const* client_alpn = picoquic_tls_get_negotiated_alpn(cnx_client);
    char const* server_alpn = picoquic_tls_get_negotiated_alpn(cnx_server);

    if (alpn == NULL) {
        if (cnx_client->alpn != NULL) {
            ret = -1;
        } else if (client_alpn != NULL) {
            ret = -1;
        } else if (server_alpn != NULL) {
            ret = -1;
        }
    } else {
        if (cnx_client->alpn == NULL) {
            ret = -1;
        } else if (client_alpn == NULL) {
            ret = -1;
        } else if (server_alpn == NULL) {
            ret = -1;
        } else if (strcmp(cnx_client->alpn, alpn) != 0) {
            ret = -1;
        } else if (strcmp(client_alpn, alpn) != 0) {
            ret = -1;
        } else if (strcmp(server_alpn, alpn) != 0) {
            ret = -1;
        }
    }

    return ret;
}

static int verify_version(picoquic_cnx_t* cnx_client, picoquic_cnx_t* cnx_server)
{
    int ret = 0;

    if (cnx_client->version_index != cnx_server->version_index) {
        ret = -1;
    } else if (cnx_client->version_index < 0 || cnx_client->version_index >= (int)picoquic_nb_supported_versions) {
        ret = -1;
    } else {
        for (size_t i = 0; i < picoquic_nb_supported_versions; i++) {
            if (cnx_client->proposed_version != picoquic_supported_versions[cnx_client->version_index].version && cnx_client->proposed_version == picoquic_supported_versions[i].version) {
                ret = -1;
                break;
            }
        }
    }

    return ret;
}

void tls_api_delete_ctx(picoquic_test_tls_api_ctx_t* test_ctx)
{
    if (test_ctx->qclient != NULL) {
        picoquic_free(test_ctx->qclient);
    }

    if (test_ctx->qserver != NULL) {
        picoquic_free(test_ctx->qserver);
    }

    for (size_t i = 0; i < test_ctx->nb_test_streams; i++) {
        test_api_delete_test_stream(&test_ctx->test_stream[i]);
    }

    if (test_ctx->c_to_s_link != NULL) {
        picoquictest_sim_link_delete(test_ctx->c_to_s_link);
    }

    if (test_ctx->s_to_c_link != NULL) {
        picoquictest_sim_link_delete(test_ctx->s_to_c_link);
    }

    free(test_ctx);
}

int tls_api_init_ctx(picoquic_test_tls_api_ctx_t** pctx, uint32_t proposed_version,
    char const* sni, char const* alpn, uint64_t* p_simulated_time, 
    char const* ticket_file_name, char const* token_file_name,
    int force_zero_share, int delayed_init, int use_bad_crypt)
{
    int ret = 0;
    picoquic_test_tls_api_ctx_t* test_ctx = NULL;
    char test_server_cert_file[512];
    char test_server_key_file[512];
    char test_server_cert_store_file[512];

    ret = picoquic_get_input_path(test_server_cert_file, sizeof(test_server_cert_file), picoquic_test_solution_dir, PICOQUIC_TEST_FILE_SERVER_CERT);

    if (ret == 0) {
        ret = picoquic_get_input_path(test_server_key_file, sizeof(test_server_key_file), picoquic_test_solution_dir, PICOQUIC_TEST_FILE_SERVER_KEY);
    }

    if (ret == 0) {
        ret = picoquic_get_input_path(test_server_cert_store_file, sizeof(test_server_cert_store_file), picoquic_test_solution_dir, PICOQUIC_TEST_FILE_CERT_STORE);
    }

    if (ret != 0) {
        DBG_PRINTF("%s", "Cannot set the cert, key or store file names.\n");
    } else {
        test_ctx = (picoquic_test_tls_api_ctx_t*)
            malloc(sizeof(picoquic_test_tls_api_ctx_t));

        if (test_ctx == NULL) {
            ret = -1;
        } else {
            /* Init to NULL */
            memset(test_ctx, 0, sizeof(picoquic_test_tls_api_ctx_t));
            test_ctx->client_callback.client_mode = 1;

            /* Init of the IP addresses */
            memset(&test_ctx->client_addr, 0, sizeof(struct sockaddr_in));
            test_ctx->client_addr.sin_family = AF_INET;
#ifdef _WINDOWS
            test_ctx->client_addr.sin_addr.S_un.S_addr = 0x0A000002;
#else
            test_ctx->client_addr.sin_addr.s_addr = 0x0A000002;
#endif
            test_ctx->client_addr.sin_port = 1234;

            memset(&test_ctx->server_addr, 0, sizeof(struct sockaddr_in));
            test_ctx->server_addr.sin_family = AF_INET;
#ifdef _WINDOWS
            test_ctx->server_addr.sin_addr.S_un.S_addr = 0x0A000001;
#else
            test_ctx->server_addr.sin_addr.s_addr = 0x0A000001;
#endif
            test_ctx->server_addr.sin_port = 4321;

            /* Test the creation of the client and server contexts */
            test_ctx->qclient = picoquic_create(8, NULL, NULL, test_server_cert_store_file, NULL, test_api_callback,
                (void*)&test_ctx->client_callback, NULL, NULL, NULL, *p_simulated_time,
                p_simulated_time, ticket_file_name, NULL, 0);

            if (token_file_name != NULL) {
                (void)picoquic_load_token_file(test_ctx->qclient, token_file_name);
            }

            test_ctx->qserver = picoquic_create(8,
                test_server_cert_file, test_server_key_file, test_server_cert_store_file,
                PICOQUIC_TEST_ALPN, test_api_callback, (void*)&test_ctx->server_callback, NULL, NULL, NULL,
                *p_simulated_time, p_simulated_time, NULL,
                (use_bad_crypt == 0) ? test_ticket_encrypt_key : test_ticket_badcrypt_key,
                (use_bad_crypt == 0) ? sizeof(test_ticket_encrypt_key) : sizeof(test_ticket_badcrypt_key));

            if (test_ctx->qclient == NULL || test_ctx->qserver == NULL) {
                ret = -1;
            }

            /* register the links */
            if (ret == 0) {
                test_ctx->c_to_s_link = picoquictest_sim_link_create(0.01, 10000, NULL, 0, 0);
                test_ctx->s_to_c_link = picoquictest_sim_link_create(0.01, 10000, NULL, 0, 0);

                if (test_ctx->c_to_s_link == NULL || test_ctx->s_to_c_link == NULL) {
                    ret = -1;
                }
            }

            if (ret == 0) {
                /* Apply the zero share parameter if required */
                if (force_zero_share != 0)
                {
                    test_ctx->qclient->flags |= picoquic_context_client_zero_share;
                }

                /* Create a client connection */
                test_ctx->cnx_client = picoquic_create_cnx(test_ctx->qclient,
                    picoquic_null_connection_id, picoquic_null_connection_id,
                    (struct sockaddr*)&test_ctx->server_addr, *p_simulated_time,
                    proposed_version, sni, alpn, 1);

                if (test_ctx->cnx_client == NULL) {
                    ret = -1;
                }
                else if (delayed_init == 0) {
                    ret = picoquic_start_client_cnx(test_ctx->cnx_client);
                }
            }
        }
    }

    if (ret != 0 && test_ctx != NULL) {
        tls_api_delete_ctx(test_ctx);
        test_ctx = NULL;
    }

    *pctx = test_ctx;

    return ret;
}

int tls_api_one_sim_round(picoquic_test_tls_api_ctx_t* test_ctx,
    uint64_t* simulated_time, uint64_t time_out, int* was_active)
{
    int ret = 0;
    picoquictest_sim_link_t* target_link = NULL;
    int next_action = 0;

    if (test_ctx->qserver->pending_stateless_packet != NULL) {
        next_action = 1;
    }
    else {
        uint64_t next_time = *simulated_time + 120000000ull;
        uint64_t client_arrival, server_arrival;

        if (test_ctx->cnx_client->cnx_state != picoquic_state_disconnected) {
            uint64_t client_departure = test_ctx->cnx_client->next_wake_time;
            if (client_departure < next_time) {
                next_time = client_departure;
                next_action = 2;
            }
        }

        if (test_ctx->cnx_server != NULL && test_ctx->cnx_server->cnx_state != picoquic_state_disconnected) {
            uint64_t server_departure = test_ctx->cnx_server->next_wake_time;
            if (server_departure < next_time) {
                next_time = server_departure;
                next_action = 3;
            }
        }

        client_arrival = picoquictest_sim_link_next_arrival(test_ctx->s_to_c_link, next_time);
        if (client_arrival < next_time) {
            next_time = client_arrival;
            next_action = 4;
        }

        server_arrival = picoquictest_sim_link_next_arrival(test_ctx->c_to_s_link, next_time);
        if (server_arrival < next_time) {
            next_time = server_arrival;
            next_action = 5;
        }


        if (time_out > 0 && next_time > time_out) {
            next_action = 0;
            *simulated_time = time_out;
        } else if (next_time > *simulated_time) {
            *simulated_time = next_time;
        }
    }

    if (next_action >= 1 && next_action <= 3) {
        /* If there is something to send, do it now */
        picoquictest_sim_packet_t* packet = picoquictest_sim_link_create_packet();

        if (packet == NULL || test_ctx->cnx_client == NULL) {
            ret = -1;
        }
        else {
            if (next_action == 1) {
                picoquic_stateless_packet_t* sp = picoquic_dequeue_stateless_packet(test_ctx->qserver);

                if (sp != NULL) {
                    if (sp->length > 0) {

                        *was_active |= 1;
                        memcpy(&packet->addr_from, &sp->addr_local,
                            (sp->addr_local.ss_family == AF_INET) ? sizeof(struct sockaddr_in) : sizeof(struct sockaddr_in6));
                        memcpy(&packet->addr_to, &sp->addr_to,
                            (sp->addr_to.ss_family == AF_INET) ? sizeof(struct sockaddr_in) : sizeof(struct sockaddr_in6));
                        memcpy(packet->bytes, sp->bytes, sp->length);
                        packet->length = sp->length;

                        target_link = test_ctx->s_to_c_link;
                    }
                    picoquic_delete_stateless_packet(sp);
                }
            }
            else if (next_action == 2) {
                /* check whether the client has something to send */
                int peer_addr_len = 0;
                int local_addr_len = 0;
                uint8_t coalesced_length = 0;

                if (test_ctx->do_bad_coalesce_test && test_ctx->cnx_client->cnx_state > picoquic_state_server_handshake) {
                    uint32_t hl = 0;
                    memmove(packet->bytes + coalesced_length, packet->bytes, packet->length);

                    packet->bytes[hl++] = 0xE0; /* handshake */
                    picoformat_32(&packet->bytes[hl],
                        picoquic_supported_versions[test_ctx->cnx_client->version_index].version);
                    hl += 4;
                    packet->bytes[hl++] = test_ctx->cnx_client->path[0]->remote_cnxid.id_len;
                    hl += picoquic_format_connection_id(&packet->bytes[hl], PICOQUIC_MAX_PACKET_SIZE - hl, test_ctx->cnx_client->path[0]->remote_cnxid);
                    packet->bytes[hl++] = test_ctx->cnx_client->path[0]->local_cnxid.id_len;
                    hl += picoquic_format_connection_id(&packet->bytes[hl], PICOQUIC_MAX_PACKET_SIZE - hl, test_ctx->cnx_client->path[0]->local_cnxid);
                    packet->bytes[hl++] = 21;
                    picoquic_public_random(&packet->bytes[hl], 21);
                    coalesced_length = hl + 21;
                }
                ret = picoquic_prepare_packet(test_ctx->cnx_client, *simulated_time,
                    packet->bytes + coalesced_length, PICOQUIC_MAX_PACKET_SIZE - coalesced_length, &packet->length,
                    &packet->addr_to, &peer_addr_len, &packet->addr_from, &local_addr_len);
                if (ret != 0)
                {
                    /* useless test, but makes it easier to add a breakpoint under debugger */
                    ret = -1;
                }
                else if (packet->length > 0) {
                    packet->length += coalesced_length;
                    /* queue in c_to_s */
                    if (local_addr_len == 0) {
                        memcpy(&packet->addr_from, &test_ctx->client_addr, sizeof(struct sockaddr_in));
                    }
                    target_link = test_ctx->c_to_s_link;
                }
            }
            else if (next_action == 3) {
                int peer_addr_len = 0;
                int local_addr_len = 0;

                ret = picoquic_prepare_packet(test_ctx->cnx_server, *simulated_time,
                    packet->bytes, PICOQUIC_MAX_PACKET_SIZE, &packet->length,
                    &packet->addr_to, &peer_addr_len, &packet->addr_from, &local_addr_len);
                if (ret != 0)
                {
                    /* useless test, but makes it easier to add a breakpoint under debugger */
                    ret = -1;
                }
                else if (packet->length > 0) {
                    /* copy and queue in s to c */
                    if (local_addr_len == 0) {
                        memcpy(&packet->addr_from, &test_ctx->server_addr, sizeof(struct sockaddr_in));
                    }
                    target_link = test_ctx->s_to_c_link;
                }
            }

            if (packet->length > 0) {
                int simulate_loss = 0;
                if (target_link == test_ctx->c_to_s_link) {
                    if (picoquic_compare_addr((struct sockaddr *)&test_ctx->client_addr,
                        (struct sockaddr *)&packet->addr_from) != 0) {
                        if (test_ctx->client_use_nat) {
                            /* Rewrite the address */
                            picoquic_store_addr(&packet->addr_from, (struct sockaddr *)&test_ctx->client_addr);
                        }
                        else {
                            /* Using wrong address: simulate loss */
                            simulate_loss = 1;
                        }
                    }
                }
                if (simulate_loss == 0) {
                    picoquictest_sim_link_submit(target_link, packet, *simulated_time);
                }
                else {
                    free(packet);
                }
                *was_active |= 1;
            }
            else {
                free(packet);
            }
        }
    }
    else if (next_action == 4) {
        /* If there is something to receive, do it now */
        picoquictest_sim_packet_t* packet = picoquictest_sim_link_dequeue(test_ctx->s_to_c_link, *simulated_time);

        if (packet != NULL) {

            /* Check the destination address  before submitting the packet */
            if (picoquic_compare_addr((struct sockaddr *)&test_ctx->client_addr,
                (struct sockaddr *)&packet->addr_to) == 0) {
                ret = picoquic_incoming_packet(test_ctx->qclient, packet->bytes, (uint32_t)packet->length,
                    (struct sockaddr*)&packet->addr_from,
                    (struct sockaddr*)&packet->addr_to, 0, 0,
                    *simulated_time);
                *was_active |= 1;
            }

            if (ret != 0)
            {
                /* useless test, but makes it easier to add a breakpoint under debugger */
                ret = -1;
            }

            free(packet);
        }
    }
    else if (next_action == 5) {
        picoquictest_sim_packet_t* packet = picoquictest_sim_link_dequeue(test_ctx->c_to_s_link, *simulated_time);

        if (packet != NULL) {

            /* Check the destination address  before submitting the packet */
            /* TODO: better test when testing more than NAT rebinding. */
            if (test_ctx->server_use_multiple_addresses ||
                picoquic_compare_addr((struct sockaddr *)&test_ctx->server_addr,
                (struct sockaddr *)&packet->addr_to) == 0) {
                ret = picoquic_incoming_packet(test_ctx->qserver, packet->bytes, (uint32_t)packet->length,
                    (struct sockaddr*)&packet->addr_from,
                    (struct sockaddr*)&packet->addr_to, 0, 0,
                    *simulated_time);
            }

            if (ret != 0)
            {
                /* useless test, but makes it easier to add a breakpoint under debugger */
                ret = -1;
            }

            if (test_ctx->cnx_server == NULL) {
                picoquic_connection_id_t target_cnxid = test_ctx->cnx_client->initial_cnxid;
                picoquic_cnx_t* next = test_ctx->qserver->cnx_list;

                while (next != NULL && picoquic_compare_connection_id(&next->initial_cnxid, &target_cnxid) != 0) {
                    next = next->next_in_table;
                }

                test_ctx->cnx_server = next;
            }

            *was_active |= 1;
            free(packet);
        }
    }

    return ret;
}

#define TEST_CLIENT_READY (test_ctx->cnx_client->cnx_state == picoquic_state_ready || test_ctx->cnx_client->cnx_state == picoquic_state_client_ready_start)
#define TEST_SERVER_READY (test_ctx->cnx_server != NULL &&(test_ctx->cnx_server->cnx_state == picoquic_state_ready || test_ctx->cnx_server->cnx_state == picoquic_state_server_false_start))

int tls_api_connection_loop(picoquic_test_tls_api_ctx_t* test_ctx,
    uint64_t* loss_mask, uint64_t queue_delay_max, uint64_t* simulated_time)
{
    int ret = 0;
    int nb_trials = 0;
    int nb_inactive = 0;

    test_ctx->c_to_s_link->loss_mask = loss_mask;
    test_ctx->s_to_c_link->loss_mask = loss_mask;

    test_ctx->c_to_s_link->queue_delay_max = queue_delay_max;
    test_ctx->s_to_c_link->queue_delay_max = queue_delay_max;

    while (ret == 0 && nb_trials < 1024 && nb_inactive < 512 && (!TEST_CLIENT_READY || (test_ctx->cnx_server == NULL || !TEST_SERVER_READY))) {
        int was_active = 0;
        nb_trials++;

        ret = tls_api_one_sim_round(test_ctx, simulated_time, 0, &was_active);

        if (test_ctx->cnx_client->cnx_state == picoquic_state_disconnected &&
            (test_ctx->cnx_server == NULL || test_ctx->cnx_server->cnx_state == picoquic_state_disconnected)) {
            break;
        }

        if (was_active) {
            nb_inactive = 0;
        } else {
            nb_inactive++;
        }
    }

    return ret;
}

static int tls_api_data_sending_loop(picoquic_test_tls_api_ctx_t* test_ctx,
    uint64_t* loss_mask, uint64_t* simulated_time, int max_trials)
{
    int ret = 0;
    int nb_trials = 0;
    int nb_inactive = 0;

    test_ctx->c_to_s_link->loss_mask = loss_mask;
    test_ctx->s_to_c_link->loss_mask = loss_mask;

    if (max_trials <= 0) {
        max_trials = 100000;
    }

    while (ret == 0 && nb_trials < max_trials && nb_inactive < 256 && TEST_CLIENT_READY && TEST_SERVER_READY) {
        int was_active = 0;

        nb_trials++;

        ret = tls_api_one_sim_round(test_ctx, simulated_time, 0, &was_active);

        if (ret < 0)
        {
            break;
        }

        if (was_active) {
            nb_inactive = 0;
        } else {
            nb_inactive++;
        }

        if (test_ctx->test_finished) {
            if (picoquic_is_cnx_backlog_empty(test_ctx->cnx_client) && picoquic_is_cnx_backlog_empty(test_ctx->cnx_server)) {
                break;
            }
        }
    }

    return ret; /* end of sending loop */
}

static int tls_api_synch_to_empty_loop(picoquic_test_tls_api_ctx_t* test_ctx,
    uint64_t* simulated_time, int max_trials,
    int path_target, int wait_for_ready)
{
    /* run a receive loop until no outstanding data */
    int ret = 0;
    uint64_t time_out = *simulated_time + 4000000;
    int nb_rounds = 0;
    int success = 0;

    while (ret == 0 && *simulated_time < time_out &&
        nb_rounds < max_trials && test_ctx->cnx_client->cnx_state != picoquic_state_disconnected) {
        int was_active = 0;

        ret = tls_api_one_sim_round(test_ctx, simulated_time, time_out, &was_active);
        nb_rounds++;

        if (test_ctx->cnx_server == NULL) {
            break;
        }

        if (test_ctx->cnx_client->nb_paths >= path_target &&
            test_ctx->cnx_server->nb_paths >= path_target &&
            picoquic_is_cnx_backlog_empty(test_ctx->cnx_client) &&
            picoquic_is_cnx_backlog_empty(test_ctx->cnx_server) &&
            (!wait_for_ready || (
                test_ctx->cnx_client->cnx_state == picoquic_state_ready &&
                test_ctx->cnx_server->cnx_state == picoquic_state_ready))) {
            success = 1;
            break;
        }
    }

    if (ret == 0 && success == 0) {
        DBG_PRINTF("Exit synch loop after %d rounds, backlog or not enough paths (%d & %d).\n",
            nb_rounds, test_ctx->cnx_client->nb_paths, (test_ctx->cnx_server == NULL)?0: test_ctx->cnx_server->nb_paths);
    }

    return ret;
}


static int wait_application_aead_ready(picoquic_test_tls_api_ctx_t* test_ctx,
    uint64_t * simulated_time)
{
    int ret = 0;
    uint64_t time_out = *simulated_time + 4000000;
    int nb_trials = 0;
    int nb_inactive = 0;

    while (*simulated_time < time_out &&
        TEST_CLIENT_READY &&
        TEST_SERVER_READY &&
        test_ctx->cnx_server->crypto_context[3].aead_decrypt == NULL &&
        nb_trials < 1024 &&
        nb_inactive < 64 &&
        ret == 0) {
        int was_active = 0;
        nb_trials++;

        ret = tls_api_one_sim_round(test_ctx, simulated_time, time_out, &was_active);

        if (was_active) {
            nb_inactive = 0;
        }
        else {
            nb_inactive++;
        }
    }

    if (ret == 0 && test_ctx->cnx_server != NULL && test_ctx->cnx_server->crypto_context[3].aead_decrypt == NULL) {
        DBG_PRINTF("Could not obtain the 1-RTT decryption key, state = %d\n",
            test_ctx->cnx_server->cnx_state);
        ret = -1;
    }

    return ret;
}

static int tls_api_attempt_to_close(
    picoquic_test_tls_api_ctx_t* test_ctx, uint64_t* simulated_time)
{
    int ret = 0;
    int nb_rounds = 0;
    
    ret = picoquic_close(test_ctx->cnx_client, 0);

    if (ret == 0) {
        /* packet from client to server */
        /* Do not simulate losses there, as there is no way to correct them */

        test_ctx->c_to_s_link->loss_mask = 0;
        test_ctx->s_to_c_link->loss_mask = 0;

        while (ret == 0 && (test_ctx->cnx_client->cnx_state != picoquic_state_disconnected || test_ctx->cnx_server->cnx_state != picoquic_state_disconnected) && nb_rounds < 100000) {
            int was_active = 0;
            ret = tls_api_one_sim_round(test_ctx, simulated_time, 0, &was_active);
            nb_rounds++;
        }
    }

    if (ret == 0 && (test_ctx->cnx_client->cnx_state != picoquic_state_disconnected || test_ctx->cnx_server->cnx_state != picoquic_state_disconnected)) {
        ret = -1;
    }

    return ret;
}

static int tls_api_test_with_loss(uint64_t* loss_mask, uint32_t proposed_version,
    char const* sni, char const* alpn)
{
    uint64_t simulated_time = 0;
    picoquic_test_tls_api_ctx_t* test_ctx = NULL;
    int ret = tls_api_init_ctx(&test_ctx, proposed_version, sni, alpn, &simulated_time, NULL, NULL, 0, 0, 0);

    if (ret != 0)
    {
        DBG_PRINTF("Could not create the QUIC test contexts for V=%x\n", proposed_version);
    }
    else if (sni == NULL) {
        picoquic_set_null_verifier(test_ctx->qclient);
    }

    if (ret == 0) {
        ret = tls_api_connection_loop(test_ctx, loss_mask, 0, &simulated_time);

        if (ret != 0)
        {
            DBG_PRINTF("Connection loop returns %d\n", ret);
        }
    }

    if (ret == 0) {
        ret = tls_api_attempt_to_close(test_ctx, &simulated_time);

        if (ret != 0)
        {
            DBG_PRINTF("Connection close returns %d\n", ret);
        }

        if (ret == 0) {
            ret = verify_transport_extension(test_ctx->cnx_client, test_ctx->cnx_server);
            if (ret != 0)
            {
                DBG_PRINTF("%s", "Transport extensions do no match\n");
            }
        }

        if (ret == 0) {
            ret = verify_sni(test_ctx->cnx_client, test_ctx->cnx_server, sni);

            if (ret != 0)
            {
                DBG_PRINTF("%s", "SNI do not match\n");
            }
        }

        if (ret == 0) {
            ret = verify_alpn(test_ctx->cnx_client, test_ctx->cnx_server, alpn);

            if (ret != 0)
            {
                DBG_PRINTF("%s", "ALPN do not match\n");
            }
        }

        if (ret == 0) {
            ret = verify_version(test_ctx->cnx_client, test_ctx->cnx_server);

            if (ret != 0)
            {
                DBG_PRINTF("%s", "Negotiated versions do not match\n");
            }
        }
    }

    if (test_ctx != NULL) {
        tls_api_delete_ctx(test_ctx);
        test_ctx = NULL;
    }

    return ret;
}

int tls_api_test()
{
    return tls_api_test_with_loss(NULL, PICOQUIC_INTERNAL_TEST_VERSION_1, PICOQUIC_TEST_SNI, PICOQUIC_TEST_ALPN);
}

int tls_api_silence_test()
{
    uint64_t loss_mask = 0;
    uint64_t simulated_time = 0;
    uint64_t next_time = 0;
    picoquic_test_tls_api_ctx_t* test_ctx = NULL;
    int ret = tls_api_init_ctx(&test_ctx, 0, PICOQUIC_TEST_SNI, PICOQUIC_TEST_ALPN, &simulated_time, NULL, NULL, 0, 0, 0);

    if (ret == 0) {
        ret = tls_api_connection_loop(test_ctx, &loss_mask, 0, &simulated_time);
    }

    /* simulate 5 seconds of silence */
    next_time = simulated_time + 5000000;
    while (ret == 0 && simulated_time < next_time && TEST_CLIENT_READY && TEST_SERVER_READY) {
        int was_active = 0;

        ret = tls_api_one_sim_round(test_ctx, &simulated_time, next_time, &was_active);
    }

    if (ret == 0) {
        ret = tls_api_attempt_to_close(test_ctx, &simulated_time);
    }

    if (ret == 0) {
        /* verify the absence of any spurious retransmission */
        if (test_ctx->cnx_client->nb_retransmission_total != 0) {
            ret = -1;
        } else if (test_ctx->cnx_server != NULL && test_ctx->cnx_server->nb_retransmission_total != 0) {
            ret = -1;
        }
    }

    if (test_ctx != NULL) {
        tls_api_delete_ctx(test_ctx);
        test_ctx = NULL;
    }

    return ret;
}

int tls_api_loss_test(uint64_t mask)
{
    uint64_t loss_mask = mask;

    return tls_api_test_with_loss(&loss_mask, 0, PICOQUIC_TEST_SNI, PICOQUIC_TEST_ALPN);
}

int tls_api_many_losses()
{
    uint64_t loss_mask = 0;
    int ret = 0;

    for (uint64_t i = 0; ret == 0 && i < 6; i++) {
        for (uint64_t j = 1; ret == 0 && j < 4; j++) {
            loss_mask = ((((uint64_t)1) << j) - ((uint64_t)1)) << i;
            ret = tls_api_test_with_loss(&loss_mask, 0, NULL, NULL);
        }
    }

    return ret;
}

int tls_api_version_negotiation_test()
{
    const uint32_t version_grease = 0x0aca4a0a;
    uint64_t simulated_time = 0;
    picoquic_test_tls_api_ctx_t* test_ctx = NULL;
    int ret = tls_api_init_ctx(&test_ctx, version_grease, PICOQUIC_TEST_SNI, PICOQUIC_TEST_ALPN, &simulated_time, NULL, NULL, 0, 0, 0);

    if (ret != 0)
    {
        DBG_PRINTF("Could not create the QUIC test contexts for V=%x\n", version_grease);
    }

    if (ret == 0) {
        (void)tls_api_connection_loop(test_ctx, NULL, 0, &simulated_time);

        if (test_ctx->cnx_client->cnx_state == picoquic_state_disconnected) {
            ret = 0;
        }
        else {
            DBG_PRINTF("Unexpected state: %d\n", test_ctx->cnx_client->cnx_state);
            ret = -1;
        }
    }


    if (ret == 0) {
        if (!test_ctx->received_version_negotiation){
            DBG_PRINTF("%s", "No version negotiation notified\n");
            ret = -1;
        }

    }

    if (test_ctx != NULL) {
        tls_api_delete_ctx(test_ctx);
        test_ctx = NULL;
    }

    return ret;
}

int tls_api_sni_test()
{
    return tls_api_test_with_loss(NULL, 0, PICOQUIC_TEST_SNI, PICOQUIC_TEST_ALPN);
}

int tls_api_alpn_test()
{
    return tls_api_test_with_loss(NULL, 0, PICOQUIC_TEST_SNI, PICOQUIC_TEST_ALPN);
}

int tls_api_wrong_alpn_test()
{
    return tls_api_test_with_loss(NULL, 0, PICOQUIC_TEST_SNI, PICOQUIC_TEST_WRONG_ALPN);
}

/*
 * Scenario based transmission tests.
 */

int tls_api_one_scenario_init(
    picoquic_test_tls_api_ctx_t** p_test_ctx, uint64_t * simulated_time,
    uint32_t proposed_version,
    picoquic_tp_t * client_params, picoquic_tp_t * server_params)
{
    int ret = tls_api_init_ctx(p_test_ctx,
        (proposed_version == 0) ? PICOQUIC_INTERNAL_TEST_VERSION_1 : proposed_version,
        PICOQUIC_TEST_SNI, PICOQUIC_TEST_ALPN, simulated_time, NULL, NULL, 0, 1, 0);

    if (ret != 0) {
        DBG_PRINTF("Could not create the QUIC test contexts for V=%x\n", proposed_version);
    }
    else if (*p_test_ctx == NULL || (*p_test_ctx)->cnx_client == NULL || (*p_test_ctx)->qserver == NULL) {
        DBG_PRINTF("%s", "Connections where not properly created!\n");
        ret = -1;
    }

    if (ret == 0 && client_params != NULL) {
        picoquic_set_transport_parameters((*p_test_ctx)->cnx_client, client_params);
    }

    if (ret == 0 && server_params != NULL) {
        ret = picoquic_set_default_tp((*p_test_ctx)->qserver, server_params);
        if (server_params->prefered_address.ipv4Port != 0 ||
            server_params->prefered_address.ipv6Port != 0) {
            /* If testing server migration, disable address check */
            (*p_test_ctx)->server_use_multiple_addresses = 1;
        }
    }

    return ret;
}

int tls_api_one_scenario_verify(picoquic_test_tls_api_ctx_t* test_ctx) {
    int ret = 0;

    if (test_ctx->server_callback.error_detected) {
        ret = -1;
    }
    else if (test_ctx->client_callback.error_detected) {
        ret = -1;
    }
    else {
        for (size_t i = 0; ret == 0 && i < test_ctx->nb_test_streams; i++) {
            if (test_ctx->test_stream[i].q_recv_nb != test_ctx->test_stream[i].q_len) {
                ret = -1;
            }
            else if (test_ctx->test_stream[i].r_recv_nb != test_ctx->test_stream[i].r_len) {
                ret = -1;
            }
            else if (test_ctx->test_stream[i].q_received == 0 || test_ctx->test_stream[i].r_received == 0) {
                ret = -1;
            }
        }

        if (test_ctx->stream0_sent != test_ctx->stream0_target ||
            test_ctx->stream0_sent != test_ctx->stream0_received) {
            ret = -1;
        }
    }
    if (ret != 0)
    {
        DBG_PRINTF("Test scenario verification returns %d\n", ret);
    }

    return ret;
}

int tls_api_one_scenario_body_connect(picoquic_test_tls_api_ctx_t* test_ctx,
    uint64_t * simulated_time, size_t stream0_target, uint64_t max_data, uint64_t queue_delay_max)
{
    uint64_t loss_mask = 0;
    int ret = picoquic_start_client_cnx(test_ctx->cnx_client);

    if (ret != 0)
    {
        DBG_PRINTF("%s", "Could not initialize connection for the client\n");
    }
    else {
        ret = tls_api_connection_loop(test_ctx, &loss_mask, queue_delay_max, simulated_time);

        if (ret != 0)
        {
            DBG_PRINTF("Connection loop returns error %d\n", ret);
        }
    }

    if (ret == 0 && max_data != 0) {
        test_ctx->cnx_client->maxdata_local = max_data;
        test_ctx->cnx_client->maxdata_remote = max_data;
        test_ctx->cnx_server->maxdata_local = max_data;
        test_ctx->cnx_server->maxdata_remote = max_data;
    }

    return ret;
}

int tls_api_one_scenario_body_verify(picoquic_test_tls_api_ctx_t* test_ctx,
    uint64_t * simulated_time,
    uint64_t max_completion_microsec)
{
    int ret = tls_api_one_scenario_verify(test_ctx);

    if (ret == 0) {
        ret = picoquic_close(test_ctx->cnx_client, 0);
        if (ret != 0)
        {
            DBG_PRINTF("Picoquic close returns %d\n", ret);
        }
    }

    if (ret == 0 && max_completion_microsec != 0) {
        if (*simulated_time > max_completion_microsec)
        {
            DBG_PRINTF("Scenario completes in %llu microsec, more than %llu\n",
                (unsigned long long)*simulated_time, (unsigned long long)max_completion_microsec);
            ret = -1;
        }
    }

    return ret;
}

int tls_api_one_scenario_body(picoquic_test_tls_api_ctx_t* test_ctx,
    uint64_t * simulated_time,
    test_api_stream_desc_t* scenario, size_t sizeof_scenario, size_t stream0_target,
    uint64_t init_loss_mask, uint64_t max_data, uint64_t queue_delay_max,
    uint64_t max_completion_microsec)
{
    uint64_t loss_mask = 0;
    int ret = tls_api_one_scenario_body_connect(test_ctx, simulated_time, stream0_target,
        max_data, queue_delay_max);

    /* Prepare to send data */
    if (ret == 0) {
        test_ctx->stream0_target = stream0_target;
        loss_mask = init_loss_mask;
        ret = test_api_init_send_recv_scenario(test_ctx, scenario, sizeof_scenario);

        if (ret != 0)
        {
            DBG_PRINTF("Init send receive scenario returns %d\n", ret);
        }
    }

    /* Perform a data sending loop */
    if (ret == 0) {
        ret = tls_api_data_sending_loop(test_ctx, &loss_mask, simulated_time, 0);

        if (ret != 0)
        {
            DBG_PRINTF("Data sending loop returns %d\n", ret);
        }
    }

    if (ret == 0) {
        ret = tls_api_one_scenario_body_verify(test_ctx, simulated_time, max_completion_microsec);
    }

    return ret;
}

int tls_api_one_scenario_test(test_api_stream_desc_t* scenario,
    size_t sizeof_scenario, size_t stream0_target,
    uint64_t init_loss_mask, uint64_t max_data, uint64_t queue_delay_max,
    uint32_t proposed_version, uint64_t max_completion_microsec,
    picoquic_tp_t * client_params, picoquic_tp_t * server_params) 
{
    uint64_t simulated_time = 0;
    picoquic_test_tls_api_ctx_t* test_ctx = NULL;

    int ret = tls_api_one_scenario_init(&test_ctx, &simulated_time,
        proposed_version, client_params, server_params);

    if (ret == 0) {
        ret = tls_api_one_scenario_body(test_ctx, &simulated_time,
            scenario, sizeof_scenario, stream0_target, init_loss_mask, max_data, queue_delay_max,
            max_completion_microsec);
    }

    if (test_ctx != NULL) {
        tls_api_delete_ctx(test_ctx);
        test_ctx = NULL;
    }

    return ret;
}

int tls_api_oneway_stream_test()
{
    return tls_api_one_scenario_test(test_scenario_oneway, sizeof(test_scenario_oneway), 0, 0, 0, 0, 0, 70000, NULL, NULL);
}

int tls_api_q_and_r_stream_test()
{
    return tls_api_one_scenario_test(test_scenario_q_and_r, sizeof(test_scenario_q_and_r), 0, 0, 0, 0, 0, 75000, NULL, NULL);
}

int tls_api_q2_and_r2_stream_test()
{
    return tls_api_one_scenario_test(test_scenario_q2_and_r2, sizeof(test_scenario_q2_and_r2), 0, 0, 0, 0, 0, 80000, NULL, NULL);
}

int tls_api_very_long_stream_test()
{
    return tls_api_one_scenario_test(test_scenario_very_long, sizeof(test_scenario_very_long), 0, 0, 0, 0, 0, 1000000, NULL, NULL);
}

int tls_api_very_long_max_test()
{
    return tls_api_one_scenario_test(test_scenario_very_long, sizeof(test_scenario_very_long), 0, 0, 128000, 0, 0, 1000000, NULL, NULL);
}

int tls_api_very_long_with_err_test()
{
    return tls_api_one_scenario_test(test_scenario_very_long, sizeof(test_scenario_very_long), 0, 0x30000, 128000, 0, 0, 2100000, NULL, NULL);
}

int tls_api_very_long_congestion_test()
{
    return tls_api_one_scenario_test(test_scenario_very_long, sizeof(test_scenario_very_long), 0, 0, 128000, 20000, 0, 1000000, NULL, NULL);
}

int unidir_test()
{
    return tls_api_one_scenario_test(test_scenario_unidir, sizeof(test_scenario_unidir), 0, 0, 128000, 10000, 0, 100000, NULL, NULL);
}

int many_short_loss_test()
{
    return tls_api_one_scenario_test(test_scenario_more_streams, sizeof(test_scenario_more_streams), 0, 0x882818A881288848ull, 16000, 2000, 0, 0, NULL, NULL);
}

/*
 * Server reset test.
 * Establish a connection between server and client.
 * When the connection is established, delete the server connection, and prime the client
 * to send data.
 * Expected result: the client sends a packet with a stream frame, the server responds
 * with a stateless reset, the client closes its own connection.
 */

int tls_api_server_reset_test()
{
    uint64_t simulated_time = 0;
    uint64_t loss_mask = 0;
    picoquic_test_tls_api_ctx_t* test_ctx = NULL;
    int ret = tls_api_init_ctx(&test_ctx, 0, PICOQUIC_TEST_SNI, PICOQUIC_TEST_ALPN, &simulated_time, NULL, NULL, 0, 0, 0);
    uint8_t buffer[128];
    int was_active = 0;

    if (ret == 0) {
        ret = tls_api_connection_loop(test_ctx, &loss_mask, 0, &simulated_time);
    }

    if (ret == 0) {
        ret = wait_application_aead_ready(test_ctx, &simulated_time);
    }

    /* verify that client and server have the same reset secret */
    if (ret == 0) {
        uint8_t ref_secret[PICOQUIC_RESET_SECRET_SIZE];

        (void)picoquic_create_cnxid_reset_secret(test_ctx->qserver,
            test_ctx->cnx_client->path[0]->remote_cnxid, ref_secret);
        if (memcmp(test_ctx->cnx_client->path[0]->reset_secret, ref_secret,
            PICOQUIC_RESET_SECRET_SIZE) != 0) {
            ret = -1;
        }
    }

    /* Prepare to reset */
    if (ret == 0) {
        picoquic_delete_cnx(test_ctx->cnx_server);
        test_ctx->cnx_server = NULL;

        memset(buffer, 0xaa, sizeof(buffer));
        ret = picoquic_add_to_stream(test_ctx->cnx_client, 4,
            buffer, sizeof(buffer), 1);
    }

    /* Perform a couple rounds of sending data */
    for (int i = 0; ret == 0 && i < 64 && test_ctx->cnx_client->cnx_state != picoquic_state_disconnected; i++) {
        was_active = 0;

        ret = tls_api_one_sim_round(test_ctx, &simulated_time, 0, &was_active);
    }

    /* Client should now be in state disconnected */
    if (ret == 0 && test_ctx->cnx_client->cnx_state != picoquic_state_disconnected) {
        ret = -1;
    }

    if (ret == 0 && test_ctx->reset_received == 0) {
        ret = -1;
    }
    if (test_ctx != NULL) {
        tls_api_delete_ctx(test_ctx);
        test_ctx = NULL;
    }

    return ret;
}

/*
* Server reset negative test.
* Establish a connection between server and client.
* When the connection is established, fabricate a bogus server reset and
* send it to the client.
* Expected result: the client ignores the bogus reset.
*/
int tls_api_bad_server_reset_test()
{
    uint64_t simulated_time = 0;
    uint64_t loss_mask = 0;
    picoquic_test_tls_api_ctx_t* test_ctx = NULL;
    int ret = tls_api_init_ctx(&test_ctx, 0, PICOQUIC_TEST_SNI, PICOQUIC_TEST_ALPN, &simulated_time, NULL, NULL, 0, 0, 0);
    uint8_t buffer[256];

    if (ret == 0) {
        ret = tls_api_connection_loop(test_ctx, &loss_mask, 0, &simulated_time);
    }

    /* Prepare the bogus reset */
    if (ret == 0) {
        size_t byte_index = 0;
        buffer[byte_index++] = 0x41;
        byte_index += picoquic_format_connection_id(&buffer[byte_index], sizeof(buffer) - byte_index, test_ctx->cnx_client->path[0]->local_cnxid);
        memset(buffer + byte_index, 0xcc, sizeof(buffer) - byte_index);
        
        /* Submit bogus request to client */
        ret = picoquic_incoming_packet(test_ctx->qclient, buffer, sizeof(buffer),
            (struct sockaddr*)(&test_ctx->server_addr),
            (struct sockaddr*)(&test_ctx->client_addr), 0, 0,
            simulated_time);
    }

    /* check that the client is still up */
    if (ret == 0 && !TEST_CLIENT_READY) {
        ret = -1;
    }

    if (test_ctx != NULL) {
        tls_api_delete_ctx(test_ctx);
        test_ctx = NULL;
    }

    return ret;
}

/*
 * verify that a connection is correctly established after a stateless retry,
 * and then verify that retyr is not used if tickets are available.
 */

static char const* token_file_name = "retry_tests_tokens.bin";

int tls_retry_token_test()
{
    uint64_t simulated_time = 0;
    uint64_t loss_mask = 0;
    picoquic_test_tls_api_ctx_t* test_ctx = NULL;
    /* ensure that the token file is empty */
    int ret = picoquic_save_tokens(NULL, simulated_time, token_file_name);
    
    if (ret == 0) {
        ret = tls_api_init_ctx(&test_ctx, 0, PICOQUIC_TEST_SNI, PICOQUIC_TEST_ALPN,
            &simulated_time, NULL, token_file_name, 0, 0, 0);
    }

    if (ret == 0) {
        /* Set the server in HRR/Cookies mode */
        picoquic_set_cookie_mode(test_ctx->qserver, 1);
        /* Try the connection */
        ret = tls_api_connection_loop(test_ctx, &loss_mask, 0, &simulated_time);
    }

    if (ret == 0) {
        /* Wait some time, so the connection can stabilize to ready state */
        ret = tls_api_synch_to_empty_loop(test_ctx, &simulated_time, 2048, PICOQUIC_NB_PATH_TARGET, 1);
    }

    if (ret == 0) {
        ret = tls_api_attempt_to_close(test_ctx, &simulated_time);
    }

    /* Now we remove the client connection and create a new one */
    if (ret == 0) {
        picoquic_delete_cnx(test_ctx->cnx_client);
        if (test_ctx->cnx_server != NULL) {
            picoquic_delete_cnx(test_ctx->cnx_server);
            test_ctx->cnx_server = NULL;
        }

        test_ctx->cnx_client = picoquic_create_cnx(test_ctx->qclient,
            picoquic_null_connection_id, picoquic_null_connection_id,
            (struct sockaddr*)&test_ctx->server_addr, 0,
            0, PICOQUIC_TEST_SNI, PICOQUIC_TEST_ALPN, 1);

        if (test_ctx->cnx_client == NULL) {
            ret = -1;
        }
        else {
            ret = picoquic_start_client_cnx(test_ctx->cnx_client);
        }
    }

    if (ret == 0) {
        /* Try the new connection */
        ret = tls_api_connection_loop(test_ctx, &loss_mask, 0, &simulated_time);
    }

    if (ret == 0) {
        ret = tls_api_attempt_to_close(test_ctx, &simulated_time);
    }

    if (ret == 0 && test_ctx->cnx_client->original_cnxid.id_len != 0) {
        DBG_PRINTF("Second retry did not use the stored token, odcil len=%d\n", 
            test_ctx->cnx_client->original_cnxid.id_len);
        ret = -1;
    }

    if (ret == 0) {
        /* Not strictly needed, but allows for inspection */
        ret = picoquic_save_tokens(test_ctx->qclient->p_first_token, simulated_time, token_file_name);
    }

    if (test_ctx != NULL) {
        tls_api_delete_ctx(test_ctx);
        test_ctx = NULL;
    }

    return ret;
}

int tls_api_retry_test()
{
    uint64_t simulated_time = 0;
    const uint64_t target_time = 210000ull;
    uint64_t loss_mask = 0;
    picoquic_test_tls_api_ctx_t* test_ctx = NULL;
    int ret = tls_api_init_ctx(&test_ctx, 0, PICOQUIC_TEST_SNI, PICOQUIC_TEST_ALPN, &simulated_time, NULL, NULL, 0, 0, 0);

    if (ret == 0) {
        /* Set the server in HRR/Cookies mode */
        picoquic_set_cookie_mode(test_ctx->qserver, 1);
        /* Try the connection */
        ret = tls_api_connection_loop(test_ctx, &loss_mask, 0, &simulated_time);
    }

    if (ret == 0) {
        ret = tls_api_attempt_to_close(test_ctx, &simulated_time);
    }

    if (ret == 0 && simulated_time > target_time) {
        DBG_PRINTF("Retry test completes in %llu microsec, more than %llu\n", simulated_time, target_time);
        ret = -1;
    }

    if (test_ctx != NULL) {
        tls_api_delete_ctx(test_ctx);
        test_ctx = NULL;
    }

    return ret;
}

/*
* verify that a connection is correctly established
* if the client does not initially provide a key share
*/

int tls_zero_share_test()
{
    uint64_t simulated_time = 0;
    uint64_t loss_mask = 0;
    picoquic_test_tls_api_ctx_t* test_ctx = NULL;
    int ret = tls_api_init_ctx(&test_ctx, 0, PICOQUIC_TEST_SNI, PICOQUIC_TEST_ALPN, &simulated_time, NULL, NULL, 1, 0, 0);

    if (ret == 0) {
        ret = tls_api_connection_loop(test_ctx, &loss_mask, 0, &simulated_time);
    }

    if (ret == 0) {
        ret = tls_api_attempt_to_close(test_ctx, &simulated_time);
    }

    if (test_ctx != NULL) {
        tls_api_delete_ctx(test_ctx);
        test_ctx = NULL;
    }

    return ret;
}


/*
 * Test two successive connections from the same client.
 */

int tls_api_two_connections_test()
{
    uint64_t simulated_time = 0;
    uint64_t loss_mask = 0;
    picoquic_test_tls_api_ctx_t* test_ctx = NULL;
    int ret = tls_api_init_ctx(&test_ctx, 0, PICOQUIC_TEST_SNI, PICOQUIC_TEST_ALPN, &simulated_time, NULL, NULL, 0, 0, 0);

    if (ret == 0) {
        ret = tls_api_connection_loop(test_ctx, &loss_mask, 0, &simulated_time);
    }

    if (ret == 0) {
        /* Verify that the connection is fully established */
        uint64_t target_time = simulated_time + 2000000;

        while (ret == 0 && TEST_CLIENT_READY && TEST_SERVER_READY && simulated_time < target_time) {
            int was_active = 0;
            ret = tls_api_one_sim_round(test_ctx, &simulated_time, target_time, &was_active);
        }

        /* Delete the client connection from the client context,
         * without sending notification to the server */
        while (test_ctx->qclient->cnx_list != NULL) {
            picoquic_delete_cnx(test_ctx->qclient->cnx_list);
        }

        /* Erase the server connection reference */
        test_ctx->cnx_server = NULL;

        /* Create a new connection in the client context */

        test_ctx->cnx_client = picoquic_create_cnx(test_ctx->qclient,
            picoquic_null_connection_id, picoquic_null_connection_id,
            (struct sockaddr*)&test_ctx->server_addr, simulated_time, 0, PICOQUIC_TEST_SNI, PICOQUIC_TEST_ALPN, 1);

        if (test_ctx->cnx_client == NULL) {
            ret = -1;
        }
        else {
            ret = picoquic_start_client_cnx(test_ctx->cnx_client);
        }
    }

    /* Now, restart a connection in the same context */
    if (ret == 0) {
        ret = tls_api_connection_loop(test_ctx, &loss_mask, 0, &simulated_time);
    }

    if (ret == 0) {
        ret = tls_api_attempt_to_close(test_ctx, &simulated_time);
    }

    if (test_ctx != NULL) {
        tls_api_delete_ctx(test_ctx);
        test_ctx = NULL;
    }

    return ret;
}

int tls_api_client_first_loss_test()
{
    return tls_api_loss_test(1ull);
}

int tls_api_client_second_loss_test()
{
    return tls_api_loss_test(2ull);
}

int tls_api_server_first_loss_test()
{
    return tls_api_loss_test(14ull);
}

int tls_api_client_losses_test()
{
    return tls_api_loss_test(3ull);
}

int tls_api_server_losses_test()
{
    return tls_api_loss_test(6ull);
}

/*
 * Do a simple test for all supported versions
 */
int tls_api_multiple_versions_test()
{
    int ret = 0;

    for (size_t i = 1; ret == 0 && i < picoquic_nb_supported_versions; i++) {
        ret = tls_api_one_scenario_test(test_scenario_q_and_r, sizeof(test_scenario_q_and_r), 0, 0, 0, 0,
            picoquic_supported_versions[i].version, 0, NULL, NULL);
    }

    return ret;
}

/*
 * Keep alive test.
 */

int keep_alive_test_impl(int keep_alive)
{
    uint64_t simulated_time = 0;
    const uint64_t keep_alive_interval = 0; /* Will use the default value */
    uint64_t loss_mask = 0;
    picoquic_test_tls_api_ctx_t* test_ctx = NULL;
    int ret = tls_api_init_ctx(&test_ctx, 0, PICOQUIC_TEST_SNI, PICOQUIC_TEST_ALPN, &simulated_time, NULL, NULL, 0, 0, 0);
    int was_active = 0;

    if (ret == 0 && test_ctx == NULL) {
        return PICOQUIC_ERROR_MEMORY;
    }

    /*
     * setup the connections.
     */

    if (ret == 0) {
        ret = tls_api_connection_loop(test_ctx, &loss_mask, 0, &simulated_time);
    }

    /*
     * Enable keep alive
     */
    if (ret == 0 && keep_alive) {
        picoquic_enable_keep_alive(test_ctx->cnx_client, keep_alive_interval);
    }

    /* Perform rounds of sending data until the requested time has been spent */
    for (int i = 0; ret == 0 && i < 0x10000 && test_ctx->cnx_client->cnx_state != picoquic_state_disconnected ; i++) {
        was_active = 0;

        ret = tls_api_one_sim_round(test_ctx, &simulated_time, 0, &was_active);
        if (simulated_time > 2 * PICOQUIC_MICROSEC_SILENCE_MAX) {
            break;
        }
    }

    /* Check that the status matched the expected value */
    if (test_ctx == NULL || test_ctx->cnx_client == NULL) {
        ret = -1;
    } else if (keep_alive != 0) {
        if (!TEST_CLIENT_READY) {
            ret = -1;
        } else if (simulated_time < 2 * PICOQUIC_MICROSEC_SILENCE_MAX) {
            DBG_PRINTF("Keep alive test concludes after %llu microsecs instead of %llu, ret = %d\n",
                (unsigned long long)simulated_time, (unsigned long long)2 * PICOQUIC_MICROSEC_SILENCE_MAX, ret);
            ret = -1;
        } 
    } else if (keep_alive == 0) {
        /* If keep alive was not activated, reset ret to `0`, as `tls_api_one_sim_round` returns -1
         * when the connection was disconnected.
         */
        ret = test_ctx->cnx_client->cnx_state != picoquic_state_disconnected;
    }

    /* Close the connection */
    if (ret == 0 && test_ctx->cnx_client->cnx_state != picoquic_state_disconnected) {
        ret = tls_api_attempt_to_close(test_ctx, &simulated_time);
    }

    /* Clean up */
    if (test_ctx != NULL) {
        tls_api_delete_ctx(test_ctx);
        test_ctx = NULL;
    }

    return ret;
}

int keep_alive_test()
{
    int ret = keep_alive_test_impl(1);

    if (ret == 0) {
        ret = keep_alive_test_impl(0);
    }

    return ret;
}

/*
 * Session resume test.
 */
static char const* ticket_file_name = "resume_tests_tickets.bin";

int session_resume_wait_for_ticket(picoquic_test_tls_api_ctx_t* test_ctx,
    uint64_t * simulated_time) 
{
    int ret = 0;
    uint64_t time_out = *simulated_time + 4000000;
    int nb_trials = 0;
    int nb_inactive = 0;

    while (*simulated_time <time_out &&
        TEST_CLIENT_READY &&
        TEST_SERVER_READY &&
        test_ctx->qclient->p_first_ticket == NULL &&
        nb_trials < 1024 &&
        nb_inactive < 64 &&
        ret == 0){
        int was_active = 0;
        nb_trials++;

        ret = tls_api_one_sim_round(test_ctx, simulated_time, time_out, &was_active);

        if (was_active) {
            nb_inactive = 0;
        }
        else {
            nb_inactive++;
        }
    }
    
    return ret;
}

int session_resume_test()
{
    uint64_t simulated_time = 0;
    picoquic_test_tls_api_ctx_t* test_ctx = NULL;
    char const* sni = PICOQUIC_TEST_SNI;
    char const* alpn = PICOQUIC_TEST_ALPN;
    uint64_t loss_mask = 0;
    int ret = 0;

    /* Initialize an empty ticket store */
    ret = picoquic_save_tickets(NULL, simulated_time, ticket_file_name);

    for (int i = 0; i < 2; i++) {
        /* Set up the context, while setting the ticket store parameter for the client */
        if (ret == 0) {
            ret = tls_api_init_ctx(&test_ctx, 0, sni, alpn, &simulated_time, ticket_file_name, NULL, 0, 0, 0);
        }

        if (ret == 0) {
            test_ctx->cnx_client->max_early_data_size = 0;

            ret = tls_api_connection_loop(test_ctx, &loss_mask, 0, &simulated_time);
        }

        if (ret == 0 && i == 1) {
            /* If resume succeeded, the second connection will have a type "PSK" */
            if (picoquic_tls_is_psk_handshake(test_ctx->cnx_server) == 0 || picoquic_tls_is_psk_handshake(test_ctx->cnx_client) == 0) {
                ret = -1;
            }
        }

        if (ret == 0 && i == 0) {
            /* Before closing, wait for the session ticket to arrive */
            ret = session_resume_wait_for_ticket(test_ctx, &simulated_time);
        }

        if (ret == 0) {
            ret = tls_api_attempt_to_close(test_ctx, &simulated_time);
        }

        /* Verify that the session ticket has been received correctly */
        if (ret == 0) {
            if (test_ctx->qclient->p_first_ticket == NULL) {
                ret = -1;
            } else {
                ret = picoquic_save_tickets(test_ctx->qclient->p_first_ticket, simulated_time, ticket_file_name);
            }
        }
        /* Tear down and free everything */

        if (test_ctx != NULL) {
            tls_api_delete_ctx(test_ctx);
            test_ctx = NULL;
        }
    }

    return ret;
}

/*
 * Zero RTT test. Like the session resume test, but with a twist...
 */
int zero_rtt_test_one(int use_badcrypt, int hardreset, unsigned int early_loss)
{
    uint64_t simulated_time = 0;
    picoquic_test_tls_api_ctx_t* test_ctx = NULL;
    char const* sni = PICOQUIC_TEST_SNI;
    char const* alpn = PICOQUIC_TEST_ALPN;
    uint64_t loss_mask = 0;
    uint32_t proposed_version = 0;
    int ret = 0;

    /* Initialize an empty ticket store */
    ret = picoquic_save_tickets(NULL, simulated_time, ticket_file_name);

    for (int i = 0; i < 2; i++) {
        /* Set up the context, while setting the ticket store parameter for the client */
        if (ret == 0) {
            ret = tls_api_init_ctx(&test_ctx, 
                (i==0)?0: proposed_version, sni, alpn, &simulated_time, ticket_file_name, NULL, 0, 0,
                (i == 0)?0:use_badcrypt);

            if (ret == 0 && hardreset != 0 && i == 1) {
                picoquic_set_cookie_mode(test_ctx->qserver, 1);
            }
        }

        if (ret == 0 && i == 1) {
            uint8_t test_data[8] = { 't', 'e', 's', 't', '0', 'r', 't', 't' };
            /* set the link delays to 100 ms, for realistic testing */
            test_ctx->c_to_s_link->microsec_latency = 100000ull;
            test_ctx->s_to_c_link->microsec_latency = 100000ull;

            /* Queue an initial frame on the client connection */
            (void)picoquic_add_to_stream(test_ctx->cnx_client, 0, test_data, sizeof(test_data), 1);

            if (early_loss > 0) {
                loss_mask = 1ull << (early_loss - 1);
            }
        }

        if (ret == 0) {
            ret = tls_api_connection_loop(test_ctx, &loss_mask, 0, &simulated_time);

            if (ret != 0) {
                DBG_PRINTF("Zero RTT test (badcrypt: %d, hard: %d), connection %d fails (0x%x)\n",
                    use_badcrypt, hardreset, i, ret);
            }
        }

        if (ret == 0 && i == 1) {
            /* If resume succeeded, the second connection will have a type "PSK" */
            if (use_badcrypt == 0 && hardreset == 0 && (
                picoquic_tls_is_psk_handshake(test_ctx->cnx_server) == 0 || 
                picoquic_tls_is_psk_handshake(test_ctx->cnx_client) == 0)) {
                DBG_PRINTF("Zero RTT test (badcrypt: %d, hard: %d), connection %d not PSK.\n",
                    use_badcrypt, hardreset, i);
                ret = -1;
            } else {
                /* run a receive loop until no outstanding data */
                ret = tls_api_synch_to_empty_loop(test_ctx, &simulated_time, 2048, 0, 0);
            }
        }

        if (ret == 0 && i == 0) {
            /* Before closing, wait for the session ticket to arrive */
            ret = session_resume_wait_for_ticket(test_ctx, &simulated_time);
        }

        if (ret == 0) {
            ret = tls_api_attempt_to_close(test_ctx, &simulated_time);

            if (ret != 0) {
                DBG_PRINTF("Zero RTT test (badcrypt: %d, hard: %d), connection %d close error (0x%x).\n",
                    use_badcrypt, hardreset, i, ret);
            }
        }

        /* Verify that the 0RTT data was sent and acknowledged */
        if (ret == 0 && i == 1) {
            if (use_badcrypt == 0 && hardreset == 0) {
                if (test_ctx->cnx_client->nb_zero_rtt_sent == 0) {
                    DBG_PRINTF("Zero RTT test (badcrypt: %d, hard: %d), no zero RTT sent.\n",
                        use_badcrypt, hardreset);
                    ret = -1;
                }
                else if (early_loss == 0 &&
                    test_ctx->cnx_client->nb_zero_rtt_acked != test_ctx->cnx_client->nb_zero_rtt_sent) {
                    DBG_PRINTF("Zero RTT test (badcrypt: %d, hard: %d), no zero RTT acked.\n",
                        use_badcrypt, hardreset);
                    ret = -1;
                }
            } else {
                if (test_ctx->cnx_client->nb_zero_rtt_sent == 0) {
                    DBG_PRINTF("Zero RTT test (badcrypt: %d, hard: %d), no zero RTT sent.\n",
                        use_badcrypt, hardreset);
                    ret = -1;
                }
                else if (early_loss == 0 && hardreset == 0 && test_ctx->cnx_client->nb_zero_rtt_acked != 0) {
                    DBG_PRINTF("Zero RTT test (badcrypt: %d, hard: %d), zero acked, not expected.\n",
                        use_badcrypt, hardreset);
                    ret = -1;
                }
                else if (test_ctx->sum_data_received_at_server == 0) {
                    DBG_PRINTF("Zero RTT test (badcrypt: %d, hard: %d, loss: %d), no data received.\n",
                        use_badcrypt, hardreset, early_loss);
                    ret = -1;
                }
            }
        }

        /* Verify that the session ticket has been received correctly */
        if (ret == 0) {
            if (test_ctx->qclient->p_first_ticket == NULL) {
                DBG_PRINTF("Zero RTT test (badcrypt: %d, hard: %d), cnx %d, no ticket received.\n",
                    use_badcrypt, hardreset, i);
                ret = -1;
            } else {
                ret = picoquic_save_tickets(test_ctx->qclient->p_first_ticket, simulated_time, ticket_file_name);
                DBG_PRINTF("Zero RTT test (badcrypt: %d, hard: %d), cnx %d, ticket save error (0x%x).\n",
                    use_badcrypt, hardreset, i, ret);
            }
        }

        /* Tear down and free everything */
        if (test_ctx != NULL) {
            tls_api_delete_ctx(test_ctx);
            test_ctx = NULL;
        }
    }

    return ret;
}

/* 
* Basic 0-RTT test. Verify that things work in the absence of loss 
*/

int zero_rtt_test()
{
    return zero_rtt_test_one(0, 0, 0);
}

/*
* zero rtt test with losses. Verify that the connection setup works even 
* if packets are lost. The "loss test" indicates which packet will be lost
* during the exchange. As the code stands for draft-13, the EOED is sent in
* a zero RTT packet, the 9th packet on the connection. This order is
* however very dependent on the details of the implementation. To be on the safe
* side, we should repeat the test while emulating the loss of any packet
* between 1 and 16.
*/

int zero_rtt_loss_test()
{
    int ret = 0;

    for (unsigned int i = 1; ret == 0 && i < 16; i++) {
        ret = zero_rtt_test_one(0, 0, i);
        if (ret != 0) {
            DBG_PRINTF("Zero RTT test fails when packet #%d is lost.\n", i);
        }
    }

    return ret;
}
/*
* Zero Spurious RTT test.
* Check what happens if the client attempts to resume a connection using a bogus ticket.
* This will cause a connection retry of some kind, the 0rtt packet will be lost.
* This is simulated by runnig the zero-rtt code, but using a different
* ticket key for the second server instance.
*/

int zero_rtt_spurious_test()
{
    return zero_rtt_test_one(1, 0, 0);
}

/*
* Zero RTT Retry test.
* Check what happens if the client attempts to resume a connection but the
* server responds with a retry. This is simulated by activating the retry
* mode on the server between the 2 client connections.
*/

int zero_rtt_retry_test()
{
    return zero_rtt_test_one(0, 1, 0);
}

/*
 * Stop sending test. Start a long transmission, but after receiving some bytes,
 * send a stop sending request. Then ask for another transmission. The
 * test succeeds if only few bytes of the first are received, and all bytes
 * of the second.
 */

int stop_sending_test()
{
    uint64_t simulated_time = 0;
    uint64_t loss_mask = 0;
    picoquic_test_tls_api_ctx_t* test_ctx = NULL;
    int ret = tls_api_init_ctx(&test_ctx, PICOQUIC_INTERNAL_TEST_VERSION_1,
        PICOQUIC_TEST_SNI, PICOQUIC_TEST_ALPN, &simulated_time, NULL, NULL, 0, 0, 0);
    int nb_initial_loop = 0;

    if (ret == 0) {
        ret = tls_api_connection_loop(test_ctx, &loss_mask, 0, &simulated_time);
    }

    /* Prepare to send data */
    if (ret == 0) {
        ret = test_api_init_send_recv_scenario(test_ctx, test_scenario_stop_sending, sizeof(test_scenario_stop_sending));
    }

    /* Perform a data sending loop for a few rounds, until some bytes are received on the first stream */
    while (ret == 0 && nb_initial_loop < 64) {
        nb_initial_loop++;

        ret = tls_api_data_sending_loop(test_ctx, &loss_mask, &simulated_time, 16);

        if (test_ctx->test_stream[0].r_recv_nb != 0) {
            break;
        }
    }

    /* issue the stop sending command */
    if (ret == 0 && test_ctx->cnx_client != NULL) {
        ret = picoquic_stop_sending(test_ctx->cnx_client, test_scenario_stop_sending[0].stream_id, 1);
    }

    /* resume the sending scenario */
    if (ret == 0) {
        ret = tls_api_data_sending_loop(test_ctx, &loss_mask, &simulated_time, 0);
    }

    if (ret == 0) {
        if (test_ctx->server_callback.error_detected) {
            ret = -1;
        } else if (test_ctx->client_callback.error_detected) {
            ret = -1;
        } else {
            for (size_t i = 0; ret == 0 && i < test_ctx->nb_test_streams; i++) {
                if (test_ctx->test_stream[i].q_recv_nb != test_ctx->test_stream[i].q_len) {
                    ret = -1;
                } else if (i == 0 && test_ctx->test_stream[i].r_recv_nb == test_ctx->test_stream[i].r_len) {
                    ret = -1;
                } else if (i != 0 && test_ctx->test_stream[i].r_recv_nb != test_ctx->test_stream[i].r_len) {
                    ret = -1;
                } else if (test_ctx->test_stream[i].q_received == 0 || test_ctx->test_stream[i].r_received == 0) {
                    ret = -1;
                }
            }
        }
    }

    if (ret == 0) {
        ret = picoquic_close(test_ctx->cnx_client, 0);
    }

    if (test_ctx != NULL) {
        tls_api_delete_ctx(test_ctx);
        test_ctx = NULL;
    }

    return ret;
}

/*
* MTU discovery test. Perform a moderate transmission.
* Verify that MTU was properly set to expected value
*/

int mtu_discovery_test()
{
    uint64_t simulated_time = 0;
    uint64_t loss_mask = 0;
    picoquic_test_tls_api_ctx_t* test_ctx = NULL;
    int ret = tls_api_init_ctx(&test_ctx, PICOQUIC_INTERNAL_TEST_VERSION_1,
        PICOQUIC_TEST_SNI, PICOQUIC_TEST_ALPN, &simulated_time, NULL, NULL, 0, 0, 0);

    if (ret == 0) {
        ret = tls_api_connection_loop(test_ctx, &loss_mask, 0, &simulated_time);
    }

    /* Prepare to send data */
    if (ret == 0) {
        ret = test_api_init_send_recv_scenario(test_ctx, test_scenario_mtu_discovery, sizeof(test_scenario_mtu_discovery));
    }

    /* Perform a data sending loop */
    if (ret == 0) {
        ret = tls_api_data_sending_loop(test_ctx, &loss_mask, &simulated_time, 0);
    }

    if (ret == 0) {
        if (test_ctx->cnx_client->path[0]->send_mtu != test_ctx->cnx_server->local_parameters.max_packet_size) {
            ret = -1;
        } else if (test_ctx->cnx_server->path[0]->send_mtu != test_ctx->cnx_client->local_parameters.max_packet_size) {
            ret = -1;
        }
    }

    if (test_ctx != NULL) {
        tls_api_delete_ctx(test_ctx);
        test_ctx = NULL;
    }

    return ret;
}

/*
 * Trying to reproduce the scenario that resulted in
 * spurious retransmissions,and checking that it is fixed.
 */

int spurious_retransmit_test()
{
    uint64_t loss_mask = 0;
    uint64_t simulated_time = 0;
    uint64_t next_time = 0;
    picoquic_test_tls_api_ctx_t* test_ctx = NULL;
    int ret = tls_api_init_ctx(&test_ctx, 0, PICOQUIC_TEST_SNI, PICOQUIC_TEST_ALPN, &simulated_time, NULL, NULL, 0, 0, 0);

    if (ret == 0) {
        test_ctx->c_to_s_link->microsec_latency = 50000ull;
        test_ctx->s_to_c_link->microsec_latency = 50000ull;

        ret = tls_api_connection_loop(test_ctx, &loss_mask, 0, &simulated_time);
    }

    /* simulate 1 second of silence */
    next_time = simulated_time + 1000000ull;
    while (ret == 0 && simulated_time < next_time && TEST_CLIENT_READY && TEST_SERVER_READY) {
        int was_active = 0;

        ret = tls_api_one_sim_round(test_ctx, &simulated_time, next_time, &was_active);
    }

    if (ret == 0) {
        ret = tls_api_attempt_to_close(test_ctx, &simulated_time);
    }

    if (ret == 0) {
        /* verify the absence of any spurious retransmission */
        if (test_ctx->cnx_client->nb_spurious != 0) {
            ret = -1;
        } else if (test_ctx->cnx_server != NULL && test_ctx->cnx_server->nb_spurious != 0) {
            ret = -1;
        }
    }

    if (test_ctx != NULL) {
        tls_api_delete_ctx(test_ctx);
        test_ctx = NULL;
    }

    return ret;
}

/*
* Set up a connection, and verify
* that the key generated for PN encryption on
* client and server produce the correct results.
*/

int pn_enc_1rtt_test()
{
    uint64_t loss_mask = 0;
    uint64_t simulated_time = 0;
    picoquic_test_tls_api_ctx_t* test_ctx = NULL;
    int ret = tls_api_init_ctx(&test_ctx, 0, PICOQUIC_TEST_SNI, PICOQUIC_TEST_ALPN, &simulated_time, NULL, NULL, 0, 0, 0);

    if (ret == 0) {
        ret = tls_api_connection_loop(test_ctx, &loss_mask, 0, &simulated_time);
    }

    if (ret == 0) {
        ret = wait_application_aead_ready(test_ctx, &simulated_time);
    }

    if (ret == 0)
    {
        /* Try to encrypt a sequence number */
        uint8_t seq_num_1[4] = { 0xde, 0xad, 0xbe, 0xef };
        uint8_t sample_1[16] = {
            0x6b, 0xc1, 0xbe, 0xe2, 0x2e, 0x40, 0x9f, 0x96,
            0xe9, 0x3d, 0x7e, 0x11, 0x73, 0x93, 0x17, 0x2a };
        uint8_t seq_num_2[4] = { 0xba, 0xba, 0xc0, 0x0l };
        uint8_t sample_2[16] = {
            0xe9, 0x3d, 0x7e, 0x11, 0x73, 0x93, 0x17, 0x2a,
            0x6b, 0xc1, 0xbe, 0xe2, 0x2e, 0x40, 0x9f, 0x96 };

        for (int i = 1; i < 4; i *= 2)
        {
            ret = test_one_pn_enc_pair(seq_num_1, 4, test_ctx->cnx_client->crypto_context[3].pn_enc, test_ctx->cnx_server->crypto_context[3].pn_dec, sample_1);

            if (ret == 0)
            {
                ret = test_one_pn_enc_pair(seq_num_2, 4, test_ctx->cnx_server->crypto_context[3].pn_enc, test_ctx->cnx_client->crypto_context[3].pn_dec, sample_2);
            }
        }
    }

    if (test_ctx != NULL) {
        tls_api_delete_ctx(test_ctx);
        test_ctx = NULL;
    }

    return ret;
}

int bad_certificate_test()
{
    uint64_t simulated_time = 0;
    uint64_t loss_mask = 0;
    picoquic_test_tls_api_ctx_t* test_ctx = NULL;
    char test_server_cert_file[512];
    char test_server_key_file[512];
    char test_server_cert_store_file[512];
    int ret = tls_api_init_ctx(&test_ctx, 0, PICOQUIC_TEST_SNI, PICOQUIC_TEST_ALPN, &simulated_time, NULL, NULL, 0, 0, 0);

    if (ret == 0) {
        ret = picoquic_get_input_path(test_server_cert_file, sizeof(test_server_cert_file), picoquic_test_solution_dir, PICOQUIC_TEST_FILE_SERVER_BAD_CERT);

        if (ret == 0) {
            ret = picoquic_get_input_path(test_server_key_file, sizeof(test_server_key_file), picoquic_test_solution_dir, PICOQUIC_TEST_FILE_SERVER_KEY);
        }

        if (ret == 0) {
            ret = picoquic_get_input_path(test_server_cert_store_file, sizeof(test_server_cert_store_file), picoquic_test_solution_dir, PICOQUIC_TEST_FILE_CERT_STORE);
        }

        if (ret != 0) {
            DBG_PRINTF("%s", "Cannot set the cert, key or store file names.\n");
        }
    }

    /* Delete the server context, and recreate it with the bad certificate */

    if (ret == 0)
    {
        if (test_ctx->qserver != NULL) {
            picoquic_free(test_ctx->qserver);
        }

        test_ctx->qserver = picoquic_create(8,
            test_server_cert_file, test_server_key_file, test_server_cert_store_file,
            PICOQUIC_TEST_ALPN, test_api_callback, (void*)&test_ctx->server_callback, NULL, NULL, NULL,
            simulated_time, &simulated_time, NULL,
            test_ticket_encrypt_key, sizeof(test_ticket_encrypt_key));

        if (test_ctx->qserver == NULL) {
            ret = -1;
        }
    }

    /* Proceed with the connection loop. It should fail, and thus we don't test the return code */
    if (ret == 0) {
        (void)tls_api_connection_loop(test_ctx, &loss_mask, 0, &simulated_time);

        if (test_ctx->cnx_client == NULL) {
            ret = -1;
        }
        else if (test_ctx->cnx_client->cnx_state != picoquic_state_disconnected) {
            ret = -1;
        }
        else if (!picoquic_is_handshake_error(picoquic_get_local_error(test_ctx->cnx_client))) {
            ret = -1;
        }
        else if (!picoquic_is_handshake_error(picoquic_get_remote_error(test_ctx->cnx_server))) {
            ret = -1;
        }
        else {
            ret = 0;
        }
    }

    if (test_ctx != NULL) {
        tls_api_delete_ctx(test_ctx);
        test_ctx = NULL;
    }

    return ret;
}

/*
* Test setting the verify certificate callback.
*/

static int verify_sign_test(void* verify_ctx, ptls_iovec_t data, ptls_iovec_t sign) {
    int* ptr = (int*)verify_ctx;
    *ptr += 1;

    return 0;
}

static int verify_certificate_test(void* ctx, picoquic_cnx_t* cnx, ptls_iovec_t* certs, size_t num_certs,
                                   picoquic_verify_sign_cb_fn* verify_sign, void** verify_sign_ctx) {
    int* data = (int*)ctx;
    *data += 1;

    *verify_sign = verify_sign_test;
    *verify_sign_ctx = ctx;

    return 0;
}

int set_verify_certificate_callback_test()
{
    uint64_t simulated_time = 0;
    uint64_t loss_mask = 0;
    picoquic_test_tls_api_ctx_t* test_ctx = NULL;
    int call_count = 0;
    char test_server_cert_file[512];
    char test_server_key_file[512];
    char test_server_cert_store_file[512];
    int ret = tls_api_init_ctx(&test_ctx, PICOQUIC_INTERNAL_TEST_VERSION_1,
        PICOQUIC_TEST_SNI, PICOQUIC_TEST_ALPN, &simulated_time, NULL, NULL, 0, 0, 0);

    if (ret == 0) {
        ret = picoquic_get_input_path(test_server_cert_file, sizeof(test_server_cert_file), picoquic_test_solution_dir, PICOQUIC_TEST_FILE_SERVER_CERT);
    }

    if (ret == 0) {
        ret = picoquic_get_input_path(test_server_key_file, sizeof(test_server_key_file), picoquic_test_solution_dir, PICOQUIC_TEST_FILE_SERVER_KEY);
    }

    if (ret == 0) {
        ret = picoquic_get_input_path(test_server_cert_store_file, sizeof(test_server_cert_store_file), picoquic_test_solution_dir, PICOQUIC_TEST_FILE_CERT_STORE);
    }

    if (ret != 0) {
        DBG_PRINTF("%s", "Cannot set the cert, key or store file names.\n");
    }

    /* Delete the client context, and recreate with a certificate */
    if (ret == 0) {
        if (test_ctx->qclient != NULL) {
            picoquic_free(test_ctx->qclient);
            test_ctx->cnx_client = NULL;
        }

        test_ctx->qclient = picoquic_create(8,
            test_server_cert_file, test_server_key_file, test_server_cert_store_file,
            NULL, test_api_callback, (void*)&test_ctx->client_callback, NULL, NULL, NULL,
            simulated_time, &simulated_time, NULL, NULL, 0);

        if (test_ctx->qclient == NULL) {
            ret = -1;
        }
    }

    /* recreate the client connection */
    if (ret == 0) {
        test_ctx->cnx_client = picoquic_create_cnx(test_ctx->qclient, picoquic_null_connection_id,
                                                   picoquic_null_connection_id,
                                                   (struct sockaddr*)&test_ctx->server_addr, 0,
                                                   0, PICOQUIC_TEST_SNI, PICOQUIC_TEST_ALPN, 1);

        if (test_ctx->cnx_client == NULL) {
            ret = -1;
        } else {
            ret = picoquic_start_client_cnx(test_ctx->cnx_client);
        }
    }

    /* Set the verify callback for the client */
    if (ret == 0) {
        ret = picoquic_set_verify_certificate_callback(test_ctx->qclient, verify_certificate_test,
                                                       &call_count, NULL);
    }

    /* Set the verify callback for the server */
    if (ret == 0) {
        ret = picoquic_set_verify_certificate_callback(test_ctx->qserver, verify_certificate_test,
                                                       &call_count, NULL);
    }

    /* Activate client authentication */
    if (ret == 0) {
        picoquic_set_client_authentication(test_ctx->qserver, 1);

        ret = tls_api_connection_loop(test_ctx, &loss_mask, 0, &simulated_time);
    }

    if (ret == 0 && call_count != 4) {
        ret = -1;
    }

    if (test_ctx != NULL) {
        tls_api_delete_ctx(test_ctx);
        test_ctx = NULL;
    }

    return ret;
}

/*
 * Verify that the simulated time works as expected
 */

int virtual_time_test()
{
    int ret = 0;
    uint64_t test_time = 0;
    uint64_t simulated_time = 0;
    uint64_t current_time = picoquic_current_time();
    uint64_t ptls_time = 0;
    uint8_t callback_ctx[256];
    char test_server_cert_store_file[512];
    picoquic_quic_t * qsimul = NULL;
    picoquic_quic_t * qdirect = NULL;

    ret = picoquic_get_input_path(test_server_cert_store_file, sizeof(test_server_cert_store_file), picoquic_test_solution_dir, PICOQUIC_TEST_FILE_CERT_STORE);

    if (ret != 0) {
        DBG_PRINTF("%s", "Cannot set the cert, key or store file names.\n");
    }
    else {

        qsimul = picoquic_create(8, NULL, NULL, test_server_cert_store_file,
            NULL, test_api_callback,
            (void*)callback_ctx, NULL, NULL, NULL, simulated_time,
            &simulated_time, ticket_file_name, NULL, 0);
        qdirect = picoquic_create(8, NULL, NULL, PICOQUIC_TEST_FILE_CERT_STORE,
            NULL, test_api_callback,
            (void*)callback_ctx, NULL, NULL, NULL, current_time,
            NULL, ticket_file_name, NULL, 0);

        if (qsimul == NULL || qdirect == NULL)
        {
            ret = -1;
        }
        else
        {
            /* Check that the simulated time follows the simulation */
            for (int i = 0; ret == 0 && i < 5; i++) {
                simulated_time += 12345678;
                test_time = picoquic_get_quic_time(qsimul);
                ptls_time = picoquic_get_tls_time(qsimul);
                if (test_time != simulated_time) {
                    DBG_PRINTF("Test time: %llu != Simulated: %llu",
                        (unsigned long long)test_time,
                        (unsigned long long)simulated_time);
                    ret = -1;
                }
                else if (ptls_time < (test_time / 1000) || ptls_time >(test_time / 1000) + 1) {
                    DBG_PRINTF("Test time: %llu does match ptls time: %llu",
                        (unsigned long long)test_time,
                        (unsigned long long)ptls_time);
                    ret = -1;
                }
            }
        }

        /* Check that the non simulated time follows the current time */
        for (int i = 0; ret == 0 && i < 5; i++) {
#ifdef _WINDOWS
            Sleep(1);
#else
            usleep(1000);
#endif
            current_time = picoquic_current_time();
            test_time = picoquic_get_quic_time(qdirect);
            ptls_time = picoquic_get_tls_time(qdirect);

            if (test_time < current_time) {
                DBG_PRINTF("Test time: %llu < previous current time: %llu",
                    (unsigned long long)test_time,
                    (unsigned long long)current_time);
                ret = -1;
            }
            else {
                current_time = picoquic_current_time();
                if (test_time > current_time) {
                    DBG_PRINTF("Test time: %llu > next current time: %llu",
                        (unsigned long long)test_time,
                        (unsigned long long)current_time);
                    ret = -1;
                }
                else if (ptls_time < (test_time / 1000) || ptls_time >(test_time / 1000) + 1) {
                    DBG_PRINTF("Test current time: %llu does match ptls time: %llu",
                        (unsigned long long)test_time,
                        (unsigned long long)ptls_time);
                    ret = -1;
                }
            }
        }
    }

    if (qsimul != NULL)
    {
        picoquic_free(qsimul);
        qsimul = NULL;
    }

    if (qdirect != NULL)
    {
        picoquic_free(qdirect);
        qdirect = NULL;
    }

    return ret;
}

/*
 * Testing with different initial connection parameters
 */

int tls_different_params_test()
{
    picoquic_tp_t test_parameters;

    memset(&test_parameters, 0, sizeof(picoquic_tp_t));

    picoquic_init_transport_parameters(&test_parameters, 1);

    test_parameters.initial_max_stream_id_bidir = 0;

    return tls_api_one_scenario_test(test_scenario_very_long, sizeof(test_scenario_very_long), 0, 0, 0, 0, 0, 3510000, &test_parameters, NULL);
}

int tls_quant_params_test()
{
    picoquic_tp_t test_parameters;

    memset(&test_parameters, 0, sizeof(picoquic_tp_t));

    picoquic_init_transport_parameters(&test_parameters, 1);

    test_parameters.initial_max_data = 0x4000;
    test_parameters.initial_max_stream_id_bidir = 0;
    test_parameters.initial_max_stream_id_unidir = 65535;
    test_parameters.initial_max_stream_data_bidi_local = 0x2000;
    test_parameters.initial_max_stream_data_bidi_remote = 0x2000;
    test_parameters.initial_max_stream_data_uni = 0x2000;

    return tls_api_one_scenario_test(test_scenario_quant, sizeof(test_scenario_quant), 0, 0, 0, 0, 0, 3510000, &test_parameters, NULL);
}

int set_certificate_and_key_test()
{
    uint64_t simulated_time = 0;
    uint64_t loss_mask = 0;
    picoquic_test_tls_api_ctx_t* test_ctx = NULL;
    char test_server_cert_file[512];
    char test_server_key_file[512];
    char test_server_cert_store_file[512];
    int ret = tls_api_init_ctx(&test_ctx, 0, PICOQUIC_TEST_SNI, PICOQUIC_TEST_ALPN, &simulated_time, NULL, NULL, 0, 0, 0);

    if (ret == 0) {
        ret = picoquic_get_input_path(test_server_cert_file, sizeof(test_server_cert_file), picoquic_test_solution_dir, PICOQUIC_TEST_FILE_SERVER_CERT);
    }

    if (ret == 0) {
        ret = picoquic_get_input_path(test_server_key_file, sizeof(test_server_key_file), picoquic_test_solution_dir, PICOQUIC_TEST_FILE_SERVER_KEY);
    }

    if (ret == 0) {
        ret = picoquic_get_input_path(test_server_cert_store_file, sizeof(test_server_cert_store_file), picoquic_test_solution_dir, PICOQUIC_TEST_FILE_CERT_STORE);
    }

    if (ret != 0) {
        DBG_PRINTF("%s", "Cannot set the cert, key or store file names.\n");
    }

    /* Delete the server context, and recreate it. */
    if (ret == 0)
    {
        if (test_ctx->qserver != NULL) {
            picoquic_free(test_ctx->qserver);
        }

        test_ctx->qserver = picoquic_create(8,
            NULL, NULL, NULL,
            PICOQUIC_TEST_ALPN, test_api_callback, (void*)&test_ctx->server_callback, NULL, NULL, NULL,
            simulated_time, &simulated_time, NULL,
            test_ticket_encrypt_key, sizeof(test_ticket_encrypt_key));

        if (test_ctx->qserver == NULL) {
            ret = -1;
        }

        if (ret == 0) {
            BIO* bio_key = BIO_new_file(test_server_key_file, "rb");
            /* Load key and convert to DER */
            EVP_PKEY* key = PEM_read_bio_PrivateKey(bio_key, NULL, NULL, NULL);
            int length = i2d_PrivateKey(key, NULL);
            unsigned char* key_der = (unsigned char*)malloc(length);
            unsigned char* tmp = key_der;
            i2d_PrivateKey(key, &tmp);
            EVP_PKEY_free(key);
            BIO_free(bio_key);

            if (picoquic_set_tls_key(test_ctx->qserver, key_der, length) != 0) {
                ret = -1;
            }
        }

        if (ret == 0) {
            BIO* bio_key = BIO_new_file(test_server_cert_file, "rb");
            /* Load cert and convert to DER */
            X509* cert = PEM_read_bio_X509(bio_key, NULL, NULL, NULL);
            int length = i2d_X509(cert, NULL);
            unsigned char* cert_der = (unsigned char*)malloc(length);
            unsigned char* tmp = cert_der;
            i2d_X509(cert, &tmp);
            X509_free(cert);
            BIO_free(bio_key);

            ptls_iovec_t* chain = malloc(sizeof(ptls_iovec_t));
            if (chain == NULL) {
                ret = -1;
            } else {
                chain[0] = ptls_iovec_init(cert_der, length);

                picoquic_set_tls_certificate_chain(test_ctx->qserver, chain, 1);
            }
        }

        if (ret == 0) {
            BIO* bio_key = BIO_new_file(test_server_cert_store_file, "rb");
            /* Load cert and convert to DER */
            X509* cert = PEM_read_bio_X509(bio_key, NULL, NULL, NULL);
            int length = i2d_X509(cert, NULL);
            unsigned char* cert_der = (unsigned char*)malloc(length);
            unsigned char* tmp = cert_der;
            i2d_X509(cert, &tmp);
            X509_free(cert);
            BIO_free(bio_key);

            ptls_iovec_t* chain = malloc(sizeof(ptls_iovec_t));
            if (chain == NULL) {
                ret = -1;
            } else {
                chain[0] = ptls_iovec_init(cert_der, length);

                picoquic_set_tls_root_certificates(test_ctx->qserver, chain, 1);
            }
        }
    }

    /* Proceed with the connection loop. */
    if (ret == 0) {
        ret = tls_api_connection_loop(test_ctx, &loss_mask, 0, &simulated_time);
    }

    if (ret == 0 && (!TEST_CLIENT_READY || !TEST_SERVER_READY)) {
        ret = -1;
    }

    if (test_ctx != NULL) {
        tls_api_delete_ctx(test_ctx);
        test_ctx = NULL;
    }

    return ret;
}

int request_client_authentication_test()
{
    uint64_t simulated_time = 0;
    uint64_t loss_mask = 0;
    picoquic_test_tls_api_ctx_t* test_ctx = NULL;
    char test_server_cert_file[512];
    char test_server_key_file[512];
    char test_server_cert_store_file[512];
    int ret = picoquic_get_input_path(test_server_cert_file, sizeof(test_server_cert_file), picoquic_test_solution_dir, PICOQUIC_TEST_FILE_SERVER_CERT);

    if (ret == 0) {
        ret = picoquic_get_input_path(test_server_key_file, sizeof(test_server_key_file), picoquic_test_solution_dir, PICOQUIC_TEST_FILE_SERVER_KEY);
    }

    if (ret == 0) {
        ret = picoquic_get_input_path(test_server_cert_store_file, sizeof(test_server_cert_store_file), picoquic_test_solution_dir, PICOQUIC_TEST_FILE_CERT_STORE);
    }

    if (ret != 0) {
        DBG_PRINTF("%s", "Cannot set the cert, key or store file names.\n");
    }
    else {
        ret = tls_api_init_ctx(&test_ctx, 0, PICOQUIC_TEST_SNI, PICOQUIC_TEST_ALPN, &simulated_time, NULL, NULL, 0, 0, 0);
    }

    if (ret == 0 && test_ctx == NULL) {
        ret = -1;
    }

    /* Delete the client context, and recreate with a certificate */
    if (ret == 0)
    {
        if (test_ctx->qclient != NULL) {
            picoquic_free(test_ctx->qclient);
            test_ctx->cnx_client = NULL;
        }

        test_ctx->qclient = picoquic_create(8,
            test_server_cert_file, test_server_key_file, test_server_cert_store_file,
            NULL, test_api_callback, (void*)&test_ctx->client_callback, NULL, NULL, NULL,
            simulated_time, &simulated_time, NULL, NULL, 0);

        if (test_ctx->qclient == NULL) {
            ret = -1;
        }
    }

    /* recreate the client connection */
    if (ret == 0) {
        test_ctx->cnx_client = picoquic_create_cnx(test_ctx->qclient, picoquic_null_connection_id,
                                                   picoquic_null_connection_id,
                                                   (struct sockaddr*)&test_ctx->server_addr, 0,
                                                   0, PICOQUIC_TEST_SNI, PICOQUIC_TEST_ALPN, 1);

        if (test_ctx->cnx_client == NULL) {
            ret = -1;
        } else {
            ret = picoquic_start_client_cnx(test_ctx->cnx_client);
        }
    }

    if (ret == 0) {
        picoquic_set_client_authentication(test_ctx->qserver, 1);
        
        /* Proceed with the connection loop. */
        ret = tls_api_connection_loop(test_ctx, &loss_mask, 0, &simulated_time);
    }
  
    /* Check that both the client and server are ready. */
    if (ret == 0) {
        if (test_ctx->cnx_client == NULL
            || test_ctx->cnx_server == NULL
            || !TEST_CLIENT_READY
            || !TEST_SERVER_READY) {
            ret = -1;
        }
    }

    if (test_ctx != NULL) {
        tls_api_delete_ctx(test_ctx);
        test_ctx = NULL;
    }

    return ret;
}

int bad_client_certificate_test()
{
    uint64_t simulated_time = 0;
    uint64_t loss_mask = 0;
    picoquic_test_tls_api_ctx_t* test_ctx = NULL;
    char test_server_cert_file[512];
    char test_server_key_file[512];
    char test_server_cert_store_file[512];
    int ret = picoquic_get_input_path(test_server_cert_file, sizeof(test_server_cert_file), picoquic_test_solution_dir, PICOQUIC_TEST_FILE_SERVER_BAD_CERT);

    if (ret == 0) {
        ret = picoquic_get_input_path(test_server_key_file, sizeof(test_server_key_file), picoquic_test_solution_dir, PICOQUIC_TEST_FILE_SERVER_KEY);
    }

    if (ret == 0) {
        ret = picoquic_get_input_path(test_server_cert_store_file, sizeof(test_server_cert_store_file), picoquic_test_solution_dir, PICOQUIC_TEST_FILE_CERT_STORE);
    }

    if (ret != 0) {
        DBG_PRINTF("%s", "Cannot set the cert, key or store file names.\n");
    }
    else {
        ret = tls_api_init_ctx(&test_ctx, 0, PICOQUIC_TEST_SNI, PICOQUIC_TEST_ALPN, &simulated_time, NULL, NULL, 0, 0, 0);
    }

    if (ret == 0 && test_ctx == NULL) {
        ret = -1;
    }

    /* Delete the client context, and recreate with a certificate */
    if (ret == 0)
    {
        if (test_ctx->qclient != NULL) {
            picoquic_free(test_ctx->qclient);
            test_ctx->cnx_client = NULL;
        }

        test_ctx->qclient = picoquic_create(8,
            test_server_cert_file, test_server_key_file, test_server_cert_store_file,
            NULL, test_api_callback, (void*)&test_ctx->client_callback, NULL, NULL, NULL,
            simulated_time, &simulated_time, NULL, NULL, 0);

        if (test_ctx->qclient == NULL) {
            ret = -1;
        }
    }

    /* recreate the client connection */
    if (ret == 0) {
        test_ctx->cnx_client = picoquic_create_cnx(test_ctx->qclient, picoquic_null_connection_id,
                                                   picoquic_null_connection_id,
                                                   (struct sockaddr*)&test_ctx->server_addr, 0,
                                                   0, PICOQUIC_TEST_SNI, PICOQUIC_TEST_ALPN, 1);

        if (test_ctx->cnx_client == NULL) {
            ret = -1;
        } else {
            ret = picoquic_start_client_cnx(test_ctx->cnx_client);
        }
    }

    if (ret == 0) {
        picoquic_set_client_authentication(test_ctx->qserver, 1);
        
        /* Proceed with the connection loop. It should fail */
        ret = tls_api_connection_loop(test_ctx, &loss_mask, 0, &simulated_time);
    }

    if (ret == 0) {
        if (test_ctx->cnx_client == NULL) {
            ret = -1;
        }
        else if (test_ctx->cnx_client->cnx_state != picoquic_state_disconnected) {
            ret = -1;
        }
        else if (!picoquic_is_handshake_error(picoquic_get_local_error(test_ctx->cnx_server))) {
            ret = -1;
        }
        else if (!picoquic_is_handshake_error(picoquic_get_remote_error(test_ctx->cnx_client))) {
            ret = -1;
        }
    }

    if (test_ctx != NULL) {
        tls_api_delete_ctx(test_ctx);
        test_ctx = NULL;
    }

    return ret;
}

/*
* NAT Rebinding test. The client is unaware of the migration.
* Start with one basic transmission, then switch the client
* to a different port number. Verify that the server issues 
* a path challenge, that the client responds with a path
* response, and that the connection completes.
*/

int nat_rebinding_test_one(uint64_t loss_mask_data)
{
    uint64_t simulated_time = 0;
    uint64_t next_time = 0;
    uint64_t loss_mask = 0;
    uint64_t initial_challenge = 0;
    uint64_t client_challenge = 0;
    int nb_inactive = 0;
    picoquic_test_tls_api_ctx_t* test_ctx = NULL;
    int ret = tls_api_init_ctx(&test_ctx, PICOQUIC_INTERNAL_TEST_VERSION_1,
        PICOQUIC_TEST_SNI, PICOQUIC_TEST_ALPN, &simulated_time, NULL, NULL, 0, 0, 0);

    if (ret == 0 && test_ctx == NULL) {
        ret = PICOQUIC_ERROR_MEMORY;
    }

    if (ret == 0) {
        ret = tls_api_connection_loop(test_ctx, &loss_mask, 0, &simulated_time);
    }

    if (ret == 0) {
        initial_challenge = test_ctx->cnx_server->path[0]->challenge[0];
        client_challenge = test_ctx->cnx_client->path[0]->challenge[0];
        loss_mask = loss_mask_data; 
        
        /* Change the client address */
        test_ctx->client_addr.sin_port += 17;
        test_ctx->client_use_nat = 1;
        
        /* Prepare to send data */
        ret = test_api_init_send_recv_scenario(test_ctx, test_scenario_q_and_r, sizeof(test_scenario_q_and_r));
    }

    /* Perform a data sending loop */
    if (ret == 0) {
        ret = tls_api_data_sending_loop(test_ctx, &loss_mask, &simulated_time, 0);
    }
    
    /* verify that the transmission was complete */
    if (ret == 0) {
        ret = tls_api_one_scenario_verify(test_ctx);
    }

    /* Add a time loop of 3 seconds to give some time for the challenge to be repeated */
    next_time = simulated_time + 3000000;
    loss_mask = 0;
    while (ret == 0 && simulated_time < next_time && TEST_CLIENT_READY 
        && TEST_SERVER_READY
        && test_ctx->cnx_server->path[0]->challenge_verified != 1) {
        int was_active = 0;

        ret = tls_api_one_sim_round(test_ctx, &simulated_time, next_time, &was_active);

        if (was_active) {
            nb_inactive = 0;
        }
        else {
            nb_inactive++;
            if (nb_inactive > 128) {
                ret = 0;
                if (nb_inactive > 256) {
                    break;
                }
            }
        }
    }

    /* Verify that the challenge was updated and done */
    if (ret == 0) {
        if (test_ctx->cnx_server == NULL) {
            DBG_PRINTF("%s", "Server connection disappeared");
            ret = -1;
        } else if (initial_challenge == test_ctx->cnx_server->path[0]->challenge[0]) {
            DBG_PRINTF("%s", "Challenge was not renewed after NAT rebinding");
            ret = -1;
        }
        else if (test_ctx->cnx_server->path[0]->challenge_verified != 1) {
            DBG_PRINTF("%s", "Challenge was not verified after NAT rebinding");
            ret = -1;
        }
        else if (client_challenge != test_ctx->cnx_client->path[0]->challenge[0]) {
            DBG_PRINTF("%s", "Extra client challenge after NAT rebinding");
            ret = -1;
        }
    }

    if (test_ctx != NULL) {
        tls_api_delete_ctx(test_ctx);
        test_ctx = NULL;
    }

    return ret;
}

int nat_rebinding_test()
{
    uint64_t loss_mask = 0;

    return nat_rebinding_test_one(loss_mask);
}

int nat_rebinding_loss_test()
{
    uint64_t loss_mask = 0x2412;

    return nat_rebinding_test_one(loss_mask);
}

/*
 * Spin bit test. Verify that the bit does spin, and that the number
 * of rotations is plausible given the duration and the min delay.
 */

int spin_bit_test()
{
    uint64_t simulated_time = 0;
    uint64_t loss_mask = 0;
    uint64_t spin_duration = 0;
    picoquic_test_tls_api_ctx_t* test_ctx = NULL;
    int spin_count = 0;
    int ret = tls_api_init_ctx(&test_ctx, PICOQUIC_INTERNAL_TEST_VERSION_1,
        PICOQUIC_TEST_SNI, PICOQUIC_TEST_ALPN, &simulated_time, NULL, NULL, 0, 1, 0);

    if (ret != 0)
    {
        DBG_PRINTF("%s", "Could not create the QUIC test contexts\n");
    }

    if (ret == 0) {
        test_ctx->client_use_nat = 1;

        /* force spinbit policy to basic, then start */
        test_ctx->cnx_client->spin_policy = picoquic_spinbit_basic;

        ret = picoquic_start_client_cnx(test_ctx->cnx_client);
        if (ret != 0)
        {
            DBG_PRINTF("%s", "Could not initialize stream zero for the client\n");
        }

    }

    if (ret == 0) {
        ret = tls_api_connection_loop(test_ctx, &loss_mask, 0, &simulated_time);

        if (ret != 0)
        {
            DBG_PRINTF("Connection loop returns error %d\n", ret);
        }
    }

    /* Prepare to send data */
    if (ret == 0) {
        /* force the server spin bit policy to basic, then init the scenario */
        test_ctx->cnx_server->spin_policy = picoquic_spinbit_basic;

        ret = test_api_init_send_recv_scenario(test_ctx, test_scenario_very_long, sizeof(test_scenario_very_long));

        if (ret != 0)
        {
            DBG_PRINTF("Init send receive scenario returns %d\n", ret);
        }
    }

    /* Explore the data sending loop so we can observe the spin bit  */
    if (ret == 0) {
        uint64_t spin_begin_time = simulated_time;
        uint64_t next_time = simulated_time + 10000000;
        int ret = 0;
        int nb_trials = 0;
        int nb_inactive = 0;
        int max_trials = 100000;
        int current_spin = test_ctx->cnx_client->path[0]->current_spin;

        test_ctx->c_to_s_link->loss_mask = &loss_mask;
        test_ctx->s_to_c_link->loss_mask = &loss_mask;

        while (ret == 0 && nb_trials < max_trials && simulated_time < next_time && nb_inactive < 256 && TEST_CLIENT_READY && TEST_SERVER_READY) {
            int was_active = 0;

            nb_trials++;

            ret = tls_api_one_sim_round(test_ctx, &simulated_time, next_time, &was_active);

            if (ret < 0)
            {
                break;
            }

            if (test_ctx->cnx_client->path[0]->current_spin != current_spin) {
                spin_count++;
                current_spin = test_ctx->cnx_client->path[0]->current_spin;
            }

            if (was_active) {
                nb_inactive = 0;
            }
            else {
                nb_inactive++;
            }

            if (test_ctx->test_finished) {
                if (picoquic_is_cnx_backlog_empty(test_ctx->cnx_client) && picoquic_is_cnx_backlog_empty(test_ctx->cnx_server)) {
                    break;
                }
            }
        }

        spin_duration = simulated_time - spin_begin_time;

        if (ret != 0)
        {
            DBG_PRINTF("Data sending loop fails with ret = %d\n", ret);
        }
    }

    if (ret == 0) {
        ret = picoquic_close(test_ctx->cnx_client, 0);
        if (ret != 0)
        {
            DBG_PRINTF("Picoquic close returns %d\n", ret);
        }
    }

    if (ret == 0) {
        if (spin_count < 6) {
            DBG_PRINTF("Unplausible spin bit: %d rotations, rtt_min = %d, duration = %d\n",
                spin_count, (int)test_ctx->cnx_client->path[0]->rtt_min, (int)spin_duration);
            ret = -1;
        }
    }

    if (test_ctx != NULL) {
        tls_api_delete_ctx(test_ctx);
        test_ctx = NULL;
    }

    return ret;
}


/*
* Closing on error test. We voluntarily inject an erroneous
* frame on the client connection. The expected result is that
* the server connection gets closed, but the server remains
* responsive.
*/

int client_error_test()
{
    uint64_t simulated_time = 0;
    uint64_t next_time = 0;
    uint64_t loss_mask = 0;
    picoquic_test_tls_api_ctx_t* test_ctx = NULL;
    int ret = tls_api_init_ctx(&test_ctx, PICOQUIC_INTERNAL_TEST_VERSION_1,
        PICOQUIC_TEST_SNI, PICOQUIC_TEST_ALPN, &simulated_time, NULL, NULL, 0, 0, 0);

    if (ret == 0) {
        ret = tls_api_connection_loop(test_ctx, &loss_mask, 0, &simulated_time);
    }

    /* Prepare to send data */
    if (ret == 0) {
        ret = test_api_init_send_recv_scenario(test_ctx, test_scenario_q_and_r, sizeof(test_scenario_q_and_r));
    }

    /* Perform a data sending loop */
    if (ret == 0) {
        ret = tls_api_data_sending_loop(test_ctx, &loss_mask, &simulated_time, 0);
    }

    /* Inject an erroneous frame */
    if (ret == 0) {
        /* Queue a data frame on stream 4, which was already closed */
        uint8_t stream_error_frame[] = { 0x17, 0x04, 0x41, 0x01, 0x08, 0x00, 0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07 };
        picoquic_queue_misc_frame(test_ctx->cnx_client, stream_error_frame, sizeof(stream_error_frame));
    }

    /* Add a time loop of 3 seconds to give some time for the error to be repeated */
    next_time = simulated_time + 3000000;
    loss_mask = 0;
    while (ret == 0 && simulated_time < next_time
        && (test_ctx->cnx_client->cnx_state < picoquic_state_disconnected ||
            test_ctx->cnx_server->cnx_state < picoquic_state_disconnected)) {
        int was_active = 0;

        ret = tls_api_one_sim_round(test_ctx, &simulated_time, next_time, &was_active);
    }

    if (ret == 0 && test_ctx->cnx_server->cnx_state != picoquic_state_disconnected) {
        ret = -1;
    }

    if (ret == 0) {
        /* Delete the client connection from the client context,
         * without sending notification to the server */
        while (test_ctx->qclient->cnx_list != NULL) {
            picoquic_delete_cnx(test_ctx->qclient->cnx_list);
        }

        /* Erase the server connection reference */
        test_ctx->cnx_server = NULL;

        /* Create a new connection in the client context */

        test_ctx->cnx_client = picoquic_create_cnx(test_ctx->qclient,
            picoquic_null_connection_id, picoquic_null_connection_id,
            (struct sockaddr*)&test_ctx->server_addr, simulated_time, 0, PICOQUIC_TEST_SNI, PICOQUIC_TEST_ALPN, 1);

        if (test_ctx->cnx_client == NULL) {
            ret = -1;
        }
        else if (ret == 0) {
            ret = picoquic_start_client_cnx(test_ctx->cnx_client);
        }
    }

    /* Now, restart a connection in the same context */
    if (ret == 0) {
        ret = tls_api_connection_loop(test_ctx, &loss_mask, 0, &simulated_time);
    }

    if (ret == 0){
        ret = wait_application_aead_ready(test_ctx, &simulated_time);
    }

    if (ret == 0) {
        ret = tls_api_attempt_to_close(test_ctx, &simulated_time);
    }

    if (test_ctx != NULL) {
        tls_api_delete_ctx(test_ctx);
        test_ctx = NULL;
    }

    return ret;
}

/*
 * Set a connection, then verify that the "new connection id" frames have been exchanged properly.
 * Use the "check stash" function to verify that new connection ID were properly
 * stashed on each side.
 *
 * TODO: also test that no New Connection Id frames are sent if migration is disabled 
 */

int test_cnxid_count_stash(picoquic_cnx_t * cnx) {
    picoquic_cnxid_stash_t * stash = cnx->cnxid_stash_first;
    int nb = 0;

    while (stash != NULL) {
        nb++;
        stash = stash->next_in_stash;
    }

    return nb;
}

int transmit_cnxid_test_stash(picoquic_cnx_t * cnx1, picoquic_cnx_t * cnx2, char const * cnx_text)
{
    int ret = 0;
    picoquic_cnxid_stash_t * stash = cnx1->cnxid_stash_first;
    int path_id = 1;

    while (stash != NULL && path_id < cnx2->nb_paths) {
        if (picoquic_compare_connection_id(&stash->cnx_id, &cnx2->path[path_id]->local_cnxid) != 0) {
            DBG_PRINTF("On %s, cnx ID of stash #%d does not match path[%d] of peer.\n",
                cnx_text, path_id - 1, path_id);
            ret = -1;
            break;
        }
        stash = stash->next_in_stash;
        path_id++;
    }

    if (ret == 0 && path_id < cnx2->nb_paths) {
        DBG_PRINTF("On %s, %d items in stash instead instead of %d.\n", cnx_text, path_id - 1, cnx2->nb_paths);
        ret = -1;
    }

    if (ret == 0 && stash != NULL) {
        DBG_PRINTF("On %s, more than %d items in stash.\n", cnx_text, path_id - 1);
        ret = -1;
    }

    return ret;

}

int transmit_cnxid_test()
{
    uint64_t simulated_time = 0;
    uint64_t loss_mask = 0;
    picoquic_test_tls_api_ctx_t* test_ctx = NULL;
    int ret = tls_api_init_ctx(&test_ctx, PICOQUIC_INTERNAL_TEST_VERSION_1,
        PICOQUIC_TEST_SNI, PICOQUIC_TEST_ALPN, &simulated_time, NULL, NULL, 0, 0, 0);

    if (ret == 0) {
        ret = tls_api_connection_loop(test_ctx, &loss_mask, 0, &simulated_time);
    }

    /* run a receive loop until no outstanding data */
    if (ret == 0) {
        ret = tls_api_synch_to_empty_loop(test_ctx, &simulated_time, 2048, PICOQUIC_NB_PATH_TARGET, 0);
    }

    if (ret == 0) {
        if (test_ctx->cnx_client->nb_paths < PICOQUIC_NB_PATH_TARGET) {
            DBG_PRINTF("Only %d paths created on client.\n", test_ctx->cnx_client->nb_paths);
            ret = -1;
        } else if (test_ctx->cnx_server->nb_paths < PICOQUIC_NB_PATH_TARGET) {
            DBG_PRINTF("Only %d paths created on server.\n", test_ctx->cnx_server->nb_paths);
        }
    }

    if (ret == 0) {
        ret = transmit_cnxid_test_stash(test_ctx->cnx_client, test_ctx->cnx_server, "client");
    }

    if (ret == 0) {
        ret = transmit_cnxid_test_stash(test_ctx->cnx_server, test_ctx->cnx_client, "server");
    }

    if (test_ctx != NULL) {
        tls_api_delete_ctx(test_ctx);
        test_ctx = NULL;
    }

    return ret;
}

/*
 * Unit test for the probe management functions.
 *
 * Set up a connection, exchange new cnxid frames, then create a number of probes.
 * When the number exceeds the number of connections, the probing should fail.
 */

int probe_api_test()
{
    uint64_t simulated_time = 0;
    uint64_t loss_mask = 0;
    picoquic_test_tls_api_ctx_t* test_ctx = NULL;
    struct sockaddr_in t4[PICOQUIC_NB_PATH_TARGET];
    struct sockaddr_in6 t6[PICOQUIC_NB_PATH_TARGET];
    int nb_trials;

    /* Initialize the test addresses to synthetic values */
    for (int i = 0; i < PICOQUIC_NB_PATH_TARGET; i++) {
        memset(&t4[i], 0, sizeof(struct sockaddr_in));
        t4[i].sin_family = AF_INET;
        t4[i].sin_port = 1000+i;
        memset(&t4[i].sin_addr, i, 4);
        memset(&t6[i], 0, sizeof(struct sockaddr_in6));
        t6[i].sin6_family = AF_INET6;
        t6[i].sin6_port = 2000 + i;
        memset(&t6[i].sin6_addr, i, 16);
    }

    /* Set a test connection between client and server */
    int ret = tls_api_init_ctx(&test_ctx, PICOQUIC_INTERNAL_TEST_VERSION_1,
        PICOQUIC_TEST_SNI, PICOQUIC_TEST_ALPN, &simulated_time, NULL, NULL, 0, 0, 0);

    if (ret == 0) {
        ret = tls_api_connection_loop(test_ctx, &loss_mask, 0, &simulated_time);
    }

    /* run a receive loop until no outstanding data */
    if (ret == 0) {
        ret = tls_api_synch_to_empty_loop(test_ctx, &simulated_time, 2048, PICOQUIC_NB_PATH_TARGET, 0);
    }

    if (ret == 0) {
        if (test_ctx->cnx_client->nb_paths < PICOQUIC_NB_PATH_TARGET) {
            DBG_PRINTF("Only %d paths created on client.\n", test_ctx->cnx_client->nb_paths);
            ret = -1;
        }
        else if (test_ctx->cnx_server->nb_paths < PICOQUIC_NB_PATH_TARGET) {
            DBG_PRINTF("Only %d paths created on server.\n", test_ctx->cnx_server->nb_paths);
        }
    }

    /* Now, create a series of probes.
     * There are only PICOQUIC_NB_PATH_TARGET - 1 paths available. 
     * The last trial should fail.
     */
    nb_trials = 0;

    for (int i = 1; ret == 0 && i < PICOQUIC_NB_PATH_TARGET; i++) {
        for (int j = 0; ret == 0 && j < 2; j++) {
            int ret_probe;
            if (j == 0) {
                ret_probe = picoquic_create_probe(test_ctx->cnx_client, (struct sockaddr *) &t4[0], (struct sockaddr *) &t4[i]);
            } else {
                ret_probe = picoquic_create_probe(test_ctx->cnx_client, (struct sockaddr *) &t6[0], (struct sockaddr *) &t6[i]);
            }

            nb_trials++;

            if (nb_trials <= PICOQUIC_NB_PATH_TARGET) {
                if (ret_probe != 0) {
                    DBG_PRINTF("Trial %d (%d, %d) fails with ret = %x\n", nb_trials, i, j, ret_probe);
                    ret = -1;
                }
            }
            else if (ret_probe == 0) {
                DBG_PRINTF("Trial %d (%d, %d) succeeds (unexpected)\n", nb_trials, i, j);
                ret = -1;
            }

            if (ret == 0 && ret_probe == 0) {
                for (int ichal = 0; ichal < PICOQUIC_CHALLENGE_REPEAT_MAX; ichal++) {
                    test_ctx->cnx_client->probe_first->challenge[ichal] = (uint64_t)10000 + (uint64_t)10 * i + j + (uint64_t)1000*ichal;
                }
            }
        }
    }

    /* Now, test retrieval functions */
    /* First test retrieval by address */
    nb_trials = 0;
    for (int i = 1; ret == 0 && i < PICOQUIC_NB_PATH_TARGET; i++) {
        for (int j = 0; ret == 0 && j < 2; j++) {
            picoquic_probe_t * probe;
            uint64_t challenge = (uint64_t)10000 + (uint64_t)10 * i + j;
            if (j == 0) {
                probe = picoquic_find_probe_by_addr(test_ctx->cnx_client, (struct sockaddr *) &t4[0], (struct sockaddr *) &t4[i]);
            }
            else {
                probe = picoquic_find_probe_by_addr(test_ctx->cnx_client, (struct sockaddr *) &t6[0], (struct sockaddr *) &t6[i]);
            }

            nb_trials++;

            if (nb_trials <= PICOQUIC_NB_PATH_TARGET) {
                if (probe == NULL) {
                    DBG_PRINTF("Retrieve by addr %d (%d, %d) fails\n", nb_trials, i, j);
                    ret = -1;
                }
                else if (probe->challenge[0] != challenge){
                    DBG_PRINTF("Retrieve by addr %d (%d, %d) finds %d instead of %d\n", 
                        nb_trials, i, j, (int)probe->challenge[0], (int)challenge);
                    ret = -1;
                }
            }
            else if (probe != 0) {
                DBG_PRINTF("Retrieve by addr %d (%d, %d) succeeds (unexpected)\n", nb_trials, i, j);
                ret = -1;
            }
        }
    }

    /* Then test retrieval by challenge */
    nb_trials = 0;
    for (int i = 1; ret == 0 && i < PICOQUIC_NB_PATH_TARGET; i++) {
        for (int j = 0; ret == 0 && j < 2; j++) {
            picoquic_probe_t * probe;
            nb_trials++;

            for (int ichal = 0; ichal < PICOQUIC_CHALLENGE_REPEAT_MAX; ichal++) {
                uint64_t challenge = (uint64_t)10000 + (uint64_t)10 * i + j + (uint64_t)1000 * ichal;

                probe = picoquic_find_probe_by_challenge(test_ctx->cnx_client, challenge);


                if (nb_trials <= PICOQUIC_NB_PATH_TARGET) {
                    if (probe == NULL) {
                        DBG_PRINTF("Retrieve by challenge %d (%d, %d) fails\n", nb_trials, i, j);
                        ret = -1;
                    }
                }
                else if (probe != NULL) {
                    DBG_PRINTF("Retrieve by challenge %d (%d, %d) succeeds (unexpected)\n", nb_trials, i, j);
                    ret = -1;
                }
            }
        }
    }

    /* Remove exactly one probe, then try to retrieve it */
    if (ret == 0) {
        picoquic_probe_t * probe;
        int i = 1;
        int j = 1;
        uint64_t challenge = (uint64_t)10000 + (uint64_t)10 * i + j;

        probe = picoquic_find_probe_by_challenge(test_ctx->cnx_client, challenge);

        if (probe == NULL) {
            DBG_PRINTF("Retrieve by challenge=%d (%d, %d) fails\n", (int)challenge, i, j);
            ret = -1;
        }
        else {
            picoquic_delete_probe(test_ctx->cnx_client, probe);

            probe = picoquic_find_probe_by_challenge(test_ctx->cnx_client, challenge);
            if (probe != NULL) {
                DBG_PRINTF("Retrieve by challenge %d succeeds after delete\n", (int)challenge);
                ret = -1;
            }
        }
    }


    /* Releasing the context will test the delete functions. */
    if (test_ctx != NULL) {
        tls_api_delete_ctx(test_ctx);
        test_ctx = NULL;
    }

    return ret;
}

/*
 * Migration test. The client is aware of the migration, and
 * starts the migration by explicitly probing a new path.
 */

int migration_test_scenario(test_api_stream_desc_t * scenario, size_t size_of_scenario, uint64_t loss_target)
{
    uint64_t simulated_time = 0;
    uint64_t next_time = 0;
    uint64_t loss_mask = 0;
    uint64_t initial_challenge = 0;
    picoquic_connection_id_t target_id = picoquic_null_connection_id;
    picoquic_connection_id_t previous_local_id = picoquic_null_connection_id;
    picoquic_test_tls_api_ctx_t* test_ctx = NULL;
    int ret = tls_api_init_ctx(&test_ctx, PICOQUIC_INTERNAL_TEST_VERSION_1,
        PICOQUIC_TEST_SNI, PICOQUIC_TEST_ALPN, &simulated_time, NULL, NULL, 0, 0, 0);

    if (ret == 0 && test_ctx == NULL) {
        ret = PICOQUIC_ERROR_MEMORY;
    }

    if (ret == 0) {
        ret = tls_api_connection_loop(test_ctx, &loss_mask, 0, &simulated_time);
    }

    /* run a receive loop until no outstanding data */
    if (ret == 0) {

        ret = tls_api_synch_to_empty_loop(test_ctx, &simulated_time, 2048, PICOQUIC_NB_PATH_TARGET, 1);
    }

    if (ret == 0) {
        /* Change the client address */
        test_ctx->client_addr.sin_port += 17;

        /* Probe the new path */
        ret = picoquic_create_probe(
            test_ctx->cnx_client, (struct sockaddr *)&test_ctx->server_addr, (struct sockaddr *)&test_ctx->client_addr);

        if (ret == 0) {
            target_id = test_ctx->cnx_client->probe_first->remote_cnxid;
            previous_local_id = test_ctx->cnx_client->path[0]->local_cnxid;
        }
    }

    /* Prepare to send data */
    if (ret == 0) {
        ret = test_api_init_send_recv_scenario(test_ctx, scenario, size_of_scenario);
    }

    /* Perform a data sending loop */
    loss_mask = loss_target;

    if (ret == 0) {
        ret = tls_api_data_sending_loop(test_ctx, &loss_mask, &simulated_time, 0);
    }

    /* Check that the data was sent and received */
    if (ret == 0) {
        ret = tls_api_one_scenario_verify(test_ctx);
    }

    /* Add a time loop of 3 seconds to give some time for the probes to be repeated */
    next_time = simulated_time + 4000000;
    loss_mask = 0;
    while (ret == 0 && simulated_time < next_time && TEST_CLIENT_READY
        && TEST_SERVER_READY
        && (test_ctx->cnx_server->path[0]->challenge_verified != 1 || test_ctx->cnx_client->path[0]->path_is_demoted == 1 ||
            initial_challenge == test_ctx->cnx_server->path[0]->challenge[0])) {
        int was_active = 0;

        ret = tls_api_one_sim_round(test_ctx, &simulated_time, next_time, &was_active);
    }

    /* Verify that the challenge was updated and done */
    /* TODO: verify that exactly one challenge was sent */
    if (ret == 0 && test_ctx->cnx_server != NULL) {
        if (initial_challenge == test_ctx->cnx_server->path[0]->challenge[0]) {
            DBG_PRINTF("%s", "Challenge was not renewed after migration");
            ret = -1;
        }
        else if (test_ctx->cnx_server->path[0]->challenge_verified != 1) {
            DBG_PRINTF("%s", "Challenge was not verified after migration");
            ret = -1;
        }
    }

    /* Verify that the connection ID are what we expect */
    if (ret == 0) {
        if (picoquic_compare_connection_id(&test_ctx->cnx_client->path[0]->remote_cnxid, &target_id) != 0) {
            DBG_PRINTF("%s", "The remote CNX ID did not change to selected value");
            ret = -1;
        }
        else if (picoquic_compare_connection_id(&test_ctx->cnx_client->path[0]->local_cnxid, &previous_local_id) == 0) {
            DBG_PRINTF("%s", "The local CNX ID did not change to a new value");
            ret = -1;
        }
    }


    if (test_ctx != NULL) {
        tls_api_delete_ctx(test_ctx);
        test_ctx = NULL;
    }

    return ret;
}

int migration_test()
{
    return migration_test_scenario(test_scenario_q_and_r, sizeof(test_scenario_q_and_r), 0);
}

int migration_test_long()
{
    return migration_test_scenario(test_scenario_very_long, sizeof(test_scenario_very_long), 0);
}

int migration_test_loss()
{
    uint64_t loss_mask = 0x09;

    return migration_test_scenario(test_scenario_q_and_r, sizeof(test_scenario_q_and_r), loss_mask);
}

/* Migration stress test. 
 * This simulates an attack, during which a man on the side injects fake migration
 * packets from false addresses. One of the addresses is maintained so that packets sent
 * to it are natted back to the client.
 *
 * The goal of the attack is to verify that the connection resists.
 */

int rebinding_stress_test()
{
    int nb_trials = 0;
    const int max_trials = 10000;
    int nb_inactive = 0;
    int client_rebinding_done = 0;
    struct sockaddr_in hack_address;
    struct sockaddr_in hack_address_random;
    uint64_t simulated_time = 0;
    uint64_t loss_mask = 0;
    uint64_t last_inject_time = 0;
    uint64_t random_context = 0xBABAC001;
    picoquic_test_tls_api_ctx_t* test_ctx = NULL;
    int ret = tls_api_init_ctx(&test_ctx, PICOQUIC_INTERNAL_TEST_VERSION_1,
        PICOQUIC_TEST_SNI, PICOQUIC_TEST_ALPN, &simulated_time, NULL, NULL, 0, 0, 0);

    if (ret == 0 && test_ctx == NULL) {
        ret = PICOQUIC_ERROR_MEMORY;
    }

    if (ret == 0) {
        memcpy(&hack_address, &test_ctx->client_addr, sizeof(struct sockaddr_in));
        memcpy(&hack_address_random, &test_ctx->client_addr, sizeof(struct sockaddr_in));

        hack_address.sin_port += 1023;

        ret = tls_api_connection_loop(test_ctx, &loss_mask, 0, &simulated_time);
    }

    /* Prepare to send data */
    if (ret == 0) {
        ret = test_api_init_send_recv_scenario(test_ctx, test_scenario_very_long, sizeof(test_scenario_very_long));
    }

    /* Rewrite the sending loop, so we can add injection of packet copies */

    while (ret == 0 && nb_trials < max_trials && nb_inactive < 256 && TEST_CLIENT_READY && TEST_SERVER_READY) {
        int was_active = 0;

        nb_trials++;

        ret = tls_api_one_sim_round(test_ctx, &simulated_time, 0, &was_active);

        if (ret < 0)
        {
            break;
        }

        if (was_active) {
            nb_inactive = 0;
        }
        else {
            nb_inactive++;
        }

        if (test_ctx->test_finished) {
            if (picoquic_is_cnx_backlog_empty(test_ctx->cnx_client) && picoquic_is_cnx_backlog_empty(test_ctx->cnx_server)) {
                break;
            }
        }

        /* Packet injection at the server */
        if (test_ctx->c_to_s_link->last_packet != NULL) {
            uint64_t server_arrival = test_ctx->c_to_s_link->last_packet->arrival_time;

            if (server_arrival > last_inject_time) {
                /* 49% chance of packet injection, 20% chances of reusing test address */
                uint64_t rand100 = picoquic_test_uniform_random(&random_context, 100);
                last_inject_time = server_arrival;
                if (rand100 < 37) {
                    struct sockaddr * bad_address;
                    if (rand100 < 20) {
                        bad_address = (struct sockaddr *)&hack_address;
                    }
                    else {
                        hack_address_random.sin_port = (uint16_t)picoquic_test_uniform_random(&random_context, 0x10000);
                        bad_address = (struct sockaddr *)&hack_address_random;
                    }
                    ret = picoquic_incoming_packet(test_ctx->qserver,
                        test_ctx->c_to_s_link->last_packet->bytes,
                        (uint32_t)test_ctx->c_to_s_link->last_packet->length,
                        bad_address,
                        (struct sockaddr*)&test_ctx->c_to_s_link->last_packet->addr_to, 0, 0,
                        simulated_time);
                }
            }
        }

        /* Initially, the attacker relays packets to the client. Then, it gives up */
        if (test_ctx->cnx_server->pkt_ctx[picoquic_packet_context_application].send_sequence < 256) {
            /* Packet reinjection at the client if using the special address */
            if (test_ctx->s_to_c_link->last_packet != NULL &&
                picoquic_compare_addr((struct sockaddr *)&hack_address, (struct sockaddr *)&test_ctx->s_to_c_link->last_packet->addr_to) == 0)
            {
                picoquic_store_addr(&test_ctx->s_to_c_link->last_packet->addr_to, (struct sockaddr *)&test_ctx->client_addr);
            }
        }

        /* At some point, the client does migrate to a new address */
        if (!client_rebinding_done && test_ctx->cnx_server->pkt_ctx[picoquic_packet_context_application].send_sequence > 128) {
            test_ctx->client_addr.sin_port += 17;
            test_ctx->client_use_nat = 1;
            client_rebinding_done = 1;
        }
    }

    if (ret == 0) {
        ret = tls_api_attempt_to_close(test_ctx, &simulated_time);
    }

    if (ret == 0) {
        if (test_ctx->server_callback.error_detected) {
            ret = -1;
        }
        else if (test_ctx->client_callback.error_detected) {
            ret = -1;
        }
        else {
            for (size_t i = 0; ret == 0 && i < test_ctx->nb_test_streams; i++) {
                if (test_ctx->test_stream[i].q_recv_nb != test_ctx->test_stream[i].q_len) {
                    ret = -1;
                }
                else if (test_ctx->test_stream[i].r_recv_nb != test_ctx->test_stream[i].r_len) {
                    ret = -1;
                }
                else if (test_ctx->test_stream[i].q_received == 0 || test_ctx->test_stream[i].r_received == 0) {
                    ret = -1;
                }
            }
        }
        if (ret != 0)
        {
            DBG_PRINTF("Test scenario verification returns %d\n", ret);
        }
    }

    if (test_ctx != NULL) {
        tls_api_delete_ctx(test_ctx);
        test_ctx = NULL;
    }

    return ret;
}



/* Connection ID renewal test.
 */

int cnxid_renewal_test()
{
    uint64_t simulated_time = 0;
    uint64_t next_time = 0;
    uint64_t loss_mask = 0;
    picoquic_connection_id_t target_id = picoquic_null_connection_id;
    picoquic_connection_id_t previous_local_id = picoquic_null_connection_id;
    picoquic_test_tls_api_ctx_t* test_ctx = NULL;
    int ret = tls_api_init_ctx(&test_ctx, PICOQUIC_INTERNAL_TEST_VERSION_1,
        PICOQUIC_TEST_SNI, PICOQUIC_TEST_ALPN, &simulated_time, NULL, NULL, 0, 0, 0);

    if (ret == 0 && test_ctx == NULL) {
        ret = PICOQUIC_ERROR_MEMORY;
    }

    if (ret == 0) {
        ret = tls_api_connection_loop(test_ctx, &loss_mask, 0, &simulated_time);
    }

    /* run a receive loop until no outstanding data */
    if (ret == 0) {
        ret = tls_api_synch_to_empty_loop(test_ctx, &simulated_time, 2048, PICOQUIC_NB_PATH_TARGET, 1);
    }

    /* Renew the connection ID */
    if (ret == 0) {
        ret = picoquic_renew_connection_id(test_ctx->cnx_client, 0);
        if (ret == 0) {
            target_id = test_ctx->cnx_client->path[0]->remote_cnxid;
            previous_local_id = test_ctx->cnx_client->path[0]->local_cnxid;
        }
    }

    /* Prepare to send data */
    if (ret == 0) {
        ret = test_api_init_send_recv_scenario(test_ctx, test_scenario_q_and_r, sizeof(test_scenario_q_and_r));
    }

    /* Perform a data sending loop */
    if (ret == 0) {
        ret = tls_api_data_sending_loop(test_ctx, &loss_mask, &simulated_time, 0);
    }

    /* Add a time loop of 7 seconds to give some time for the probes to be repeated,
     * and to ensure that the demotion timers expire. */
    next_time = simulated_time + 7000000;
    loss_mask = 0;
    while (ret == 0 && simulated_time < next_time && TEST_CLIENT_READY
        && TEST_SERVER_READY) {
        int was_active = 0;

        ret = tls_api_one_sim_round(test_ctx, &simulated_time, next_time, &was_active);
    }

    /* verify that path[0] was not demoted */
    if (ret == 0) {
        if (test_ctx->cnx_client->path[0]->path_is_demoted) {
            DBG_PRINTF("%s", "The default client path is demoted");
            ret = -1;
        }
        if (test_ctx->cnx_server != NULL && test_ctx->cnx_server->path[0]->path_is_demoted) {
            DBG_PRINTF("%s", "The default server path is demoted");
            ret = -1;
        }
    }

    /* Verify that the connection ID are what we expect */
    if (ret == 0) {
        if (picoquic_compare_connection_id(&test_ctx->cnx_client->path[0]->remote_cnxid, &target_id) != 0) {
            DBG_PRINTF("%s", "The remote CNX ID migrated from the selected value");
            ret = -1;
        }
        else if (picoquic_compare_connection_id(&test_ctx->cnx_client->path[0]->local_cnxid, &previous_local_id) != 0) {
            DBG_PRINTF("%s", "The local CNX ID changed to a new value");
            ret = -1;
        }
    }

    if (test_ctx != NULL) {
        tls_api_delete_ctx(test_ctx);
        test_ctx = NULL;
    }

    return ret;
}


/*
 * Perform a test of the "retire connection id" function.
 * The test will artificially retire connection ID on the client,
 * and verify that the server will refill the stash of 
 * connection ID.
 */
int retire_cnxid_test()
{
    uint64_t simulated_time = 0;
    uint64_t loss_mask = 0;
    picoquic_test_tls_api_ctx_t* test_ctx = NULL;
    int ret = tls_api_init_ctx(&test_ctx, PICOQUIC_INTERNAL_TEST_VERSION_1,
        PICOQUIC_TEST_SNI, PICOQUIC_TEST_ALPN, &simulated_time, NULL, NULL, 0, 0, 0);

    if (ret == 0) {
        ret = tls_api_connection_loop(test_ctx, &loss_mask, 0, &simulated_time);
    }

    /* run a receive loop until no outstanding data */
    if (ret == 0) {
        ret = tls_api_synch_to_empty_loop(test_ctx, &simulated_time, 2048, PICOQUIC_NB_PATH_TARGET, 0);
    }

    if (ret == 0) {
        if (test_ctx->cnx_client->nb_paths < PICOQUIC_NB_PATH_TARGET) {
            DBG_PRINTF("Only %d paths created on client.\n", test_ctx->cnx_client->nb_paths);
            ret = -1;
        }
        else if (test_ctx->cnx_server->nb_paths < PICOQUIC_NB_PATH_TARGET) {
            DBG_PRINTF("Only %d paths created on server.\n", test_ctx->cnx_server->nb_paths);
        }
    }

    /* Delete several connection ID */
    for (int i = 2; ret == 0 && i < PICOQUIC_NB_PATH_TARGET; i++) {
        picoquic_cnxid_stash_t * stashed = picoquic_dequeue_cnxid_stash(test_ctx->cnx_client);

        if (stashed == NULL) {
            DBG_PRINTF("Could not retrieve cnx ID #%d.\n", i-1);
        } else {
            ret = picoquic_queue_retire_connection_id_frame(test_ctx->cnx_client, stashed->sequence);
            free(stashed);
        }
    }

    /* run the loop again until no outstanding data */
    if (ret == 0) {
        uint64_t time_out = simulated_time + 8000000;
        int nb_rounds = 0;
        int success = 0;

        while (ret == 0 && simulated_time < time_out &&
            nb_rounds < 2048 && test_ctx->cnx_client->cnx_state != picoquic_state_disconnected) {
            int was_active = 0; 

            ret = tls_api_one_sim_round(test_ctx, &simulated_time, time_out, &was_active);
            nb_rounds++;

            if (test_ctx->cnx_client->nb_paths >= PICOQUIC_NB_PATH_TARGET &&
                test_ctx->cnx_server->nb_paths >= PICOQUIC_NB_PATH_TARGET &&
                test_ctx->cnx_client->first_misc_frame == NULL &&
                test_cnxid_count_stash(test_ctx->cnx_client) >= (PICOQUIC_NB_PATH_TARGET - 1) &&
                picoquic_is_cnx_backlog_empty(test_ctx->cnx_client) &&
                picoquic_is_cnx_backlog_empty(test_ctx->cnx_server)) {
                success = 1;
                break;
            }
        }

        if (ret == 0 && success == 0) {
            DBG_PRINTF("Exit synch loop after %d rounds, backlog or not enough paths (%d & %d).\n",
                nb_rounds, test_ctx->cnx_client->nb_paths, test_ctx->cnx_server->nb_paths);
        }
    }

    /* Check */

    if (ret == 0) {
        if (test_ctx->cnx_server->nb_paths != PICOQUIC_NB_PATH_TARGET + 1) {
            DBG_PRINTF("Found %d paths active on server instead of %d.\n", test_ctx->cnx_server->nb_paths, PICOQUIC_NB_PATH_TARGET+1);
            ret = -1;
        }
    }

    if (ret == 0) {
        ret = transmit_cnxid_test_stash(test_ctx->cnx_client, test_ctx->cnx_server, "client");
    }

    if (ret == 0) {
        ret = transmit_cnxid_test_stash(test_ctx->cnx_server, test_ctx->cnx_client, "server");
    }

    if (test_ctx != NULL) {
        tls_api_delete_ctx(test_ctx);
        test_ctx = NULL;
    }

    return ret;
}


/*
 * Perform a test of the "not before" CNXID function.
 * The test will artificially simulate receiving a "not before"
 * parameter in a new connection ID test, to check that the
 * old connection ID are removed and successfully replaced.
 */
int not_before_cnxid_test()
{
    uint64_t simulated_time = 0;
    uint64_t loss_mask = 0;
    uint64_t not_before;
    picoquic_test_tls_api_ctx_t* test_ctx = NULL;
    int ret = tls_api_init_ctx(&test_ctx, PICOQUIC_INTERNAL_TEST_VERSION_1,
        PICOQUIC_TEST_SNI, PICOQUIC_TEST_ALPN, &simulated_time, NULL, NULL, 0, 0, 0);

    if (ret == 0) {
        ret = tls_api_connection_loop(test_ctx, &loss_mask, 0, &simulated_time);
    }

    /* run a receive loop until no outstanding data */
    if (ret == 0) {
        ret = tls_api_synch_to_empty_loop(test_ctx, &simulated_time, 2048, PICOQUIC_NB_PATH_TARGET, 0);
    }

    /* find a plausible "not before" value,and apply it */
    if (ret == 0) {
        not_before = test_ctx->cnx_server->path[test_ctx->cnx_server->nb_paths - 1]->path_sequence;
        ret = picoquic_remove_not_before_cid(test_ctx->cnx_client, not_before, simulated_time);
    }

    /* run the loop again until no outstanding data */
    if (ret == 0) {
        uint64_t time_out = simulated_time + 8000000;
        int nb_rounds = 0;
        int success = 0;

        while (ret == 0 && simulated_time < time_out &&
            nb_rounds < 2048 && test_ctx->cnx_client->cnx_state != picoquic_state_disconnected) {
            int was_active = 0;

            ret = tls_api_one_sim_round(test_ctx, &simulated_time, time_out, &was_active);
            nb_rounds++;

            if (test_ctx->cnx_client->nb_paths >= PICOQUIC_NB_PATH_TARGET &&
                test_ctx->cnx_server->nb_paths >= PICOQUIC_NB_PATH_TARGET &&
                test_ctx->cnx_client->first_misc_frame == NULL &&
                test_cnxid_count_stash(test_ctx->cnx_client) >= (PICOQUIC_NB_PATH_TARGET - 1) &&
                picoquic_is_cnx_backlog_empty(test_ctx->cnx_client) &&
                picoquic_is_cnx_backlog_empty(test_ctx->cnx_server)) {
                success = 1;
                break;
            }
        }

        if (ret == 0 && success == 0) {
            DBG_PRINTF("Exit synch loop after %d rounds, backlog or not enough paths (%d & %d).\n",
                nb_rounds, test_ctx->cnx_client->nb_paths, test_ctx->cnx_server->nb_paths);
        }
    }

    /* Check */

    if (ret == 0) {
        if (test_ctx->cnx_server->nb_paths != PICOQUIC_NB_PATH_TARGET) {
            DBG_PRINTF("Found %d paths active on server instead of %d.\n", test_ctx->cnx_server->nb_paths, PICOQUIC_NB_PATH_TARGET + 1);
            ret = -1;
        }
    }

    if (ret == 0) {
        ret = transmit_cnxid_test_stash(test_ctx->cnx_client, test_ctx->cnx_server, "client");
    }

    if (ret == 0) {
        ret = transmit_cnxid_test_stash(test_ctx->cnx_server, test_ctx->cnx_client, "server");
    }

    if (test_ctx != NULL) {
        tls_api_delete_ctx(test_ctx);
        test_ctx = NULL;
    }

    return ret;
}

/*
 * Server busy. Verify that the connection fails with the proper error code, and then that once the server is not busy the next connection succeeds.
 */

int server_busy_test()
{
    uint64_t loss_mask = 0;
    uint64_t simulated_time = 0;
    picoquic_test_tls_api_ctx_t* test_ctx = NULL;
    int ret = tls_api_init_ctx(&test_ctx, 0, PICOQUIC_TEST_SNI, PICOQUIC_TEST_ALPN, &simulated_time, NULL, NULL, 0, 0, 0);

    if (ret == 0) {
        test_ctx->qserver->flags |= picoquic_context_server_busy;
        (void) tls_api_connection_loop(test_ctx, &loss_mask, 0, &simulated_time);

        if (test_ctx->cnx_server != NULL &&
            test_ctx->cnx_server->cnx_state != picoquic_state_disconnected) {
            DBG_PRINTF("Server state: %d, local error: %x\n", test_ctx->cnx_server->cnx_state, test_ctx->cnx_server->local_error);
            ret = -1;
        }
        else if (test_ctx->cnx_client->cnx_state != picoquic_state_disconnected ||
            test_ctx->cnx_client->remote_error != PICOQUIC_TRANSPORT_SERVER_BUSY) {
            DBG_PRINTF("Client state: %d, remote error: %x", test_ctx->cnx_client->cnx_state, test_ctx->cnx_client->remote_error);
            ret = -1;
        }
        else if (simulated_time > 50000ull) {
            DBG_PRINTF("Simulated time: %llu", (unsigned long long)simulated_time);
            ret = -1;
        }
    }

    if (ret == 0) {
        test_ctx->qserver->flags &= ~picoquic_context_server_busy;

        if (test_ctx->cnx_server != NULL) {
            picoquic_delete_cnx(test_ctx->cnx_server);
            test_ctx->cnx_server = NULL;
        }
        if (test_ctx->cnx_client != NULL) {
            picoquic_delete_cnx(test_ctx->cnx_client);
            test_ctx->cnx_client = NULL;
        }

        /* Create a new client connection */
        test_ctx->cnx_client = picoquic_create_cnx(test_ctx->qclient,
            picoquic_null_connection_id, picoquic_null_connection_id,
            (struct sockaddr*)&test_ctx->server_addr, simulated_time,
            0, PICOQUIC_TEST_SNI, PICOQUIC_TEST_ALPN, 1);

        if (test_ctx->cnx_client == NULL) {
            ret = -1;
        } else {
            ret = picoquic_start_client_cnx(test_ctx->cnx_client);
        }
    }

    if (ret == 0) {
        ret = tls_api_connection_loop(test_ctx, &loss_mask, 0, &simulated_time);
    }

    if (ret == 0) {
        ret = tls_api_attempt_to_close(test_ctx, &simulated_time);
    }

    if (test_ctx != NULL) {
        tls_api_delete_ctx(test_ctx);
        test_ctx = NULL;
    }

    return ret;
}

/*
 * Initial close test. Check what happens when the client closes a connection without waiting for the full establishment
 */

int initial_close_test()
{
    uint64_t loss_mask = 0;
    uint64_t simulated_time = 0;
    int was_active = 0;
    picoquic_test_tls_api_ctx_t* test_ctx = NULL;
    int ret = tls_api_init_ctx(&test_ctx, 0, PICOQUIC_TEST_SNI, PICOQUIC_TEST_ALPN, &simulated_time, NULL, NULL, 0, 0, 0);

    if (ret == 0) {
        /* Send the initial packet, but no more than that */
        ret = tls_api_one_sim_round(test_ctx, &simulated_time, 0, &was_active);

        if (ret == 0) {
            test_ctx->cnx_client->cnx_state = picoquic_state_handshake_failure;
            test_ctx->cnx_client->local_error = 0xDEAD;
            picoquic_reinsert_by_wake_time(test_ctx->qclient, test_ctx->cnx_client, simulated_time);
        }
    }

    if (ret == 0) {
        for (int i = 0; i < 128; i++) {
            ret = tls_api_one_sim_round(test_ctx, &simulated_time, 0, &was_active);
            if (test_ctx->cnx_server != NULL) {
                break;
            }
        }
        if (ret == 0) {
            ret = tls_api_connection_loop(test_ctx, &loss_mask, 0, &simulated_time);
        }

        if (ret == 0) {
            if (test_ctx->cnx_server == NULL) {
                DBG_PRINTF("%s", "Server connection deleted, cannot verify error code.\n");
                ret = -1;
            }
            else if (test_ctx->cnx_server->cnx_state != picoquic_state_disconnected ||
                test_ctx->cnx_server->remote_error != 0xDEAD) {
                DBG_PRINTF("Server state: %d, remote error: %x\n", test_ctx->cnx_server->cnx_state, test_ctx->cnx_server->remote_error);
                ret = -1;
            }
            else if (test_ctx->cnx_client->cnx_state != picoquic_state_disconnected) {
                DBG_PRINTF("Client state: %d, local error: %x", test_ctx->cnx_client->cnx_state, test_ctx->cnx_client->local_error);
                ret = -1;
            }
            else if (simulated_time > 50000ull) {
                DBG_PRINTF("Simulated time: %llu", (unsigned long long)simulated_time);
                ret = -1;
            }
        }
    }

    if (test_ctx != NULL) {
        tls_api_delete_ctx(test_ctx);
        test_ctx = NULL;
    }

    return ret;
}

/*
 * Check what happens if the server detects an error in the client's initial
 * message. Verify that the client receives the error code.
 */

int initial_server_close_test()
{
    uint64_t loss_mask = 0;
    uint64_t simulated_time = 0;
    int was_active = 0;
    int nb_trials = 0;
    picoquic_test_tls_api_ctx_t* test_ctx = NULL;
    int ret = tls_api_init_ctx(&test_ctx, 0, PICOQUIC_TEST_SNI, PICOQUIC_TEST_ALPN, &simulated_time, NULL, NULL, 0, 0, 0);

    /* Set the connection on the server side, but not on the client side */
    while (ret == 0 && nb_trials < 32 ) {
        nb_trials++;

        ret = tls_api_one_sim_round(test_ctx, &simulated_time, 0, &was_active);

        if (test_ctx->cnx_server != NULL && test_ctx->cnx_server->cnx_state == picoquic_state_server_almost_ready) {
            break;
        }
    }

    if (test_ctx->cnx_server == NULL || test_ctx->cnx_server->cnx_state != picoquic_state_server_almost_ready) {
        DBG_PRINTF("Server state: %d\n", (test_ctx->cnx_server == NULL) ?
            -1 : test_ctx->cnx_server->cnx_state);
        ret = -1;
    }

    if (ret == 0) {
        test_ctx->cnx_server->cnx_state = picoquic_state_handshake_failure;
        test_ctx->cnx_server->local_error = 0xDEAD;
        picoquic_reinsert_by_wake_time(test_ctx->qserver, test_ctx->cnx_server, simulated_time);
    }


    if (ret == 0) {
        ret = tls_api_connection_loop(test_ctx, &loss_mask, 0, &simulated_time);
    }

    if (ret == 0) {
        if (test_ctx->cnx_server != NULL &&
            test_ctx->cnx_server->cnx_state != picoquic_state_disconnected) {
            DBG_PRINTF("Server state: %d, remote error: %x\n", test_ctx->cnx_server->cnx_state, test_ctx->cnx_server->remote_error);
            ret = -1;
        }
        else if (test_ctx->cnx_client->cnx_state != picoquic_state_disconnected ||
            test_ctx->cnx_client->remote_error != 0xDEAD) {
            DBG_PRINTF("Client state: %d, local error: %x", test_ctx->cnx_client->cnx_state, test_ctx->cnx_client->local_error);
            ret = -1;
        }
        else if (simulated_time > 50000ull) {
            DBG_PRINTF("Simulated time: %llu", (unsigned long long)simulated_time);
            ret = -1;
        }
    }

    if (test_ctx != NULL) {
        tls_api_delete_ctx(test_ctx);
        test_ctx = NULL;
    }

    return ret;
}


/*
 * Test that rotated keys are computed in a compatible way on client and server.
 */

static int aead_iv_check(void * aead1, void * aead2)
{
    int ret = 0; 
    ptls_aead_context_t *ctx1 = (ptls_aead_context_t *)aead1;
    ptls_aead_context_t *ctx2 = (ptls_aead_context_t *)aead2;

    if (memcmp(ctx1->static_iv, ctx2->static_iv, ctx1->algo->iv_size) != 0) {
        ret = -1;
    }
    return ret;
}

#if 0
static int pn_enc_check(void * pn1, void * pn2)
{
    int ret = 0;
    uint8_t seed[16] = { 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16 };
    uint8_t pn[4] = { 0, 1, 2 ,3 };
    uint8_t pn_enc[4];
    uint8_t pn_dec[4];

    picoquic_pn_encrypt(pn1, seed, pn_enc, pn, 4);
    picoquic_pn_encrypt(pn2, seed, pn_dec, pn_enc, 4);

    if (memcmp(pn_dec, pn, 4) != 0) {
        ret = -1;
    }
    return ret;
}
#endif

int new_rotated_key_test()
{
    uint64_t loss_mask = 0;
    uint64_t simulated_time = 0;
    picoquic_test_tls_api_ctx_t* test_ctx = NULL;
    int ret = tls_api_init_ctx(&test_ctx, 0, PICOQUIC_TEST_SNI, PICOQUIC_TEST_ALPN, &simulated_time, NULL, NULL, 0, 0, 0);

    if (ret == 0) {
        ret = tls_api_connection_loop(test_ctx, &loss_mask, 0, &simulated_time);
    }

    if (ret == 0) {
        ret = wait_application_aead_ready(test_ctx, &simulated_time);
    }


    for (int i = 1; ret == 0 && i <= 3; i++) {
        
        /* Try to compute rotated keys on server */
        ret = picoquic_compute_new_rotated_keys(test_ctx->cnx_server);
        if (ret != 0) {
            DBG_PRINTF("Could not rotate server key, ret: %x\n", ret);
        } else {
            /* Try to compute rotated keys on client */
            ret = picoquic_compute_new_rotated_keys(test_ctx->cnx_client);
            if (ret != 0) {
                DBG_PRINTF("Could not rotate client key, round %d, ret: %x\n", i, ret);
            }
        }

        if (ret == 0)
        {
            /* Compare server encryption and client decryption */
            size_t key_size = picoquic_get_app_secret_size(test_ctx->cnx_client);

            if (key_size != picoquic_get_app_secret_size(test_ctx->cnx_server)) {
                DBG_PRINTF("Round %d. Key sizes dont match, client: %d, server: %d\n", i, key_size, picoquic_get_app_secret_size(test_ctx->cnx_server));
                ret = -1;
            }
            else if (memcmp(picoquic_get_app_secret(test_ctx->cnx_server, 1), picoquic_get_app_secret(test_ctx->cnx_client, 0), key_size) != 0) {
                DBG_PRINTF("Round %d. Server encryption secret does not match client decryption secret\n", i);
                ret = -1;
            }
            else if (memcmp(picoquic_get_app_secret(test_ctx->cnx_server, 0), picoquic_get_app_secret(test_ctx->cnx_client, 1), key_size) != 0) {
                DBG_PRINTF("Round %d. Server decryption secret does not match client encryption secret\n", i);
                ret = -1;
            }
            else if (aead_iv_check(test_ctx->cnx_server->crypto_context_new.aead_encrypt, test_ctx->cnx_client->crypto_context_new.aead_decrypt) != 0) {
                DBG_PRINTF("Round %d. Client AEAD decryption does not match server AEAD encryption.\n", i);
                ret = -1;
            }
            else if (aead_iv_check(test_ctx->cnx_client->crypto_context_new.aead_encrypt, test_ctx->cnx_server->crypto_context_new.aead_decrypt) != 0) {
                DBG_PRINTF("Round %d. Server AEAD decryption does not match cliens AEAD encryption.\n", i);
                ret = -1;
            }
#if 0
            else if (pn_enc_check(test_ctx->cnx_server->crypto_context_new.pn_enc, test_ctx->cnx_client->crypto_context_new.pn_dec) != 0) {
                DBG_PRINTF("Round %d. Client PN decryption does not match server PN encryption.\n", i);
                ret = -1;
            }
            else if (pn_enc_check(test_ctx->cnx_client->crypto_context_new.pn_enc, test_ctx->cnx_server->crypto_context_new.pn_dec) != 0) {
                DBG_PRINTF("Round %d. Server PN decryption does not match client PN encryption.\n", i);
                ret = -1;
            }
#endif
        }

        picoquic_crypto_context_free(&test_ctx->cnx_server->crypto_context_new);
        picoquic_crypto_context_free(&test_ctx->cnx_client->crypto_context_new);
    }

    if (test_ctx != NULL) {
        tls_api_delete_ctx(test_ctx);
        test_ctx = NULL;
    }

    return ret;
}


/*
 * Key rotation tests
 */

static int inject_false_rotation(picoquic_test_tls_api_ctx_t* test_ctx, int target_client, uint64_t simulated_time)
{
    /* In order to test robustness of key rotation against attacks, we inject a
     * random packet with properly set header indication transition */
    int ret = 0;
    picoquic_cnx_t * cnx = (target_client) ? test_ctx->cnx_client : test_ctx->cnx_server;
    picoquictest_sim_link_t* target_link = (target_client) ? test_ctx->s_to_c_link : test_ctx->c_to_s_link;
    picoquictest_sim_packet_t* packet = picoquictest_sim_link_create_packet();

    if (packet == NULL || cnx == NULL) {
        ret = -1;
    }
    else {
        uint64_t random_context = (0x123456789ABCDEF0ull)|cnx->pkt_ctx[picoquic_packet_context_application].send_sequence;
        size_t byte_index = 1;

        packet->bytes[0] = 0x3F | ((cnx->key_phase_dec) ? 0 : 0x40); /* Set phase to opposite of expected value */

        for (uint8_t i = 0; i < cnx->path[0]->local_cnxid.id_len; i++) {
            packet->bytes[byte_index++] = cnx->path[0]->local_cnxid.id[i];
        }
        picoquic_test_random_bytes(&random_context, packet->bytes + byte_index, 128u - byte_index);
        packet->length = 128;

        if (target_client) {
            picoquic_store_addr(&packet->addr_from, (struct sockaddr *)&test_ctx->server_addr);
            picoquic_store_addr(&packet->addr_to, (struct sockaddr *)&test_ctx->client_addr);
        }
        else {
            picoquic_store_addr(&packet->addr_from, (struct sockaddr *)&test_ctx->client_addr);
            picoquic_store_addr(&packet->addr_to, (struct sockaddr *)&test_ctx->server_addr);
        }

        picoquictest_sim_link_submit(target_link, packet, simulated_time);
    }

    return ret;
}

static int key_rotation_test_one(int inject_bad_packet)
{
    uint64_t simulated_time = 0;
    uint64_t loss_mask = 0;
    int nb_trials = 0;
    int nb_inactive = 0;
    int max_trials = 100000;
    int nb_rotation = 0;
    uint64_t rotation_sequence = 100;
    uint64_t injection_sequence = 50;
    picoquic_test_tls_api_ctx_t* test_ctx = NULL;
    int ret = tls_api_init_ctx(&test_ctx, PICOQUIC_INTERNAL_TEST_VERSION_1,
        PICOQUIC_TEST_SNI, PICOQUIC_TEST_ALPN, &simulated_time, NULL, NULL, 0, 0, 0);

    if (ret == 0 && test_ctx == NULL) {
        ret = PICOQUIC_ERROR_MEMORY;
    }

    if (ret == 0) {
        ret = tls_api_connection_loop(test_ctx, &loss_mask, 0, &simulated_time);
    }

    /* Prepare to send data */
    if (ret == 0) {
        ret = test_api_init_send_recv_scenario(test_ctx, test_scenario_sustained, sizeof(test_scenario_sustained));
    }

    /* Perform a data sending loop, during which various key rotations are tried
     * every 100 packets or so. To test robustness, inject bogus packets that
     * mimic a transition trigger */

    while (ret == 0 && nb_trials < max_trials && nb_inactive < 256 && TEST_CLIENT_READY && TEST_SERVER_READY) {
        int was_active = 0;

        nb_trials++;

        if (inject_bad_packet &&
            test_ctx->cnx_server->pkt_ctx[picoquic_packet_context_application].send_sequence > injection_sequence) {
            ret = inject_false_rotation(test_ctx, inject_bad_packet >> 1, simulated_time);
            if (ret != 0) {
                DBG_PRINTF("Could not inject bad packet, ret = %d\n", ret);
                break;
            }
            else {
                injection_sequence += 50;
            }
        }

        if (test_ctx->cnx_server->pkt_ctx[picoquic_packet_context_application].send_sequence > rotation_sequence &&
            test_ctx->cnx_server->pkt_ctx[picoquic_packet_context_application].first_sack_item.end_of_sack_range >
            test_ctx->cnx_server->crypto_epoch_sequence &&
            test_ctx->cnx_client->pkt_ctx[picoquic_packet_context_application].first_sack_item.end_of_sack_range >
            test_ctx->cnx_client->crypto_epoch_sequence &&
            test_ctx->cnx_server->key_phase_enc == test_ctx->cnx_server->key_phase_dec &&
            test_ctx->cnx_client->key_phase_enc == test_ctx->cnx_client->key_phase_dec) {
            rotation_sequence = test_ctx->cnx_server->pkt_ctx[picoquic_packet_context_application].send_sequence + 100;
            injection_sequence = test_ctx->cnx_server->pkt_ctx[picoquic_packet_context_application].send_sequence + 50;
            nb_rotation++;
            switch (nb_rotation) {
            case 1: /* Key rotation at the client */
                ret = picoquic_start_key_rotation(test_ctx->cnx_client);
                break;
            case 2: /* Key rotation at the server */
                ret = picoquic_start_key_rotation(test_ctx->cnx_server);
                break;
            case 3: /* Simultaneous key rotation at the client */
                rotation_sequence += 1000000000;
                ret = picoquic_start_key_rotation(test_ctx->cnx_client);
                if (ret == 0) {
                    ret = picoquic_start_key_rotation(test_ctx->cnx_server);
                }
                break;
            default:
                break;
            }

            if (ret != 0) {
                DBG_PRINTF("Could not start rotation #%d, ret = %x\n", nb_rotation, ret);
            }
        }

        if (ret == 0) {
            ret = tls_api_one_sim_round(test_ctx, &simulated_time, 0, &was_active);
        }

        if (ret < 0)
        {
            break;
        }

        if (was_active) {
            nb_inactive = 0;
        }
        else {
            nb_inactive++;
        }

        if (test_ctx->test_finished) {
            if (picoquic_is_cnx_backlog_empty(test_ctx->cnx_client) && picoquic_is_cnx_backlog_empty(test_ctx->cnx_server)) {
                break;
            }
        }
    }

    if (ret == 0 && nb_rotation < 3) {
        DBG_PRINTF("Only %d key rotations completed out of 3\n", nb_rotation);
        ret = -1;
    }

    if (ret == 0) {
        ret = tls_api_attempt_to_close(test_ctx, &simulated_time);

        if (ret != 0)
        {
            DBG_PRINTF("Connection close returns %d\n", ret);
        }
    }

    if (test_ctx != NULL) {
        tls_api_delete_ctx(test_ctx);
        test_ctx = NULL;
    }

    return ret;
}

int key_rotation_test()
{
    int ret = key_rotation_test_one(0);

    if (ret == 0) {
        /* test rotation with injection of bad packets on client */
        ret = key_rotation_test_one(2);
        if (ret != 0) {
            DBG_PRINTF("%s", "Packet injection on client defeats rotation.\n", ret);
        }
    }

    if (ret == 0) {
        /* test rotation with injection of bad packets on server */
        ret = key_rotation_test_one(1);
        if (ret != 0) {
            DBG_PRINTF("%s", "Packet injection on server defeats rotation.\n", ret);
        }
    }

    return ret;
}

/*
 * Key rotation stress: mimic a client that rotates its keys very rapidly.
 * Expected results: the server should survive. The server connection should be
 * deleted or closed.
 */

static int key_rotation_stress_test_one(int nb_packets)
{
    uint64_t simulated_time = 0;
    uint64_t closing_time = 0;
    uint64_t loss_mask = 0;
    int nb_trials = 0;
    int nb_inactive = 0;
    int max_trials = 100000;
    int max_rotations = 100;
    int nb_rotation = 0;
    uint64_t rotation_sequence = 100;
    picoquic_test_tls_api_ctx_t* test_ctx = NULL;
    int ret = tls_api_init_ctx(&test_ctx, PICOQUIC_INTERNAL_TEST_VERSION_1,
        PICOQUIC_TEST_SNI, PICOQUIC_TEST_ALPN, &simulated_time, NULL, NULL, 0, 0, 0);

    if (ret == 0 && test_ctx == NULL) {
        ret = PICOQUIC_ERROR_MEMORY;
    }

    if (ret == 0) {
        ret = tls_api_connection_loop(test_ctx, &loss_mask, 0, &simulated_time);
    }

    /* Prepare to send data */
    if (ret == 0) {
        ret = test_api_init_send_recv_scenario(test_ctx, test_scenario_sustained, sizeof(test_scenario_sustained));
    }

    /* Perform a data sending loop, during which various key rotations are tried
     * every "nb_packets". */

    while (ret == 0 && nb_trials < max_trials && nb_inactive < 256 && TEST_CLIENT_READY && TEST_SERVER_READY) {
        int was_active = 0;

        nb_trials++;

        if (test_ctx->cnx_client->pkt_ctx[picoquic_packet_context_application].send_sequence > rotation_sequence &&
            test_ctx->cnx_client->key_phase_enc == test_ctx->cnx_client->key_phase_dec) {
            rotation_sequence = test_ctx->cnx_client->pkt_ctx[picoquic_packet_context_application].send_sequence + nb_packets;
            nb_rotation++;
            if (nb_rotation > max_rotations) {
                break;
            }
            else {
                ret = picoquic_start_key_rotation(test_ctx->cnx_client);
                if (ret != 0) {
                    DBG_PRINTF("Start key rotation returns %d\n", ret);
                }
            }
        }

        if (ret == 0) {
            ret = tls_api_one_sim_round(test_ctx, &simulated_time, 0, &was_active);
        }

        if (ret != 0)
        {
            break;
        }

        if (was_active) {
            nb_inactive = 0;
        }
        else {
            nb_inactive++;
        }

        if (test_ctx->test_finished) {
            if (picoquic_is_cnx_backlog_empty(test_ctx->cnx_client) && picoquic_is_cnx_backlog_empty(test_ctx->cnx_server)) {
                break;
            }
        }
    }

    if (ret == 0 && TEST_CLIENT_READY) {
        ret = tls_api_attempt_to_close(test_ctx, &simulated_time);

        if (ret != 0) {
            DBG_PRINTF("Connection close returns %d\n", ret);
        }
    }

    /*
     * Allow for some time for the server connection to close.
     */
    closing_time = simulated_time + 4000000;
    while (ret == 0 && simulated_time < closing_time) {
        int was_active = 0; 
        if (test_ctx->cnx_server == NULL) {
            ret = -1;
            break;
        }
        if (test_ctx->qserver->cnx_list == NULL || test_ctx->cnx_server->cnx_state == picoquic_state_disconnected) {
            break;
        }

        ret = tls_api_one_sim_round(test_ctx, &simulated_time, 0, &was_active);
    }

    if (test_ctx != NULL) {
        tls_api_delete_ctx(test_ctx);
        test_ctx = NULL;
    }

    return ret;
}

int key_rotation_stress_test()
{
    return key_rotation_stress_test_one(10);
}


/*
 * False migration. Test that the client server connection resists injection of
 * some packets sent from a wrong address. The "false migration inject" acts as
 * a misbehaving NAT. The expectation is that the server will ignore handshake
 * packets from a wrong origin, and that the connection will recover from
 * false packet injection during the data phase.
 */

int false_migration_inject(picoquic_test_tls_api_ctx_t* test_ctx, int target_client, picoquic_packet_context_enum false_pc, uint64_t simulated_time)
{

    /* In order to test robustness of key rotation against attacks, we inject a
     * random packet with properly set header indication transition */
    int ret = 0;
    picoquic_cnx_t * cnx = (target_client) ? test_ctx->cnx_client : test_ctx->cnx_server;
    picoquictest_sim_link_t* target_link = (target_client) ? test_ctx->c_to_s_link : test_ctx->s_to_c_link;
    picoquictest_sim_packet_t* sim_packet = picoquictest_sim_link_create_packet();
    picoquic_packet_t* packet = NULL;

    if (cnx == NULL) {
        return -1;
    }

    packet = picoquic_create_packet(cnx->quic);

    if (sim_packet == NULL || packet == NULL || cnx == NULL) {
        if (sim_packet != NULL) {
            free(sim_packet);
        }
        if (packet != NULL) {
            picoquic_recycle_packet(cnx->quic, packet);
        }
        ret = -1;
    }
    else {
        struct sockaddr_in false_address;
        size_t checksum_overhead = 8;
        uint32_t header_length = 0;
        size_t length = 0;
        int is_cleartext_mode = 0;
        picoquic_path_t * path_x = cnx->path[0];

        switch (false_pc) {
        case picoquic_packet_context_application:
            packet->ptype = picoquic_packet_1rtt_protected;
            break;
        case picoquic_packet_context_handshake:
            packet->ptype = picoquic_packet_handshake;
            is_cleartext_mode = 1;
            break;
        case picoquic_packet_context_initial:
        default:
            packet->ptype = picoquic_packet_initial;
            is_cleartext_mode = 1;
            break;
        }

        if (target_client) {
            memcpy(&false_address, &test_ctx->client_addr, sizeof(false_address));
        }
        else {
            memcpy(&false_address, &test_ctx->server_addr, sizeof(false_address));
        }
        false_address.sin_port += 1234;


        checksum_overhead = picoquic_get_checksum_length(cnx, is_cleartext_mode);
        packet->checksum_overhead = checksum_overhead;
        packet->pc = false_pc;
        length = checksum_overhead + 32;
        memset(packet->bytes, 0, length);

        picoquic_finalize_and_protect_packet(cnx, packet,
            ret, length, header_length, checksum_overhead,
            &sim_packet->length, sim_packet->bytes, PICOQUIC_MAX_PACKET_SIZE,
            &path_x->remote_cnxid, &path_x->local_cnxid, path_x, simulated_time);

        picoquic_store_addr(&sim_packet->addr_from, (struct sockaddr *)&false_address);

        if (target_client) {
            picoquic_store_addr(&sim_packet->addr_to, (struct sockaddr *)&test_ctx->server_addr);
        }
        else {
            picoquic_store_addr(&sim_packet->addr_to, (struct sockaddr *)&test_ctx->client_addr);
        }

        picoquictest_sim_link_submit(target_link, sim_packet, simulated_time);
    }

    return ret;
}

int false_migration_test_scenario(test_api_stream_desc_t * scenario, size_t size_of_scenario, uint64_t loss_target, int target_client, picoquic_packet_context_enum false_pc, uint64_t false_rank)
{
    uint64_t simulated_time = 0;
    int nb_injected = 0;
    int nb_trials = 0;
    int nb_inactive = 0;
    picoquic_test_tls_api_ctx_t* test_ctx = NULL;
    int ret = tls_api_init_ctx(&test_ctx, PICOQUIC_INTERNAL_TEST_VERSION_1,
        PICOQUIC_TEST_SNI, PICOQUIC_TEST_ALPN, &simulated_time, NULL, NULL, 0, 0, 0);

    if (ret == 0 && test_ctx == NULL) {
        ret = PICOQUIC_ERROR_MEMORY;
    }

    /* Run a connection loop with injection test */
    if (ret == 0) {

        while (ret == 0 && nb_trials < 1024 && nb_inactive < 512 && (!TEST_CLIENT_READY || (test_ctx->cnx_server == NULL || !TEST_SERVER_READY))) {
            int was_active = 0;
            nb_trials++;

            if (nb_injected == 0) {
                if ((target_client && test_ctx->cnx_client->pkt_ctx[false_pc].send_sequence > false_rank && test_ctx->cnx_client->path[0]->remote_cnxid.id_len != 0) ||
                    (!target_client && test_ctx->cnx_server != NULL && test_ctx->cnx_server->pkt_ctx[false_pc].send_sequence > false_rank)) {
                    /* Inject a spoofed packet in the context */
                    ret = false_migration_inject(test_ctx, target_client, false_pc, simulated_time);
                    if (ret == 0) {
                        nb_injected++;
                    }
                    else
                    {
                        DBG_PRINTF("Could not inject false packet, ret = %x\n", ret);
                    }
                }
            }

            ret = tls_api_one_sim_round(test_ctx, &simulated_time, 0, &was_active);

            if (test_ctx->cnx_client->cnx_state == picoquic_state_disconnected &&
                (test_ctx->cnx_server == NULL || test_ctx->cnx_server->cnx_state == picoquic_state_disconnected)) {
                break;
            }

            if (was_active) {
                nb_inactive = 0;
            }
            else {
                nb_inactive++;
            }
        }
    }

    /* Prepare to send data */
    if (ret == 0) {
        ret = test_api_init_send_recv_scenario(test_ctx, scenario, size_of_scenario);
    }

    /* Perform a data sending loop */
    nb_trials = 0;
    nb_inactive = 0;

    /* Perform a data sending loop, during which various key rotations are tried
     * every 100 packets or so. To test robustness, inject bogus packets that
     * mimic a transition trigger */

    while (ret == 0 && nb_trials < 1024 && nb_inactive < 256 && TEST_CLIENT_READY && TEST_SERVER_READY) {
        int was_active = 0;

        nb_trials++;

        if (nb_injected == 0) {
            if ((target_client && test_ctx->cnx_client->pkt_ctx[false_pc].send_sequence > false_rank) ||
                (!target_client && test_ctx->cnx_server != NULL && test_ctx->cnx_server->pkt_ctx[false_pc].send_sequence > false_rank)) {
                /* Inject a spoofed packet in the context */
                ret = false_migration_inject(test_ctx, target_client, false_pc, simulated_time);
                if (ret == 0) {
                    nb_injected++;
                }
                else
                {
                    DBG_PRINTF("Could not inject false packet, ret = %x\n", ret);
                }
            }
        }

        if (ret == 0) {
            ret = tls_api_one_sim_round(test_ctx, &simulated_time, 0, &was_active);
        }

        if (ret < 0)
        {
            break;
        }

        if (was_active) {
            nb_inactive = 0;
        }
        else {
            nb_inactive++;
        }

        if (test_ctx->test_finished) {
            if (picoquic_is_cnx_backlog_empty(test_ctx->cnx_client) && picoquic_is_cnx_backlog_empty(test_ctx->cnx_server)) {
                break;
            }
        }
    }
    if (ret == 0 && nb_injected == 0) {
        DBG_PRINTF("Could not inject after packet #%d in context %d\n", (int)false_rank, (int)false_pc);
        ret = -1;
    }

    if (ret == 0) {
        ret = tls_api_attempt_to_close(test_ctx, &simulated_time);

        if (ret != 0)
        {
            DBG_PRINTF("Connection close returns %d\n", ret);
        }
    }

    if (test_ctx != NULL) {
        tls_api_delete_ctx(test_ctx);
        test_ctx = NULL;
    }

    return ret;
}

int false_migration_test()
{
    int ret = 0;
    int target_client;

    for (target_client = 1; ret == 0 && target_client >= 0; target_client--) {
        ret = false_migration_test_scenario(test_scenario_q2_and_r2, sizeof(test_scenario_q2_and_r2), 0, target_client, picoquic_packet_context_initial, 0);
        
        if (ret == 0) {
            ret = false_migration_test_scenario(test_scenario_q2_and_r2, sizeof(test_scenario_q2_and_r2), 0, target_client, picoquic_packet_context_handshake, 0);
        }

        for (uint64_t seq = 0; ret == 0 && seq < 4; seq++) {
            ret = false_migration_test_scenario(test_scenario_q2_and_r2, sizeof(test_scenario_q2_and_r2), 0, target_client, picoquic_packet_context_application, seq);
        }
    }

    return ret;
}

/*
* Testing what happens in case of NAT rebinding during handshake.
* In theory, it should cause the handshake to fail
*/

int nat_handshake_test_one(int test_rank)
{
    uint64_t simulated_time = 0;
    uint64_t loss_mask = 0;
    int nb_inactive = 0;
    int nb_trials = 0;
    int natted = 0;
    picoquic_test_tls_api_ctx_t* test_ctx = NULL;
    int ret = tls_api_init_ctx(&test_ctx, PICOQUIC_INTERNAL_TEST_VERSION_1,
        PICOQUIC_TEST_SNI, PICOQUIC_TEST_ALPN, &simulated_time, NULL, NULL, 0, 0, 0);

    if (ret == 0 && test_ctx == NULL) {
        ret = PICOQUIC_ERROR_MEMORY;
    }

    /* Run a connection loop with rebinding test */
    if (ret == 0) {

        while (ret == 0 && nb_trials < 1024 && nb_inactive < 512 && (!TEST_CLIENT_READY || (test_ctx->cnx_server == NULL || !TEST_SERVER_READY))) {
            int was_active = 0;
            nb_trials++;

            if (natted == 0) {
                int should_nat = 0;

                switch (test_rank) {
                case 0: /* check that at least one packet was received from the server, setting the CNX_ID */
                    should_nat = (test_ctx->cnx_client->path[0]->remote_cnxid.id_len > 0);
                    break;
                case 1: /* Check that the connection is almost complete, but finished has not been sent */
                    should_nat = (test_ctx->cnx_client->crypto_context[3].aead_decrypt != NULL);
                    break;
                default:
                    break;
                }
                if (should_nat) {
                    /* Simulate a NAT rebinding */
                    test_ctx->client_addr.sin_port += 17;
                    test_ctx->client_use_nat = 1;
                    natted++;
                }
            }

            ret = tls_api_one_sim_round(test_ctx, &simulated_time, 0, &was_active);

            if (test_ctx->cnx_client->cnx_state == picoquic_state_disconnected ||
                (test_ctx->cnx_server != NULL && test_ctx->cnx_server->cnx_state == picoquic_state_disconnected)) {
                break;
            }

            if (was_active) {
                nb_inactive = 0;
            }
            else {
                nb_inactive++;
            }
        }
    }

    /* Prepare to send data */
    if (ret == 0) {
        ret = test_api_init_send_recv_scenario(test_ctx, test_scenario_q2_and_r2, sizeof(test_scenario_q2_and_r2));
    }

    /* Try send data */
    if (ret == 0) {
        ret = tls_api_data_sending_loop(test_ctx, &loss_mask, &simulated_time, 0);
    }

    if (ret == 0) {
        ret = tls_api_attempt_to_close(test_ctx, &simulated_time);

        if (ret != 0)
        {
            DBG_PRINTF("Connection close returns %d\n", ret);
        }
    }

    /* verify that the connection did change address */
    if (ret == 0 && !natted) {
        DBG_PRINTF("Connection succeeded after %d natting in handshake, rank %d\n", natted, test_rank);
        ret = -1;
    }

    if (test_ctx != NULL) {
        tls_api_delete_ctx(test_ctx);
        test_ctx = NULL;
    }


    return ret;
}

int nat_handshake_test()
{
    int ret = 0;

    for (int test_rank = 0; test_rank < 2; test_rank++) {
        ret = nat_handshake_test_one(test_rank);
    }

    return ret;
}

/*
 * Verify that connection attempts with a too-short CID are rejected.
 */
static int short_initial_cid_test_one(uint8_t cid_length)
{
    uint64_t simulated_time = 0;
    uint64_t loss_mask = 0;
    picoquic_test_tls_api_ctx_t* test_ctx = NULL;
    int ret = tls_api_init_ctx(&test_ctx, 0, PICOQUIC_TEST_SNI, PICOQUIC_TEST_ALPN, &simulated_time, NULL, NULL, 0, 0, 0);

    if (ret == 0 && test_ctx == NULL) {
        ret = -1;
    }

    /* Delete the client context, and recreate with a short CID */
    if (ret == 0)
    {
        picoquic_connection_id_t init_cid;

        for (unsigned int i = 0; i < cid_length; i++) {
            init_cid.id[i] = (uint8_t)(i + 1);
        }
        init_cid.id_len = cid_length;

        if (test_ctx->cnx_client != NULL) {
            picoquic_delete_cnx(test_ctx->cnx_client);
            test_ctx->cnx_client = NULL;
        }

        test_ctx->cnx_client = picoquic_create_cnx(test_ctx->qclient, init_cid,
            picoquic_null_connection_id,
            (struct sockaddr*)&test_ctx->server_addr, 0,
            0, PICOQUIC_TEST_SNI, PICOQUIC_TEST_ALPN, 1);

        if (test_ctx->cnx_client == NULL) {
            ret = -1;
        }
        else {
            ret = picoquic_start_client_cnx(test_ctx->cnx_client);
        }
    }

    /* Proceed with the connection loop. */
    if (ret == 0) {
        ret = tls_api_connection_loop(test_ctx, &loss_mask, 0, &simulated_time);
    }

    /* Check that both the client and server are ready. */
    if (ret == 0 && TEST_CLIENT_READY) {
        if (cid_length < PICOQUIC_ENFORCED_INITIAL_CID_LENGTH) {
            DBG_PRINTF("Connection succeeds despites cid_length %d < %d\n",
                cid_length, PICOQUIC_ENFORCED_INITIAL_CID_LENGTH);
            ret = -1;
        }
    }
    else if (cid_length > PICOQUIC_ENFORCED_INITIAL_CID_LENGTH) {
        DBG_PRINTF("Connection fails despites cid_length %d >= %d\n",
            cid_length, PICOQUIC_ENFORCED_INITIAL_CID_LENGTH);
        if (ret == 0) {
            ret = -1;
        }
    }
    else if (cid_length < PICOQUIC_ENFORCED_INITIAL_CID_LENGTH) {
        ret = 0;
    }

    if (test_ctx != NULL) {
        tls_api_delete_ctx(test_ctx);
        test_ctx = NULL;
    }

    return ret;
}

int short_initial_cid_test()
{
    int ret = 0;
    for (uint8_t i = 4; ret == 0 && i < 18; i++) {
        ret = short_initial_cid_test_one(i);
    }

    return ret;
}

/*
 * Test whether the number of open streams is properly enforced
 */

int stream_id_max_test()
{
    picoquic_tp_t test_parameters;

    memset(&test_parameters, 0, sizeof(picoquic_tp_t));

    picoquic_init_transport_parameters(&test_parameters, 0);
    test_parameters.initial_max_stream_id_bidir = 4;

    return tls_api_one_scenario_test(test_scenario_many_streams, sizeof(test_scenario_many_streams), 0, 0, 0, 0, 0, 250000, NULL, &test_parameters);
}

/*
 * Test whether padding policy is correctly applied, and whether the corresponding
 * connection succeeds.
 */

int padding_test()
{
    uint64_t simulated_time = 0;
    picoquic_test_tls_api_ctx_t* test_ctx = NULL;
    int ret = tls_api_init_ctx(&test_ctx, PICOQUIC_INTERNAL_TEST_VERSION_1, PICOQUIC_TEST_SNI, PICOQUIC_TEST_ALPN, &simulated_time, NULL, NULL, 0, 1, 0);

    if (ret == 0 && test_ctx == NULL) {
        ret = -1;
    }

    /* Set the padding policy in the server context and in the client connection
     */
    if (ret == 0) {
        picoquic_set_default_padding(test_ctx->qserver, 128, 64);
        picoquic_cnx_set_padding_policy(test_ctx->cnx_client, 128, 64);

        /* Run a basic test scenario
         */

        ret = tls_api_one_scenario_body(test_ctx, &simulated_time,
            test_scenario_many_streams, sizeof(test_scenario_many_streams), 0, 0, 0, 0, 250000);
    }

    /* And then free the resource
     */

    if (test_ctx != NULL) {
        tls_api_delete_ctx(test_ctx);
        test_ctx = NULL;
    }

    return ret;
}

/*
 * Test whether the server correctly processes coalesced packets when one of the segments does not decrypt correctly 
 */

int bad_coalesce_test()
{
    uint64_t simulated_time = 0;
    picoquic_test_tls_api_ctx_t* test_ctx = NULL;
    int ret = tls_api_init_ctx(&test_ctx, PICOQUIC_INTERNAL_TEST_VERSION_1, PICOQUIC_TEST_SNI, PICOQUIC_TEST_ALPN, &simulated_time, NULL, NULL, 0, 1, 0);

    if (ret == 0 && test_ctx == NULL) {
        ret = -1;
    }

    /* Set the coalescing policy in the test context
     */
    if (ret == 0) {
        test_ctx->do_bad_coalesce_test = 1;

        /* Run a basic test scenario
         */

        ret = tls_api_one_scenario_body(test_ctx, &simulated_time,
            test_scenario_q_and_r, sizeof(test_scenario_q_and_r), 0, 0, 0, 0, 250000);
    }

    /* And then free the resource
     */

    if (test_ctx != NULL) {
        tls_api_delete_ctx(test_ctx);
        test_ctx = NULL;
    }

    return ret;
}



/*
 * Test whether packet tracing works correctly by setting up a basic connection
 * and verifying that the log file is what we expect.
 */
#ifdef _WINDOWS
#define PACKET_TRACE_TEST_REF "picoquictest\\packet_trace_ref.txt"
#else
#define PACKET_TRACE_TEST_REF "picoquictest/packet_trace_ref.txt"
#endif
#define PACKET_TRACE_CSV "packet_trace.csv"

int packet_trace_test()
{
    uint64_t simulated_time = 0;
    picoquic_test_tls_api_ctx_t* test_ctx = NULL;
    int ret = tls_api_init_ctx(&test_ctx, PICOQUIC_INTERNAL_TEST_VERSION_1, PICOQUIC_TEST_SNI, PICOQUIC_TEST_ALPN, &simulated_time, NULL, NULL, 0, 1, 0);
    char trace_file_name[512];

    if (ret == 0 && test_ctx == NULL) {
        ret = -1;
    }

    /* Set the logging policy on the server side, to store data in the
     * current working directory, and run a basic test scenario */
    if (ret == 0) {
        picoquic_set_cc_log(test_ctx->qserver, ".");
    
        ret = tls_api_one_scenario_body(test_ctx, &simulated_time,
            test_scenario_very_long, sizeof(test_scenario_very_long), 0, 0, 0, 20000, 1000000);
    }

    /* Derive the test file name from the ICID observed in the server connection,
     * then compare to expected result.
     */
    if (ret == 0) {
        if (test_ctx->cnx_server == NULL) {
            DBG_PRINTF("%s", "Cannot access the server connection\n");
            ret = -1;
        }
        else {
            for (size_t i = 0; ret == 0 && i < test_ctx->cnx_server->initial_cnxid.id_len; i++) {
                ret = picoquic_sprintf(&trace_file_name[2 * i], sizeof(trace_file_name) - 2 * i, NULL, "%02x", test_ctx->cnx_server->initial_cnxid.id[i]);
                if (ret != 0) {
                    DBG_PRINTF("Cannot format the file name, i=%d, icid len=%d\n", i, test_ctx->cnx_server->initial_cnxid.id_len);
                    ret = -1;
                }
            }
            if (ret == 0){
                memcpy(&trace_file_name[2 * test_ctx->cnx_server->initial_cnxid.id_len], "-log.bin", 9);
            }

        }
    }

    /* Free the resource, which will close the log file.
     */

    if (test_ctx != NULL) {
        tls_api_delete_ctx(test_ctx);
        test_ctx = NULL;
    }

    /* Create a CSV file from the .bin log file */
    if (ret == 0) {
        ret = picoquic_cc_log_file_to_csv(trace_file_name, PACKET_TRACE_CSV);
    }

    /* compare the log file to the expected value */
    if (ret == 0)
    {
        char packet_trace_test_ref[512];

        ret = picoquic_get_input_path(packet_trace_test_ref, sizeof(packet_trace_test_ref), picoquic_test_solution_dir, PACKET_TRACE_TEST_REF);

        if (ret != 0) {
            DBG_PRINTF("%s", "Cannot set the packet trace test ref file name.\n");
        }
        else {
            ret = picoquic_test_compare_files(PACKET_TRACE_CSV, packet_trace_test_ref);
        }
    }

    return ret;
}

/*
 * Testing the flow controlled sending scenario 
 */

int ready_to_send_test()
{
    int ret = 0;

    for (int i = 0; ret == 0 && i < 3; i++) {
        uint64_t simulated_time = 0;
        picoquic_test_tls_api_ctx_t* test_ctx = NULL;

        ret = tls_api_one_scenario_init(&test_ctx, &simulated_time,
            0, NULL, NULL);

        if (ret == 0) {
            test_ctx->stream0_test_option = i;
            ret = tls_api_one_scenario_body(test_ctx, &simulated_time,
                test_scenario_q_and_r, sizeof(test_scenario_q_and_r), 1000000, 0, 0, 20000,
                1200000);
        }

        if (test_ctx != NULL) {
            tls_api_delete_ctx(test_ctx);
            test_ctx = NULL;
        }

        if (ret != 0) {
            DBG_PRINTF("Ready to send variant %d fails\n", i);
        }
    }

    return ret;
}

int cubic_test()
{
    uint64_t simulated_time = 0;
    picoquic_test_tls_api_ctx_t* test_ctx = NULL;
    int ret = tls_api_init_ctx(&test_ctx, PICOQUIC_INTERNAL_TEST_VERSION_1, PICOQUIC_TEST_SNI, PICOQUIC_TEST_ALPN, &simulated_time, NULL, NULL, 0, 1, 0);

    if (ret == 0 && test_ctx == NULL) {
        ret = -1;
    }

    /* Set the congestion algorithm to cubic. Also, request a packet trace */
    if (ret == 0) {
        picoquic_set_default_congestion_algorithm(test_ctx->qserver, picoquic_cubic_algorithm);
        picoquic_set_congestion_algorithm(test_ctx->cnx_client, picoquic_cubic_algorithm);

        picoquic_set_cc_log(test_ctx->qserver, ".");

        ret = tls_api_one_scenario_body(test_ctx, &simulated_time,
            test_scenario_sustained, sizeof(test_scenario_sustained), 0, 0, 0, 20000, 3600000);
    }

    /* Free the resource, which will close the log file.
     */

    if (test_ctx != NULL) {
        tls_api_delete_ctx(test_ctx);
        test_ctx = NULL;
    }

    return ret;
}

/* Test that different CID length are properly supported */
int cid_length_test_one(uint8_t length)
{
    uint64_t simulated_time = 0;
    picoquic_test_tls_api_ctx_t* test_ctx = NULL;
    int ret = tls_api_init_ctx(&test_ctx, PICOQUIC_INTERNAL_TEST_VERSION_1, PICOQUIC_TEST_SNI, PICOQUIC_TEST_ALPN, &simulated_time, NULL, NULL, 0, 1, 0);

    if (ret == 0 && test_ctx == NULL) {
        ret = -1;
    }

    /* Set the CID length to cubic */
    if (ret == 0) {
        /* Delete the old connection */
        picoquic_delete_cnx(test_ctx->cnx_client);
        test_ctx->cnx_client = NULL;
        /* Change the default cnx_id length*/
        test_ctx->qclient->local_cnxid_length = length;
        /* re-create a client connection */
        test_ctx->cnx_client = picoquic_create_cnx(test_ctx->qclient,
            picoquic_null_connection_id, picoquic_null_connection_id,
            (struct sockaddr*)&test_ctx->server_addr, 0,
            PICOQUIC_INTERNAL_TEST_VERSION_1, PICOQUIC_TEST_SNI, PICOQUIC_TEST_ALPN, 1);
        if (test_ctx->cnx_client == NULL) {
            ret = -1;
        }
        else {
            ret = tls_api_one_scenario_body(test_ctx, &simulated_time,
                test_scenario_q_and_r, sizeof(test_scenario_q_and_r), 0, 0, 0, 20000, 100000);

            if (ret == 0 &&
                test_ctx->cnx_client->path[0]->local_cnxid.id_len != length) {
                ret = -1;
            }

            if (ret == 0 &&
                test_ctx->cnx_server->path[0]->remote_cnxid.id_len != length) {
                ret = -1;
            }
        }
    }

    if (test_ctx != NULL) {
        tls_api_delete_ctx(test_ctx);
        test_ctx = NULL;
    }

    return ret;
}

int cid_length_test()
{
    int ret = 0;
    const uint8_t tested_length[] = { 0, 4, 5, 6, 7, 9, 10, 11, 12, 13, 14, 15, 16, 17, 18 };

    for (size_t i = 0; i < sizeof(tested_length); i++) {
        ret = cid_length_test_one(tested_length[i]);
        if (ret != 0) {
            DBG_PRINTF("Test fails for cid_length = %d\n", tested_length[i]);
            break;
        }
    }

    return ret;
}

/* Testing transmission behavior over large RTT links
 */

int long_rtt_test()
{
    int ret = 0;
    uint64_t simulated_time = 0;
    uint64_t latency = 300000ull; /* assume that each direction is 300 ms, e.g. satellite link */
    picoquic_test_tls_api_ctx_t* test_ctx = NULL;

    ret = tls_api_one_scenario_init(&test_ctx, &simulated_time,
        0, NULL, NULL);

    if (ret == 0) {
        /* set the delay estimate, then launch the test */
        test_ctx->c_to_s_link->microsec_latency = latency;
        test_ctx->s_to_c_link->microsec_latency = latency;

        picoquic_set_cc_log(test_ctx->qserver, ".");

        /* The transmission delay cannot be less than 2.6 sec:
         * 3 handshakes at 1 RTT each = 1.8 sec, plus
         * 1MB over a 10Mbps link = 0.8 sec. We observe
         * 3.31 seconds instead, i.e. 1.51 sec for the
         * data transmission. This is due to the slow start
         * phase of the congestion control, which we accelerated
         * but could not completely fix. */
        ret = tls_api_one_scenario_body(test_ctx, &simulated_time,
            test_scenario_very_long, sizeof(test_scenario_very_long), 0, 0, 0, 2*latency,
            3400000);
    }

    if (test_ctx != NULL) {
        tls_api_delete_ctx(test_ctx);
        test_ctx = NULL;
    }

    return ret;
}

/*
 * Test the insertion of holes in the ACK sequence. We start a large
 * download, while setting the policy to insert a hole approximately
 * every 16 packets. We verify that the transfer completes. Then,
 * we repeat that test but inject optimistic acks, which should
 * break the connection.
 */
int optimistic_ack_test_one(int shall_spoof_ack)
{
    int ret = 0;
    uint64_t simulated_time = 0;
    int nb_holes = 0;
    picoquic_test_tls_api_ctx_t* test_ctx = NULL;

    ret = tls_api_one_scenario_init(&test_ctx, &simulated_time,
        0, NULL, NULL);

    if (ret == 0) {
        /* set the optimistic ack policy*/
        picoquic_set_optimistic_ack_policy(test_ctx->qserver, 29);
        /* add a log request for debugging */
        picoquic_set_cc_log(test_ctx->qserver, ".");

        /* Reset the uniform random test */
        picoquic_public_random_seed_64(RANDOM_PUBLIC_TEST_SEED, 1);

        ret = tls_api_one_scenario_body_connect(test_ctx, &simulated_time, 0,
            0, 0);

        if (ret != 0)
        {
            DBG_PRINTF("Connect scenario returns %d\n", ret);
        }
    }

    /* Prepare to send data */
    if (ret == 0) {
        test_ctx->stream0_target = 0;
        ret = test_api_init_send_recv_scenario(test_ctx, test_scenario_very_long, sizeof(test_scenario_very_long));

        if (ret != 0)
        {
            DBG_PRINTF("Init send receive scenario returns %d\n", ret);
        }
    }

    /* Perform a data sending loop */
    if (ret == 0) {
        int nb_trials = 0;
        int nb_inactive = 0;
        uint64_t hole_number = 0;

        test_ctx->c_to_s_link->loss_mask = NULL;
        test_ctx->s_to_c_link->loss_mask = NULL;

        while (ret == 0 && nb_trials < 64000 && nb_inactive < 1024 && TEST_CLIENT_READY && TEST_SERVER_READY) {
            int was_active = 0;

            nb_trials++;

            ret = tls_api_one_sim_round(test_ctx, &simulated_time, 0, &was_active);

            if (ret < 0) {
                DBG_PRINTF("Sim round number %d returns %d\n", nb_trials, ret);
                break;
            }
            else if (test_ctx->cnx_server->nb_retransmission_total > 0) {
                DBG_PRINTF("Unexpected retransmission at T=%d", (int)simulated_time);
                ret = -1;
                break;
            }

            if (was_active) {
                nb_inactive = 0;
            }
            else {
                nb_inactive++;
            }

            if (test_ctx->test_finished) {
                if (picoquic_is_cnx_backlog_empty(test_ctx->cnx_client) && picoquic_is_cnx_backlog_empty(test_ctx->cnx_server)) {
                    break;
                }
            }

            /* find whether there was a new hole inserted */
            if (test_ctx->cnx_server != NULL) {
                picoquic_packet_t * packet = test_ctx->cnx_server->pkt_ctx[picoquic_packet_context_application].retransmit_oldest;

                while (packet != NULL && packet->sequence_number > hole_number) {
                    if (packet->is_ack_trap) {
                        hole_number = packet->sequence_number;
                        if (shall_spoof_ack) {
                            ret = picoquic_record_pn_received(test_ctx->cnx_client, picoquic_packet_context_application,
                                hole_number, simulated_time);
                            if (ret != 0) {
                                DBG_PRINTF("Record pn hole %d number returns %d\n", (int)hole_number, ret);
                                break;
                            }
                        }
                        nb_holes++;
                        break;
                    }
                    packet = packet->previous_packet;
                }
            }
        }

        if (!test_ctx->test_finished) {
            DBG_PRINTF("Data loop exit after %d rounds, %d inactive\n", nb_trials, nb_inactive);
        }

        if (ret != 0)
        {
            DBG_PRINTF("Data sending loop returns %d\n", ret);
        }
        else if (test_ctx->cnx_server != NULL) {
            DBG_PRINTF("Complete after %d packets sent, %d r. by client, %d retransmits, %d spurious.\n",
                (int)(test_ctx->cnx_server->pkt_ctx[picoquic_packet_context_application].send_sequence - 1),
                (int)test_ctx->cnx_client->pkt_ctx[picoquic_packet_context_application].first_sack_item.end_of_sack_range,
                test_ctx->cnx_server->nb_retransmission_total,
                test_ctx->cnx_server->nb_spurious);
        }
    }

    if (ret == 0 && nb_holes == 0) {
        DBG_PRINTF("%s", "No holes inserted\n");
        ret = -1;
    }

    if (shall_spoof_ack) {
        if (ret == 0 && test_ctx->test_finished) {
            DBG_PRINTF("Despite %d holes and spoofs, the transfer completed\n", nb_holes);
            ret = -1;
        }
        else {
            ret = 0;
        }
    }
    else {
        if (ret == 0) {
            ret = tls_api_one_scenario_body_verify(test_ctx, &simulated_time, 0);
            if (ret != 0) {
                DBG_PRINTF("Scenario verification returns %d", ret);
            }
        }
    }

    if (test_ctx != NULL) {
        tls_api_delete_ctx(test_ctx);
        test_ctx = NULL;
    }

    return ret;
}

int optimistic_ack_test()
{
    int ret = optimistic_ack_test_one(1);

    return ret;
}

int optimistic_hole_test()
{
    int ret = optimistic_ack_test_one(0);

    return ret;
}

/*
 * test that local and remote addresses are properly documented during the
 * call setup process.
 */

typedef struct st_tls_api_address_are_documented_t {
    /* addresses returned by almost ready callback */
    int nb_almost_ready;
    int local_addr_almost_ready_len;
    int remote_addr_almost_ready_len;
    struct sockaddr_storage local_addr_almost_ready;
    struct sockaddr_storage remote_addr_almost_ready;
    /* addresses returned by ready callback */
    int nb_ready;
    int local_addr_ready_len;
    int remote_addr_ready_len;
    struct sockaddr_storage local_addr_ready;
    struct sockaddr_storage remote_addr_ready;
    /* Pointer to the underlying callback */
    picoquic_stream_data_cb_fn callback_fn;
    void * callback_ctx;
} tls_api_address_are_documented_t;

static int test_local_address_callback(picoquic_cnx_t* cnx,
    uint64_t stream_id, uint8_t* bytes, size_t length,
    picoquic_call_back_event_t fin_or_event, void* callback_ctx, void* v_stream_ctx)
{
    int ret = 0;
    int local_addr_len, remote_addr_len;
    struct sockaddr * local_addr;
    struct sockaddr * remote_addr;

    tls_api_address_are_documented_t* cb_ctx = (tls_api_address_are_documented_t*)callback_ctx;

    if (fin_or_event == picoquic_callback_almost_ready) {
        picoquic_get_peer_addr(cnx, &remote_addr, &remote_addr_len);
        picoquic_get_local_addr(cnx, &local_addr, &local_addr_len);
        cb_ctx->nb_almost_ready++;
        if (cb_ctx->nb_almost_ready == 1) {
            if (local_addr_len > 0 && local_addr != NULL) {
                cb_ctx->local_addr_almost_ready_len = picoquic_store_addr(&cb_ctx->local_addr_almost_ready,
                    local_addr);
            }
            if (remote_addr_len > 0 && remote_addr != NULL) {
                cb_ctx->remote_addr_almost_ready_len = picoquic_store_addr(&cb_ctx->remote_addr_almost_ready,
                    remote_addr);
            }
        }
    }
    else if (fin_or_event == picoquic_callback_ready) {
        picoquic_get_peer_addr(cnx, &remote_addr, &remote_addr_len);
        picoquic_get_local_addr(cnx, &local_addr, &local_addr_len);
        cb_ctx->nb_ready++;
        if (cb_ctx->nb_ready == 1) {
            if (local_addr_len > 0 && local_addr != NULL) {
                cb_ctx->local_addr_ready_len = picoquic_store_addr(&cb_ctx->local_addr_ready,
                    local_addr);
            }
            if (remote_addr_len > 0 && remote_addr != NULL) {
                cb_ctx->remote_addr_ready_len = picoquic_store_addr(&cb_ctx->remote_addr_ready,
                    remote_addr);
            }
        }
    };

    if (cb_ctx->callback_fn != NULL) {
        picoquic_stream_data_cb_fn new_fn;
        void * new_ctx;

        ret = (cb_ctx->callback_fn)(cnx, stream_id, bytes, length, fin_or_event, cb_ctx->callback_ctx, NULL);

        /* Check that the callbacks were not reset during the last call */
        new_fn = picoquic_get_callback_function(cnx);
        new_ctx = picoquic_get_callback_context(cnx);

        if (new_fn != test_local_address_callback || new_ctx != callback_ctx) {
            cb_ctx->callback_fn = new_fn;
            cb_ctx->callback_ctx = new_ctx;
            picoquic_set_callback(cnx, test_local_address_callback, callback_ctx);
        }
    }

    return ret;
}

int document_addresses_check(tls_api_address_are_documented_t * test_cb_ctx,
    struct sockaddr * local_addr_ref, struct sockaddr * remote_addr_ref)
{
    int ret = 0;

    if (ret == 0 && test_cb_ctx->nb_almost_ready != 1) {
        DBG_PRINTF("Expected 1 almost ready callback, got %d\n", test_cb_ctx->nb_ready);
        ret = -1;
    }

    if (ret == 0 && test_cb_ctx->local_addr_almost_ready_len == 0) {
        DBG_PRINTF("%s", "Expected almost ready local address, got length = 0\n");
        ret = -1;
    }

    if (ret == 0 && picoquic_compare_addr(local_addr_ref,
        (struct sockaddr*)&test_cb_ctx->local_addr_almost_ready) != 0) {
        DBG_PRINTF("%s", "Local address from almost ready callback does not match\n");
        ret = -1;
    }

    if (ret == 0 && test_cb_ctx->remote_addr_almost_ready_len == 0) {
        DBG_PRINTF("%s", "Expected almost ready remote address, got length = 0\n");
        ret = -1;
    }

    if (ret == 0 && picoquic_compare_addr(remote_addr_ref,
        (struct sockaddr*)&test_cb_ctx->remote_addr_almost_ready) != 0) {
        DBG_PRINTF("%s", "Local address from almost ready callback does not match\n");
        ret = -1;
    }

    if (ret == 0 && test_cb_ctx->nb_ready != 1) {
        DBG_PRINTF("Expected 1 ready callback, got %d\n", test_cb_ctx->nb_ready);
        ret = -1;
    }

    if (ret == 0 && test_cb_ctx->local_addr_ready_len == 0) {
        DBG_PRINTF("%s", "Expected ready local address, got length = 0\n");
        ret = -1;
    }

    if (ret == 0 && picoquic_compare_addr(local_addr_ref,
        (struct sockaddr*)&test_cb_ctx->local_addr_ready) != 0) {
        DBG_PRINTF("%s", "Local address from ready callback does not match\n");
        ret = -1;
    }

    if (ret == 0 && test_cb_ctx->remote_addr_ready_len == 0) {
        DBG_PRINTF("%s", "Expected ready remote address, got length = 0\n");
        ret = -1;
    }

    if (ret == 0 && picoquic_compare_addr(remote_addr_ref,
        (struct sockaddr*)&test_cb_ctx->remote_addr_ready) != 0) {
        DBG_PRINTF("%s", "Local address from almost ready callback does not match\n");
        ret = -1;
    }

    return ret;
}

int document_addresses_test()
{
    uint64_t simulated_time = 0; 
    tls_api_address_are_documented_t client_address_callback_ctx, server_address_callback_ctx;

    picoquic_test_tls_api_ctx_t* test_ctx = NULL;
    int ret = tls_api_init_ctx(&test_ctx, PICOQUIC_INTERNAL_TEST_VERSION_1, PICOQUIC_TEST_SNI, PICOQUIC_TEST_ALPN, &simulated_time, NULL, NULL, 0, 1, 0);

    if (ret == 0 && test_ctx == NULL) {
        ret = -1;
    }
    if (ret == 0) {
        /* Set the call backs to intercept the almost ready and ready transitions */
        memset(&client_address_callback_ctx, 0, sizeof(tls_api_address_are_documented_t));
        client_address_callback_ctx.callback_fn = picoquic_get_callback_function(test_ctx->cnx_client);
        client_address_callback_ctx.callback_ctx = picoquic_get_callback_context(test_ctx->cnx_client);
        picoquic_set_callback(test_ctx->cnx_client, test_local_address_callback, &client_address_callback_ctx);

        memset(&server_address_callback_ctx, 0, sizeof(tls_api_address_are_documented_t));
        server_address_callback_ctx.callback_fn = picoquic_get_default_callback_function(test_ctx->qserver);
        server_address_callback_ctx.callback_ctx = picoquic_get_default_callback_context(test_ctx->qserver);
        picoquic_set_default_callback(test_ctx->qserver, test_local_address_callback, &server_address_callback_ctx);

        ret = tls_api_one_scenario_body(test_ctx, &simulated_time,
            test_scenario_q_and_r, sizeof(test_scenario_q_and_r), 0, 0, 0, 20000, 3600000);
    }

    /* Verify that the addresses and calls are what we expect */
    if (ret == 0) {
        ret = document_addresses_check(&client_address_callback_ctx,
            (struct sockaddr*)&test_ctx->client_addr, (struct sockaddr*)&test_ctx->server_addr);
        if (ret != 0) {
            DBG_PRINTF("%s", "Client addresses were not properly documented\n");
        }
    }

    if (ret == 0) {
        ret = document_addresses_check(&server_address_callback_ctx,
            (struct sockaddr*)&test_ctx->server_addr, (struct sockaddr*)&test_ctx->client_addr);
        if (ret != 0) {
            DBG_PRINTF("%s", "Server addresses were not properly documented\n");
        }
    }

    /* Free the resource */

    if (test_ctx != NULL) {
        tls_api_delete_ctx(test_ctx);
        test_ctx = NULL;
    }

    return ret;
}

/*
 * Test whether a connection succeed when SNI is not specified.
 */

int null_sni_test()
{
    return tls_api_test_with_loss(NULL, PICOQUIC_INTERNAL_TEST_VERSION_1, NULL, PICOQUIC_TEST_ALPN);
}

/*
 * Test whether server redirection is applied properly
 */

int preferred_address_test()
{
    int ret;
    uint64_t simulated_time = 0;
    picoquic_test_tls_api_ctx_t* test_ctx = NULL;
    struct sockaddr_in server_preferred;
    picoquic_tp_t server_parameters;


    server_preferred.sin_family = AF_INET;
#ifdef _WINDOWS
    server_preferred.sin_addr.S_un.S_addr = 0x0A00000B;
#else
    server_preferred.sin_addr.s_addr = 0x0A00000B;
#endif
    server_preferred.sin_port = 5678;

    memset(&server_parameters, 0, sizeof(picoquic_tp_t));

    picoquic_init_transport_parameters(&server_parameters, 1);

    /* Create an alternate IP address, and use it as preferred address */
    server_parameters.prefered_address.is_defined = 1;
    memcpy(server_parameters.prefered_address.ipv4Address, &server_preferred.sin_addr, 4);
    server_parameters.prefered_address.ipv4Port = ntohs(server_preferred.sin_port);

    ret = tls_api_one_scenario_init(&test_ctx, &simulated_time, PICOQUIC_INTERNAL_TEST_VERSION_1,
        NULL, &server_parameters);


    if (ret == 0) {
        /* Run a basic test scenario */

        ret = tls_api_one_scenario_body(test_ctx, &simulated_time,
            test_scenario_very_long, sizeof(test_scenario_very_long), 0, 0, 0, 0, 1500000);
    }

    /* Verify that the client and server have both migrated to using the preferred address */
    if (ret == 0 && picoquic_compare_addr((struct sockaddr*)&server_preferred,
        (struct sockaddr*)&test_ctx->cnx_client->path[0]->peer_addr) != 0) {
        DBG_PRINTF("%s", "Server address at client not updated\n");
        ret = -1;
    }

    if (ret == 0 && test_ctx->cnx_server != NULL &&
        picoquic_compare_addr((struct sockaddr*)&server_preferred,
        (struct sockaddr*)&test_ctx->cnx_server->path[0]->local_addr) != 0) {
        DBG_PRINTF("%s", "Server address not promoted\n");
        ret = -1;
    }

    /* And then free the resource
     */

    if (test_ctx != NULL) {
        tls_api_delete_ctx(test_ctx);
        test_ctx = NULL;
    }

    return ret;
}


/* Test that the random public generation behaves in expected ways */
int random_public_tester_test()
{
#define RANDOM_PUBLIC_TEST_CONST 11
#define RANDOM_PUBLIC_TEST_ROUNDS 100
#define RANDOM_PUBLIC_CHI_SQUARE 18.31 /* Fail if significance of bias < P = 0.05 */
    int ret = 0;
    int r_count[RANDOM_PUBLIC_TEST_CONST];

    picoquic_public_random_seed_64(RANDOM_PUBLIC_TEST_SEED, 1);

    memset(r_count, 0, sizeof(r_count));

    for (int i = 0; i < RANDOM_PUBLIC_TEST_CONST*RANDOM_PUBLIC_TEST_ROUNDS; i++) {
        uint64_t x = picoquic_public_uniform_random(RANDOM_PUBLIC_TEST_CONST);

        if (x >= RANDOM_PUBLIC_TEST_CONST) {
            DBG_PRINTF("Value %d >= %d\n", x, RANDOM_PUBLIC_TEST_CONST);
            ret = -1;
            break;
        }
        else {
            r_count[x] += 1;
        }
    }

    if (ret == 0) {
        double chi_squared = 0;

        for (int i = 0; i < RANDOM_PUBLIC_TEST_CONST; i++) {
            double delta = ((double)RANDOM_PUBLIC_TEST_ROUNDS - r_count[i]);
            double d2 = delta * delta;
            d2 /= ((double)RANDOM_PUBLIC_TEST_ROUNDS);
            chi_squared += d2;
        }

        if (chi_squared > RANDOM_PUBLIC_CHI_SQUARE) {
            DBG_PRINTF("Chi2 = %f, larger than %f\n", chi_squared, RANDOM_PUBLIC_CHI_SQUARE);
            ret = -1;
        }
    }

    return ret;
}