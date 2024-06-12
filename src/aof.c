/*
 * Copyright (c) 2009-2012, Salvatore Sanfilippo <antirez at gmail dot com>
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are met:
 *
 *   * Redistributions of source code must retain the above copyright notice,
 *     this list of conditions and the following disclaimer.
 *   * Redistributions in binary form must reproduce the above copyright
 *     notice, this list of conditions and the following disclaimer in the
 *     documentation and/or other materials provided with the distribution.
 *   * Neither the name of Redis nor the names of its contributors may be used
 *     to endorse or promote products derived from this software without
 *     specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
 * AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT OWNER OR CONTRIBUTORS BE
 * LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
 * CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
 * SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
 * INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN
 * CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
 * ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
 * POSSIBILITY OF SUCH DAMAGE.
 */
/**
 * AOF 持久化的实现
 */
#include "server.h"
#include "bio.h"
#include "rio.h"

#include <signal.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/time.h>
#include <sys/resource.h>
#include <sys/wait.h>
#include <sys/param.h>
//更新当前aof文件的大小
void aofUpdateCurrentSize(void);
//关闭aof父子进程通信管道
void aofClosePipes(void);

/* ----------------------------------------------------------------------------
 * AOF rewrite buffer implementation.
 *
 * The following code implement a simple buffer used in order to accumulate
 * changes while the background process is rewriting the AOF file.
 *
 * We only need to append, but can't just use realloc with a large block
 * because 'huge' reallocs are not always handled as one could expect
 * (via remapping of pages at OS level) but may involve copying data.
 *
 * For this reason we use a list of blocks, every block is
 * AOF_RW_BUF_BLOCK_SIZE bytes.
 * ------------------------------------------------------------------------- */
/*
 * AOF重写buffer的实现代码
 * 代码实现是利用缓冲区来实现重写AOF文件时的更改积累，即在AOF重写时的更新记录是如何追加的
 * realloc 是扩容函数
 * 我们只需要通过追加的方式，而不是采用扩容巨大的空间来实现，因为扩容(realloc)大的空间总会造成一些我们意想不到的情况，
 * 在操作系统层面的重新映射可能会涉及到数据的复制
 *  基于上述原因，所以我们采用数据块链表，每个数据块都是AOF_RW_BUF_BLOCK_SIZE(10MB) 的大小
 */
#define AOF_RW_BUF_BLOCK_SIZE (1024*1024*10)    /* 10 MB per block */
//重写数据块的结构体，里面记录了已经使用的大小，空闲的大小，还有记录内容的buffer数组
typedef struct aofrwblock {
    unsigned long used, free;
    char buf[AOF_RW_BUF_BLOCK_SIZE];
} aofrwblock;

/* This function free the old AOF rewrite buffer if needed, and initialize
 * a fresh new one. It tests for server.aof_rewrite_buf_blocks equal to NULL
 * so can be used for the first initialization as well. */
/*下方函数的作用是在需要的时候释放旧的重写的AOF缓冲区，并且初始化一个新的缓冲区；
 * 测试 server.aof_rewrite_buf_blocks 是否等于 NULL，因此也可以用于第一次初始化。
 */
void aofRewriteBufferReset(void) {
    if (server.aof_rewrite_buf_blocks)
        listRelease(server.aof_rewrite_buf_blocks);

    server.aof_rewrite_buf_blocks = listCreate();
    listSetFreeMethod(server.aof_rewrite_buf_blocks,zfree);
}

/* Return the current size of the AOF rewrite buffer. */
/*返回当前AOF重写缓冲区的大小*/
unsigned long aofRewriteBufferSize(void) {
    listNode *ln;
    listIter li;
    unsigned long size = 0;
    //将li指向头节点
    listRewind(server.aof_rewrite_buf_blocks,&li);
    //循环累加每一个数据块的大小(默认10MB)，，直到链表的尾节点
    while((ln = listNext(&li))) {
        aofrwblock *block = listNodeValue(ln);
        size += block->used;
    }
    return size;
}

/* Event handler used to send data to the child process doing the AOF
 * rewrite. We send pieces of our AOF differences buffer so that the final
 * write when the child finishes the rewrite will be small. */
/*
 * 用于将数据发送到执行 AOF 重写的子进程的事件处理程序。我们发送 AOF 差异缓冲区的片段，以便子进程完成重写时最终写入的数据较小
 */
//将AOF数据写到与子进程通信的管道上
void aofChildWriteDiffData(aeEventLoop *el, int fd, void *privdata, int mask) {
    listNode *ln;
    aofrwblock *block;
    ssize_t nwritten;
    UNUSED(el);
    UNUSED(fd);
    UNUSED(privdata);
    UNUSED(mask);

    while(1) {
        ln = listFirst(server.aof_rewrite_buf_blocks);
        block = ln ? ln->value : NULL;
        if (server.aof_stop_sending_diff || !block) {
            aeDeleteFileEvent(server.el,server.aof_pipe_write_data_to_child,
                              AE_WRITABLE);
            return;
        }
        if (block->used > 0) {
            nwritten = write(server.aof_pipe_write_data_to_child,
                             block->buf,block->used);
            if (nwritten <= 0) return;
            memmove(block->buf,block->buf+nwritten,block->used-nwritten);
            block->used -= nwritten;
            block->free += nwritten;
        }
        if (block->used == 0) listDelNode(server.aof_rewrite_buf_blocks,ln);
    }
}

/* Append data to the AOF rewrite buffer, allocating new blocks if needed. */
/*发送数据到AOF重写缓冲区，在需要的时候进行新的数据块空间的申请*/
void aofRewriteBufferAppend(unsigned char *s, unsigned long len) {
    //定位到数据块链表的最后一个节点
    listNode *ln = listLast(server.aof_rewrite_buf_blocks);
    aofrwblock *block = ln ? ln->value : NULL;
    //未写完之前一直循环写入
    while(len) {
        /* If we already got at least an allocated block, try appending
         * at least some piece into it. */
        //追加写入到数据块中
        if (block) {
            //如果空闲位置少，则只写入空闲位置的数量，不然写入全部
            unsigned long thislen = (block->free < len) ? block->free : len;
            if (thislen) {  /* The current block is not already full. */
                //将新传入的s写入到数据块的buf中,并跟着变动空闲和使用的数据
                memcpy(block->buf+block->used, s, thislen);
                block->used += thislen;
                block->free -= thislen;
                s += thislen;
                len -= thislen;
            }
        }
        //如果是第一个，或者需要添加新的数据块的时候执行
        if (len) { /* First block to allocate, or need another block. */
            int numblocks;
            //申请一个新的数据块
            block = zmalloc(sizeof(*block));
            block->free = AOF_RW_BUF_BLOCK_SIZE;
            block->used = 0;
            //添加到数据块链表的末尾
            listAddNodeTail(server.aof_rewrite_buf_blocks,block);

            /* Log every time we cross more 10 or 100 blocks, respectively
             * as a notice or warning. */
            //每次10或者100个分区的时候，进行通知或者警告
            //当节点数为整百的时候，进行警告日志，为10的倍数的时候进行通知日志，原因未知
            numblocks = listLength(server.aof_rewrite_buf_blocks);
            if (((numblocks+1) % 10) == 0) {
                int level = ((numblocks+1) % 100) == 0 ? LL_WARNING :
                                                         LL_NOTICE;
                serverLog(level,"Background AOF buffer size: %lu MB",
                    aofRewriteBufferSize()/(1024*1024));
            }
        }
    }

    /* Install a file event to send data to the rewrite child if there is
     * not one already. */
    //如果还没有文件事件，则生成一个文件事件发送给重写子进程
    if (!server.aof_stop_sending_diff &&
        aeGetFileEvents(server.el,server.aof_pipe_write_data_to_child) == 0)
    {
        aeCreateFileEvent(server.el, server.aof_pipe_write_data_to_child,
            AE_WRITABLE, aofChildWriteDiffData, NULL);
    }
}

/* Write the buffer (possibly composed of multiple blocks) into the specified
 * fd. If a short write or any other error happens -1 is returned,
 * otherwise the number of bytes written is returned. */
/*
 * 将缓冲区的数据写入到指定的fd句柄(文件)中(可能多个文件)，如果是发生短写，或者错误，则返回-1.其他情况返回写入的长度
 */
// ssize_t 是long别名typedef的作用
ssize_t aofRewriteBufferWrite(int fd) {
    listNode *ln;
    listIter li;
    ssize_t count = 0;

    listRewind(server.aof_rewrite_buf_blocks,&li);
    //追加写入数据到数据块中,并记录写入的数量
    while((ln = listNext(&li))) {
        aofrwblock *block = listNodeValue(ln);
        ssize_t nwritten;

        if (block->used) {
            nwritten = write(fd,block->buf,block->used);
            if (nwritten != (ssize_t)block->used) {
                if (nwritten == 0) errno = EIO;
                return -1;
            }
            count += nwritten;
        }
    }
    return count;
}

/* ----------------------------------------------------------------------------
 * AOF file implementation
 * ------------------------------------------------------------------------- */
/*
 *----------------------------------------------------------------------------
 * 下面是AOF的实现代码
 * ----------------------------------------------------------------------------
 */
/* Return true if an AOf fsync is currently already in progress in a
 * BIO thread. */
//如果AOF同步已经正在BIO线程中进行，则返回true
int aofFsyncInProgress(void) {
    return bioPendingJobsOfType(BIO_AOF_FSYNC) != 0;
}

/* Starts a background task that performs fsync() against the specified
 * file descriptor (the one of the AOF file) in another thread. */
/*
 * 启动一个后台任务，在另一个线程中对指定的文件描述符（AOF 文件的一个）执行 fsync()。
 */
void aof_background_fsync(int fd) {
    //创建一个bio的同步任务
    bioCreateFsyncJob(fd);
}

/* Kills an AOFRW child process if exists */
//当AOF重写结束的时候，结束掉子进程
void killAppendOnlyChild(void) {
    int statloc;
    /* No AOFRW child? return. */
    //如果没有子进程，则立马返回
    if (server.child_type != CHILD_TYPE_AOF) return;
    /* Kill AOFRW child, wait for child exit. */
    //等待子进程退出，结束AOF重写的子进程
    serverLog(LL_NOTICE,"Killing running AOF rewrite child: %ld",
        (long) server.child_pid);
    if (kill(server.child_pid,SIGUSR1) != -1) {
        while(waitpid(-1, &statloc, 0) != server.child_pid);
    }
    /* Reset the buffer accumulating changes while the child saves. */
    //在子进程保存的时候，重制缓冲区并开始记录
    aofRewriteBufferReset();
    //清理aof的临时文件
    aofRemoveTempFile(server.child_pid);
    //重置子进程的状态记录
    resetChildState();
    //aof重写开始时间记录为-1
    server.aof_rewrite_time_start = -1;
    /* Close pipes used for IPC between the two processes. */
    //关闭aof管道 ,IPC:ipc一般指进程通信。 进程通信是指在进程间传输数据(交换信息)
    aofClosePipes();
}

/* Called when the user switches from "appendonly yes" to "appendonly no"
 * at runtime using the CONFIG command. */
//当用户从客户端使用config命令修改appendonly 从yes修改为no的时候停止aof的记录
void stopAppendOnly(void) {
    //断言判断,不满足的时候触发错误
    serverAssert(server.aof_state != AOF_OFF);
    //刷写aof文件
    flushAppendOnlyFile(1);
    //aof文件同步失败？
    if (redis_fsync(server.aof_fd) == -1) {
        //记录log
        serverLog(LL_WARNING,"Fail to fsync the AOF file: %s",strerror(errno));
    } else {
        //变更同步的游标和时间
        server.aof_fsync_offset = server.aof_current_size;
        server.aof_last_fsync = server.unixtime;
    }
    //关闭句柄
    close(server.aof_fd);
    //将相关的信息重置
    server.aof_fd = -1;
    server.aof_selected_db = -1;
    server.aof_state = AOF_OFF;
    server.aof_rewrite_scheduled = 0;
    //关闭写aof的子进程
    killAppendOnlyChild();
    //??SDS,没明白
    sdsfree(server.aof_buf);
    server.aof_buf = sdsempty();
}

