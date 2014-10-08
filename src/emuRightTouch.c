/*
 * Copyright © 2011 Jesper Jensen
 *
 * Permission to use, copy, modify, distribute, and sell this software
 * and its documentation for any purpose is hereby granted without
 * fee, provided that the above copyright notice appear in all copies
 * and that both that copyright notice and this permission notice
 * appear in supporting documentation, and that the name of the authors
 * not be used in advertising or publicity pertaining to distribution of the
 * software without specific, written prior permission.  The authors make no
 * representations about the suitability of this software for any
 * purpose.  It is provided "as is" without express or implied
 * warranty.
 *
 * THE AUTHORS DISCLAIM ALL WARRANTIES WITH REGARD TO THIS SOFTWARE,
 * INCLUDING ALL IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS, IN
 * NO EVENT SHALL THE AUTHORS BE LIABLE FOR ANY SPECIAL, INDIRECT OR
 * CONSEQUENTIAL DAMAGES OR ANY DAMAGES WHATSOEVER RESULTING FROM LOSS
 * OF USE, DATA OR PROFITS, WHETHER IN AN ACTION OF CONTRACT,
 * NEGLIGENCE OR OTHER TORTIOUS ACTION, ARISING OUT OF OR IN
 * CONNECTION WITH THE USE OR PERFORMANCE OF THIS SOFTWARE.
 *
 */

/* Right mouse button emulation for touchscreens.
 */

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include "evdev.h"

#include <X11/Xatom.h>
#include <xf86.h>
#include <xf86Xinput.h>
#include <exevents.h>

#include <evdev-properties.h>

/* Threshold (in device coordinates) for devices to cancel emulation */
#define DEFAULT_MOVE_THRESHOLD 20

static Atom prop_rctemu;         /* Right button emulation on/off property   */
static Atom prop_rcttimeout;     /* Right button timeout property            */
static Atom prop_rctbutton;      /* Right button target physical button      */
static Atom prop_rctthreshold;   /* Right button move cancellation threshold */

/* State machine for 3rd button emulation */
enum EmulationState {
    EMRCT_OFF,             /* no event      */
    EMRCT_PENDING,         /* timer pending */
    EMRCT_EMULATING        /* in emulation  */
};

static void
EvdevRCTEmuPostButtonEvent(InputInfoPtr pInfo, int button, enum ButtonAction act)
{
    EvdevPtr          pEvdev   = pInfo->private;
    struct emulateRCT *emuRCT    = &pEvdev->emulateRCT;
    int               absolute = Relative;

    /* if we cancel, emit the button down event at our start position,
     * not at the current position. Only for absolute devices though. For
     * relative events, this may be a bit iffy since pointer accel may shoot
     * us back more than we moved and confuse the user.
     */
    if (emuRCT->flags & EVDEV_ABSOLUTE_EVENTS)
        absolute = Absolute;

    xf86PostButtonEventP(pInfo->dev, absolute, button,
                         (act == BUTTON_PRESS) ? 1 : 0, 0,
                         (absolute ? 2 : 0), emuRCT->startpos);
}


/**
 * Timer function. Post a button down event to the server.
 *
 * @param arg The InputInfoPtr for this device.
 */
CARD32
EvdevRCTEmuTimer(OsTimerPtr timer, CARD32 time, pointer arg)
{
    InputInfoPtr      pInfo    = (InputInfoPtr)arg;
    EvdevPtr          pEvdev   = pInfo->private;
    struct emulateRCT *emuRCT    = &pEvdev->emulateRCT;
    int               sigstate = 0;

    sigstate = xf86BlockSIGIO ();
    emuRCT->state = EMRCT_EMULATING;
    EvdevRCTEmuPostButtonEvent(pInfo, emuRCT->button, BUTTON_PRESS);
    xf86UnblockSIGIO (sigstate);
    return 0;
}


/**
 * Cancel all emulation, reset the timer and reset deltas.
 */
static void
EvdevRCTCancel(InputInfoPtr pInfo)
{
    EvdevPtr          pEvdev = pInfo->private;
    struct emulateRCT *emuRCT  = &pEvdev->emulateRCT;

    if (emuRCT->state != EMRCT_OFF)
    {
        TimerCancel(emuRCT->timer);
        emuRCT->state = EMRCT_OFF;
        memset(emuRCT->delta, 0, sizeof(emuRCT->delta));
    }

    emuRCT->flags = 0;
}

/**
 * Emulate a third button on button press. Note that emulation only triggers
 * on button 1.
 *
 * Return TRUE if event was swallowed by middle mouse button emulation,
 * FALSE otherwise.
 */
