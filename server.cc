//
// Created by 熊嘉晟 on 2024/7/12.
//

#include <chrono>
#include "server.h"

void print_config(void) {
    fprintf(stdout, " ------------------------------------------------\n");
    fprintf(stdout, " Device name : \"%s\"\n", config.dev_name);
    fprintf(stdout, " IB port : %u\n", config.ib_port);
    if (config.server_name)
        fprintf(stdout, " IP : %s\n", config.server_name);
    fprintf(stdout, " TCP port : %u\n", config.tcp_port);
    if (config.gid_idx >= 0)
        fprintf(stdout, " GID index : %u\n", config.gid_idx);
    fprintf(stdout, " ------------------------------------------------\n\n");
}

void resources_init(struct resources *res) {
    memset(res, 0, sizeof *res);
    res->sock = -1;
}

int resources_create(struct resources *res) {
    struct ibv_device **dev_list = NULL;
    struct ibv_qp_init_attr qp_init_attr;
    struct ibv_device *ib_dev = NULL;
    size_t size;
    int i;
    int mr_flags = 0;
    int cq_size = 0;
    int num_devices;
    int rc = 0;
    fprintf(stdout, "waiting on port %d for TCP connection\n", config.tcp_port);
    res->sock = sock_connect(NULL, config.tcp_port);
    if (res->sock < 0) {
        fprintf(stderr, "failed to establish TCP connection with client on port %d\n", config.tcp_port);
        rc = -1;
        goto resources_create_exit;
    }
    fprintf(stdout, "TCP connection was established\n");
    fprintf(stdout, "searching for IB devices in host\n");
    /* get device names in the system */
    dev_list = ibv_get_device_list(&num_devices);
    if (!dev_list) {
        fprintf(stderr, "failed to get IB devices list\n");
        rc = 1;
        goto resources_create_exit;
    }
    /* if there isn't any IB device in host */
    if (!num_devices) {
        fprintf(stderr, "found %d device(s)\n", num_devices);
        rc = 1;
        goto resources_create_exit;
    }
    fprintf(stdout, "found %d device(s)\n", num_devices);
    /* search for the specific device in device list */
    for (i = 0; i < num_devices; i++) {
        if (!config.dev_name) {
            config.dev_name = strdup(ibv_get_device_name(dev_list[i]));
            fprintf(stdout, "device not specified, using first one found: %s\n", config.dev_name);
        }
        if (!strcmp(ibv_get_device_name(dev_list[i]), config.dev_name)) {
            ib_dev = dev_list[i];
            break;
        }
    }
    /* if the device wasn't found in host */
    if (!ib_dev) {
        fprintf(stderr, "IB device %s wasn't found\n", config.dev_name);
        rc = 1;
        goto resources_create_exit;
    }
    /* get device handle */
    res->ib_ctx = ibv_open_device(ib_dev);
    if (!res->ib_ctx) {
        fprintf(stderr, "failed to open device %s\n", config.dev_name);
        rc = 1;
        goto resources_create_exit;
    }
    /* We are now done with device list, free it */
    ibv_free_device_list(dev_list);
    dev_list = NULL;
    ib_dev = NULL;
    /* query port properties */
    if (ibv_query_port(res->ib_ctx, config.ib_port, &res->port_attr)) {
        fprintf(stderr, "ibv_query_port on port %u failed\n", config.ib_port);
        rc = 1;
        goto resources_create_exit;
    }
    /* allocate Protection Domain */
    res->pd = ibv_alloc_pd(res->ib_ctx);
    if (!res->pd) {
        fprintf(stderr, "ibv_alloc_pd failed\n");
        rc = 1;
        goto resources_create_exit;
    }
    /* each side will send only one WR, so Completion Queue with 1 entry is enough */
    cq_size = 1;
    res->cq = ibv_create_cq(res->ib_ctx, cq_size, NULL, NULL, 0);
    if (!res->cq) {
        fprintf(stderr, "failed to create CQ with %u entries\n", cq_size);
        rc = 1;
        goto resources_create_exit;
    }
    /* allocate the memory buffer that will hold the data */
    size = MSG_SIZE;
    res->buf = (char *) malloc(size);
    if (!res->buf) {
        fprintf(stderr, "failed to malloc %Zu bytes to memory buffer\n", size);
        rc = 1;
        goto resources_create_exit;
    }
    memset(res->buf, 0, size);
    /* register the memory buffer */
    mr_flags = IBV_ACCESS_LOCAL_WRITE | IBV_ACCESS_REMOTE_READ | IBV_ACCESS_REMOTE_WRITE;
    res->mr = ibv_reg_mr(res->pd, res->buf, size, mr_flags);
    if (!res->mr) {
        fprintf(stderr, "ibv_reg_mr failed with mr_flags=0x%x\n", mr_flags);
        rc = 1;
        goto resources_create_exit;
    }
    fprintf(stdout, "MR was registered with addr=%p, lkey=0x%x, rkey=0x%x, flags=0x%x\n", res->buf, res->mr->lkey,
            res->mr->rkey, mr_flags);
    /* create the Queue Pair */
    memset(&qp_init_attr, 0, sizeof(qp_init_attr));
    qp_init_attr.qp_type = IBV_QPT_RC;
    qp_init_attr.sq_sig_all = 1;
    qp_init_attr.send_cq = res->cq;
    qp_init_attr.recv_cq = res->cq;
    qp_init_attr.cap.max_send_wr = 1;
    qp_init_attr.cap.max_recv_wr = 1;
    qp_init_attr.cap.max_send_sge = 1;
    qp_init_attr.cap.max_recv_sge = 1;
    res->qp = ibv_create_qp(res->pd, &qp_init_attr);
    if (!res->qp) {
        fprintf(stderr, "failed to create QP\n");
        rc = 1;
        goto resources_create_exit;
    }
    fprintf(stdout, "QP was created, QP number=0x%x\n", res->qp->qp_num);
    resources_create_exit:
    if (rc) {
        /* Error encountered, cleanup */
        if (res->qp) {
            ibv_destroy_qp(res->qp);
            res->qp = NULL;
        }
        if (res->mr) {
            ibv_dereg_mr(res->mr);
            res->mr = NULL;
        }
        if (res->buf) {
            free(res->buf);
            res->buf = NULL;
        }
        if (res->cq) {
            ibv_destroy_cq(res->cq);
            res->cq = NULL;
        }
        if (res->pd) {
            ibv_dealloc_pd(res->pd);
            res->pd = NULL;
        }
        if (res->ib_ctx) {
            ibv_close_device(res->ib_ctx);
            res->ib_ctx = NULL;
        }
        if (dev_list) {
            ibv_free_device_list(dev_list);
            dev_list = NULL;
        }
        if (res->sock >= 0) {
            if (close(res->sock))
                fprintf(stderr, "failed to close socket\n");
            res->sock = -1;
        }
    }
    return rc;
}

