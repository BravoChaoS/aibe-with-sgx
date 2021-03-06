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

#include "ra.h"
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


// Needed to calculate keys

#include "aibe.h"

#define LENOFMSE 1024

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


int client_keygen(int id, AibeAlgo aibeAlgo, sgx_enclave_id_t enclave_id, FILE *OUTPUT, NetworkClient client) {
    int ret = 0;
    sgx_status_t status = SGX_SUCCESS;
    ra_samp_request_header_t *p_request = NULL;
    ra_samp_response_header_t *p_response = NULL;
    int recvlen = 0;
    int busy_retry_time;
    int data_size;
    int msg_size;

    uint8_t p_data[LENOFMSE] = {0};
    uint8_t out_data[LENOFMSE] = {0};
    sgx_aes_gcm_128bit_tag_t mac;

    // keygen 1
    aibeAlgo.keygen1(id);

    data_size = aibeAlgo.size_comp_G1 * 2;
    element_to_bytes_compressed(p_data, aibeAlgo.R);
    element_to_bytes_compressed(p_data + aibeAlgo.size_comp_G1, aibeAlgo.Hz);
    ra_encrypt(p_data, data_size, out_data, mac, enclave_id, OUTPUT);

    element_fprintf(OUTPUT, "Send R:\n%B", aibeAlgo.R);

    msg_size = data_size + SGX_AESGCM_MAC_SIZE;
    p_request = (ra_samp_request_header_t *) malloc(sizeof(ra_samp_request_header_t) + msg_size);
    p_request->size = msg_size;
    p_request->type = TYPE_RA_KEYGEN;

    if (memcpy_s(p_request->body, data_size, out_data, data_size)) {
        fprintf(OUTPUT, "Error: INTERNAL ERROR - memcpy failed in [%s]-[%d].",
                __FUNCTION__, __LINE__);
        ret = -1;
        goto CLEANUP;
    }
    if (memcpy_s(p_request->body + data_size, SGX_AESGCM_MAC_SIZE, mac, SGX_AESGCM_MAC_SIZE)) {
        fprintf(OUTPUT, "Error: INTERNAL ERROR - memcpy failed in [%s]-[%d].",
                __FUNCTION__, __LINE__);
        ret = -1;
        goto CLEANUP;
    }

    memset(client.sendbuf, 0, BUFSIZ);
    memcpy_s(client.sendbuf, BUFSIZ, p_request, sizeof(ra_samp_request_header_t) + msg_size);
    client.SendTo(sizeof(ra_samp_request_header_t) + p_request->size);

    // keygen 3
    recvlen = client.RecvFrom();
    p_response = (ra_samp_response_header_t *) malloc(
            sizeof(ra_samp_response_header_t) + ((ra_samp_response_header_t *) client.recvbuf)->size);

    if (memcpy_s(p_response, recvlen, client.recvbuf, recvlen)) {
        fprintf(OUTPUT, "Error: INTERNAL ERROR - memcpy failed in [%s]-[%d].",
                __FUNCTION__, __LINE__);
        ret = -1;
        goto CLEANUP;
    }
    if ((p_response->type != TYPE_RA_KEYGEN)) {
        fprintf(OUTPUT, "Error: INTERNAL ERROR - memcpy failed in [%s]-[%d].",
                __FUNCTION__, __LINE__);
        ret = -1;
        goto CLEANUP;
    }

    data_size = p_response->size - SGX_AESGCM_MAC_SIZE;

    memcpy_s(p_data, data_size, p_response->body, data_size);
    memcpy_s(mac, SGX_AESGCM_MAC_SIZE, p_response->body + data_size, SGX_AESGCM_MAC_SIZE);
//    fprintf(OUTPUT, "Success Encrypt\n");
//    PRINT_BYTE_ARRAY(OUTPUT, p_data, sizeof(p_data));
//    fprintf(OUTPUT, "Encrypt Mac\n");
//    PRINT_BYTE_ARRAY(OUTPUT, mac, SGX_AESGCM_MAC_SIZE);

    ra_decrypt(p_data, data_size, out_data, mac, enclave_id, OUTPUT);

    dk_from_bytes(&aibeAlgo.dk1, out_data, aibeAlgo.size_comp_G1);
    {
        fprintf(stdout, "Data of dk' is\n");
        element_fprintf(stdout, "dk'.d1: %B\n", aibeAlgo.dk1.d1);
        element_fprintf(stdout, "dk'.d2: %B\n", aibeAlgo.dk1.d2);
        element_fprintf(stdout, "dk'.d3: %B\n", aibeAlgo.dk1.d3);
    }

    if (aibeAlgo.keygen3()) {
        ret = -1;
        goto CLEANUP;
    }

    {
        fprintf(stdout, "Data of dk is\n");
        element_fprintf(stdout, "dk.d1: %B\n", aibeAlgo.dk.d1);
        element_fprintf(stdout, "dk.d2: %B\n", aibeAlgo.dk.d2);
        element_fprintf(stdout, "dk.d3: %B\n", aibeAlgo.dk.d3);
    }

    CLEANUP:
    SAFE_FREE(p_response);
    return ret;
}

