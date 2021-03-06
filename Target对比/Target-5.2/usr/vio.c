/*
 * SCSI object storage device command processing
 *
 * Copyright (C) 2016 chaolu zhang <finals@126.com>
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License as
 * published by the Free Software Foundation, version 2 of the
 * License.
 *
 * This program is distributed in the hope that it will be useful, but
 * WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA
 * 02110-1301 USA
 */

#include <stdio.h>
#include <unistd.h>
#include <errno.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <stdlib.h>

#include "log.h"
#include "vio.h"

const int interval = 5;
const int retry_times = 5;

int readn(int fd, void *buf, int len) {
    int readed = 0;
    int ret;

    while (readed < len) {
        ret = read(fd, buf + readed, len - readed);
        if (ret < 0) {
            if (errno == EINTR){
                ret = 0;
            }
            else{
                //eprintf("read return %d", ret);
                return ret;
            }
        }else if (ret == 0){
            break; /*eof*/
            }
        readed += ret;
    }
    return readed;
}

int writen(int fd, void *buf, int len) {
    int wrote = 0;
    int ret;

    while (wrote < len) {
        ret = write(fd, buf + wrote, len - wrote);
        if (ret < 0) {
            if (errno == EINTR){
                ret = 0;
            }
            else{
                //eprintf("read return %d", ret);
                return ret;
            }
        }
        wrote += ret;
     }
     return wrote;
}

int send_msg(int fd, uint32_t idx) {
    int n = 0;
	//eprintf("send_msg type:%u offset:%ld length:%u", msg->Type, (long int)msg->Offset, msg->DataLength);

    n = writen(fd, &idx, sizeof(idx));
    if (n != sizeof(idx)) {
        Log_error("[send_msg] writen return: %d\n", n);
        return -EINVAL;
    }

    return 0;
}

// Caller need to release msg->Data
int receive_msg(int fd, uint32_t *idx) {
	int n;
	//eprintf("receive_msg type:%u offset:%ld length:%u", msg->Type, (long int)msg->Offset, msg->DataLength);

	n = readn(fd, idx, sizeof(*idx));
    if (n != sizeof(*idx)) {
        Log_error("[receive_msg] readn return: %d\n", n);
		return -EINVAL;
    }

	return 0;
}

int send_request(struct vio_connection *conn, uint32_t idx) {
    int rc = 0;
	//eprintf("send_request");

    pthread_mutex_lock(&conn->mutex);
    rc = send_msg(conn->fd, idx);
    pthread_mutex_unlock(&conn->mutex);
    return rc;
}

int receive_response(struct vio_connection *conn, uint32_t *idx) {
    int rc = 0;

    rc = receive_msg(conn->fd, idx);
    return rc;
}

void* response_process(void *arg) {
    struct vio_connection *conn = arg;
    struct cmnd *cmd;
    int ret = 0;
    uint32_t idx;
    //Log_debug("000");
	//ret = receive_response(conn, resp);
    while (1) {
        ret = receive_response(conn, &idx);
        if (ret != 0) {
            Log_error("[receive_response] idx:%u ret:%d\n", idx, ret);
            goto exit;
        }
		//Log_debug("111 Receive idx:%u", idx);
        cmd = get(conn->msg_buffer, idx);
        if (NULL == cmd) {
            Log_error("[receive_response] ring get idx:%u cmd==NULL\n", idx);
            ret = -1;
            break;
        }

		//Log_debug("222 rtype: %d", cmd->rtype);
        switch (cmd->rtype) {
        case TypeOK:
            break;
        case TypeError:
            Log_error("[receive_response] Receive error for response %d of seq %lu\n",
                                        cmd->rtype, (unsigned long)cmd->seq);
            /* fall through so we can response to caller */
            break;
        case TypeEOF:
            Log_error("[receive_response] Receive eof for response %d of seq %lu\n",
                                        cmd->rtype, (unsigned long)cmd->seq);
            break;
        case TypeTimeout:
            Log_error("[receive_response] Receive timeout for response %d of seq %lu\n",
                                        cmd->rtype, (unsigned long)cmd->seq);
            break;
        case TypeClose:
            Log_error("[receive_response] Receive close for response %d of seq %lu\n",
                                        cmd->rtype, (unsigned long)cmd->seq);
			conn->state = CLIENT_CONN_STATE_CLOSE; //prevent future requests
            pthread_mutex_lock(&conn->mutex);
            drain_cmnd(conn);
            pthread_mutex_unlock(&conn->mutex);
            break;
        default:
            Log_error("[receive_response] Unknown message type %d\n", cmd->rtype);
        }

		//Log_debug("333 cond: %p", &cmd->cond);
        //pthread_mutex_lock(&cmd->mutex);
        pthread_mutex_unlock(&cmd->mutex);
        //pthread_cond_signal(&cmd->cond);
        //ret = receive_response(conn, resp);
    }

    if (ret != 0) {
        Log_error("[receive_response] Receive response returned error\n");
    }

exit:
    return NULL;
}