int resources_destroy(struct resources *res) {
    int rc = 0;
    if (res->qp)
        if (ibv_destroy_qp(res->qp)) {
            fprintf(stderr, "failed to destroy QP\n");
            rc = 1;
        }
    if (res->mr)
        if (ibv_dereg_mr(res->mr)) {
            fprintf(stderr, "failed to deregister MR\n");
            rc = 1;
        }
    if (res->buf)
        free(res->buf);
    if (res->cq)
        if (ibv_destroy_cq(res->cq)) {
            fprintf(stderr, "failed to destroy CQ\n");
            rc = 1;
        }
    if (res->pd)
        if (ibv_dealloc_pd(res->pd)) {
            fprintf(stderr, "failed to deallocate PD\n");
            rc = 1;
        }
    if (res->ib_ctx)
        if (ibv_close_device(res->ib_ctx)) {
            fprintf(stderr, "failed to close device context\n");
            rc = 1;
        }
    if (res->sock >= 0)
        if (close(res->sock)) {
            fprintf(stderr, "failed to close socket\n");
            rc = 1;
        }
    return rc;
}

int connect_qp(struct resources *res) {
    struct cm_con_data_t local_con_data;
    struct cm_con_data_t remote_con_data;
    struct cm_con_data_t tmp_con_data;
    int rc = 0;
    char temp_char;
    union ibv_gid my_gid;
    if (config.gid_idx >= 0) {
        rc = ibv_query_gid(res->ib_ctx, config.ib_port, config.gid_idx, &my_gid);
        if (rc) {
            fprintf(stderr, "could not get gid for port %d, index %d\n", config.ib_port, config.gid_idx);
            goto connect_qp_exit;
        }
    } else {
        memset(&my_gid, 0, sizeof my_gid);
    }
    /* exchange using TCP sockets info required to connect QPs */
    local_con_data.addr = htonll((uintptr_t) res->buf);
    local_con_data.rkey = htonl(res->mr->rkey);
    local_con_data.qp_num = htonl(res->qp->qp_num);
    local_con_data.lid = htons(res->port_attr.lid);
    memcpy(local_con_data.gid, &my_gid, 16);
    fprintf(stdout, "\nLocal LID = 0x%x\n", res->port_attr.lid);
    if (sock_sync_data(res->sock, sizeof(struct cm_con_data_t), (char *) &local_con_data, (char *) &tmp_con_data) < 0) {
        fprintf(stderr, "failed to exchange connection data between sides\n");
        rc = 1;
        goto connect_qp_exit;
    }
    remote_con_data.addr = ntohll(tmp_con_data.addr);
    remote_con_data.rkey = ntohl(tmp_con_data.rkey);
    remote_con_data.qp_num = ntohl(tmp_con_data.qp_num);
    remote_con_data.lid = ntohs(tmp_con_data.lid);
    memcpy(remote_con_data.gid, tmp_con_data.gid, 16);
    res->remote_props = remote_con_data;
    fprintf(stdout, "Remote address = 0x%" PRIx64 "\n", remote_con_data.addr);
    fprintf(stdout, "Remote rkey = 0x%x\n", remote_con_data.rkey);
    fprintf(stdout, "Remote QP number = 0x%x\n", remote_con_data.qp_num);
    fprintf(stdout, "Remote LID = 0x%x\n", remote_con_data.lid);
    if (config.gid_idx >= 0) {
        uint8_t *p = remote_con_data.gid;
        fprintf(stdout, "Remote GID = %02x:%02x:%02x:%02x:%02x:%02x:%02x:%02x:"
                        "%02x:%02x:%02x:%02x:%02x:%02x:%02x:%02x\n",
                p[0], p[1], p[2], p[3], p[4], p[5], p[6], p[7],
                p[8], p[9], p[10], p[11], p[12], p[13], p[14], p[15]);
    }
    /* modify the QP to init */
    rc = modify_qp_to_init(res->qp);
    if (rc) {
        fprintf(stderr, "failed to modify QP state to INIT\n");
        goto connect_qp_exit;
    }
    rc = modify_qp_to_rtr(res->qp, remote_con_data.qp_num, remote_con_data.lid, remote_con_data.gid);
    if (rc) {
        fprintf(stderr, "failed to modify QP state to RTR\n");
        goto connect_qp_exit;
    }
    rc = modify_qp_to_rts(res->qp);
    if (rc) {
        fprintf(stderr, "failed to modify QP state to RTS\n");
        goto connect_qp_exit;
    }
    fprintf(stdout, "QP %u successfully connected\n", res->qp->qp_num);
    /* sync to make sure that both sides are in states that they can connect to prevent packet loss */
    if (sock_sync_data(res->sock, 1, "Q", &temp_char)) {
        fprintf(stderr, "sync error after QPs are were moved to RTS\n");
        rc = 1;
    }
    connect_qp_exit:
    return rc;
}