// This sample code doesn't have any recovery/retry mechanisms for the remote
// attestation. Since the enclave can be lost due S3 transitions, apps
// susceptible to S3 transitions should have logic to restart attestation in
// these scenarios.
#define _T(x) x


int client_keyreq(NetworkClient client) {

    int ret = 0;
    sgx_status_t status = SGX_SUCCESS;
    ra_samp_request_header_t *p_request = NULL;
    ra_samp_response_header_t *p_response = NULL;
    int recvlen = 0;
    int busy_retry_time;
    int data_size;
    int msg_size;

    msg_size = sizeof(int);
    p_request = (ra_samp_request_header_t *) malloc(sizeof(ra_samp_request_header_t) + msg_size);
    p_request->size = msg_size;
    p_request->type = TYPE_LM_KEYREQ;
    *((int *) p_request->body) = ID;

    memset(client.sendbuf, 0, BUFSIZ);
    memcpy_s(client.sendbuf, BUFSIZ, p_request, sizeof(ra_samp_request_header_t) + msg_size);
    client.SendTo(sizeof(ra_samp_request_header_t) + msg_size);

// recv
    recvlen = client.RecvFrom();
    p_response = (ra_samp_response_header_t *) malloc(
            sizeof(ra_samp_response_header_t) + ((ra_samp_response_header_t *) client.recvbuf)->size);

    if (memcpy_s(p_response, recvlen, client.recvbuf, recvlen)) {
        fprintf(stderr, "Error: INTERNAL ERROR - memcpy failed in [%s]-[%d].",
                __FUNCTION__, __LINE__);
        ret = -1;
        goto CLEANUP;
    }
    if ((p_response->type != TYPE_LM_KEYREQ)) {
        fprintf(stderr, "Error: INTERNAL ERROR - recv type error in [%s]-[%d].",
                __FUNCTION__, __LINE__);
        printf("%d\n", p_response->type);
        ret = -1;
        goto CLEANUP;
    }
    puts("Certificate received");

    CLEANUP:
    SAFE_FREE(p_request);
    SAFE_FREE(p_response);
    return ret;
}

