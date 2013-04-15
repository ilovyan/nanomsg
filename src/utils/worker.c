/*
    Copyright (c) 2013 250bpm s.r.o.

    Permission is hereby granted, free of charge, to any person obtaining a copy
    of this software and associated documentation files (the "Software"),
    to deal in the Software without restriction, including without limitation
    the rights to use, copy, modify, merge, publish, distribute, sublicense,
    and/or sell copies of the Software, and to permit persons to whom
    the Software is furnished to do so, subject to the following conditions:

    The above copyright notice and this permission notice shall be included
    in all copies or substantial portions of the Software.

    THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
    IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
    FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL
    THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
    LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
    FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS
    IN THE SOFTWARE.
*/

#include "worker.h"
#include "err.h"
#include "fast.h"
#include "cont.h"

/*  Private functions. */
static void nn_worker_routine (void *arg);

void nn_worker_callback_init (struct nn_worker_callback *self,
    const struct nn_worker_callback_vfptr *vfptr)
{
    self->vfptr = vfptr;
}

void nn_worker_callback_term (struct nn_worker_callback *self)
{
}

void nn_worker_fd_init (struct nn_worker_fd *self,
    struct nn_worker_callback *callback)
{
    self->callback = callback;
}

void nn_worker_fd_term (struct nn_worker_fd *self)
{
}

void nn_worker_timer_init (struct nn_worker_timer *self,
    struct nn_worker_callback *callback)
{
    self->callback = callback;
    nn_timerset_hndl_init (&self->hndl);
}

void nn_worker_timer_term (struct nn_worker_timer *self)
{
    nn_timerset_hndl_term (&self->hndl);
}

void nn_worker_poller_add_fd (struct nn_worker_poller *self,
    int s, struct nn_worker_fd *fd)
{
    nn_poller_add (&((struct nn_worker*) self)->poller, s, &fd->hndl);
}

void nn_worker_poller_rm_fd (struct nn_worker_poller *self,
    struct nn_worker_fd *fd)
{
    nn_poller_rm (&((struct nn_worker*) self)->poller, &fd->hndl);
}

void nn_worker_poller_set_in (struct nn_worker_poller *self,
    struct nn_worker_fd *fd)
{
    nn_poller_set_in (&((struct nn_worker*) self)->poller, &fd->hndl);
}

void nn_worker_poller_reset_in (struct nn_worker_poller *self,
    struct nn_worker_fd *fd)
{
    nn_poller_reset_in (&((struct nn_worker*) self)->poller, &fd->hndl);
}

void nn_worker_poller_set_out (struct nn_worker_poller *self,
    struct nn_worker_fd *fd)
{
    nn_poller_set_out (&((struct nn_worker*) self)->poller, &fd->hndl);
}

void nn_worker_poller_reset_out (struct nn_worker_poller *self,
    struct nn_worker_fd *fd)
{
    nn_poller_reset_out (&((struct nn_worker*) self)->poller, &fd->hndl);
}

void nn_worker_poller_add_timer (struct nn_worker_poller *self, 
    int timeout, struct nn_worker_timer *timer)
{
    nn_timerset_add (&((struct nn_worker*) self)->timerset, timeout,
        &timer->hndl);
}

void nn_worker_poller_rm_timer (struct nn_worker_poller *self,
    struct nn_worker_timer *timer)
{
    nn_timerset_rm (&((struct nn_worker*) self)->timerset, &timer->hndl);
}

void nn_worker_task_init (struct nn_worker_task *self,
    struct nn_worker_callback *callback)
{
    self->callback = callback;
    nn_queue_item_init (&self->item);
}

void nn_worker_task_term (struct nn_worker_task *self)
{
    nn_queue_item_term (&self->item);
}

int nn_worker_init (struct nn_worker *self)
{
    int rc;

    rc = nn_efd_init (&self->efd);
    if (rc < 0)
        return rc;

    nn_mutex_init (&self->sync);
    nn_queue_init (&self->tasks);
    nn_queue_item_init (&self->stop);
    nn_poller_init (&self->poller);
    nn_poller_add (&self->poller, nn_efd_getfd (&self->efd), &self->efd_hndl);
    nn_poller_set_in (&self->poller, &self->efd_hndl);
    nn_timerset_init (&self->timerset);
    nn_thread_init (&self->thread, nn_worker_routine, self);

    return 0;
}

