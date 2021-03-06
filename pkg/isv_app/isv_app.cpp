/*
 * Copyright (C) 2011-2018 Intel Corporation. All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 *
 *   * Redistributions of source code must retain the above copyright
 *     notice, this list of conditions and the following disclaimer.
 *   * Redistributions in binary form must reproduce the above copyright
 *     notice, this list of conditions and the following disclaimer in
 *     the documentation and/or other materials provided with the
 *     distribution.
 *   * Neither the name of Intel Corporation nor the names of its
 *     contributors may be used to endorse or promote products derived
 *     from this software without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
 * "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
 * LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR
 * A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT
 * OWNER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL,
 * SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT
 * LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE,
 * DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY
 * THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 * (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
 * OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 *
 */

// This sample is confined to the communication between a SGX client platform
// and an ISV Application Server.

#include "log.h"
#include "aibe.h"
#include <stdio.h>
#include <limits.h>
#include <unistd.h>
// Needed for definition of remote attestation messages.
#include "remote_attestation_result.h"

#include "isv_enclave_u.h"

// Needed to call untrusted key exchange library APIs, i.e. sgx_ra_proc_msg2.
#include "sgx_ukey_exchange.h"

// Needed to get service provider's information, in your real project, you will
// need to talk to real server.
#include "network_ra.h"

// Needed to create enclave and do ecall.
#include "sgx_urts.h"

// Needed to query extended epid group id.
#include "sgx_uae_service.h"

#include "service_provider.h"

#ifndef SAFE_FREE
#define SAFE_FREE(ptr)     \
    {                      \
        if (NULL != (ptr)) \
        {                  \
            free(ptr);     \
            (ptr) = NULL;  \
        }                  \
    }
#endif

// In addition to generating and sending messages, this application
// can use pre-generated messages to verify the generation of
// messages and the information flow.
#include "sample_messages.h"

#define ENCLAVE_PATH "isv_enclave.signed.so"

#define LENOFMSE 1024

uint8_t *msg1_samples[] = {msg1_sample1, msg1_sample2};
uint8_t *msg2_samples[] = {msg2_sample1, msg2_sample2};
uint8_t *msg3_samples[] = {msg3_sample1, msg3_sample2};
uint8_t *attestation_msg_samples[] =
        {attestation_msg_sample1, attestation_msg_sample2};

// Some utility functions to output some of the data structures passed between
// the ISV app and the remote attestation service provider.
void PRINT_BYTE_ARRAY(
        FILE *file, void *mem, uint32_t len) {
    if (!mem || !len) {
        fprintf(file, "\n( null )\n");
        return;
    }
    uint8_t *array = (uint8_t *) mem;
    fprintf(file, "%u bytes:\n{\n", len);
    uint32_t i = 0;
    for (i = 0; i < len - 1; i++) {
        fprintf(file, "0x%x, ", array[i]);
        if (i % 8 == 7)
            fprintf(file, "\n");
    }
    fprintf(file, "0x%x ", array[i]);
    fprintf(file, "\n}\n");
}

