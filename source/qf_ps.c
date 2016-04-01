/**
* @file
* @brief Publish-Subscribe services
* @ingroup qf
* @cond
******************************************************************************
* Last updated for version 5.6.2
* Last updated on  2016-03-31
*
*                    Q u a n t u m     L e a P s
*                    ---------------------------
*                    innovating embedded systems
*
* Copyright (C) Quantum Leaps, LLC. All rights reserved.
*
* This program is open source software: you can redistribute it and/or
* modify it under the terms of the GNU General Public License as published
* by the Free Software Foundation, either version 3 of the License, or
* (at your option) any later version.
*
* Alternatively, this program may be distributed and modified under the
* terms of Quantum Leaps commercial licenses, which expressly supersede
* the GNU General Public License and are specifically designed for
* licensees interested in retaining the proprietary status of their code.
*
* This program is distributed in the hope that it will be useful,
* but WITHOUT ANY WARRANTY; without even the implied warranty of
* MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
* GNU General Public License for more details.
*
* You should have received a copy of the GNU General Public License
* along with this program. If not, see <http://www.gnu.org/licenses/>.
*
* Contact information:
* http://www.state-machine.com
* mailto:info@state-machine.com
******************************************************************************
* @endcond
*/
#define QP_IMPL           /* this is QP implementation */
#include "qf_port.h"      /* QF port */
#include "qf_pkg.h"       /* QF package-scope interface */
#include "qassert.h"      /* QP embedded systems-friendly assertions */
#ifdef Q_SPY              /* QS software tracing enabled? */
    #include "qs_port.h"  /* include QS port */
#else
    #include "qs_dummy.h" /* disable the QS software tracing */
#endif /* Q_SPY */

Q_DEFINE_THIS_MODULE("qf_ps")


/* Package-scope objects ****************************************************/
QSubscrList *QF_subscrList_;
enum_t QF_maxSignal_;

/****************************************************************************/
/**
* @description
* This function initializes the publish-subscribe facilities of QF and must
* be called exactly once before any subscriptions/publications occur in
* the application.
*
* @param[in] subscrSto pointer to the array of subscriber lists
* @param[in] maxSignal the dimension of the subscriber array and at
*                      the same time the maximum signal that can be published
*                      or subscribed.
*
* The array of subscriber-lists is indexed by signals and provides a mapping
* between the signals and subscriber-lists. The subscriber-lists are bitmasks
* of type ::QSubscrList, each bit in the bit mask corresponding to the unique
* priority of an active object. The size of the ::QSubscrList bit mask
* depends on the value of the #QF_MAX_ACTIVE macro.
*
* @note The publish-subscribe facilities are optional, meaning that you
* might choose not to use publish-subscribe. In that case calling QF_psInit()
* and using up memory for the subscriber-lists is unnecessary.
*
* @sa ::QSubscrList
*
* @usage
* The following example shows the typical initialization sequence of QF:
* @include qf_main.c
*/
void QF_psInit(QSubscrList * const subscrSto, enum_t const maxSignal) {
    QF_subscrList_ = subscrSto;
    QF_maxSignal_  = maxSignal;

    /* zero the subscriber list, so that the framework can start correctly
    * even if the startup code fails to clear the uninitialized data
    * (as is required by the C Standard).
    */
    QF_bzero(subscrSto,
             (uint_fast16_t)((uint_fast16_t)maxSignal
                           * (uint_fast16_t)sizeof(QSubscrList)));
}

