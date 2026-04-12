/*
 * Copyright (c) 2025 Æther Authors
 *
 * SPDX-License-Identifier: MIT
 */

#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/net/heymac.h>

LOG_MODULE_REGISTER(heymac_hsm, CONFIG_NET_L2_HEYMAC_LOG_LEVEL);

/* Forward declarations */
void heymac_beacon_work_handler(struct k_work *work);
void heymac_timeout_work_handler(struct k_work *work);

/* State transition handlers */
static void heymac_enter_initializing(struct heymac_context *ctx)
{
    LOG_DBG("Entering INITIALIZING state");
    ctx->state = HEYMAC_STATE_INITIALIZING;
    
    /* Initialize beacon and timeout timers with valid handlers */
    k_work_init_delayable(&ctx->beacon_work, heymac_beacon_work_handler);
    k_work_init_delayable(&ctx->timeout_work, heymac_timeout_work_handler);
    
    /* Transition to lurking */
    heymac_state_machine_run(ctx, HEYMAC_EVENT_INIT_COMPLETE);
}

static void heymac_enter_lurking(struct heymac_context *ctx)
{
    LOG_DBG("Entering LURKING state");
    ctx->state = HEYMAC_STATE_LURKING;
    
    /* Enable receiver */
    /* TODO: Enable RX on radio */
    
    /* Start beacon timer with longer interval */
    k_work_schedule(&ctx->beacon_work, K_MSEC(CONFIG_HEYMAC_BEACON_INTERVAL_MS * 4));
}

static void heymac_enter_beaconing(struct heymac_context *ctx)
{
    LOG_DBG("Entering BEACONING state");
    ctx->state = HEYMAC_STATE_BEACONING;
    
    /* Use normal beacon interval */
    k_work_reschedule(&ctx->beacon_work, K_MSEC(CONFIG_HEYMAC_BEACON_INTERVAL_MS));
    
    /* Start neighbor update timer */
    k_work_schedule(&ctx->timeout_work, K_SECONDS(60));
}

static void heymac_enter_linking(struct heymac_context *ctx)
{
    LOG_DBG("Entering LINKING state");
    ctx->state = HEYMAC_STATE_LINKING;
    
    /* Continue beaconing at normal rate */
    /* Neighbors can now hear us */
}

/* Main state machine */
void heymac_state_machine_run(struct heymac_context *ctx, enum heymac_event event)
{
    LOG_DBG("HSM: state=%d, event=%d", ctx->state, event);
    
    switch (ctx->state) {
    case HEYMAC_STATE_INITIAL:
        if (event == HEYMAC_EVENT_INIT_COMPLETE) {
            heymac_enter_initializing(ctx);
        }
        break;
        
    case HEYMAC_STATE_INITIALIZING:
        if (event == HEYMAC_EVENT_INIT_COMPLETE) {
            heymac_enter_lurking(ctx);
        }
        break;
        
    case HEYMAC_STATE_LURKING:
        switch (event) {
        case HEYMAC_EVENT_BEACON_TIMEOUT:
            /* Send beacon even while lurking */
            heymac_send_beacon(ctx);
            break;
        case HEYMAC_EVENT_NEIGHBOR_FOUND:
            /* Transition to beaconing when we find neighbors */
            heymac_enter_beaconing(ctx);
            break;
        case HEYMAC_EVENT_FRAME_RECEIVED:
            /* Process received frame */
            break;
        }
        break;
        
    case HEYMAC_STATE_BEACONING:
        switch (event) {
        case HEYMAC_EVENT_BEACON_TIMEOUT:
            heymac_send_beacon(ctx);
            break;
        case HEYMAC_EVENT_NEIGHBOR_LOST:
            /* Can go back to lurking */
            heymac_enter_lurking(ctx);
            break;
        case HEYMAC_EVENT_FRAME_RECEIVED:
            /* Process received frame */
            break;
        default:
            break;
        }
        break;
        
    case HEYMAC_STATE_LINKING:
        switch (event) {
        case HEYMAC_EVENT_BEACON_TIMEOUT:
            heymac_send_beacon(ctx);
            break;
        case HEYMAC_EVENT_NEIGHBOR_LOST:
            /* Go back to beaconing */
            heymac_enter_beaconing(ctx);
            break;
        case HEYMAC_EVENT_FRAME_RECEIVED:
            /* Process received frame */
            break;
        default:
            break;
        }
        break;
    
    default:
        LOG_WRN("Unhandled state: %d", ctx->state);
        break;
    }
}

/* Timer callbacks */
void heymac_beacon_work_handler(struct k_work *work)
{
    struct k_work_delayable *dwork = k_work_delayable_from_work(work);
    struct heymac_context *ctx = CONTAINER_OF(dwork, struct heymac_context, beacon_work);
    
    heymac_state_machine_run(ctx, HEYMAC_EVENT_BEACON_TIMEOUT);
    
    /* Reschedule beacon timer */
    k_work_reschedule(&ctx->beacon_work, K_MSEC(CONFIG_HEYMAC_BEACON_INTERVAL_MS));
}

void heymac_timeout_work_handler(struct k_work *work)
{
    struct k_work_delayable *dwork = k_work_delayable_from_work(work);
    struct heymac_context *ctx = CONTAINER_OF(dwork, struct heymac_context, timeout_work);
    
    /* General timeout handler - can be used for various timeouts */
    LOG_DBG("HeyMac timeout handler called");
    
    /* Reschedule timeout timer for next update */
    k_work_reschedule(&ctx->timeout_work, K_SECONDS(60));
}