void PRINT_ATTESTATION_SERVICE_RESPONSE(
        FILE *file,
        ra_samp_response_header_t *response) {
    if (!response) {
        fprintf(file, "\t\n( null )\n");
        return;
    }

    fprintf(file, "RESPONSE TYPE:   0x%x\n", response->type);
    fprintf(file, "RESPONSE STATUS: 0x%x 0x%x\n", response->status[0],
            response->status[1]);
    fprintf(file, "RESPONSE BODY SIZE: %u\n", response->size);

    if (response->type == TYPE_RA_MSG2) {
        sgx_ra_msg2_t *p_msg2_body = (sgx_ra_msg2_t *) (response->body);

        fprintf(file, "MSG2 gb - ");
        PRINT_BYTE_ARRAY(file, &(p_msg2_body->g_b), sizeof(p_msg2_body->g_b));

        fprintf(file, "MSG2 spid - ");
        PRINT_BYTE_ARRAY(file, &(p_msg2_body->spid), sizeof(p_msg2_body->spid));

        fprintf(file, "MSG2 quote_type : %hx\n", p_msg2_body->quote_type);

        fprintf(file, "MSG2 kdf_id : %hx\n", p_msg2_body->kdf_id);

        fprintf(file, "MSG2 sign_gb_ga - ");
        PRINT_BYTE_ARRAY(file, &(p_msg2_body->sign_gb_ga),
                         sizeof(p_msg2_body->sign_gb_ga));

        fprintf(file, "MSG2 mac - ");
        PRINT_BYTE_ARRAY(file, &(p_msg2_body->mac), sizeof(p_msg2_body->mac));

        fprintf(file, "MSG2 sig_rl - ");
        PRINT_BYTE_ARRAY(file, &(p_msg2_body->sig_rl),
                         p_msg2_body->sig_rl_size);
    } else if (response->type == TYPE_RA_ATT_RESULT) {
        sample_ra_att_result_msg_t *p_att_result =
                (sample_ra_att_result_msg_t *) (response->body);
        fprintf(file, "ATTESTATION RESULT MSG platform_info_blob - ");
        PRINT_BYTE_ARRAY(file, &(p_att_result->platform_info_blob),
                         sizeof(p_att_result->platform_info_blob));

        fprintf(file, "ATTESTATION RESULT MSG mac - ");
        PRINT_BYTE_ARRAY(file, &(p_att_result->mac), sizeof(p_att_result->mac));

        fprintf(file, "ATTESTATION RESULT MSG secret.payload_tag - %u bytes\n",
                p_att_result->secret.payload_size);

        fprintf(file, "ATTESTATION RESULT MSG secret.payload - ");
        PRINT_BYTE_ARRAY(file, p_att_result->secret.payload,
                         p_att_result->secret.payload_size);
    } else {
        fprintf(file, "\nERROR in printing out the response. "
                      "Response of type not supported %d\n",
                response->type);
    }
}

int myaesencrypt(const ra_samp_request_header_t *p_msgenc,
                 uint32_t msg_size,
                 sgx_enclave_id_t id,
                 sgx_status_t *status,
                 sgx_ra_context_t context,
                 NetworkServer &server) {
    if (!p_msgenc || msg_size > LENOFMSE) {
        return -1;
    }
    int ret = 0;

    int data_size = msg_size;
    int busy_retry_time = 4;
    uint8_t p_data[LENOFMSE] = {0};
    uint8_t out_data[LENOFMSE] = {0};
    ra_samp_response_header_t *p_msg2_full = NULL;
    uint8_t msg2_size = data_size + SGX_AESGCM_MAC_SIZE;

    sgx_aes_gcm_128bit_tag_t mac;

    memcpy_s(p_data, data_size, p_msgenc, data_size);
    do {
        ret = enclave_encrypt(
                id,
                status,
                p_data,
                data_size,
                out_data,
                mac);
        fprintf(stdout, "\nE %d %d", id, *status);
    } while (SGX_ERROR_BUSY == ret && busy_retry_time--);
    fprintf(stdout, "\nData of Encrypt is\n");
    PRINT_BYTE_ARRAY(stdout, p_data, data_size);
    fprintf(stdout, "\nData of Encrypted and mac is\n");
    PRINT_BYTE_ARRAY(stdout, out_data, data_size);
    PRINT_BYTE_ARRAY(stdout, mac, SGX_AESGCM_MAC_SIZE);
    p_msg2_full = (ra_samp_response_header_t *) malloc(msg2_size + sizeof(ra_samp_response_header_t));
    if (!p_msg2_full) {
        fprintf(stderr, "\nError, out of memory in [%s]-[%d].", __FUNCTION__, __LINE__);
        ret = SP_INTERNAL_ERROR;
        return ret;
    }
    memset(p_msg2_full, 0, msg2_size + sizeof(ra_samp_response_header_t));
    p_msg2_full->type = TYPE_RA_MSGENC;
    p_msg2_full->size = msg2_size;
    p_msg2_full->status[0] = 0;
    p_msg2_full->status[1] = 0;

    if (memcpy_s(p_msg2_full->body, data_size, out_data, data_size)) {
        fprintf(stderr, "\nError, memcpy failed in [%s]-[%d].", __FUNCTION__, __LINE__);
        ret = SP_INTERNAL_ERROR;
        return ret;
    }
    if (memcpy_s(p_msg2_full->body + data_size, SGX_AESGCM_MAC_SIZE, mac, SGX_AESGCM_MAC_SIZE)) {
        fprintf(stderr, "\nError, memcpy failed in [%s]-[%d].", __FUNCTION__, __LINE__);
        ret = SP_INTERNAL_ERROR;
        return ret;
    }
    memset(server.sendbuf, 0, BUFSIZ);
    if (memcpy_s(server.sendbuf,
                 msg2_size + sizeof(ra_samp_response_header_t),
                 p_msg2_full,
                 msg2_size + sizeof(ra_samp_response_header_t))) {
        fprintf(stderr, "\nError, memcpy failed in [%s]-[%d].", __FUNCTION__, __LINE__);
        ret = SP_INTERNAL_ERROR;
        return ret;
    }

    if (server.SendTo(msg2_size + sizeof(ra_samp_response_header_t)) < 0) {
        fprintf(stderr, "\nError, send encrypted data failed in [%s]-[%d].", __FUNCTION__, __LINE__);
        ret = SP_INTERNAL_ERROR;
        return ret;
    }
    SAFE_FREE(p_msg2_full);

    return ret;
}