/****************************************************************************/
/**
* @description
* This function posts (using the FIFO policy) the event @a e to **all**
* active objects that have subscribed to the signal @a e->sig, which is
* called _multicasting_. The multicasting performed in this function is
* very efficient based on reference-counting inside the published event
* ("zero-copy" event multicasting). This function is designed to be
* callable from any part of the system, including ISRs, device drivers,
* and active objects.
*
* @note
* To avoid any unexpected re-ordering of events posted into AO queues,
* the event multicasting is performed with scheduler __locked__. However,
* the scheduler is locked only up to the priority level of the highest-
* priority subscriber, so any AOs of even higher priority, which did not
* subscribe to this event are _not_ affected.
*
* @attention this function should be called only via the macro QF_PUBLISH()
*/
#ifndef Q_SPY
void QF_publish_(QEvt const * const e)
#else
void QF_publish_(QEvt const * const e, void const * const sender)
#endif
{
    QF_SCHED_STAT_TYPE_ lockStat;
    QF_CRIT_STAT_

    /** @pre the published signal must be within the configured range */
    Q_REQUIRE_ID(200, e->sig < (QSignal)QF_maxSignal_);

    QF_CRIT_ENTRY_();

    QS_BEGIN_NOCRIT_(QS_QF_PUBLISH, (void *)0, (void *)0)
        QS_TIME_();          /* the timestamp */
        QS_OBJ_(sender);     /* the sender object */
        QS_SIG_(e->sig);     /* the signal of the event */
        QS_2U8_(e->poolId_, e->refCtr_);/* pool Id & ref Count of the event */
    QS_END_NOCRIT_()

    /* is it a dynamic event? */
    if (e->poolId_ != (uint8_t)0) {
        QF_EVT_REF_CTR_INC_(e); /* increment reference counter, NOTE01 */
    }
    QF_CRIT_EXIT_();

    lockStat.lockPrio = (uint_fast8_t)0xFF; /* set as uninitialized */

#if (QF_MAX_ACTIVE <= 8)
    {
        uint_fast8_t tmp = QF_subscrList_[e->sig].bits[0];

        while (tmp != (uint_fast8_t)0) {
            /* find the most-significant bit number */
            uint_fast8_t p = (uint_fast8_t)QF_LOG2(tmp);

            /* remove the most-significant bit from the bitmask */
            tmp &= (uint_fast8_t)QF_invPwr2Lkup[p];

            /* has the scheduler been locked yet? */
            if (lockStat.lockPrio == (uint_fast8_t)0xFF) {
                QF_SCHED_LOCK_(&lockStat, p);
            }

            /* the prio of the AO must be registered with the framework */
            Q_ASSERT_ID(210, QF_active_[p] != (QMActive *)0);

            /* QACTIVE_POST() asserts internally if the queue overflows */
            QACTIVE_POST(QF_active_[p], e, sender);
        }
    }
#else /* (QF_MAX_ACTIVE > 8) */
    {
        uint_fast8_t i = (uint_fast8_t)Q_DIM(QF_subscrList_[0].bits);

        /* go through all bytes in the subscription list */
        do {
            uint_fast8_t tmp;
            --i;
            tmp = (uint_fast8_t)QF_PTR_AT_(QF_subscrList_, e->sig).bits[i];

            while (tmp != (uint_fast8_t)0) {
                /* find the most-significant bit number */
                uint_fast8_t p = (uint_fast8_t)QF_LOG2(tmp);

                /* remove the most-significant bit from the bitmask */
                tmp &= (uint_fast8_t)QF_invPwr2Lkup[p];

                /* convert bit number to the highest-priority subscriber */
                p += (uint_fast8_t)(i << 3);

                /* has the scheduler been locked yet? */
                if (lockStat.lockPrio == (uint_fast8_t)0xFF) {
                    QF_SCHED_LOCK_(&lockStat, p);
                }

                /* the prio of the AO must be registered with the framework */
                Q_ASSERT_ID(220, QF_active_[p] != (QMActive *)0);

                /* QACTIVE_POST() asserts internally if the queue overflows */
                QACTIVE_POST(QF_active_[p], e, sender);
            }
        } while (i != (uint_fast8_t)0);
    }
#endif /* (QF_MAX_ACTIVE > 8) */

    /* was the scheduler locked? */
    if (lockStat.lockPrio <= (uint_fast8_t)QF_MAX_ACTIVE) {
        QF_SCHED_UNLOCK_(&lockStat); /* unlock the scheduler */
    }

    /* run the garbage collector */
    QF_gc(e);

    /* NOTE: QF_publish_() increments the reference counter to prevent
    * premature recycling of the event while the multicasting is still
    * in progress. At the end of the function, the garbage collector step
    * decrements the reference counter and recycles the event if the
    * counter drops to zero. This covers the case when the event was
    * published without any subscribers.
    */
}

/****************************************************************************/
/**
* @description
* This function is part of the Publish-Subscribe event delivery mechanism
* available in QF. Subscribing to an event means that the framework will
* start posting all published events with a given signal @p sig to the
* event queue of the active object @p me.
*
* @param[in,out] me  pointer (see @ref oop)
* @param[in]     sig event signal to subscribe
*
* @usage
* The following example shows how the Table active object subscribes
* to three signals in the initial transition:
* @include qf_subscribe.c
*
* @sa QF_publish_(), QActive_unsubscribe(), and QActive_unsubscribeAll()
*/
void QActive_subscribe(QActive const * const me, enum_t const sig) {
    uint_fast8_t p = me->prio;
    uint_fast8_t i = (uint_fast8_t)QF_div8Lkup[p];
    QF_CRIT_STAT_

    Q_REQUIRE_ID(300, ((enum_t)Q_USER_SIG <= sig)
              && (sig < QF_maxSignal_)
              && ((uint_fast8_t)0 < p) && (p <= (uint_fast8_t)QF_MAX_ACTIVE)
              && (QF_active_[p] == me));

    QF_CRIT_ENTRY_();

    QS_BEGIN_NOCRIT_(QS_QF_ACTIVE_SUBSCRIBE, QS_priv_.aoObjFilter, me)
        QS_TIME_();             /* timestamp */
        QS_SIG_((QSignal)sig);  /* the signal of this event */
        QS_OBJ_(me);            /* this active object */
    QS_END_NOCRIT_()

    /* set the priority bit */
    QF_PTR_AT_(QF_subscrList_, sig).bits[i] |= QF_pwr2Lkup[p];
    QF_CRIT_EXIT_();
}

