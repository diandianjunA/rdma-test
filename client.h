//
// Created by 熊嘉晟 on 2024/7/12.
//

#ifndef RDMA_TEST_CLIENT_H
#define RDMA_TEST_CLIENT_H

#include "rdma_common.h"

struct config_t config = {
        "mlx5_0",  /* dev_name */
        NULL,  /* server_name */
        2345, /* tcp_port */
        10241,     /* ib_port */
        0, /* gid_idx */
        "receive" /* mode */
};

int resources_create(struct resources *res);

void resources_init(struct resources *res);

void print_config(void);

int resources_destroy(struct resources *res);

int sock_sync_data(int sock, int xfer_size, char *local_data, char *remote_data);

int modify_qp_to_init(struct ibv_qp *qp);

int modify_qp_to_rtr(struct ibv_qp *qp, uint32_t remote_qpn, uint16_t dlid, uint8_t *dgid);

int modify_qp_to_rts(struct ibv_qp *qp);

int poll_completion(struct resources *res);

#endif //RDMA_TEST_CLIENT_H