/* Called when the user switches from "appendonly no" to "appendonly yes"
 * at runtime using the CONFIG command. */
//从命令行通过config命令配置aof启动时调用的函数
int startAppendOnly(void) {
    //当前工作目录
    char cwd[MAXPATHLEN]; /* Current working dir path for error messages. */
    //新的文件操作句柄
    int newfd;
    //打开对应的文件
    newfd = open(server.aof_filename,O_WRONLY|O_APPEND|O_CREAT,0644);
    //断言已经开启aof
    serverAssert(server.aof_state == AOF_OFF);
    //如果aof文件无法打开，报错并返回
    if (newfd == -1) {
        char *cwdp = getcwd(cwd,MAXPATHLEN);

        serverLog(LL_WARNING,
            "Redis needs to enable the AOF but can't open the "
            "append only file %s (in server root dir %s): %s",
            server.aof_filename,
            cwdp ? cwdp : "unknown",
            strerror(errno));
        return C_ERR;
    }
    //是否有活跃重写子进程
    if (hasActiveChildProcess() && server.child_type != CHILD_TYPE_AOF) {
        server.aof_rewrite_scheduled = 1;
        serverLog(LL_WARNING,"AOF was enabled but there is already another background operation. An AOF background was scheduled to start when possible.");
    } else {
        /* If there is a pending AOF rewrite, we need to switch it off and
         * start a new one: the old one cannot be reused because it is not
         * accumulating the AOF buffer. */
        /*如果有已经存在的aof重写的，我们需要关闭并且重新开启一个新的，因为老的缓冲区无法缓冲新的AOF buffer*/
        if (server.child_type == CHILD_TYPE_AOF) {
            serverLog(LL_WARNING,"AOF was enabled but there is already an AOF rewriting in background. Stopping background AOF and starting a rewrite now.");
            killAppendOnlyChild();
        }
        if (rewriteAppendOnlyFileBackground() == C_ERR) {
            close(newfd);
            serverLog(LL_WARNING,"Redis needs to enable the AOF but can't trigger a background AOF rewrite operation. Check the above logs for more info about the error.");
            return C_ERR;
        }
    }
    /* We correctly switched on AOF, now wait for the rewrite to be complete
     * in order to append data on disk. */
    //等待完成aof重写数据到磁盘上
    //将对应的状态进行变更
    server.aof_state = AOF_WAIT_REWRITE;
    server.aof_last_fsync = server.unixtime;
    server.aof_fd = newfd;

    /* If AOF fsync error in bio job, we just ignore it and log the event. */
    //如果aof同步报错，忽略并且将其记录到日志中
    int aof_bio_fsync_status;
    atomicGet(server.aof_bio_fsync_status, aof_bio_fsync_status);
    if (aof_bio_fsync_status == C_ERR) {
        serverLog(LL_WARNING,
            "AOF reopen, just ignore the AOF fsync error in bio job");
        atomicSet(server.aof_bio_fsync_status,C_OK);
    }

    /* If AOF was in error state, we just ignore it and log the event. */
    //如果aof状态报错，忽略，记录到日志中
    if (server.aof_last_write_status == C_ERR) {
        serverLog(LL_WARNING,"AOF reopen, just ignore the last error.");
        server.aof_last_write_status = C_OK;
    }
    return C_OK;
}

/* This is a wrapper to the write syscall in order to retry on short writes
 * or if the syscall gets interrupted. It could look strange that we retry
 * on short writes given that we are writing to a block device: normally if
 * the first call is short, there is a end-of-space condition, so the next
 * is likely to fail. However apparently in modern systems this is no longer
 * true, and in general it looks just more resilient to retry the write. If
 * there is an actual error condition we'll get it at the next try. */
/*
 *这是写入系统调用的包装器，用于在短写入或系统调用中断时重试。
 *考虑到我们正在写入块设备，我们在短写入时重试可能看起来很奇怪：
 *通常，如果第一个调用很短，则会出现空间结束的情况，因此下一个调用很可能会失败。
 *然而，在现代系统中，这显然不再是事实，而且一般来说，重试写入看起来更具弹性。
 *如果存在实际错误情况，我们将在下一次尝试时得到它
 */
//AOF写入,返回的是写入的字节数长度long类型
/**
 * AOF写入
 * @param fd 操作文件的句柄
 * @param buf 缓冲区数据
 * @param len  数据的长度
 * @return
 */
ssize_t aofWrite(int fd, const char *buf, size_t len) {
    ssize_t nwritten = 0, totwritten = 0;
    //当数据未完全写完的时候，一直循环写入
    while(len) {
        //nwritten 为写入的长度
        nwritten = write(fd, buf, len);

        if (nwritten < 0) {
            //如果是系统中断门，则继续，否则返回写入的长度
            if (errno == EINTR) continue;
            return totwritten ? totwritten : -1;
        }

        len -= nwritten;
        buf += nwritten;
        totwritten += nwritten;
    }

    return totwritten;
}

/* Write the append only file buffer on disk.
 *
 * Since we are required to write the AOF before replying to the client,
 * and the only way the client socket can get a write is entering when the
 * the event loop, we accumulate all the AOF writes in a memory
 * buffer and write it on disk using this function just before entering
 * the event loop again.
 *
 * About the 'force' argument:
 *
 * When the fsync policy is set to 'everysec' we may delay the flush if there
 * is still an fsync() going on in the background thread, since for instance
 * on Linux write(2) will be blocked by the background fsync anyway.
 * When this happens we remember that there is some aof buffer to be
 * flushed ASAP, and will try to do that in the serverCron() function.
 *
 * However if force is set to 1 we'll write regardless of the background
 * fsync. */
/*
 * 将aof的缓冲区写入到磁盘中
 * 由于我们需要在回复客户端之前写入 AOF，而客户端套接字获得写入的唯一方法是在事件循环时进入，
 * 因此我们将所有 AOF 写入累积在内存缓冲区中，并在再次进入事件循环之前使用此函数将其写入磁盘。
 *
 * 关于“force”参数：
 *
 * 当 fsync 策略设置为“everysec”时，如果后台线程中仍有 fsync() 进行，我们可能会延迟刷新，因为例如在 Linux 上，write(2) 无论如何都会被后台 fsync 阻止。
 * 发生这种情况时，我们会记得有一些 aof 缓冲区需要尽快刷新，并会尝试在 serverCron() 函数中执行此操作。
 *
 * 但是，如果 force 设置为 1，我们将不
 */