BOOL
EvdevRCTEmuFilterEvent(InputInfoPtr pInfo, int button, BOOL press)
{
    EvdevPtr          pEvdev = pInfo->private;
    struct emulateRCT *emuRCT  = &pEvdev->emulateRCT;
    int               ret    = FALSE;

    if (!emuRCT->enabled)
        goto out;

    /* Any other button pressed? Cancel timer */
    if (button != 1)
    {
        switch (emuRCT->state)
        {
            case EMRCT_PENDING:
                EvdevRCTEmuPostButtonEvent(pInfo, 1, BUTTON_PRESS);
                EvdevRCTCancel(pInfo);
                break;
            case EMRCT_EMULATING:
                /* We're emulating and now the user pressed a different
                 * button. Just release the emulating one, tell the user to
                 * not do that and get on with life */
                EvdevRCTEmuPostButtonEvent(pInfo, emuRCT->button, BUTTON_RELEASE);
                EvdevRCTCancel(pInfo);
                break;
            default:
                break;
        }
        goto out;
    }

    /* Release event → cancel, send press and release now. */
    if (!press)
    {
        switch(emuRCT->state)
        {
            case EMRCT_PENDING:
                EvdevRCTEmuPostButtonEvent(pInfo, 1, BUTTON_PRESS);
                EvdevRCTCancel(pInfo);
                break;
            case EMRCT_EMULATING:
                EvdevRCTEmuPostButtonEvent(pInfo, emuRCT->button, BUTTON_RELEASE);
                EvdevRCTCancel(pInfo);
                ret = TRUE;
                break;
            default:
                break;
        }

        goto out;
    }

    if (press && emuRCT->state == EMRCT_OFF)
    {
        emuRCT->state = EMRCT_PENDING;
        emuRCT->timer = TimerSet(emuRCT->timer, 0, emuRCT->timeout,
                                EvdevRCTEmuTimer, pInfo);
        ret = TRUE;
        goto out;
    }

out:
    return ret;
}

/**
 * Handle absolute x/y motion. If the motion is above the threshold, cancel
 * emulation.
 */
void
EvdevRCTEmuProcessAbsMotion(InputInfoPtr pInfo, ValuatorMask *vals)
{
    EvdevPtr          pEvdev = pInfo->private;
    struct emulateRCT *emuRCT  = &pEvdev->emulateRCT;
    int               cancel = FALSE;
    int               axis   = 0;

    if (emuRCT->state != EMRCT_PENDING)
    {
        if (valuator_mask_isset(vals, 0))
            emuRCT->startpos[0] = valuator_mask_get(vals, 0);
        if (valuator_mask_isset(vals, 1))
            emuRCT->startpos[1] = valuator_mask_get(vals, 1);

        return;
    }

    if ((emuRCT->flags & EVDEV_ABSOLUTE_EVENTS) == 0)
        emuRCT->flags |= EVDEV_ABSOLUTE_EVENTS;

    while (axis <= 1 && !cancel)
    {
        if (valuator_mask_isset(vals, axis))
        {
            int delta = valuator_mask_get(vals, axis) - emuRCT->startpos[axis];
            if (abs(delta) > emuRCT->threshold)
                cancel = TRUE;
        }
        axis++;
    }

    if (cancel)
    {
        EvdevRCTEmuPostButtonEvent(pInfo, 1, BUTTON_PRESS);
        EvdevRCTCancel(pInfo);
    }
}

/**
 * Handle relative x/y motion. If the motion is above the threshold, cancel
 * emulation.
 */
void
EvdevRCTEmuProcessRelMotion(InputInfoPtr pInfo, int dx, int dy)
{
    EvdevPtr          pEvdev = pInfo->private;
    struct emulateRCT *emuRCT  = &pEvdev->emulateRCT;

    if (emuRCT->state != EMRCT_PENDING)
        return;

    emuRCT->delta[0] += dx;
    emuRCT->delta[1] += dy;
    emuRCT->flags |= EVDEV_RELATIVE_EVENTS;

    if (abs(emuRCT->delta[0]) > emuRCT->threshold ||
        abs(emuRCT->delta[1]) > emuRCT->threshold)
    {
        EvdevRCTEmuPostButtonEvent(pInfo, 1, BUTTON_PRESS);
        EvdevRCTCancel(pInfo);
    }
}

void
EvdevRCTEmuPreInit(InputInfoPtr pInfo)
{
    EvdevPtr          pEvdev = pInfo->private;
    struct emulateRCT *emuRCT  = &pEvdev->emulateRCT;

    emuRCT->enabled = xf86SetBoolOption(pInfo->options,
                                       "EmulateThirdButton",
                                       FALSE);
    emuRCT->timeout = xf86SetIntOption(pInfo->options,
                                      "EmulateThirdButtonTimeout",
                                      1000);
    emuRCT->button = xf86SetIntOption(pInfo->options,
                                      "EmulateThirdButtonButton",
                                      3);
    /* FIXME: this should be auto-configured based on axis ranges */
    emuRCT->threshold = xf86SetIntOption(pInfo->options,
                                         "EmulateThirdButtonMoveThreshold",
                                         DEFAULT_MOVE_THRESHOLD);
    /* allocate now so we don't allocate in the signal handler */
    emuRCT->timer = TimerSet(NULL, 0, 0, NULL, NULL);
}

