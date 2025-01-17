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

/*
 * Really basic network simulator, only simulates a simple link using a
 * packet structure. 
 * Init: link creation. Returns a link structure with defined bandwidth,
 * latency, loss pattern and initial time. The link is empty. The loss
 * pattern is a 64 bit bit mask.
 * Submit packet of length L at time t. The packet is queued to the link.
 * Get packet out of link at time T + L + Queue.
 */

#include "picoquic_internal.h"
#include "picoquictest_internal.h"
#include <stdlib.h>
#include <string.h>

picoquictest_sim_link_t* picoquictest_sim_link_create(double data_rate_in_gps,
    uint64_t microsec_latency, uint64_t* loss_mask, uint64_t queue_delay_max, uint64_t current_time)
{
    picoquictest_sim_link_t* link = (picoquictest_sim_link_t*)malloc(sizeof(picoquictest_sim_link_t));
    if (link != 0) {
        double pico_d = (data_rate_in_gps <= 0) ? 0 : (8000.0 / data_rate_in_gps);
        pico_d *= (1.024 * 1.024); /* account for binary units */
        link->next_send_time = current_time;
        link->queue_time = current_time;
        link->queue_delay_max = queue_delay_max;
        link->picosec_per_byte = (uint64_t)pico_d; 
        link->microsec_latency = microsec_latency;
        link->packets_dropped = 0;
        link->packets_sent = 0;
        link->first_packet = NULL;
        link->last_packet = NULL;
        link->loss_mask = loss_mask;
    }

    return link;
}

void picoquictest_sim_link_delete(picoquictest_sim_link_t* link)
{
    picoquictest_sim_packet_t* packet;

    while ((packet = link->first_packet) != NULL) {
        link->first_packet = packet->next_packet;
        free(packet);
    }

    free(link);
}

picoquictest_sim_packet_t* picoquictest_sim_link_create_packet()
{
    picoquictest_sim_packet_t* packet = (picoquictest_sim_packet_t*)malloc(sizeof(picoquictest_sim_packet_t));
    if (packet != NULL) {
        packet->next_packet = NULL;
        packet->arrival_time = 0;
        packet->length = 0;
    }

    return packet;
}

uint64_t picoquictest_sim_link_next_arrival(picoquictest_sim_link_t* link, uint64_t current_time)
{
    picoquictest_sim_packet_t* packet = link->first_packet;

    if (packet != NULL && packet->arrival_time < current_time) {
        current_time = packet->arrival_time;
    }

    return current_time;
}

picoquictest_sim_packet_t* picoquictest_sim_link_dequeue(picoquictest_sim_link_t* link,
    uint64_t current_time)
{
    picoquictest_sim_packet_t* packet = link->first_packet;

    if (packet != NULL && packet->arrival_time <= current_time) {
        link->first_packet = packet->next_packet;
        if (link->first_packet == NULL) {
            link->last_packet = NULL;
        }
    } else {
        packet = NULL;
    }

    return packet;
}

static int picoquictest_sim_link_testloss(uint64_t* loss_mask)
{
    uint64_t loss_bit = 0;

    if (loss_mask != NULL) {
        /* Last bit indicates loss or not */
        loss_bit = (uint64_t)((*loss_mask) & 1ull);

        /* Rotate loss mask by 1 to prepare next round */
        *loss_mask >>= 1;
        *loss_mask |= (loss_bit << 63);
    }

    return (int)loss_bit;
}

void picoquictest_sim_link_submit(picoquictest_sim_link_t* link, picoquictest_sim_packet_t* packet,
    uint64_t current_time)
{
    uint64_t queue_delay = (current_time > link->queue_time) ? 0 : link->queue_time - current_time;
    uint64_t transmit_time = ((link->picosec_per_byte * ((uint64_t)packet->length)) >> 20);
    if (transmit_time <= 0)
        transmit_time = 1;

    if (link->queue_delay_max == 0 || queue_delay < link->queue_delay_max) {

        link->queue_time = current_time + queue_delay + transmit_time;

        if (picoquictest_sim_link_testloss(link->loss_mask) != 0) {
            link->packets_dropped++;
            free(packet);
        } else {
            link->packets_sent++;
            if (link->last_packet == NULL) {
                link->first_packet = packet;
            } else {
                link->last_packet->next_packet = packet;
            }
            link->last_packet = packet;
            packet->next_packet = NULL;
            packet->arrival_time = link->queue_time + link->microsec_latency;
        }
    } else {
        /* simulate congestion loss on queue full */
        link->packets_dropped++;
        free(packet);
    }
}

int sim_link_one_test(uint64_t* loss_mask, uint64_t queue_delay_max, uint64_t nb_losses)
{
    int ret = 0;
    uint64_t current_time = 0;
    uint64_t departure_time = 0;
    picoquictest_sim_link_t* link = picoquictest_sim_link_create(0.01, 10000, loss_mask, queue_delay_max, current_time);
    uint64_t dequeued = 0;
    uint64_t queued = 0;
    const uint64_t nb_packets = 16;

    if (link == NULL) {
        ret = -1;
    }
    else {

        while (ret == 0) {
            if (queued >= nb_packets) {
                departure_time = (uint64_t)((int64_t)-1);
            }

            current_time = picoquictest_sim_link_next_arrival(link, departure_time);

            picoquictest_sim_packet_t* packet = picoquictest_sim_link_dequeue(link, current_time);

            if (packet != NULL) {
                dequeued++;
                free(packet);
            }
            else if (queued < nb_packets) {
                packet = picoquictest_sim_link_create_packet();

                if (packet == NULL) {
                    ret = -1;
                }
                else {
                    packet->length = sizeof(packet->bytes);
                    picoquictest_sim_link_submit(link, packet, departure_time);
                    departure_time += 250;
                    queued++;
                }
            }
            else {
                break;
            }
        }

        if ((dequeued + nb_losses) != nb_packets) {
            ret = -1;
        }
        
        picoquictest_sim_link_delete(link);
    }

    return ret;
}

int sim_link_test()
{
    int ret = 0;
    uint64_t loss_mask = 0;
    
    ret = sim_link_one_test(&loss_mask, 0, 0);

    if (ret == 0) {
        loss_mask = 8;
        ret = sim_link_one_test(&loss_mask, 0, 1);
    }

    if (ret == 0) {
        loss_mask = 0x18;
        ret = sim_link_one_test(&loss_mask, 0, 2);
    }

    return ret;
}

void picoquic_set_test_address(struct sockaddr_in * addr, uint32_t addr_val, uint16_t port)
{
    /* Init of the IP addresses */
    memset(addr, 0, sizeof(struct sockaddr_in));
    addr->sin_family = AF_INET;
#ifdef _WINDOWS
    addr->sin_addr.S_un.S_addr = addr_val;
#else
    addr->sin_addr.s_addr = addr_val;
#endif
    addr->sin_port = port;
}