//两次异常的报告时间间隔
#define AOF_WRITE_LOG_ERROR_RATE 30 /* Seconds between errors logging. */
//刷写aof文件到磁盘
void flushAppendOnlyFile(int force) {
    ssize_t nwritten;//写入的长度
    int sync_in_progress = 0;//同步的进程
    mstime_t latency;//最后的时间
    //如果缓冲区为空
    if (sdslen(server.aof_buf) == 0) {
        /* Check if we need to do fsync even the aof buffer is empty,
         * because previously in AOF_FSYNC_EVERYSEC mode, fsync is
         * called only when aof buffer is not empty, so if users
         * stop write commands before fsync called in one second,
         * the data in page cache cannot be flushed in time. */
        /*
         * 检查是否需要在 aof 缓冲区为空时执行 fsync，
         * 因为之前在 AOF_FSYNC_EVERYSEC 模式下，仅当 aof 缓冲区不为空时才调用 fsync，因此如果用户
         * 在一秒钟内调用 fsync 之前停止写入命令，
         * 页面缓存中的数据将无法及时刷新。
         */
        //下面的判断条件是 当前的aof刷盘策略是每秒刷盘，并且当前offset和已经刷盘的offset不相同，且当前时间>最后一次数盘时间，并且没有aof的刷盘作业
        //满足上面条件，则跳转到try_fsync函数
        if (server.aof_fsync == AOF_FSYNC_EVERYSEC &&
            server.aof_fsync_offset != server.aof_current_size &&
            server.unixtime > server.aof_last_fsync &&
            !(sync_in_progress = aofFsyncInProgress())) {
            goto try_fsync;
        } else {
            return;
        }
    }
    //如果是每秒刷新一次
    if (server.aof_fsync == AOF_FSYNC_EVERYSEC)
        sync_in_progress = aofFsyncInProgress();

    if (server.aof_fsync == AOF_FSYNC_EVERYSEC && !force) {
        /* With this append fsync policy we do background fsyncing.
         * If the fsync is still in progress we can try to delay
         * the write for a couple of seconds. */
        /*
         * 使用此附加 fsync 策略，我们可以进行后台 fsync。
         * 如果 fsync 仍在进行中，我们可以尝试延迟写入几秒钟
         */
        //如果正在进行刷盘
        if (sync_in_progress) {
            //配置关闭推迟刷盘
            if (server.aof_flush_postponed_start == 0) {
                /* No previous write postponing, remember that we are
                 * postponing the flush and return. */
                //之前没有延迟的，现在记录延迟时间并且返回
                server.aof_flush_postponed_start = server.unixtime;
                return;
            } else if (server.unixtime - server.aof_flush_postponed_start < 2) {
                /* We were already waiting for fsync to finish, but for less
                 * than two seconds this is still ok. Postpone again. */
                //在2s时间内可以再次延迟
                return;
            }
            /* Otherwise fall trough, and go write since we can't wait
             * over two seconds. */
            //超过了2s，直接写入，不管了
            server.aof_delayed_fsync++;//更新延迟写入的次数
            //记录log
            serverLog(LL_NOTICE,"Asynchronous AOF fsync is taking too long (disk is busy?). Writing the AOF buffer without waiting for fsync to complete, this may slow down Redis.");
        }
    }
    /* We want to perform a single write. This should be guaranteed atomic
     * at least if the filesystem we are writing is a real physical one.
     * While this will save us against the server being killed I don't think
     * there is much to do about the whole server stopping for power problems
     * or alike */
    /*
     * 我们希望执行一次写入。这应该保证是原子的
     * 至少如果我们写入的文件系统是真实的物理文件系统。
     * 虽然这将使我们免于服务器被关闭，但我认为
     * 对于整个服务器因电源问题而停止运行，我们无能为力
     * 或类似情况
     */
    //如果缓冲区不为空，并且配置了刷新前休眠，则进行一次休眠
    if (server.aof_flush_sleep && sdslen(server.aof_buf)) {
        usleep(server.aof_flush_sleep);
    }
    //开始监控
    latencyStartMonitor(latency);
    //文件写入,将缓冲区的数据写入到aof_fd的文件句柄中，长度是buf的长度
    nwritten = aofWrite(server.aof_fd,server.aof_buf,sdslen(server.aof_buf));
    //结束监控
    latencyEndMonitor(latency);
    /* We want to capture different events for delayed writes:
     * when the delay happens with a pending fsync, or with a saving child
     * active, and when the above two conditions are missing.
     * We also use an additional event name to save all samples which is
     * useful for graphing / monitoring purposes. */
    /*
     * 我们希望捕获延迟写入的不同事件：
     * 当延迟发生在挂起的 fsync 中，或处于保存子进程活动状态时，以及当上述两个条件缺失时。
     * 我们还使用额外的事件名称来保存所有样本，这对于绘图/监控目的很有用。
     */
    if (sync_in_progress) {
        //如果正在同步的情况
        latencyAddSampleIfNeeded("aof-write-pending-fsync",latency);
    } else if (hasActiveChildProcess()) {
        //如果有活跃的子进程的情况
        latencyAddSampleIfNeeded("aof-write-active-child",latency);
    } else {
        //
        latencyAddSampleIfNeeded("aof-write-alone",latency);
    }
    latencyAddSampleIfNeeded("aof-write",latency);

    /* We performed the write so reset the postponed flush sentinel to zero. */
    /*我们执行了写入操作，因此将推迟的刷新哨兵重置为零*/
    server.aof_flush_postponed_start = 0;
    //如果新写入的长度和buffer的长度不相同，则执行下面的逻辑
    //Short write表示写入不完整？？？
    if (nwritten != (ssize_t)sdslen(server.aof_buf)) {
        static time_t last_write_error_log = 0;
        int can_log = 0;

        /* Limit logging rate to 1 line per AOF_WRITE_LOG_ERROR_RATE seconds. */
        // 根据配置限制日志输出频率
        if ((server.unixtime - last_write_error_log) > AOF_WRITE_LOG_ERROR_RATE) {
            can_log = 1;
            last_write_error_log = server.unixtime;
        }

        /* Log the AOF write error and record the error code. */
        //记录错误日志和错误代码
        if (nwritten == -1) {
            if (can_log) {
                serverLog(LL_WARNING,"Error writing to the AOF file: %s",
                    strerror(errno));
                server.aof_last_write_errno = errno;
            }
        } else {
            if (can_log) {
                serverLog(LL_WARNING,"Short write while writing to "
                                       "the AOF file: (nwritten=%lld, "
                                       "expected=%lld)",
                                       (long long)nwritten,
                                       (long long)sdslen(server.aof_buf));
            }
            //尝试移除追加写入的不完整内容
            if (ftruncate(server.aof_fd, server.aof_current_size) == -1) {
                if (can_log) {
                    serverLog(LL_WARNING, "Could not remove short write "
                             "from the append-only file.  Redis may refuse "
                             "to load the AOF the next time it starts.  "
                             "ftruncate: %s", strerror(errno));
                }
            } else {
                /* If the ftruncate() succeeded we can set nwritten to
                 * -1 since there is no longer partial data into the AOF. */
                nwritten = -1;
            }
            server.aof_last_write_errno = ENOSPC;
        }

        /* Handle the AOF write error. */
        //处理aof写入错误
        //当aof策略为每次都写入的时候执行
        if (server.aof_fsync == AOF_FSYNC_ALWAYS) {
            /* We can't recover when the fsync policy is ALWAYS since the reply
             * for the client is already in the output buffers (both writes and
             * reads), and the changes to the db can't be rolled back. Since we
             * have a contract with the user that on acknowledged or observed
             * writes are is synced on disk, we must exit. */
            /*当 fsync 策略为 ALWAYS 时，我们无法恢复，因为客户端的回复
             * 已经在输出缓冲区中（写入和
             * 读取），并且无法回滚对数据库的更改。由于我们
             * 与用户签订了合同，确认或观察到的
             * 写入在磁盘上同步，因此我们必须退出。*/
            serverLog(LL_WARNING,"Can't recover from AOF write error when the AOF fsync policy is 'always'. Exiting...");
            exit(1);
        } else {
            /* Recover from failed write leaving data into the buffer. However
             * set an error to stop accepting writes as long as the error
             * condition is not cleared. */
            /*从失败的写入中恢复，将数据留在缓冲区中。但是
             * 只要错误
             * 条件未清除，就设置错误以停止接受写入*/
            server.aof_last_write_status = C_ERR;

            /* Trim the sds buffer if there was a partial write, and there
             * was no way to undo it with ftruncate(2). */
            //如果有部分写入，且无法撤销ftruncate操作，则修剪 sds 缓冲区
            if (nwritten > 0) {
                server.aof_current_size += nwritten;
                sdsrange(server.aof_buf,nwritten,-1);
            }
            return; /* We'll try again on the next call... */
        }
    } else {
        /* Successful write(2). If AOF was in error state, restore the
         * OK state and log the event. */
        //成功写入，如果状态不对，则修改状态为成功的ok
        if (server.aof_last_write_status == C_ERR) {
            serverLog(LL_WARNING,
                "AOF write error looks solved, Redis can write again.");
            server.aof_last_write_status = C_OK;
        }
    }
    //更新当前的aof的size大小
    server.aof_current_size += nwritten;

    /* Re-use AOF buffer when it is small enough. The maximum comes from the
     * arena size of 4k minus some overhead (but is otherwise arbitrary). */
    //如果缓冲区足够小，则进行重新使用，这样可以减少4K的操作（磁盘）
    if ((sdslen(server.aof_buf)+sdsavail(server.aof_buf)) < 4000) {
        //buffer清空
        sdsclear(server.aof_buf);
    } else {
        sdsfree(server.aof_buf);
        //新建一个新的buffer，并指向新的buffer
        server.aof_buf = sdsempty();
    }

try_fsync:
    /* Don't fsync if no-appendfsync-on-rewrite is set to yes and there are
     * children doing I/O in the background. */
    //如果 no-appendfsync-on-rewrite 设置为 yes，并且有子进程在后台执行 I/O，则不要进行 fsync
    if (server.aof_no_fsync_on_rewrite && hasActiveChildProcess())
        return;

    /* Perform the fsync if needed. */
    //每次都持久化一次的配置
    if (server.aof_fsync == AOF_FSYNC_ALWAYS) {
        /* redis_fsync is defined as fdatasync() for Linux in order to avoid
         * flushing metadata. */
        //fdatasync是为了避免刷新元数据
        //开启监控
        latencyStartMonitor(latency);
        /* Let's try to get this data on the disk. To guarantee data safe when
         * the AOF fsync policy is 'always', we should exit if failed to fsync
         * AOF (see comment next to the exit(1) after write error above). */
        //在持久化策略为always的时候，为了保证数据的安全，如果出现写入失败，则进行退出
        if (redis_fsync(server.aof_fd) == -1) {
            serverLog(LL_WARNING,"Can't persist AOF for fsync error when the "
              "AOF fsync policy is 'always': %s. Exiting...", strerror(errno));
            exit(1);
        }
        //结束监控,并更新相关的游标和时间
        latencyEndMonitor(latency);
        latencyAddSampleIfNeeded("aof-fsync-always",latency);
        server.aof_fsync_offset = server.aof_current_size;
        server.aof_last_fsync = server.unixtime;
    } else if ((server.aof_fsync == AOF_FSYNC_EVERYSEC &&
                server.unixtime > server.aof_last_fsync)) {
        //每秒执行一次的配置
        //如果没有正在同步的作业，则开启一个后台作业
        if (!sync_in_progress) {
            aof_background_fsync(server.aof_fd);
            server.aof_fsync_offset = server.aof_current_size;
        }
        server.aof_last_fsync = server.unixtime;
    }
}
//根据传入的命令和参数构建出满足aof格式的sds
sds catAppendOnlyGenericCommand(sds dst, int argc, robj **argv) {
    char buf[32];
    int len, j;
    robj *o;

    buf[0] = '*';
    len = 1+ll2string(buf+1,sizeof(buf)-1,argc);
    buf[len++] = '\r';
    buf[len++] = '\n';
    dst = sdscatlen(dst,buf,len);

    for (j = 0; j < argc; j++) {
        o = getDecodedObject(argv[j]);
        buf[0] = '$';
        len = 1+ll2string(buf+1,sizeof(buf)-1,sdslen(o->ptr));
        buf[len++] = '\r';
        buf[len++] = '\n';
        dst = sdscatlen(dst,buf,len);
        dst = sdscatlen(dst,o->ptr,sdslen(o->ptr));
        dst = sdscatlen(dst,"\r\n",2);
        decrRefCount(o);
    }
    return dst;
}

/* Create the sds representation of a PEXPIREAT command, using
 * 'seconds' as time to live and 'cmd' to understand what command
 * we are translating into a PEXPIREAT.
 *
 * This command is used in order to translate EXPIRE and PEXPIRE commands
 * into PEXPIREAT command so that we retain precision in the append only
 * file, and the time is always absolute and not relative. */
//根据expire 或者expire的相关命令构建出符合的aof格式的sds，时间是绝对时间(具体过期时间)
sds catAppendOnlyExpireAtCommand(sds buf, struct redisCommand *cmd, robj *key, robj *seconds) {
    long long when;
    robj *argv[3];

    /* Make sure we can use strtoll */
    seconds = getDecodedObject(seconds);
    when = strtoll(seconds->ptr,NULL,10);
    /* Convert argument into milliseconds for EXPIRE, SETEX, EXPIREAT */
    if (cmd->proc == expireCommand || cmd->proc == setexCommand ||
        cmd->proc == expireatCommand)
    {
        when *= 1000;
    }
    /* Convert into absolute time for EXPIRE, PEXPIRE, SETEX, PSETEX */
    if (cmd->proc == expireCommand || cmd->proc == pexpireCommand ||
        cmd->proc == setexCommand || cmd->proc == psetexCommand)
    {
        when += mstime();
    }
    decrRefCount(seconds);

    argv[0] = shared.pexpireat;
    argv[1] = key;
    argv[2] = createStringObjectFromLongLong(when);
    buf = catAppendOnlyGenericCommand(buf, 3, argv);
    decrRefCount(argv[2]);
    return buf;
}
//将给定的命令及其参数以协议格式写入aof文件中
void feedAppendOnlyFile(struct redisCommand *cmd, int dictid, robj **argv, int argc) {
    sds buf = sdsempty();
    /* The DB this command was targeting is not the same as the last command
     * we appended. To issue a SELECT command is needed. */
    //当最后一次db操作的库和当前所在库不同的时候，我们需要通过select命令来修改库
    if (dictid != server.aof_selected_db) {
        char seldb[64];

        snprintf(seldb,sizeof(seldb),"%d",dictid);
        buf = sdscatprintf(buf,"*2\r\n$6\r\nSELECT\r\n$%lu\r\n%s\r\n",
            (unsigned long)strlen(seldb),seldb);
        //修改db
        server.aof_selected_db = dictid;
    }
    //如果是expire命令
    if (cmd->proc == expireCommand || cmd->proc == pexpireCommand ||
        cmd->proc == expireatCommand) {
        /* Translate EXPIRE/PEXPIRE/EXPIREAT into PEXPIREAT */
        //将其转化为具体的过期时间
        buf = catAppendOnlyExpireAtCommand(buf,cmd,argv[1],argv[2]);
    } else if (cmd->proc == setCommand && argc > 3) {
        robj *pxarg = NULL;
        /* When SET is used with EX/PX argument setGenericCommand propagates them with PX millisecond argument.
         * So since the command arguments are re-written there, we can rely here on the index of PX being 3. */
        //使用ex/px的时候，时间会被修改成绝对值时间，所以根据参数修订
        if (!strcasecmp(argv[3]->ptr, "px")) {
            pxarg = argv[4];
        }
        /* For AOF we convert SET key value relative time in milliseconds to SET key value absolute time in
         * millisecond. Whenever the condition is true it implies that original SET has been transformed
         * to SET PX with millisecond time argument so we do not need to worry about unit here.*/
        /*对于 AOF，我们将 SET 键值相对时间（以毫秒为单位）转换为 SET 键值绝对时间（以
         * 毫秒为单位）。只要条件为真，就意味着原始 SET 已转换为
         * 带有毫秒时间参数的 SET PX，因此我们无需担心这里的单位。*/
        if (pxarg) {
            robj *millisecond = getDecodedObject(pxarg);
            long long when = strtoll(millisecond->ptr,NULL,10);
            when += mstime();

            decrRefCount(millisecond);

            robj *newargs[5];
            newargs[0] = argv[0];
            newargs[1] = argv[1];
            newargs[2] = argv[2];
            newargs[3] = shared.pxat;
            newargs[4] = createStringObjectFromLongLong(when);
            buf = catAppendOnlyGenericCommand(buf,5,newargs);
            decrRefCount(newargs[4]);
        } else {
            buf = catAppendOnlyGenericCommand(buf,argc,argv);
        }
    } else {
        /* All the other commands don't need translation or need the
         * same translation already operated in the command vector
         * for the replication itself. */
        //其他不需要翻译的命令直接写入到aof文件中即可
        buf = catAppendOnlyGenericCommand(buf,argc,argv);
    }

    /* Append to the AOF buffer. This will be flushed on disk just before
     * of re-entering the event loop, so before the client will get a
     * positive reply about the operation performed. */
    /*在客户端收到有关所执行操作的肯定答复之前,附加到 AOF 缓冲区的数据将在重新进入事件循环之前刷新到磁盘。*/
    if (server.aof_state == AOF_ON)
        server.aof_buf = sdscatlen(server.aof_buf,buf,sdslen(buf));

    /* If a background append only file rewriting is in progress we want to
     * accumulate the differences between the child DB and the current one
     * in a buffer, so that when the child process will do its work we
     * can append the differences to the new append only file. */
    /* 如果后台的追加文件重写正在进行中，我们希望
     * 在缓冲区中累积子数据库和当前数据库之间的差异
     *，这样当子进程执行其工作时，我们
     * 可以将差异附加到新的追加文件中*/
    //如果有正在作业的aof重写操作，则将对应的变更操作也记录到aof重写的buffer中
    if (server.child_type == CHILD_TYPE_AOF)
        aofRewriteBufferAppend((unsigned char*)buf,sdslen(buf));
    //释放buffer
    sdsfree(buf);
}