int myaesdecrypt(const ra_samp_request_header_t *p_msgenc,
                 uint32_t msg_size,
                 sgx_enclave_id_t id,
                 sgx_status_t *status,
                 sgx_ra_context_t context,
                 NetworkServer &server) {
    if (!p_msgenc ||
        (msg_size > LENOFMSE)) {
        return -1;
    }
    sgx_aes_gcm_128bit_tag_t mac;
    int ret = 0;
    fprintf(stdout, "\nD %d %d", id, *status);
    int busy_retry_time = 4;
    uint8_t p_data[LENOFMSE] = {0};
    uint8_t out_data[LENOFMSE] = {0};
    ra_samp_response_header_t *p_msg2_full = NULL;
    uint8_t data_size = msg_size - SGX_AESGCM_MAC_SIZE;
    uint8_t msg2_size = data_size;

    printf("====%d %d\n", data_size, msg_size);
    PRINT_BYTE_ARRAY(stdout, (uint8_t *) p_msgenc, msg_size);

    memcpy_s(p_data, msg_size, p_msgenc, msg_size);
    memcpy_s(mac, SGX_AESGCM_MAC_SIZE, p_data + data_size, SGX_AESGCM_MAC_SIZE);
    do {
        ret = enclave_decrypt(
                id,
                status,
                p_data,
                data_size,
                out_data,
                mac);
    } while (SGX_ERROR_BUSY == ret && busy_retry_time--);
    if (ret != SGX_SUCCESS)
        return ret;
    fprintf(stdout, "\nData of Decrypt and mac is\n");
    PRINT_BYTE_ARRAY(stdout, p_data, data_size);
    PRINT_BYTE_ARRAY(stdout, mac, SGX_AESGCM_MAC_SIZE);
    fprintf(stdout, "\nData of Decrypted is\n");
    PRINT_BYTE_ARRAY(stdout, out_data, data_size);

    p_msg2_full = (ra_samp_response_header_t *) malloc(msg2_size + sizeof(ra_samp_response_header_t));
    if (!p_msg2_full) {
        fprintf(stderr, "\nError, out of memory in [%s]-[%d].", __FUNCTION__, __LINE__);
        ret = SP_INTERNAL_ERROR;
        return ret;
    }
    memset(p_msg2_full, 0, msg2_size + sizeof(ra_samp_response_header_t));
    p_msg2_full->type = TYPE_RA_MSGDEC;
    p_msg2_full->size = msg2_size;
    // The simulated message2 always passes.  This would need to be set
    // accordingly in a real service provider implementation.
    p_msg2_full->status[0] = 0;
    p_msg2_full->status[1] = 0;

    if (memcpy_s(&p_msg2_full->body[0], msg2_size, &out_data[0], msg2_size)) {
        fprintf(stderr, "\nError, memcpy failed in [%s]-[%d].", __FUNCTION__, __LINE__);
        ret = SP_INTERNAL_ERROR;
        return ret;
    }
    memset(server.sendbuf, 0, BUFSIZ);
    if (memcpy_s(server.sendbuf,
                 msg2_size + sizeof(ra_samp_response_header_t),
                 p_msg2_full,
                 msg2_size + sizeof(ra_samp_response_header_t))) {
        fprintf(stderr, "\nError, memcpy failed in [%s]-[%d].", __FUNCTION__, __LINE__);
        ret = SP_INTERNAL_ERROR;
        return ret;
    }

    if (server.SendTo(msg2_size + sizeof(ra_samp_response_header_t)) < 0) {
        fprintf(stderr, "\nError, send encrypted data failed in [%s]-[%d].", __FUNCTION__, __LINE__);
        ret = SP_INTERNAL_ERROR;
        return ret;
    }
    SAFE_FREE(p_msg2_full);
    fprintf(stdout, "\nSend Decrypt Data Done.");
    return ret;
}