void nn_worker_term (struct nn_worker *self)
{
    /*  Ask worker thread to terminate. */
    nn_mutex_lock (&self->sync);
    nn_queue_push (&self->tasks, &self->stop);
    nn_efd_signal (&self->efd);
    nn_mutex_unlock (&self->sync);

    /*  Wait till worker thread terminates. */
    nn_thread_term (&self->thread);

    /*  Clean up. */
    nn_timerset_term (&self->timerset);
    nn_poller_term (&self->poller);
    nn_efd_term (&self->efd);
    nn_queue_item_term (&self->stop);
    nn_queue_term (&self->tasks);
    nn_mutex_term (&self->sync);
}

void nn_worker_execute (struct nn_worker *self, struct nn_worker_task *task)
{
    nn_mutex_lock (&self->sync);
    nn_queue_push (&self->tasks, &task->item);
    nn_efd_signal (&self->efd);
    nn_mutex_unlock (&self->sync);
}

static void nn_worker_routine (void *arg)
{
    int rc;
    struct nn_worker *self;
    int pevent;
    struct nn_poller_hndl *phndl;
    struct nn_timerset_hndl *thndl;
    struct nn_queue tasks;
    struct nn_queue_item *item;
    struct nn_worker_task *task;
    struct nn_worker_fd *fd;
    struct nn_worker_timer *timer;

    self = (struct nn_worker*) arg;

    /*  Infinite loop. It will be interrupted only when the object is
        shut down. */
    while (1) {

        /*  Wait for any activity. */
        
        rc = nn_poller_wait (&self->poller,
            nn_timerset_timeout (&self->timerset));
        errnum_assert (rc == 0, -rc);

        /*  Process all expired timers. */
        while (1) {
            rc = nn_timerset_event (&self->timerset, &thndl);
            if (rc == -EAGAIN)
                break;
            errnum_assert (rc == 0, -rc);
            timer = nn_cont (thndl, struct nn_worker_timer, hndl);
            timer->callback->vfptr->callback (timer->callback, timer,
                NN_WORKER_TIMER_TIMEOUT, (struct nn_worker_poller*) self);
        }

        /*  Process all events from the poller. */
        while (1) {

            /*  Get next poller event, such as IN or OUT. */
            rc = nn_poller_event (&self->poller, &pevent, &phndl);
            if (nn_slow (rc == -EAGAIN))
                break;

            /*  If there are any new incoming worker tasks, process them. */
            if (phndl == &self->efd_hndl) {
                nn_assert (pevent == NN_POLLER_IN);

                /*  Make a local copy of the task queue. This way
                    the application threads are not blocked and can post new
                    tasks while the existing tasks are being processed. */
                nn_mutex_lock (&self->sync);
                nn_efd_unsignal (&self->efd);
                memcpy (&tasks, &self->tasks, sizeof (tasks));
                nn_queue_init (&self->tasks);
                nn_mutex_unlock (&self->sync);

                while (1) {

                    /*  Next worker task. */
                    item = nn_queue_pop (&tasks);
                    if (nn_slow (!item))
                        break;

                    /*  If the worker thread is asked to stop, do so. */
                    if (nn_slow (item == &self->stop)) {
                        nn_queue_term (&tasks);
                        return;
                    }

                    /*  It's a user-defined task. Notify the user that it has
                        arrived in the worker thread. */
                    task = nn_cont (item, struct nn_worker_task, item);
                    task->callback->vfptr->callback (task->callback,
                        task, NN_WORKER_TASK_EXECUTE,
                        (struct nn_worker_poller*) self);
                }
                nn_queue_term (&tasks);
                continue;
            }

            /*  It's a true I/O event. Invoke the handler. */
            fd = nn_cont (phndl, struct nn_worker_fd, hndl);
            fd->callback->vfptr->callback (fd->callback, fd, pevent,
                (struct nn_worker_poller*) self);
        }
    }
}