/* ----------------------------------------------------------------------------
 * AOF loading
 * ------------------------------------------------------------------------- */
/*
 * AOF启动的时候恢复数据的内容模块
 */
/* In Redis commands are always executed in the context of a client, so in
 * order to load the append only file we need to create a fake client. */
//创建aof恢复的客户端(伪造/虚假的)
struct client *createAOFClient(void) {
    struct client *c = zmalloc(sizeof(*c));

    selectDb(c,0);
    c->id = CLIENT_ID_AOF; /* So modules can identify it's the AOF client. */
    c->conn = NULL;
    c->name = NULL;
    c->querybuf = sdsempty();
    c->querybuf_peak = 0;
    c->argc = 0;
    c->argv = NULL;
    c->original_argc = 0;
    c->original_argv = NULL;
    c->argv_len_sum = 0;
    c->bufpos = 0;

    /*
     * The AOF client should never be blocked (unlike master
     * replication connection).
     * This is because blocking the AOF client might cause
     * deadlock (because potentially no one will unblock it).
     * Also, if the AOF client will be blocked just for
     * background processing there is a chance that the
     * command execution order will be violated.
     * AOF 客户端永远不应被阻止（与主复制连接不同）。
     * 这是因为阻止 AOF 客户端可能会导致死锁（因为可能没有人会解除阻止）。
     * 此外，如果 AOF 客户端只是为了后台处理而被阻止，则有可能违反命令执行顺序。
     */
    c->flags = CLIENT_DENY_BLOCKING;

    c->btype = BLOCKED_NONE;
    /* We set the fake client as a slave waiting for the synchronization
     * so that Redis will not try to send replies to this client.
     * 因为是虚拟的客户端，所以不会向这个客户端发送同步的数据*/
    c->replstate = SLAVE_STATE_WAIT_BGSAVE_START;
    c->reply = listCreate();
    c->reply_bytes = 0;
    c->obuf_soft_limit_reached_time = 0;
    c->watched_keys = listCreate();
    c->peerid = NULL;
    c->sockname = NULL;
    c->resp = 2;
    c->user = NULL;
    listSetFreeMethod(c->reply,freeClientReplyValue);
    listSetDupMethod(c->reply,dupClientReplyValue);
    initClientMultiState(c);
    return c;
}
//完成了数据恢复，释放掉虚拟的客户端的参数
void freeFakeClientArgv(struct client *c) {
    int j;

    for (j = 0; j < c->argc; j++)
        decrRefCount(c->argv[j]);
    zfree(c->argv);
    c->argv_len_sum = 0;
}
//完成数据恢复，释放虚拟客户端
void freeFakeClient(struct client *c) {
    sdsfree(c->querybuf);
    listRelease(c->reply);
    listRelease(c->watched_keys);
    freeClientMultiState(c);
    freeClientOriginalArgv(c);
    zfree(c);
}

/* Replay the append log file. On success C_OK is returned. On non fatal
 * error (the append only file is zero-length) C_ERR is returned. On
 * fatal error an error message is logged and the program exists. */
//重新执行所有的aof记录的操作，如果返回ok，则表示恢复完成，如果aof为空，则返回c_err,如果退出，则出现了异常错误
//从aof文件中恢复
int loadAppendOnlyFile(char *filename) {
    struct client *fakeClient;
    //打开文件
    FILE *fp = fopen(filename,"r");
    struct redis_stat sb;
    //旧的aof状态
    int old_aof_state = server.aof_state;
    long loops = 0;
    //命令偏移量
    off_t valid_up_to = 0; /* Offset of latest well-formed command loaded. */
    //multi的偏移量
    off_t valid_before_multi = 0; /* Offset before MULTI command loaded. */
    //没有文件，则直接退出
    if (fp == NULL) {
        serverLog(LL_WARNING,"Fatal error: can't open the append log file for reading: %s",strerror(errno));
        exit(1);
    }

    /* Handle a zero-length AOF file as a special case. An empty AOF file
     * is a valid AOF because an empty server with AOF enabled will create
     * a zero length file at startup, that will remain like that if no write
     * operation is received. */
    //如果aof文件为空数据，则进行特殊处理
    if (fp && redis_fstat(fileno(fp),&sb) != -1 && sb.st_size == 0) {
        server.aof_current_size = 0;
        server.aof_fsync_offset = server.aof_current_size;
        fclose(fp);
        return C_ERR;
    }

    /* Temporarily disable AOF, to prevent EXEC from feeding a MULTI
     * to the same file we're about to read. */
    //暂时禁用aof，避免同时读取操作同一个文件
    server.aof_state = AOF_OFF;
    //创建以恶搞用于恢复的客户端
    fakeClient = createAOFClient();
    //读取aof文件内容
    startLoadingFile(fp, filename, RDBFLAGS_AOF_PREAMBLE);

    /* Check if this AOF file has an RDB preamble. In that case we need to
     * load the RDB file and later continue loading the AOF tail. */
    //检查是否是aof和rdb的组合持久化模式，如果是，则在恢复aof之前，恢复rdb的内容，再后追加aof的内容恢复
    char sig[5]; /* "REDIS" */
    if (fread(sig,1,5,fp) != 5 || memcmp(sig,"REDIS",5) != 0) {
        /* No RDB preamble, seek back at 0 offset. */
        if (fseek(fp,0,SEEK_SET) == -1) goto readerr;
    } else {
        /* RDB preamble. Pass loading the RDB functions. */
        rio rdb;

        serverLog(LL_NOTICE,"Reading RDB preamble from AOF file...");
        if (fseek(fp,0,SEEK_SET) == -1) goto readerr;
        rioInitWithFile(&rdb,fp);
        if (rdbLoadRio(&rdb,RDBFLAGS_AOF_PREAMBLE,NULL) != C_OK) {
            serverLog(LL_WARNING,"Error reading the RDB preamble of the AOF file, AOF loading aborted");
            goto readerr;
        } else {
            serverLog(LL_NOTICE,"Reading the remaining AOF tail...");
        }
    }

    /* Read the actual AOF file, in REPL format, command by command. */
    //一个一个命令的从aof文件中读取出来
    while(1) {
        int argc, j;
        unsigned long len;
        robj **argv;
        char buf[128];
        sds argsds;
        struct redisCommand *cmd;

        /* Serve the clients from time to time */
        if (!(loops++ % 1000)) {
            loadingProgress(ftello(fp));
            processEventsWhileBlocked();
            processModuleLoadingProgressEvent(1);
        }

        if (fgets(buf,sizeof(buf),fp) == NULL) {
            if (feof(fp))
                break;
            else
                goto readerr;
        }
        if (buf[0] != '*') goto fmterr;
        if (buf[1] == '\0') goto readerr;
        argc = atoi(buf+1);
        if (argc < 1) goto fmterr;

        /* Load the next command in the AOF as our fake client
         * argv. */
        //为虚拟客户端读取aof的执行指令
        argv = zmalloc(sizeof(robj*)*argc);
        fakeClient->argc = argc;
        fakeClient->argv = argv;

        for (j = 0; j < argc; j++) {
            /* Parse the argument len. */
            char *readres = fgets(buf,sizeof(buf),fp);
            if (readres == NULL || buf[0] != '$') {
                fakeClient->argc = j; /* Free up to j-1. */
                freeFakeClientArgv(fakeClient);
                if (readres == NULL)
                    goto readerr;
                else
                    goto fmterr;
            }
            len = strtol(buf+1,NULL,10);

            /* Read it into a string object. */
            //转化成字符串对象
            argsds = sdsnewlen(SDS_NOINIT,len);
            //如果为空，则释放资源并跳转异常
            if (len && fread(argsds,len,1,fp) == 0) {
                sdsfree(argsds);
                fakeClient->argc = j; /* Free up to j-1. */
                freeFakeClientArgv(fakeClient);
                goto readerr;
            }
            //将参数转化成字符串对象类型
            argv[j] = createObject(OBJ_STRING,argsds);

            /* Discard CRLF. */
            if (fread(buf,2,1,fp) == 0) {
                fakeClient->argc = j+1; /* Free up to j. */
                freeFakeClientArgv(fakeClient);
                goto readerr;
            }
        }

        /* Command lookup */
        //转变成命令
        cmd = lookupCommand(argv[0]->ptr);
        //如果没有对应的命令则退出
        if (!cmd) {
            serverLog(LL_WARNING,
                "Unknown command '%s' reading the append only file",
                (char*)argv[0]->ptr);
            exit(1);
        }
        //如果是多指令命令，则将游标指向过来
        if (cmd == server.multiCommand) valid_before_multi = valid_up_to;

        /* Run the command in the context of a fake client */
        //通过虚拟客户端执行命令
        fakeClient->cmd = fakeClient->lastcmd = cmd;
        if (fakeClient->flags & CLIENT_MULTI &&
            fakeClient->cmd->proc != execCommand)
        {
            //执行多指令
            queueMultiCommand(fakeClient);
        } else {
            //普通指令的执行
            cmd->proc(fakeClient);
        }

        /* The fake client should not have a reply */
        serverAssert(fakeClient->bufpos == 0 &&
                     listLength(fakeClient->reply) == 0);

        /* The fake client should never get blocked */
        serverAssert((fakeClient->flags & CLIENT_BLOCKED) == 0);

        /* Clean up. Command code may have changed argv/argc so we use the
         * argv/argc of the client instead of the local variables. */
        //因为可能会改变argv/argc，所以我们需要使用客户端的而不是局部变量的
        freeFakeClientArgv(fakeClient);
        fakeClient->cmd = NULL;
        if (server.aof_load_truncated) valid_up_to = ftello(fp);
        if (server.key_load_delay)
            debugDelay(server.key_load_delay);
    }

    /* This point can only be reached when EOF is reached without errors.
     * If the client is in the middle of a MULTI/EXEC, handle it as it was
     * a short read, even if technically the protocol is correct: we want
     * to remove the unprocessed tail and continue. */
    if (fakeClient->flags & CLIENT_MULTI) {
        serverLog(LL_WARNING,
            "Revert incomplete MULTI/EXEC transaction in AOF file");
        valid_up_to = valid_before_multi;
        goto uxeof;
    }
    //加载完毕，关闭资源，并返回ok
loaded_ok: /* DB loaded, cleanup and return C_OK to the caller. */
    fclose(fp);
    freeFakeClient(fakeClient);
    server.aof_state = old_aof_state;
    stopLoading(1);
    aofUpdateCurrentSize();
    server.aof_rewrite_base_size = server.aof_current_size;
    server.aof_fsync_offset = server.aof_current_size;
    return C_OK;
    //读取失败，关闭资源并退出
readerr: /* Read error. If feof(fp) is true, fall through to unexpected EOF. */
    if (!feof(fp)) {
        if (fakeClient) freeFakeClient(fakeClient); /* avoid valgrind warning */
        fclose(fp);
        serverLog(LL_WARNING,"Unrecoverable error reading the append only file: %s", strerror(errno));
        exit(1);
    }
//aof文件结尾不正确
uxeof: /* Unexpected AOF end of file. */
    if (server.aof_load_truncated) {
        serverLog(LL_WARNING,"!!! Warning: short read while loading the AOF file !!!");
        serverLog(LL_WARNING,"!!! Truncating the AOF at offset %llu !!!",
            (unsigned long long) valid_up_to);
        if (valid_up_to == -1 || truncate(filename,valid_up_to) == -1) {
            if (valid_up_to == -1) {
                serverLog(LL_WARNING,"Last valid command offset is invalid");
            } else {
                serverLog(LL_WARNING,"Error truncating the AOF file: %s",
                    strerror(errno));
            }
        } else {
            /* Make sure the AOF file descriptor points to the end of the
             * file after the truncate call. */
            if (server.aof_fd != -1 && lseek(server.aof_fd,0,SEEK_END) == -1) {
                serverLog(LL_WARNING,"Can't seek the end of the AOF file: %s",
                    strerror(errno));
            } else {
                serverLog(LL_WARNING,
                    "AOF loaded anyway because aof-load-truncated is enabled");
                goto loaded_ok;
            }
        }
    }
    if (fakeClient) freeFakeClient(fakeClient); /* avoid valgrind warning */
    fclose(fp);
    serverLog(LL_WARNING,"Unexpected end of file reading the append only file. You can: 1) Make a backup of your AOF file, then use ./redis-check-aof --fix <filename>. 2) Alternatively you can set the 'aof-load-truncated' configuration option to yes and restart the server.");
    exit(1);
//错误格式化
fmterr: /* Format error. */
    if (fakeClient) freeFakeClient(fakeClient); /* avoid valgrind warning */
    fclose(fp);
    serverLog(LL_WARNING,"Bad file format reading the append only file: make a backup of your AOF file, then use ./redis-check-aof --fix <filename>");
    exit(1);
}