int main(int argc, char *argv[]) {
    int ret = 0;
    sgx_enclave_id_t enclave_id = 0;
    AibeAlgo aibeAlgo;
    FILE *OUTPUT = stdout;
    NetworkClient client;
    int pkg_port = 12333;
    int lm_port = 22333;
    int mod = 0;
    int launch_token_update = 0;
    sgx_launch_token_t launch_token = {0};
    FILE *f;
    int ct_size, msg_size;

    //aibe load_param
    pairing_t pairing;

////    aibe load_param
    if (aibeAlgo.load_param(param_path)) {
        fprintf(stderr, "Param File Path error\n");
        exit(-1);
    }
//    printf("%d, %d, %d\n", aibeAlgo.size_GT, aibeAlgo.size_comp_G1, aibeAlgo.size_Zr);
    uint8_t ct_buf[aibeAlgo.size_ct + 10];
    uint8_t msg_buf[aibeAlgo.size_ct + 10];
    fprintf(OUTPUT, "A-IBE Success Set Up\n");
////    element init
    aibeAlgo.init();

    memset(&launch_token, 0, sizeof(sgx_launch_token_t));
    {
        ret = sgx_create_enclave(_T(ENCLAVE_PATH),
                                 SGX_DEBUG_FLAG,
                                 &launch_token,
                                 &launch_token_update,
                                 &enclave_id, NULL);
        if (SGX_SUCCESS != ret) {
            ret = -1;
            fprintf(OUTPUT, "Error, call sgx_create_enclave fail [%s].\n",
                    __FUNCTION__);
            goto CLEANUP;
        }
        fprintf(OUTPUT, "Call sgx_create_enclave success.\n");
    }

//    aibeAlgo.load_param(param_path);
//    aibeAlgo.init();
//    aibeAlgo.dk_load();
//    aibeAlgo.mpk_load();
//    element_random(aibeAlgo.m);
//    element_printf("%B\n", aibeAlgo.m);
//    aibeAlgo.block_encrypt(ID);
//    aibeAlgo.ct_write();
//    aibeAlgo.ct_read();
//    aibeAlgo.block_decrypt();
//    element_printf("%B\n", aibeAlgo.m);

    printf("Please choose a function:\n"
           "1) key request\n"
           "2) key generation\n"
           "3) block_encrypt\n"
           "4) block_decrypt\n"
           "Please input a number:");
    scanf("%d", &mod);

    switch (mod) {
        case 1:
            if (client.client("127.0.0.1", lm_port) != 0) {
                fprintf(OUTPUT, "Connect Server Error, Exit!\n");
                ret = -1;
                goto CLEANUP;
            }
            client_keyreq(client);
            puts("Key request finished\n");
            break;

        case 2:
            fprintf(OUTPUT, "Start key generation\n");
            // SOCKET: connect to server
            if (client.client("127.0.0.1", pkg_port) != 0) {
                fprintf(OUTPUT, "Connect Server Error, Exit!\n");
                ret = -1;
                goto CLEANUP;
            }
            if (remote_attestation(enclave_id, client) != SGX_SUCCESS) {
                fprintf(OUTPUT, "Remote Attestation Error, Exit!\n");
                ret = -1;
                goto CLEANUP;
            }
            aibeAlgo.mpk_load();
            puts("Client: setup finished");
////    aibe: keygen
            if (client_keygen(ID, aibeAlgo, enclave_id, OUTPUT, client)) {
                fprintf(stderr, "Key verify failed\n");
                goto CLEANUP;
            }
            aibeAlgo.dk_store();
            fprintf(OUTPUT, "A-IBE Success Keygen \n");

            break;

        case 3:
            aibeAlgo.mpk_load();
            puts("Client: setup finished");
            fprintf(OUTPUT, "Start Encrypt\n");
            f = fopen(msg_path, "r+");
            msg_size = fread(msg_buf, sizeof(uint8_t), aibeAlgo.size_ct, f);
            fclose(f);

            fprintf(OUTPUT, "Message:\n%s\n", msg_buf);
            fprintf(OUTPUT, "Message size: %d\n", msg_size);


            ct_size = aibeAlgo.encrypt(ct_buf, (char *)msg_buf, ID);
            f = fopen(ct_path, "w+");
            fwrite(ct_buf, ct_size, 1, f);
            fclose(f);

            fprintf(OUTPUT, "encrypt size: %d, block size %d\n", ct_size, aibeAlgo.size_block);

            break;

        case 4:
            aibeAlgo.dk_load();
            aibeAlgo.mpk_load();
            puts("Client: setup finished");
            fprintf(OUTPUT, "Start Decrypt\n");

            f = fopen(ct_path, "r+");
            ct_size = fread(ct_buf, sizeof(uint8_t), aibeAlgo.size_ct, f);
            fclose(f);
            fprintf(OUTPUT, "decrypt size: %d, ct size: %d\n", ct_size, aibeAlgo.size_ct);

            aibeAlgo.decrypt(msg_buf, ct_buf, ct_size);
            printf("%s\n", msg_buf);

            f = fopen(out_path, "w+");
            fwrite(msg_buf, strlen((char *)msg_buf), 1, f);
            fclose(f);

            break;

        default:
            printf("Invalid function number, exit\n");
            goto CLEANUP;
    }


    CLEANUP:
    terminate(client);
    client.Cleanupsocket();
    sgx_destroy_enclave(enclave_id);

    aibeAlgo.clear();
    fprintf(OUTPUT, "Success Clean Up A-IBE \n");

    return ret;
}