void start_response_processing(struct vio_connection *conn) {
    int rc;

    rc = pthread_create(&conn->response_thread, NULL, &response_process, conn);
    if (rc < 0) {
        Log_error("[start_response_processing] Fail to create response thread\n");
        return;
    }
    rc = pthread_detach(conn->response_thread);
    if (rc < 0) {
        Log_error("[start_response_processing] Fail to detach thread ret:%d\n", (int)rc);
    }
}

int new_seq(struct vio_connection *conn) {
    return __sync_fetch_and_add(&conn->seq, 1);
}

int process_request(struct vio_connection *conn, void *buf, size_t count, off_t offset,
                uint32_t type) {
    //struct Message *req = malloc(sizeof(struct Message)); 
    struct cmnd *cmd = NULL;
    int rc = 0;
    uint32_t idx = 0; //命令数组的下标

    //Log_debug("000");

    if (type != TypeRead && type != TypeWrite) {
        Log_error("[process_request] BUG: Invalid type for process_request %d\n", type);
        rc = -EFAULT;
        goto free;
    }

	//Log_debug("[process_request] type:%u offset:%ld length:%u\n", req->Type, (long)req->Offset, req->DataLength);
	pthread_mutex_lock(&conn->mutex);
	if (conn->state != CLIENT_CONN_STATE_OPEN) {
		Log_error("[process_request] Cannot queue in more request, Connection is closed\n");
		rc = -EFAULT;
	    pthread_mutex_unlock(&conn->mutex);
		goto free;
	}
	pthread_mutex_unlock(&conn->mutex);

	//Log_debug("111 length:%d, offset:%u type:%u", (unsigned int)count,
	//    (unsigned int)offset, (unsigned int)type);
    cmd = add(conn->msg_buffer, buf, count, offset, type, new_seq(conn), &idx);
    //Log_debug("222 add return cmd:%p idx:%u", cmd, idx);
    Log_debug("[process_request]seq:%ld type:%u offset:%ld length:%zu idx:%d\n",cmd->seq, type, (long)offset, count,idx);

	//Log_debug("333");
	pthread_mutex_lock(&cmd->mutex);
    rc = send_request(conn, idx);
    if (rc < 0) {
        Log_error("[process_request] type:%u send_request return:%d\n", cmd->type, rc);
        goto out;
    }
	//Log_debug("444");

    //Log_debug("555 idx:%u cond:%p", idx, &cmd->cond);
    pthread_mutex_lock(&cmd->mutex); //等待response那边解锁，pthread_cond_wait 可能response的pthread_cond_signal提前了，导致wait卡死
    //pthread_cond_wait(&cmd->cond, &cmd->mutex);
    Log_debug("[process_request]command return seq:%ld type:%d offset:%ld length:%d\n",cmd->seq, cmd->rtype, (long)cmd->offset, cmd->length);

    if (cmd->rtype != TypeOK) {
        Log_error("[process_request] type:%u offset:%ld length:%u\n", cmd->type, (long)cmd->offset, cmd->length);
        rc = -EFAULT;
        goto out;
    }

    if (cmd->type == TypeRead) {
        memcpy(buf, (const void *)(cmd->data), cmd->length);
    }

out:
	//Log_debug("888 idx: %d", idx);
	del(conn->msg_buffer, idx);
	//Log_debug("999 del return idx: %d", idx);
    pthread_mutex_unlock(&cmd->mutex);

free:
    return rc;
}

int read_at(struct vio_connection *conn, void *buf, size_t count, off_t offset) {
    return process_request(conn, buf, count, offset, TypeRead);
}

int write_at(struct vio_connection *conn, void *buf, size_t count, off_t offset) {
    return process_request(conn, buf, count, offset, TypeWrite);
}

struct vio_connection *new_vio_connection(char *socket_path, char *shm_file) {
    //eprintf("new_vio_connection socket_path:%s", socket_path); 
    struct sockaddr_un addr;
    int fd, rc = 0;
    int i, connected = 0;
    struct vio_connection *conn = NULL;

    conn = malloc(sizeof(struct vio_connection));
    if (conn == NULL) {
        Log_error("[new_vio_connection] cannot allocate memory for conn\n");
        return NULL;
    }

    //初始化ring buffer, 必须先初始化，再与vio建立unix链接，否则有可能vio进程
    //在open共享文件时，报no such file or directory
    conn->msg_buffer = (struct ringbuffer *)init(shm_file);
    if (NULL == conn->msg_buffer) {
        Log_error("[new_vio_connection] fail to alloc ringbuffer\n");
        exit(-EFAULT);
    }

    fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (fd == -1) {
        Log_error("[new_vio_connection] socket error\n");
        exit(-1);
    }

    memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    if (strlen(socket_path) >= 108) {
        Log_error("[new_vio_connection] socket path is too long, more than 108 characters\n");
        exit(-EINVAL);
    }

    strncpy(addr.sun_path, socket_path, strlen(socket_path));

    for (i = 0; i < retry_times; i++) {
        if (connect(fd, (struct sockaddr*)&addr, sizeof(addr)) == 0) {
            connected = 1;
            break;
        }

        Log_warning("[new_vio_connection] connect error, retrying\n");
        sleep(interval);
    }

    if (connected == 0) {
        Log_error("[new_vio_connection] connect error\n");
        exit(-EFAULT);
    }

    conn->fd = fd;
    conn->seq = 0;

    rc = pthread_mutex_init(&conn->mutex, NULL);
    if (rc < 0) {
        Log_error("[new_vio_connection] fail to init conn->mutex\n");
        exit(-EFAULT);
    }

	Log_debug("[new_vio_connection] socket_path:%s successfully\n", socket_path);
    conn->state = CLIENT_CONN_STATE_OPEN;
    return conn;
}

int shutdown_vio_connection1(struct vio_connection *conn) {
    struct cmnd_idx *cidx = NULL;
    struct cmnd *cmd = NULL;
    int retry = 5;

    if (NULL == conn) {
        return 0;
    }

    pthread_mutex_lock(&conn->mutex);
    if (conn->state == CLIENT_CONN_STATE_CLOSE) {
        pthread_mutex_unlock(&conn->mutex);
        return 0;
    }

	conn->state = CLIENT_CONN_STATE_CLOSE; //prevent future requests

	//清理共享内存中所有未处理的cmnd
    list_for_each_entry(cidx, &conn->msg_buffer->used_list, used) {
        pthread_mutex_lock(&conn->msg_buffer->mutex);
        cmd = cidx->cmd;
		cmd->rtype = TypeError;
        pthread_mutex_unlock(&conn->msg_buffer->mutex);
        pthread_cond_signal(&cmd->cond);
    }

    //等待所有命令返回iscsi
    while (retry--) {
        pthread_mutex_lock(&conn->msg_buffer->mutex);
        if (conn->msg_buffer->restix == CMD_DEPTH) {
            pthread_mutex_unlock(&conn->msg_buffer->mutex);
            break;
        }
        pthread_mutex_unlock(&conn->msg_buffer->mutex);
        Log_debug("shutdown_vio_connection wait for cmd complete retry:%d", retry);
        usleep(100000); //100ms
    }

	pthread_mutex_unlock(&conn->mutex);

	destroy(conn->msg_buffer);
    close(conn->fd);
    free(conn);
    conn=NULL;
    Log_debug("shutdown_vio_connection successfully\n");
    return 0;
}


int shutdown_vio_connection(struct vio_connection *conn) {
    if (NULL == conn) {
        return 0;
    }
	Log_debug("shutdown_vio_connection entry\n");

    pthread_mutex_lock(&conn->mutex);
    if (conn->state == CLIENT_CONN_STATE_CLOSE) {
        pthread_mutex_unlock(&conn->mutex);
        return 0;
    }

	conn->state = CLIENT_CONN_STATE_CLOSE; //prevent future requests

	drain_cmnd(conn);

	pthread_mutex_unlock(&conn->mutex);

	destroy(conn->msg_buffer);
    close(conn->fd);
    free(conn);
    conn=NULL;
    Log_debug("shutdown_vio_connection successfully\n");
    return 0;
}

void drain_cmnd(struct vio_connection *conn) {
    struct cmnd_idx *cidx = NULL;
    struct cmnd *cmd = NULL;
    int retry = 5;

    if (NULL == conn) {
        return;
    }

	//清理共享内存中所有未处理的cmnd
    list_for_each_entry(cidx, &conn->msg_buffer->used_list, used) {
        pthread_mutex_lock(&conn->msg_buffer->mutex);
        cmd = cidx->cmd;
		cmd->rtype = TypeError;
        pthread_mutex_unlock(&conn->msg_buffer->mutex);
		pthread_mutex_unlock(&cmd->mutex);
        //pthread_cond_signal(&cmd->cond);
    }

    //等待所有命令返回iscsi
    while (retry--) {
        pthread_mutex_lock(&conn->msg_buffer->mutex);
        if (conn->msg_buffer->restix == CMD_DEPTH) {
            pthread_mutex_unlock(&conn->msg_buffer->mutex);
            break;
        }
        pthread_mutex_unlock(&conn->msg_buffer->mutex);
        Log_debug("shutdown_vio_connection wait for cmd complete retry:%d", retry);
        usleep(100000); //100ms
    }

    return;
}