/* ----------------------------------------------------------------------------
 * AOF rewrite
 * ------------------------------------------------------------------------- */
//AOF文件重写的相关代码
/* Delegate writing an object to writing a bulk string or bulk long long.
 * This is not placed in rio.c since that adds the server.h dependency. */
//将写入对象委托给写入批量字符串或批量长字符串，它没有放在 rio.c 中，因为这添加了 server.h 依赖项
int rioWriteBulkObject(rio *r, robj *obj) {
    /* Avoid using getDecodedObject to help copy-on-write (we are often
     * in a child process when this function is called). */
    //避免使用 getDecodedObject 来帮助实现写时复制（我们经常在子进程中调用此函数）
    if (obj->encoding == OBJ_ENCODING_INT) {
        return rioWriteBulkLongLong(r,(long)obj->ptr);
    } else if (sdsEncodedObject(obj)) {
        return rioWriteBulkString(r,obj->ptr,sdslen(obj->ptr));
    } else {
        serverPanic("Unknown string encoding");
    }
}

/* Emit the commands needed to rebuild a list object.
 * The function returns 0 on error, 1 on success. */
//重写list对象命令，就是根据现有的list数据来构建aof的新的list对象指令
int rewriteListObject(rio *r, robj *key, robj *o) {
    long long count = 0, items = listTypeLength(o);
    //只有快速链表才进行处理，其他的报错
    if (o->encoding == OBJ_ENCODING_QUICKLIST) {
        quicklist *list = o->ptr;
        //转换成链表的迭代器
        quicklistIter *li = quicklistGetIterator(list, AL_START_HEAD);
        quicklistEntry entry;
        //逐个迭代链表节点，构建aof的命令行
        while (quicklistNext(li,&entry)) {
            if (count == 0) {
                int cmd_items = (items > AOF_REWRITE_ITEMS_PER_CMD) ?
                    AOF_REWRITE_ITEMS_PER_CMD : items;
                if (!rioWriteBulkCount(r,'*',2+cmd_items) ||
                    !rioWriteBulkString(r,"RPUSH",5) ||
                    !rioWriteBulkObject(r,key)) 
                {
                    quicklistReleaseIterator(li);
                    return 0;
                }
            }

            if (entry.value) {
                if (!rioWriteBulkString(r,(char*)entry.value,entry.sz)) {
                    quicklistReleaseIterator(li);
                    return 0;
                }
            } else {
                if (!rioWriteBulkLongLong(r,entry.longval)) {
                    quicklistReleaseIterator(li);
                    return 0;
                }
            }
            //如果一个列表中包含超过64个元素，则重写为多个列表
            if (++count == AOF_REWRITE_ITEMS_PER_CMD) count = 0;
            items--;
        }
        //释放资源
        quicklistReleaseIterator(li);
    } else {
        serverPanic("Unknown list encoding");
    }
    return 1;
}

/* Emit the commands needed to rebuild a set object.
 * The function returns 0 on error, 1 on success. */
//重新构建set的命令，逻辑同上面list类似
int rewriteSetObject(rio *r, robj *key, robj *o) {
    long long count = 0, items = setTypeSize(o);
    //整形的集合
    if (o->encoding == OBJ_ENCODING_INTSET) {
        int ii = 0;
        int64_t llval;

        while(intsetGet(o->ptr,ii++,&llval)) {
            if (count == 0) {
                //如果超过64个元素，拆分成多个
                int cmd_items = (items > AOF_REWRITE_ITEMS_PER_CMD) ?
                    AOF_REWRITE_ITEMS_PER_CMD : items;

                if (!rioWriteBulkCount(r,'*',2+cmd_items) ||
                    !rioWriteBulkString(r,"SADD",4) ||
                    !rioWriteBulkObject(r,key)) 
                {
                    return 0;
                }
            }
            if (!rioWriteBulkLongLong(r,llval)) return 0;
            if (++count == AOF_REWRITE_ITEMS_PER_CMD) count = 0;
            items--;
        }
    } else if (o->encoding == OBJ_ENCODING_HT) {
        //普通的hash表
        dictIterator *di = dictGetIterator(o->ptr);
        dictEntry *de;

        while((de = dictNext(di)) != NULL) {
            sds ele = dictGetKey(de);
            if (count == 0) {
                int cmd_items = (items > AOF_REWRITE_ITEMS_PER_CMD) ?
                    AOF_REWRITE_ITEMS_PER_CMD : items;

                if (!rioWriteBulkCount(r,'*',2+cmd_items) ||
                    !rioWriteBulkString(r,"SADD",4) ||
                    !rioWriteBulkObject(r,key)) 
                {
                    dictReleaseIterator(di);
                    return 0;
                }
            }
            if (!rioWriteBulkString(r,ele,sdslen(ele))) {
                dictReleaseIterator(di);
                return 0;          
            }
            if (++count == AOF_REWRITE_ITEMS_PER_CMD) count = 0;
            items--;
        }
        dictReleaseIterator(di);
    } else {
        serverPanic("Unknown set encoding");
    }
    return 1;
}

/* Emit the commands needed to rebuild a sorted set object.
 * The function returns 0 on error, 1 on success. */
//根据数据重新构建zset的命令
int rewriteSortedSetObject(rio *r, robj *key, robj *o) {
    long long count = 0, items = zsetLength(o);
    //压缩链表
    if (o->encoding == OBJ_ENCODING_ZIPLIST) {
        unsigned char *zl = o->ptr;
        unsigned char *eptr, *sptr;
        unsigned char *vstr;
        unsigned int vlen;
        long long vll;
        double score;

        eptr = ziplistIndex(zl,0);
        serverAssert(eptr != NULL);
        sptr = ziplistNext(zl,eptr);
        serverAssert(sptr != NULL);

        while (eptr != NULL) {
            serverAssert(ziplistGet(eptr,&vstr,&vlen,&vll));
            score = zzlGetScore(sptr);

            if (count == 0) {
                int cmd_items = (items > AOF_REWRITE_ITEMS_PER_CMD) ?
                    AOF_REWRITE_ITEMS_PER_CMD : items;

                if (!rioWriteBulkCount(r,'*',2+cmd_items*2) ||
                    !rioWriteBulkString(r,"ZADD",4) ||
                    !rioWriteBulkObject(r,key)) 
                {
                    return 0;
                }
            }
            if (!rioWriteBulkDouble(r,score)) return 0;
            if (vstr != NULL) {
                if (!rioWriteBulkString(r,(char*)vstr,vlen)) return 0;
            } else {
                if (!rioWriteBulkLongLong(r,vll)) return 0;
            }
            zzlNext(zl,&eptr,&sptr);
            if (++count == AOF_REWRITE_ITEMS_PER_CMD) count = 0;
            items--;
        }
    } else if (o->encoding == OBJ_ENCODING_SKIPLIST) {
        //跳跃链表
        zset *zs = o->ptr;
        dictIterator *di = dictGetIterator(zs->dict);
        dictEntry *de;

        while((de = dictNext(di)) != NULL) {
            sds ele = dictGetKey(de);
            double *score = dictGetVal(de);

            if (count == 0) {
                int cmd_items = (items > AOF_REWRITE_ITEMS_PER_CMD) ?
                    AOF_REWRITE_ITEMS_PER_CMD : items;

                if (!rioWriteBulkCount(r,'*',2+cmd_items*2) ||
                    !rioWriteBulkString(r,"ZADD",4) ||
                    !rioWriteBulkObject(r,key)) 
                {
                    dictReleaseIterator(di);
                    return 0;
                }
            }
            if (!rioWriteBulkDouble(r,*score) ||
                !rioWriteBulkString(r,ele,sdslen(ele)))
            {
                dictReleaseIterator(di);
                return 0;
            }
            if (++count == AOF_REWRITE_ITEMS_PER_CMD) count = 0;
            items--;
        }
        dictReleaseIterator(di);
    } else {
        serverPanic("Unknown sorted zset encoding");
    }
    return 1;
}

/* Write either the key or the value of the currently selected item of a hash.
 * The 'hi' argument passes a valid Redis hash iterator.
 * The 'what' filed specifies if to write a key or a value and can be
 * either OBJ_HASH_KEY or OBJ_HASH_VALUE.
 *
 * The function returns 0 on error, non-zero on success. */
//写入哈希表中当前选定项的键或值
static int rioWriteHashIteratorCursor(rio *r, hashTypeIterator *hi, int what) {
    if (hi->encoding == OBJ_ENCODING_ZIPLIST) {
        unsigned char *vstr = NULL;
        unsigned int vlen = UINT_MAX;
        long long vll = LLONG_MAX;

        hashTypeCurrentFromZiplist(hi, what, &vstr, &vlen, &vll);
        if (vstr)
            return rioWriteBulkString(r, (char*)vstr, vlen);
        else
            return rioWriteBulkLongLong(r, vll);
    } else if (hi->encoding == OBJ_ENCODING_HT) {
        sds value = hashTypeCurrentFromHashTable(hi, what);
        return rioWriteBulkString(r, value, sdslen(value));
    }

    serverPanic("Unknown hash encoding");
    return 0;
}

/* Emit the commands needed to rebuild a hash object.
 * The function returns 0 on error, 1 on success. */
//重写hash的指令
int rewriteHashObject(rio *r, robj *key, robj *o) {
    hashTypeIterator *hi;
    long long count = 0, items = hashTypeLength(o);

    hi = hashTypeInitIterator(o);
    //逐个迭代,并且构建对应的指令
    while (hashTypeNext(hi) != C_ERR) {
        if (count == 0) {
            int cmd_items = (items > AOF_REWRITE_ITEMS_PER_CMD) ?
                AOF_REWRITE_ITEMS_PER_CMD : items;

            if (!rioWriteBulkCount(r,'*',2+cmd_items*2) ||
                !rioWriteBulkString(r,"HMSET",5) ||
                !rioWriteBulkObject(r,key)) 
            {
                hashTypeReleaseIterator(hi);
                return 0;
            }
        }

        if (!rioWriteHashIteratorCursor(r, hi, OBJ_HASH_KEY) ||
            !rioWriteHashIteratorCursor(r, hi, OBJ_HASH_VALUE))
        {
            hashTypeReleaseIterator(hi);
            return 0;           
        }
        if (++count == AOF_REWRITE_ITEMS_PER_CMD) count = 0;
        items--;
    }
    //释放迭代器的资源
    hashTypeReleaseIterator(hi);

    return 1;
}

/* Helper for rewriteStreamObject() that generates a bulk string into the
 * AOF representing the ID 'id'. */
//重写流对象，这个流没有了解过，这部分不进行解读
int rioWriteBulkStreamID(rio *r,streamID *id) {
    int retval;

    sds replyid = sdscatfmt(sdsempty(),"%U-%U",id->ms,id->seq);
    retval = rioWriteBulkString(r,replyid,sdslen(replyid));
    sdsfree(replyid);
    return retval;
}

/* Helper for rewriteStreamObject(): emit the XCLAIM needed in order to
 * add the message described by 'nack' having the id 'rawid', into the pending
 * list of the specified consumer. All this in the context of the specified
 * key and group. */