int pkg_keygen(const ra_samp_request_header_t *p_msg,
               uint32_t msg_size,
               sgx_enclave_id_t id,
               sgx_status_t *status,
               AibeAlgo aibeAlgo,
               NetworkServer &server) {
    if (!p_msg ||
        (msg_size > LENOFMSE)) {
        return -1;
    }
    sgx_aes_gcm_128bit_tag_t mac;
    int ret = 0;
    int busy_retry_time = 4;
    int msg2_size = aibeAlgo.size_comp_G1 * 2 + aibeAlgo.size_Zr + SGX_AESGCM_MAC_SIZE;
    uint8_t p_data[LENOFMSE] = {0};
    uint8_t out_data[LENOFMSE] = {0};
    ra_samp_response_header_t *p_msg2_full = NULL;
    uint8_t data_size = msg_size - SGX_AESGCM_MAC_SIZE;

    memcpy_s(p_data, msg_size, p_msg, msg_size);
    memcpy_s(mac, SGX_AESGCM_MAC_SIZE, p_data + data_size, SGX_AESGCM_MAC_SIZE);
    do {
        ret = enclave_decrypt(
                id,
                status,
                p_data,
                data_size,
                out_data,
                mac);
    } while (SGX_ERROR_BUSY == ret && busy_retry_time--);
    if (ret != SGX_SUCCESS)
        return ret;
//    fprintf(stdout, "\nData of Encrypted R and its MAC is\n");
//    PRINT_BYTE_ARRAY(stdout, p_data, data_size);
//    PRINT_BYTE_ARRAY(stdout, mac, SGX_AESGCM_MAC_SIZE);

    element_from_bytes_compressed(aibeAlgo.R, out_data);
    element_from_bytes_compressed(aibeAlgo.Hz, out_data + aibeAlgo.size_comp_G1);

    {
        fprintf(stdout, "\nData of Hz and R is\n");
        element_fprintf(stdout, "Hz: %B\n", aibeAlgo.Hz);
        element_fprintf(stdout, "R: %B\n", aibeAlgo.R);
    }

    aibeAlgo.keygen2();

    {
        fprintf(stdout, "\nData of dk' is\n");
        element_fprintf(stdout, "dk'.d1: %B\n", aibeAlgo.dk1.d1);
        element_fprintf(stdout, "dk'.d2: %B\n", aibeAlgo.dk1.d2);
        element_fprintf(stdout, "dk'.d3: %B\n", aibeAlgo.dk1.d3);
    }

    busy_retry_time = 4;
    dk_to_bytes(p_data, &aibeAlgo.dk1, aibeAlgo.size_comp_G1);
    data_size = msg2_size - SGX_AESGCM_MAC_SIZE;
    do {
        ret = enclave_encrypt(
                id,
                status,
                p_data,
                data_size,
                out_data,
                mac);
    } while (SGX_ERROR_BUSY == ret && busy_retry_time--);

//    fprintf(stdout, "\nData of Encrypt is\n");
//    PRINT_BYTE_ARRAY(stdout, p_data, data_size);
//    fprintf(stdout, "\nData of Encrypted and mac is\n");
//    PRINT_BYTE_ARRAY(stdout, out_data, data_size);
//    PRINT_BYTE_ARRAY(stdout, mac, SGX_AESGCM_MAC_SIZE);

    p_msg2_full = (ra_samp_response_header_t *) malloc(msg2_size + sizeof(ra_samp_response_header_t));
    if (!p_msg2_full) {
        fprintf(stderr, "\nError, out of memory in [%s]-[%d].", __FUNCTION__, __LINE__);
        ret = SP_INTERNAL_ERROR;
        return ret;
    }
    memset(p_msg2_full, 0, msg2_size + sizeof(ra_samp_response_header_t));
    p_msg2_full->type = TYPE_RA_KEYGEN;
    p_msg2_full->size = msg2_size;
    p_msg2_full->status[0] = 0;
    p_msg2_full->status[1] = 0;

    if (memcpy_s(p_msg2_full->body, data_size, out_data, data_size)) {
        fprintf(stderr, "\nError, memcpy failed in [%s]-[%d].", __FUNCTION__, __LINE__);
        ret = SP_INTERNAL_ERROR;
        return ret;
    }
    if (memcpy_s(p_msg2_full->body + data_size, SGX_AESGCM_MAC_SIZE, mac, SGX_AESGCM_MAC_SIZE)) {
        fprintf(stderr, "\nError, memcpy failed in [%s]-[%d].", __FUNCTION__, __LINE__);
        ret = SP_INTERNAL_ERROR;
        return ret;
    }
    memset(server.sendbuf, 0, BUFSIZ);
    if (memcpy_s(server.sendbuf,
                 msg2_size + sizeof(ra_samp_response_header_t),
                 p_msg2_full,
                 msg2_size + sizeof(ra_samp_response_header_t))) {
        fprintf(stderr, "\nError, memcpy failed in [%s]-[%d].", __FUNCTION__, __LINE__);
        ret = SP_INTERNAL_ERROR;
        return ret;
    }

    if (server.SendTo(msg2_size + sizeof(ra_samp_response_header_t)) < 0) {
        fprintf(stderr, "\nError, send encrypted data failed in [%s]-[%d].", __FUNCTION__, __LINE__);
        ret = SP_INTERNAL_ERROR;
        return ret;
    }

    SAFE_FREE(p_msg2_full);
    fprintf(stdout, "\nKeygen2 Done.");
    return ret;
}