/****************************************************************************/
/**
* @description
* This function is part of the Publish-Subscribe event delivery mechanism
* available in QF. Un-subscribing from an event means that the framework
* will stop posting published events with a given signal @p sig to the
* event queue of the active object @p me.
*
* @param[in] me  pointer (see @ref oop)
* @param[in] sig event signal to unsubscribe
*
* @note Due to the latency of event queues, an active object should NOT
* assume that a given signal @p sig will never be dispatched to the
* state machine of the active object after un-subscribing from that signal.
* The event might be already in the queue, or just about to be posted
* and the un-subscribe operation will not flush such events.
*
* @note Un-subscribing from a signal that has never been subscribed in the
* first place is considered an error and QF will raise an assertion.
*
* @sa QF_publish_(), QActive_subscribe(), and QActive_unsubscribeAll()
*/
void QActive_unsubscribe(QActive const * const me, enum_t const sig) {
    uint_fast8_t p = me->prio;
    uint_fast8_t i = (uint_fast8_t)QF_div8Lkup[p];
    QF_CRIT_STAT_

    /** @pre the singal and the prioriy must be in ragne, the AO must also
    * be registered with the framework
    */
    Q_REQUIRE_ID(400, ((enum_t)Q_USER_SIG <= sig)
              && (sig < QF_maxSignal_)
              && ((uint_fast8_t)0 < p) && (p <= (uint_fast8_t)QF_MAX_ACTIVE)
              && (QF_active_[p] == me));

    QF_CRIT_ENTRY_();

    QS_BEGIN_NOCRIT_(QS_QF_ACTIVE_UNSUBSCRIBE, QS_priv_.aoObjFilter, me)
        QS_TIME_();             /* timestamp */
        QS_SIG_((QSignal)sig);  /* the signal of this event */
        QS_OBJ_(me);            /* this active object */
    QS_END_NOCRIT_()

    /* clear priority bit */
    QF_PTR_AT_(QF_subscrList_, sig).bits[i] &= QF_invPwr2Lkup[p];
    QF_CRIT_EXIT_();
}

/****************************************************************************/
/**
* @description
* This function is part of the Publish-Subscribe event delivery mechanism
* available in QF. Un-subscribing from all events means that the framework
* will stop posting any published events to the event queue of the active
* object @p me.
*
* @param[in] me  pointer (see @ref oop)
*
* @note Due to the latency of event queues, an active object should NOT
* assume that no events will ever be dispatched to the state machine of
* the active object after un-subscribing from all events.
* The events might be already in the queue, or just about to be posted
* and the un-subscribe operation will not flush such events. Also, the
* alternative event-delivery mechanisms, such as direct event posting or
* time events, can be still delivered to the event queue of the active
* object.
*
* @sa QF_publish_(), QActive_subscribe(), and QActive_unsubscribe()
*/
void QActive_unsubscribeAll(QActive const * const me) {
    uint_fast8_t p = me->prio;
    uint_fast8_t i;
    enum_t sig;

    Q_REQUIRE_ID(500, ((uint_fast8_t)0 < p)
                       && (p <= (uint_fast8_t)QF_MAX_ACTIVE)
                       && (QF_active_[p] == me));

    i = (uint_fast8_t)QF_div8Lkup[p];
    for (sig = (enum_t)Q_USER_SIG; sig < QF_maxSignal_; ++sig) {
        QF_CRIT_STAT_
        QF_CRIT_ENTRY_();
        if ((QF_PTR_AT_(QF_subscrList_, sig).bits[i]
             & QF_pwr2Lkup[p]) != (uint8_t)0)
        {

            QS_BEGIN_NOCRIT_(QS_QF_ACTIVE_UNSUBSCRIBE,
                             QS_priv_.aoObjFilter, me)
                QS_TIME_();            /* timestamp */
                QS_SIG_((QSignal)sig); /* the signal of this event */
                QS_OBJ_(me);           /* this active object */
            QS_END_NOCRIT_()

            /* clear the priority bit */
            QF_PTR_AT_(QF_subscrList_, sig).bits[i] &= QF_invPwr2Lkup[p];
        }
        QF_CRIT_EXIT_();
    }
}