int rioWriteStreamPendingEntry(rio *r, robj *key, const char *groupname, size_t groupname_len, streamConsumer *consumer, unsigned char *rawid, streamNACK *nack) {
     /* XCLAIM <key> <group> <consumer> 0 <id> TIME <milliseconds-unix-time>
               RETRYCOUNT <count> JUSTID FORCE. */
    streamID id;
    streamDecodeID(rawid,&id);
    if (rioWriteBulkCount(r,'*',12) == 0) return 0;
    if (rioWriteBulkString(r,"XCLAIM",6) == 0) return 0;
    if (rioWriteBulkObject(r,key) == 0) return 0;
    if (rioWriteBulkString(r,groupname,groupname_len) == 0) return 0;
    if (rioWriteBulkString(r,consumer->name,sdslen(consumer->name)) == 0) return 0;
    if (rioWriteBulkString(r,"0",1) == 0) return 0;
    if (rioWriteBulkStreamID(r,&id) == 0) return 0;
    if (rioWriteBulkString(r,"TIME",4) == 0) return 0;
    if (rioWriteBulkLongLong(r,nack->delivery_time) == 0) return 0;
    if (rioWriteBulkString(r,"RETRYCOUNT",10) == 0) return 0;
    if (rioWriteBulkLongLong(r,nack->delivery_count) == 0) return 0;
    if (rioWriteBulkString(r,"JUSTID",6) == 0) return 0;
    if (rioWriteBulkString(r,"FORCE",5) == 0) return 0;
    return 1;
}

/* Helper for rewriteStreamObject(): emit the XGROUP CREATECONSUMER is
 * needed in order to create consumers that do not have any pending entries.
 * All this in the context of the specified key and group. */
int rioWriteStreamEmptyConsumer(rio *r, robj *key, const char *groupname, size_t groupname_len, streamConsumer *consumer) {
    /* XGROUP CREATECONSUMER <key> <group> <consumer> */
    if (rioWriteBulkCount(r,'*',5) == 0) return 0;
    if (rioWriteBulkString(r,"XGROUP",6) == 0) return 0;
    if (rioWriteBulkString(r,"CREATECONSUMER",14) == 0) return 0;
    if (rioWriteBulkObject(r,key) == 0) return 0;
    if (rioWriteBulkString(r,groupname,groupname_len) == 0) return 0;
    if (rioWriteBulkString(r,consumer->name,sdslen(consumer->name)) == 0) return 0;
    return 1;
}

/* Emit the commands needed to rebuild a stream object.
 * The function returns 0 on error, 1 on success. */
int rewriteStreamObject(rio *r, robj *key, robj *o) {
    stream *s = o->ptr;
    streamIterator si;
    streamIteratorStart(&si,s,NULL,NULL,0);
    streamID id;
    int64_t numfields;

    if (s->length) {
        /* Reconstruct the stream data using XADD commands. */
        while(streamIteratorGetID(&si,&id,&numfields)) {
            /* Emit a two elements array for each item. The first is
             * the ID, the second is an array of field-value pairs. */

            /* Emit the XADD <key> <id> ...fields... command. */
            if (!rioWriteBulkCount(r,'*',3+numfields*2) || 
                !rioWriteBulkString(r,"XADD",4) ||
                !rioWriteBulkObject(r,key) ||
                !rioWriteBulkStreamID(r,&id)) 
            {
                streamIteratorStop(&si);
                return 0;
            }
            while(numfields--) {
                unsigned char *field, *value;
                int64_t field_len, value_len;
                streamIteratorGetField(&si,&field,&value,&field_len,&value_len);
                if (!rioWriteBulkString(r,(char*)field,field_len) ||
                    !rioWriteBulkString(r,(char*)value,value_len)) 
                {
                    streamIteratorStop(&si);
                    return 0;                  
                }
            }
        }
    } else {
        /* Use the XADD MAXLEN 0 trick to generate an empty stream if
         * the key we are serializing is an empty string, which is possible
         * for the Stream type. */
        id.ms = 0; id.seq = 1; 
        if (!rioWriteBulkCount(r,'*',7) ||
            !rioWriteBulkString(r,"XADD",4) ||
            !rioWriteBulkObject(r,key) ||
            !rioWriteBulkString(r,"MAXLEN",6) ||
            !rioWriteBulkString(r,"0",1) ||
            !rioWriteBulkStreamID(r,&id) ||
            !rioWriteBulkString(r,"x",1) ||
            !rioWriteBulkString(r,"y",1))
        {
            streamIteratorStop(&si);
            return 0;     
        }
    }

    /* Append XSETID after XADD, make sure lastid is correct,
     * in case of XDEL lastid. */
    if (!rioWriteBulkCount(r,'*',3) ||
        !rioWriteBulkString(r,"XSETID",6) ||
        !rioWriteBulkObject(r,key) ||
        !rioWriteBulkStreamID(r,&s->last_id)) 
    {
        streamIteratorStop(&si);
        return 0; 
    }


    /* Create all the stream consumer groups. */
    if (s->cgroups) {
        raxIterator ri;
        raxStart(&ri,s->cgroups);
        raxSeek(&ri,"^",NULL,0);
        while(raxNext(&ri)) {
            streamCG *group = ri.data;
            /* Emit the XGROUP CREATE in order to create the group. */
            if (!rioWriteBulkCount(r,'*',5) ||
                !rioWriteBulkString(r,"XGROUP",6) ||
                !rioWriteBulkString(r,"CREATE",6) ||
                !rioWriteBulkObject(r,key) ||
                !rioWriteBulkString(r,(char*)ri.key,ri.key_len) ||
                !rioWriteBulkStreamID(r,&group->last_id)) 
            {
                raxStop(&ri);
                streamIteratorStop(&si);
                return 0;
            }

            /* Generate XCLAIMs for each consumer that happens to
             * have pending entries. Empty consumers would be generated with
             * XGROUP CREATECONSUMER. */
            raxIterator ri_cons;
            raxStart(&ri_cons,group->consumers);
            raxSeek(&ri_cons,"^",NULL,0);
            while(raxNext(&ri_cons)) {
                streamConsumer *consumer = ri_cons.data;
                /* If there are no pending entries, just emit XGROUP CREATECONSUMER */
                if (raxSize(consumer->pel) == 0) {
                    if (rioWriteStreamEmptyConsumer(r,key,(char*)ri.key,
                                                    ri.key_len,consumer) == 0)
                    {
                        raxStop(&ri_cons);
                        raxStop(&ri);
                        streamIteratorStop(&si);
                        return 0;
                    }
                    continue;
                }
                /* For the current consumer, iterate all the PEL entries
                 * to emit the XCLAIM protocol. */
                raxIterator ri_pel;
                raxStart(&ri_pel,consumer->pel);
                raxSeek(&ri_pel,"^",NULL,0);
                while(raxNext(&ri_pel)) {
                    streamNACK *nack = ri_pel.data;
                    if (rioWriteStreamPendingEntry(r,key,(char*)ri.key,
                                                   ri.key_len,consumer,
                                                   ri_pel.key,nack) == 0)
                    {
                        raxStop(&ri_pel);
                        raxStop(&ri_cons);
                        raxStop(&ri);
                        streamIteratorStop(&si);
                        return 0;
                    }
                }
                raxStop(&ri_pel);
            }
            raxStop(&ri_cons);
        }
        raxStop(&ri);
    }

    streamIteratorStop(&si);
    return 1;
}

/* Call the module type callback in order to rewrite a data type
 * that is exported by a module and is not handled by Redis itself.
 * The function returns 0 on error, 1 on success. */
int rewriteModuleObject(rio *r, robj *key, robj *o) {
    RedisModuleIO io;
    moduleValue *mv = o->ptr;
    moduleType *mt = mv->type;
    moduleInitIOContext(io,mt,r,key);
    mt->aof_rewrite(&io,key,mv->value);
    if (io.ctx) {
        moduleFreeContext(io.ctx);
        zfree(io.ctx);
    }
    return io.error ? 0 : 1;
}

/* This function is called by the child rewriting the AOF file to read
 * the difference accumulated from the parent into a buffer, that is
 * concatenated at the end of the rewrite. */
//AOF子进程调用，用于从管道中读取父进程对buffer的修改,返回变更的长度
ssize_t aofReadDiffFromParent(void) {
    char buf[65536]; /* Default pipe buffer size on most Linux systems. */
    ssize_t nread, total = 0;

    while ((nread =
            read(server.aof_pipe_read_data_from_parent,buf,sizeof(buf))) > 0) {
        server.aof_child_diff = sdscatlen(server.aof_child_diff,buf,nread);
        total += nread;
    }
    return total;
}
//将当前数据库的所有键值对和相关操作写入到新的 AOF 文件中
int rewriteAppendOnlyFileRio(rio *aof) {
    dictIterator *di = NULL;
    dictEntry *de;
    size_t processed = 0;
    int j;
    long key_count = 0;
    long long updated_time = 0;
    //逐一遍历每一个db
    for (j = 0; j < server.dbnum; j++) {
        char selectcmd[] = "*2\r\n$6\r\nSELECT\r\n";
        redisDb *db = server.db+j;
        dict *d = db->dict;
        if (dictSize(d) == 0) continue;
        di = dictGetSafeIterator(d);

        /* SELECT the new DB */
        //把db选择命令写入到aof中
        if (rioWrite(aof,selectcmd,sizeof(selectcmd)-1) == 0) goto werr;
        if (rioWriteBulkLongLong(aof,j) == 0) goto werr;

        /* Iterate this DB writing every entry */
        //迭代db中每一个entry
        while((de = dictNext(di)) != NULL) {
            sds keystr;
            robj key, *o;
            long long expiretime;
            //获取key
            keystr = dictGetKey(de);
            //获取value
            o = dictGetVal(de);
            //初始化字符串对象
            initStaticStringObject(key,keystr);
            //获取过期时间
            expiretime = getExpire(db,&key);

            /* Save the key and associated value */
            //保存key和对应的值
            //如果是字符串对象
            if (o->type == OBJ_STRING) {
                /* Emit a SET command */
                //字符串对象指令为SET命令
                char cmd[]="*3\r\n$3\r\nSET\r\n";
                if (rioWrite(aof,cmd,sizeof(cmd)-1) == 0) goto werr;
                /* Key and value */
                //写入key
                if (rioWriteBulkObject(aof,&key) == 0) goto werr;
                //写入value
                if (rioWriteBulkObject(aof,o) == 0) goto werr;
            } else if (o->type == OBJ_LIST) {
                //如果是list对象，则采用对应的方式写入
                if (rewriteListObject(aof,&key,o) == 0) goto werr;
            } else if (o->type == OBJ_SET) {
                //set对象
                if (rewriteSetObject(aof,&key,o) == 0) goto werr;
            } else if (o->type == OBJ_ZSET) {
                //zset对象
                if (rewriteSortedSetObject(aof,&key,o) == 0) goto werr;
            } else if (o->type == OBJ_HASH) {
                //hash表对象
                if (rewriteHashObject(aof,&key,o) == 0) goto werr;
            } else if (o->type == OBJ_STREAM) {
                //流
                if (rewriteStreamObject(aof,&key,o) == 0) goto werr;
            } else if (o->type == OBJ_MODULE) {
                //模块
                if (rewriteModuleObject(aof,&key,o) == 0) goto werr;
            } else {
                //其他的做处理
                serverPanic("Unknown object type");
            }
            /* Save the expire time */
            //保存其有效期
            if (expiretime != -1) {
                char cmd[]="*3\r\n$9\r\nPEXPIREAT\r\n";
                //写入指令
                if (rioWrite(aof,cmd,sizeof(cmd)-1) == 0) goto werr;
                //写入key
                if (rioWriteBulkObject(aof,&key) == 0) goto werr;
                //写入有效时间
                if (rioWriteBulkLongLong(aof,expiretime) == 0) goto werr;
            }
            /* Read some diff from the parent process from time to time. */
            //从父进程获取到的变更数据，这里进行处理
            if (aof->processed_bytes > processed+AOF_READ_DIFF_INTERVAL_BYTES) {
                processed = aof->processed_bytes;
                aofReadDiffFromParent();
            }

            /* Update info every 1 second (approximately).
             * in order to avoid calling mstime() on each iteration, we will
             * check the diff every 1024 keys */
            /* 每 1 秒更新一次信息（大约）。
             * 为了避免在每次迭代时调用 mstime()，我们将
             * 每 1024 个键检查一次差异*/
            if ((key_count++ & 1023) == 0) {
                long long now = mstime();
                if (now - updated_time >= 1000) {
                    sendChildInfo(CHILD_INFO_TYPE_CURRENT_INFO, key_count, "AOF rewrite");
                    updated_time = now;
                }
            }
        }
        //释放资源
        dictReleaseIterator(di);
        di = NULL;
    }
    return C_OK;

werr:
    if (di) dictReleaseIterator(di);
    return C_ERR;
}