int pkg_keyreq(const ra_samp_request_header_t *p_msg,
               uint32_t msg_size,
               sgx_enclave_id_t id,
               sgx_status_t *status,
               NetworkServer &server) {
    if (!p_msg ||
        (msg_size > BUFSIZ)) {
        return -1;
    }
    int ret = 0;
    uint8_t data[BUFSIZ];
    Proofs proofs;
    int msg2_size;
    ra_samp_response_header_t *p_response = NULL;

    memcpy_s(data, msg_size, p_msg, msg_size);
    puts("\nstart deserialise");
    proofs.deserialise(data);
    if (!proofs.verify_proofs()) {
        fprintf(stderr, "\nProofs verify failed.");
    }

    fprintf(stderr, "\nProofs verify succeed.");

    msg2_size = 0;
    p_response = (ra_samp_response_header_t *) malloc(msg2_size + sizeof(ra_samp_response_header_t));
    if (!p_response) {
        fprintf(stderr, "\nError, out of memory in [%s]-[%d].", __FUNCTION__, __LINE__);
        ret = SP_INTERNAL_ERROR;
        return ret;
    }
    memset(p_response, 0, msg2_size + sizeof(ra_samp_response_header_t));
    p_response->type = TYPE_RA_KEYREQ;
    p_response->size = msg2_size;
    p_response->status[0] = 0;
    p_response->status[1] = 0;


    memset(server.sendbuf, 0, BUFSIZ);
    if (memcpy_s(server.sendbuf,
                 msg2_size + sizeof(ra_samp_response_header_t),
                 p_response,
                 msg2_size + sizeof(ra_samp_response_header_t))) {
        fprintf(stderr, "\nError, memcpy failed in [%s]-[%d].", __FUNCTION__, __LINE__);
        ret = SP_INTERNAL_ERROR;
        return ret;
    }

    if (server.SendTo(msg2_size + sizeof(ra_samp_response_header_t)) < 0) {
        fprintf(stderr, "\nError, send encrypted data failed in [%s]-[%d].", __FUNCTION__, __LINE__);
        ret = SP_INTERNAL_ERROR;
        return ret;
    }

    fprintf(stdout, "\nKeyreq Done.");
    return ret;
}

