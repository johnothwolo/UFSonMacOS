//
//  IOTaskQueue.cpp
//  itlwm
//
//  Created by 钟先耀 on 2020/4/16.
//  Copyright © 2020 钟先耀. All rights reserved.
//

#include <libkern/c++/OSString.h>
#include <IOKit/IOEventSource.h>
#include <IOKit/IOLocks.h>
#include <sys/queue.h>
#include <freebsd/compat/taskqueue.h>

#pragma mark - Header 

typedef void (*IOTaskQueueAction)(void *);

#define TASK_ONQUEUE        1
#define SET(t, f)       (t) |= (f)
#define CLR(t, f)       (t) &= ~(f)
#define ISSET(t, f)     ((t) & (f))

struct IOTask {
    TAILQ_ENTRY(IOTask) entry_t;
    IOTaskQueueAction func_t;
    void *arg_t;
    unsigned int flag;
};

TAILQ_HEAD(io_task_queue, IOTask);

class IOTaskQueue : public IOEventSource {
    OSDeclareDefaultStructors(IOTaskQueue)
    
public:
    static IOTaskQueue *taskQueue(const char *name, void *context);
    
    virtual kern_return_t enqueueTask(IOTask *task);
    virtual kern_return_t delTask(IOTask *task);
    
    virtual bool init(const char *name, void *context);
    virtual void free() override;
    virtual bool checkForWork() override;
    
protected:
    
private:
    OSObject *owner;
    void *context;
    IOLock *entryLock;
    IOTask *currentTask;
    struct io_task_queue tq_worklist;
};

#pragma mark - C++ api

#define super IOEventSource
OSDefineMetaClassAndStructors(IOTaskQueue, IOEventSource)

IOTaskQueue *IOTaskQueue::taskQueue(const char *name, void *context)
{
    IOTaskQueue *tq = new IOTaskQueue;
    if (!tq) {
        return NULL;
    }
    
    if (!tq->init(name, context)) {
        tq->free();
        return NULL;
    }
    return tq;
}

bool IOTaskQueue::init(const char *name, void *context)
{
    owner = (OSObject*)OSString::withCString(name);
    if(!owner){
        return false;
    }
    if (!super::init(owner))
        return false;
    entryLock = IOLockAlloc();
    if (!entryLock) {
        return false;
    }
    TAILQ_INIT(&tq_worklist);
    return true;
}

void IOTaskQueue::free()
{
    if (entryLock) {
        IOLockFree(entryLock);
        entryLock = NULL;
    }
    if (owner){
        ((OSString*)owner)->free();
    }
}

bool IOTaskQueue::checkForWork()
{
    IOLog("itlwm: IOTaskQueue::%s\n", __FUNCTION__);
    if (!isEnabled()) {
        IOLog("itlwm: IOTaskQueue::%s !isEnabled()\n", __FUNCTION__);
        return false;
    }
    if (currentTask == NULL) {
        currentTask = TAILQ_FIRST(&tq_worklist);
        if (currentTask == NULL) {
            IOLog("itlwm: IOTaskQueue::%s TAILQ_FIRST currentTask == NULL\n", __FUNCTION__);
            return false;
        }
    } else {
        currentTask = TAILQ_NEXT(currentTask, entry_t);
        if (currentTask == NULL) {
            IOLog("itlwm: IOTaskQueue::%s TAILQ_NEXT currentTask == NULL\n", __FUNCTION__);
            return true;
        }
    }
    IOLog("itlwm: IOTaskQueue::%s execute\n", __FUNCTION__);
    (*(IOTaskQueueAction) currentTask->func_t)(currentTask->arg_t);
    return true;
}

kern_return_t IOTaskQueue::delTask(IOTask *task)
{
    IOLog("itlwm: IOTaskQueue::%s\n", __FUNCTION__);
    IOTakeLock(entryLock);
    if (!ISSET(task->flag, TASK_ONQUEUE)) {
        IOLog("itlwm: IOTaskQueue::delTask is already delete\n");
        IOUnlock(entryLock);
        return kIOReturnSuccess;
    }
    IOLog("itlwm: IOTaskQueue::delTask done\n");
    CLR(task->flag, TASK_ONQUEUE);
    TAILQ_REMOVE(&tq_worklist, task, entry_t);
    IOUnlock(entryLock);
    return kIOReturnSuccess;
}

kern_return_t IOTaskQueue::enqueueTask(IOTask *task)
{
    IOLog("itlwm: IOTaskQueue::%s\n", __FUNCTION__);
    IOTakeLock(entryLock);
    if (ISSET(task->flag, TASK_ONQUEUE)) {
        IOLog("itlwm: IOTaskQueue::enqueueTask is already on queue\n");
        IOUnlock(entryLock);
        signalWorkAvailable();
        return kIOReturnSuccess;
    }
    SET(task->flag, TASK_ONQUEUE);
    TAILQ_INSERT_TAIL(&tq_worklist, task, entry_t);
    IOUnlock(entryLock);
    signalWorkAvailable();
    return kIOReturnSuccess;
}

#pragma mark - C api

struct taskqueue *
taskqueue_create(const char *name, int mflags, void *context)
{
    IOTaskQueue* queue = IOTaskQueue::taskQueue(name, context);
    return (struct taskqueue*)queue;
}

int
taskqueue_enqueue(struct taskqueue *_queue, struct task *task)
{
    IOTaskQueue* queue = (IOTaskQueue*)_queue;
    IOTask *iotask = new IOTask;
    iotask->arg_t = task->ta_context;
    iotask->flag = 0;
    iotask->func_t = task->ta_func;
    task->ta_priv = task;
    return queue->enqueueTask(iotask);
}

void
taskqueue_drain(struct taskqueue *_queue, struct task *task)
{
    IOTaskQueue* queue = (IOTaskQueue*)_queue;
    queue->delTask((IOTask*)task->ta_priv);
}

void
taskqueue_drain_all(struct taskqueue *_queue)
{
    
}

void
taskqueue_free(struct taskqueue *_queue)
{
    taskqueue_drain_all(_queue); // drain tq
    IOTaskQueue* queue = (IOTaskQueue*)_queue;
    queue->free();
}

