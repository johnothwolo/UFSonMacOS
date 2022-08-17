//
//  taskqueue.h
//  ufsX
//
//  Created by John Othwolo on 7/21/22.
//  Copyright © 2022 John Othwolo. All rights reserved.
//

#ifndef taskqueue_h
#define taskqueue_h

typedef void task_fn_t(void *context);

struct taskqueue;

struct task {
    uint8_t    ta_priority;        /* (c) Priority */
    task_fn_t *ta_func;            /* (c) task handler */
    void      *ta_context;         /* (c) argument for handler */
    void      *ta_priv;               /* private cookie */
};

#define TASK_INIT(task, priority, func, context) do {    \
    (task)->ta_priority = (priority);        \
    (task)->ta_func = (func);                \
    (task)->ta_context = (context);          \
} while (0)

__BEGIN_DECLS

struct taskqueue *taskqueue_create(const char *name, int mflags, void *context);
int    taskqueue_enqueue(struct taskqueue *queue, struct task *task);
void   taskqueue_drain(struct taskqueue *queue, struct task *task);
void   taskqueue_drain_all(struct taskqueue *queue);
void   taskqueue_free(struct taskqueue *queue);

__END_DECLS

#endif /* taskqueue_h */