void
EvdevRCTEmuOn(InputInfoPtr pInfo)
{
    /* This function just exists for symmetry in evdev.c */
}

void
EvdevRCTEmuFinalize(InputInfoPtr pInfo)
{
    EvdevPtr          pEvdev = pInfo->private;
    struct emulateRCT *emuRCT  = &pEvdev->emulateRCT;

    TimerFree(emuRCT->timer);
    emuRCT->timer = NULL;
}

static int
EvdevRCTEmuSetProperty(DeviceIntPtr dev, Atom atom, XIPropertyValuePtr val,
                      BOOL checkonly)
{
    InputInfoPtr      pInfo  = dev->public.devicePrivate;
    EvdevPtr          pEvdev = pInfo->private;
    struct emulateRCT *emuRCT  = &pEvdev->emulateRCT;

    if (atom == prop_rctemu)
    {
        if (val->format != 8 || val->size != 1 || val->type != XA_INTEGER)
            return BadMatch;

        if (!checkonly)
            emuRCT->enabled = *((BOOL*)val->data);

    } else if (atom == prop_rcttimeout)
    {
        if (val->format != 32 || val->size != 1 || val->type != XA_INTEGER)
            return BadMatch;

        if (!checkonly)
            emuRCT->timeout = *((CARD32*)val->data);

    } else if (atom == prop_rctbutton)
    {
        if (val->format != 8 || val->size != 1 || val->type != XA_INTEGER)
            return BadMatch;

        if (!checkonly)
            emuRCT->button = *((CARD8*)val->data);
    } else if (atom == prop_rctthreshold)
    {
        if (val->format != 32 || val->size != 1 || val->type != XA_INTEGER)
            return BadMatch;

        if (!checkonly)
            emuRCT->threshold = *((CARD32*)val->data);
    }


    return Success;
}

/**
 * Initialise properties for third button emulation
 */
void
EvdevRCTEmuInitProperty(DeviceIntPtr dev)
{
    InputInfoPtr      pInfo  = dev->public.devicePrivate;
    EvdevPtr          pEvdev = pInfo->private;
    struct emulateRCT *emuRCT  = &pEvdev->emulateRCT;
    int               rc;

    if (!dev->button) /* don't init prop for keyboards */
        return;

    /* third button emulation on/off */
    prop_rctemu = MakeAtom(EVDEV_PROP_THIRDBUTTON, strlen(EVDEV_PROP_THIRDBUTTON), TRUE);
    rc = XIChangeDeviceProperty(dev, prop_rctemu, XA_INTEGER, 8,
                                PropModeReplace, 1,
                                &emuRCT->enabled,
                                FALSE);
    if (rc != Success)
        return;

    XISetDevicePropertyDeletable(dev, prop_rctemu, FALSE);

    /* third button emulation timeout */
    prop_rcttimeout = MakeAtom(EVDEV_PROP_THIRDBUTTON_TIMEOUT,
                              strlen(EVDEV_PROP_THIRDBUTTON_TIMEOUT),
                              TRUE);
    rc = XIChangeDeviceProperty(dev, prop_rcttimeout, XA_INTEGER, 32, PropModeReplace, 1,
                                &emuRCT->timeout, FALSE);

    if (rc != Success)
        return;

    XISetDevicePropertyDeletable(dev, prop_rcttimeout, FALSE);

    /* third button emulation button to be triggered  */
    prop_rctbutton = MakeAtom(EVDEV_PROP_THIRDBUTTON_BUTTON,
                             strlen(EVDEV_PROP_THIRDBUTTON_BUTTON),
                             TRUE);
    rc = XIChangeDeviceProperty(dev, prop_rctbutton, XA_INTEGER, 8, PropModeReplace, 1,
                                &emuRCT->button, FALSE);

    if (rc != Success)
        return;

    XISetDevicePropertyDeletable(dev, prop_rctbutton, FALSE);

    /* third button emulation movement threshold */
    prop_rctthreshold = MakeAtom(EVDEV_PROP_THIRDBUTTON_THRESHOLD,
                                strlen(EVDEV_PROP_THIRDBUTTON_THRESHOLD),
                                TRUE);
    rc = XIChangeDeviceProperty(dev, prop_rctthreshold, XA_INTEGER, 32, PropModeReplace, 1,
                                &emuRCT->threshold, FALSE);

    if (rc != Success)
        return;

    XISetDevicePropertyDeletable(dev, prop_rctthreshold, FALSE);

    XIRegisterPropertyHandler(dev, EvdevRCTEmuSetProperty, NULL, NULL);
}