/* Write a sequence of commands able to fully rebuild the dataset into
 * "filename". Used both by REWRITEAOF and BGREWRITEAOF.
 *
 * In order to minimize the number of commands needed in the rewritten
 * log Redis uses variadic commands when possible, such as RPUSH, SADD
 * and ZADD. However at max AOF_REWRITE_ITEMS_PER_CMD items per time
 * are inserted using a single command.
 * 为了尽量减少重写日志所需的命令数量，Redis 会尽可能使用可变命令，
 * 例如 RPUSH、SADD 和 ZADD。但是，每次最多只能使用一个命令插入 AOF_REWRITE_ITEMS_PER_CMD 个项目。
 */
//编写一系列能够将数据集完全重建为“filename”的命令。REWRITEAOF 和 BGREWRITEAOF 均使用此命令。
int rewriteAppendOnlyFile(char *filename) {
    rio aof;
    FILE *fp = NULL;
    char tmpfile[256];
    char byte;

    /* Note that we have to use a different temp name here compared to the
     * one used by rewriteAppendOnlyFileBackground() function. */
    /*请注意，与 rewriteAppendOnlyFileBackground() 函数使用的名称相比，我们必须在此处使用不同的临时名称*/
    snprintf(tmpfile,256,"temp-rewriteaof-%d.aof", (int) getpid());
    fp = fopen(tmpfile,"w");
    if (!fp) {
        serverLog(LL_WARNING, "Opening the temp file for AOF rewrite in rewriteAppendOnlyFile(): %s", strerror(errno));
        return C_ERR;
    }

    server.aof_child_diff = sdsempty();
    rioInitWithFile(&aof,fp);
    //开启增量同步？
    if (server.aof_rewrite_incremental_fsync)
        rioSetAutoSync(&aof,REDIS_AUTOSYNC_BYTES);
    //开启保存rdb
    startSaving(RDBFLAGS_AOF_PREAMBLE);
    //在 AOF 重写中使用 RDB 前导
    if (server.aof_use_rdb_preamble) {
        int error;
        //开启了前导的话，则先保存rdb
        if (rdbSaveRio(&aof,&error,RDBFLAGS_AOF_PREAMBLE,NULL) == C_ERR) {
            errno = error;
            goto werr;
        }
    } else {
        //没有混合保存持久化的话，则直接重写aof
        if (rewriteAppendOnlyFileRio(&aof) == C_ERR) goto werr;
    }

    /* Do an initial slow fsync here while the parent is still sending
     * data, in order to make the next final fsync faster. */
    //如果父进程在持续发送数据，则进行慢同步，以方便后门同步更快速
    if (fflush(fp) == EOF) goto werr;
    if (fsync(fileno(fp)) == -1) goto werr;

    /* Read again a few times to get more data from the parent.
     * We can't read forever (the server may receive data from clients
     * faster than it is able to send data to the child), so we try to read
     * some more data in a loop as soon as there is a good chance more data
     * will come. If it looks like we are wasting time, we abort (this
     * happens after 20 ms without new data).
     * 重复读取几次，以便从父进程获取更多数据。
     * 我们无法一直读取（服务器从客户端接收数据的速度可能比它向子进程发送数据的速度更快），
     * 因此，只要有更多数据出现的机会，我们就会尝试在循环中读取更多数据。
     * 如果看起来我们在浪费时间，我们会中止（这种情况会在 20 毫秒后发生，并且不会有新数据）。
     */
    int nodata = 0;
    mstime_t start = mstime();
    while(mstime()-start < 1000 && nodata < 20) {
        if (aeWait(server.aof_pipe_read_data_from_parent, AE_READABLE, 1) <= 0)
        {
            nodata++;
            continue;
        }
        nodata = 0; /* Start counting from zero, we stop on N *contiguous*
                       timeouts. */
        aofReadDiffFromParent();
    }

    /* Ask the master to stop sending diffs. */
    //通知父进程停止发送diffs
    if (write(server.aof_pipe_write_ack_to_parent,"!",1) != 1) goto werr;
    if (anetNonBlock(NULL,server.aof_pipe_read_ack_from_parent) != ANET_OK)
        goto werr;
    /* We read the ACK from the server using a 5 seconds timeout. Normally
     * it should reply ASAP, but just in case we lose its reply, we are sure
     * the child will eventually get terminated.
     * 我们使用 5 秒超时从服务器读取 ACK。通常情况下
     * 它应该尽快回复，但万一我们丢失了它的回复，我们确信
     * 子进程最终会被终止
     */
    if (syncRead(server.aof_pipe_read_ack_from_parent,&byte,1,5000) != 1 ||
        byte != '!') goto werr;
    serverLog(LL_NOTICE,"Parent agreed to stop sending diffs. Finalizing AOF...");

    /* Read the final diff if any. */
    //最后读取差异，也就是变更的内容
    aofReadDiffFromParent();

    /* Write the received diff to the file. */
    //写入接收到的差异数据/变更内容
    serverLog(LL_NOTICE,
        "Concatenating %.2f MB of AOF diff received from parent.",
        (double) sdslen(server.aof_child_diff) / (1024*1024));

    /* Now we write the entire AOF buffer we received from the parent
     * via the pipe during the life of this fork child.
     * once a second, we'll take a break and send updated COW info to the parent
     * 现在，我们写入从父进程收到的整个 AOF 缓冲区
     * 通过管道，在此 fork 子进程的生命周期内。
     * 每秒一次，我们会暂停一下，并将更新后的 COW 信息发送给父进程
     */
    size_t bytes_to_write = sdslen(server.aof_child_diff);
    const char *buf = server.aof_child_diff;
    long long cow_updated_time = mstime();
    long long key_count = dbTotalServerKeyCount();
    while (bytes_to_write) {
        /* We write the AOF buffer in chunk of 8MB so that we can check the time in between them */
        //AOF缓冲区以8MB的大小来写入
        size_t chunk_size = bytes_to_write < (8<<20) ? bytes_to_write : (8<<20);

        if (rioWrite(&aof,buf,chunk_size) == 0)
            goto werr;

        bytes_to_write -= chunk_size;
        buf += chunk_size;

        /* Update COW info */
        //更新COW（写时复制）的信息
        long long now = mstime();
        if (now - cow_updated_time >= 1000) {
            sendChildInfo(CHILD_INFO_TYPE_CURRENT_INFO, key_count, "AOF rewrite");
            cow_updated_time = now;
        }
    }

    /* Make sure data will not remain on the OS's output buffers */
    //确保数据不会保留在操作系统的缓冲内
    if (fflush(fp)) goto werr;
    if (fsync(fileno(fp))) goto werr;
    if (fclose(fp)) { fp = NULL; goto werr; }
    fp = NULL;

    /* Use RENAME to make sure the DB file is changed atomically only
     * if the generate DB file is ok. */
    //原子化的重命名文件,完成后释放临时文件，将save持久化结束
    if (rename(tmpfile,filename) == -1) {
        serverLog(LL_WARNING,"Error moving temp append only file on the final destination: %s", strerror(errno));
        unlink(tmpfile);
        stopSaving(0);
        return C_ERR;
    }
    serverLog(LL_NOTICE,"SYNC append only file rewrite performed");
    stopSaving(1);
    return C_OK;

werr:
    serverLog(LL_WARNING,"Write error writing append only file on disk: %s", strerror(errno));
    if (fp) fclose(fp);
    unlink(tmpfile);
    stopSaving(0);
    return C_ERR;
}

/* ----------------------------------------------------------------------------
 * AOF rewrite pipes for IPC
 * -------------------------------------------------------------------------- */

/* This event handler is called when the AOF rewriting child sends us a
 * single '!' char to signal we should stop sending buffer diffs. The
 * parent sends a '!' as well to acknowledge. */
//AOF子进程对父进程的回调
void aofChildPipeReadable(aeEventLoop *el, int fd, void *privdata, int mask) {
    char byte;
    UNUSED(el);
    UNUSED(privdata);
    UNUSED(mask);

    if (read(fd,&byte,1) == 1 && byte == '!') {
        serverLog(LL_NOTICE,"AOF rewrite child asks to stop sending diffs.");
        server.aof_stop_sending_diff = 1;
        if (write(server.aof_pipe_write_ack_to_child,"!",1) != 1) {
            /* If we can't send the ack, inform the user, but don't try again
             * since in the other side the children will use a timeout if the
             * kernel can't buffer our write, or, the children was
             * terminated. */
            serverLog(LL_WARNING,"Can't send ACK to AOF child: %s",
                strerror(errno));
        }
    }
    /* Remove the handler since this can be called only one time during a
     * rewrite. */
    aeDeleteFileEvent(server.el,server.aof_pipe_read_ack_from_child,AE_READABLE);
}

/* Create the pipes used for parent - child process IPC during rewrite.
 * We have a data pipe used to send AOF incremental diffs to the child,
 * and two other pipes used by the children to signal it finished with
 * the rewrite so no more data should be written, and another for the
 * parent to acknowledge it understood this new condition. */
//创建父子进程通信的管道
int aofCreatePipes(void) {
    int fds[6] = {-1, -1, -1, -1, -1, -1};
    int j;

    if (pipe(fds) == -1) goto error; /* parent -> children data. */
    if (pipe(fds+2) == -1) goto error; /* children -> parent ack. */
    if (pipe(fds+4) == -1) goto error; /* parent -> children ack. */
    /* Parent -> children data is non blocking. */
    if (anetNonBlock(NULL,fds[0]) != ANET_OK) goto error;
    if (anetNonBlock(NULL,fds[1]) != ANET_OK) goto error;
    if (aeCreateFileEvent(server.el, fds[2], AE_READABLE, aofChildPipeReadable, NULL) == AE_ERR) goto error;

    server.aof_pipe_write_data_to_child = fds[1];
    server.aof_pipe_read_data_from_parent = fds[0];
    server.aof_pipe_write_ack_to_parent = fds[3];
    server.aof_pipe_read_ack_from_child = fds[2];
    server.aof_pipe_write_ack_to_child = fds[5];
    server.aof_pipe_read_ack_from_parent = fds[4];
    server.aof_stop_sending_diff = 0;
    return C_OK;

error:
    serverLog(LL_WARNING,"Error opening /setting AOF rewrite IPC pipes: %s",
        strerror(errno));
    for (j = 0; j < 6; j++) if(fds[j] != -1) close(fds[j]);
    return C_ERR;
}
//关闭父子进程通信管道
void aofClosePipes(void) {
    aeDeleteFileEvent(server.el,server.aof_pipe_read_ack_from_child,AE_READABLE);
    aeDeleteFileEvent(server.el,server.aof_pipe_write_data_to_child,AE_WRITABLE);
    close(server.aof_pipe_write_data_to_child);
    close(server.aof_pipe_read_data_from_parent);
    close(server.aof_pipe_write_ack_to_parent);
    close(server.aof_pipe_read_ack_from_child);
    close(server.aof_pipe_write_ack_to_child);
    close(server.aof_pipe_read_ack_from_parent);
}

/* ----------------------------------------------------------------------------
 * AOF background rewrite
 * ------------------------------------------------------------------------- */

/* This is how rewriting of the append only file in background works:
 * 后台AOF重写的工作流
 *
 * 1) The user calls BGREWRITEAOF
 * 1）用户使用BGREWRITEAOF指令
 * 2) Redis calls this function, that forks():
 * 2）redis调用方法，创建子进程
 *    2a) the child rewrite the append only file in a temp file.
 *    2a）子进程重写aof到临时文件
 *    2b) the parent accumulates differences in server.aof_rewrite_buf.
 *    2b）父进程将重写过程中的变更写入aof重写缓冲区
 * 3) When the child finished '2a' exists.
 * 3）子进程完成退出
 * 4) The parent will trap the exit code, if it's OK, will append the
 *    data accumulated into server.aof_rewrite_buf into the temp file, and
 *    finally will rename(2) the temp file in the actual file name.
 *    The the new file is reopened as the new append only file. Profit!
 * 4)父进程接收到退出结果，如果重写成功，则将后续的aof变更缓冲期写入临时文件，并且重命名临时文件为正式的aof文件名，
 * 这个就成为新的aof文件，完成!
 */