int sock_sync_data(int sock, int xfer_size, char *local_data, char *remote_data) {
    int rc;
    int read_bytes = 0;
    int total_read_bytes = 0;
    rc = write(sock, local_data, xfer_size);
    if (rc < xfer_size) {
        fprintf(stderr, "failed to send data during sock_sync_data\n");
    } else {
        rc = 0;
    }
    while (!rc && total_read_bytes < xfer_size) {
        read_bytes = read(sock, remote_data, xfer_size);
        if (read_bytes > 0) {
            total_read_bytes += read_bytes;
        } else {
            rc = read_bytes;
        }
    }
    return rc;
}

int modify_qp_to_init(struct ibv_qp *qp) {
    struct ibv_qp_attr attr;
    int flags;
    int rc;
    memset(&attr, 0, sizeof(attr));
    attr.qp_state = IBV_QPS_INIT;
    attr.port_num = config.ib_port;
    attr.pkey_index = 0;
    attr.qp_access_flags = IBV_ACCESS_LOCAL_WRITE | IBV_ACCESS_REMOTE_READ | IBV_ACCESS_REMOTE_WRITE;
    flags = IBV_QP_STATE | IBV_QP_PKEY_INDEX | IBV_QP_PORT | IBV_QP_ACCESS_FLAGS;
    rc = ibv_modify_qp(qp, &attr, flags);
    if (rc) {
        fprintf(stderr, "failed to modify QP state to INIT\n");
    }
    return rc;
}