// This sample code doesn't have any recovery/retry mechanisms for the remote
// attestation. Since the enclave can be lost due S3 transitions, apps
// susceptible to S3 transitions should have logic to restart attestation in
// these scenarios.
#define _T(x) x

int main(int argc, char *argv[]) {
    int ret = 0;
    NetworkServer server;
    AibeAlgo aibeAlgo;
    ra_samp_request_header_t *p_msg0_full = NULL;
    ra_samp_response_header_t *p_msg0_resp_full = NULL;
    ra_samp_request_header_t *p_msg1_full = NULL;
    ra_samp_response_header_t *p_msg2_full = NULL;
    sgx_ra_msg3_t *p_msg3 = NULL;
    ra_samp_response_header_t *p_att_result_msg_full = NULL;
    sgx_enclave_id_t enclave_id = 0;
    int enclave_lost_retry_time = 1;
    int busy_retry_time = 4;
    sgx_ra_context_t context = INT_MAX;
    sgx_status_t status = SGX_SUCCESS;
    ra_samp_request_header_t *p_msg3_full = NULL;
    ra_samp_request_header_t *p_msgaes_full = NULL;

    int32_t verify_index = -1;
    int32_t verification_samples = sizeof(msg1_samples) / sizeof(msg1_samples[0]);

    FILE *OUTPUT = stdout;
    ra_samp_request_header_t *p_req;
    ra_samp_response_header_t **p_resp;
    ra_samp_response_header_t *p_resp_msg;
    int server_port = 12333;
    int buflen = 0;
    uint32_t extended_epid_group_id = 0;

    aibeAlgo.load_param(param_path);
    puts("param loaded");
    aibeAlgo.init();
    puts("init");
    aibeAlgo.mpk_load();
    aibeAlgo.msk_load();
    puts("mpk loaded");

//    aibeAlgo.run(OUTPUT);

    { // creates the cryptserver enclave.

        ret = sgx_get_extended_epid_group_id(&extended_epid_group_id);
        if (SGX_SUCCESS != ret) {
            ret = -1;
            fprintf(OUTPUT, "\nError, call sgx_get_extended_epid_group_id fail [%s].",
                    __FUNCTION__);
            return ret;
        }
        fprintf(OUTPUT, "\nCall sgx_get_extended_epid_group_id success.");

        int launch_token_update = 0;
        sgx_launch_token_t launch_token = {0};
        memset(&launch_token, 0, sizeof(sgx_launch_token_t));
        do {
            ret = sgx_create_enclave(_T(ENCLAVE_PATH),
                                     SGX_DEBUG_FLAG,
                                     &launch_token,
                                     &launch_token_update,
                                     &enclave_id, NULL);
            if (SGX_SUCCESS != ret) {
                ret = -1;
                fprintf(OUTPUT, "\nError, call sgx_create_enclave fail [%s].",
                        __FUNCTION__);
                goto CLEANUP;
            }
            fprintf(OUTPUT, "\nCall sgx_create_enclave success.");

            ret = enclave_init_ra(enclave_id,
                                  &status,
                                  false,
                                  &context);
            //Ideally, this check would be around the full attestation flow.
        } while (SGX_ERROR_ENCLAVE_LOST == ret && enclave_lost_retry_time--);

        if (SGX_SUCCESS != ret || status) {
            ret = -1;
            fprintf(OUTPUT, "\nError, call enclave_init_ra fail [%s].",
                    __FUNCTION__);
            goto CLEANUP;
        }
        fprintf(OUTPUT, "\nCall enclave_init_ra success.");
    }

    //?????????????????????????????????????????????
    fprintf(OUTPUT, "\nstart socket....");
    server.server(server_port);

    //??????????????????????????????????????????????????????
    do {
        bool is_recv = true;
        do {
            //????????????socket
            buflen = server.RecvFrom();
            if (buflen > 0 && buflen < BUFSIZ) {
                p_req = (ra_samp_request_header_t *) malloc(buflen + 2);

                fprintf(OUTPUT, "\nPrepare receive struct");
                if (NULL == p_req) {
                    ret = -1;
                    goto CLEANUP;
                }
                if (memcpy_s(p_req, buflen + 2, server.recvbuf, buflen)) {
                    fprintf(OUTPUT, "\nError: INTERNAL ERROR - memcpy failed in [%s].",
                            __FUNCTION__);
                    ret = -1;
                    goto CLEANUP;
                }
                fprintf(OUTPUT, "\nrequest type is %d", p_req->type);
                switch (p_req->type) {
                    case TYPE_EXIT:
                        fprintf(OUTPUT, "\nConnection terminated");
                        SAFE_FREE(p_req);
                        is_recv = false;
                        break;
                        //??????msg1????????????????????????msg2
                    //??????msg0???????????????
                    case TYPE_RA_MSG0:
                        fprintf(OUTPUT, "\nProcess Message 0");
                        ret = sp_ra_proc_msg0_req(
                                (const sample_ra_msg0_t *) ((uint8_t *) p_req + sizeof(ra_samp_request_header_t)),
                                p_req->size);
                        fprintf(OUTPUT, "\nProcess Message 0 Done");
                        if (0 != ret) {
                            fprintf(stderr, "\nError, call sp_ra_proc_msg1_req fail [%s].",
                                    __FUNCTION__);
                        }
                        SAFE_FREE(p_req);
                        break;
                        //??????msg1????????????????????????msg2
                    case TYPE_RA_MSG1:
                        fprintf(OUTPUT, "\nBuffer length is %d\n", buflen);
                        p_resp_msg = (ra_samp_response_header_t *) malloc(sizeof(ra_samp_response_header_t) + 170);//????????????
                        memset(p_resp_msg, 0, sizeof(ra_samp_response_header_t) + 170);
                        fprintf(OUTPUT, "\nProcess Message 1\n");
                        ret = sp_ra_proc_msg1_req(
                                (const sample_ra_msg1_t *) ((uint8_t *) p_req + sizeof(ra_samp_request_header_t)),
                                p_req->size,
                                &p_resp_msg);
                        fprintf(OUTPUT, "\nProcess Message 1 Done");
                        if (0 != ret) {
                            fprintf(stderr, "\nError, call sp_ra_proc_msg1_req fail [%s].",
                                    __FUNCTION__);
                        } else {
                            memset(server.sendbuf, 0, BUFSIZ);
                            if (memcpy_s(server.sendbuf, BUFSIZ, p_resp_msg,
                                         sizeof(ra_samp_response_header_t) + p_resp_msg->size)) {
                                fprintf(OUTPUT, "\nError: INTERNAL ERROR - memcpy failed in [%s].",
                                        __FUNCTION__);
                                ret = -1;
                                goto CLEANUP;
                            }
                            fprintf(OUTPUT, "\nSend Message 2\n");
                            PRINT_BYTE_ARRAY(OUTPUT, p_resp_msg, 176);
                            int buflen = server.SendTo(sizeof(ra_samp_response_header_t) + p_resp_msg->size);
                            fprintf(OUTPUT, "\nSend Message 2 Done,send length = %d", buflen);
                        }
                        SAFE_FREE(p_req);
                        SAFE_FREE(p_resp_msg);
                        break;
                        //??????msg3?????????attestation result
                    case TYPE_RA_MSG3:
                        fprintf(OUTPUT, "\nProcess Message 3");
                        p_resp_msg = (ra_samp_response_header_t *) malloc(sizeof(ra_samp_response_header_t) + 200);//????????????
                        memset(p_resp_msg, 0, sizeof(ra_samp_response_header_t) + 200);
                        ret = sp_ra_proc_msg3_req((const sample_ra_msg3_t *) ((uint8_t *) p_req +
                                                                              sizeof(ra_samp_request_header_t)),
                                                  p_req->size,
                                                  &p_resp_msg);
                        if (0 != ret) {
                            fprintf(stderr, "\nError, call sp_ra_proc_msg3_req fail [%s].",
                                    __FUNCTION__);
                        } else {
                            memset(server.sendbuf, 0, BUFSIZ);
                            if (memcpy_s(server.sendbuf, BUFSIZ, p_resp_msg,
                                         sizeof(ra_samp_response_header_t) + p_resp_msg->size)) {
                                fprintf(OUTPUT, "\nError: INTERNAL ERROR - memcpy failed in [%s].",
                                        __FUNCTION__);
                                ret = -1;
                                goto CLEANUP;
                            }
                            fprintf(OUTPUT, "\nSend attestation data\n");
                            PRINT_BYTE_ARRAY(OUTPUT, p_resp_msg, sizeof(ra_samp_response_header_t) + p_resp_msg->size);
                            int buflen = server.SendTo(sizeof(ra_samp_response_header_t) + p_resp_msg->size);
                            fprintf(OUTPUT, "\nSend attestation data Done,send length = %d", buflen);
                        }

                        {
                            sample_ec_key_128bit_t secret;
                            get_secret(&secret);
                            put_secret_data(enclave_id,
                                            &status,
                                            secret);
                        }


                        SAFE_FREE(p_req);
                        SAFE_FREE(p_resp_msg);
                        break;
                    case TYPE_RA_KEYGEN:
                        fprintf(OUTPUT, "\nProcess Keygen");
                        ret = pkg_keygen((const ra_samp_request_header_t *) ((uint8_t *) p_req +
                                                                             sizeof(ra_samp_request_header_t)),
                                         p_req->size,
                                         enclave_id,
                                         &status,
                                         aibeAlgo,
                                         server);
                        fprintf(OUTPUT, "\nKeygen2 Done %d %d", enclave_id, status);
                        if (0 != ret) {
                            fprintf(stderr, "\nError, call keygen fail [%s].",
                                    __FUNCTION__);
                        }
                        SAFE_FREE(p_req);
                        is_recv = false;
                        break;
                    case TYPE_RA_KEYREQ:
                        fprintf(OUTPUT, "\nProcess Keyreq");
                        ret = pkg_keyreq((const ra_samp_request_header_t *) ((uint8_t *) p_req +
                                                                             sizeof(ra_samp_request_header_t)),
                                         p_req->size,
                                         enclave_id,
                                         &status,
                                         server);
                        fprintf(OUTPUT, "\nKeyreq Done %d %d", enclave_id, status);
                        if (0 != ret) {
                            fprintf(stderr, "\nError, call keyreq fail [%s].",
                                    __FUNCTION__);
                        }
                        SAFE_FREE(p_req);
                        is_recv = false;
                        break;

                    default:
                        ret = -1;
                        fprintf(stderr, "\nError, unknown ra message type. Type = %d [%s].",
                                p_req->type, __FUNCTION__);
                        goto CLEANUP;
                }
            }
        } while (is_recv);

        ret = server.accept_client();
        if (ret) {
            fprintf(OUTPUT, "\nAccept failed.");
            goto CLEANUP;
        }

    } while (true);


    puts("\npkg: keygen2 finished");

    CLEANUP:
    // Clean-up
    // Need to close the RA key state.
    if (INT_MAX != context) {
        int ret_save = ret;
        ret = enclave_ra_close(enclave_id, &status, context);
        if (SGX_SUCCESS != ret || status) {
            ret = -1;
            fprintf(OUTPUT, "\nError, call enclave_ra_close fail [%s].",
                    __FUNCTION__);
        } else {
            // enclave_ra_close was successful, let's restore the value that
            // led us to this point in the code.
            ret = ret_save;
        }
        fprintf(OUTPUT, "\nCall enclave_ra_close success.");
    }

    sgx_destroy_enclave(enclave_id);

    ra_free_network_response_buffer(p_msg0_resp_full);
    ra_free_network_response_buffer(p_msg2_full);
    ra_free_network_response_buffer(p_att_result_msg_full);

    server.Cleanupsocket();

    // p_msg3 is malloc'd by the untrusted KE library. App needs to free.
    SAFE_FREE(p_msg3);
    SAFE_FREE(p_msg3_full);
    SAFE_FREE(p_msg1_full);
    printf("\nExit ...\n");
    return ret;
}
