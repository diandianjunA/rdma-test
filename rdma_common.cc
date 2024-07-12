//
// Created by 熊嘉晟 on 2024/7/12.
//

#include "rdma_common.h"

int sock_connect(const char *servername, int port) {
    struct addrinfo *resolved_addr = NULL;
    struct addrinfo *iterator;
    char service[6];
    int sockfd = -1;
    int listenfd = 0;
    int tmp;
    struct addrinfo hints = {
            .ai_flags = AI_PASSIVE,
            .ai_family = AF_INET,
            .ai_socktype = SOCK_STREAM
    };
    if (sprintf(service, "%d", port) < 0) {
        goto sock_connect_exit;
    }
    /* Resolve DNS address, use sockfd as temp storage */
    sockfd = getaddrinfo(servername, service, &hints, &resolved_addr);
    if (sockfd < 0) {
        fprintf(stderr, "%s for %s:%d\n", gai_strerror(sockfd), servername, port);
        goto sock_connect_exit;
    }
    /* Search through results and find the one we want */
    for (iterator = resolved_addr; iterator; iterator = iterator->ai_next) {
        sockfd = socket(iterator->ai_family, iterator->ai_socktype, iterator->ai_protocol);
        if (sockfd >= 0) {
            if (servername) {
                /* Client mode. Initiate connection to remote */
                if ((tmp = connect(sockfd, iterator->ai_addr, iterator->ai_addrlen))) {
                    fprintf(stdout, "failed connect \n");
                    close(sockfd);
                    sockfd = -1;
                }
            } else {
                /* Server mode. Set up listening socket an accept a connection */
                listenfd = sockfd;
                sockfd = -1;
                if (bind(listenfd, iterator->ai_addr, iterator->ai_addrlen))
                    goto sock_connect_exit;
                listen(listenfd, 1);
                sockfd = accept(listenfd, NULL, 0);
            }
        }
    }
    sock_connect_exit:
    if (listenfd)
        close(listenfd);
    if (resolved_addr)
        freeaddrinfo(resolved_addr);
    if (sockfd < 0) {
        if (servername)
            fprintf(stderr, "Couldn't connect to %s:%d\n", servername, port);
        else {
            perror("server accept");
            fprintf(stderr, "accept() failed\n");
        }
    }
    return sockfd;
}

int post_receive(struct resources *res) {
    struct ibv_recv_wr rr;
    struct ibv_sge sge;
    struct ibv_recv_wr *bad_wr;
    int rc;
    /* prepare the scatter/gather entry */
    memset(&sge, 0, sizeof(sge));
    sge.addr = (uintptr_t) res->buf;
    sge.length = MSG_SIZE;
    sge.lkey = res->mr->lkey;
    /* prepare the receive work request */
    memset(&rr, 0, sizeof(rr));
    rr.next = NULL;
    rr.wr_id = 0;
    rr.sg_list = &sge;
    rr.num_sge = 1;
    /* post the Receive Request to the RQ */
    rc = ibv_post_recv(res->qp, &rr, &bad_wr);
    if (rc) {
        fprintf(stderr, "failed to post RR\n");
    } else {
        fprintf(stdout, "Receive Request was posted\n");
    }
    return rc;
}

int post_send(struct resources *res, int opcode) {
    struct ibv_send_wr sr;
    struct ibv_sge sge;
    struct ibv_send_wr *bad_wr = NULL;
    int rc;
    /* prepare the scatter/gather entry */
    memset(&sge, 0, sizeof(sge));
    sge.addr = (uintptr_t) res->buf;
    sge.length = MSG_SIZE;
    sge.lkey = res->mr->lkey;
    /* prepare the send work request */
    memset(&sr, 0, sizeof(sr));
    sr.next = NULL;
    sr.wr_id = 0;
    sr.sg_list = &sge;
    sr.num_sge = 1;
    sr.opcode = (ibv_wr_opcode) opcode;
    sr.send_flags = IBV_SEND_SIGNALED;
    if (opcode != IBV_WR_SEND) {
        sr.wr.rdma.remote_addr = res->remote_props.addr;
        sr.wr.rdma.rkey = res->remote_props.rkey;
    }
    /* there is a Receive Request in the responder side, so we won't get any into RNR flow */
    rc = ibv_post_send(res->qp, &sr, &bad_wr);
    if (rc) {
        fprintf(stderr, "failed to post SR\n");
    } else {
        switch (opcode) {
            case IBV_WR_SEND:
                fprintf(stdout, "Send Request was posted\n");
                break;
            case IBV_WR_RDMA_READ:
                fprintf(stdout, "RDMA Read Request was posted\n");
                break;
            case IBV_WR_RDMA_WRITE:
                fprintf(stdout, "RDMA Write Request was posted\n");
                break;
            default:
                fprintf(stdout, "Unknown Request was posted\n");
                break;
        }
    }
    return rc;
}