int modify_qp_to_rtr(struct ibv_qp *qp, uint32_t remote_qpn, uint16_t dlid, uint8_t *dgid) {
    struct ibv_qp_attr attr;
    int flags;
    int rc;
    memset(&attr, 0, sizeof(attr));
    attr.qp_state = IBV_QPS_RTR;
    attr.path_mtu = IBV_MTU_256;
    attr.dest_qp_num = remote_qpn;
    attr.rq_psn = 0;
    attr.max_dest_rd_atomic = 1;
    attr.min_rnr_timer = 0x12;
    attr.ah_attr.is_global = 0;
    attr.ah_attr.dlid = dlid;
    attr.ah_attr.sl = 0;
    attr.ah_attr.src_path_bits = 0;
    attr.ah_attr.port_num = config.ib_port;
    if (config.gid_idx >= 0) {
        attr.ah_attr.is_global = 1;
        attr.ah_attr.port_num = 1;
        memcpy(&attr.ah_attr.grh.dgid, dgid, 16);
        attr.ah_attr.grh.flow_label = 0;
        attr.ah_attr.grh.hop_limit = 1;
        attr.ah_attr.grh.sgid_index = config.gid_idx;
        attr.ah_attr.grh.traffic_class = 0;
    }
    flags = IBV_QP_STATE | IBV_QP_AV | IBV_QP_PATH_MTU | IBV_QP_DEST_QPN |
            IBV_QP_RQ_PSN | IBV_QP_MAX_DEST_RD_ATOMIC | IBV_QP_MIN_RNR_TIMER;
    rc = ibv_modify_qp(qp, &attr, flags);
    if (rc)
        fprintf(stderr, "failed to modify QP state to RTR\n");
    return rc;
}

int modify_qp_to_rts(struct ibv_qp *qp) {
    struct ibv_qp_attr attr;
    int flags;
    int rc;
    memset(&attr, 0, sizeof(attr));
    attr.qp_state = IBV_QPS_RTS;
    attr.timeout = 0x12;
    attr.retry_cnt = 6;
    attr.rnr_retry = 0;
    attr.sq_psn = 0;
    attr.max_rd_atomic = 1;
    flags = IBV_QP_STATE | IBV_QP_TIMEOUT | IBV_QP_RETRY_CNT |
            IBV_QP_RNR_RETRY | IBV_QP_SQ_PSN | IBV_QP_MAX_QP_RD_ATOMIC;
    rc = ibv_modify_qp(qp, &attr, flags);
    if (rc) {
        fprintf(stderr, "failed to modify QP state to RTS\n");
    }
    return rc;
}

int poll_completion(struct resources *res) {
    struct ibv_wc wc;
    unsigned long start_time_msec;
    unsigned long cur_time_msec;
    struct timeval cur_time;
    int poll_result;
    int rc = 0;
    /* poll the completion for a while before giving up of doing it .. */
    gettimeofday(&cur_time, NULL);
    start_time_msec = (cur_time.tv_sec * 1000) + (cur_time.tv_usec / 1000);
    int count = 0;
    do {
        count++;
        poll_result = ibv_poll_cq(res->cq, 1, &wc);
        gettimeofday(&cur_time, NULL);
        cur_time_msec = (cur_time.tv_sec * 1000) + (cur_time.tv_usec / 1000);
    } while ((poll_result == 0) && ((cur_time_msec - start_time_msec) < MAX_POLL_CQ_TIMEOUT));
    printf("poll count: %d\n", count);
    if (poll_result < 0) {
        /* poll CQ failed */
        fprintf(stderr, "poll CQ failed\n");
        rc = 1;
    } else if (poll_result == 0) {
        /* the CQ is empty */
        fprintf(stderr, "completion wasn't found in the CQ after timeout\n");
        rc = 1;
    } else {
        /* CQE found */
        fprintf(stdout, "completion was found in CQ with status 0x%x\n", wc.status);
        /* check the completion status (here we don't care about the completion opcode */
        if (wc.status != IBV_WC_SUCCESS) {
            fprintf(stderr, "got bad completion with status: 0x%x, vendor syndrome: 0x%x\n", wc.status, wc.vendor_err);
            rc = 1;
        }
    }
    return rc;
}