//后台运行aof重写
int rewriteAppendOnlyFileBackground(void) {
    pid_t childpid;
    //检查是否有正在运行的aof重写子进程。如果没有，则创建一个
    if (hasActiveChildProcess()) return C_ERR;
    if (aofCreatePipes() != C_OK) return C_ERR;
    //创建子进程来重写aof文件
    if ((childpid = redisFork(CHILD_TYPE_AOF)) == 0) {
        char tmpfile[256];

        /* Child */
        redisSetProcTitle("redis-aof-rewrite");
        redisSetCpuAffinity(server.aof_rewrite_cpulist);
        snprintf(tmpfile,256,"temp-rewriteaof-bg-%d.aof", (int) getpid());
        if (rewriteAppendOnlyFile(tmpfile) == C_OK) {
            sendChildCowInfo(CHILD_INFO_TYPE_AOF_COW_SIZE, "AOF rewrite");
            exitFromChild(0);
        } else {
            exitFromChild(1);
        }
    } else {
        /* Parent */
        if (childpid == -1) {
            serverLog(LL_WARNING,
                "Can't rewrite append only file in background: fork: %s",
                strerror(errno));
            aofClosePipes();
            return C_ERR;
        }
        serverLog(LL_NOTICE,
            "Background append only file rewriting started by pid %ld",(long) childpid);
        server.aof_rewrite_scheduled = 0;
        server.aof_rewrite_time_start = time(NULL);

        /* We set appendseldb to -1 in order to force the next call to the
         * feedAppendOnlyFile() to issue a SELECT command, so the differences
         * accumulated by the parent into server.aof_rewrite_buf will start
         * with a SELECT statement and it will be safe to merge. */
        /*我们将 appendseldb 设置为 -1，以强制下一次调用 feedAppendOnlyFile() 发出 SELECT 命令，
         *这样父级在 server.aof_rewrite_buf 中积累的差异将以 SELECT 语句开始，并且可以安全地进行合并*/
        server.aof_selected_db = -1;
        replicationScriptCacheFlush();
        return C_OK;
    }
    return C_OK; /* unreached */
}
//接受到bgrewriteaof指令
void bgrewriteaofCommand(client *c) {
    if (server.child_type == CHILD_TYPE_AOF) {
        addReplyError(c,"Background append only file rewriting already in progress");
    } else if (hasActiveChildProcess()) {
        server.aof_rewrite_scheduled = 1;
        addReplyStatus(c,"Background append only file rewriting scheduled");
    } else if (rewriteAppendOnlyFileBackground() == C_OK) {
        addReplyStatus(c,"Background append only file rewriting started");
    } else {
        addReplyError(c,"Can't execute an AOF background rewriting. "
                        "Please check the server logs for more information.");
    }
}
//移除AOF临时文件
void aofRemoveTempFile(pid_t childpid) {
    char tmpfile[256];

    snprintf(tmpfile,256,"temp-rewriteaof-bg-%d.aof", (int) childpid);
    bg_unlink(tmpfile);

    snprintf(tmpfile,256,"temp-rewriteaof-%d.aof", (int) childpid);
    bg_unlink(tmpfile);
}

/* Update the server.aof_current_size field explicitly using stat(2)
 * to check the size of the file. This is useful after a rewrite or after
 * a restart, normally the size is updated just adding the write length
 * to the current length, that is much faster. */
//更新aof文件大小
void aofUpdateCurrentSize(void) {
    struct redis_stat sb;
    mstime_t latency;

    latencyStartMonitor(latency);
    if (redis_fstat(server.aof_fd,&sb) == -1) {
        serverLog(LL_WARNING,"Unable to obtain the AOF file length. stat: %s",
            strerror(errno));
    } else {
        server.aof_current_size = sb.st_size;
    }
    latencyEndMonitor(latency);
    latencyAddSampleIfNeeded("aof-fstat",latency);
}

/* A background append only file rewriting (BGREWRITEAOF) terminated its work.
 * Handle this. */
//aof重写完毕后的处理器
void backgroundRewriteDoneHandler(int exitcode, int bysignal) {
    if (!bysignal && exitcode == 0) {
        int newfd, oldfd;
        char tmpfile[256];
        long long now = ustime();
        mstime_t latency;

        serverLog(LL_NOTICE,
            "Background AOF rewrite terminated with success");

        /* Flush the differences accumulated by the parent to the
         * rewritten AOF. */
        //将后续的数据刷到aof的重写文件中
        latencyStartMonitor(latency);
        snprintf(tmpfile,256,"temp-rewriteaof-bg-%d.aof",
            (int)server.child_pid);
        newfd = open(tmpfile,O_WRONLY|O_APPEND);
        if (newfd == -1) {
            serverLog(LL_WARNING,
                "Unable to open the temporary AOF produced by the child: %s", strerror(errno));
            goto cleanup;
        }

        if (aofRewriteBufferWrite(newfd) == -1) {
            serverLog(LL_WARNING,
                "Error trying to flush the parent diff to the rewritten AOF: %s", strerror(errno));
            close(newfd);
            goto cleanup;
        }
        latencyEndMonitor(latency);
        latencyAddSampleIfNeeded("aof-rewrite-diff-write",latency);
  
        if (server.aof_fsync == AOF_FSYNC_EVERYSEC) {
            //将后学的每秒刷写的文件，变成新的文件
            aof_background_fsync(newfd);
        } else if (server.aof_fsync == AOF_FSYNC_ALWAYS) {
            latencyStartMonitor(latency);
            //修改当前写入的文件为最新的文件，如果修改失败，进入失败处理
            if (redis_fsync(newfd) == -1) {
                serverLog(LL_WARNING,
                    "Error trying to fsync the parent diff to the rewritten AOF: %s", strerror(errno));
                close(newfd);
                goto cleanup;
            }
            latencyEndMonitor(latency);
            latencyAddSampleIfNeeded("aof-rewrite-done-fsync",latency);
        }

        serverLog(LL_NOTICE,
            "Residual parent diff successfully flushed to the rewritten AOF (%.2f MB)", (double) aofRewriteBufferSize() / (1024*1024));

        /* The only remaining thing to do is to rename the temporary file to
         * the configured file and switch the file descriptor used to do AOF
         * writes. We don't want close(2) or rename(2) calls to block the
         * server on old file deletion.
         *
         * There are two possible scenarios:
         *
         * 1) AOF is DISABLED and this was a one time rewrite. The temporary
         * file will be renamed to the configured file. When this file already
         * exists, it will be unlinked, which may block the server.
         *
         * 2) AOF is ENABLED and the rewritten AOF will immediately start
         * receiving writes. After the temporary file is renamed to the
         * configured file, the original AOF file descriptor will be closed.
         * Since this will be the last reference to that file, closing it
         * causes the underlying file to be unlinked, which may block the
         * server.
         *
         * To mitigate the blocking effect of the unlink operation (either
         * caused by rename(2) in scenario 1, or by close(2) in scenario 2), we
         * use a background thread to take care of this. First, we
         * make scenario 1 identical to scenario 2 by opening the target file
         * when it exists. The unlink operation after the rename(2) will then
         * be executed upon calling close(2) for its descriptor. Everything to
         * guarantee atomicity for this switch has already happened by then, so
         * we don't care what the outcome or duration of that close operation
         * is, as long as the file descriptor is released again. */
        /*
         * 剩下要做的就是将临时文件重命名为已配置的文件，并切换用于执行 AOF
         * 写入的文件描述符。我们不希望 close(2) 或 rename(2) 调用在删除旧文件时阻塞服务器。
         *
         * 有两种可能的情况：
         *
         * 1) AOF 已禁用，这是一次性重写。临时文件将重命名为已配置的文件。当此文件已存在时，它将被取消链接，这可能会阻塞服务器。
         *
         * 2) AOF 已启用，重写的 AOF 将立即开始接收写入。将临时文件重命名为已配置的文件后，原始 AOF 文件描述符将被关闭。
         * 由于这将是对该文件的最后一次引用，因此关闭它会导致底层文件被取消链接，这可能会阻塞服务器。
         *
         * 为了减轻取消链接操作的阻塞效应（无论是由场景 1 中的 rename(2) 引起的，还是由场景 2 中的 close(2) 引起的），我们
         * 使用后台线程来处理这个问题。首先，我们
         * 通过在目标文件存在时打开它，使场景 1 与场景 2 相同。然后，在调用其描述符的 close(2) 时，将执行 rename(2) 之后的取消链接操作。
         * 到那时，保证此切换原子性的所有操作都已经发生，因此我们不关心该关闭操作的结果或持续时间，只要再次释放文件描述符即可。
         */
        if (server.aof_fd == -1) {
            /* AOF disabled */

            /* Don't care if this fails: oldfd will be -1 and we handle that.
             * One notable case of -1 return is if the old file does
             * not exist. */
            oldfd = open(server.aof_filename,O_RDONLY|O_NONBLOCK);
        } else {
            /* AOF enabled */
            oldfd = -1; /* We'll set this to the current AOF filedes later. */
        }

        /* Rename the temporary file. This will not unlink the target file if
         * it exists, because we reference it with "oldfd". */
        latencyStartMonitor(latency);
        if (rename(tmpfile,server.aof_filename) == -1) {
            serverLog(LL_WARNING,
                "Error trying to rename the temporary AOF file %s into %s: %s",
                tmpfile,
                server.aof_filename,
                strerror(errno));
            close(newfd);
            if (oldfd != -1) close(oldfd);
            goto cleanup;
        }
        latencyEndMonitor(latency);
        latencyAddSampleIfNeeded("aof-rename",latency);

        if (server.aof_fd == -1) {
            /* AOF disabled, we don't need to set the AOF file descriptor
             * to this new file, so we can close it. */
            close(newfd);
        } else {
            /* AOF enabled, replace the old fd with the new one. */
            oldfd = server.aof_fd;
            server.aof_fd = newfd;
            server.aof_selected_db = -1; /* Make sure SELECT is re-issued */
            aofUpdateCurrentSize();
            server.aof_rewrite_base_size = server.aof_current_size;
            server.aof_fsync_offset = server.aof_current_size;
            server.aof_last_fsync = server.unixtime;

            /* Clear regular AOF buffer since its contents was just written to
             * the new AOF from the background rewrite buffer. */
            sdsfree(server.aof_buf);
            server.aof_buf = sdsempty();
        }

        server.aof_lastbgrewrite_status = C_OK;

        serverLog(LL_NOTICE, "Background AOF rewrite finished successfully");
        /* Change state from WAIT_REWRITE to ON if needed */
        if (server.aof_state == AOF_WAIT_REWRITE)
            server.aof_state = AOF_ON;

        /* Asynchronously close the overwritten AOF. */
        if (oldfd != -1) bioCreateCloseJob(oldfd);

        serverLog(LL_VERBOSE,
            "Background AOF rewrite signal handler took %lldus", ustime()-now);
    } else if (!bysignal && exitcode != 0) {
        server.aof_lastbgrewrite_status = C_ERR;

        serverLog(LL_WARNING,
            "Background AOF rewrite terminated with error");
    } else {
        /* SIGUSR1 is whitelisted, so we have a way to kill a child without
         * triggering an error condition. */
        if (bysignal != SIGUSR1)
            server.aof_lastbgrewrite_status = C_ERR;

        serverLog(LL_WARNING,
            "Background AOF rewrite terminated by signal %d", bysignal);
    }
//关闭释放资源
cleanup:
    aofClosePipes();
    aofRewriteBufferReset();
    aofRemoveTempFile(server.child_pid);
    server.aof_rewrite_time_last = time(NULL)-server.aof_rewrite_time_start;
    server.aof_rewrite_time_start = -1;
    /* Schedule a new rewrite if we are waiting for it to switch the AOF ON. */
    if (server.aof_state == AOF_WAIT_REWRITE)
        server.aof_rewrite_scheduled = 1;
}