int main(int argc, char *argv[]) {
    struct resources res;
    int rc = 0;
    char temp_char;
    int count = 0;
    while (true) {
        int c;
        static struct option long_options[] = {
                {.name = "port", .has_arg = 1, .val = 'p'},
                {.name = "ib-dev", .has_arg = 1, .val = 'd'},
                {.name = "ib-port", .has_arg = 1, .val = 'i'},
                {.name = "gid-idx", .has_arg = 1, .val = 'g'},
                {.name = "op", .has_arg = 1, .val = 'o'},
                {.name = "times", .has_arg = 1, .val = 't'},
                {.name = NULL, .has_arg = 0, .val = '\0'}
        };
        c = getopt_long(argc, argv, "p:d:i:g:o:t:", long_options, NULL);
        if (c == -1) {
            break;
        }
        switch (c) {
            case 'p':
                config.tcp_port = strtoul(optarg, NULL, 0);
                break;
            case 'd':
                config.dev_name = strdup(optarg);
                break;
            case 'i':
                config.ib_port = strtol(optarg, NULL, 0);
                if (config.ib_port < 0) {
                    fprintf(stderr, "Invalid port number\n");
                    return 1;
                }
                break;
            case 'g':
                config.gid_idx = strtol(optarg, NULL, 0);
                if (config.gid_idx < 0) {
                    fprintf(stderr, "Invalid GID index\n");
                    return 1;
                }
                break;
            case 'o':
                config.operation = strdup(optarg);
                break;
            case 't':
                count = strtol(optarg, NULL, 0);
                break;
            default:
                fprintf(stderr, "Invalid command line argument\n");
                return 1;
        }
    }
    print_config();
    resources_init(&res);
    if (resources_create(&res)) {
        fprintf(stderr, "failed to create resources\n");
        goto main_exit;
    }
    if (connect_qp(&res)) {
        fprintf(stderr, "failed to connect QPs\n");
        goto main_exit;
    }
    if (strcmp(config.operation, "send") == 0) {
        strcpy(res.buf, MSG);
        std::chrono::nanoseconds total(0);
        for (int i = 0; i < count; ++i) {
            if (post_send(&res, IBV_WR_SEND)) {
                fprintf(stderr, "failed to post SR\n");
                goto main_exit;
            }
            auto start = std::chrono::high_resolution_clock::now();
            if (poll_completion(&res)) {
                fprintf(stderr, "poll completion failed\n");
                goto main_exit;
            }
            auto end = std::chrono::high_resolution_clock::now();
            std::chrono::nanoseconds elapsed = end - start;
            total += elapsed;
        }
        fprintf(stdout, "RDMA send operation took %lld ns\n", total.count());
    } else if (strcmp(config.operation, "receive") == 0) {
        for (int i = 0; i < count; ++i) {
            if (post_receive(&res)) {
                fprintf(stderr, "failed to post RR\n");
                goto main_exit;
            }
            if (poll_completion(&res)) {
                fprintf(stderr, "poll completion failed\n");
                goto main_exit;
            }
        }
        fprintf(stdout, "Message is: %s\n", res.buf);
    } else if (!strcmp(config.operation, "read")) {
        /* setup server buffer with read message */
        strcpy(res.buf, RDMAMSGR);
        /* Sync so we are sure server side has data ready before client tries to read it */
        /* just send a dummy char back and forth */
        if (sock_sync_data(res.sock, 1, "R", &temp_char)) {
            fprintf(stderr, "sync error before RDMA ops\n");
            rc = 1;
            goto main_exit;
        }
        if (sock_sync_data(res.sock, 1, "R", &temp_char)) {
            fprintf(stderr, "sync error before RDMA ops\n");
            rc = 1;
            goto main_exit;
        }
    } else if (!strcmp(config.operation, "write")) {
        strcpy(res.buf, RDMAMSGW);
        /* Sync so server will know that client is done mucking with its memory */
        /* just send a dummy char back and forth */
        if (sock_sync_data(res.sock, 1, "W", &temp_char)) {
            fprintf(stderr, "sync error after RDMA ops\n");
            rc = 1;
            goto main_exit;
        }
        fprintf(stdout, "Contents of server buffer: '%s'\n", res.buf);
        if (sock_sync_data(res.sock, 1, "W", &temp_char)) {
            fprintf(stderr, "sync error after RDMA ops\n");
            rc = 1;
            goto main_exit;
        }
    } else {
        fprintf(stderr, "unknown operation\n");
        goto main_exit;
    }
    main_exit:
    if (resources_destroy(&res)) {
        fprintf(stderr, "failed to destroy resources\n");
        rc = 1;
    }
    fprintf(stdout, "test result is %d\n", rc);
    return rc;
}
